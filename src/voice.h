// Sonidos del personaje: "ardillita" (chirridos sin palabras) + avisos de micrófono.
// Todo se genera en la placa; no usa internet. El micrófono NO se inicializa acá.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include "es8311.h"

#define PIN_PA     7
#define PIN_MCLK   8
#define PIN_BCLK   9
#define PIN_WS     10
#define PIN_DOUT   12
#define PIN_I2C_SDA 42
#define PIN_I2C_SCL 41
#define SR 16000

namespace voice {

enum Cmd : uint8_t { CHATTER, LISTEN_ON, LISTEN_OFF, HAPPY_SQUEAK };
static I2SClass i2s;
static QueueHandle_t q;
static volatile bool busy = false;
static bool ok = false;
static int16_t buf[256];

static void writeTone(float f0, float f1, int ms, float amp, float squeak) {
  int n = SR * ms / 1000;
  static float ph = 0;
  int att = SR * 4 / 1000;
  for (int i = 0; i < n; i += 256) {
    int m = min(256, n - i);
    for (int k = 0; k < m; k++) {
      int j = i + k;
      float u = (float)j / n;
      float f = f0 + (f1 - f0) * u;
      ph += 2 * PI * f / SR; if (ph > 2 * PI) ph -= 2 * PI;
      float env = j < att ? (float)j / att : (1 - u) * (1 - u);
      float s = sinf(ph) * (1 - squeak) + (sinf(ph) > 0 ? 1 : -1) * squeak * 0.5f;
      buf[k] = (int16_t)(s * env * amp * 32767);
    }
    i2s.write((uint8_t *)buf, m * 2);
  }
}
static void silence(int ms) {
  memset(buf, 0, sizeof(buf));
  int n = SR * ms / 1000;
  for (int i = 0; i < n; i += 256) i2s.write((uint8_t *)buf, min(256, n - i) * 2);
}

static void task(void *) {
  Cmd c;
  for (;;) {
    if (xQueueReceive(q, &c, portMAX_DELAY) != pdTRUE) continue;
    busy = true;
    digitalWrite(PIN_PA, HIGH);
    silence(15);
    if (c == CHATTER) {
      int syl = 6 + esp_random() % 6;
      for (int s = 0; s < syl; s++) {
        float base = 1500 + esp_random() % 900;
        int len = 45 + esp_random() % 45;
        bool up = (esp_random() % 3) != 0;
        writeTone(base, up ? base * 1.45f : base * 0.8f, len, 0.35f, 0.18f);
        silence(20 + esp_random() % 40);
        if ((esp_random() % 5) == 0) silence(80);
      }
      writeTone(2300, 3300, 70, 0.3f, 0.1f);  // remate agudo
    } else if (c == LISTEN_ON) {
      writeTone(880, 880, 60, 0.28f, 0); silence(20); writeTone(1320, 1320, 80, 0.28f, 0);
    } else if (c == LISTEN_OFF) {
      writeTone(1320, 1320, 60, 0.28f, 0); silence(20); writeTone(880, 880, 80, 0.28f, 0);
    } else if (c == HAPPY_SQUEAK) {
      writeTone(1800, 3000, 90, 0.3f, 0.15f); silence(30); writeTone(2200, 3400, 90, 0.3f, 0.15f);
    }
    silence(30);
    digitalWrite(PIN_PA, LOW);
    busy = false;
  }
}

static void begin() {
  pinMode(PIN_PA, OUTPUT); digitalWrite(PIN_PA, LOW);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  i2s.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, -1, PIN_MCLK);
  if (!i2s.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) return;
  es8311_handle_t h = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!h) return;
  const es8311_clock_config_t clk = {false, false, true, SR * 256, SR};
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return;
  es8311_sample_frequency_config(h, SR * 256, SR);
  es8311_voice_volume_set(h, 70, NULL);
  es8311_microphone_config(h, false);
  q = xQueueCreate(4, sizeof(Cmd));
  xTaskCreatePinnedToCore(task, "voice", 4096, NULL, 3, NULL, 0);
  ok = true;
}
static void play(Cmd c) { if (ok) xQueueSend(q, &c, 0); }

}  // namespace voice

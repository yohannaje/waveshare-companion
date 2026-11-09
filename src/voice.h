// Audio del personaje:
//  - Sonidos "ardillita" y avisos (ES8311 + parlante).
//  - Micrófono (ES7210): se ENCIENDE solo mientras se mantiene apretado el botón + y se APAGA al soltar.
//  - Envío de la grabación al cerebro (Cloudflare Worker, plan gratis) y recepción de la respuesta en texto.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "es8311.h"
#include "es7210.h"

#define PIN_PA     7
#define PIN_MCLK   8
#define PIN_BCLK   9
#define PIN_WS     10
#define PIN_DIN    11
#define PIN_DOUT   12
#define PIN_I2C_SDA 42
#define PIN_I2C_SCL 41
#define SR 16000
#define ES7210_ADDR 0x40
#define BRAIN_URL "https://companero.vc5z8pn7nd.workers.dev/ask"
#define VOZ_URL   "https://companero.vc5z8pn7nd.workers.dev/voz"
#define VOICE_SPEED 1.30f   // >1 = voz mas aguda y rapida, tipo ardilla (1.0 = normal)
#define SPK_MAX_SEC 8
#define MAX_REC_SEC 8

namespace voice {

enum Cmd : uint8_t { CHATTER, LISTEN_ON, LISTEN_OFF, SPEAK };
static I2SClass i2s;
static QueueHandle_t q;
static volatile bool busy = false;
static bool ok = false;
static int16_t buf[512];  // estéreo: L,R
static int16_t *spk = nullptr;           // voz hablada que llega del cerebro (mono 16 kHz, PSRAM)
static volatile size_t spkLen = 0;

// ---- reproducción ----
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
      int16_t v = (int16_t)(s * env * amp * 32767);
      buf[2 * k] = v; buf[2 * k + 1] = v;
    }
    i2s.write((uint8_t *)buf, m * 4);
  }
}
static void silence(int ms) {
  memset(buf, 0, sizeof(buf));
  int n = SR * ms / 1000;
  for (int i = 0; i < n; i += 256) i2s.write((uint8_t *)buf, min(256, n - i) * 4);
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
      writeTone(2300, 3300, 70, 0.3f, 0.1f);
    } else if (c == SPEAK) {
      // 2 gorjeos de ardilla y despues la frase en espanol, aguda y rapida
      for (int k = 0; k < 2; k++) { float b = 1700 + esp_random() % 700; writeTone(b, b * 1.5f, 55, 0.3f, 0.15f); silence(25); }
      silence(40);
      size_t n = spkLen;
      size_t outN = (size_t)((n - 1) / VOICE_SPEED);
      for (size_t i = 0; i < outN; i += 256) {
        int m = (int)min((size_t)256, outN - i);
        for (int k = 0; k < m; k++) {
          float x = (i + k) * VOICE_SPEED; size_t i0 = (size_t)x; float fr = x - i0;
          int v = (int)(spk[i0] * (1 - fr) + spk[min(i0 + 1, n - 1)] * fr);
          buf[2 * k] = buf[2 * k + 1] = (int16_t)constrain(v, -32767, 32767);
        }
        i2s.write((uint8_t *)buf, m * 4);
      }
    } else if (c == LISTEN_ON) {
      writeTone(880, 880, 60, 0.28f, 0); silence(20); writeTone(1320, 1320, 80, 0.28f, 0);
    } else if (c == LISTEN_OFF) {
      writeTone(1320, 1320, 60, 0.28f, 0); silence(20); writeTone(880, 880, 80, 0.28f, 0);
    }
    silence(30);
    digitalWrite(PIN_PA, LOW);
    busy = false;
  }
}
static void play(Cmd c) { if (ok) xQueueSend(q, &c, 0); }

// ---- micrófono ----
static es7210_dev_handle_t mic = NULL;
static int16_t *rec = nullptr;            // mono, en PSRAM
static const size_t REC_MAX = SR * MAX_REC_SEC;
static volatile size_t recLen = 0;
static volatile bool recording = false;
static volatile bool micOn = false;

static void es7210Write(uint8_t reg, uint8_t val) { Wire.beginTransmission(ES7210_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission(); }
static void micPowerUp() {
  if (!mic) { es7210_i2c_config_t ic = {}; ic.i2c_addr = ES7210_ADDR; if (es7210_new_codec(&ic, &mic) != ESP_OK) { mic = NULL; return; } }
  es7210_codec_config_t cc = {};
  cc.i2s_format = ES7210_I2S_FMT_I2S; cc.mclk_ratio = 256; cc.sample_rate_hz = SR; cc.bit_width = ES7210_I2S_BITS_16B;
  cc.mic_bias = ES7210_MIC_BIAS_2V87; cc.mic_gain = ES7210_MIC_GAIN_30DB; cc.flags.tdm_enable = false;
  es7210_config_codec(mic, &cc);
  es7210_config_volume(mic, 0);
  micOn = true;
}
static void micPowerDown() {
  // Apaga polarización, ADC y preamplificadores de los micrófonos.
  es7210Write(0x4B, 0xFF); es7210Write(0x4C, 0xFF);
  micOn = false;
}
static void recTask(void *) {
  static int16_t in[512];
  for (;;) {
    if (!recording) { vTaskDelay(10); continue; }
    size_t got = i2s.readBytes((char *)in, sizeof(in));
    int frames = got / 4;
    for (int k = 0; k < frames && recLen < REC_MAX; k++) rec[recLen++] = (int16_t)(((int)in[2 * k] + (int)in[2 * k + 1]) / 2);
    if (recLen >= REC_MAX) recording = false;
  }
}
static void startRecording() {
  if (!ok || !rec) return;
  micPowerUp();
  recLen = 0;
  // descartar lo que quedó en el buffer
  int16_t tmp[256]; i2s.readBytes((char *)tmp, sizeof(tmp));
  recording = true;
}
static void stopRecording() {
  recording = false;
  delay(20);
  micPowerDown();
}

// ---- envío al cerebro ----
static String deviceId() { uint64_t m = ESP.getEfuseMac(); char b[20]; snprintf(b, sizeof(b), "%04X%08X", (uint16_t)(m >> 32), (uint32_t)m); return String(b); }
static volatile bool asking = false, answerReady = false;
static String answerText;
class PcmSink : public Stream {
 public:
  size_t n = 0;
  size_t write(const uint8_t *d, size_t len) override {
    size_t room = SR * SPK_MAX_SEC * 2 - n; size_t c = min(len, room);
    if (spk && c) { memcpy((uint8_t *)spk + n, d, c); n += c; }
    return len;
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
};
static void askTask(void *) {
  String out, hablado;
  spkLen = 0;
  if (WiFi.status() != WL_CONNECTED) out = "Me falta el WiFi para pensar.";
  else if (recLen < SR / 2) out = "No te escuche. Mantene el boton mientras hablas.";
  else {
    // ganancia simple para que la voz llegue clara
    int peak = 1; for (size_t i = 0; i < recLen; i++) { int a = abs(rec[i]); if (a > peak) peak = a; }
    float g = min(8.0f, 24000.0f / peak);
    for (size_t i = 0; i < recLen; i++) rec[i] = (int16_t)constrain((int)(rec[i] * g), -32767, 32767);
    {
      WiFiClientSecure client; client.setInsecure();
      HTTPClient http;
      http.setTimeout(25000);
      const char *keys[] = {"X-Voz"};
      http.collectHeaders(keys, 1);
      if (http.begin(client, BRAIN_URL)) {
        http.addHeader("Content-Type", "application/octet-stream");
        http.addHeader("X-Device", deviceId());
        int code = http.POST((uint8_t *)rec, recLen * 2);
        if (code > 0) { out = http.getString(); if (code == 200) hablado = http.header("X-Voz"); }
        else out = "Uy, no llegue al cerebro. Proba de nuevo.";
        http.end();
      } else out = "Uy, no llegue al cerebro.";
    }
    // voz hablada (si falla, queda solo el cartel + ardilla)
    if (hablado.length() && spk) {
      WiFiClientSecure client2; client2.setInsecure();
      HTTPClient h2;
      h2.setTimeout(20000);
      if (h2.begin(client2, VOZ_URL)) {
        h2.addHeader("Content-Type", "text/plain");
        h2.addHeader("X-Device", deviceId());
        int code = h2.POST(hablado);
        if (code == 200) {
          PcmSink sink;
          h2.writeToStream(&sink);
          if (sink.n > SR / 2) spkLen = sink.n / 2;
        }
        h2.end();
      }
    }
  }
  out.trim();
  if (out.length() == 0) out = "Mmm...";
  answerText = out;
  answerReady = true;
  asking = false;
  vTaskDelete(NULL);
}
static void ask() {
  if (asking) return;
  asking = true; answerReady = false;
  xTaskCreatePinnedToCore(askTask, "ask", 16384, NULL, 2, NULL, 0);
}

static void begin() {
  pinMode(PIN_PA, OUTPUT); digitalWrite(PIN_PA, LOW);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  i2s.setPins(PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN, PIN_MCLK);
  i2s.setTimeout(100);
  if (!i2s.begin(I2S_MODE_STD, SR, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) return;
  es8311_handle_t h = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!h) return;
  const es8311_clock_config_t clk = {false, false, true, SR * 256, SR};
  if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return;
  es8311_sample_frequency_config(h, SR * 256, SR);
  es8311_voice_volume_set(h, 70, NULL);
  es8311_microphone_config(h, false);
  rec = (int16_t *)ps_malloc(REC_MAX * sizeof(int16_t));
  spk = (int16_t *)ps_malloc(SR * SPK_MAX_SEC * sizeof(int16_t));
  micPowerDown();  // el micrófono arranca apagado
  q = xQueueCreate(4, sizeof(Cmd));
  xTaskCreatePinnedToCore(task, "voice", 4096, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(recTask, "rec", 4096, NULL, 4, NULL, 0);
  ok = true;
}

}  // namespace voice

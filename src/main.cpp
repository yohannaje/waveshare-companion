// Compañero del Waveshare ESP32-S3-Touch-LCD-1.54
// v0.1: personaje + 4 ambientes según la hora + animación de "escuchando" (sonido de ardilla, sin palabras todavía).
// El mundo se dibuja a 80x80 y se amplía x3 a la pantalla de 240x240.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include "voice.h"
#include <ImprovWiFiLibrary.h>

// ---------- pines (de los ejemplos oficiales de Waveshare) ----------
#define PIN_BL       46
#define PIN_PWR_HOLD 2   // mantiene la placa prendida con batería
#define BTN_BOOT     0   // clic: cambiar colores
#define BTN_PWR      5   // mantener 2 s: apagar
#define BTN_PLUS     4   // mantener: escuchar

Arduino_DataBus *bus = new Arduino_ESP32SPI(45 /*DC*/, 21 /*CS*/, 38 /*SCK*/, 39 /*MOSI*/, -1);
Arduino_GFX *panel = new Arduino_ST7789(bus, 40 /*RST*/, 0, true, 240, 240);
Arduino_Canvas *canvas = new Arduino_Canvas(240, 240, panel);

WiFiManager wm;
ImprovWiFi improv(&Serial);
Preferences prefs;

// ---------- colores ----------
static inline uint16_t C(uint32_t rgb) { return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x1F); }
static inline uint16_t blend(uint16_t d, uint16_t s, uint8_t a) {
  int dr = (d >> 11) & 31, dg = (d >> 5) & 63, db = d & 31;
  int sr = (s >> 11) & 31, sg = (s >> 5) & 63, sb = s & 31;
  dr += ((sr - dr) * a) / 255; dg += ((sg - dg) * a) / 255; db += ((sb - db) * a) / 255;
  return (dr << 11) | (dg << 5) | db;
}
static uint16_t mix888(uint32_t a, uint32_t b, float t) {
  int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  uint32_t r = lroundf(ar + (br - ar) * t), g = lroundf(ag + (bg - ag) * t), bl = lroundf(ab + (bb - ab) * t);
  return C((r << 16) | (g << 8) | bl);
}

// ---------- superficie de dibujo ----------
struct Surf { uint16_t *p; int w, h, cx0, cx1; };
static Surf *S;
static uint8_t gA = 255;
static inline void P(int x, int y, uint16_t c) {
  if (y < 0 || y >= S->h || x < S->cx0 || x >= S->cx1) return;
  uint16_t &d = S->p[y * S->w + x];
  d = (gA == 255) ? c : blend(d, c, gA);
}
static void rect(float fx, float fy, int w, int h, uint16_t c) {
  int x = lroundf(fx), y = lroundf(fy);
  for (int yy = y; yy < y + h; yy++) for (int xx = x; xx < x + w; xx++) P(xx, yy, c);
}
static void hline(int x, int y, int w, uint16_t c) { for (int i = 0; i < w; i++) P(x + i, y, c); }
static void disc(float fcx, float fcy, float r, uint16_t c) {
  int cx = lroundf(fcx), cy = lroundf(fcy), R = (int)ceilf(r);
  for (int dy = -R; dy <= R; dy++) {
    float v = r * r - dy * dy; if (v < 0) continue;
    int w = (int)floorf(sqrtf(v) + 0.35f);
    hline(cx - w, cy + dy, 2 * w + 1, c);
  }
}
static void ellipse(float fcx, float fcy, int rx, int ry, uint16_t c) {
  int cx = lroundf(fcx), cy = lroundf(fcy);
  for (int dy = -ry; dy <= ry; dy++) {
    float q = 1.0f - (float)(dy * dy) / (float)(ry * ry); if (q < 0) q = 0;
    int w = (int)floorf(rx * sqrtf(q) + 0.35f);
    hline(cx - w, cy + dy, 2 * w + 1, c);
  }
}
static void line(float fx0, float fy0, float fx1, float fy1, uint16_t c) {
  int x0 = lroundf(fx0), y0 = lroundf(fy0), x1 = lroundf(fx1), y1 = lroundf(fy1);
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) { P(x0, y0, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
static void starOutline(float cx, float cy, float R, uint16_t c) {
  float px[10], py[10];
  for (int i = 0; i < 10; i++) { float a = -M_PI / 2 + i * M_PI / 5, r = (i % 2) ? R * 0.48f : R; px[i] = cx + cosf(a) * r; py[i] = cy + sinf(a) * r; }
  for (int i = 0; i < 10; i++) line(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
}

const uint16_t LINE_C = C(0x2e2430), WHITE = C(0xffffff);
static void orect(float x, float y, int w, int h, uint16_t f) { rect(x, y, w, h, LINE_C); rect(x + 1, y + 1, w - 2, h - 2, f); }
static void odisc(float cx, float cy, float r, uint16_t f) { disc(cx, cy, r + 1, LINE_C); disc(cx, cy, r, f); }
static void oell(float cx, float cy, int rx, int ry, uint16_t f) { ellipse(cx, cy, rx + 1, ry + 1, LINE_C); ellipse(cx, cy, rx, ry, f); }

// ---------- hora, cielo ----------
static float nightAmt(float h) { if (h >= 8 && h < 18) return 0; if (h >= 18 && h < 21) return (h - 18) / 3; if (h >= 5 && h < 8) return 1 - (h - 5) / 3; return 1; }
// Se muda de habitacion cada ROOM_MINUTES minutos, siguiendo un recorrido segun el momento del dia
// (0 bano, 1 cocina, 2 living, 3 patio). De noche (23-6) duerme en el living.
#define ROOM_MINUTES 8
static int sceneFor(float h) {
  if (h >= 23 || h < 6) return 2;
  static const uint8_t P[4][6] = {
    {0, 1, 0, 2, 1, 0},   // 6-10   manana: mas en el bano y la cocina
    {1, 2, 1, 3, 1, 2},   // 10-15  mediodia: cocina, con salidas al patio
    {3, 2, 3, 1, 3, 0},   // 15-19  tarde: patio
    {2, 1, 2, 0, 2, 1},   // 19-23  noche: living
  };
  int per = h < 10 ? 0 : h < 15 ? 1 : h < 19 ? 2 : 3;
  int slot = ((int)(h * 60)) / ROOM_MINUTES;
  return P[per][slot % 6];
}
static bool sleepy(float h) { return h >= 23 || h < 6; }
static uint16_t sky(float h) {
  static const float H[] = {0, 5, 7, 9, 17, 19, 21, 24};
  static const uint32_t K[] = {0x6f6aa8, 0x6f6aa8, 0xffd9c6, 0xcfe7fb, 0xcfe7fb, 0xffcfd9, 0x6f6aa8, 0x6f6aa8};
  for (int i = 0; i < 7; i++) if (h >= H[i] && h <= H[i + 1]) { float d = H[i + 1] - H[i]; return mix888(K[i], K[i + 1], d > 0 ? (h - H[i]) / d : 0); }
  return C(0x6f6aa8);
}

// ---------- ambientes (80x80 cada uno, en orden: baño, cocina, living, patio) ----------
const int RW = 80, RH = 80, FLOORTOP = 62;
static void clipRoom(int o) { S->cx0 = o; S->cx1 = o + 80; }
static void wall(int o, uint16_t col, const float st[][3], int n) {
  rect(o, 0, RW, FLOORTOP, col);
  for (int i = 0; i < n; i++) starOutline(o + st[i][0], st[i][1], st[i][2], C(0xcfe0f5));
}
static void floorBand(int o, uint16_t col) {
  rect(o, FLOORTOP, RW, RH - FLOORTOP, col); rect(o, FLOORTOP, RW, 1, LINE_C);
  gA = 26; rect(o, 70, RW, 1, LINE_C); gA = 255;
}

static void bano() {
  const int o = 0; clipRoom(o);
  const float st[][3] = {{10, 9, 5}, {38, 6, 3}}; wall(o, C(0xf6fbff), st, 2);
  floorBand(o, C(0xe7f0fb));
  oell(o + 56, 20, 8, 10, C(0xfde3ea)); oell(o + 56, 20, 5, 7, C(0xeaf5ff)); rect(o + 53, 15, 1, 4, WHITE);
  orect(o + 46, 35, 20, 5, WHITE); orect(o + 52, 39, 8, 23, WHITE); rect(o + 55, 32, 2, 3, LINE_C);
  orect(o + 2, 42, 32, 20, WHITE); rect(o + 5, 61, 2, 2, LINE_C); rect(o + 29, 61, 2, 2, LINE_C);
  const int b[][3] = {{7, 41, 3}, {13, 39, 4}, {20, 41, 3}, {27, 40, 3}, {11, 34, 2}, {23, 34, 2}};
  for (auto &q : b) odisc(o + q[0], q[1], q[2], WHITE);
  orect(o + 68, 14, 11, 2, C(0xfff1e2)); orect(o + 70, 15, 7, 18, C(0xffd6df)); rect(o + 71, 22, 5, 1, C(0xf7b6c5));
}
static void cocina(float h) {
  const int o = 80; clipRoom(o);
  const float st[][3] = {{44, 8, 3}}; wall(o, C(0xfffaf1), st, 1);
  floorBand(o, C(0xfbeedd));
  orect(o + 5, 5, 30, 24, WHITE); rect(o + 7, 7, 26, 20, sky(h));
  if (nightAmt(h) > 0.6f) odisc(o + 27, 13, 2, C(0xfff4c2)); else { odisc(o + 13, 13, 2, C(0xffe9a0)); ellipse(o + 26, 21, 4, 2, WHITE); }
  rect(o + 19, 7, 2, 20, WHITE); rect(o + 19, 7, 1, 20, LINE_C); rect(o + 7, 16, 26, 2, WHITE); rect(o + 7, 16, 26, 1, LINE_C);
  orect(o + 3, 28, 34, 3, C(0xfff1e2));
  orect(o + 2, 38, 46, 4, C(0xfffdf8)); orect(o + 2, 41, 46, 21, C(0xfff1e2));
  orect(o + 5, 44, 19, 16, C(0xfffaf1)); orect(o + 26, 44, 19, 16, C(0xfffaf1)); rect(o + 21, 51, 1, 2, LINE_C); rect(o + 28, 51, 1, 2, LINE_C);
  odisc(o + 12, 33, 4, C(0xffd6df)); rect(o + 11, 27, 3, 1, LINE_C); rect(o + 17, 32, 3, 1, LINE_C); rect(o + 10, 31, 1, 1, WHITE);
  orect(o + 37, 30, 7, 8, C(0xeaf5ff)); rect(o + 38, 31, 5, 2, C(0xffe2c8));
  orect(o + 55, 12, 23, 50, C(0xf3f8ff)); rect(o + 56, 30, 21, 1, LINE_C); orect(o + 58, 18, 2, 9, WHITE); orect(o + 58, 34, 2, 11, WHITE);
  starOutline(o + 70, 20, 3, C(0xa9c8ea)); orect(o + 66, 38, 7, 7, C(0xfff6c8));
}
static void living() {
  const int o = 160; clipRoom(o);
  const float st[][3] = {{8, 8, 4}, {72, 10, 3}}; wall(o, C(0xfcf8ff), st, 2);
  floorBand(o, C(0xf6ece2));
  oell(o + 40, 70, 30, 4, C(0xffe3ea));
  orect(o + 27, 5, 26, 19, C(0xfff1e2)); rect(o + 30, 8, 20, 13, C(0xeaf5ff)); rect(o + 30, 16, 20, 5, C(0xd9f0cc)); rect(o + 30, 15, 20, 1, LINE_C);
  odisc(o + 45, 11, 1, C(0xffe9a0)); starOutline(o + 35, 11, 2, C(0xa9c8ea));
  orect(o + 11, 30, 58, 16, C(0xffd6df)); rect(o + 40, 32, 1, 14, LINE_C);
  orect(o + 11, 44, 58, 10, C(0xffc4d0)); orect(o + 5, 36, 9, 21, C(0xffc4d0)); orect(o + 66, 36, 9, 21, C(0xffc4d0));
  orect(o + 13, 53, 3, 9, C(0xfff1e2)); orect(o + 64, 53, 3, 9, C(0xfff1e2));
  odisc(o + 22, 39, 3, C(0xfff6c8)); odisc(o + 58, 39, 3, C(0xeaf5ff));
}
static void patio(float h) {
  const int o = 240; clipRoom(o);
  rect(o, 0, RW, FLOORTOP, sky(h));
  if (nightAmt(h) > 0.6f) {
    starOutline(o + 10, 8, 2, C(0xfff4c2)); starOutline(o + 34, 5, 2, C(0xfff4c2)); starOutline(o + 22, 22, 2, C(0xfff4c2));
    odisc(o + 48, 12, 4, C(0xfff4c2));
  } else {
    float k = (h - 7) / 12; if (k < 0) k = 0; if (k > 1) k = 1;
    odisc(o + 10 + k * 44, 12, 4, C(0xffe9a0));
    odisc(o + 26, 23, 3, WHITE); odisc(o + 34, 23, 3, WHITE); odisc(o + 30, 22, 4, WHITE); ellipse(o + 30, 24, 6, 2, WHITE);
  }
  oell(o + 14, 70, 28, 18, C(0xe3f4d6));
  for (int x = o + 2; x < o + 58; x += 9) orect(x, 42, 4, 20, WHITE);
  orect(o, 46, 58, 3, WHITE); orect(o, 53, 58, 3, WHITE);
  rect(o, FLOORTOP, RW, RH - FLOORTOP, C(0xd8efc8)); rect(o, FLOORTOP, RW, 1, LINE_C);
  gA = 64; for (int i = 0; i < 10; i++) { int x = o + (i * 29) % 76 + 2, y = 66 + (i * 7) % 12; rect(x, y, 1, 2, LINE_C); rect(x + 2, y + 1, 1, 1, LINE_C); } gA = 255;
  odisc(o + 8, 72, 1, C(0xffd6df)); odisc(o + 30, 76, 1, C(0xfff3c4)); odisc(o + 52, 70, 1, C(0xffd6df)); odisc(o + 72, 75, 1, WHITE);
  orect(o + 64, 26, 7, 36, C(0xffe2c8));
  odisc(o + 59, 26, 7, C(0xcdeccd)); odisc(o + 76, 26, 7, C(0xcdeccd)); odisc(o + 67, 17, 10, C(0xcdeccd)); ellipse(o + 67, 25, 13, 5, C(0xcdeccd));
  odisc(o + 62, 14, 1, C(0xffd6df)); odisc(o + 71, 20, 1, C(0xffd6df)); odisc(o + 58, 26, 1, C(0xffd6df));
}
static void pillars() {
  S->cx0 = 0; S->cx1 = 320;
  orect(78, 0, 4, FLOORTOP + 1, WHITE); orect(158, 0, 4, FLOORTOP + 1, WHITE); orect(238, 0, 4, FLOORTOP + 1, C(0xfff1e2));
}

// ---------- personaje (diseño de ella) ----------
struct Pal { uint16_t body, shade, cup, cupHi, rim, star, belly; };
static Pal PALS[3];
static int palIdx = 0;
static Pal *PAL;
const uint16_t OUTC = C(0x2e2430), EYE = C(0x1d1720), BLUSH = C(0xffc2cd), BLUSH2 = C(0xff9fb2), TONGUE = C(0xff8fa6), BUBBLE_OFF = C(0xe3d6ee);

static void starPix(int x, int y, uint16_t col) {
  rect(x, y - 2, 1, 1, OUTC); rect(x - 1, y - 1, 3, 1, OUTC); rect(x - 2, y, 5, 1, OUTC); rect(x - 1, y + 1, 3, 1, OUTC); rect(x - 1, y + 2, 1, 1, OUTC); rect(x + 1, y + 2, 1, 1, OUTC);
  rect(x, y - 1, 1, 1, col); rect(x - 1, y, 3, 1, col); rect(x, y + 1, 1, 1, col);
}
static void bellyStar(int x, int y, uint16_t col) {
  rect(x, y - 2, 1, 1, col); rect(x - 1, y - 1, 3, 1, col); rect(x - 2, y, 5, 1, col); rect(x - 1, y + 1, 3, 1, col); rect(x - 1, y + 2, 1, 1, col); rect(x + 1, y + 2, 1, 1, col);
}

enum St { IDLE, WALK, SIT, SLEEP, LISTEN, THINK, TALK, HAPPY };
struct Ch { float x, y, ty, phase; int face, wander; St state, rest; float nextThink; } ch;

static void drawChar(int x, int y, float t) {
  int bob = 0, hop = 0, sit = 0, fx = 0;
  St st = ch.state;
  if (st == WALK) { bob = ((int)floorf(ch.phase * 4) % 2) ? -1 : 0; fx = ch.face; }
  else if (st == IDLE) bob = lroundf(sinf(t * 2.4f) * 0.7f);
  else if (st == SIT) { sit = 3; bob = lroundf(sinf(t * 2) * 0.6f); }
  else if (st == SLEEP) { sit = 4; bob = sinf(t * 1.4f) > 0.3f ? 1 : 0; }
  else if (st == LISTEN) bob = sinf(t * 9) > 0.2f ? -1 : 0;
  else if (st == THINK) bob = lroundf(sinf(t * 3) * 0.6f);
  else if (st == TALK) bob = sinf(t * 14) > 0.4f ? -1 : 0;
  else if (st == HAPPY) hop = lroundf(fabsf(sinf(t * 8)) * 5);
  const int cx = x, by = y - 7 + bob - hop + sit, hy = by - 12, fy = y - 2 - hop;
  // pies
  int feet[2][2];
  if (st == SIT || st == SLEEP) { feet[0][0] = cx - 6; feet[0][1] = y - 1; feet[1][0] = cx + 6; feet[1][1] = y - 1; }
  else if (st == WALK) { int a = (int)floorf(ch.phase * 4) % 2; feet[0][0] = cx - 4; feet[0][1] = fy - (a ? 1 : 0); feet[1][0] = cx + 4; feet[1][1] = fy - (a ? 0 : 1); }
  else { feet[0][0] = cx - 4; feet[0][1] = fy; feet[1][0] = cx + 4; feet[1][1] = fy; }
  for (auto &f : feet) disc(f[0], f[1], 3, OUTC);
  for (auto &f : feet) disc(f[0], f[1], 2, PAL->body);
  // cuerpo
  ellipse(cx, by, 8, 6, OUTC); ellipse(cx, by, 7, 5, PAL->shade); ellipse(cx - 1, by - 1, 6, 4, PAL->body);
  bellyStar(cx + fx, by + 1, PAL->belly);
  // bracitos
  bool listenArm = st == LISTEN;
  int arms[2][2] = {{cx - 8, by - 1}, {cx + 8, by - 1}};
  if (st == WALK) { int a = (int)floorf(ch.phase * 4) % 2; arms[0][1] += a ? 1 : 0; arms[1][1] += a ? 0 : 1; }
  if (st == HAPPY) { arms[0][0] = cx - 9; arms[0][1] = by - 4; arms[1][0] = cx + 9; arms[1][1] = by - 4; }
  if (st == THINK) { arms[1][0] = cx + 4; arms[1][1] = hy + 8; }
  for (int i = 0; i < 2; i++) { if (listenArm && i == 1) continue; disc(arms[i][0], arms[i][1], 3, OUTC); disc(arms[i][0], arms[i][1], 2, PAL->body); }
  // auricular de atrás
  int cupLx = cx - 12 + fx, cupRx = cx + 12 + fx, cupY = hy + 2;
  ellipse(cupLx, cupY, 4, 6, OUTC); ellipse(cupLx, cupY, 3, 5, PAL->rim); ellipse(cupLx - 1, cupY, 2, 4, PAL->cup);
  // cabeza (nube con dos orejitas)
  int ex0 = cx - 7 + fx, ex1 = cx + 7 + fx, ey0 = hy - 7;
  disc(ex0, ey0, 6, OUTC); disc(ex1, ey0, 6, OUTC);
  ellipse(cx + fx, hy, 14, 9, OUTC);
  disc(ex0, ey0, 5, PAL->shade); disc(ex1, ey0, 5, PAL->shade);
  ellipse(cx + fx, hy, 13, 8, PAL->shade);
  disc(ex0 - 1, ey0 - 1, 4, PAL->body); disc(ex1 - 1, ey0 - 1, 4, PAL->body);
  ellipse(cx + fx - 1, hy - 1, 12, 7, PAL->body);
  rect(cx + fx - 1, hy - 10, 2, 1, OUTC);
  rect(ex0 - 2, ey0 - 3, 2, 1, WHITE);
  starPix(cx - 12 + fx, hy - 9, PAL->star);
  // auricular de adelante
  uint16_t glow = (st == LISTEN) ? (sinf(t * 8) > 0 ? PAL->cupHi : PAL->cup) : PAL->cup;
  ellipse(cupRx, cupY, 4, 7, OUTC); ellipse(cupRx, cupY, 3, 6, PAL->rim); ellipse(cupRx + 1, cupY, 2, 5, glow);
  rect(cupRx + 1, cupY - 2, 1, 1, PAL->star); rect(cupRx + 1, cupY + 2, 1, 1, WHITE);
  if (listenArm) { disc(cupRx + 1, cupY + 6, 3, OUTC); disc(cupRx + 1, cupY + 6, 2, PAL->body); }
  // cara
  int ey = hy - 1, e1 = cx + fx - 8, e2 = cx + fx + 4;
  bool blink = fmodf(t, 3.6f) < 0.13f && st != SLEEP && st != HAPPY;
  int es[2] = {e1, e2};
  for (int e : es) {
    if (st == SLEEP || blink) { rect(e, ey + 3, 1, 1, EYE); rect(e + 1, ey + 4, 2, 1, EYE); rect(e + 3, ey + 3, 1, 1, EYE); }
    else if (st == HAPPY) { rect(e, ey + 3, 1, 1, EYE); rect(e + 1, ey + 2, 2, 1, EYE); rect(e + 3, ey + 3, 1, 1, EYE); }
    else {
      int up = st == THINK ? -1 : 0;
      rect(e, ey + up, 4, 6, EYE);
      rect(e, ey + up, 1, 1, PAL->body); rect(e + 3, ey + up, 1, 1, PAL->body); rect(e, ey + 5 + up, 1, 1, PAL->body); rect(e + 3, ey + 5 + up, 1, 1, PAL->body);
      bool big = st == LISTEN && ((int)floorf(t * 4) % 2 == 0);
      if (big) rect(e + 1, ey + 1 + up, 2, 3, WHITE);
      else { rect(e + 2, ey + 1 + up, 1, 1, WHITE); rect(e + 1, ey + 2 + up, 3, 1, WHITE); rect(e + 2, ey + 3 + up, 1, 1, WHITE); }
    }
  }
  rect(e1 - 2, ey + 6, 3, 2, BLUSH); rect(e2 + 3, ey + 6, 3, 2, BLUSH);
  rect(e1 - 1, ey + 7, 1, 1, BLUSH2); rect(e2 + 4, ey + 7, 1, 1, BLUSH2);
  int mx = cx + fx, my = ey + 6;
  bool open = (st == TALK && ((int)floorf(t * 7) % 2 == 0)) || st == LISTEN;
  if (open || st == HAPPY) { rect(mx - 1, my, 3, 1, OUTC); rect(mx - 1, my + 1, 1, 1, OUTC); rect(mx + 1, my + 1, 1, 1, OUTC); rect(mx, my + 1, 1, 1, TONGUE); rect(mx - 1, my + 2, 3, 1, OUTC); }
  else { rect(mx - 2, my, 1, 1, OUTC); rect(mx - 1, my + 1, 1, 1, OUTC); rect(mx, my, 1, 1, OUTC); rect(mx + 1, my + 1, 1, 1, OUTC); rect(mx + 2, my, 1, 1, OUTC); }
  // extras
  if (st == LISTEN) {
    for (int k = 0; k < 3; k++) {
      if (fmodf(t * 5 + k, 3.0f) >= 1.6f) continue;
      int d = 17 + k * 3, len = 3 + k * 2;
      rect(cx + d, hy + 2 - len / 2.0f, 1, len, PAL->cup); rect(cx - d, hy + 2 - len / 2.0f, 1, len, PAL->cup);
    }
  }
  if (st == THINK) {
    disc(cx + 13, hy - 11, 2, OUTC); disc(cx + 13, hy - 11, 1, WHITE);
    disc(cx + 17, hy - 16, 3, OUTC); disc(cx + 17, hy - 16, 2, WHITE);
    ellipse(cx + 25, hy - 25, 9, 6, OUTC); ellipse(cx + 25, hy - 25, 8, 5, WHITE);
    int k = (int)floorf(t * 3) % 3;
    for (int i = 0; i < 3; i++) rect(cx + 20 + i * 4, hy - 26, 2, 2, i <= k ? OUTC : BUBBLE_OFF);
  }
  if (st == SLEEP) {
    float zz = fmodf(t * 0.8f, 1.0f);
    int zx = cx + 12 + lroundf(zz * 4), zy = hy - 12 - lroundf(zz * 10);
    gA = (uint8_t)(255 * (1 - zz * 0.6f));
    rect(zx, zy, 4, 1, OUTC); rect(zx + 2, zy + 1, 1, 1, OUTC); rect(zx + 1, zy + 2, 1, 1, OUTC); rect(zx, zy + 3, 4, 1, OUTC);
    gA = 255;
  }
  if (st == HAPPY) {
    const int pts[4][2] = {{-18, -8}, {18, -12}, {-15, 8}, {17, 6}};
    for (int i = 0; i < 4; i++) { if (((int)floorf(t * 6 + i)) % 2) continue; starPix(cx + pts[i][0], hy + pts[i][1], (i % 2) ? PAL->star : PAL->belly); }
  }
}

// ---------- estado del mundo ----------
const int W = 80, WORLD = 320, FLOOR_Y = 72, SOFA_Y = 54;
const int SPOTS[4] = {40, 120, 200, 280};
static uint16_t bgbuf[WORLD * 80];
static uint16_t fb[80 * 80];
static Surf bgS = {bgbuf, WORLD, 80, 0, WORLD};
static Surf fbS = {fb, 80, 80, 0, 80};
static float bgHour = -100;
static int cam = 0;
static St conv = IDLE; static bool inConv = false; static uint32_t convStart = 0; static bool holding = false;
static bool portalActive = false, timeStarted = false;
static String answer = "";
static const char *ROOM_NAMES[4] = {"el bano", "la cocina", "el living", "el patio"};

// Cartel de texto arriba (mismo estilo que el del WiFi). Corta en palabras, hasta 4 renglones de 17 letras.
static void drawBubble(const String &txt) {
  const int maxc = 17, maxLines = 4;
  String lines[maxLines]; int n = 0; String cur = "";
  int i = 0;
  while (i <= (int)txt.length() && n < maxLines) {
    int sp = txt.indexOf(' ', i); if (sp < 0) sp = txt.length();
    String w = txt.substring(i, sp);
    if (cur.length() == 0) cur = w;
    else if ((int)(cur.length() + 1 + w.length()) <= maxc) cur += " " + w;
    else { lines[n++] = cur; cur = w; }
    i = sp + 1;
  }
  if (n < maxLines && cur.length()) lines[n++] = cur;
  int h = 16 + n * 20, y0 = 6;  // arriba, para no tapar al personaje
  canvas->fillRoundRect(8, y0, 224, h, 10, RGB565_WHITE);
  canvas->drawRoundRect(8, y0, 224, h, 10, C(0x2e2430));
  canvas->setTextColor(C(0x2e2430)); canvas->setTextSize(2);
  for (int k = 0; k < n; k++) { canvas->setCursor(18, y0 + 9 + k * 20); canvas->print(lines[k]); }
}
static String localAnswer(float h, int idx) {
  char b[64];
  time_t now = time(nullptr);
  if (now < 1700000000) snprintf(b, sizeof(b), "Hola! Todavia no tengo hora: falta el WiFi");
  else { struct tm lt; localtime_r(&now, &lt); snprintf(b, sizeof(b), "Son las %02d:%02d. Estoy en %s.", lt.tm_hour, lt.tm_min, ROOM_NAMES[idx]); }
  return String(b);
}

static void buildBg(float h) {
  S = &bgS; gA = 255;
  bano(); cocina(h); living(); patio(h); pillars();
  bgHour = h;
}

static float nowHour() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 12.0f;  // todavía sin hora de internet: mediodía
  struct tm lt; localtime_r(&now, &lt);
  return lt.tm_hour + lt.tm_min / 60.0f;
}

static void startListen() { if ((inConv && conv == LISTEN) || voice::asking) return; holding = true; inConv = true; conv = LISTEN; convStart = millis(); answer = ""; voice::play(voice::LISTEN_ON); voice::startRecording(); }
static void stopListen() { if (!holding) return; holding = false; voice::stopRecording(); conv = THINK; convStart = millis(); voice::play(voice::LISTEN_OFF); voice::ask(); }

// ---------- botones ----------
struct Btn { int pin; bool down; uint32_t since; bool longFired; };
static Btn bPlus = {BTN_PLUS, false, 0, false}, bBoot = {BTN_BOOT, false, 0, false}, bPwr = {BTN_PWR, false, 0, false};
static void powerOff() {
  canvas->fillScreen(RGB565_WHITE);
  canvas->setTextColor(C(0x2e2430)); canvas->setTextSize(3); canvas->setCursor(78, 108); canvas->print("Chau");
  canvas->flush(); delay(600);
  analogWrite(PIN_BL, 0);
  digitalWrite(PIN_PWR_HOLD, LOW);
  delay(2000);  // si está enchufada por USB sigue prendida
}
static void pollButtons() {
  uint32_t ms = millis();
  // PLUS: mantener para hablar
  bool d = digitalRead(bPlus.pin) == LOW;
  if (d && !bPlus.down) { bPlus.down = true; bPlus.since = ms; startListen(); }
  else if (!d && bPlus.down && ms - bPlus.since > 30) { bPlus.down = false; stopListen(); }
  // BOOT: clic = cambiar colores
  d = digitalRead(bBoot.pin) == LOW;
  if (d && !bBoot.down) { bBoot.down = true; bBoot.since = ms; }
  else if (!d && bBoot.down) { bBoot.down = false; if (ms - bBoot.since > 30) { palIdx = (palIdx + 1) % 3; PAL = &PALS[palIdx]; prefs.putUChar("pal", palIdx); } }
  // PWR: mantener 2 s = apagar
  d = digitalRead(bPwr.pin) == LOW;
  if (d && !bPwr.down) { bPwr.down = true; bPwr.since = ms; bPwr.longFired = false; }
  else if (d && bPwr.down && !bPwr.longFired && ms - bPwr.since > 2000) { bPwr.longFired = true; powerOff(); }
  else if (!d) bPwr.down = false;
}

// ---------- setup / loop ----------
void setup() {
  pinMode(PIN_PWR_HOLD, OUTPUT); digitalWrite(PIN_PWR_HOLD, HIGH);
  pinMode(BTN_PLUS, INPUT_PULLUP); pinMode(BTN_BOOT, INPUT_PULLUP); pinMode(BTN_PWR, INPUT_PULLUP);
  Serial.begin(115200);

  PALS[0] = {C(0xffffff), C(0xe6ebf5), C(0xa9c8ea), C(0xd6e6f8), C(0xf6efd9), C(0xf7e08a), C(0xb9d3f0)};
  PALS[1] = {C(0xf9d4b0), C(0xefbd93), C(0xe8766a), C(0xf6a99f), C(0xfff1e2), C(0xee8a7a), C(0xee8a7a)};
  PALS[2] = {C(0xfff6ea), C(0xf3dfc8), C(0xf2b3c3), C(0xfbd7e0), C(0xffffff), C(0xf6c56b), C(0xa9c8ea)};
  prefs.begin("companion", false);
  palIdx = prefs.getUChar("pal", 0) % 3; PAL = &PALS[palIdx];

  canvas->begin(80000000);
  pinMode(PIN_BL, OUTPUT); analogWrite(PIN_BL, 220);
  voice::begin();

  // WiFi sin bloquear la animación: si no hay red guardada abre la red "Waveshare-setup"
  // Opción 1 (más fácil): cargar el WiFi desde la página de instalación en Chrome, por USB (protocolo Improv).
  improv.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32_S3, "Companero", "0.2", "Waveshare");
  // Opción 2: red "Waveshare-setup" desde el celular.
  WiFi.mode(WIFI_STA);
  wm.setDebugOutput(false);
  wm.setConfigPortalBlocking(false);
  wm.setTitle("Waveshare");
  wm.setAPCallback([](WiFiManager *) { WiFi.setTxPower(WIFI_POWER_8_5dBm); });
  portalActive = !wm.autoConnect("Waveshare-setup");
  if (portalActive) WiFi.setTxPower(WIFI_POWER_8_5dBm);

  float h = nowHour();
  int i = sceneFor(h);
  ch = {(float)SPOTS[i], (float)FLOOR_Y, (float)FLOOR_Y, 0, 1, 0, IDLE, IDLE, 4};
  cam = i * 80;
}

static uint32_t lastMs = 0;
void loop() {
  improv.handleSerial();
  if (portalActive) { if (wm.process()) portalActive = false; }
  if (!timeStarted && WiFi.status() == WL_CONNECTED) {
    if (portalActive) { wm.stopConfigPortal(); portalActive = false; }
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com"); timeStarted = true;
  }
  pollButtons();

  uint32_t ms = millis();
  float dt = (ms - lastMs) / 1000.0f; if (dt > 0.05f) dt = 0.05f; lastMs = ms;
  float t = ms / 1000.0f;
  float h = nowHour();
  if (fabsf(h - bgHour) >= 0.25f) buildBg(h);
  int idx = sceneFor(h);

  if (inConv) {
    uint32_t el = ms - convStart;
    ch.state = conv;
    if (conv == LISTEN && holding && !voice::recording && el > 1000) stopListen();   // llegó al máximo de grabación
    if (conv == THINK && voice::answerReady && el > 600) { voice::answerReady = false; conv = TALK; convStart = ms; answer = voice::answerText; voice::play(voice::spkLen ? voice::SPEAK : voice::CHATTER); }
    else if (conv == THINK && el > 50000) { conv = TALK; convStart = ms; answer = "Me quede pensando... proba de nuevo"; voice::play(voice::CHATTER); }
    else if (conv == TALK && el > 400 && !voice::busy) { conv = HAPPY; convStart = ms; }
    else if (conv == HAPPY && el > 4000) { inConv = false; ch.nextThink = t + 4; answer = ""; }
  }
  bool traveling = false;
  if (!inConv) {
    float dest = SPOTS[idx] + ch.wander, dx = dest - ch.x;
    traveling = fabsf(SPOTS[idx] - ch.x) > 14;
    if (fabsf(dx) > 0.5f) { ch.state = WALK; ch.face = dx > 0 ? 1 : -1; float step = 24 * dt; ch.x += (dx > 0 ? 1 : -1) * (fabsf(dx) < step ? fabsf(dx) : step); ch.phase += dt * 1.5f; ch.ty = FLOOR_Y; }
    else {
      ch.x = dest;
      if (idx == 2 && sleepy(h)) { ch.state = SLEEP; ch.ty = SOFA_Y; }
      else {
        if (t > ch.nextThink) {
          ch.nextThink = t + 5 + (esp_random() % 5000) / 1000.0f;
          if ((esp_random() % 100) < 45) { ch.wander = (int)(esp_random() % 21) - 10; ch.rest = IDLE; }
          else ch.rest = (ch.rest == SIT) ? IDLE : SIT;
        }
        ch.state = ch.rest; ch.ty = (ch.rest == SIT && idx == 2) ? SOFA_Y : FLOOR_Y;
      }
    }
  } else if (conv != LISTEN && ch.ty != SOFA_Y) ch.ty = FLOOR_Y;
  if (fabsf(ch.ty - ch.y) < 0.6f) ch.y = ch.ty; else ch.y += (ch.ty - ch.y) * fminf(1.0f, dt * 8);

  int px = lroundf(ch.x);
  int target = traveling ? constrain(px - W / 2, 0, WORLD - W) : idx * 80;
  if (cam < target) cam = min(target, cam + max(1, (int)lroundf((target - cam) * 0.15f)));
  else if (cam > target) cam = max(target, cam - max(1, (int)lroundf((cam - target) * 0.15f)));

  // componer el cuadro de 80x80
  for (int y = 0; y < 80; y++) memcpy(&fb[y * 80], &bgbuf[y * WORLD + cam], 80 * 2);
  S = &fbS; S->cx0 = 0; S->cx1 = 80; gA = 255;
  float n = nightAmt(h);
  if (n > 0) { gA = (uint8_t)(n * 0.28f * 255); rect(0, 0, 80, 80, C(0x3c3478)); gA = 255; }
  drawChar(px - cam, lroundf(ch.y), t);
  if (inConv && conv == LISTEN) {
    gA = (uint8_t)(255 * (0.55f + 0.45f * sinf(t * 6)));
    rect(0, 0, 80, 1, PAL->cup); rect(0, 79, 80, 1, PAL->cup); rect(0, 0, 1, 80, PAL->cup); rect(79, 0, 1, 80, PAL->cup);
    gA = 255;
  }

  // ampliar x3 a la pantalla
  uint16_t *out = canvas->getFramebuffer();
  for (int y = 0; y < 80; y++) {
    uint16_t *row = &out[(y * 3) * 240];
    for (int x = 0; x < 80; x++) { uint16_t c = fb[y * 80 + x]; row[x * 3] = c; row[x * 3 + 1] = c; row[x * 3 + 2] = c; }
    memcpy(row + 240, row, 480); memcpy(row + 480, row, 480);
  }
  if (inConv && (conv == TALK || conv == HAPPY) && answer.length()) drawBubble(answer);
  else if (portalActive) drawBubble("Falta el WiFi: cargalo en Chrome");
  canvas->flush();

  int32_t wait = 33 - (int32_t)(millis() - ms);
  if (wait > 0) delay(wait);
}

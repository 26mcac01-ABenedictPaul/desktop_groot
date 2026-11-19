// ==================================================================
// DeskBuddy Groot - ESP32-C3 Super Mini + 1.3" SH1106 OLED + touch
// Bluetooth LE only (no WiFi) so the battery lasts and the link stays up.
//
// TOUCH
//   single tap  : next emotion (20 animated faces, in order)
//   double tap  : angry (4 angry faces, in order)
//   long press  : app menu  ->  Time | Dino game | Temperature | Weather
//   in menu     : tap = next app, long press = open, double tap = back
//   in an app   : tap = back to menu, double tap = back to face
//   in dino     : touch = jump, long press = exit
//
// PHONE (Bluetooth LE, Nordic UART service)
//   Open deskbuddy_phone.html in Chrome, press Connect.
//   It sends the time and the weather, and re-sends every 10 minutes
//   while connected. The buddy keeps its own clock between syncs.
//
// Libraries: Adafruit GFX Library, Adafruit SH110X  (BLE is built in)
// Board: "ESP32C3 Dev Module", USB CDC On Boot: Enabled
//        (if the sketch is too big: Partition Scheme -> Huge APP)
// ==================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <math.h>
#include <time.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// ---------------- Hardware ----------------
#define SDA_PIN 8
#define SCL_PIN 9
#define TOUCH_PIN 7

Adafruit_SH1106G display(128, 64, &Wire, -1);

#undef WHITE
#undef BLACK
#define WHITE SH110X_WHITE
#define BLACK SH110X_BLACK
#define INV SH110X_INVERSE

// ---------------- Constants ----------------
#define LONG_PRESS_MS 800
#define DOUBLE_TAP_MS 300
#define EM_COUNT 20
#define AN_COUNT 4
#define EM_HOLD_MS 14000UL
#define FRAME_MS 30

const int EYE_LX = 36;   // left eye centre x
const int EYE_RX = 92;   // right eye centre x
const int CY = 30;   // eye centre y

enum Screen { SCR_FACE, SCR_MENU, SCR_TIME, SCR_DINO, SCR_TEMP, SCR_WEATHER };
enum Ev { EV_NONE, EV_TAP, EV_DOUBLE, EV_LONG };

enum Emotion {
  E_HAPPY, E_LOVE, E_BLUSH, E_SHY, E_COLD, E_HOT, E_SNOW, E_WINDY, E_SURPRISED, E_SLEEPY,
  E_SAD, E_CRYING, E_WINK, E_EXCITED, E_DIZZY, E_SUSPICIOUS, E_LAUGH, E_THINKING, E_MUSIC, E_COOL,
  E_ANNOYED, E_ANGRY, E_FURIOUS, E_GRUMPY,
  E_IDLE
};

struct DObs {
  float x;
  int w, h, y, type;
  bool on;
};

const char* EM_NAMES[] = {
  "Happy", "Love", "Blush", "Shy", "Cold", "Hot", "Snow", "Windy", "Surprised", "Sleepy",
  "Sad", "Crying", "Wink", "Excited", "Dizzy", "Suspicious", "Laugh", "Thinking", "Music", "Cool",
  "Annoyed", "Angry", "Furious", "Grumpy", "Idle"
};

const unsigned char bmp_anger[] PROGMEM = { 0x00, 0x00, 0x11, 0x10, 0x2a, 0x90, 0x44, 0x40, 0x80, 0x20, 0x80, 0x20, 0x44, 0x40, 0x2a, 0x90, 0x11, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// ---------------- State ----------------
Screen screen = SCR_FACE;
int curEm = E_IDLE;
unsigned long emStart = 0;
int tapIdx = -1;
int angIdx = -1;
unsigned long lastInteract = 0;
unsigned long lastFrame = 0;
float popK = 1.0f;
int menuSel = 0;
const char* MENU_NAMES[4] = { "Time", "Dino", "Temp", "Weather" };

// Time (set by phone)
bool timeSynced = false;
long syncEpoch = 0;
unsigned long syncMillis = 0;
int tzMin = 0;

// Weather (set by phone)
bool wxValid = false;
int wxTemp = 0, wxFeels = 0, wxHum = 0, wxCode = 0, wxWind = 0, wxHi = 0, wxLo = 0;
int fcCode[3] = { 0, 0, 0 };
int fcTemp[3] = { 0, 0, 0 };
bool fcValid[3] = { false, false, false };
unsigned long wxAt = 0;

// BLE
volatile bool bleConnected = false;
volatile bool needAdv = false;
unsigned long advAt = 0;
#define NUS_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Touch
bool touchDownEdge = false;

// Pending commands from BLE (handled in loop, not in the BLE task)
String pendingCmd = "";
volatile bool hasCmd = false;

Preferences prefs;

// ==================================================
// Time helpers
// ==================================================
long nowEpoch() {
  return syncEpoch + (long)((millis() - syncMillis) / 1000UL);
}

bool getLocal(struct tm* t) {
  if (!timeSynced) return false;
  time_t lt = (time_t)(nowEpoch() + (long)tzMin * 60L);
  gmtime_r(&lt, t);
  return true;
}

// ==================================================
// Emotion control
// ==================================================
void setEmotion(int id, bool byTouch) {
  curEm = id;
  emStart = millis();
  if (byTouch) lastInteract = millis();
}

void nextEmotion() {
  tapIdx = (tapIdx + 1) % EM_COUNT;
  setEmotion(tapIdx, true);
}

void nextAngry() {
  angIdx = (angIdx + 1) % AN_COUNT;
  setEmotion(E_ANNOYED + angIdx, true);
}

void emotionFromWeather() {
  if (!wxValid || screen != SCR_FACE) return;
  int m = wxCode;
  if ((m >= 71 && m <= 77) || m == 85 || m == 86) setEmotion(E_SNOW, false);
  else if (wxWind >= 35) setEmotion(E_WINDY, false);
  else if (wxTemp >= 34) setEmotion(E_HOT, false);
  else if (wxTemp <= 14) setEmotion(E_COLD, false);
}

// ==================================================
// Weather text + icons
// ==================================================
const char* wxMain(int c) {
  if (c == 0 || c == 1) return "Clear";
  if (c == 2 || c == 3 || c == 45 || c == 48) return "Clouds";
  if (c >= 51 && c <= 57) return "Drizzle";
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return "Rain";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "Snow";
  if (c >= 95) return "Storm";
  return "Clouds";
}

const char* wxDesc(int c) {
  if (c == 0) return "Clear sky";
  if (c == 1) return "Mostly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return "Rain";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "Snow";
  if (c >= 95) return "Thunderstorm";
  return "Cloudy";
}

void drawCloudShape(int cx, int cy, int r) {
  display.fillCircle(cx - r * 4 / 10, cy + r / 10, r * 4 / 10, WHITE);
  display.fillCircle(cx + r / 10, cy - r / 10, r * 5 / 10, WHITE);
  display.fillCircle(cx + r * 5 / 10, cy + r * 2 / 10, r * 3 / 10, WHITE);
  display.fillRect(cx - r * 4 / 10, cy + r / 10, r * 9 / 10, r * 4 / 10, WHITE);
}

void drawWx(int code, int cx, int cy, int r) {
  const char* m = wxMain(code);
  if (!strcmp(m, "Clear")) {
    display.fillCircle(cx, cy, r / 2, WHITE);
    for (int i = 0; i < 8; i++) {
      float a = i * PI / 4.0f;
      display.drawLine(cx + cosf(a) * (r * 0.7f), cy + sinf(a) * (r * 0.7f), cx + cosf(a) * r, cy + sinf(a) * r, WHITE);
    }
  } else if (!strcmp(m, "Rain") || !strcmp(m, "Drizzle")) {
    drawCloudShape(cx, cy - r / 4, r);
    for (int i = -1; i <= 1; i++) {
      int x = cx + i * r * 4 / 10;
      display.drawLine(x, cy + r * 55 / 100, x - 2, cy + r * 95 / 100, WHITE);
    }
  } else if (!strcmp(m, "Storm")) {
    drawCloudShape(cx, cy - r / 4, r);
    display.drawLine(cx + 2, cy + r / 2, cx - 3, cy + r * 3 / 4, WHITE);
    display.drawLine(cx - 3, cy + r * 3 / 4, cx + 3, cy + r * 3 / 4, WHITE);
    display.drawLine(cx + 3, cy + r * 3 / 4, cx - 2, cy + r, WHITE);
  } else if (!strcmp(m, "Snow")) {
    drawCloudShape(cx, cy - r / 4, r);
    for (int i = -1; i <= 1; i++) display.fillCircle(cx + i * r * 4 / 10, cy + r * 3 / 4, 1, WHITE);
  } else {
    drawCloudShape(cx, cy, r);
  }
}

// ==================================================
// Drawing helpers for faces
// ==================================================
float easeOutBack(float x) {
  if (x <= 0) return 0;
  if (x >= 1) return 1;
  const float c1 = 1.70158f, c3 = 2.70158f;
  float y = x - 1;
  return 1 + c3 * y * y * y + c1 * y * y;
}

void rrect(int cx, int cy, int w, int h, int r) {
  int hh = (int)(h * popK);
  if (hh < 2) hh = 2;
  if (w < 2) w = 2;
  int rr = r;
  if (rr > w / 2) rr = w / 2;
  if (rr > hh / 2) rr = hh / 2;
  display.fillRoundRect(cx - w / 2, cy - hh / 2, w, hh, rr, WHITE);
}

void lidFlat(int cx, int cy, int w, int h, int depth) {
  display.fillRect(cx - w / 2 - 1, cy - h / 2 - 1, w + 2, depth + 1, BLACK);
}

// cutLeft=true removes the top-left corner (sad outer / angry inner on right eye)
void lidSlope(int cx, int cy, int w, int h, int depth, bool cutLeft) {
  int x0 = cx - w / 2 - 1, x1 = cx + w / 2 + 1, y0 = cy - h / 2 - 1;
  if (cutLeft) display.fillTriangle(x0, y0, x1, y0, x0, y0 + depth, BLACK);
  else display.fillTriangle(x0, y0, x1, y0, x1, y0 + depth, BLACK);
}

float blinkK(unsigned long t, unsigned long period = 3300, unsigned long dur = 180, unsigned long offset = 1800) {
  unsigned long ph = (t + offset) % period;
  if (ph > period - dur) {
    float x = (float)(ph - (period - dur)) / (float)dur;
    return 1.0f - 0.92f * sinf(PI * x);
  }
  return 1.0f;
}

void caret(int cx, int cy, int hw, int up, int down, int th) {
  for (int k = -(th / 2); k <= th / 2; k++) {
    display.drawLine(cx - hw, cy + down + k, cx, cy - up + k, WHITE);
    display.drawLine(cx, cy - up + k, cx + hw, cy + down + k, WHITE);
  }
}

void smileArc(int cx, int cy, int hw, int up, int down, int th) {
  for (int k = -(th / 2); k <= th / 2; k++) {
    display.drawLine(cx - hw, cy - down + k, cx, cy + up + k, WHITE);
    display.drawLine(cx, cy + up + k, cx + hw, cy - down + k, WHITE);
  }
}

void mouthSmile(int cx, int cy, int w) {
  for (int k = 0; k < 2; k++) {
    display.drawLine(cx - w / 2, cy + k, cx - w / 4, cy + 4 + k, WHITE);
    display.drawLine(cx - w / 4, cy + 4 + k, cx + w / 4, cy + 4 + k, WHITE);
    display.drawLine(cx + w / 4, cy + 4 + k, cx + w / 2, cy + k, WHITE);
  }
}

void mouthFrown(int cx, int cy, int w) {
  for (int k = 0; k < 2; k++) {
    display.drawLine(cx - w / 2, cy + 4 + k, cx - w / 4, cy + k, WHITE);
    display.drawLine(cx - w / 4, cy + k, cx + w / 4, cy + k, WHITE);
    display.drawLine(cx + w / 4, cy + k, cx + w / 2, cy + 4 + k, WHITE);
  }
}

void drawHeart(int x, int y, int size) {
  int r = size / 4;
  if (r < 1) r = 1;
  display.fillCircle(x - r, y - r / 2, r, WHITE);
  display.fillCircle(x + r, y - r / 2, r, WHITE);
  display.fillTriangle(x - 2 * r, y - r / 2, x + 2 * r, y - r / 2, x, y + size / 2, WHITE);
}

void drawDrop(int x, int y) {
  display.fillCircle(x, y + 2, 2, WHITE);
  display.fillTriangle(x - 2, y + 1, x + 2, y + 1, x, y - 4, WHITE);
}

void drawPlus(int x, int y, int s) {
  display.drawFastHLine(x - s, y, 2 * s + 1, WHITE);
  display.drawFastVLine(x, y - s, 2 * s + 1, WHITE);
}

void drawFlake(int x, int y) {
  display.drawLine(x - 2, y, x + 2, y, INV);
  display.drawLine(x, y - 2, x, y + 2, INV);
  display.drawLine(x - 1, y - 1, x + 1, y + 1, INV);
  display.drawLine(x - 1, y + 1, x + 1, y - 1, INV);
}

void drawNote(int x, int y) {
  display.fillCircle(x, y, 2, WHITE);
  display.drawFastVLine(x + 2, y - 8, 8, WHITE);
  display.drawLine(x + 2, y - 8, x + 5, y - 5, WHITE);
}

void drawSpiral(int cx, int cy, float maxR, float phase) {
  float px = cx, py = cy;
  for (float a = 0.4f; a < 6.0f * PI; a += 0.4f) {
    float r = maxR * a / (6.0f * PI);
    float x = cx + r * cosf(a + phase);
    float y = cy + r * sinf(a + phase);
    display.drawLine((int)px, (int)py, (int)x, (int)y, WHITE);
    display.drawLine((int)px + 1, (int)py, (int)x + 1, (int)y, WHITE);
    px = x;
    py = y;
  }
}

// ==================================================
// The 20 emotions + 4 angry + idle
// ==================================================
static int gzx(unsigned long seg) { return (int)((seg * 37UL + 11UL) % 9UL) - 4; }
static int gzy(unsigned long seg) { return (int)((seg * 53UL + 5UL) % 5UL) - 2; }

void emIdle(unsigned long t) {
  float bk = blinkK(t);
  unsigned long seg = t / 2300UL;
  unsigned long prev = seg > 0 ? seg - 1 : 0;
  float ph = (float)(t % 2300UL) / 350.0f;
  if (ph > 1) ph = 1;
  float dx = gzx(prev) + (gzx(seg) - gzx(prev)) * ph;
  float dy = gzy(prev) + (gzy(seg) - gzy(prev)) * ph;
  float br = sinf(t / 700.0f) * 1.2f;
  rrect(EYE_LX + (int)(dx * 1.5f), CY + (int)(dy * 1.5f), 30, (int)((30 + br) * bk), 8);
  rrect(EYE_RX + (int)(dx * 1.5f), CY + (int)(dy * 1.5f), 30, (int)((30 + br) * bk), 8);
}

void emHappy(unsigned long t) {
  int bob = (int)(sinf(t / 140.0f) * 2);
  caret(EYE_LX, CY + bob, 11, 7, 5, 3);
  caret(EYE_RX, CY + bob, 11, 7, 5, 3);
  mouthSmile(64, 48 + bob, 18);
}

void emLove(unsigned long t) {
  int s = 26 + (int)(sinf(t / 120.0f) * 3);
  drawHeart(EYE_LX, CY, s);
  drawHeart(EYE_RX, CY, s);
  for (int i = 0; i < 3; i++) {
    int ph = (int)(((t / 25) + i * 40) % 100);
    int y = 60 - ph * 6 / 10;
    int x = 22 + i * 42 + (int)(sinf((t + i * 300) / 250.0f) * 4);
    if (y > 2) drawHeart(x, y, 7);
  }
}

void emBlush(unsigned long t) {
  float bk = blinkK(t);
  int bob = (int)(sinf(t / 260.0f) * 1.5f);
  rrect(EYE_LX, CY - 4 + bob, 26, (int)(20 * bk), 7);
  rrect(EYE_RX, CY - 4 + bob, 26, (int)(20 * bk), 7);
  for (int e = 0; e < 2; e++) {
    int cx = e ? EYE_RX : EYE_LX;
    for (int j = 0; j < 3; j++) display.drawLine(cx - 9 + j * 6, CY + 17 + bob, cx - 6 + j * 6, CY + 12 + bob, WHITE);
  }
  mouthSmile(64, 46 + bob, 10);
}

void emShy(unsigned long t) {
  float bk = blinkK(t, 2600, 170, 900);
  int glance = (sinf(t / 900.0f) > 0.7f) ? 10 : 0;
  int dx = -8 + glance, dy = 5;
  rrect(EYE_LX + dx, CY + dy, 22, (int)(18 * bk), 6);
  rrect(EYE_RX + dx, CY + dy, 22, (int)(18 * bk), 6);
  lidFlat(EYE_LX + dx, CY + dy, 22, 18, 3);
  lidFlat(EYE_RX + dx, CY + dy, 22, 18, 3);
  for (int e = 0; e < 2; e++) {
    int cx = e ? EYE_RX : EYE_LX;
    for (int j = 0; j < 3; j++) display.drawLine(cx - 9 + j * 6, CY + 24, cx - 6 + j * 6, CY + 19, WHITE);
  }
}

void emCold(unsigned long t) {
  int sx = (int)(sinf(t / 17.0f) * 1.6f);
  float bk = blinkK(t, 2800, 150, 700);
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + sx;
    rrect(cx, CY + 3, 26, (int)(22 * bk), 6);
    lidSlope(cx, CY + 3, 26, 22, 7, e == 0);
  }
  for (int i = 0; i < 3; i++) {
    int jx = ((t / 70 + i) % 2) ? 2 : -2;
    display.drawLine(4 + i * 4, 20, 4 + i * 4 + jx, 34, WHITE);
    display.drawLine(118 - i * 4, 20, 118 - i * 4 + jx, 34, WHITE);
  }
  for (int i = 0; i < 6; i++) {
    int x = 6 + i * 22;
    int len = 4 + (i % 3) * 2;
    display.fillTriangle(x, 0, x + 6, 0, x + 3, len, WHITE);
  }
  for (int i = 0; i < 3; i++) {
    int ph = (int)(((t / 45) + i * 33) % 100);
    int y = 56 - ph * 3 / 10;
    int r = 1 + ph / 35;
    int x = 72 + ph / 5;
    display.drawCircle(x, y, r, WHITE);
  }
  int off = ((t / 60) % 2) ? 1 : 0;
  for (int x = 56; x < 72; x += 4) {
    int y1 = 49 + ((((x - 56) / 4) + off) % 2) * 3;
    int y2 = 49 + ((((x - 52) / 4) + off) % 2) * 3;
    display.drawLine(x, y1, x + 4, y2, WHITE);
  }
}

void emHot(unsigned long t) {
  int depth = 14 + (int)(sinf(t / 420.0f) * 2);
  for (int e = 0; e < 2; e++) {
    int cx = e ? EYE_RX : EYE_LX;
    rrect(cx, CY + 2, 28, 28, 7);
    lidFlat(cx, CY + 2, 28, 28, depth);
  }
  for (int w = 0; w < 2; w++) {
    int px = 0, py = 0;
    for (int x = 0; x <= 128; x += 4) {
      int y = 4 + w * 5 + (int)(sinf((x + t / 35.0f) / 7.0f) * 2);
      if (x > 0) display.drawLine(px, py, x, y, WHITE);
      px = x;
      py = y;
    }
  }
  for (int i = 0; i < 2; i++) {
    int ph = (int)(((t / 16) + i * 50) % 100);
    drawDrop(i ? 118 : 10, 12 + ph * 4 / 10);
  }
  int th = 8 + (int)(sinf(t / 140.0f) * 2);
  display.fillRoundRect(58, 46, 12, th, 3, WHITE);
  display.drawFastVLine(64, 47, th - 3, BLACK);
}

void emSnow(unsigned long t) {
  float bk = blinkK(t);
  for (int e = 0; e < 2; e++) {
    int cx = e ? EYE_RX : EYE_LX;
    rrect(cx, CY - 1, 28, (int)(30 * bk), 8);
    if (bk > 0.6f) display.fillRoundRect(cx - 9, CY - 12, 7, 7, 2, BLACK);
  }
  mouthSmile(64, 50, 14);
  for (int i = 0; i < 16; i++) {
    long spd = 8 + (i % 3) * 4;
    int y = (int)(((long)t * spd / 100 + i * 23) % 72) - 4;
    int x = (i * 9 + (int)(sinf(t / 600.0f + i) * 4) + 128) % 128;
    drawFlake(x, y);
  }
}

void emWindy(unsigned long t) {
  int sw = (int)(sinf(t / 190.0f) * 4);
  rrect(EYE_LX + sw + 3, CY, 30, 12, 5);
  rrect(EYE_RX + sw + 3, CY, 30, 12, 5);
  display.drawCircle(64 + sw, 50, 3, WHITE);
  int ys[5] = { 6, 17, 30, 43, 54 };
  for (int i = 0; i < 5; i++) {
    int len = 22 + (i % 2) * 14;
    int x = (int)((((long)t * (3 + i % 3)) / 18 + i * 41) % 190) - 50;
    display.drawFastHLine(x, ys[i], len, INV);
    display.drawLine(x + len, ys[i], x + len + 3, ys[i] - 2, INV);
  }
}

void emSurprised(unsigned long t) {
  int dx = (int)(sinf(t / 40.0f) * 1.0f);
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + dx;
    rrect(cx, CY, 24, 38, 8);
    display.fillRoundRect(cx - 4, CY - 4, 8, 8, 2, BLACK);
  }
  display.fillRect(61, 4, 3, 9, WHITE);
  display.fillRect(61, 15, 3, 3, WHITE);
}

void emSleepy(unsigned long t) {
  int hh = 6 + (int)((sinf(t / 900.0f) + 1.0f) * 5.0f);
  int dy = (int)(sinf(t / 900.0f) * 3);
  rrect(EYE_LX, CY + 8 + dy, 30, hh, 4);
  rrect(EYE_RX, CY + 8 + dy, 30, hh, 4);
  for (int i = 0; i < 3; i++) {
    int ph = (int)(((t / 50) + i * 33) % 100);
    display.setTextColor(WHITE);
    display.setTextSize(1 + (i % 2));
    display.setCursor(100 + i * 6 + ph / 12, 22 - ph / 5 - i * 3);
    display.print(i == 0 ? "z" : "Z");
  }
  display.setTextSize(1);
}

void emSad(unsigned long t) {
  float bk = blinkK(t, 3800, 200, 1000);
  int dy = (int)(sinf(t / 600.0f) * 1.5f);
  for (int e = 0; e < 2; e++) {
    int cx = e ? EYE_RX : EYE_LX;
    rrect(cx, CY + 2 + dy, 28, (int)(30 * bk), 8);
    lidSlope(cx, CY + 2 + dy, 28, 30, 13, e == 0);
  }
  int ph = (int)((t / 30) % 100);
  drawDrop(EYE_LX - 8, CY + 18 + ph * 2 / 10 + dy);
  mouthFrown(64, 50, 14);
}

void emCrying(unsigned long t) {
  int sx = (int)(sinf(t / 45.0f) * 1.0f);
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + sx;
    rrect(cx, CY, 28, 28, 8);
    lidSlope(cx, CY, 28, 28, 13, e == 0);
    for (int i = 0; i < 2; i++) {
      int ph = (int)(((t / 14) + i * 50 + e * 20) % 100);
      int y = CY + 14 + ph * 28 / 100;
      int x = cx + (i ? 9 : -9);
      display.fillCircle(x, y, 2, WHITE);
    }
  }
  for (int x = 54; x < 74; x += 4) {
    int y1 = 50 + (((x - 54) / 4) % 2) * 3;
    int y2 = 50 + (((x - 50) / 4) % 2) * 3;
    display.drawLine(x, y1, x + 4, y2, WHITE);
  }
}

void emWink(unsigned long t) {
  float bk = blinkK(t);
  unsigned long ph = t % 2200UL;
  bool closed = (ph > 200 && ph < 1100);
  rrect(EYE_LX, CY, 28, (int)(30 * bk), 8);
  if (closed) {
    caret(EYE_RX, CY, 11, 7, 5, 3);
    drawPlus(114, 10, 3);
    display.drawLine(111, 7, 117, 13, WHITE);
    display.drawLine(111, 13, 117, 7, WHITE);
  } else {
    rrect(EYE_RX, CY, 28, (int)(30 * bk), 8);
  }
  mouthSmile(64, 50, 14);
}

void emExcited(unsigned long t) {
  int by = -(int)(fabsf(sinf(t / 130.0f)) * 5);
  for (int e = 0; e < 2; e++) {
    int cx = e ? EYE_RX : EYE_LX;
    rrect(cx, CY + by + 2, 34, 34, 9);
    display.fillRoundRect(cx - 9, CY + by - 9, 8, 8, 2, BLACK);
  }
  int sx[4] = { 8, 118, 12, 116 };
  int sy[4] = { 8, 10, 54, 52 };
  for (int i = 0; i < 4; i++) {
    int m = (int)(((t / 120) + i) % 3);
    if (m != 0) drawPlus(sx[i], sy[i], m + 1);
  }
}

void emDizzy(unsigned long t) {
  int dx = (int)(sinf(t / 500.0f) * 3);
  float ph = t / 200.0f;
  drawSpiral(EYE_LX + dx, CY, 14, ph);
  drawSpiral(EYE_RX + dx, CY, 14, -ph);
  for (int i = 0; i < 3; i++) {
    float th = t / 300.0f + i * 2.1f;
    drawPlus(64 + (int)(34 * cosf(th)), 8 + (int)(4 * sinf(th)), 2);
  }
  int px = 52, py = 52;
  for (int x = 52; x <= 76; x += 4) {
    int y = 52 + (int)(sinf((x + t / 40.0f) / 3.0f) * 2);
    display.drawLine(px, py, x, y, WHITE);
    px = x;
    py = y;
  }
}

void emSuspicious(unsigned long t) {
  float bk = blinkK(t, 4200, 180, 1500);
  int dx = (int)(sinf(t / 1200.0f) * 5);
  rrect(EYE_LX + dx, CY + 2, 30, (int)(24 * bk), 7);
  lidFlat(EYE_LX + dx, CY + 2, 30, 24, 10);
  rrect(EYE_RX + dx, CY, 30, (int)(32 * bk), 8);
  display.drawLine(EYE_RX + dx - 14, 11, EYE_RX + dx + 12, 6, WHITE);
  display.drawLine(EYE_RX + dx - 14, 12, EYE_RX + dx + 12, 7, WHITE);
  display.drawLine(54, 52, 74, 52, WHITE);
}

void emLaugh(unsigned long t) {
  int dy = (int)(sinf(t / 50.0f) * 2);
  int cy = 20 + dy;
  for (int k = -1; k <= 1; k++) {
    display.drawLine(EYE_LX - 9, cy - 9 + k, EYE_LX + 6, cy + k, WHITE);
    display.drawLine(EYE_LX + 6, cy + k, EYE_LX - 9, cy + 9 + k, WHITE);
    display.drawLine(EYE_RX + 9, cy - 9 + k, EYE_RX - 6, cy + k, WHITE);
    display.drawLine(EYE_RX - 6, cy + k, EYE_RX + 9, cy + 9 + k, WHITE);
  }
  int r = 11 + (int)(sinf(t / 60.0f) * 2);
  display.fillCircle(64, 42 + dy, r, WHITE);
  display.fillRect(64 - r - 1, 42 + dy - r - 1, 2 * r + 3, r + 1, BLACK);
  for (int i = 0; i < 2; i++) {
    int ph = (int)(((t / 20) + i * 50) % 100);
    drawDrop(EYE_LX - 16 - ph / 7, cy + 2 + ph / 5);
    drawDrop(EYE_RX + 16 + ph / 7, cy + 2 + ph / 5);
  }
}

void emThinking(unsigned long t) {
  float bk = blinkK(t);
  int bob = (int)(sinf(t / 400.0f) * 1.5f);
  rrect(EYE_LX + 7, CY - 7 + bob, 26, (int)(26 * bk), 7);
  rrect(EYE_RX + 7, CY - 7 + bob, 22, (int)(22 * bk), 7);
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(4, 4 + bob);
  display.print("?");
  display.setTextSize(1);
  int n = (int)((t / 350) % 4);
  for (int i = 0; i < n; i++) display.fillCircle(46 + i * 12, 54, 2, WHITE);
}

void emMusic(unsigned long t) {
  int dx = (int)(sinf(t / 260.0f) * 6);
  smileArc(EYE_LX + dx, CY - 3, 11, 7, 5, 3);
  smileArc(EYE_RX + dx, CY - 3, 11, 7, 5, 3);
  mouthSmile(64 + dx, 48, 14);
  int nx[3] = { 8, 20, 112 };
  for (int i = 0; i < 3; i++) {
    int ph = (int)(((t / 30) + i * 33) % 100);
    int y = 58 - ph * 55 / 100;
    int x = nx[i] + (int)(sinf(ph / 8.0f) * 3);
    if (y > 10) drawNote(x, y);
  }
}

void emCool(unsigned long t) {
  int sy = 0;
  if (t < 500) sy = -(int)((500 - t) * 24 / 500);
  display.fillRoundRect(14, 18 + sy, 44, 22, 7, WHITE);
  display.fillRoundRect(70, 18 + sy, 44, 22, 7, WHITE);
  display.fillRect(58, 20 + sy, 12, 3, WHITE);
  display.drawLine(14, 22 + sy, 5, 19 + sy, WHITE);
  display.drawLine(114, 22 + sy, 123, 19 + sy, WHITE);
  display.drawLine(22, 34 + sy, 30, 22 + sy, BLACK);
  display.drawLine(26, 36 + sy, 33, 27 + sy, BLACK);
  display.drawLine(78, 34 + sy, 86, 22 + sy, BLACK);
  display.drawLine(82, 36 + sy, 89, 27 + sy, BLACK);
  display.drawLine(54, 50, 70, 50, WHITE);
  display.drawLine(70, 50, 76, 46, WHITE);
  if (t > 500 && ((t / 300) % 4) == 0) drawPlus(118, 10, 3);
}

void emAnnoyed(unsigned long t) {
  float bk = blinkK(t, 4000, 200, 1200);
  int dx = (sinf(t / 900.0f) > 0.5f) ? 6 : 0;
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + dx;
    rrect(cx, CY + 2, 30, (int)(24 * bk), 7);
    lidSlope(cx, CY + 2, 30, 24, 9, e == 1);
  }
  display.drawLine(54, 52, 74, 52, WHITE);
  display.drawLine(54, 53, 74, 53, WHITE);
}

void emAngry(unsigned long t) {
  int dx = (int)(sinf(t / 30.0f) * 1.0f);
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + dx;
    rrect(cx, CY + 2, 30, 30, 7);
    lidSlope(cx, CY + 2, 30, 30, 16, e == 1);
  }
  if ((t / 250) % 2 == 0) display.drawBitmap(56, 0, bmp_anger, 16, 16, WHITE);
  mouthFrown(64, 50, 16);
}

void emFurious(unsigned long t) {
  int dx = (int)(sinf(t / 15.0f) * 2);
  int dy = (int)(sinf(t / 17.0f) * 1.5f);
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + dx;
    rrect(cx, CY + 2 + dy, 32, 28, 6);
    lidSlope(cx, CY + 2 + dy, 32, 28, 20, e == 1);
  }
  display.drawBitmap(2, 0, bmp_anger, 16, 16, WHITE);
  display.drawBitmap(110, 0, bmp_anger, 16, 16, WHITE);
  for (int s = 0; s < 2; s++) {
    for (int i = 0; i < 3; i++) {
      int ph = (int)(((t / 20) + i * 33) % 100);
      int y = 40 - ph * 34 / 100;
      int r = 1 + ph / 40;
      int x = (s ? 120 : 8) + (int)(sinf(t / 100.0f + i) * 2);
      if (y > 14) display.drawCircle(x, y, r, WHITE);
    }
  }
  display.drawRect(48, 46, 32, 10, WHITE);
  for (int x = 52; x < 80; x += 4) display.drawFastVLine(x, 47, 8, WHITE);
}

void emGrumpy(unsigned long t) {
  float bk = blinkK(t, 3600, 200, 800);
  int dx = (int)(sinf(t / 60.0f) * 0.8f);
  for (int e = 0; e < 2; e++) {
    int cx = (e ? EYE_RX : EYE_LX) + dx;
    rrect(cx, CY + 3, 26, (int)(22 * bk), 6);
    lidFlat(cx, CY + 3, 26, 22, 6);
    lidSlope(cx, CY + 3, 26, 22, 6, e == 1);
  }
  mouthFrown(64, 48, 20);
  if ((t / 400) % 2 == 0) {
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(52, 56);
    display.print("grr..");
  }
}

void drawEmotion(int id, unsigned long t) {
  popK = easeOutBack(t / 240.0f);
  switch (id) {
    case E_HAPPY: emHappy(t); break;
    case E_LOVE: emLove(t); break;
    case E_BLUSH: emBlush(t); break;
    case E_SHY: emShy(t); break;
    case E_COLD: emCold(t); break;
    case E_HOT: emHot(t); break;
    case E_SNOW: emSnow(t); break;
    case E_WINDY: emWindy(t); break;
    case E_SURPRISED: emSurprised(t); break;
    case E_SLEEPY: emSleepy(t); break;
    case E_SAD: emSad(t); break;
    case E_CRYING: emCrying(t); break;
    case E_WINK: emWink(t); break;
    case E_EXCITED: emExcited(t); break;
    case E_DIZZY: emDizzy(t); break;
    case E_SUSPICIOUS: emSuspicious(t); break;
    case E_LAUGH: emLaugh(t); break;
    case E_THINKING: emThinking(t); break;
    case E_MUSIC: emMusic(t); break;
    case E_COOL: emCool(t); break;
    case E_ANNOYED: emAnnoyed(t); break;
    case E_ANGRY: emAngry(t); break;
    case E_FURIOUS: emFurious(t); break;
    case E_GRUMPY: emGrumpy(t); break;
    default: emIdle(t); break;
  }
  popK = 1.0f;
}

// ==================================================
// Dino game
// ==================================================
DObs dobs[3];
float dY = 0, dVy = 0, dSpeed = 3.0f, dScore = 0, dGround = 0, dNext = 60;
bool dDead = false;
unsigned long dLast = 0, dDeadAt = 0;
int dHi = 0, dFrame = 0;

void drawDino(int x, int y, int frame, uint16_t c, uint16_t e) {
  display.fillRect(x + 6, y, 8, 6, c);
  display.fillRect(x + 11, y + 1, 1, 2, e);
  display.fillRect(x + 3, y + 6, 9, 6, c);
  display.fillRect(x + 12, y + 8, 2, 2, c);
  display.fillTriangle(x + 3, y + 6, x + 3, y + 11, x - 3, y + 7, c);
  if (frame == 2) {
    display.fillRect(x + 5, y + 12, 2, 4, c);
    display.fillRect(x + 9, y + 12, 2, 4, c);
  } else if (frame == 0) {
    display.fillRect(x + 5, y + 12, 2, 4, c);
    display.fillRect(x + 9, y + 12, 2, 2, c);
  } else {
    display.fillRect(x + 5, y + 12, 2, 2, c);
    display.fillRect(x + 9, y + 12, 2, 4, c);
  }
}

void dinoReset() {
  dY = 0;
  dVy = 0;
  dSpeed = 3.0f;
  dScore = 0;
  dGround = 0;
  dDead = false;
  dNext = 60;
  dLast = millis();
  for (int i = 0; i < 3; i++) dobs[i].on = false;
}

void dinoJump() {
  if (dDead) {
    if (millis() - dDeadAt > 500) dinoReset();
  } else if (dY < 0.5f) {
    dVy = 6.6f;
  }
}

void spawnObs() {
  for (int i = 0; i < 3; i++) {
    if (dobs[i].on) continue;
    DObs& o = dobs[i];
    int r = (int)random(0, 10);
    o.on = true;
    o.x = 132;
    o.y = 0;
    o.type = 0;
    if (r < 4) { o.w = 7; o.h = 14; }
    else if (r < 7) { o.w = 9; o.h = 20; }
    else if (r < 9) { o.w = 17; o.h = 14; }
    else {
      o.type = 1;
      o.w = 14;
      o.h = 10;
      o.y = random(0, 2) ? 4 : 20;
    }
    return;
  }
}

void dinoStep() {
  dVy -= 0.65f;
  dY += dVy;
  if (dY <= 0) {
    dY = 0;
    dVy = 0;
  }
  dSpeed = 3.0f + dScore / 260.0f;
  if (dSpeed > 6.5f) dSpeed = 6.5f;
  dGround += dSpeed;
  dFrame++;
  for (int i = 0; i < 3; i++) {
    if (!dobs[i].on) continue;
    dobs[i].x -= (dobs[i].type == 1) ? dSpeed * 1.15f : dSpeed;
    if (dobs[i].x + dobs[i].w < 0) dobs[i].on = false;
  }
  dNext -= dSpeed;
  if (dNext <= 0) {
    spawnObs();
    dNext = 70 + (float)random(0, 70) + dSpeed * 14;
  }
  dScore += dSpeed * 0.12f;

  int dx0 = 14, dx1 = 26;
  int dyTop = 56 - 16 - (int)dY + 2;
  int dyBot = 56 - (int)dY;
  for (int i = 0; i < 3; i++) {
    if (!dobs[i].on) continue;
    int ox0 = (int)dobs[i].x + 1, ox1 = (int)dobs[i].x + dobs[i].w - 1;
    int oyTop = 56 - dobs[i].y - dobs[i].h + 1, oyBot = 56 - dobs[i].y;
    if (dx1 > ox0 && dx0 < ox1 && dyBot > oyTop && dyTop < oyBot) {
      dDead = true;
      dDeadAt = millis();
      if ((int)dScore > dHi) {
        dHi = (int)dScore;
        prefs.begin("deskbuddy", false);
        prefs.putInt("dinohi", dHi);
        prefs.end();
      }
      return;
    }
  }
}

void drawObstacle(DObs& o) {
  int x = (int)o.x;
  if (o.type == 1) {
    int y = 56 - o.y - o.h;
    display.fillRect(x + 3, y + 4, 9, 4, WHITE);
    display.fillTriangle(x, y + 5, x + 3, y + 3, x + 3, y + 7, WHITE);
    display.fillRect(x + 11, y + 3, 3, 3, WHITE);
    if ((dFrame / 5) % 2) display.drawLine(x + 5, y + 4, x + 8, y, WHITE);
    else display.drawLine(x + 5, y + 7, x + 8, y + 10, WHITE);
    return;
  }
  int y = 56 - o.h;
  if (o.w >= 16) {
    for (int k = 0; k < 2; k++) {
      int bx = x + k * 9;
      display.fillRect(bx + 2, y, 3, o.h, WHITE);
      display.fillRect(bx, y + 4, 2, 4, WHITE);
      display.fillRect(bx + 5, y + 6, 2, 4, WHITE);
    }
  } else {
    int cx = x + o.w / 2 - 1;
    display.fillRect(cx, y, 3, o.h, WHITE);
    display.fillRect(x, y + o.h / 3, o.w / 2, 2, WHITE);
    display.fillRect(x, y + o.h / 3 - 3, 2, 5, WHITE);
    display.fillRect(x + o.w - 2, y + o.h / 2, 2, 2, WHITE);
    display.fillRect(cx + 3, y + o.h / 2 - 3, o.w / 2 - 1, 2, WHITE);
    display.fillRect(x + o.w - 2, y + o.h / 2 - 3, 2, 5, WHITE);
  }
}

void drawDinoGame() {
  unsigned long now = millis();
  if (!dDead) {
    int loops = 0;
    while (now - dLast >= 30 && loops < 3) {
      dLast += 30;
      dinoStep();
      loops++;
      if (dDead) break;
    }
    if (now - dLast > 200) dLast = now;
  }
  display.drawFastHLine(0, 56, 128, WHITE);
  for (int i = 0; i < 6; i++) {
    int gx = (((i * 29 - (int)dGround) % 128) + 128) % 128;
    display.drawFastHLine(gx, 59 + (i % 2) * 2, 2 + (i % 3) * 2, WHITE);
  }
  int frame = (dY > 0.5f) ? 2 : (dFrame / 4) % 2;
  drawDino(14, 56 - 16 - (int)dY, dDead ? 2 : frame, WHITE, BLACK);
  for (int i = 0; i < 3; i++)
    if (dobs[i].on) drawObstacle(dobs[i]);

  char buf[24];
  snprintf(buf, sizeof(buf), "HI %05d  %05d", dHi, (int)dScore);
  display.setFont(NULL);
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(128 - 14 * 6, 0);
  display.print(buf);
  if (dDead) {
    display.fillRect(22, 18, 84, 26, BLACK);
    display.drawRect(22, 18, 84, 26, WHITE);
    display.setCursor(34, 22);
    display.print("GAME OVER");
    display.setCursor(28, 33);
    display.print("touch = retry");
  }
}

// ==================================================
// App screens
// ==================================================
void centerText(const char* s, int y, const GFXfont* f) {
  int16_t x1, y1;
  uint16_t w, h;
  display.setFont(f);
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - (int)w) / 2, y);
  display.print(s);
}

void drawMenuIcon(int id, int cx, int cy, uint16_t c) {
  uint16_t e = (c == WHITE) ? BLACK : WHITE;
  switch (id) {
    case 0:
      display.drawCircle(cx, cy, 9, c);
      display.drawLine(cx, cy, cx, cy - 6, c);
      display.drawLine(cx, cy, cx + 4, cy + 2, c);
      break;
    case 1:
      drawDino(cx - 6, cy - 8, 0, c, e);
      break;
    case 2:
      display.drawRoundRect(cx - 3, cy - 10, 7, 15, 3, c);
      display.fillCircle(cx, cy + 7, 4, c);
      display.fillRect(cx - 1, cy - 3, 3, 10, c);
      break;
    default:
      display.fillCircle(cx - 3, cy - 3, 3, c);
      for (int i = 0; i < 8; i++) {
        float a = i * PI / 4.0f;
        display.drawLine(cx - 3 + cosf(a) * 5, cy - 3 + sinf(a) * 5, cx - 3 + cosf(a) * 7, cy - 3 + sinf(a) * 7, c);
      }
      display.fillCircle(cx + 2, cy + 4, 3, c);
      display.fillCircle(cx + 6, cy + 5, 2, c);
      display.fillRect(cx + 2, cy + 4, 4, 3, c);
      break;
  }
}

void drawMenu() {
  for (int i = 0; i < 4; i++) {
    int x = 1 + i * 32;
    bool sel = (i == menuSel);
    if (sel) display.fillRoundRect(x, 4, 30, 32, 6, WHITE);
    else display.drawRoundRect(x, 4, 30, 32, 6, WHITE);
    drawMenuIcon(i, x + 15, 20, sel ? BLACK : WHITE);
  }
  display.setTextColor(WHITE);
  centerText(MENU_NAMES[menuSel], 58, &FreeSansBold9pt7b);
  display.setFont(NULL);
  if (bleConnected) display.fillCircle(124, 42, 2, WHITE);
  else display.drawCircle(124, 42, 2, WHITE);
}

void drawTimeScreen() {
  struct tm t;
  display.setTextColor(WHITE);
  if (!getLocal(&t)) {
    centerText("Not synced", 28, &FreeSansBold9pt7b);
    display.setFont(NULL);
    display.setCursor(10, 38);
    display.print("Open the phone page");
    display.setCursor(10, 50);
    display.print("and press Connect");
    return;
  }
  int h12 = t.tm_hour % 12;
  if (h12 == 0) h12 = 12;
  char hh[4], mm[4];
  snprintf(hh, sizeof(hh), "%d", h12);
  snprintf(mm, sizeof(mm), "%02d", t.tm_min);
  display.setFont(&FreeSansBold18pt7b);
  int16_t x1, y1;
  uint16_t wh, hhh, wm, hm;
  display.getTextBounds(hh, 0, 0, &x1, &y1, &wh, &hhh);
  display.getTextBounds(mm, 0, 0, &x1, &y1, &wm, &hm);
  int gap = 12;
  int x = (128 - ((int)wh + gap + (int)wm)) / 2;
  display.setCursor(x, 38);
  display.print(hh);
  if (t.tm_sec % 2 == 0) {
    int cxn = x + (int)wh + gap / 2 - 1;
    display.fillRect(cxn, 19, 3, 3, WHITE);
    display.fillRect(cxn, 29, 3, 3, WHITE);
  }
  display.setCursor(x + (int)wh + gap, 38);
  display.print(mm);
  char date[20];
  strftime(date, sizeof(date), "%a %d %b", &t);
  centerText(date, 61, &FreeSansBold9pt7b);
  display.setFont(NULL);
  display.setCursor(110, 0);
  display.print(t.tm_hour >= 12 ? "PM" : "AM");
}

void drawTempScreen() {
  display.setTextColor(WHITE);
  display.setFont(NULL);
  if (!wxValid) {
    centerText("No data yet", 28, &FreeSansBold9pt7b);
    display.setFont(NULL);
    display.setCursor(10, 40);
    display.print("Connect the phone");
    return;
  }
  int level = constrain(map(wxTemp, -5, 45, 0, 28), 0, 28);
  display.drawRoundRect(10, 4, 9, 38, 4, WHITE);
  display.fillCircle(14, 46, 7, WHITE);
  display.fillRect(13, 42 - level, 3, level + 3, WHITE);

  char buf[16];
  display.setFont(&FreeSansBold18pt7b);
  snprintf(buf, sizeof(buf), "%d", wxTemp);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(34, 32);
  display.print(buf);
  display.fillCircle(34 + (int)w + 5, 12, 3, WHITE);
  display.setFont(NULL);
  display.setTextSize(2);
  display.setCursor(34 + (int)w + 11, 8);
  display.print("C");
  display.setTextSize(1);
  snprintf(buf, sizeof(buf), "Feels %dC", wxFeels);
  display.setCursor(34, 38);
  display.print(buf);
  snprintf(buf, sizeof(buf), "Hi %d  Lo %d", wxHi, wxLo);
  display.setCursor(34, 47);
  display.print(buf);
  snprintf(buf, sizeof(buf), "Humidity %d%%", wxHum);
  display.setCursor(34, 56);
  display.print(buf);
}

void drawWeatherScreen() {
  display.setTextColor(WHITE);
  display.setFont(NULL);
  if (!wxValid) {
    centerText("No data yet", 28, &FreeSansBold9pt7b);
    display.setFont(NULL);
    display.setCursor(10, 40);
    display.print("Connect the phone");
    return;
  }
  drawWx(wxCode, 22, 20, 16);
  display.setCursor(46, 2);
  display.print(wxDesc(wxCode));
  char buf[16];
  snprintf(buf, sizeof(buf), "%dC", wxTemp);
  display.setTextSize(2);
  display.setCursor(46, 14);
  display.print(buf);
  display.setTextSize(1);
  snprintf(buf, sizeof(buf), "Wind %d km/h", wxWind);
  display.setCursor(46, 31);
  display.print(buf);
  display.drawFastHLine(0, 40, 128, WHITE);

  struct tm t;
  int today = 0;
  if (getLocal(&t)) today = t.tm_wday;
  const char* days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  for (int i = 0; i < 3; i++) {
    int cx = 21 + i * 43;
    if (i > 0) display.drawFastVLine(i * 43, 42, 22, WHITE);
    if (!fcValid[i]) continue;
    const char* dn = days[(today + i + 1) % 7];
    display.setCursor(cx - 9, 43);
    display.print(dn);
    drawWx(fcCode[i], cx - 11, 57, 5);
    snprintf(buf, sizeof(buf), "%d", fcTemp[i]);
    display.setCursor(cx - 2, 54);
    display.print(buf);
  }
}

// ==================================================
// Touch
// ==================================================
Ev pollTouch() {
  static bool last = false;
  static unsigned long pressStart = 0, lastTap = 0;
  static bool longDone = false;
  static int taps = 0;
  touchDownEdge = false;
  bool p = digitalRead(TOUCH_PIN);
  unsigned long now = millis();
  Ev ev = EV_NONE;

  if (p && !last) {
    pressStart = now;
    longDone = false;
    touchDownEdge = true;
  } else if (p && last) {
    if (!longDone && now - pressStart > LONG_PRESS_MS) {
      longDone = true;
      taps = 0;
      ev = EV_LONG;
    }
  } else if (!p && last) {
    if (!longDone && now - pressStart < LONG_PRESS_MS) {
      taps++;
      lastTap = now;
    }
  }
  last = p;

  if (ev == EV_NONE && taps > 0 && now - lastTap > DOUBLE_TAP_MS) {
    ev = (taps == 1) ? EV_TAP : EV_DOUBLE;
    taps = 0;
  }
  return ev;
}

void handleEvent(Ev ev) {
  if (ev == EV_NONE && !touchDownEdge) return;
  if (touchDownEdge || ev != EV_NONE) lastInteract = millis();

  if (screen == SCR_DINO) {
    if (touchDownEdge) dinoJump();
    if (ev == EV_LONG) screen = SCR_MENU;
    return;
  }
  if (ev == EV_NONE) return;

  switch (screen) {
    case SCR_FACE:
      if (ev == EV_TAP) nextEmotion();
      else if (ev == EV_DOUBLE) nextAngry();
      else if (ev == EV_LONG) screen = SCR_MENU;
      break;
    case SCR_MENU:
      if (ev == EV_TAP) menuSel = (menuSel + 1) % 4;
      else if (ev == EV_DOUBLE) screen = SCR_FACE;
      else if (ev == EV_LONG) {
        if (menuSel == 0) screen = SCR_TIME;
        else if (menuSel == 1) {
          dinoReset();
          screen = SCR_DINO;
        } else if (menuSel == 2) screen = SCR_TEMP;
        else screen = SCR_WEATHER;
      }
      break;
    default:  // time, temp, weather
      if (ev == EV_DOUBLE) screen = SCR_FACE;
      else screen = SCR_MENU;
      break;
  }
}

// ==================================================
// BLE (Nordic UART service, write only)
// ==================================================
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) { bleConnected = true; }
  void onDisconnect(BLEServer*) {
    bleConnected = false;
    needAdv = true;
    advAt = millis() + 400;
  }
};

class RxCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    auto v = c->getValue();
    String s = String(v.c_str());
    if (!hasCmd) {
      pendingCmd = s;
      hasCmd = true;
    }
  }
};

void bleInit() {
  BLEDevice::init("DeskBuddy");
  BLEServer* srv = BLEDevice::createServer();
  srv->setCallbacks(new ServerCb());
  BLEService* svc = srv->createService(NUS_SERVICE);
  BLECharacteristic* rx = svc->createCharacteristic(NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());
  BLECharacteristic* tx = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  tx->addDescriptor(new BLE2902());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// Commands (each <= 20 bytes so they fit one BLE packet):
//   T,<epoch>,<tzMinutes>        time
//   W,<temp>,<feels>,<hum>,<code>,<wind>
//   R,<hi>,<lo>
//   D,<1..3>,<code>,<tempMax>    forecast day
//   E,<0..24>                    show emotion
void handleCommand(String s) {
  s.trim();
  if (s.length() < 2) return;
  const char* c = s.c_str();
  if (c[0] == 'T') {
    long ep = 0;
    int tz = 0;
    if (sscanf(c, "T,%ld,%d", &ep, &tz) == 2 && ep > 1600000000L) {
      syncEpoch = ep;
      syncMillis = millis();
      tzMin = tz;
      timeSynced = true;
    }
  } else if (c[0] == 'W') {
    int t, f, h, code, w;
    if (sscanf(c, "W,%d,%d,%d,%d,%d", &t, &f, &h, &code, &w) == 5) {
      wxTemp = t;
      wxFeels = f;
      wxHum = h;
      wxCode = code;
      wxWind = w;
      wxValid = true;
      wxAt = millis();
      emotionFromWeather();
    }
  } else if (c[0] == 'R') {
    int hi, lo;
    if (sscanf(c, "R,%d,%d", &hi, &lo) == 2) {
      wxHi = hi;
      wxLo = lo;
    }
  } else if (c[0] == 'D') {
    int i, code, t;
    if (sscanf(c, "D,%d,%d,%d", &i, &code, &t) == 3 && i >= 1 && i <= 3) {
      fcCode[i - 1] = code;
      fcTemp[i - 1] = t;
      fcValid[i - 1] = true;
    }
  } else if (c[0] == 'E') {
    int n;
    if (sscanf(c, "E,%d", &n) == 1 && n >= 0 && n <= E_IDLE) {
      screen = SCR_FACE;
      setEmotion(n, true);
    }
  }
}

// ==================================================
// Setup & loop
// ==================================================
void setup() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  pinMode(TOUCH_PIN, INPUT);
  display.begin(0x3C, true);
  display.setTextColor(WHITE);

  prefs.begin("deskbuddy", true);
  dHi = prefs.getInt("dinohi", 0);
  prefs.end();

  display.clearDisplay();
  centerText("DeskBuddy", 36, &FreeSansBold9pt7b);
  display.display();
  bleInit();
  delay(900);

  setEmotion(E_IDLE, true);
}

void loop() {
  unsigned long now = millis();
  Ev ev = pollTouch();
  handleEvent(ev);

  if (hasCmd) {
    String c = pendingCmd;
    hasCmd = false;
    handleCommand(c);
  }

  if (needAdv && now > advAt) {
    needAdv = false;
    BLEDevice::startAdvertising();
  }

  // keep the clock safe from millis() wrap
  static unsigned long lastRebase = 0;
  if (timeSynced && now - lastRebase > 3600000UL) {
    lastRebase = now;
    syncEpoch = nowEpoch();
    syncMillis = now;
  }

  // face returns to idle after a while; apps return to the face
  if (screen == SCR_FACE && curEm != E_IDLE && now - emStart > EM_HOLD_MS && now - lastInteract > EM_HOLD_MS) {
    setEmotion(E_IDLE, false);
  }
  if ((screen == SCR_MENU || screen == SCR_TIME || screen == SCR_TEMP || screen == SCR_WEATHER) && now - lastInteract > 45000UL) {
    screen = SCR_FACE;
  }

  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  display.clearDisplay();
  display.setFont(NULL);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  switch (screen) {
    case SCR_FACE:
      drawEmotion(curEm, now - emStart);
      if (bleConnected) display.fillRect(123, 1, 3, 3, WHITE);
      break;
    case SCR_MENU: drawMenu(); break;
    case SCR_TIME: drawTimeScreen(); break;
    case SCR_DINO: drawDinoGame(); break;
    case SCR_TEMP: drawTempScreen(); break;
    case SCR_WEATHER: drawWeatherScreen(); break;
  }
  display.display();
}

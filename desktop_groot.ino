/*
 * desktop_groot  -  ESP32-C3 Super Mini + 1.3" SH1106 OLED desk buddy
 *
 * What it does
 *  - Boot / Bluetooth connect : shows "Hi ovs" with a bounce-in animation
 *  - Text from phone (BLE)    : pops up animated on the OLED
 *                               (short text = bounce-in, long text = scrolling)
 *  - Automatic emotions       : eyes look around, blink, yawn, get confused,
 *                               act like a baby, get sleepy, happy, love
 *  - Touch sensor             : 1 tap = cute, 2 taps = angry, long press = hook for menu
 *
 * Libraries (Arduino Library Manager): "U8g2" by olikraus
 * Board package: "esp32" by Espressif  ->  board "ESP32C3 Dev Module"
 *
 * Phone side: use nRF Connect (or any BLE UART app that writes to the
 * Nordic UART RX characteristic). Connect to "DeskBuddy" and write text to
 * characteristic 6E400002-B5A3-F393-E0A9-E50E24DCCA9E.
 * Plain text   -> shown on the OLED
 * Commands     -> /yawn /confused /baby /happy /love /angry /sleepy /hi
 *
 * NOTE: This is BLE, so it will NOT show in the phone's normal Bluetooth
 * settings list. It only appears inside a BLE scanner app.
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <math.h>

// ---------------- CONFIG (change to match your wiring) ----------------
#define PIN_SDA            8        // ESP32-C3 Super Mini default SDA
#define PIN_SCL            9        // ESP32-C3 Super Mini default SCL
#define PIN_TOUCH          4        // touch module OUT pin  <-- change if different
#define TOUCH_ACTIVE_LEVEL HIGH     // TTP223 gives HIGH when touched
#define BLE_NAME           "DeskBuddy"
#define GREETING           "Hi ovs"
#define MAX_MSG_LEN        100
#define SCROLL_PPS         70       // scroll speed, pixels per second
#define LONG_PRESS_MS      800
#define DOUBLE_TAP_MS      350
#define FRAME_MS           30

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

#define MSG_FONT u8g2_font_helvB14_tr

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ---------------- STATE ----------------
enum Mode { MODE_MSG, MODE_FACE };
enum Emo  { E_IDLE, E_YAWN, E_CONFUSED, E_BABY, E_HAPPY, E_LOVE, E_ANGRY, E_SLEEPY };

Mode mode = MODE_FACE;
Emo  emo  = E_IDLE;
unsigned long emoStart = 0, emoDur = 0, nextEmoAt = 0;
unsigned long blinkStart = 0, nextBlinkAt = 0;
bool blinking = false;
float lookX = 0, lookY = 0, tgtX = 0, tgtY = 0;
unsigned long nextLookAt = 0;

String msg;
unsigned long msgStart = 0, msgDur = 0;
int  msgW = 0;
bool msgScroll = false;

volatile bool newMsg = false;
volatile bool greetPending = false;
String pendingMsg;
unsigned long lastFrame = 0;

// ---------------- BLE ----------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* server) {
    greetPending = true;
    // keep advertising so a second phone can still find the device
    if (server->getConnectedCount() < 2) BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer* server) {
    BLEDevice::startAdvertising();   // important: advertise again after disconnect
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    String v = String(c->getValue().c_str());   // works on core 2.x and 3.x
    pendingMsg = v;
    newMsg = true;
  }
};

void startBLE() {
  BLEDevice::init(BLE_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService* svc = server->createService(SERVICE_UUID);
  BLECharacteristic* rx = svc->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ---------------- HELPERS ----------------
unsigned long defaultDur(int e) {
  switch (e) {
    case E_YAWN:     return 3200;
    case E_CONFUSED: return 3200;
    case E_BABY:     return 4200;
    case E_HAPPY:    return 2500;
    case E_LOVE:     return 2800;
    case E_ANGRY:    return 2600;
    case E_SLEEPY:   return 4200;
  }
  return 2000;
}

void setEmotion(int e, unsigned long dur) {
  emo = (Emo)e;
  emoStart = millis();
  emoDur = dur;
  nextEmoAt = millis() + dur + random(4000, 9000);
}

void eyeBox(int cx, int cy, int w, int h, int r) {
  if (h < 2) h = 2;
  int rr = min(r, min(w, h) / 2);
  u8g2.drawRBox(cx - w / 2, cy - h / 2, w, h, rr);
}

void blush(int x, int y) {
  for (int i = 0; i < 3; i++) u8g2.drawLine(x + i * 5, y + 4, x + 3 + i * 5, y);
}

void sparkle(int x, int y, int s) {
  u8g2.drawPixel(x, y);
  if (s > 0) {
    u8g2.drawLine(x - s, y, x + s, y);
    u8g2.drawLine(x, y - s, x, y + s);
  }
}

void heart(int cx, int cy, float s) {
  int r = (int)(7 * s);
  u8g2.drawDisc(cx - (int)(0.8 * r), cy - r / 2, r);
  u8g2.drawDisc(cx + (int)(0.8 * r), cy - r / 2, r);
  u8g2.drawTriangle(cx - (int)(1.75 * r), cy - r / 4,
                    cx + (int)(1.75 * r), cy - r / 4,
                    cx, cy + (int)(1.9 * r));
}

float blinkScale(unsigned long now) {
  if (!blinking) return 1.0;
  float p = (now - blinkStart) / 220.0;
  if (p > 1) p = 1;
  return 1.0 - 0.92 * sin(PI * p);
}

// ---------------- BEHAVIOUR (eyes move, blink, random emotions) ----------------
void updateBehavior(unsigned long now) {
  if (now >= nextLookAt) {
    if (random(0, 6) == 0) { tgtX = random(0, 2) ? -12 : 12; tgtY = random(-4, 5); }
    else                   { tgtX = random(-7, 8);           tgtY = random(-4, 5); }
    nextLookAt = now + random(800, 2600);
  }
  lookX += (tgtX - lookX) * 0.3;
  lookY += (tgtY - lookY) * 0.3;

  if (!blinking && now >= nextBlinkAt) { blinking = true; blinkStart = now; }
  if (blinking && now - blinkStart >= 220) {
    blinking = false;
    nextBlinkAt = now + random(1800, 5000);
    if (random(0, 5) == 0) nextBlinkAt = now + 260;   // occasional double blink
  }

  if (emo != E_IDLE && now - emoStart >= emoDur) emo = E_IDLE;

  if (emo == E_IDLE && now >= nextEmoAt) {
    static const Emo pool[] = { E_YAWN, E_CONFUSED, E_BABY, E_BABY, E_HAPPY, E_LOVE, E_SLEEPY };
    Emo e = pool[random(0, 7)];
    setEmotion(e, defaultDur(e));
  }
}

// ---------------- FACE DRAWING ----------------
void drawFace(unsigned long now) {
  u8g2.setDrawColor(1);
  float t = (float)(now - emoStart);
  float p = emoDur ? t / (float)emoDur : 0;
  if (p > 1) p = 1;
  int lx = (int)lookX, ly = (int)lookY;
  float bs = blinkScale(now);

  switch (emo) {

    case E_HAPPY: {
      int by = -(int)(fabs(sin(t / 200.0)) * 3);
      for (int r = 11; r <= 14; r++) {
        u8g2.drawCircle(34, 38 + by, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
        u8g2.drawCircle(94, 38 + by, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      }
      u8g2.drawCircle(64, 48 + by, 7, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      u8g2.drawCircle(64, 48 + by, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      blush(10, 46 + by);
      blush(103, 46 + by);
      break;
    }

    case E_LOVE: {
      float s = 1.0 + 0.15 * sin(t / 110.0);
      heart(34, 28, s);
      heart(94, 28, s);
      u8g2.drawCircle(64, 50, 6, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      break;
    }

    case E_ANGRY: {
      int sx = (int)(sin(t / 25.0) * 1.5);
      eyeBox(34 + sx, 32, 32, 28, 6);
      eyeBox(94 + sx, 32, 32, 28, 6);
      u8g2.setDrawColor(0);
      u8g2.drawTriangle(34 + sx - 17, 17, 34 + sx + 17, 17, 34 + sx + 17, 34);
      u8g2.drawTriangle(94 + sx - 17, 34, 94 + sx - 17, 17, 94 + sx + 17, 17);
      u8g2.setDrawColor(1);
      u8g2.drawCircle(64 + sx, 60, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
      break;
    }

    case E_YAWN: {
      float a;
      if (p < 0.25)      a = p / 0.25;
      else if (p < 0.75) a = 1;
      else               a = (1 - p) / 0.25;
      int h = (int)(34 * (1 - 0.9 * a));
      int cy = 30 + (int)(a * 4);
      eyeBox(34, cy, 30, h, 9);
      eyeBox(94, cy, 30, h, 9);
      if (a > 0.05) {
        int rx = 3 + (int)(9 * a), ry = 2 + (int)(9 * a);
        u8g2.drawEllipse(64, 51, rx, ry);
        u8g2.drawEllipse(64, 51, rx - 1, ry - 1);
      }
      break;
    }

    case E_SLEEPY: {
      float a;
      if (p < 0.75)      a = p / 0.75;
      else if (p < 0.82) a = 1;
      else               a = fmax(0.0, 1.0 - (p - 0.82) / 0.06);
      int h = (int)(34 - 26 * a);
      if (p > 0.88) h = 38;                       // startled wake-up
      int nod = (int)(sin(t / 300.0) * 2 * a);
      int cy = 47 - h / 2 + nod;
      eyeBox(34, cy, 30, h, 9);
      eyeBox(94, cy, 30, h, 9);
      if (a > 0.4 && p < 0.8) {
        float zp = fmod(t, 1400.0) / 1400.0;
        u8g2.setFont(u8g2_font_6x10_tr);
        u8g2.drawStr(104 + (int)(zp * 10), 24 - (int)(zp * 14) + 10, "z");
        float zp2 = fmod(t + 700.0, 1400.0) / 1400.0;
        u8g2.drawStr(112 + (int)(zp2 * 8), 24 - (int)(zp2 * 14) + 10, "Z");
      }
      break;
    }

    case E_CONFUSED: {
      int jx = (int)(sin(t / 140.0) * 5);
      eyeBox(34 + jx, 30, 30, (int)(36 * bs), 9);
      eyeBox(94 + jx, 36, 22, (int)(18 * bs), 7);
      u8g2.drawLine(82, 19, 106, 15);                       // raised brow
      u8g2.setFont(u8g2_font_helvB14_tr);
      u8g2.drawStr(114, 14 + (int)(sin(t / 120.0) * 2), "?");
      for (int i = 0; i < 5; i++) {                          // wobbly mouth
        int y1 = (i % 2) ? 55 : 51;
        int y2 = (i % 2) ? 51 : 55;
        u8g2.drawLine(47 + i * 7, y1, 54 + i * 7, y2);
      }
      break;
    }

    case E_BABY: {
      int by = -(int)(fabs(sin(t / 260.0)) * 3);
      int ex = lx / 2, ey = ly / 2;
      int h = (int)(38 * bs);
      int cyE = 27 + by + ey;
      eyeBox(32 + ex, cyE, 38, h, 14);
      eyeBox(96 + ex, cyE, 38, h, 14);
      if (bs > 0.5) {
        u8g2.setDrawColor(0);
        u8g2.drawDisc(32 + ex - 6, cyE - 7, 5);
        u8g2.drawDisc(96 + ex - 6, cyE - 7, 5);
        u8g2.drawDisc(32 + ex + 7, cyE + 7, 2);
        u8g2.drawDisc(96 + ex + 7, cyE + 7, 2);
        u8g2.setDrawColor(1);
      }
      blush(10, 50 + by);
      blush(103, 50 + by);
      if (((int)(t / 700)) % 2) u8g2.drawCircle(64, 55 + by, 3);
      else u8g2.drawCircle(64, 52 + by, 4, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
      if (((int)(t / 300)) % 2) { sparkle(6, 8, 2); sparkle(122, 8, 2); }
      break;
    }

    case E_IDLE:
    default: {
      int h = (int)((34 + sin(now / 700.0) * 1.5) * bs);
      eyeBox(34 + lx, 30 + ly, 30, h, 9);
      eyeBox(94 + lx, 30 + ly, 30, h, 9);
      break;
    }
  }
}

// ---------------- MESSAGE DISPLAY ----------------
void startMessage(const String& s) {
  msg = s;
  msgStart = millis();
  mode = MODE_MSG;
  u8g2.setFont(MSG_FONT);
  msgW = u8g2.getStrWidth(msg.c_str());
  msgScroll = (msgW > 120);
  if (msgScroll) {
    int travel = 128 + msgW;
    int passes = (msgW > 500) ? 1 : 2;
    msgDur = (unsigned long)passes * travel * 1000UL / SCROLL_PPS;
  } else {
    msgDur = 4500;
  }
}

void drawMessage(unsigned long now) {
  unsigned long t = now - msgStart;
  u8g2.setDrawColor(1);
  u8g2.setFont(MSG_FONT);

  if (msgScroll) {
    // draw char by char so text can slide off the left edge cleanly
    int travel = 128 + msgW;
    int x = 128 - (int)((t * SCROLL_PPS / 1000) % travel);
    char one[2] = {0, 0};
    for (unsigned int i = 0; i < msg.length(); i++) {
      one[0] = msg[i];
      int gw = u8g2.getStrWidth(one);
      if (x >= 0 && x < 128) u8g2.drawStr(x, 38 + (int)(sin((t + i * 90) / 150.0) * 3), one);
      x += gw;
    }
  } else {
    int off = 0;
    if (t < 900) off = (int)(-50.0 * exp(-(float)t / 160.0) * cos((float)t / 70.0));
    int x = (128 - msgW) / 2;
    u8g2.drawStr(x, 38 + off, msg.c_str());
    int ul = (int)(t / 6);
    if (ul > msgW) ul = msgW;
    u8g2.drawHLine(x, 46, ul);
    sparkle(10, 12, (int)((t / 250) % 3));
    sparkle(118, 14, (int)(((t + 300) / 250) % 3));
    sparkle(14, 54, (int)(((t + 600) / 250) % 3));
    sparkle(114, 52, (int)(((t + 900) / 250) % 3));
  }
}

// ---------------- INCOMING TEXT / COMMANDS ----------------
bool runCommand(String c) {
  c.toLowerCase();
  mode = MODE_FACE;
  if (c == "yawn")     { setEmotion(E_YAWN, defaultDur(E_YAWN)); return true; }
  if (c == "confused") { setEmotion(E_CONFUSED, defaultDur(E_CONFUSED)); return true; }
  if (c == "baby")     { setEmotion(E_BABY, defaultDur(E_BABY)); return true; }
  if (c == "happy")    { setEmotion(E_HAPPY, defaultDur(E_HAPPY)); return true; }
  if (c == "love")     { setEmotion(E_LOVE, defaultDur(E_LOVE)); return true; }
  if (c == "angry")    { setEmotion(E_ANGRY, defaultDur(E_ANGRY)); return true; }
  if (c == "sleepy")   { setEmotion(E_SLEEPY, defaultDur(E_SLEEPY)); return true; }
  if (c == "hi")       { startMessage(GREETING); return true; }
  return false;
}

void handleIncoming(String s) {
  s.trim();
  if (s.length() == 0) return;
  if (s.length() > MAX_MSG_LEN) s = s.substring(0, MAX_MSG_LEN);
  if (s[0] == '/' && runCommand(s.substring(1))) return;
  startMessage(s);
}

// ---------------- TOUCH ----------------
void onSingleTap() {
  mode = MODE_FACE;
  static const Emo cute[] = { E_HAPPY, E_LOVE, E_BABY };
  setEmotion(cute[random(0, 3)], 3000);
}

void onDoubleTap() {
  mode = MODE_FACE;
  setEmotion(E_ANGRY, defaultDur(E_ANGRY));
}

void onLongPress() {
  // TODO: open your app interface / menu here
  startMessage("Menu soon");
}

void handleTouch(unsigned long now) {
  static bool state = false, longFired = false;
  static unsigned long pressStart = 0, lastRelease = 0, lastChange = 0;
  static uint8_t taps = 0;

  bool raw = (digitalRead(PIN_TOUCH) == TOUCH_ACTIVE_LEVEL);
  if (raw != state && now - lastChange > 30) {
    lastChange = now;
    state = raw;
    if (state) { pressStart = now; longFired = false; }
    else if (!longFired && now - pressStart < LONG_PRESS_MS) { taps++; lastRelease = now; }
  }
  if (state && !longFired && now - pressStart >= LONG_PRESS_MS) {
    longFired = true;
    taps = 0;
    onLongPress();
  }
  if (taps > 0 && !state && now - lastRelease > DOUBLE_TAP_MS) {
    if (taps == 1) onSingleTap(); else onDoubleTap();
    taps = 0;
  }
}

// ---------------- SETUP / LOOP ----------------
void setup() {
  randomSeed(esp_random());
  pinMode(PIN_TOUCH, INPUT);
  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);
  startBLE();

  unsigned long now = millis();
  nextBlinkAt = now + 2000;
  nextEmoAt   = now + 9000;
  nextLookAt  = now;
  startMessage(GREETING);          // "Hi ovs" at boot
}

void loop() {
  unsigned long now = millis();

  handleTouch(now);

  if (greetPending) { greetPending = false; startMessage(GREETING); }   // "Hi ovs" on BLE connect
  if (newMsg) { newMsg = false; handleIncoming(pendingMsg); }

  if (mode == MODE_MSG && now - msgStart >= msgDur) {
    mode = MODE_FACE;
    emo = E_IDLE;
    nextEmoAt = now + random(3000, 6000);
  }

  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    if (mode == MODE_FACE) updateBehavior(now);
    u8g2.clearBuffer();
    if (mode == MODE_MSG) drawMessage(now);
    else drawFace(now);
    u8g2.sendBuffer();
  }
}

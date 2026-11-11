// ===========================================================
//  pet_eye.ino  -  pixel eyes for ESP32
//
//  Hardware: ESP32-S3 + ST7735S 1.8" (128x160, 8-pin SPI)
//  Wiring: SCK=12  MOSI=11  CS=10  DC=9  RES=8  BLK=7
//  Memory: one frame is 160x128x2 = 40KB, all of it in internal RAM
//
//  Serial (115200) commands:
//    m <name|index>  switch expression   e.g. m love / m 11
//    d <0-8>         gaze   0 centre 1 up 2 up-right 3 right 4 down-right 5 down 6 down-left 7 left 8 up-left
//    b <0|1>         auto blink on/off
//    h <0-360>       hue     default 146 (teal)
//    l <0.4-1.3>     brightness
//    i               print current state
//    ?               help
// ===========================================================
#include <Esp.h>
#include "pet_face.h"

// ---------------- pins ----------------
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_CS    10
#define PIN_DC     9
#define PIN_RST    8
#define PIN_BLK    7

// 40fps is stable; 60 works but saturates the SPI bus
#define TARGET_FPS 40

TFT_eSPI tft;
TFT_eSprite spr(&tft);

static uint32_t lastFrameMs = 0;
static uint32_t fpsCount = 0, fpsLast = 0;

// ---------------- serial ----------------
static void printHelp() {
  Serial.println(F("---- pet_eye ----"));
  Serial.println(F("m <name|idx>  switch expression"));
  Serial.println(F("d <0-8>       gaze direction"));
  Serial.println(F("b <0|1>       auto blink"));
  Serial.println(F("h <0-360>     hue"));
  Serial.println(F("l <0.4-1.3>   brightness"));
  Serial.println(F("i             state"));
  for (int i = 0; i < MOOD_COUNT; i++) {
    Serial.printf("  %2d %s\n", i, MOOD_NAMES[i]);
  }
}

static int findMood(const String& s) {
  for (int i = 0; i < MOOD_COUNT; i++)
    if (s.equalsIgnoreCase(MOOD_NAMES[i])) return i;
  int n = s.toInt();
  if (n >= 0 && n < MOOD_COUNT) return n;
  return -1;
}

static void handleSerial() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf.trim();
      if (buf.length()) {
        char cmd = buf[0];
        String arg = buf.substring(1); arg.trim();
        switch (cmd) {
          case 'm': case 'M': {
            int id = findMood(arg);
            if (id >= 0) { faceSetMood(id); Serial.printf("mood -> %s\n", MOOD_NAMES[id]); }
            else Serial.println(F("unknown mood"));
            break;
          }
          case 'd': case 'D': {
            int d = arg.toInt();
            faceSetDir(constrain(d, 0, 8));
            Serial.printf("dir -> %d\n", d);
            break;
          }
          case 'b': case 'B':
            faceCfg.blink = (arg.toInt() != 0);
            Serial.printf("blink %s\n", faceCfg.blink ? "on" : "off");
            break;
          case 'h': case 'H':
            faceCfg.hue = arg.toFloat();
            faceSetHue(faceCfg.hue);
            Serial.printf("hue %d\n", (int)faceCfg.hue);
            break;
          case 'l': case 'L':
            faceCfg.lum = constrain(arg.toFloat(), 0.3f, 1.4f);
            Serial.printf("lum %.2f\n", faceCfg.lum);
            break;
          case 'i': case 'I':
            Serial.printf("mood=%s dir=(%.1f,%.1f) fps=%lu heap=%u\n",
                          MOOD_NAMES[g_moodId], g_dirTgt[0], g_dirTgt[1],
                          (unsigned long)fpsCount, (unsigned)ESP.getFreeHeap());
            break;
          case '?': printHelp(); break;
          default: Serial.println(F("? for help")); break;
        }
      }
      buf = "";
    } else if (buf.length() < 40) {
      buf += c;
    }
  }
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println(F("\npet_eye boot"));

  pinMode(PIN_BLK, OUTPUT);
  digitalWrite(PIN_BLK, HIGH);        // backlight on early, so init is not a black screen

  tft.init();
  tft.setRotation(1);                 // 1 or 3: swap if the image comes out rotated
  tft.fillScreen(TFT_BLACK);

  // 40KB per frame; failure means not enough RAM (usually claimed by BLE/WiFi)
  spr.setColorDepth(16);
  if (!spr.createSprite(SCR_W, SCR_H)) {
    Serial.println(F("!! sprite alloc failed"));
    while (true) delay(1000);
  }
  spr.setSwapBytes(true);
  spr.fillSprite(TFT_BLACK);
  g_spr = &spr;

  faceSetHue(faceCfg.hue);
  copyMood(g_cur, MOOD_TABLE[M_NORMAL]);
  copyMood(g_tgt, MOOD_TABLE[M_NORMAL]);
  g_moodT = 1;
  facePrimeHearts();          // pre-seed hearts so "love" is not empty for two seconds

  printHelp();
  Serial.printf("sprite ok, free heap = %u\n", (unsigned)ESP.getFreeHeap());

  // The first frame must not start from dt=0: Serial wait + tft.init + 40KB alloc have
  // already taken hundreds of ms, which advances a dozen frame numbers at once - a phantom blink.
  lastFrameMs = millis();
}

// ---------------- loop ----------------
void loop() {
  handleSerial();

  uint32_t now = millis();
  uint32_t dt = now - lastFrameMs;
  if (dt < (1000 / TARGET_FPS)) return;
  lastFrameMs = now;

  g_faceStepMs = (float)dt;
  g_faceT += dt * 60.0f / 1000.0f;    // virtual 60fps frame number; all timing hangs off it

  faceRender();
  spr.pushSprite(0, 0);

  fpsCount++;
  if (now - fpsLast >= 1000) {
    fpsLast = now;
    // print once a second to spot dropped frames on the bench
    // Serial.printf("fps=%lu\n", (unsigned long)fpsCount);
    fpsCount = 0;
  }
}

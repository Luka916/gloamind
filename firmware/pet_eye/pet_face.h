// ===========================================================
//  pet_face.h  -  parameterised pixel eyes, ESP32 port
//  Mirrors the browser build line for line. Not redrawn, translated.
//
//  Depends on TFT_eSPI (User_Setup.h in docs/pinout.md)
//  Panel: ST7735S 128x160, run landscape at 160x128
// ===========================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <esp_system.h>
#include "moods.h"

// ---------------- Canvas ----------------
#define SCR_W 160
#define SCR_H 128
#define PX    4              // one cell = 4px, same as the web build
#define EYE_CL 14.0f         // left eye centre (cells)
#define EYE_CR 26.0f
#define EYE_CY 16.0f
#define MAX_HEART 12

// ---------------- Config ----------------
struct FaceConfig {
  bool  blink   = true;
  bool  breath  = true;
  bool  heart   = true;
  bool  zzz     = true;
  bool  sweat   = true;
  bool  qmark   = true;
  bool  dots    = true;
  bool  swapEye = true;      // think: eye size swap
  float hue     = 146.0f;
  float lum     = 1.0f;
};
static FaceConfig faceCfg;

// ---------------- Palette ----------------
// Hook a colour picker in later: edit the numbers here, render code stays untouched.
// H/S/L are 0~1 (H is 0~360); R/G/B also 0~1, quantised by rgb565() at the end.
struct Palette {
  float eyeH,  eyeS,  eyeL;                 // eye body
  float glowH, glowS, glowL;                // highlight on the top eyelid edge
  float iconH, iconS;                       // question mark / dots / Zzz (brightness per icon)
  float tearR,  tearG,  tearB;              // cry: tears
  float sweatR, sweatG, sweatB;             // speechless: sweat drop
  float blushR, blushG, blushB;             // shy: blush
  uint16_t heart[6];                        // six heart tints
};
static Palette theme = {
  146.0f, 1.00f, 0.56f,                     // eye
  146.0f, 1.00f, 0.92f,                     // glow
  146.0f, 1.00f,                            // icon
  190/255.f, 245/255.f, 1.000f,             // tear
  120/255.f, 205/255.f, 245/255.f,          // sweat
  1.000f, 0.463f, 0.643f,                   // blush
  {0xFBF4, 0xFA6F, 0xFD99, 0xFEDC, 0xFC76, 0x5FF2}
};

// ---------------- Global state ----------------
static float g_faceT = 0;                 // virtual 60fps frame counter
static float g_faceStepMs = 25.0f;        // real interval of this frame (ms), written by loop()

struct LiveMood {
  EyeShape L, R;
  float dx, dy, lx, rx, ldy, rdy;
  Decor decor;
};

static LiveMood g_cur, g_tgt;
static float g_moodT = 1.0f, g_moodDur = 260.0f;
static int   g_moodId = M_NORMAL;
static float g_dirCur[2] = {0, 0}, g_dirTgt[2] = {0, 0};
static float g_dirT = 1.0f, g_dirDur = 260.0f;
static float g_blinkAmt = 0, g_blinkPhase = -1, g_nextBlink = 200;
static int   g_doubleBlink = 0;
static float g_qEnter = -1, g_thinkEnter = -1;
static float g_peekOff = 0, g_peekEnter = -1;   // peek offset: actual value + entry frame
static TFT_eSprite* g_spr = nullptr;

struct DirDef { float x, y; };
static const DirDef DIRS[9] = {
  {0, 0}, {0, -3.2f}, {2.6f, -2.6f}, {3.2f, 0}, {2.6f, 2.6f},
  {0, 3.2f}, {-2.6f, 2.6f}, {-3.2f, 0}, {-2.6f, -2.6f}
};

// ---------------- Small helpers ----------------
static inline float easeC(float k) {
  if (k <= 0) return 0;
  if (k >= 1) return 1;
  return k < 0.5f ? 4 * k * k * k : 1 - powf(-2 * k + 2, 3) / 2;
}
static inline void faceSetHue(float h) { faceCfg.hue = h; theme.eyeH = h; theme.glowH = h; theme.iconH = h; }
static inline float rnd01() { return (esp_random() & 0xFFFF) / 65535.0f; }
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static inline uint16_t rgb565(float r, float g, float b) {
  if (r < 0) r = 0;
  if (r > 1) r = 1;
  if (g < 0) g = 0;
  if (g > 1) g = 1;
  if (b < 0) b = 0;
  if (b > 1) b = 1;
  return ((uint16_t)(r * 31 + 0.5f) << 11) |
         ((uint16_t)(g * 63 + 0.5f) << 5)  |
         (uint16_t)(b * 31 + 0.5f);
}

// HSL -> RGB. The web build is hsla(h,100%,L%,a) everywhere, so HSL is mandatory here:
// multiplying RGB directly crushes the green (at L=56% the max channel is already 1.0).
static void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
  h = fmodf(h, 360.0f);
  if (h < 0) h += 360.0f;
  if (l < 0) l = 0;
  if (l > 1) l = 1;
  if (s < 0) s = 0;
  if (s > 1) s = 1;
  float C = (1.0f - fabsf(2.0f * l - 1.0f)) * s;
  float X = C * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = l - C * 0.5f;
  switch ((int)(h / 60.0f) % 6) {
    case 0: r = C; g = X; b = 0; break;
    case 1: r = X; g = C; b = 0; break;
    case 2: r = 0; g = C; b = X; break;
    case 3: r = 0; g = X; b = C; break;
    case 4: r = X; g = 0; b = C; break;
    default:r = C; g = 0; b = X; break;
  }
  r += m; g += m; b += m;
}

// hsla(h,100%,L%,a) over black
static inline uint16_t hslTone(float h, float s, float l, float a) {
  float r, g, b;
  hslToRgb(h, s, l, r, g, b);
  return rgb565(r * a, g * a, b * a);
}

// Eye body: lightness locked to 56% * lum, only alpha follows bright * edgeK
// (web col(a) is hsla(h,100%,56*lum%,a) - l and alpha are independent parameters)
static inline uint16_t eyeTone(float a0)  { return hslTone(theme.eyeH,  theme.eyeS,  theme.eyeL  * faceCfg.lum, a0); }
static inline uint16_t glowTone(float br) { return hslTone(theme.glowH, theme.glowS, min(0.97f, theme.glowL * br), 0.97f); }
static inline uint16_t iconTone(float l, float a) { return hslTone(theme.iconH, theme.iconS, l, a); }

static uint16_t rainbowTone(int col, int row) {
  float h = g_faceT * 4.0f + col * 22.0f + row * 9.0f + 600.0f;
  float r, g, b;
  hslToRgb(h, 1.0f, 0.62f, r, g, b);      // l = min(74, 62*bright)%, alpha is always 1
  return rgb565(r, g, b);
}

// ---------------- Mood switching ----------------
static void copyShape(EyeShape& d, const EyeShape& s) {
  for (int i = 0; i < EYE_COLS; i++) { d.top[i] = s.top[i]; d.bot[i] = s.bot[i]; }
}
static void copyMood(LiveMood& d, const MoodDef& m) {
  copyShape(d.L, m.L); copyShape(d.R, m.R);
  d.dx = m.dx; d.dy = m.dy; d.lx = m.lx; d.rx = m.rx;
  d.ldy = m.ldy; d.rdy = m.rdy; d.decor = m.decor;
}

static void liveMood(LiveMood& out) {
  float k = easeC(g_moodT);
  for (int i = 0; i < EYE_COLS; i++) {
    out.L.top[i] = g_cur.L.top[i] + (g_tgt.L.top[i] - g_cur.L.top[i]) * k;
    out.L.bot[i] = g_cur.L.bot[i] + (g_tgt.L.bot[i] - g_cur.L.bot[i]) * k;
    out.R.top[i] = g_cur.R.top[i] + (g_tgt.R.top[i] - g_cur.R.top[i]) * k;
    out.R.bot[i] = g_cur.R.bot[i] + (g_tgt.R.bot[i] - g_cur.R.bot[i]) * k;
  }
  out.dx  = g_cur.dx  + (g_tgt.dx  - g_cur.dx)  * k;
  out.dy  = g_cur.dy  + (g_tgt.dy  - g_cur.dy)  * k;
  out.lx  = g_cur.lx  + (g_tgt.lx  - g_cur.lx)  * k;
  out.rx  = g_cur.rx  + (g_tgt.rx  - g_cur.rx)  * k;
  out.ldy = g_cur.ldy + (g_tgt.ldy - g_cur.ldy) * k;
  out.rdy = g_cur.rdy + (g_tgt.rdy - g_cur.rdy) * k;
  out.decor = (g_moodT < 0.5f) ? g_cur.decor : g_tgt.decor;
}

static void faceSetMood(int id, float durMs = 260.0f) {
  if (id < 0 || id >= MOOD_COUNT) return;
  liveMood(g_cur);                  // freeze the current interpolation as the new start
  // Leaving "think", what is on screen is "canonical pose + swing phase". Freezing only
  // liveMood snaps back to the canonical pose first and then moves - a visible pop.
  // Bake the phase into the start point so it continues from wherever it is.
  if (g_moodId == M_THINK && faceCfg.swapEye && g_thinkEnter >= 0 && g_moodT >= 1) {
    float k = (1 - cosf((g_faceT - g_thinkEnter) / 14.0f)) / 2;
    for (int i = 0; i < EYE_COLS; i++) {
      float aT = MOOD_TABLE[M_THINK].L.top[i], aB = MOOD_TABLE[M_THINK].L.bot[i];
      float bT = MOOD_TABLE[M_THINK].R.top[i], bB = MOOD_TABLE[M_THINK].R.bot[i];
      g_cur.L.top[i] = aT + (bT - aT) * k;
      g_cur.L.bot[i] = aB + (bB - aB) * k;
      g_cur.R.top[i] = bT + (aT - bT) * k;
      g_cur.R.bot[i] = bB + (aB - bB) * k;
    }
  }
  copyMood(g_tgt, MOOD_TABLE[id]);
  g_moodT = 0; g_moodDur = durMs;
  g_moodId = id;
  g_qEnter = -1;
  g_thinkEnter = -1;
  g_peekEnter = -1;                 // every entry into peek starts from "not yet leaning out"
}

static void faceSetDir(int d, float durMs = 260.0f) {
  if (d < 0 || d > 8) return;
  float k = easeC(g_dirT);
  g_dirCur[0] += (g_dirTgt[0] - g_dirCur[0]) * k;
  g_dirCur[1] += (g_dirTgt[1] - g_dirCur[1]) * k;
  g_dirTgt[0] = DIRS[d].x;
  g_dirTgt[1] = DIRS[d].y;
  g_dirT = 0; g_dirDur = durMs;
}

static inline int faceMoodId() { return g_moodId; }

// ---------------- Blink curves ----------------
static float blinkCurve(float p) {
  if (p < 0.30f) return easeC(p / 0.30f);
  if (p < 0.42f) return 1.0f;
  return 1.0f - easeC((p - 0.42f) / 0.58f);
}
// Single-eye wink: close -> hold -> open -> long rest, u in [0, 0.5)
// Matches web WINK_PRESETS.soft: 360 frames per round (6s), one eye 5% close / 10% hold / 6% open
// The old build held it shut for 0.8s and swapped every 3s, which read as constant squinting.
static const float WINK_PERIOD = 360.0f;
static float winkCurve(float u) {
  float t = u * 2.0f;                       // normalise to this eye own 0~1
  const float A = 0.050f, B = A + 0.100f, C = B + 0.060f;
  if (t < A) return easeC(t / A);
  if (t < B) return 1.0f;
  if (t < C) return 1.0f - easeC((t - B) / 0.060f);
  return 0;                                 // rest of the window: normal open eye
}
// Eyelid closure: the top edge sweeps down throughout, the bottom edge only lifts late
static void blinkShape(const EyeShape& in, float amt, EyeShape& out) {
  if (amt <= 0.002f) { copyShape(out, in); return; }
  float c = 0; int n = 0;
  for (int i = 0; i < EYE_COLS; i++)
    if (in.top[i] <= in.bot[i]) { c += (in.top[i] + in.bot[i]) * 0.5f; n++; }
  if (!n) { copyShape(out, in); return; }
  float cy = c / n;
  float aT = min(1.0f, amt * 1.06f);
  float aB = min(1.0f, powf(amt, 2.6f));
  for (int i = 0; i < EYE_COLS; i++) {
    out.top[i] = in.top[i] + (cy - in.top[i]) * aT;
    out.bot[i] = in.bot[i] + (cy - in.bot[i]) * aB;
  }
}

// ---------------- Draw one eye ----------------
static void drawEye(TFT_eSprite& g, const EyeShape& s, float cx,
                    float dx, float dy, float bright, bool rainbow) {
  int lo = 99, hi = -1; float minTop = 99;
  for (int i = 0; i < EYE_COLS; i++) {
    if (s.top[i] <= s.bot[i]) {
      if (i < lo) lo = i;
      if (i > hi) hi = i;
      if (s.top[i] < minTop) minTop = s.top[i];
    }
  }
  if (hi < 0) return;
  int mT = (int)lroundf(minTop + dy + EYE_CY);

  for (int i = lo; i <= hi; i++) {
    float t = s.top[i], b = s.bot[i];
    if (t > b) continue;
    float edgeK = (i == lo || i == hi) ? 0.58f : 1.0f;
    int x  = (int)lroundf((cx - 6 + i + dx) * PX);   // to pixels, then round: 1px precision, not a cell
    int t0 = (int)lroundf(t + dy + EYE_CY);
    int b0 = (int)lroundf(b + dy + EYE_CY);
    if (b0 < t0) continue;

    int r = t0;
    if ((b0 - t0) > 4 && t0 <= mT + 1) {         // lit cap on the top row
      g.fillRect(x, r * PX, PX, PX, glowTone(bright));
      r++;
    }
    if (r > b0) continue;
    if (rainbow) {
      for (int rr = r; rr <= b0; rr++)
        g.fillRect(x, rr * PX, PX, PX, rainbowTone((int)lroundf(cx - 6 + i), rr));
    } else {
      float a = bright * edgeK;
      g.fillRect(x, r * PX, PX, (b0 - r + 1) * PX, eyeTone(a));
    }
  }
}

// ---------------- Pixel bitmaps ----------------
static const uint8_t HEART_BITS[7] = {
  0b0110110, 0b1111111, 0b1111111, 0b1111111, 0b0111110, 0b0011100, 0b0001000
};
// #ff7ca6 #ff4d7d #ffb3cc #ffd9e6 #ff8fb4 #5cff92 as RGB565

static const uint8_t Z5_BITS[5] = {
  0b11111, 0b00010, 0b00100, 0b01000, 0b11111
};

// Confused question mark: sampled stroke positions plus the tail, scaled on a spring at runtime
static const float Q_PTS[34][2] = {
  {32.40f,4.66f},{32.41f,4.42f},{32.45f,4.18f},{32.53f,3.94f},{32.64f,3.73f},{32.78f,3.53f},
  {32.95f,3.35f},{33.14f,3.20f},{33.35f,3.07f},{33.58f,2.98f},{33.81f,2.92f},{34.06f,2.90f},
  {34.30f,2.91f},{34.54f,2.96f},{34.77f,3.04f},{34.99f,3.15f},{35.19f,3.29f},{35.36f,3.46f},
  {35.51f,3.65f},{35.63f,3.87f},{35.72f,4.09f},{35.78f,4.33f},{35.80f,4.57f},{35.79f,4.82f},
  {35.74f,5.06f},{35.66f,5.29f},{35.54f,5.50f},{35.42f,5.78f},{35.30f,6.06f},{35.18f,6.34f},
  {35.06f,6.61f},{34.94f,6.89f},{34.82f,7.17f},{34.70f,7.45f}
};
static const float Q_DOT_X = 34.70f, Q_DOT_Y = 9.05f;
static const float Q_AX = 34.10f, Q_AY = 8.75f;   // scale anchor (bottom), so it grows upward

// ---------------- Decor ----------------
struct HeartP {
  float birth, x0, y0, vy, amp, ph, fr, life;
  float exitA = -1, exitAge = -1;           // brightness frozen at exit / age in frames
  uint8_t unit, col;
};
static HeartP g_heart[MAX_HEART];
static int   g_heartN = 0;
static float g_heartNext = 0;
// Leaving is not a hard clear: stop spawning, hang in the air, fade over 0.3s
static float g_heartFade = 1;
static bool  g_heartOn = false;              // only fade on exit if "love" was really entered

static void spawnHeart(float birth) {
  if (g_heartN >= MAX_HEART) return;
  HeartP& p = g_heart[g_heartN++];
  p.birth = birth;
  float r = rnd01();
  if (r < 0.60f)      p.y0 = 32 + rnd01() * 3;
  else if (r < 0.86f) p.y0 = 18 + rnd01() * 14;
  else                p.y0 = 6  + rnd01() * 12;
  p.vy   = -(0.30f + rnd01() * 0.16f);
  p.life = (p.y0 - 2) / fabsf(p.vy) * 1.18f;
  p.x0   = 2 + rnd01() * 34;
  p.amp  = 0.10f + rnd01() * 0.16f;
  p.ph   = rnd01() * 6.2832f;
  p.fr   = 0.07f + rnd01() * 0.06f;
  p.unit = (rnd01() < 0.55f) ? 2 : 3;
  p.col  = (rnd01() < 0.06f) ? 5 : (uint8_t)(rnd01() * 5);
}

static void facePrimeHearts() {
  for (int i = 0; i < 7; i++) spawnHeart(-rnd01() * 60.0f);
}

// brightness from its own lifetime alone (exit fade not included)
static float heartAlpha(const HeartP& p) {
  float age = g_faceT - p.birth;
  if (age < 0) return 0;
  float t = age / p.life;
  if (t >= 1) return 0;
  float a = t < 0.16f ? t / 0.16f : (t > 0.58f ? 1 - (t - 0.58f) / 0.42f : 1);
  float y = p.y0 + p.vy * age;
  if (y < 6) a *= max(0.0f, (y - 1) / 5);
  return a;
}

static void updateHearts(float dtF, bool spawn) {
  if (spawn) {
    if (g_heartNext <= 0 && g_heartN < MAX_HEART) {
      spawnHeart(g_faceT);
      g_heartNext = 15 + rnd01() * 16;
    }
    g_heartNext -= dtF;
    // recycle: expired ones are dropped (no recycling during exit, the fade handles it)
    for (int i = g_heartN - 1; i >= 0; i--) {
      if (g_faceT - g_heart[i].birth > g_heart[i].life) {
        g_heart[i] = g_heart[--g_heartN];
      }
    }
  }
}

static void drawHearts(TFT_eSprite& g, float fade) {
  if (fade <= 0.01f) return;
  for (int i = 0; i < g_heartN; i++) {
    HeartP& p = g_heart[i];
    float age = g_faceT - p.birth;
    if (age < 0) continue;

    // On exit drop the upward speed to 1/5 so hearts hang and dim in place,
    // instead of flying on into the eye and being covered by it.
    float ae = p.exitAge >= 0 ? p.exitAge + (age - p.exitAge) * 0.20f : age;

    float y = p.y0 + p.vy * ae;
    if (y < -4 || y > 34) continue;
    float x = p.x0 + sinf(ae * p.fr + p.ph) * p.amp * 4;

    float a = (p.exitA >= 0 ? p.exitA : heartAlpha(p)) * fade;
    if (a < 0.02f) continue;

    int px = (int)lroundf(x * PX), py = (int)lroundf(y * PX);
    if (px < -4 * PX || px > SCR_W + 4 * PX) continue;

    // scale brightness by a (the web uses rgba alpha, here it becomes colour depth)
    uint16_t c0 = theme.heart[p.col];
    float k = min(1.0f, a) * 0.95f;
    uint16_t c = rgb565(((c0 >> 11) & 31) / 31.0f * k,
                        ((c0 >> 5) & 63) / 63.0f * k,
                        (c0 & 31) / 31.0f * k);

    int u = p.unit;
    for (int r = 0; r < 7; r++) {
      uint8_t bits = HEART_BITS[r];
      for (int q = 0; q < 7; q++)
        if (bits & (0x40 >> q)) g.fillRect(px + q * u, py + r * u, u, u, c);
    }
  }
}

// Sleep Zzz: three glyphs drifting up and shrinking
static void drawZzz(TFT_eSprite& g, float ddx, float ddy, const LiveMood& sh) {
  const float T = 270.0f;
  float cyc = fmodf(g_faceT, T) / T;
  float rise = cyc;
  float fx = clampf(ddx * 0.52f + sh.dx * 0.35f, -1.9f, 1.9f);
  float fy = clampf(ddy * 0.35f + sh.dy * 0.35f, -1.3f, 1.3f);
  float fade = 1.0f;
  if (cyc > 0.76f) fade = max(0.0f, 1 - (cyc - 0.76f) / 0.24f);

  const float zx[3] = {29.6f, 32.6f, 35.2f};
  const float zy[3] = {10.0f, 6.6f, 3.7f};
  const float t0[3] = {0.03f, 0.19f, 0.34f};
  const float t1[3] = {0.13f, 0.29f, 0.44f};
  const float un[3] = {3.2f, 2.7f, 2.3f};

  for (int i = 0; i < 3; i++) {
    float a = cyc < t0[i] ? 0 : (cyc < t1[i] ? (cyc - t0[i]) / (t1[i] - t0[i]) : 1);
    a *= fade;
    if (a <= 0.02f) continue;
    float gx = zx[i] + fx + sinf(cyc * 7 + i) * 0.22f;
    float gy = zy[i] - rise + fy;
    if (gy < 0) continue;
    int ox = (int)lroundf(gx) * PX, oy = (int)lroundf(gy) * PX;
    uint16_t c = iconTone(0.78f, a);           // Zzz：hsla(h,s%,78%,a)
    int u = max(1, (int)lroundf(un[i]));   // web uses 3.2/2.7/2.3px with AA; here it is hard-edged
    for (int r = 0; r < 5; r++) {
      uint8_t bits = Z5_BITS[r];
      for (int q = 0; q < 5; q++)
        if (bits & (0x10 >> q)) g.fillRect(ox + q * u, oy + r * u, u, u, c);
    }
  }
}

// Confused question mark: whole glyph on one under-damped spring, pops out once
struct SpringState { float kx, ky, a, y, wob; };
static SpringState qSpring(float t, float period) {
  const float WN = 19.0f / 60.0f, ZETA = 0.50f;
  const float WD = WN * sqrtf(1 - ZETA * ZETA), DZ = ZETA * WN;
  float k = 1 - expf(-DZ * t) * (cosf(WD * t) + (DZ / WD) * sinf(WD * t));
  float j = (k - 1) * 0.22f;
  SpringState s;
  s.kx = k * (1 - j); s.ky = k * (1 + j);
  s.a = 1; s.y = 0; s.wob = 0;
  if (t > 18) {
    float u = t - 18;
    s.wob = 0.42f * expf(-u / 9) * sinf(u * 0.58f);
  }
  float outStart = period - 15;
  if (t >= outStart) {
    float u = min(1.0f, (t - outStart) / 15.0f);
    float sc = 1 - 0.78f * powf(u, 1.25f);
    s.kx *= sc; s.ky *= sc;
    s.a = 1 - powf(u, 1.15f);
    s.y = -1.15f * u;
    s.wob *= 1 - u * 0.6f;
  }
  return s;
}

static void drawQ(TFT_eSprite& g, float kx, float ky, float ox, float oy, float alpha) {
  if (alpha <= 0.02f || kx <= 0.03f || ky <= 0.03f) return;
  int w = max(1, (int)lroundf(PX * kx));
  int h = max(1, (int)lroundf(PX * ky));
  uint16_t c = iconTone(0.74f, alpha);
  for (int i = 0; i < 34; i++) {
    float X = Q_AX + (Q_PTS[i][0] - Q_AX) * kx + ox;
    float Y = Q_AY + (Q_PTS[i][1] - Q_AY) * ky + oy;
    g.fillRect((int)lroundf(X * PX) - (w >> 1),
               (int)lroundf(Y * PX) - (h >> 1), w, h, c);
  }
  uint16_t cd = iconTone(0.93f, alpha);
  g.fillRect((int)lroundf((Q_AX + (Q_DOT_X - Q_AX) * kx + ox) * PX) - (w >> 1),
             (int)lroundf((Q_AY + (Q_DOT_Y - Q_AY) * ky + oy) * PX) - (h >> 1), w, h, cd);
}

// Think dots: 1/3 cycle apart, lifting in sequence, brighter and bigger at the peak
static void drawDots(TFT_eSprite& g, float ox, float oy, float alpha) {
  if (alpha <= 0.02f) return;
  const float baseX = 30.40f, baseY = 5.45f, gap = 3.05f;
  for (int k = 0; k < 3; k++) {
    float ph = g_faceT * 0.104f - k * 2.094f;
    float s = max(0.0f, sinf(ph));
    float e = s * s * (3 - 2 * s);
    float x = baseX + k * gap + ox;
    float y = baseY - e * 1.25f + oy;
    float sz = 1.02f + 0.72f * e;
    float a = alpha * (0.52f + 0.48f * e);
    int px = (int)lroundf((x - sz / 2) * PX);
    int py = (int)lroundf((y - sz / 2) * PX);
    int ww = (int)lroundf(sz * PX);
    g.fillRect(px, py, ww, ww, iconTone(0.64f + 0.28f * e, a));
  }
}

// ---------------- Render one frame ----------------
static void faceRender() {
  TFT_eSprite& g = *g_spr;
  g.fillSprite(TFT_BLACK);

  // advance the transition
  if (g_moodT < 1) g_moodT = min(1.0f, g_moodT + g_faceStepMs / g_moodDur);
  if (g_dirT  < 1) g_dirT  = min(1.0f, g_dirT  + g_faceStepMs / g_dirDur);

  LiveMood sh;
  liveMood(sh);

  // think: eye size swap
  if (faceCfg.swapEye && g_moodId == M_THINK && g_moodT >= 1) {
    if (g_thinkEnter < 0) g_thinkEnter = g_faceT;
    float k = (1 - cosf((g_faceT - g_thinkEnter) / 14.0f)) / 2;
    for (int i = 0; i < EYE_COLS; i++) {
      float aT = MOOD_TABLE[M_THINK].L.top[i], aB = MOOD_TABLE[M_THINK].L.bot[i];
      float bT = MOOD_TABLE[M_THINK].R.top[i], bB = MOOD_TABLE[M_THINK].R.bot[i];
      sh.L.top[i] = aT + (bT - aT) * k;
      sh.L.bot[i] = aB + (bB - aB) * k;
      sh.R.top[i] = bT + (aT - bT) * k;
      sh.R.bot[i] = bB + (aB - bB) * k;
    }
    sh.decor = DECOR_SWAP;
  }

  float ddx = g_dirCur[0] + (g_dirTgt[0] - g_dirCur[0]) * easeC(g_dirT);
  float ddy = g_dirCur[1] + (g_dirTgt[1] - g_dirCur[1]) * easeC(g_dirT);

  // peek: slow sideways drift - lean out, pull back, lean out again, hold, swap sides
  // The offset used to be added straight to ex: landing on any phase jumped from there,
  // so entering snapped and leaving popped back to centre. Now it lives in g_peekOff:
  // target from the phase, actual value follows with a time constant, continuous in and out.
  float dtF = g_faceStepMs * 60.0f / 1000.0f;
  float peekTgt = 0;
  if (g_moodId == M_PEEK) {
    if (g_peekEnter < 0) g_peekEnter = g_faceT;
    float pt = fmodf(g_faceT - g_peekEnter, 380.0f) / 380.0f;
    if      (pt < 0.16f) peekTgt = 3.0f * easeC(pt / 0.16f);
    else if (pt < 0.21f) peekTgt = 3.0f - (pt - 0.16f) / 0.05f * 0.7f;
    else if (pt < 0.27f) peekTgt = 2.3f + (pt - 0.21f) / 0.06f * 0.7f;
    else if (pt < 0.46f) peekTgt = 3.0f;
    else if (pt < 0.56f) peekTgt = 3.0f * (1 - (pt - 0.46f) / 0.10f);
    else if (pt < 0.64f) peekTgt = -2.2f * easeC((pt - 0.56f) / 0.08f);
    else if (pt < 0.84f) peekTgt = -2.2f;
    else                 peekTgt = -2.2f * (1 - (pt - 0.84f) / 0.16f);
  }
  g_peekOff += (peekTgt - g_peekOff) * (1.0f - powf(0.80f, dtF));
  float ex = ddx + g_peekOff;

  // auto blink
  // blink and sleep are already a single line; stacking auto-blink on top makes it jitter.
  bool eyesShut = (g_moodId == M_BLINK || g_moodId == M_SLEEP);
  if (faceCfg.blink && !eyesShut) {
    g_nextBlink -= g_faceStepMs * 60.0f / 1000.0f;
    if (g_nextBlink <= 0 && g_blinkPhase < 0) {
      g_blinkPhase = 0;
      g_doubleBlink = (rnd01() < 0.20f) ? 1 : 0;
    }
    if (g_blinkPhase >= 0) {
      g_blinkPhase += g_faceStepMs * 60.0f / 1000.0f / 24.0f;
      if (g_blinkPhase >= 1) {
        g_blinkPhase = -1; g_blinkAmt = 0;
        if (g_doubleBlink > 0) { g_doubleBlink--; g_nextBlink = 8; }
        else g_nextBlink = 150 + (int)(rnd01() * 210);
      } else {
        g_blinkAmt = blinkCurve(g_blinkPhase);
      }
    }
  } else {
    g_blinkAmt = 0; g_blinkPhase = -1;
    // re-roll after releasing, so the eyes do not blink the instant they open
    if (eyesShut) g_nextBlink = 150 + (int)(rnd01() * 210);
  }

  float bright = faceCfg.breath ? 0.84f + 0.16f * sinf(g_faceT / 48.0f) : 1.0f;

  // dizzy: random shake
  float jx = 0, jy = 0;
  if (sh.decor == DECOR_SHAKE) {
    jx = (rnd01() - 0.5f) * 0.9f;
    jy = (rnd01() - 0.5f) * 0.9f;
  }

  float winkL = 0, winkR = 0;
  float sqL = g_blinkAmt, sqR = g_blinkAmt;

  if (sh.decor == DECOR_SHY) {
    float v = max(0.0f, sinf(g_faceT / 72.0f));
    float m = powf(v, 8) * 0.20f;
    sqL = sqR = min(0.95f, g_blinkAmt + m);
  }
  if (sh.decor == DECOR_SMUG) {
    float v = max(0.0f, sinf(g_faceT / 45.0f));
    float m = powf(v, 14) * 0.42f;
    sqL = min(0.95f, g_blinkAmt + m);
    sqR = min(0.95f, g_blinkAmt + m * 1.15f);
  }
  if (sh.decor == DECOR_CONFUSED) {
    sh.ldy = sinf(g_faceT / 58.0f) * 0.85f;
    sh.rdy = sinf(g_faceT / 79.0f + 1.7f) * 0.85f;
  }
  if (sh.decor == DECOR_WINK) {
    float wt = fmodf(g_faceT, WINK_PERIOD) / WINK_PERIOD;
    if (wt < 0.5f) winkL = winkCurve(wt);
    else           winkR = winkCurve(wt - 0.5f);
  }

  EyeShape L, R;
  blinkShape(sh.L, min(1.0f, sqL + winkL), L);
  blinkShape(sh.R, min(1.0f, sqR + winkR), R);

  // ---- hearts drawn first, bottom layer ----
  // Test against g_moodId (target), not sh.decor (the interpolating one): decor flips past
  // the halfway point, so the first half stayed bright and the fade started midway - a visible break.
  if (g_moodId == M_LOVE && faceCfg.heart) {
    g_heartOn = true;
    g_heartFade = min(1.0f, g_heartFade + dtF * 0.060f);          // fade in ~0.28s
    for (int i = 0; i < g_heartN; i++) { g_heart[i].exitA = -1; g_heart[i].exitAge = -1; }
    updateHearts(dtF, true);
  } else if (g_heartOn && g_heartN > 0) {
    g_heartFade = max(0.0f, g_heartFade - dtF * 0.055f);          // fade out ~0.30s
    for (int i = 0; i < g_heartN; i++) {
      if (g_heart[i].exitA < 0) {                                 // first exit frame: pin brightness
        float own = heartAlpha(g_heart[i]);
        if (own > 0.01f) { g_heart[i].exitA = max(own, 0.45f); g_heart[i].exitAge = g_faceT - g_heart[i].birth; }
      }
    }
  }
  if (g_heartOn && g_heartN > 0) {
    drawHearts(g, g_heartFade);
    if (g_heartFade <= 0) { g_heartN = 0; g_heartOn = false; }    // reclaim only when fully faded
  }

  bool rb = (sh.decor == DECOR_RAINBOW && g_moodT > 0.35f);
  drawEye(g, L, EYE_CL + sh.lx, ex + sh.dx + jx, ddy + sh.dy + jy + sh.ldy, bright, rb);
  drawEye(g, R, EYE_CR + sh.rx, ex + sh.dx + jx, ddy + sh.dy + jy + sh.rdy, bright, rb);

  // ---- tears ----
  if (sh.decor == DECOR_TEAR) {
    float cyc = fmodf(g_faceT, 96.0f) / 96.0f;
    for (int k = 0; k < 2; k++) {
      float ph = fmodf(cyc + k * 0.5f, 1.0f);
      if (ph > 0.8f) continue;
      float y0 = EYE_CY + sh.dy + 6;
      float y = y0 + ph * 7;
      float a = 0.92f - 0.55f * (ph / 0.8f);
      uint16_t c = rgb565(theme.tearR * a, theme.tearG * a, theme.tearB * a);
      for (int e = 0; e < 2; e++) {
        float cx = (e == 0 ? EYE_CL : EYE_CR) - 0.5f;
        g.fillRect((int)lroundf(cx + ddx) * PX, (int)lroundf(y) * PX, PX * 2, PX * 3, c);
      }
    }
  }

  // ---- shy blush ----
  if (sh.decor == DECOR_SHY) {
    uint16_t c = rgb565(theme.blushR * 0.85f, theme.blushG * 0.85f, theme.blushB * 0.85f);
    for (int e = 0; e < 2; e++) {
      float cx = (e == 0 ? EYE_CL - 3.8f : EYE_CR + 0.8f);
      g.fillRect((int)lroundf(cx + ex) * PX,
                 (int)lroundf(EYE_CY + 7.6f + sh.dy) * PX, PX * 3, PX, c);
    }
  }

  // ---- Zzz ----
  if (sh.decor == DECOR_ZZZ && faceCfg.zzz) drawZzz(g, ddx, ddy, sh);

  // ---- sweat drop ----
  if (sh.decor == DECOR_SWEAT && faceCfg.sweat) {
    float cyc = fmodf(g_faceT, 150.0f) / 150.0f;
    int bx = (int)lroundf(EYE_CR + 5 + ddx);
    int by = (int)lroundf(EYE_CY - 8 + cyc * 5);
    float a = cyc < 0.75f ? 0.95f : 0.95f * (1 - (cyc - 0.75f) / 0.25f);
    uint16_t c = rgb565(theme.sweatR * a, theme.sweatG * a, theme.sweatB * a);
    g.fillRect(bx * PX, by * PX, PX, PX, c);
    g.fillRect((bx - 1) * PX, (by + 1) * PX, PX * 3, PX * 2, c);
  }

  // ---- confused question mark ----
  if (sh.decor == DECOR_CONFUSED && faceCfg.qmark) {
    if (g_qEnter < 0) g_qEnter = g_faceT;
    static const float QCYC[5] = {152, 206, 168, 232, 188};
    static const float QTOT = 946;
    float tt = fmodf(g_faceT - g_qEnter, QTOT);
    if (tt < 0) tt += QTOT;
    int idx = 0; float acc = 0;
    while (idx < 5 && tt >= acc + QCYC[idx]) { acc += QCYC[idx]; idx++; }
    if (idx > 4) idx = 4;
    float local = tt - acc;
    float period = QCYC[idx];

    SpringState st = qSpring(local, period);
    float outA = (g_moodId == M_CONFUSED) ? 1.0f : max(0.0f, 1 - g_moodT * 2.2f);
    if (st.a > 0.01f && outA > 0) {
      drawQ(g, st.kx, st.ky, ddx * 0.46f + st.wob, ddy * 0.32f + st.y, st.a * outA);
    }
  }

  // ---- think dots ----
  if (sh.decor == DECOR_SWAP && faceCfg.dots) {
    float outA = (g_moodId == M_THINK) ? 1.0f : max(0.0f, 1 - g_moodT * 2.2f);
    if (outA > 0) drawDots(g, ddx * 0.42f, ddy * 0.28f + sinf(g_faceT / 53.0f) * 0.16f, outA);
  }
}

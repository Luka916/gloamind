// ===========================================================
//  pet_face.h  —  参数化像素眼睛 · ESP32 移植版
//  与浏览器版 eyes_v10.html 一一对应，不是重画，是逐段翻译
//
//  依赖：TFT_eSPI（User_Setup.h 见 README.md）
//  屏幕：ST7735S 128x160，横屏 160x128
// ===========================================================
#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <esp_system.h>
#include "moods.h"

// ---------------- 画布 ----------------
#define SCR_W 160
#define SCR_H 128
#define PX    4              // 一格 = 4px，和网页版一致。2026-11-14 22:35 曾试 2px（更细腻但眼睛缩一半），已回退
#define EYE_CL 14.0f         // 左眼中心（格）
#define EYE_CR 26.0f
#define EYE_CY 16.0f
#define MAX_HEART 12

// ---------------- 配置 ----------------
struct FaceConfig {
  bool  blink   = true;
  bool  heart   = true;
  bool  zzz     = true;
  bool  sweat   = true;
  bool  qmark   = true;
  bool  dots    = true;
  bool  swapEye = true;      // 思考·大小眼互换
  float hue     = 146.0f;
  float lum     = 1.0f;
};
static FaceConfig faceCfg;

// ---------------- 调色板 ----------------
// 以后接取色器，只改这里的数值；下面的渲染代码一行都不用动。
// H/S/L 都是 0~1（H 是 0~360）；R/G/B 也是 0~1，最终由 rgb565() 量化。
struct Palette {
  float eyeH,  eyeS,  eyeL;                 // 眼睛主体
  float glowH, glowS, glowL;                // 眼皮顶部高光
  float iconH, iconS;                       // 问号 / 三点 / Zzz（亮度各自算）
  float tearR,  tearG,  tearB;              // 哭 · 泪滴
  float sweatR, sweatG, sweatB;             // 无语 · 汗滴
  float blushR, blushG, blushB;             // 害羞 · 腮红
  uint16_t heart[6];                        // 爱心 6 色
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

// ---------------- 全局状态 ----------------
static float g_faceT = 0;                 // 虚拟 60fps 帧计数
static float g_faceStepMs = 25.0f;        // 本帧实际间隔（ms），由主循环写入

struct LiveMood {
  EyeShape L, R;
  float dx, dy, lx, rx, ldy, rdy;
  Decor decor;
};

static LiveMood g_cur, g_tgt;
static float g_moodT = 1.0f, g_moodDur = 260.0f;
static int   g_moodId = M_NORMAL;
static float g_dirCur[2] = {0, 0}, g_dirTgt[2] = {0, 0};   // 单位向量：方向单独平滑
static float g_ampCur = 6.5f, g_ampTgt = 6.5f;             // 幅度（格）：按表情分档单独平滑
static int8_t g_dirIdx = 0;                                // 当前方向号 0~12，i 命令要看它
static float g_dirT = 1.0f, g_dirDur = 260.0f;
static float g_blinkAmt = 0, g_blinkPhase = -1, g_nextBlink = 200;
static int   g_doubleBlink = 0;
static float g_qEnter = -1, g_thinkEnter = -1;
static float g_peekOff = 0, g_peekEnter = -1;   // 偷看横向偏移（实际值 + 进入时刻）

/* ---- v14 新增表情的运行态（对齐 eyes_v14.html） ---- */
static float g_winkHold  = 0;      // wink = 持续闭左眼的"状态"，不是周期动作：0 睁 → 1 闭
static float g_nonoT     = -1;     // 摇头起始帧（-1 = 还没起手）
static float g_nodT      = -1;     // 点头起始帧（一次性动作，点完自己回 normal）
static float g_startleT  = -1;     // 被吓一跳的起始帧
static float g_exEnter   = -1;     // 感叹号自己的入场时刻（比眼睛晚半拍冒出来）
static float g_shakeMix  = 0;      // 0 = 正常视线偏移；1 = 完全居中摇头
static float g_glitchSeed = 0;     // 故障：每 2 帧才换一次的切片种子

static TFT_eSprite* g_spr = nullptr;

struct DirDef { float x, y; };
/* 视线：时钟十二向（对齐网页 eyes_v14 的 DIRS），每 30° 一个点位。
   这里只存**单位向量**，实际位移 = 单位向量 × 当前幅度（见 dirAmpFor）。
   早先这张表直接写死 ±3.2 格、只有八个方向，网页早就十二向 6.5 了 —— 两边对不上。
   y 向下为正，所以"上"是 -1。
   顺序就是串口号，**不要改顺序**，d 命令 0~12 对应这张表。 */
static const DirDef DIRS[13] = {
  { 0.000f,  0.000f},   //  0 中
  { 0.000f, -1.000f},   //  1 12 点
  { 0.500f, -0.866f},   //  2  1 点
  { 0.866f, -0.500f},   //  3  2 点
  { 1.000f,  0.000f},   //  4  3 点
  { 0.866f,  0.500f},   //  5  4 点
  { 0.500f,  0.866f},   //  6  5 点
  { 0.000f,  1.000f},   //  7  6 点
  {-0.500f,  0.866f},   //  8  7 点
  {-0.866f,  0.500f},   //  9  8 点
  {-1.000f,  0.000f},   // 10  9 点
  {-0.866f, -0.500f},   // 11 10 点
  {-0.500f, -0.866f}    // 12 11 点
};

/* 幅度分档（单位：格，一格 14px）。和网页 DIR_AMP_* 一一对应：
   6.5 —— 普通表情，上下左右都够走到"眼睛快贴屏幕边"，屏幕还有富余。
   4.0 —— 带飘浮装饰的六个（泪 / 害羞 / 无语 / 困惑 / 睡 / 思考）。装饰跟得慢
          （Zzz 跟 0.70、问号 0.46、三点 0.60），眼睛跑太快会压到它们。
   6.0 —— 偷看，它自己还有 ±3 格横向探出，叠满会贴边。
   2.8 —— 惊讶 / 被吓：眼睛高 24~26 格撑满屏幕，上下只剩 4 格，再多就被切掉。 */
static float dirAmpFor(int m) {
  if (m == M_SURPRISE || m == M_STARTLE) return 2.8f;
  if (m == M_PEEK) return 6.0f;
  if (m == M_CRY || m == M_SHY || m == M_SPEECHLESS ||
      m == M_CONFUSED || m == M_SLEEP || m == M_THINK) return 4.0f;
  return 6.5f;
}

/* ---------------- 云台跟随（v13.1） ----------------
 * 头往哪边转，眼睛跟着瞟一点点；头到位了眼睛就回中。
 * 网页 eyes_v13.html 里同名逻辑，参数一一对应，改一边记得改另一边。
 *
 * 三重保险，确保绝不越出 v13 验证过的眼眶边界：
 *   1. NECK_MAX   单侧最多 2.2 格
 *   2. 衰减       pan 超过 NECK_REACH 后增益按 1/(1+(p/25)^2) 往下掉
 *   3. DIR_BUDGET 表情已占的位移 + 云台增量 超过 6.5 格就整个放弃叠加，
 *                让位给表情自己演的方向（汗滴、问号就是这么保住不撞的）
 */
static const float NECK_GAIN  = 0.15f;   // 格 / 度。pan 12° → 约 1.5 格
static const float NECK_REACH = 25.0f;   // 衰减拐点（度）
static const float NECK_MAX   = 2.2f;    // 单侧上限（格）
static const float DIR_BUDGET = 6.5f;    // 眼神总位移预算
static float g_neckPanDeg = 0;           // 主程序写入的当前 PAN 角，-90~90

// ---------------- 小工具 ----------------
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

// HSL → RGB。网页版全是 hsla(h,100%,L%,a)，必须按 HSL 算，
// 直接拿 RGB 乘系数会把绿色压暗一大截（L=56% 的绿，max 通道其实是 1.0）
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

// hsla(h,100%,L%,a) 叠在黑底上
static inline uint16_t hslTone(float h, float s, float l, float a) {
  float r, g, b;
  hslToRgb(h, s, l, r, g, b);
  return rgb565(r * a, g * a, b * a);
}

// 眼睛主体：亮度锁在 56%·lum，只有不透明度跟着 bright·edgeK 走
// （网页版 col(a) 是 hsla(h,100%,56*lum%,a)，l 和 alpha 是两个独立参数）
static inline uint16_t eyeTone(float a0) {
  return hslTone(theme.eyeH, theme.eyeS, theme.eyeL * faceCfg.lum, a0);
}
static inline uint16_t glowTone(float br) {
  return hslTone(theme.glowH, theme.glowS, min(0.97f, theme.glowL * br), 0.97f);
}
static inline uint16_t iconTone(float l, float a) { return hslTone(theme.iconH, theme.iconS, l, a); }

static uint16_t rainbowTone(int col, int row) {
  float h = g_faceT * 4.0f + col * 22.0f + row * 9.0f + 600.0f;
  float r, g, b;
  hslToRgb(h, 1.0f, 0.62f, r, g, b);      // l = min(74, 62*bright)%，alpha 恒为 1
  return rgb565(r, g, b);
}

// ---------------- 表情切换 ----------------
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
  liveMood(g_cur);                  // 冻结当前插值结果作为新起点
  // 从"思考"切走时，画面上的形态是"标准大小眼 + 摆动相位"，
  // 只冻 liveMood 会先弹回标准大小眼再过渡（切到哪都"啪"一下）。
  // 这里把当前相位直接烘进起点：摆到哪儿就从哪儿接着走。
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
  g_peekEnter = -1;                 // 每次切进偷看都从"还没探出去"开始
}

static void faceSetDir(int d, float durMs = 260.0f) {
  if (d < 0 || d > 12) return;
  float k = easeC(g_dirT);
  g_dirCur[0] += (g_dirTgt[0] - g_dirCur[0]) * k;
  g_dirCur[1] += (g_dirTgt[1] - g_dirCur[1]) * k;
  g_dirTgt[0] = DIRS[d].x;            // 单位向量，幅度在渲染时乘上去
  g_dirTgt[1] = DIRS[d].y;
  g_dirIdx = (int8_t)d;
  g_dirT = 0; g_dirDur = durMs;
}

static inline int faceMoodId() { return g_moodId; }

/* 脖子联动：把"摇头说不"换算成舵机角度偏移（度）。
   曲线和眼睛那个 swingX 完全同源（3.6·e^(-t/38)·min(1,t/5)·sin(0.30t)），
   所以头和眼是一起甩的 —— 这才是同一个动作，不是两套动画各演各的。
   系数 4.0：眼睛那边 3.6 格是"脸的一半宽"，换算成本来只有 5.8°，
   太斯文了看不出在拒绝；放大到峰值约 14°，读起来才像"不"。
   没有舵机（或没接）时它只是个返回 0 的函数，不影响纯屏幕模式。 */
static inline float faceNeckSwingDeg() {
  if (g_nonoT < 0) return 0.0f;
  if (g_moodId != M_NONO && g_moodId != M_NONOBLINK) return 0.0f;
  float t = g_faceT - g_nonoT;
  return 3.6f * expf(-t / 38.0f) * min(1.0f, t / 5.0f) * sinf(t * 0.30f) * 4.0f;
}

/* 竖直轴联动：把"点头"换算成 TILT 舵机的角度偏移（度，低头为正）。
   ★ 曲线与渲染里那段 nodY 逐字同源：
       float k = fmodf(t, 15.0f) / 15.0f;
       nodY = 3.4f * sinf(k * M_PI) * (t < 15 ? 1.0 : 0.72);
     也就是 15 帧一下、共两下、第二下力度 0.72，头低下去和眼睛沉下去
     是同一帧开始、同一帧到底。写成同一个 t（都用 g_nodT 起算），
     以后哪边改了节拍，两边会一起改，不会跑成两套动画。

   为什么是"半正弦"而不是连续 sin：连续 sin 会正负交替，头上下抖四五次，
   那是打嗝不是点头。半正弦首尾精确落在 0，头能干净地停回原位。

   系数 2.9：眼睛那边 3.4 格换算成物理量本来只有 13° 的一半，
   放大到峰值约 9.9° —— SG90 带 1.78 寸屏的云台，10° 已经很明显，
   再大就会顶到壳（TILT 行程比 PAN 窄得多）。

   没有 TILT 舵机时它只是返回 0，纯屏幕模式完全不受影响。 */
static inline float faceNeckNodDeg() {
  if (g_nodT < 0) return 0.0f;
  if (g_moodId != M_NOD) return 0.0f;
  float t = g_faceT - g_nodT;
  if (t > 30.0f) return 0.0f;
  float k = fmodf(t, 15.0f) / 15.0f;
  return 3.4f * sinf(k * (float)M_PI) * (t < 15.0f ? 1.0f : 0.72f) * 2.9f;
}

// ---------------- 云台跟随 ----------------
// 由主循环每帧喂当前 PAN 角（合并版里就是 curPan），不喂就等于 0，行为和以前完全一样。
static inline void faceSetNeckPan(float deg) {
  g_neckPanDeg = clampf(deg, -90.0f, 90.0f);
}

/* 算出该给眼睛加多少格横向偏移。
 * baseDx 是表情自己已经占的位移，用来判断还有没有空间。 */
static inline float neckEyeOffset(float baseDx) {
  float p = g_neckPanDeg;
  if (p == 0) return 0;

  float w = 1.0f / (1.0f + (p * p) / (NECK_REACH * NECK_REACH));

  // 表情自己在演方向的（偷看 / 惊讶 / 带飘浮装饰的六个），只跟 35%，别抢戏
  float own = 1.0f;
  if (g_moodId == M_PEEK || g_moodId == M_SURPRISE ||
      g_moodId == M_CRY  || g_moodId == M_SHY      || g_moodId == M_SPEECHLESS ||
      g_moodId == M_CONFUSED || g_moodId == M_SLEEP || g_moodId == M_THINK) own = 0.35f;

  float off = clampf(p * NECK_GAIN * w * own, -NECK_MAX, NECK_MAX);
  if (fabsf(baseDx + off) > DIR_BUDGET) off = 0;   // 装不下就不加，绝不挤压已有画面
  return off;
}

// ---------------- 眨眼曲线 ----------------
static float blinkCurve(float p) {
  if (p < 0.30f) return easeC(p / 0.30f);
  if (p < 0.42f) return 1.0f;
  return 1.0f - easeC((p - 0.42f) / 0.58f);
}
// 单眼 wink：v14 起不是"眨一下"，而是**持续状态** —— 只要还在 wink，左眼就一直闭着，
// 切到任何别的表情才睁开。所以这里没有周期、没有左右轮换，只有一条 0/1 的平滑逼近。
// 早先是"周期 360 帧、左右眼轮着眨"，看着像在打拍子，不是 wink。
static const float WINK_TAU = 0.88f;     // 每帧残差保留比例：60fps 下约 15 帧（0.25s）走完 85%
// 眼皮式闭合：上缘全程下压，下缘后段才抬
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

// ---------------- 画一只眼 ----------------
/* kw：横向缩放，1 = 原样。只有"被吓一跳"会传 <1 ——
   纵向拉长必须同时横向收窄，否则那只是"纵向放大"，不像被拽。
   注意 x 的写法 (i-6)*kw：kw=1 时等价于原来的 cx-6+i，一个像素都不变。 */
/* glitchAmp：>0 时逐行绘制并按横条错位（故障表情），0 = 正常的整列一笔。
   之所以在 drawEye 里做而不是像网页那样"整屏拷一份再逐条贴回"：
   这块 MCU 没有第二块 sprite 的内存，读回自己又会染色。
   而故障要撕的基本只有眼睛（其余区域本来全黑），逐行错位是等价且更省的做法。 */
static void drawEye(TFT_eSprite& g, const EyeShape& s, float cx,
                    float dx, float dy, float bright, bool rainbow,
                    float kw = 1.0f, float glitchAmp = 0.0f) {
  int cw = (int)ceilf(PX * kw);
  if (cw < 1) cw = 1;
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

  // 故障用的确定性伪随机：同一条横带、同一帧，永远同一个数（左右眼才对得齐）
  auto bandRnd = [](int band, uint32_t salt) -> float {
    uint32_t h = (uint32_t)band * 2654435761u ^ ((uint32_t)g_glitchSeed * 40503u + salt * 97u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (h & 0xFFFFu) / 65535.0f;
  };

  for (int i = lo; i <= hi; i++) {
    float t = s.top[i], b = s.bot[i];
    if (t > b) continue;
    float edgeK = (i == lo || i == hi) ? 0.58f : 1.0f;
    /* 亚像素定位（和网页 v12 的修法完全一致）：先乘 PX 再取整。
       X 在 11-14 已经改对了，Y 漏了 —— 竖直方向仍然是 lroundf(格号)*PX，
       只能整格跳 4px。眼睛上下浮动、上下瞟、点头、眨眼全在 Y 上，
       这就是"板子上没有网页那么顺"的最后一块短板。
       现在两轴都是 1px 精度，块本身仍是硬边 4px，形状一格没变。 */
    int x  = (int)lroundf((cx + (i - 6.0f) * kw + dx) * PX);
    int yT = (int)lroundf((t + dy + EYE_CY) * PX);
    int yB = (int)lroundf((b + dy + EYE_CY) * PX) + PX;
    if (yB <= yT) continue;

    int t0 = (int)lroundf(t + dy + EYE_CY);   // 格行号，只用于"贴顶/够不够高"这类判断
    int b0 = (int)lroundf(b + dy + EYE_CY);

    if (glitchAmp > 0) {
      int yOff = yT - t0 * PX;                // 让故障横条也吃上亚像素位移
      // 每 2 行算一"条"：条内偏移一致，才看得出是横条错位而不是噪点
      for (int rr = t0; rr <= b0; rr++) {
        int band = rr >> 1;
        if (bandRnd(band, 47) < 0.12f) continue;            // 缺行 → 黑缝
        float d = bandRnd(band, 11) * 2.0f - 1.0f;
        int ox = (int)lroundf(d * d * d * PX * 1.8f * glitchAmp);   // 立方：小位移多、大位移少
        uint16_t c = (rr == t0 && (b0 - t0) > 4)
                     ? glowTone(bright) : eyeTone(bright * edgeK);
        g.fillRect(x + ox, rr * PX + yOff, cw, PX, c);
      }
      continue;
    }

    int y = yT, row = t0;
    if ((b0 - t0) > 4 && t0 <= mT + 1) {         // 顶沿那一行的亮头
      g.fillRect(x, yT, cw, PX, glowTone(bright));
      y = yT + PX; row = t0 + 1;
    }
    if (y >= yB) continue;
    if (rainbow) {
      for (int yy = y; yy < yB; yy += PX) {
        g.fillRect(x, yy, cw, min(PX, yB - yy),
                   rainbowTone((int)lroundf(cx - 6 + i), row));
        row++;
      }
    } else {
      float a = bright * edgeK;
      g.fillRect(x, y, cw, yB - y, eyeTone(a));
    }
  }
}

// ---------------- 像素位图 ----------------
static const uint8_t HEART_BITS[7] = {
  0b0110110, 0b1111111, 0b1111111, 0b1111111, 0b0111110, 0b0011100, 0b0001000
};
// #ff7ca6 #ff4d7d #ffb3cc #ffd9e6 #ff8fb4 #5cff92 的 RGB565

static const uint8_t Z5_BITS[5] = {
  0b11111, 0b00010, 0b00100, 0b01000, 0b11111
};

// 困惑·问号的笔画点位（圆周采样 + 收尾竖线），运行时按弹簧整体缩放
static const float Q_PTS[34][2] = {
  {32.40f,4.66f},{32.41f,4.42f},{32.45f,4.18f},{32.53f,3.94f},{32.64f,3.73f},{32.78f,3.53f},
  {32.95f,3.35f},{33.14f,3.20f},{33.35f,3.07f},{33.58f,2.98f},{33.81f,2.92f},{34.06f,2.90f},
  {34.30f,2.91f},{34.54f,2.96f},{34.77f,3.04f},{34.99f,3.15f},{35.19f,3.29f},{35.36f,3.46f},
  {35.51f,3.65f},{35.63f,3.87f},{35.72f,4.09f},{35.78f,4.33f},{35.80f,4.57f},{35.79f,4.82f},
  {35.74f,5.06f},{35.66f,5.29f},{35.54f,5.50f},{35.42f,5.78f},{35.30f,6.06f},{35.18f,6.34f},
  {35.06f,6.61f},{34.94f,6.89f},{34.82f,7.17f},{34.70f,7.45f}
};
static const float Q_DOT_X = 34.70f, Q_DOT_Y = 9.05f;
static const float Q_AX = 34.10f, Q_AY = 8.75f;   // 缩放锚点（底部），弹起时往上长

// ---------------- 装饰 ----------------
struct HeartP {
  float birth, x0, y0, vy, amp, ph, fr, life;
  float exitA = -1, exitAge = -1;           // 离场时冻结的亮度 / 帧龄
  uint8_t unit, col;
};
static HeartP g_heart[MAX_HEART];
static int   g_heartN = 0;
static float g_heartNext = 0;
// 切走时不是清场，而是"停止撒心 + 停在半空 + 0.3s 淡出"
static float g_heartFade = 1;
static bool  g_heartOn = false;              // 只有真的进过「喜欢」才需要离场淡出

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

// 这颗心"因为自己寿命"该有多亮（不含离场淡出）
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
    // 回收：过期的直接移除（离场阶段不回收，交给淡出）
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

    // 离场时把上飘速度降到 1/5：让爱心停在半空慢慢暗下去，
    // 而不是继续飞、飞进眼睛背后被整块遮掉。
    float ae = p.exitAge >= 0 ? p.exitAge + (age - p.exitAge) * 0.20f : age;

    float y = p.y0 + p.vy * ae;
    if (y < -4 || y > 34) continue;
    float x = p.x0 + sinf(ae * p.fr + p.ph) * p.amp * 4;

    float a = (p.exitA >= 0 ? p.exitA : heartAlpha(p)) * fade;
    if (a < 0.02f) continue;

    int px = (int)lroundf(x * PX), py = (int)lroundf(y * PX);
    if (px < -4 * PX || px > SCR_W + 4 * PX) continue;

    // 亮度按 a 缩放（网页是 rgba 透明度，这里换成颜色深浅）
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

// 睡着·Zzz：三颗由大到小斜向上飘
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
    int ox = (int)lroundf(gx * PX), oy = (int)lroundf(gy * PX);   // 1px 精度，飘起来才连
    uint16_t c = iconTone(0.78f, a);           // Zzz：hsla(h,s%,78%,a)
    int u = max(1, (int)lroundf(un[i]));   // 网页是 3.2/2.7/2.3px，浏览器抗锯齿；这里取整成硬边
    for (int r = 0; r < 5; r++) {
      uint8_t bits = Z5_BITS[r];
      for (int q = 0; q < 5; q++)
        if (bits & (0x10 >> q)) g.fillRect(ox + q * u, oy + r * u, u, u, c);
    }
  }
}

// 困惑·问号：整只按欠阻尼弹簧缩放，一次弹出来
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

// 思考·三点：相位差 1/3 周期，依次抬起并变亮变大
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

// ---------------- v14 新表情的绘制部件 ----------------

/* 死鱼眼：两只 X。
   网页版 W_=6 / H_=11，逐行 2 格横条拼对角线 —— 逐点画会成虚线，横向要 2 格宽才连得上。
   先铺一块黑底：形状是空壳，不铺底会把上一帧的竖条余晖透出来。 */
static void drawDeadX(TFT_eSprite& g, float cx, float dx, float dy) {
  const int W_ = 6, H_ = 11;
  float x0 = cx - W_ * 0.5f + dx;
  float y0 = EYE_CY - H_ * 0.5f + dy;

  g.fillRect((int)lroundf((x0 - 1.2f) * PX), (int)lroundf((y0 - 1.6f) * PX),
             (int)lroundf(PX * (W_ + 2.4f)), (int)lroundf(PX * (H_ + 3.2f)), TFT_BLACK);

  uint16_t c = rgb565(0.588f, 0.588f, 0.588f);      // THEME.dead = #969696
  for (int r = 0; r < H_; r++) {
    float u  = (float)r / (H_ - 1);
    float xa = x0 + W_ * u;            // 左上 → 右下
    float xb = x0 + W_ * (1.0f - u);   // 右上 → 左下
    int yy = (int)lroundf((y0 + r) * PX);
    g.fillRect((int)lroundf((xa - 0.5f) * PX), yy, PX * 2, PX, c);
    g.fillRect((int)lroundf((xb - 0.5f) * PX), yy, PX * 2, PX, c);
  }
}

/* 被吓一跳右上角的感叹号：7 排渐窄矩形 + 一个独立方点。
   半宽 1.0 格 → 0.5 格（整宽 2.0 → 1.0）；排与排之间留 ≥1 格的口子，
   不然缩放后会连成一整根锥形棒；方点也必须跟条尾分开，否则成了"T 字"。
   缩放锚点 (ax, ay) 取最下沿：弹起时往上长，不往眼睛那边挤。 */
static void drawExcl(TFT_eSprite& g, float kx, float ky, float ox, float oy, float alpha) {
  if (alpha <= 0.02f || kx <= 0.03f || ky <= 0.03f) return;
  const float ax = 34.15f, ay = 9.80f;
  const int   N  = 7;
  const float y0 = 2.50f, y1 = 7.05f;

  uint16_t c = rgb565(1.000f * alpha, 0.894f * alpha, 0.471f * alpha);   // #ffe478
  for (int i = 0; i < N; i++) {
    float ra = y0 + (y1 - y0) * i / N;
    float rb = y0 + (y1 - y0) * (i + 1) / N;
    float hw = 1.00f - 0.50f * i / (N - 1);
    float X0 = ax - hw * kx + ox, X1 = ax + hw * kx + ox;
    float Y0 = ay + (ra - ay) * ky + oy, Y1 = ay + (rb - ay) * ky + oy;
    g.fillRect((int)lroundf(X0 * PX), (int)lroundf(Y0 * PX),
               max(1, (int)lroundf((X1 - X0) * PX)),
               max(1, (int)lroundf((Y1 - Y0) * PX) + 1), c);   // +1 盖住排间缝隙
  }
  // 下面那个点：一整块方点（横向不跟着缩，不然会缩成 1px 的点看不出是感叹号）
  float dX = ax + ox, dY = ay + (8.55f - ay) * ky + oy;
  int rw = max(2, (int)lroundf(PX * kx * 0.62f * 2.0f));
  g.fillRect((int)lroundf(dX * PX) - (rw >> 1), (int)lroundf(dY * PX) - (rw >> 1), rw, rw, c);
}

// ---------------- 渲染一帧 ----------------
static void faceRender() {
  TFT_eSprite& g = *g_spr;
  g.fillSprite(TFT_BLACK);

  // 过渡推进
  if (g_moodT < 1) g_moodT = min(1.0f, g_moodT + g_faceStepMs / g_moodDur);
  if (g_dirT  < 1) g_dirT  = min(1.0f, g_dirT  + g_faceStepMs / g_dirDur);

  /* ---- 一次性动作的自动收场 ----
     受惊 / 摇头 / 点头 都是"演完就回默认"，不是会一直挂着的状态。
     判定用 g_moodId（目标表情），跟网页版 moodName 那三段一一对应。
     收手时机都挑在曲线本身的零点上，切走不会掐断半下。 */
  if (g_moodId == M_STARTLE) {
    if (g_startleT < 0) g_startleT = g_faceT;
    else if (g_faceT - g_startleT >= 40) faceSetMood(M_NORMAL);
  } else if (g_moodId != M_NORMAL) {
    g_startleT = -1;
  } else if (g_startleT >= 0 && g_faceT - g_startleT > 70) {
    g_startleT = -1;
  }

  bool isShakeMood = (g_moodId == M_NONO || g_moodId == M_NONOBLINK);
  if (isShakeMood) {
    if (g_nonoT < 0) g_nonoT = g_faceT;
    else if (g_faceT - g_nonoT >= 62) faceSetMood(M_NORMAL);   // sin(t*0.30) 的第 6 个零点
  } else if (g_moodId != M_NORMAL) {
    g_nonoT = -1;
  } else if (g_nonoT >= 0 && g_faceT - g_nonoT > 90) {
    g_nonoT = -1;
  }

  if (g_moodId == M_NOD) {
    if (g_nodT < 0) g_nodT = g_faceT;
    else if (g_faceT - g_nodT >= 32) faceSetMood(M_NORMAL);    // 两下点完（headNod 在 t>30 已归零）
  } else if (g_moodId != M_NORMAL) {
    g_nodT = -1;
  } else if (g_nodT >= 0 && g_faceT - g_nodT > 80) {
    g_nodT = -1;
  }

  LiveMood sh;
  liveMood(sh);

  // 思考：大小眼互换
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

  float dtF = g_faceStepMs * 60.0f / 1000.0f;

  /* 位移 = 单位向量 × 幅度（对齐网页：切表情改的是幅度、转视线改的是方向）。
     幅度单独平滑（0.84^dt，约 0.3s 到位），所以从惊喜切到普通表情时眼睛是
     "慢慢往外走"，不是啪地跳出去；反过来也不会一下子撞到眼眶边。 */
  g_ampTgt = dirAmpFor(g_moodId);
  g_ampCur += (g_ampTgt - g_ampCur) * (1.0f - powf(0.84f, dtF));
  float ux = g_dirCur[0] + (g_dirTgt[0] - g_dirCur[0]) * easeC(g_dirT);
  float uy = g_dirCur[1] + (g_dirTgt[1] - g_dirCur[1]) * easeC(g_dirT);
  float ddx = ux * g_ampCur;
  float ddy = uy * g_ampCur;

  // 偷看：整体缓慢横移，探出→缩回→再探出→停→换边
  // 以前偏移是直接加到 ex 上的，相位落在哪就从哪起跳，从别的表情切进来会瞬移；
  // 切走时又会"啪"地弹回中间。现在偏移单独存成 g_peekOff，目标由相位算、
  // 实际值按时间常数平滑跟随，进出都是连续的。
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

  /* 摇头：横向摆动，整个头在摆而不是"眼睛在看某处"。
     曲线 3.6·e^(-t/38)·min(1,t/5)·sin(0.30t)：起手 5 帧渐入（不然第一下是硬切），
     之后指数衰减，约 62 帧（1.04s）走完三个来回。
     同时用 g_shakeMix 把"视线偏移"压掉 —— 不压的话，眼睛是斜着甩，
     看着像在瞟，不像摇头。0.45 取刻意快：3 帧压掉 91%，67ms 就归中。 */
  float swingX = 0;
  if (isShakeMood && g_nonoT >= 0) {
    float t = g_faceT - g_nonoT;
    swingX = 3.6f * expf(-t / 38.0f) * min(1.0f, t / 5.0f) * sinf(t * 0.30f);
  }
  g_shakeMix += ((isShakeMood ? 1.0f : 0.0f) - g_shakeMix) * (1.0f - powf(0.45f, dtF));
  float baseEx = ddx + g_peekOff;                 // 表情自带的视线
  float ex = (baseEx + neckEyeOffset(baseEx)) * (1.0f - g_shakeMix) + swingX;

  // 自动眨眼
  // 闭眼(blink)、睡着(sleep)、摇头 本身就是一条横线/一直在甩，再叠眨眼很别扭。
  bool eyesShut = (g_moodId == M_BLINK || g_moodId == M_SLEEP || isShakeMood);
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
    // 退出闭眼表情时重新掷计时，避免一睁眼立刻补眨一下
    if (eyesShut) g_nextBlink = 150 + (int)(rnd01() * 210);
  }

  /* 亮度恒定。原来这里有个"呼吸"（0.84~1.00 慢正弦，1.6 秒一轮），
     在 1.8 寸屏上看着像屏幕在闪，已经去掉（网页 v14.4 同步）。 */
  float bright = 1.0f;

  /* ---- 抖动 ----
     晕（DECOR_SHAKE）：0.9 格，规则小幅晃。生气二级沿用同一档。 */
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
  /* wink = 持续闭左眼的状态，不是"眨一下"。
     目标只看 g_moodId（目标表情），不看 sh.decor（插值中的表情）——
     decor 要过半才翻面，用它的话切走那半程左眼还死死闭着，翻面那一帧才弹开，就是硬切。
     0.88^dt 指数逼近：60fps 下约 15 帧（0.25s）走完 85%。
     切进来是上眼皮一路盖下来，切走是一路抬起来，两头都不跳格。 */
  {
    float winkGoal = (g_moodId == M_WINK) ? 1.0f : 0.0f;
    g_winkHold += (winkGoal - g_winkHold) * (1.0f - powf(WINK_TAU, dtF));
    if (g_winkHold < 0.004f) g_winkHold = 0;
    winkL = g_winkHold;                       // 只闭左眼（WINK 的语义就是单眼）
  }

  EyeShape L, R;
  blinkShape(sh.L, min(1.0f, sqL + winkL), L);
  blinkShape(sh.R, min(1.0f, sqR + winkR), R);

  // ---- 爱心先画，压在最底层 ----
  // 判定用 g_moodId（目标表情）而不是 sh.decor（插值中的表情）：
  // decor 是"过半才翻面"的，用它会让前半程还满亮度飘、后半程才开始淡，中间有个断点。
  if (g_moodId == M_LOVE && faceCfg.heart) {
    g_heartOn = true;
    g_heartFade = min(1.0f, g_heartFade + dtF * 0.060f);          // 淡入 ≈ 0.28s
    for (int i = 0; i < g_heartN; i++) { g_heart[i].exitA = -1; g_heart[i].exitAge = -1; }
    updateHearts(dtF, true);
  } else if (g_heartOn && g_heartN > 0) {
    g_heartFade = max(0.0f, g_heartFade - dtF * 0.055f);          // 淡出 ≈ 0.30s
    for (int i = 0; i < g_heartN; i++) {
      if (g_heart[i].exitA < 0) {                                 // 离场第一帧：把亮度钉住
        float own = heartAlpha(g_heart[i]);
        if (own > 0.01f) { g_heart[i].exitA = max(own, 0.45f); g_heart[i].exitAge = g_faceT - g_heart[i].birth; }
      }
    }
  }
  if (g_heartOn && g_heartN > 0) {
    drawHearts(g, g_heartFade);
    if (g_heartFade <= 0) { g_heartN = 0; g_heartOn = false; }    // 淡干净了才真正回收
  }

  /* ---- 点头：只改纵向位移，不改眼形 ----
     读的是"脖子在动"，跟摇头（改横向 ex）是同一套思路的竖版。
     曲线是"半个正弦"而不是连续 sin：连续 sin 会正负交替，眼睛上下抖四五次，
     那是打嗝不是点头。一下 = sin(kπ)，首尾精确落在 0；15 帧一下、共两下，
     第二下力度 0.72（真人点头第二下都会轻一点）。 */
  float nodY = 0;
  if (g_moodId == M_NOD && g_nodT >= 0) {
    float t = g_faceT - g_nodT;
    if (t <= 30.0f) {
      float k = fmodf(t, 15.0f) / 15.0f;
      nodY = 3.4f * sinf(k * (float)M_PI) * (t < 15.0f ? 1.0f : 0.72f);
    }
  }

  /* ---- 被吓一跳：果冻拉长（66 帧时间轴）----
       p  0~10  猛拉长（easeOutBack，冲到 1 再收住）
       p 10~40  保持超长 —— 这 0.5 秒是"瞪着你"的时间
       p 40~50  快速收回（同时 g_moodId 已回 normal，形状 26→14 格一起缩）
       p 50~57  继续压到 -0.45：比默认还扁，这是"橡皮筋弹回来"的关键
       p 57~66  弹回 0
     sy=1.22 → 26×1.22 = 31.7 格（屏幕 32 格，刚好填满不截断；
                再高就顶出框，两端被切平，反而像断掉的荧光管）
     kw=0.80 → 纵向拉长必须横向收窄，否则只是"纵向放大"，不像被拽。 */
  float jbKW = 1.0f, jbY = 0.0f;
  if (sh.decor == DECOR_JBLOB && g_startleT >= 0) {
    float p = g_faceT - g_startleT;
    float inf;
    if (p < 10.0f) {
      float k = p / 10.0f, c1 = 1.3f, c3 = c1 + 1.0f;
      inf = 1.0f + c3 * powf(k - 1, 3) + c1 * powf(k - 1, 2);
    } else if (p < 40.0f) {
      inf = 1.0f;
    } else if (p < 50.0f) {
      float k = (p - 40.0f) / 10.0f; inf = 1.0f - k * k * (3.0f - 2.0f * k);
    } else if (p < 57.0f) {
      float k = (p - 50.0f) / 7.0f;  inf = -0.45f * k * k * (3.0f - 2.0f * k);
    } else if (p < 66.0f) {
      float k = (p - 57.0f) / 9.0f;  inf = -0.45f * (1.0f - k) * (1.0f - k);
    } else {
      inf = 0.0f;
    }
    float sy = 1.0f + 0.22f * inf;
    jbKW = 1.0f - 0.20f * inf;
    jbY  = -1.1f * inf;
    for (int i = 0; i < EYE_COLS; i++) {
      L.top[i] *= sy; L.bot[i] *= sy;
      R.top[i] *= sy; R.bot[i] *= sy;
    }
  }

  /* ---- 故障：横条错位 ----
     不是"晕"那种整体小幅晃，是画面本身被切断重接。
     90 帧（1.5s）一轮里炸三段（0~10% / 38~46% / 74~80%）—— 一次炸太久像在抖，
     短促几下才像"接触不良"。每 2 帧才换一次切片：每帧都换的话错位全糊成噪点，
     "横条"这个结构就读不出来了。 */
  float glitchAmp = 0;
  if (sh.decor == DECOR_GLITCH) {
    float ph = fmodf(g_faceT, 90.0f) / 90.0f;
    bool burst = ph < 0.10f || (ph > 0.38f && ph < 0.46f) || (ph > 0.74f && ph < 0.80f);
    if (burst) { g_glitchSeed = floorf(fmodf(g_faceT, 90.0f) * 0.5f); glitchAmp = 1.0f; }
  }

  bool rb = (sh.decor == DECOR_RAINBOW && g_moodT > 0.35f);
  float eyeDY = ddy + sh.dy + jy + jbY + nodY;
  drawEye(g, L, EYE_CL + sh.lx, ex + sh.dx + jx, eyeDY + sh.ldy, bright, rb, jbKW, glitchAmp);
  drawEye(g, R, EYE_CR + sh.rx, ex + sh.dx + jx, eyeDY + sh.rdy, bright, rb, jbKW, glitchAmp);

  // ---- 故障 · 信号溢出：3 道白线 ----
  if (glitchAmp > 0) {
    uint16_t wc = rgb565(0.90f * 0.55f, 1.0f * 0.55f, 0.94f * 0.55f);
    for (int i = 0; i < 3; i++) {
      float h = (sinf(g_glitchSeed * 11.0f + i * 3.7f) * 43758.5f);
      h = h - floorf(h);                                   // 伪随机 0~1
      int ly = (int)lroundf(h * SCR_H / PX) * PX;
      g.fillRect(0, ly, SCR_W, PX, wc);
    }
  }

  /* ---- 死鱼眼：形状是空壳，drawEye 一个像素都没画，这里把两只 X 补上 ----
     位移跟正常眼睛完全同一套，所以它照样能被时钟方向指挥、被云台带着瞟。 */
  if (sh.decor == DECOR_DEAD) {
    drawDeadX(g, EYE_CL + sh.lx, ex + sh.dx + jx, eyeDY + sh.ldy);
    drawDeadX(g, EYE_CR + sh.rx, ex + sh.dx + jx, eyeDY + sh.rdy);
  }

  /* ---- 被吓一跳 · 右上角感叹号 ----
     符号比眼睛晚半拍冒出来（decor 在过渡到一半才上线），这个"晚"是对的：
     先被吓，才看见"!"。
     生命周期：弹簧走完（约 22 帧）后再停 6 帧，然后用 12 帧退场，共约 0.6 秒。
     它是那一下的爆点，不是状态，必须自己退场 —— 一直挂着像画面卡住了。
     眼睛该一直保持受惊形状，符号不该。 */
  if (sh.decor == DECOR_JBLOB) {
    if (g_exEnter < 0) g_exEnter = g_faceT;
    float t = max(0.0f, g_faceT - g_exEnter);
    float out = (g_moodId == M_STARTLE)
                ? min(1.0f, max(0.0f, (34.0f - t) / 12.0f))
                : max(0.0f, 1.0f - g_moodT * 2.2f);
    if (out > 0.01f) {
      const float WN = 26.0f / 60.0f, ZT = 0.42f;
      float WD = WN * sqrtf(1.0f - ZT * ZT), DZ = ZT * WN;
      float k = 1.0f - expf(-DZ * t) * (cosf(WD * t) + (DZ / WD) * sinf(WD * t));
      float jj = (k - 1.0f) * 0.30f;                       // 纵向拉长、横向收窄，才像"惊"
      float kx = max(0.0f, k * (1.0f - jj)), ky = max(0.0f, k * (1.0f + jj));
      // 落稳后一次快速左右抖，像被人拍了一下还在晃
      float wob = (t > 12.0f) ? 0.55f * expf(-(t - 12.0f) / 9.0f) * sinf((t - 12.0f) * 0.95f) : 0.0f;
      drawExcl(g, kx, ky, wob + ex * 0.18f + sh.dx * 0.5f, ddy * 0.20f + sh.dy * 0.4f, out);
    }
  } else {
    g_exEnter = -1;                       // 离开就复位，下次进来重新计时
  }

  // ---- 泪滴 ----
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
        g.fillRect((int)lroundf((cx + ddx) * PX), (int)lroundf(y * PX),
                   PX * 2, PX * 3, c);
      }
    }
  }

  // ---- 害羞腮红 ----
  if (sh.decor == DECOR_SHY) {
    uint16_t c = rgb565(theme.blushR * 0.85f, theme.blushG * 0.85f, theme.blushB * 0.85f);
    for (int e = 0; e < 2; e++) {
      float cx = (e == 0 ? EYE_CL - 3.8f : EYE_CR + 0.8f);
      g.fillRect((int)lroundf((cx + ex) * PX),
                 (int)lroundf((EYE_CY + 7.6f + sh.dy) * PX), PX * 3, PX, c);
    }
  }

  // ---- Zzz ----
  if (sh.decor == DECOR_ZZZ && faceCfg.zzz) drawZzz(g, ddx, ddy, sh);

  // ---- 汗滴 ----
  if (sh.decor == DECOR_SWEAT && faceCfg.sweat) {
    float cyc = fmodf(g_faceT, 150.0f) / 150.0f;
    float bx = EYE_CR + 5 + ddx;
    float by = EYE_CY - 8 + cyc * 5;
    float a = cyc < 0.75f ? 0.95f : 0.95f * (1 - (cyc - 0.75f) / 0.25f);
    uint16_t c = rgb565(theme.sweatR * a, theme.sweatG * a, theme.sweatB * a);
    g.fillRect((int)lroundf(bx * PX), (int)lroundf(by * PX), PX, PX, c);
    g.fillRect((int)lroundf((bx - 1) * PX), (int)lroundf((by + 1) * PX), PX * 3, PX * 2, c);
  }

  // ---- 困惑 · 问号 ----
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

  // ---- 思考 · 三点 ----
  if (sh.decor == DECOR_SWAP && faceCfg.dots) {
    float outA = (g_moodId == M_THINK) ? 1.0f : max(0.0f, 1 - g_moodT * 2.2f);
    if (outA > 0) drawDots(g, ddx * 0.42f, ddy * 0.28f + sinf(g_faceT / 53.0f) * 0.16f, outA);
  }
}

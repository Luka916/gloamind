// ===========================================================
//  pet_eye.ino  —  桌宠像素眼睛 · ESP32 烧录版
//
//  硬件：ESP32-S3 + ST7735S 1.8" (128x160, 8pin SPI)
//  接线：SCK=12  MOSI=11  CS=10  DC=9  RES=8  BLK=7
//  内存：一帧 160x128x2 = 40KB，全部放内部 RAM
//
//  串口（115200）调试命令：
//    m <名字|序号>   切表情   例：m love / m 11
//    d <0-12>        切视线   时钟十二向：0 中 1 十二点 2 一点 3 两点 4 三点
//                             5 四点 6 五点 7 六点 8 七点 9 八点 10 九点 11 十点 12 十一点
//    b <0|1>         自动眨眼开关
//    h <0-360>       色相     默认 146（青绿）
//    l <0.4-1.3>     亮度
//    i               打印当前状态
//    ?               帮助
// ===========================================================
#include <Esp.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "pet_face.h"
#include "../../shared/servo_drv.h"
#include <FFat.h>   /* 10MB ffat 分区（0x610000，0x9E0000）。fs_probe 已实测可挂载 */
#include <Preferences.h>  /* 蓝牙禁用标记改存 NVS：这个板子的 FAT 分区现在写不进去 */
#include <WiFi.h>   /* 只用来做"射频体检"：扫一次 AP 就知道天线和前端好不好 */

/* ---------------- 蓝牙（BLE）----------------
   用 Nordic UART Service（NUS）——这不是我发明的协议，是行业里"蓝牙串口"
   的通用壳子，UUID 全世界统一。好处是手机上随便一个蓝牙串口 App
   （Serial Bluetooth Terminal / nRF Connect / LightBlue）连上就能发文本命令，
   跟电脑上串口敲的一模一样，不用我另写一个 App。

   为什么命令要"过一道队列"而不是直接在蓝牙回调里改表情：
   蓝牙回调跑在另一个任务（另一个线程）里，跟渲染任务同时动 g_moodId /
   g_cur / g_tgt 会打架 —— 表现是偶尔闪一下怪形状，或者卡在半截。
   所以回调只把字节塞进环缓冲，主循环再拿出来当普通命令执行，全程单线程。 */
#define BLE_NAME    "PetEye"
#define NUS_SERVICE "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   // 手机 → 板子
#define NUS_TX      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // 板子 → 手机（通知）

#define RING_SZ 256
static char            g_ring[RING_SZ];
static volatile uint16_t g_rHead = 0, g_rTail = 0;
static portMUX_TYPE    g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool   g_bleConnected = false;
static volatile bool   g_bleJustIn    = false;   // 刚连上（主循环处理完清零）
static volatile bool   g_bleJustOut   = false;   // 刚断开
static volatile bool   g_bleReset     = false;   // 连接状态变了，丢弃残留半条命令
static volatile uint32_t g_bleRxMs    = 0;       // 最后一次收到字节的时刻（用于无换行的命令补刀）
static BLECharacteristic *g_txChar = nullptr;

static String g_reply;   // 本次命令要回给手机的话

// ---------------- 引脚 ----------------
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_CS    10
#define PIN_DC     9
#define PIN_RST    8
#define PIN_BLK    7
#define PIN_NECK  14        // 云台舵机信号（黄线）· 水平 PAN
#define PIN_TILT  18        // 云台舵机信号（黄线）· 竖直 TILT
#define PIN_TOUCH 13        // 头顶触摸板 OUT（TTP223，3V3 供电，高有效）
#define PIN_TOUCH2 17       // 下巴触摸板 OUT（第二个 TTP223）
                            /* 为什么是 17：18 是 TILT 舵机，13 已给头顶，
                               17 就在 18 上面一根，空着且不是启动脚。 */

/* 启动保护变量（必须放在 setup 与命令处理之前，否则 C++ 报未声明） */
/* ==================== 启动保护（防"蓝牙崩溃 → 无限重启"） ====================
   背景：2026-11-15 晚，带蓝牙的固件在初始化蓝牙时崩溃，板子每 1.5 秒重启一次。
   因为崩在 setup 早期，屏幕反复白屏黑屏、USB 反复掉线，最后连新固件都烧不进去。

   这层保护的原理：RTC 内存（RTC_NOINIT_ATTR）在软复位时**不清零**。
   每次启动先 +1；只有蓝牙顺利起来才清零。
   如果连续 3 次都没走到"蓝牙起来"那一步，就认定这次启动有毒，
   直接跳过蓝牙，让眼睛和舵机先活下来 —— 用户永远能看见一双眼睛，而不是白屏。
   串口发 K 可以清计数并重启，重新试一次。

   这条保护比"查清 bug"更重要：先保证不把用户的东西搞成砖。
   ========================================================================== */
RTC_NOINIT_ATTR static uint32_t g_bootMagic;
RTC_NOINIT_ATTR static uint32_t g_bootCount;
/* 上次蓝牙初始化走到第几步。这个变量是整个诊断的核心：
   崩溃时串口会掉线、最后几条日志会丢，但 RTC 内存不会丢。
   下次启动把它打出来，就知道上次死在 1~9 的哪一步 —— 不依赖崩溃瞬间的日志。 */
RTC_NOINIT_ATTR static uint32_t g_bleStep;
#define BOOT_MAGIC 0xB1E5AFE1u
static bool g_bleSkipped = false;

/* ---- "蓝牙起不来"标记：存在 NVS 里，不存 FAT ----
   2026-11-16 换的。原因：这块板子的 FAT 分区现在一个文件都建不了
   （能挂上、10MB 全空、格式化也失败），写进去的 flag 等于没写。
   NVS 相反：连着崩了十几次、复位十几次，ble_lean 的计数一次都没丢。
   结论：这种"断电也不能丢"的小标记，NVS 比 FAT 可靠得多。 */
static bool nvsNobleFlag() {
  Preferences p;
  if (!p.begin("peteye", true)) return false;   // true = 只读
  bool v = (p.getUChar("noble", 0) == 1);
  p.end();
  return v;
}
static void nvsNobleSet(uint8_t v) {
  Preferences p;
  if (!p.begin("peteye", false)) return;
  p.putUChar("noble", v);
  p.end();
}

/* ---- 启动尝试次数：RTC 为主，NVS 兜底 ----
   2026-11-16 加的补丁。原来的计数只存在 RTC_NOINIT 里，指望"软复位不清零"。
   但这块板子的 RTC 内存**在崩溃复位时会被随机清掉**（实测出现过 magic 对不上、
   计数归零），于是一旦蓝牙每次都在 enable 里崩，"连崩 3 次就保护"这条永远触发
   不了 —— 串口发一次 K 就可能变成无限白屏重启。
   NVS 相反：连崩十几次、复位十几次，一个字节都没丢。所以让它兜底。 */
static uint8_t nvsTryGet() {
  Preferences p;
  if (!p.begin("peteye", true)) return 0;
  uint8_t v = p.getUChar("tryn", 0);
  p.end();
  return v;
}
static void nvsTrySet(uint8_t v) {
  Preferences p;
  if (!p.begin("peteye", false)) return;
  p.putUChar("tryn", v);
  p.end();
}
                            /* 为什么是 18 而不是 13：PINOUT.md 里 13 是 TB6612 的 BIN2
                               （底盘右轮方向），18 才是留给 TILT 的。13 现在空着，
                               但一旦接上底盘就撞车，别图省事。 */

/* 脖子（云台）角度。范围先卡保守一点：30~150，中间 90 是正前方。
   机械行程还没标定，宁可在里头活动，也别让舵机顶着壳一直较劲（会发热）。 */
#define NECK_MIN  30
#define NECK_MAX  150
#define NECK_HOME 90        /* PAN 还没标定，平视暂定 90。标完要改这里 */
static float g_neckPos = 90.0f;   // 实际正在输出的角
static float g_neckTgt = 90.0f;   // 想去的角
static uint32_t g_nextNeck = 0;   // 下次"东张西望"的时刻

/* 竖直轴（TILT）
   ★ 2026-11-15 21:45 真机标定结果：抬头 23 / 平视 62 / 低头 118，行程 95°。
   ★ 方向反直觉：**数值越大越低头**，平视落在 62 而不是 90 ——
     这是舵机臂装配角度决定的，别按"90 一定是水平"去想。
   ★ 这三个数必须跟 /servo.cfg 里的值保持一致：
     万一 FAT 挂了、或者用户敲了 c d 删文件，就会回落到这里。
     如果这里还写着老的 62/90/118，删文件之后会变成
     "抬不了头（下限被卡在平视点）+ 默认低头 28°" 的怪状态。 */
#define TILT_MIN   23
#define TILT_MAX  118
#define TILT_HOME  62
#define TILT_DOWN_SIGN  (+1)
  /* 低头方向。装法不同舵机转向相反，第一次试的时候如果"点头"变成"抬头"，
     把这里改成 (-1) 就行，不用动别的地方。2026-11-15 实测 +1 是对的。 */
static float g_tiltPos = 90.0f;
static float g_tiltTgt = 90.0f;
static uint32_t g_greetAt = 0;    // 蓝牙连上后"点头→笑"的换脸时刻（0 = 没在打招呼）

/* ================= /servo.cfg：把限位从源码搬进文件 =================
   为什么要有这一层：
     03-舵机标定.md 量出 lo/home/hi 之后，改的是 NECK_MIN / TILT_MIN 这类
     #define —— 必须重烧一次才生效，而且重烧就得插电脑、开 arduino-cli。
     桌上调试时这个循环太慢：量 → 记下来 → 拔线 → 重烧 → 再看，一轮五分钟。

   加了这层之后，"改限位"变成手机上一行命令：
       c p 42 92 138     改 PAN
       c t 66 90 112     改 TILT
       c s               存盘（不存 = 只活到这次断电）
       c d               删文件，回落编译默认值

   为什么用 FAT 不用 NVS：
     · 这 10MB 分区本来就闲着（fs_probe 已验证可挂载可写），NVS 只有 20KB，
       还跟 BLE/WiFi 的键值挤在一起
     · 以后音效、开机图、日志都能直接扔进同一个分区
     · 配置文件是人能读的文本，用 `c` 打完就能看，不用反编译

   安全底线：文件内容一律先过 cfgSanity()（0~180、lo<home<hi、行程≥20°）。
   任何一条不合格 → 整份文件作废、回落编译默认值。
   绝不允许"只读到一半"就把舵机放到一个可能顶壳的角上。 */
static int  g_panLo  = NECK_MIN,  g_panHi  = NECK_MAX,  g_panHome  = NECK_HOME;
static int  g_tiltLo = TILT_MIN,  g_tiltHi = TILT_MAX,  g_tiltHome = TILT_HOME;
static bool g_cfgMounted = false;
static const char *g_cfgSrc = "编译默认值";

/* 一组限位是否物理上讲得通。行程 < 20° 基本只有两种可能：
   量错脚了，或者量的时候舵机没上力 —— 两种都不该被当成标定结果。 */
static bool cfgSanity(int lo, int home, int hi) {
  if (lo < 0 || hi > 180) return false;
  if (!(lo < home && home < hi)) return false;
  if (hi - lo < 20) return false;
  return true;
}

/* 开机载入。三个出口都必须能回到"能用"的状态：
   挂不上 → 用编译默认值继续跑（屏幕/蓝牙完全不受影响）；
   没文件 → 同上；
   文件有错 → 整份作废 + 明确打印，不静默。 */
static void cfgLoad() {
  unsigned long t0 = millis();
  g_cfgMounted = FFat.begin(false);
  if (!g_cfgMounted) {
    /* 只有"从没格式化过"（换板 / 整片擦除）才会走到这里。
       格式化一次自愈，以后每次都是几十毫秒的普通挂载。 */
    g_cfgMounted = FFat.begin(true);
    if (g_cfgMounted) g_cfgSrc = "新格式化(空)";
  }
  unsigned long dt = millis() - t0;

  if (!g_cfgMounted) {
    Serial.printf("cfg: FAT 挂载失败（%lu ms），用编译默认值\n", dt);
    return;
  }
  File f = FFat.open("/servo.cfg", FILE_READ);
  if (!f) {
    Serial.printf("cfg: 挂载 ok %lu ms，无 /servo.cfg，用默认值 PAN %d/%d/%d TILT %d/%d/%d\n",
                  dt, g_panLo, g_panHome, g_panHi, g_tiltLo, g_tiltHome, g_tiltHi);
    return;
  }

  int plo = g_panLo, pho = g_panHome, phi = g_panHi;
  int tlo = g_tiltLo, tho = g_tiltHome, thi = g_tiltHi;
  while (f.available()) {
    String ln = f.readStringUntil('\n');
    ln.trim();
    if (!ln.length() || ln[0] == '#') continue;
    int eq = ln.indexOf('=');
    if (eq < 0) continue;
    String k = ln.substring(0, eq); k.trim();
    int v = ln.substring(eq + 1).toInt();
    if      (k == "pan_lo")    plo = v;
    else if (k == "pan_home")  pho = v;
    else if (k == "pan_hi")    phi = v;
    else if (k == "tilt_lo")   tlo = v;
    else if (k == "tilt_home") tho = v;
    else if (k == "tilt_hi")   thi = v;
    /* 不认识的键直接忽略 —— 以后加 field 时老固件不会因此报错 */
  }
  f.close();

  if (!cfgSanity(plo, pho, phi) || !cfgSanity(tlo, tho, thi)) {
    Serial.printf("cfg: 文件不合格（要 0~180、lo<home<hi、行程>=20）已作废；"
                  "读到 PAN %d/%d/%d TILT %d/%d/%d\n", plo, pho, phi, tlo, tho, thi);
    g_cfgSrc = "文件有错·已作废";
    return;
  }
  g_panLo = plo;  g_panHome = pho;  g_panHi = phi;
  g_tiltLo = tlo; g_tiltHome = tho; g_tiltHi = thi;
  g_cfgSrc = "/servo.cfg";
  Serial.printf("cfg: 载入 %lu ms  PAN %d/%d/%d  TILT %d/%d/%d\n",
                dt, g_panLo, g_panHome, g_panHi, g_tiltLo, g_tiltHome, g_tiltHi);
}

/* 存盘。只在 c s 时写 —— 不在每次改数时写。
   Flash 擦写寿命有限，而"调参数"是一次几十行的连续动作，
   每行都落盘等于拿寿命换方便，不划算。 */
static bool cfgSave() {
  if (!g_cfgMounted) return false;
  File f = FFat.open("/servo.cfg", FILE_WRITE);   /* FILE_WRITE 会先截断 */
  if (!f) return false;
  f.printf("# pet_eye 舵机限位（物理角 0~180，lo<home<hi）\n");
  f.printf("# 由 c s 写出；手改后要 c r 重读或重启才生效\n");
  f.printf("pan_lo=%d\npan_home=%d\npan_hi=%d\n", g_panLo, g_panHome, g_panHi);
  f.printf("tilt_lo=%d\ntilt_home=%d\ntilt_hi=%d\n", g_tiltLo, g_tiltHome, g_tiltHi);
  f.close();
  return true;
}

// 帧率：40fps 稳，60 也跑得动但 SPI 会吃满（屏到后配合 40MHz 再提到 60）
#define TARGET_FPS 60

TFT_eSPI tft;
TFT_eSprite spr(&tft);

static uint32_t lastFrameMs = 0;
static uint32_t fpsCount = 0, fpsLast = 0;
static uint32_t fpsShown = 0;   // 上一秒的实际帧数（临时诊断用）

// 自动行为（桌宠要有活物感；串口一发命令就交还手动）
static bool     g_auto     = true;
static bool     g_showFps  = true;   // 左上角帧率字样，f 0 可关（正式摆桌上时关掉）
static uint32_t g_nextMood = 0;
static uint32_t g_nextDir  = 0;

/* ---- 困 / 睡 三级递进 ---- */
static uint32_t g_lastPet     = 0;    // 最后一次互动（被摸 / 收到命令）
static uint8_t  g_drowseBlink = 0;    // 打瞌睡时已经合眼几次
static bool     g_asleep      = false;

/* ---- 摸太多次会烦（数字出处见下面 petTally） ---- */
static uint8_t  g_petCount = 0;
static uint32_t g_petWin   = 0;

/* ==================== 表情分三层 ====================
   2026-11-16 15:05 重排。原来"能待住的"全塞进一个常驻池随机抽，于是彩虹眼、
   害羞、哭、生气每十几秒就来一次 —— 常见了就没有信息量，而且会盖掉
   "被摸才有反应"这条因果。成品应该安静，情绪要有由来。

   第 I 类 常态（IDLE_POOL）——没人理的时候自己演的，只许无聊的。
       2026-11-16 16:20 按用户要求扩到 9 个：加入开心、得意、轻视、闭眼、大眼睛。
       「轻视」和「生气」是同一副眼睛的两个版本（生气左右镜像成 V，轻视把右眼
       翻成和左眼一样，两只同向斜），所以它是情绪而不是动作，可以常驻。
       「闭眼」不进眨眼层，就是合上歇一会儿 —— 它也是睡着的前一步，见 autoMood。
   第 II 类 状态——**只能被事件触发**：触摸 / 串口命令 / Agent 说话 / 长时间没人理。
       永不随机出现。这是"跟它互动它才给表情"。
   第 III 类 彩蛋——不进任何池子，靠隐藏手势触发，见 handleTouch 末尾。
       故意不写进帮助、不写进文档：要自己玩出来才算彩蛋。 */
static const uint8_t IDLE_POOL[] = {
  M_NORMAL, M_NORMAL, M_NORMAL, M_NORMAL, M_NORMAL,   // 约 33%：大部分时间就是它
  M_HAPPY,  M_HAPPY,                                  // 开心
  M_BLINK,  M_BLINK,                                  // 闭眼：合上歇一会儿
  M_SMUG,                                             // 得意
  M_DESPISE,                                          // 轻视
  M_WINK,                                             // 单眼眨
  M_PEEK,                                             // 偷看
  M_THINK,                                            // 发呆
  M_SURPRISE,                                         // 大眼睛
};
/* 半常态：既不是"没事就在那儿"，也不是"必须有事件"。
   太久没人理，会有点无语 / 有点困惑；以后 Agent 也能直接调。概率压得很低。 */
static const uint8_t IDLE_RARE[] = { M_SPEECHLESS, M_CONFUSED };
/* 事件池：一下就走、看完自己回 normal 的。属于 II 类，但可以由 idle 偶发取用，
   因为它们自带回位、不会停在屏幕上变成"卡住的怪表情"。 */
static const uint8_t EVENT_POOL[] = { M_NOD, M_NONO, M_STARTLE, M_ANGRY2 };

// ---------------- 命令通道 ----------------
/* 一句话回给"当前说话的人"：串口照常打印，蓝牙顺手攒进 g_reply 一起发回去。
   这样一条命令的代码只写一遍，两个通道都有反馈。 */
static void say(const char *fmt, ...) {
  char b[128];
  va_list ap; va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  Serial.print(b);
  if (g_reply.length() < 700) g_reply += b;   // 蓝牙会分块发，见 bleNotifyReply
}

static void printHelp() {
  say("---- pet_eye ----\n");
  say("m <name|idx>  切表情\n");
  say("d <0-12>      切视线（时钟十二向，0 中 / 1 十二点 / 4 三点 …）\n");
  say("b <0|1>       自动眨眼\n");
  say("h <0-360>     色相\n");
  say("l <0.4-1.3>   亮度\n");
  say("p <%d-%d>   转头（舵机真转，眼睛跟着瞟）\n", g_panLo, g_panHi);
  say("t <%d-%d>   抬头/低头（%d = 平视）\n", g_tiltLo, g_tiltHi, g_tiltHome);
  say("p +1 / t -1  相对微调一格（标定行程用，不受软限位挡）\n");
  say("c            看限位（FAT 配置文件状态）\n");
  say("c p|t <lo> <home> <hi>   改限位，立刻生效\n");
  say("c s 存盘   c r 重读   c d 删文件回默认\n");
  say("z             松两个舵机（不使力，手能拨）\n");
  say("f <0|1>       左上角帧率字样开关\n");
  say("a <0|1>       自动模式开关（自己演 / 只听你的）\n");
  say("K            清启动计数并重启（蓝牙被保护跳过时用）\n");
  say("w            射频体检：扫一次 WiFi（能扫到=射频好，蓝牙只是软件问题）\n");
  say("             触摸：头顶 GPIO13（看你→抬头→笑）· 下巴 GPIO17（缩一下→歪头→眯眼）\n");
  say("             同一次连续摸超过 4/8/12 下会依次不耐烦→生气→更生气，停手 25 秒消气\n");
  say("i             状态\n");
  for (int i = 0; i < MOOD_COUNT; i++) say("  %2d %s\n", i, MOOD_NAMES[i]);
}

static int findMood(const String& s) {
  for (int i = 0; i < MOOD_COUNT; i++)
    if (s.equalsIgnoreCase(MOOD_NAMES[i])) return i;
  /* 只有"全是数字"才当序号解析。
     原来的写法直接用 s.toInt()，而 toInt() 对 "zzz" 返回 0 ——
     于是打错表情名会静默变成 normal，看着像"命令生效了但表情没变"，
     很难查。现在打错会明确回 unknown mood。 */
  if (s.length() == 0) return -1;
  for (unsigned i = 0; i < s.length(); i++)
    if (!isDigit(s[i])) return -1;
  int n = s.toInt();
  if (n >= 0 && n < MOOD_COUNT) return n;
  return -1;
}

/* 串口和蓝牙最终都走这里。返回值没用，反馈全靠 say()。 */
static bool     g_touchLast  = false;
static uint32_t g_touchCool  = 0;
static uint32_t g_touchAt    = 0;

static bool     g_touch2Last = false;
static uint32_t g_touch2Cool = 0;
static uint32_t g_touch2At   = 0;

/* 摸一下的完整反应。抽出来是为了两件事：
   ① 头顶和下巴两套只差一个 head 参数，不用写两遍；
   ② 串口 `x` 命令能走同一条路 —— 不用真用手按也能验证/演示"摸太多次会烦"。 */
static void petReact(uint32_t now, bool head);

static void execCommand(String buf) {
  buf.trim();
  if (!buf.length()) return;
  char cmd = buf[0];
  String arg = buf.substring(1); arg.trim();

  /* 任何一条命令都算"有人在跟它说话"，但只读命令不算 ——
     否则我每隔几秒 `i` 查一次状态，它就永远睡不着了。 */
  bool readOnly = (cmd == '?' || cmd == 'i' || cmd == 'I' || cmd == 'x' || cmd == 'X'
                   || ((cmd == 'c' || cmd == 'C') && arg.length() == 0));
  if (!readOnly) {
    g_lastPet     = millis();
    g_drowseBlink = 0;
    if (g_asleep) { g_asleep = false; faceSetMood(M_STARTLE, 180.0f); }
  }

  /* 只有"真会改变画面"的命令才关自动模式。
     ? 和 i 是只读的 —— 以前敲个问号看帮助，桌宠就从此不动了，很莫名其妙。 */
  /* c 不带参数是纯查询，跟 ? / i 一样不该关掉自动模式；
     c p / c t / c s 会改状态，按"会改变画面"处理。 */
  if (!readOnly) g_auto = false;

  switch (cmd) {
    case 'm': case 'M': {
      int id = findMood(arg);
      if (id >= 0) { faceSetMood(id); say("mood -> %s\n", MOOD_NAMES[id]); }
      else say("unknown mood\n");
      break;
    }
    case 'x': case 'X': {
      /* 模拟摸头顶。不给参数就摸一下，`x 12` 连摸 12 下 ——
         用来验证"第 4 下不耐烦 / 第 8 下生气 / 第 12 下更生气"这条曲线，
         也用来给不摸它的人演示。 */
      int n = arg.length() ? constrain(arg.toInt(), 1, 30) : 1;
      bool wasAsleep = g_asleep;
      for (int i = 0; i < n; i++) {
        petReact(millis(), true);
        if (g_petCount == 4 || g_petCount == 8 || g_petCount == 12)
          say("  第 %u 下 -> %s\n", (unsigned)g_petCount, MOOD_NAMES[g_moodId]);
      }
      g_touchAt = millis();                    // 让"抬手"那套回落也照样跑
      say("pet x%d, 这一轮第 %u 下，当前 %s (wasAsleep=%d)\n",
          n, (unsigned)g_petCount, MOOD_NAMES[g_moodId], wasAsleep ? 1 : 0);
      break;
    }
    case 'd': case 'D': {
      int d = constrain(arg.toInt(), 0, 12);
      faceSetDir(d);
      say("dir -> %d\n", d);
      break;
    }
    case 'b': case 'B':
      faceCfg.blink = (arg.toInt() != 0);
      say("blink %s\n", faceCfg.blink ? "on" : "off");
      break;
    case 'h': case 'H':
      /* 卡在 0~360：超范围的色相在 hslTone 里会被隐式取模，
         算出来的颜色跟输入对不上，看着像"改色没反应"。 */
      faceCfg.hue = constrain(arg.toFloat(), 0.0f, 360.0f);
      faceSetHue(faceCfg.hue);
      say("hue %d\n", (int)faceCfg.hue);
      break;
    case 'l': case 'L':
      faceCfg.lum = constrain(arg.toFloat(), 0.3f, 1.4f);
      say("lum %.2f\n", faceCfg.lum);
      break;
    case 'i': case 'I':
      /* g_dirTgt 现在是单位向量，真实位移要乘幅度；打印时两个都给，
         不然只看到 (0.87,-0.50) 会以为方向没生效。 */
      say("mood=%s dir=%d vec=(%.2f,%.2f)x%.1f格 pan=%.0f eye+%.2f fps=%lu heap=%u ble=%d\n",
          MOOD_NAMES[g_moodId], g_dirIdx, g_dirTgt[0], g_dirTgt[1], g_ampCur,
          g_neckPanDeg, neckEyeOffset(g_dirTgt[0] * g_ampCur),
          (unsigned long)fpsShown, (unsigned)ESP.getFreeHeap(), g_bleConnected ? 1 : 0);
      say("tilt=%.0f nod=%.1f\n", g_tiltPos, faceNeckNodDeg());
      /* 情绪状态一眼看全：这一轮摸了几下、是不是困了、是不是睡了。
         调"摸太多次会烦"和"太久没人理会睡"的时候，没有这三个数就只能靠猜。 */
      say("pet=%u/45s drowsy=%d asleep=%d idle=%lus\n",
          (unsigned)g_petCount, (g_petWin && (uint32_t)(millis() - g_lastPet) > 90000) ? 1 : 0,
          g_asleep ? 1 : 0, (unsigned long)((millis() - g_lastPet) / 1000));
      say("cfg=%s pan=%d~%d tilt=%d~%d\n", g_cfgSrc, g_panLo, g_panHi, g_tiltLo, g_tiltHi);
      break;
    case 'p': case 'P': {
      /* p 现在是"真的转头"：舵机走到这个角度，眼睛顺带跟着瞟（云台视差）。
         以前 p 只改眼睛里的视差量，舵机纹丝不动 —— 因为舵机那时候压根没出过信号。
         p +3 / p -3 = 相对微调，用于标定行程（见下面 t 的说明）。
         相对模式下只卡物理 0~180，不卡当前软限位，否则永远走不出去、标不了。 */
      /* 空参数必须挡住：arg.toFloat() 对空串返回 0，再被 constrain 夹到下限，
         于是"手滑发了个 p"会让舵机猛地甩到最左边。这种意外动作比报错危险得多。 */
      if (!arg.length()) { say("用法 p <角度> 或 p +N / p -N\n"); break; }
      bool rel = arg.length() && (arg[0] == '+' || arg[0] == '-');
      float a;
      if (rel) {
        int step = arg.substring(1).toInt(); if (step == 0) step = 1;
        if (arg[0] == '-') step = -step;
        a = constrain(g_neckTgt + (float)step, 0.0f, 180.0f);
      } else {
        a = constrain(arg.toFloat(), (float)g_panLo, (float)g_panHi);
      }
      g_neckTgt = a;
      if (rel) {
        say("pan -> %d°  [限位 %d~%d]%s\n", (int)a, g_panLo, g_panHi,
            (a < g_panLo || a > g_panHi) ? " 超出" : "");
      } else {
        say("neck -> %d°\n", (int)a);
      }
      break;
    }
    case 't': case 'T': {
      /* 支持三种写法：
           t 100      绝对角度（卡在当前软限位内）
           t +1       相对 +1°（标定用：一格一格往上，卡在物理 0~180）
           t -1       相对 -1°
         标定的正确顺序：先往上微调到"机械到头了"，记下这个数；
         再往下微调到"到低了"，记下这个数；最后微调到眼睛平视的位置。
         然后 c t <低> <平视> <高> 设定，c s 保存。
         为什么相对模式不卡软限位：软限位就是我们要改的东西，
         被它挡住就永远走不出去，也标不出真实行程。 */
      /* 空参数同上：t 会让它猛抬头到下限，必须先挡。 */
      if (!arg.length()) { say("用法 t <角度> 或 t +N / t -N\n"); break; }
      bool rel = arg.length() && (arg[0] == '+' || arg[0] == '-');
      float a;
      if (rel) {
        int step = arg.substring(1).toInt(); if (step == 0) step = 1;
        if (arg[0] == '-') step = -step;
        a = constrain(g_tiltTgt + (float)step, 0.0f, 180.0f);
      } else {
        a = constrain(arg.toFloat(), (float)g_tiltLo, (float)g_tiltHi);
      }
      g_tiltTgt = a;
      if (rel) {
        say("tilt -> %d°  [限位 %d~%d]%s\n", (int)a, g_tiltLo, g_tiltHi,
            (a < g_tiltLo || a > g_tiltHi) ? " 超出" : "");
      } else {
        say("tilt -> %d°\n", (int)a);
      }
      break;
    }
    case 'c': case 'C': {
      /* 一条命令管四件事，全部走同一个 sanity 检查：
           c                 看当前生效值 + 文件从哪来的
           c p 42 92 138     改 PAN（立刻生效，不落盘）
           c t 66 90 112     改 TILT
           c s               写进 /servo.cfg
           c r               重新读文件（验证"写进去的"和"生效的"是否一致）
           c d               删文件 + 回落编译默认值
         为什么不给每个动作单独一个字母：BLE 上就一个输入框，
         短命令好敲，也不容易和 m/d/b/h/l/p/z/f/a/t 抢字母。 */
      String s = arg; s.trim();
      char sub = s.length() ? s[0] : 0;
      if (sub == 'p' || sub == 'P' || sub == 't' || sub == 'T') {
        int v[3] = {0, 0, 0};
        int got = sscanf(s.c_str() + 1, "%d %d %d", &v[0], &v[1], &v[2]);
        if (got != 3) { say("用法 c p <lo> <home> <hi>\n"); break; }
        if (!cfgSanity(v[0], v[1], v[2])) {
          say("拒绝 %d/%d/%d：要 0~180、lo<home<hi、行程>=20°\n", v[0], v[1], v[2]);
          break;
        }
        if (sub == 'p' || sub == 'P') {
          g_panLo = v[0]; g_panHome = v[1]; g_panHi = v[2];
        } else {
          g_tiltLo = v[0]; g_tiltHome = v[1]; g_tiltHi = v[2];
        }
        g_neckTgt = constrain(g_neckTgt, (float)g_panLo, (float)g_panHi);
        g_tiltTgt = constrain(g_tiltTgt, (float)g_tiltLo, (float)g_tiltHi);
        say("%s %d/%d/%d 已生效（未存盘，c s 存）\n",
            (sub == 'p' || sub == 'P') ? "PAN" : "TILT", v[0], v[1], v[2]);
      } else if (sub == 's' || sub == 'S') {
        say(cfgSave() ? "已写入 /servo.cfg\n" : "写盘失败（FAT 没挂上？）\n");
      } else if (sub == 'r' || sub == 'R') {
        cfgLoad();
        say("重读 -> PAN %d/%d/%d  TILT %d/%d/%d  来源 %s\n",
            g_panLo, g_panHome, g_panHi, g_tiltLo, g_tiltHome, g_tiltHi, g_cfgSrc);
      } else if (sub == 'd' || sub == 'D') {
        if (g_cfgMounted) FFat.remove("/servo.cfg");
        g_panLo = NECK_MIN;  g_panHi = NECK_MAX;  g_panHome = NECK_HOME;
        g_tiltLo = TILT_MIN; g_tiltHi = TILT_MAX; g_tiltHome = TILT_HOME;
        g_cfgSrc = "编译默认值";
        say("已删文件，回默认 PAN %d/%d/%d TILT %d/%d/%d\n",
            g_panLo, g_panHome, g_panHi, g_tiltLo, g_tiltHome, g_tiltHi);
      } else {
        say("cfg: %s   FAT %s\n", g_cfgSrc, g_cfgMounted ? "已挂载" : "未挂载");
        say("PAN  %d/%d/%d  行程 %d°\n", g_panLo, g_panHome, g_panHi, g_panHi - g_panLo);
        say("TILT %d/%d/%d  行程 %d°\n", g_tiltLo, g_tiltHome, g_tiltHi, g_tiltHi - g_tiltLo);
        say("c p|t <lo> <home> <hi>   c s 存   c r 重读   c d 删\n");
      }
      break;
    }
    case 'z': case 'Z':
      servoRelease(PIN_NECK);        // 松下力：不发热、不较劲，手可以随便拨
      servoRelease(PIN_TILT);
      say("both servos released\n");
      break;
    case 'f': case 'F':
      g_showFps = (arg.toInt() != 0);
      say("fps overlay %s\n", g_showFps ? "on" : "off");
      break;
    case 'a': case 'A':
      g_auto = (arg.toInt() != 0);
      if (g_auto) { g_nextMood = millis() + 1200; g_nextDir = millis() + 800; }
      say("auto %s\n", g_auto ? "on" : "off");
      break;
      case 'K': {
        /* 蓝牙被"启动保护"跳过之后，用这个清计数并重启，重新试一次。
           为什么不用 RST：RST 属于硬复位，能不能清 RTC 内存不确定，
           而这个命令 100% 有效。 */
        /* 删掉"蓝牙坏了"的标记，否则重启后还是会跳过 */
        nvsNobleSet(0);
        nvsTrySet(0);
        if (g_cfgMounted && FFat.exists("/noble.flag")) FFat.remove("/noble.flag");
        g_bootMagic = 0;
        g_bootCount = 0;
        g_bleStep   = 0;
        say("已清除蓝牙禁用标记与启动计数，2 秒后重启（会重试蓝牙）…\n");
        delay(1200);          // 通知/串口输出要时间送出去，别急着重启
        ESP.restart();
        break;
      }
      case 'w': {
        /* ===== 射频体检 =====
           蓝牙和 WiFi 共用同一个射频前端（同一根天线、同一套 PLL 和放大器）。
           所以扫一次 WiFi 就能定性：
             · 扫到 AP  → 射频是好的，蓝牙崩在软件层，有救
             · 一个都扫不到 / 直接崩 → 射频前端坏了，换板子或外挂模块
           扫完立刻关掉 WiFi，把内存还回去（WiFi 要吃掉 50~70KB）。 */
        say("WiFi 扫描中（约 3 秒）…\n");
        WiFi.persistent(false);
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(200);
        int n = WiFi.scanNetworks(false, false);
        if (n < 0) {
          say("!! scanNetworks 返回 %d（负数=失败）\n", n);
        } else if (n == 0) {
          say("!! 扫到 0 个 AP（射频可能有问题，或附近确实没有 WiFi）\n");
        } else {
          say("✅ 扫到 %d 个 AP → 射频前端正常\n", n);
          for (int i = 0; i < n && i < 6; i++)
            say("   %2d. %-20s %d dBm\n", i + 1,
                WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i));
        }
        WiFi.scanDelete();
        WiFi.mode(WIFI_OFF);
        say("WiFi 已关闭，heap=%u\n", (unsigned)ESP.getFreeHeap());
        break;
      }
    case '?': printHelp(); break;
    default: say("? for help\n"); break;
  }
}

// ---------------- 串口 ----------------
static void handleSerial() {
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      g_reply = "";
      execCommand(buf);          // 里面自己 trim，空行会直接返回
      buf = "";
    } else if (buf.length() < 40) {
      buf += c;
    }
  }
}

// ---------------- 蓝牙 ----------------
/* 回调只做一件事：把字节塞进环缓冲，立刻返回。
   原因见文件头 —— 蓝牙回调在另一个任务里跑，不能直接改表情状态。 */
static void blePush(const uint8_t *p, size_t n) {
  portENTER_CRITICAL(&g_mux);
  for (size_t i = 0; i < n; i++) {
    uint16_t nx = (g_rHead + 1) % RING_SZ;
    if (nx == g_rTail) break;                 // 满了丢新字节：人手的命令不可能这么快
    g_ring[g_rHead] = (char)p[i];
    g_rHead = nx;
  }
  portEXIT_CRITICAL(&g_mux);
  g_bleRxMs = millis();
}

class RxCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String v = c->getValue();
    blePush((const uint8_t *)v.c_str(), v.length());
  }
};

class SrvCallback : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    g_bleConnected = true; g_bleJustIn = true;
    g_bleReset = true;        // 上一次连接残留的半条命令要丢掉
  }
  void onDisconnect(BLEServer *s) override {
    g_bleConnected = false; g_bleJustOut = true;
    g_bleReset = true;
    s->startAdvertising();     // 不重新广播，手机一断就再也连不回来
  }
};

/* 回话分块发。
   一条通知最多带 MTU-3 字节（手机常见 185 → 182，电脑 247 → 244）。
   帮助文本四百多字，不分块的话手机只能看到开头一小截，
   用户会以为"帮助被吃了一半"。这里按 150 字节切，连发几条。
   切的时候必须看字节，不能按字符数切 —— 一个汉字占 3 字节，
   从中间切开会变成乱码方框。 */
static size_t chunkLen(const String &s, size_t i, size_t maxN) {
  size_t n = 0;
  while (i + n < s.length() && n < maxN) {
    uint8_t c = (uint8_t)s[i + n];
    size_t clen = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
    if (n + clen > maxN) break;
    n += clen;
  }
  return n ? n : 1;          // 兜底，绝不让 i 原地打转
}

static void bleNotifyReply() {
  if (!g_txChar || !g_bleConnected || !g_reply.length()) { g_reply = ""; return; }
  const size_t CH = 150;
  for (size_t i = 0; i < g_reply.length(); ) {
    size_t n = chunkLen(g_reply, i, CH);
    g_txChar->setValue((uint8_t *)g_reply.c_str() + i, n);
    g_txChar->notify();
    i += n;
  }
  g_reply = "";
}

/* 每一步都打点 + flush。为什么这么啰嗦：
   panic/abort 的真实信息走 UART0，我们这边看不到；但如果我们自己的打点
   停在某一行，那一行就是崩溃点。flush 是必须的 —— USB CDC 有缓冲，
   不 flush 的话最后几条会跟着崩溃一起丢掉，我们就永远只看到"断在某处"。 */
static void bleBegin() {
  /* 第一件必须做的事：把"经典蓝牙"那份内存还回去。
     只用 BLE 却把 BTDM 的整份内存扣着，会在 esp_bt_controller_init 里
     因为拿不到连续的内部 DMA RAM 而失败。ESP-IDF 官方 BLE 例程统统有这一行。
     实测能回收约 30KB。 */
  g_bleStep = 1; Serial.println("[BLE] 1 mem_release"); Serial.flush();
  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  g_bleStep = 2; Serial.println("[BLE] 2 BLEDevice::init"); Serial.flush();
  BLEDevice::init(BLE_NAME);

  g_bleStep = 3; Serial.printf("[BLE] 3 setMTU  heap=%u\n", (unsigned)ESP.getFreeHeap()); Serial.flush();
  BLEDevice::setMTU(247);      // 不协商 MTU 的话一条通知只能带 20 字节，回话会被截断

  g_bleStep = 4; Serial.println("[BLE] 4 createServer"); Serial.flush();
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new SrvCallback());

  g_bleStep = 5; Serial.println("[BLE] 5 createService"); Serial.flush();
  BLEService *svc = server->createService(NUS_SERVICE);

  g_bleStep = 6; Serial.println("[BLE] 6 rx char"); Serial.flush();
  BLECharacteristic *rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCallback());

  g_bleStep = 7; Serial.println("[BLE] 7 tx char"); Serial.flush();
  g_txChar = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
  g_txChar->addDescriptor(new BLE2902());   // 少了这个描述符，iPhone 收不到通知

  g_bleStep = 8; Serial.println("[BLE] 8 svc start"); Serial.flush();
  svc->start();

  g_bleStep = 9; Serial.println("[BLE] 9 advertising"); Serial.flush();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);   // iOS 连接间隔，Nordic 官方示例的建议值
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  g_bleStep = 10; Serial.println("[BLE] 10 done"); Serial.flush();
}

static void handleBle() {
  static String buf;
  /* 连接或断开过：把缓冲里那半条没发完的命令扔掉。
     不扔的话，上次"m ha"没打完的残渣会跟这次的"ppy"接成"m happy"，
     看着像闹鬼。 */
  if (g_bleReset) {
    g_bleReset = false;
    buf = "";
    portENTER_CRITICAL(&g_mux);
    g_rTail = g_rHead;
    portEXIT_CRITICAL(&g_mux);
  }
  bool got = false;
  while (true) {
    char c;
    portENTER_CRITICAL(&g_mux);
    if (g_rTail == g_rHead) { portEXIT_CRITICAL(&g_mux); break; }
    c = g_ring[g_rTail];
    g_rTail = (g_rTail + 1) % RING_SZ;
    portEXIT_CRITICAL(&g_mux);

    got = true;
    if (c == '\n' || c == '\r') {
      if (buf.length()) { g_reply = ""; execCommand(buf); buf = ""; bleNotifyReply(); }
    } else if (buf.length() < 48) {
      buf += c;
    }
  }
  /* 很多蓝牙串口 App 发命令不带换行。最后一个字节过去 40ms 还没等到换行，
     就把攒着的一串当命令执行 —— 不然会一直等，看着像"发了没反应"。 */
  if (!got && buf.length() && millis() - g_bleRxMs > 40) {
    g_reply = ""; execCommand(buf); buf = ""; bleNotifyReply();
  }
}

// ---------------- setup ----------------
void setup() {
  Serial.begin(115200);
  delay(200);
  /* 把 ESP-IDF 内核日志也导到 USB 串口上来。
     为什么需要：蓝牙初始化崩在协议栈内部，崩之前那几行内核错误（E (xxx) BT_CTRL: ...）
     才是真原因，而它们默认走 UART0（我们看不到）。这一行把它们改道到 USB。 */
  Serial.setDebugOutput(true);

  /* 上电 / 拔插 USB 时 RTC 内存会掉，magic 对不上就重新计数。
     连续软复位才会累积。
     2026-11-16：崩溃复位也可能把 RTC 清掉，所以计数同时从 NVS 取一次较大值，
     否则保护永远触发不了（见 nvsTryGet 上面的注释）。 */
  if (g_bootMagic != BOOT_MAGIC) { g_bootMagic = BOOT_MAGIC; g_bootCount = 0; g_bleStep = 0; }
  {
    uint8_t nvsTry = nvsTryGet();
    if (nvsTry > (uint8_t)g_bootCount) g_bootCount = nvsTry;   // RTC 被清了就信 NVS
  }
  g_bootCount++;

  Serial.println(F("\npet_eye boot"));
  Serial.printf("[boot] 本次是第 %u 次尝试启动，上次蓝牙走到第 %u 步\n",
                (unsigned)g_bootCount, (unsigned)g_bleStep);

  pinMode(PIN_BLK, OUTPUT);
  digitalWrite(PIN_BLK, HIGH);        // 背光先开，避免初始化时黑屏

  tft.init();
  tft.setRotation(1);                 // 1 或 3：屏幕方向不对就换另一个
  tft.fillScreen(TFT_BLACK);

  // 40KB 一帧，失败就是内存不够（多半被蓝牙/WiFi 吃了）
  spr.setColorDepth(16);
  if (!spr.createSprite(SCR_W, SCR_H)) {
    Serial.println(F("!! sprite alloc failed"));
    while (true) delay(1000);
  }
  spr.setSwapBytes(true);
  spr.fillSprite(TFT_BLACK);
  g_spr = &spr;

  faceSetHue(faceCfg.hue);
  /* 开机从"闭眼"睁开：上电直接蹦出两只睁开的眼太硬，420ms 缓缓睁开像刚醒。
     做法是把当前形态冻成 blink（一条横线），再切 normal ——
     faceSetMood 内部会 liveMood 冻结起点，于是插值就是"从横线长开"。 */
  copyMood(g_cur, MOOD_TABLE[M_BLINK]);
  copyMood(g_tgt, MOOD_TABLE[M_BLINK]);
  g_moodId = M_BLINK; g_moodT = 1;
  faceSetMood(M_NORMAL, 420);
  facePrimeHearts();          // 喜欢表情一进去就有爱心，不用等两秒

  Serial.printf("sprite ok, free heap = %u\n", (unsigned)ESP.getFreeHeap());

  /* 云台舵机：GPIO14，50Hz，14 位。
     注意分辨率必须是 14 —— S3 的 LEDC 上限就是 14 位，
     写 16 位 ledcAttach 会静默失败，引脚上永远 0V（2026-11-15 踩过，查了两天）。 */
  /* ★ 顺序不能反：先把限位从 /servo.cfg 读出来，再 attach 舵机。
     反过来的话，第一帧会用编译默认值（30~150）把舵机甩到某个角，
     而真实安全行程可能只有 42~138 —— 那第一下就是顶着壳上电。 */
  cfgLoad();
  g_neckPos = g_neckTgt = (float)g_panHome;
  g_tiltPos = g_tiltTgt = (float)g_tiltHome;

  /* 下拉很关键：触摸板没接的时候 13 号脚是浮空的，
     浮空输入会随机读到高电平 → 莫名其妙自己笑。下拉之后没接线就永远是低。 */
  pinMode(PIN_TOUCH, INPUT_PULLDOWN);
  pinMode(PIN_TOUCH2, INPUT_PULLDOWN);   // 下巴，同样必须下拉

  servoAttach(PIN_NECK);
  servoWriteDeg(PIN_NECK, (int)g_neckPos);
  Serial.printf("neck servo ok @ GPIO%d, %d°\n", PIN_NECK, (int)g_neckPos);

  /* 第二个舵机。没接线也照样 attach —— 引脚上只是空转脉冲，不会报错，
     等线接上就立刻能用，省得"忘了烧这版"。 */
  servoAttach(PIN_TILT);
  servoWriteDeg(PIN_TILT, (int)g_tiltPos);
  Serial.printf("tilt servo ok @ GPIO%d, %d° (range %d~%d, home %d)\n",
                PIN_TILT, (int)g_tiltPos, g_tiltLo, g_tiltHi, g_tiltHome);

  /* 帮助必须在 cfgLoad 之后打：帮助里的限位范围是动态读 g_panLo/g_tiltLo 的，
     放前面会把编译默认值当成实际生效值报出来，查日志时会被误导。 */
  printHelp();

  /* 蓝牙最后开：它一开就吃掉七十多 KB 内部 RAM，前面几张分配都抢在它前头，
     免得"先开蓝牙、再要 40KB 帧缓冲"时运气不好。 */
  /* 蓝牙怎么处理，三种情况：
     ① FAT 里有 /noble.flag → 已经确认过蓝牙起不来，**直接跳过**。
        这是关键改进：以前每次断电开机都要白试 3 次（屏幕白一下、闪几下、约 4 秒），
        现在直接把结论存在 flash 里，开机立刻出眼睛。
     ② 连续 3 次没跑过初始化 → 这次跳过，并把"蓝牙坏了"写进 flag，下次就不用再试。
     ③ 其它 → 正常尝试。
     想重新试蓝牙：串口发 K（会删掉 flag、清计数、重启）。 */
  bool flagBroken = nvsNobleFlag() || (g_cfgMounted && FFat.exists("/noble.flag"));
  if (flagBroken || g_bootCount >= 3) {
    g_bleSkipped = true;
    nvsTrySet(0);                            // 已经定案，次数不用再记
    if (!flagBroken) {
      nvsNobleSet(1);                        // 主力：NVS，掉电不丢
      if (g_cfgMounted) {                    // 顺手再写一份 FAT（现在写不进，留着不碍事）
        File f = FFat.open("/noble.flag", FILE_WRITE);
        if (f) { f.print("ble broken 2026-11-16 (enable, step 30)\n"); f.close(); }
      }
    }
    Serial.printf("[保护] 跳过蓝牙（%s）。眼睛和舵机正常；串口发 K 可重试。\n",
                  flagBroken ? "已记录蓝牙起不来" : "连续失败，已记录");
  } else {
    nvsTrySet((uint8_t)g_bootCount);   // 先记账再动硬件：崩了下次也知道试过几次
    bleBegin();
    nvsTrySet(0);         // 走到这里说明蓝牙初始化没崩 —— 本次启动成功，计数清零
    g_bootCount = 0;
  }
  Serial.printf("BLE %s, free heap = %u\n",
                g_bleSkipped ? "SKIPPED" : "on", (unsigned)ESP.getFreeHeap());

  // 首帧的 dt 不能从 0 开始算：Serial 等待 + tft.init + 分配 40KB 已经过去几百毫秒，
  // 否则第一帧会一次推进十几个帧号，表现为"开机莫名眨一下"或爱心一进场就老了一截。
  lastFrameMs = millis();
  g_nextMood = millis() + 3000;
  g_nextDir  = millis() + 1200;
  g_nextNeck = millis() + 5000;      // 开机 5 秒后开始东张西望
  g_lastPet  = millis();             // 困/睡计时从开机算起，别一上电就 90 秒
}

// ---------------- 自动行为 ----------------
/* 2026-11-14 22:26 加：桌宠要有"活物感"。
   自动模式下每隔几秒换表情、换视线，节奏刻意做得长短不一 ——
   固定 3 秒一次会读出"定时器"的味道，随机反而像在发呆。
   一旦从串口发命令，自动模式立刻关闭，交还手动控制。 */
static void autoMood(uint32_t now) {
  /* ---- 三级递进：清醒 → 困（闭眼）→ 睡着 ----
     直接跳 sleep 会读成"突然关机"。合眼一下、再合一下、第三下没睁开，
     才像真的困了。这三个数字是手感，不是文献：90 秒开始困、
     闭三下（约 4~6 秒）睡着。 */
  uint32_t idle = (uint32_t)(now - g_lastPet);

  if (idle > 180000) idle = 180000;                     // 3 分钟封顶，防溢出乱来

  if (idle > 90000) {
    if (g_moodId == M_BLINK) {                          // 刚闭完这一下
      g_drowseBlink++;
      if (g_drowseBlink >= 3) {                         // 第三下没睁开 = 睡着
        g_asleep = true;
        faceSetMood(M_SLEEP, 1.0f);
        g_nextMood = now + 3600000;                     // 睡着了就别再换表情
        Serial.println("idle -> sleep");
        return;
      }
      faceSetMood(M_NORMAL, 1.0f);                      // 又睁开一下
      g_nextMood = now + 1000 + random(0, 900);
    } else {
      faceSetMood(M_BLINK, 1.0f);                       // 眼皮撑不住了
      g_nextMood = now + 800 + random(0, 600);
    }
    return;
  }

  int n;
  int r = random(0, 100);
  bool ev   = r < 8;                                    // 8% 小动作（点头/摇头/一惊）
  bool rare = !ev && r < 20;                            // 12% 半常态（无语/困惑）
  for (int tries = 0; tries < 3; tries++) {
    n = ev   ? (int)EVENT_POOL[random(0, sizeof(EVENT_POOL))]
      : rare ? (int)IDLE_RARE[random(0, sizeof(IDLE_RARE))]
             : (int)IDLE_POOL[random(0, sizeof(IDLE_POOL))];
    if (n != g_moodId) break;                       // 抽到同一个 = 看着像没反应，重抽
  }
  if (ev)                g_nextMood = now + 1600 + random(0, 1200);  // 让事件先演完再换
  else if (rare)         g_nextMood = now + 3000 + random(0, 2500);
  else if (n == M_BLINK) g_nextMood = now + 1100 + random(0, 900);   // 闭眼不能站太久
  else                   g_nextMood = now + 3200 + random(0, 3800);
  faceSetMood(n);
}

/* ==================== 仿生注视引擎 ====================
   依据（都是实测数据，不是拍脑袋）：
   · 猫（Blakemore & Donaghy 1980）：>4° 的扫视几乎都带一次头转，头动滞后眼动
     **30ms**；眼动结束后眼睛反向转回来，保持注视方向（补偿增益 0.96）。
   · 灵长类（Bizzi 1971）：眼先头后 20~40ms。这是"triggered gaze shift"。
   · 人类自由看场景（Henderson & Hollingworth 1998；Dorr et al. 2010）：
     注视平均 240~330ms、众数 230ms，右偏分布，很短和很长的都少；
     扫视幅度平均 4.6°；**中心偏置**（大部分注视落在画面中部）。
   · 变色龙（Ott et al. 1998）：用"走走停停"的扫视扫描；一旦锁定目标，
     头转向目标、双眼汇聚；目标移动时**用头追，不用眼追**。
   
   我们的硬件只有两个自由度（无 roll）。但屏幕上的眼睛可以画到任何方向，
   所以生物学的"眼-头分配"正好能实现：小角度只动眼，大角度头主转、眼只带偏移。
   ====================================================== */

struct GazeSpot { int8_t dir; int8_t dPan; int8_t dTilt; uint8_t w; };

/* 兴趣点表。dir 用时钟索引（0 中 / 1 十二点 / 4 三点 / 7 六点 / 10 九点），
   dPan / dTilt 是相对"平视正视"的偏移。w 是权重 —— 
   正前方给 8 是故意的：人和动物都最容易看"中间那个东西"（中心偏置）。 */
static const GazeSpot GAZE_SPOTS[] = {
  /* 权重设计：抬头 13 / 平视 11 / 低头 6（合计 30）→ 抬头 43%、平视 37%、低头 20%。
     为什么不平均：它只有巴掌大，面对的是一个比它大十几倍的活人 ——
     现实中它绝大部分时间应该**仰视**你，而不是低头看桌面。
     （你原话：去一个很美的地方你会抬头看，不会低头看。）
     低头只留给"看桌上的东西"，占两成多一点，够表示"这里也有东西可看"。 */
  { 1,    0, -26, 5 },   // 正上方：明显抬头 + 瞪眼（好奇的最高档）
  { 2,   13, -10, 4 },   // 右上：仰视
  {12,  -13, -10, 4 },   // 左上：仰视
  { 0,    0,  -3, 7 },   // 正前方：看你（略仰 —— 你的脸在它上方）
  { 4,   22,   0, 2 },   // 右
  {10,  -22,   0, 2 },   // 左
  { 7,    0,  14, 2 },   // 下方：看桌面
  { 6,   14,  10, 2 },   // 右下
  { 8,  -14,  10, 2 },   // 左下
};
#define N_SPOT ((int)(sizeof(GAZE_SPOTS) / sizeof(GAZE_SPOTS[0])))

static uint32_t g_gazeAt   = 0;      // 下一次换注视点
static uint32_t g_neckLag  = 0;      // 头开始动的时刻（眼先头后）
static bool     g_neckPend = false;
static float    g_pendPan  = 0, g_pendTilt = 0;
static uint32_t g_microAt  = 0;      // 注视中的微扫视
static uint8_t  g_recent[3] = {0, 0, 0};
static uint8_t  g_recentI  = 0;

/* 时钟圈上的距离（1 到 6，越过 12 点算最短） */
static int dirRingDist(int a, int b) {
  if (a == 0 || b == 0) return 3;             // 中 → 某向：算中等幅度
  int d = (a > b) ? (a - b) : (b - a);
  return (d > 6) ? (12 - d) : d;
}

static void gazePick(uint32_t now) {
  /* 权重里扣掉"最近看过的三个位置"——这就是习惯化：
     同一个地方连着看会腻，真实动物也是这样，不然看着像随机数。 */
  int w[N_SPOT], total = 0;
  for (int i = 0; i < N_SPOT; i++) {
    w[i] = GAZE_SPOTS[i].w;
    for (int k = 0; k < 3; k++)
      if (g_recent[k] == i && w[i] > 1) w[i] /= 3;
    total += w[i];
  }
  int r = random(0, total), acc = 0, pick = 0;
  for (int i = 0; i < N_SPOT; i++) { acc += w[i]; if (r < acc) { pick = i; break; } }
  g_recent[g_recentI] = (uint8_t)pick;
  g_recentI = (uint8_t)((g_recentI + 1) % 3);

  const GazeSpot &s = GAZE_SPOTS[pick];
  int dist = dirRingDist(g_dirIdx, s.dir);

  /* Main sequence：幅度越大，动作持续越久（但速度也更快）。
     生物 2.1ms/度递增、20° 约 110ms；我们一格约合 2.5°，取 70 + 11×格。 */
  faceSetDir(s.dir, 70.0f + 11.0f * (float)dist);

  /* ① 眼睛先到 ② 头 100~160ms 后才开始转。
     生物是 20~40ms，但舵机有惯性，间隔太短就看不出"眼先头后"这个层次。 */
  g_pendPan  = (float)constrain(g_panHome  + s.dPan,  g_panLo,  g_panHi);
  g_pendTilt = (float)constrain(g_tiltHome + s.dTilt, g_tiltLo, g_tiltHi);
  g_neckPend = true;
  g_neckLag  = now + 100 + random(0, 60);

  /* 抬头时顺手把表情带上去：好奇本来就是"抬头 + 瞪眼"一起发生的，
     分开演就变成两件事了。只在 normal 时改，别打断正在演的表情。 */
  if (s.dTilt <= -12 && faceMoodId() == M_NORMAL) faceSetMood(M_PEEK, 300);

  /* 注视时长：右偏分布，取 0.7~2.4 秒。人类是 240~330ms，
     但我们没有真实世界可看，停久一点才像"在盯着什么"。 */
  g_gazeAt  = now + 700 + random(0, 900) + random(0, 800);
  g_microAt = now + 900 + random(0, 700);
}

/* 注视不是完全静止的：真实眼睛有微扫视（1~2Hz，幅度极小）。
   我们最小只有一格（30°），所以做得很少、很轻，只为"没死"。 */
static void gazeMicro(uint32_t now) {
  if (g_dirIdx >= 1) {
    int step = random(0, 2) ? 1 : 11;
    int d = ((g_dirIdx - 1 + step) % 12) + 1;
    faceSetDir(d, 120.0f);
  }
  g_microAt = now + 900 + random(0, 800);
}

/* ==================== 摸太多次会烦 ====================
   不是拍脑袋定的数，三条都能查到出处：

   · **第 4 下** —— Oregon Humane Society 的行为说明写得很直白：
     "猫能享受前三次抚摸，第四下就咬你"。所以第 4 次不是"生气"，
     是**第一次不耐烦**：移开视线 + 轻视。这是警告，不是发作。
   · **第 8 下** —— University of Lincoln 用 34 只猫做过实测（8 个部位、
     每处 15 次一秒抚摸），主人摸时猫的负面行为**中位数 8.5 次**。
     到这一步才真的生气。
   · **第 12 下** —— 还不松手就恼了，加抖一抖。
     参考值：猫对头部的耐受约 15 下，这是把上限拿来当"别再摸了"的点。

   窗口 45 秒（一次连续的"撸"），停手 25 秒计数归零 ——
   猫也是这样，气一会儿就忘，不会记仇到明天。 */
static uint8_t petTally(uint32_t now) {
  /* 注意别写 `g_petWin = now | 1`：那会让 now 是偶数时窗口起点比 now 大 1ms，
     下一次算出来是 -1，转成 uint32 变成"四万多年前"，窗口立刻被重置 ——
     `x 13` 连摸 13 下就永远停在第 1 下，一次都升不了级。踩过。 */
  if (!g_petWin || (uint32_t)(now - g_petWin) > 45000) {
    g_petWin = now;
    g_petCount = 0;
  }
  if (g_petCount < 250) g_petCount++;
  return g_petCount;
}

static void autoBehavior(uint32_t now) {
  /* 停手一会儿，气就消了。手动模式下也要走，所以放在 g_auto 判断之前。 */
  if (g_petCount && (uint32_t)(now - g_petWin) > 25000) {
    g_petCount = 0;
    if (g_moodId == M_ANGRY || g_moodId == M_ANGRY2) {
      faceSetMood(M_NORMAL, 400.0f);
      g_nextMood = now + 2500;
      g_tiltTgt  = (float)g_tiltHome;
      g_neckTgt  = (float)g_panHome;
      Serial.println("calmed down");
    }
  }

  if (!g_auto) return;

  /* 睡着了就别再东张西望 —— 眼睛乱瞟的机器人看起来不像睡着，像宕机。
     头慢慢垂下去，视线定在原地。被摸或收到命令才会醒（见 handleTouch / execCommand）。 */
  if (g_asleep) {
    g_neckTgt = (float)g_panHome;
    g_tiltTgt = (float)constrain(g_tiltHome + 9, g_tiltLo, g_tiltHi);
    return;
  }
  if ((int32_t)(now - g_nextMood) >= 0) autoMood(now);
  if ((int32_t)(now - g_gazeAt) >= 0)   gazePick(now);
  else if ((int32_t)(now - g_microAt) >= 0) gazeMicro(now);
}

/* ==================== 头顶触摸 ====================
   触摸不是"触发一个表情"，是一串有先后顺序的反应 —— 这是"神态"的核心：
     ① 眼睛立刻回中（看见你了）
     ② 头抬起来（注意你）
     ③ 眯眼笑（高兴）
     ④ 1.3 秒后回落、把控制权交回自动
   分开演就是三个独立动作；连起来才像"它发现你在摸它"。
   
   TTP223 点动模式：手按住出高、松开变低。所以只认"上升沿"，
   否则按住不放会每帧触发一次。冷却 1.4 秒防止抖动连发。
   （状态变量 g_touch* 提到文件前面了，因为 `x` 命令也要用。） */

/* ---- 第 III 类：彩蛋。不进任何池子，只认手势 ----
   条件就写在这下面，故意**不写进帮助、不写进文档、不写进 README**。
   彩蛋的乐趣在于自己玩出来，写在说明书里就等于没有。 */
static uint32_t g_eggSeqT[6];
static uint8_t  g_eggSeqP[6];
static uint8_t  g_eggSeqN   = 0;
static uint32_t g_eggBothAt = 0;
static uint32_t g_eggChinAt = 0;

static void eggSeqPush(uint8_t pin, uint32_t now) {
  if (g_eggSeqN < 6) {
    g_eggSeqT[g_eggSeqN] = now; g_eggSeqP[g_eggSeqN] = pin; g_eggSeqN++;
  } else {
    for (int i = 1; i < 6; i++) { g_eggSeqT[i-1] = g_eggSeqT[i]; g_eggSeqP[i-1] = g_eggSeqP[i]; }
    g_eggSeqT[5] = now; g_eggSeqP[5] = pin;
  }
}

/* 最近 5 次是"头、下巴、头、下巴、头"这样交替，且全程不超过 4 秒 */
static bool eggAlternating() {
  if (g_eggSeqN < 5) return false;
  uint32_t span = g_eggSeqT[g_eggSeqN-1] - g_eggSeqT[g_eggSeqN-5];
  if (span > 4000) return false;
  for (int i = 1; i < 5; i++)
    if (g_eggSeqP[g_eggSeqN-1-i] == g_eggSeqP[g_eggSeqN-i]) return false;
  return true;
}

static void petReact(uint32_t now, bool head) {
  bool woke = g_asleep;                     // 刚才是不是睡着的
  g_asleep      = false;
  g_drowseBlink = 0;
  g_lastPet     = now;                      // 有人来了，困/睡的计时重来
  g_neckPend    = false;                    // 取消还没执行的下一次注视

  uint8_t n = petTally(now);                // 这是这一轮连续摸的第几下

  if (woke) {
    /* 被弄醒：先一惊，再慢慢回神。不笑 —— 刚醒的人不会先笑。 */
    faceSetDir(0, 140.0f);
    faceSetMood(M_STARTLE, 180.0f);
    g_tiltTgt = (float)constrain(g_tiltHome - 6, g_tiltLo, g_tiltHi);
    g_neckTgt = (float)g_panHome;
    g_gazeAt  = now + 2400;
    Serial.println(head ? "touch head (woke)" : "touch chin (woke)");
  } else if (n >= 12) {
    /* 还不松手：真恼了，抖一下 */
    faceSetMood(M_ANGRY2, 200.0f);
    g_tiltTgt = (float)constrain(g_tiltHome + 6, g_tiltLo, g_tiltHi);
    g_neckTgt = (float)g_panHome;
    g_gazeAt  = now + 1600;
    Serial.println(head ? "touch head (angry2)" : "touch chin (angry2)");
  } else if (n >= 8) {
    /* 第 8 下：生气，不再抬头看你 */
    faceSetDir(0, 200.0f);
    faceSetMood(M_ANGRY, 200.0f);
    g_tiltTgt = (float)constrain(g_tiltHome + 4, g_tiltLo, g_tiltHi);
    g_neckTgt = (float)g_panHome;
    g_gazeAt  = now + 1600;
    Serial.println(head ? "touch head (angry)" : "touch chin (angry)");
  } else if (n >= 4) {
    /* 第 4 下：第一次不耐烦 —— 不抬头了，眼睛溜走 */
    if (head) faceSetDir(3, 320.0f);        // 眼睛往右移开，不看你
    faceSetMood(M_DESPISE, 260.0f);
    g_tiltTgt = (float)g_tiltHome;
    g_neckTgt = (float)constrain(g_panHome + 10, NECK_MIN, NECK_MAX);
    g_gazeAt  = now + 2200;
    Serial.println(head ? "touch head (despise)" : "touch chin (despise)");
  } else if (head) {
    faceSetDir(0, 90.0f);                                   // ① 看你
    g_tiltTgt  = (float)constrain(g_tiltHome - 12, g_tiltLo, g_tiltHi); // ② 抬头
    g_neckTgt  = (float)g_panHome;
    faceSetMood(M_HAPPY, 220.0f);                           // ③ 笑
    g_gazeAt   = now + 2600;
    Serial.println("touch head");
  } else {
    /* 下巴被挠：缩一下、歪头、眯眼。和头顶那套刻意做得不一样 ——
       摸头是"抬头看你"，挠下巴是"舒服得歪过去"，两种反应不能雷同。 */
    g_tiltTgt  = (float)constrain(g_tiltHome + 7, g_tiltLo, g_tiltHi);
    g_neckTgt  = (float)constrain(g_panHome + 18, NECK_MIN, NECK_MAX);
    faceSetMood(M_SHY, 240.0f);
    g_gazeAt   = now + 1800;
    Serial.println("touch chin");
  }

  /* 反应别被自动模式下一帧就盖掉 */
  if (g_auto) g_nextMood = now + 2200;
}

static void handleTouch(uint32_t now) {
  bool v1 = digitalRead(PIN_TOUCH);    // 头顶
  bool v2 = digitalRead(PIN_TOUCH2);   // 下巴

  /* 彩蛋 A：两块一起按住不放 —— 它"卡住了" */
  if (v1 && v2) {
    if (!g_eggBothAt) g_eggBothAt = now | 1;
    else if ((uint32_t)(now - g_eggBothAt) > 1500 && g_moodId != M_GLITCH) {
      faceSetMood(M_GLITCH, 1500.0f);
      if (g_auto) g_nextMood = now + 3000;
      g_eggBothAt = 0;
      g_touchAt = g_touch2At = 0;
      Serial.println("egg");
    }
  } else {
    g_eggBothAt = 0;
  }

  /* 彩蛋 B：只按住下巴，一直不松手 —— 它在等你 */
  if (v2 && !v1) {
    if (!g_eggChinAt) g_eggChinAt = now | 1;
    else if ((uint32_t)(now - g_eggChinAt) > 5000 && g_moodId != M_DEAD) {
      faceSetMood(M_DEAD, 2800.0f);
      if (g_auto) g_nextMood = now + 4200;
      g_eggChinAt = 0;
      g_touch2At  = 0;
      Serial.println("egg");
    }
  } else {
    g_eggChinAt = 0;
  }

  /* 头顶优先。两个一起摸时只演头顶那套，并且顺手取消下巴的回落，
     否则两套动作会在同一帧里互相覆盖，看起来像抽搐。 */
  if (v1 && !g_touchLast && (int32_t)(now - g_touchCool) >= 0) {
    g_touchCool = now + 1400;
    g_touchAt   = now;
    g_touch2At  = 0;
    petReact(now, true);
  } else if (v2 && !g_touch2Last && (int32_t)(now - g_touch2Cool) >= 0) {
    g_touch2Cool = now + 1400;
    g_touch2At   = now;
    g_touchAt    = 0;
    petReact(now, false);
  }
  /* 彩蛋 C：头、下巴交替摸（像在逗它）—— 连摸 5 次、4 秒内 */
  if ((v1 && !g_touchLast) || (v2 && !g_touch2Last)) {
    eggSeqPush(v1 ? 1 : 0, now);
    if (eggAlternating() && g_moodId != M_RAINBOW) {
      faceSetMood(M_RAINBOW, 3000.0f);
      if (g_auto) g_nextMood = now + 4400;
      g_eggSeqN = 0;
      Serial.println("egg");
    }
  }

  g_touchLast  = v1;
  g_touch2Last = v2;

  if (g_touchAt && (uint32_t)(now - g_touchAt) > 1300) {     // ④ 头顶回落
    g_touchAt = 0;
    g_tiltTgt = (float)g_tiltHome;
    if (g_auto) g_nextMood = now + 1800;
  }
  if (g_touch2At && (uint32_t)(now - g_touch2At) > 1100) {   // ④ 下巴回落（快一点，像缩一下就回来了）
    g_touch2At = 0;
    g_tiltTgt  = (float)g_tiltHome;
    g_neckTgt  = (float)g_panHome;
    if (g_auto) g_nextMood = now + 1500;
  }
}

// ---------------- loop ----------------
void loop() {
  handleSerial();
  handleBle();          // 蓝牙来的命令也在主线程执行，跟串口完全同一条路

  uint32_t now = millis();
  handleTouch(now);     // 摸头：看你→抬头→笑；摸下巴：缩→歪头→眯眼。两个都摸时头顶优先

  /* 手机连上/断开的反应放在主循环做，不在蓝牙回调里做 ——
     回调在另一个任务，直接改表情状态会跟渲染打架。 */
  if (g_bleJustIn) {
    g_bleJustIn = false;
    /* 有人来了先点一下头（有 TILT 就是真低头，没 TILT 就只是眼睛沉一下），
       0.9 秒后自己切笑脸。比"直接蹦个笑脸"像个人 —— 打招呼是有顺序的。 */
    faceSetMood(M_NOD);
    g_greetAt = now + 900;
    if (g_auto) g_nextMood = now + 1800;        // 别让自动模式马上把它盖掉
    Serial.println("BLE connected");
  }
  if (g_greetAt && (int32_t)(now - g_greetAt) >= 0) {
    g_greetAt = 0;
    faceSetMood(M_HAPPY);                       // 点头演完接笑脸
    if (g_auto) g_nextMood = now + 2200;
  }
  if (g_bleJustOut) {
    g_bleJustOut = false;
    g_auto = true;                              // 手机走了，恢复自己演
    g_nextMood = now + 1500;
    g_nextDir  = now + 600;
    Serial.println("BLE disconnected");
  }

  autoBehavior(now);

  /* 眼先头后：眼睛已经转过去 100~160ms 了，头才开始跟。
     水平（pan）和竖直（tilt）一起动 —— 以前只动 pan，所以只会左右摆头。 */
  if (g_auto && g_neckPend && (int32_t)(now - g_neckLag) >= 0) {
    g_neckPend = false;
    g_neckTgt  = g_pendPan;
    g_tiltTgt  = g_pendTilt;
  }

  uint32_t dt = now - lastFrameMs;
  if (dt < (1000 / TARGET_FPS)) return;
  lastFrameMs = now;

  g_faceStepMs = (float)dt;
  g_faceT += dt * 60.0f / 1000.0f;    // 虚拟 60fps 帧号，动画节拍全靠它

  /* 脖子：目标角 = 命令/巡视给的角 + 摇头表情的摆动。
     摆动来的时候必须"直接跟"，不能过平滑 —— swing 是 0.35 秒内左右甩完的，
     用 0.2 秒的时间常数去追，幅度会被吃掉一半，看着像头懒得动。 */
  float sw = faceNeckSwingDeg();
  float neckTarget = constrain(g_neckTgt + sw, (float)g_panLo, (float)g_panHi);
  if (fabsf(sw) > 0.05f) {
    if (fabsf(neckTarget - g_neckPos) > 0.2f) {
      g_neckPos = neckTarget;
      servoWriteDeg(PIN_NECK, (int)lroundf(g_neckPos));
      faceSetNeckPan(g_neckPos - (float)g_panHome);
    }
  } else if (fabsf(neckTarget - g_neckPos) > 0.05f) {
    g_neckPos += (neckTarget - g_neckPos) * (1.0f - powf(0.86f, dt * 60.0f / 1000.0f));
    servoWriteDeg(PIN_NECK, (int)lroundf(g_neckPos));
    faceSetNeckPan(g_neckPos - (float)g_panHome);
  }

  /* 竖直轴：目标角 = 手动给的角 + 点头曲线。
     平滑系数分两档：点头正在演的时候用 0.72（跟手，不然 250ms 的一个来回
     会被平滑吃掉一半幅度，看着像"头懒得点"）；静止时用 0.86，跟脖子一致，
     慢慢回位才不像机械臂。 */
  float nodDeg = faceNeckNodDeg() * (float)TILT_DOWN_SIGN;
  /* 呼吸：±1.2°、约 4 秒一个来回。肉眼看不出在动，但头永远不是一块石头。
     静止的舵机看久了就是"关机了"，这一点点漂移是活物感的一半。 */
  float breath = g_auto ? 1.2f * sinf(g_faceT * 0.030f) : 0.0f;
  float tiltTarget = constrain(g_tiltTgt + nodDeg + breath, (float)g_tiltLo, (float)g_tiltHi);
  if (fabsf(tiltTarget - g_tiltPos) > 0.05f) {
    float k = (fabsf(nodDeg) > 0.05f) ? 0.72f : 0.86f;
    g_tiltPos += (tiltTarget - g_tiltPos) * (1.0f - powf(k, dt * 60.0f / 1000.0f));
    servoWriteDeg(PIN_TILT, (int)lroundf(g_tiltPos));
  }

  faceRender();

  /* 左上角帧率字样：开发期留着看得见性能，正式摆桌上用 f 0 关掉。
     关掉时连 setCursor/printf 都不走，省一点是一点。 */
  if (g_showFps) {
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.setCursor(2, 2);
    spr.printf("%2lu fps", (unsigned long)fpsShown);
  }

  spr.pushSprite(0, 0);

  fpsCount++;
  if (now - fpsLast >= 1000) {
    fpsLast = now;
    fpsShown = fpsCount;
    fpsCount = 0;
  }
}

/*
 * servo_cal.h —— 双舵机软限位（共享给 servo_test / 正式固件 / 合并固件）
 *
 * 为什么需要它：
 *   · TILT（抬头）受云台框架限制，物理到不了 180°，硬走会把壳顶变形、齿轮打滑
 *   · PAN（左右）实际行程约 200°，左右富余不对称（左约 10°、右约 7~8°）
 *
 * 设计原则：**只在物理安全区间内驱动**。
 *   PAN 富余的那 20° 一律不用 —— 驱动范围收在 0~180 之内再留余量，
 *   这样两端永远碰不到机械限位，不对称的问题自然消失，不需要折中。
 *
 * 角度定义：
 *   物理角 = 舵机自己的 0~180（对应脉宽 500~2500us），是「喂给舵机的数」
 *   逻辑角 = -90~+90，是「上层想让头转多少」，映射到物理区间内
 *
 * 标定方法：烧 servo_cal/，按提示一步步逼近限位，测完按 w 存进 NVS。
 * 存过之后这里的默认值就不再生效（NVS 优先）。
 */

#pragma once
#include <Arduino.h>
#include <Preferences.h>

struct ServoLimit {
  int lo;      // 物理安全下限（0~180）
  int hi;      // 物理安全上限（0~180）
  int home;    // 回中位（物理角），逻辑 0° 就落在这里
};

/* ---------------- 出厂默认值 ----------------
 * 未经实测，全部取保守值。实测后用 servo_cal/ 存 NVS，或直接改这里。
 *   TILT 默认 30~150：刻意收窄，第一次上电宁可抬不起来，也不要顶壳
 *   PAN  默认  4~176：两端各留 4°，避开左右不对称的富余差异
 */
static ServoLimit CAL_PAN  = {  4, 176, 90 };
static ServoLimit CAL_TILT = { 30, 150, 90 };

static inline int calClamp(const ServoLimit& c, int deg) {
  if (deg < c.lo) return c.lo;
  if (deg > c.hi) return c.hi;
  return deg;
}

/* 逻辑 -90~+90 → 物理角
 * 以 home 为支点，正负两侧各自按「该侧可用量」等比缩放。
 * 这样即使 lo/home 与 home/hi 不对称（TILT 常常是上抬少、下压多），
 * 逻辑 ±90 也刚好落在两端，不会某一侧先撞壳。
 */
static inline int calLogicalToPhys(const ServoLimit& c, int logical) {
  if (logical >  90) logical =  90;
  if (logical < -90) logical = -90;
  int up = c.hi - c.home;          // 正向（抬头）可用量
  int dn = c.home - c.lo;          // 负向（低头）可用量
  if (logical >= 0)
    return c.home + (int)lroundf((float)logical * up / 90.0f);
  return c.home + (int)lroundf((float)logical * dn / 90.0f);
}

/* 物理角 → 逻辑角（眼睛跟云台用它算偏移） */
static inline int calPhysToLogical(const ServoLimit& c, int deg) {
  int up = c.hi - c.home;
  int dn = c.home - c.lo;
  int d = deg - c.home;
  if (d >= 0) return up > 0 ? (int)lroundf((float)d * 90.0f / up) : 0;
  return dn > 0 ? (int)lroundf((float)d * 90.0f / dn) : 0;
}

/* ---------------- NVS 持久化 ----------------
 * 标定结果存 flash，掉电不丢。合并固件以后直接读，不用改源码重烧。
 */
static inline void calLoad() {
  Preferences p;
  if (!p.begin("servocal", true)) return;      // 只读打开，没存过就保持默认
  CAL_PAN.lo   = p.getInt("pLo",  CAL_PAN.lo);
  CAL_PAN.hi   = p.getInt("pHi",  CAL_PAN.hi);
  CAL_PAN.home = p.getInt("pHo",  CAL_PAN.home);
  CAL_TILT.lo  = p.getInt("tLo",  CAL_TILT.lo);
  CAL_TILT.hi  = p.getInt("tHi",  CAL_TILT.hi);
  CAL_TILT.home= p.getInt("tHo",  CAL_TILT.home);
  p.end();
}

static inline bool calSave() {
  Preferences p;
  if (!p.begin("servocal", false)) return false;
  p.putInt("pLo", CAL_PAN.lo);   p.putInt("pHi", CAL_PAN.hi);   p.putInt("pHo", CAL_PAN.home);
  p.putInt("tLo", CAL_TILT.lo);  p.putInt("tHi", CAL_TILT.hi);  p.putInt("tHo", CAL_TILT.home);
  p.end();
  return true;
}

static inline void calErase() {
  Preferences p;
  if (p.begin("servocal", false)) { p.clear(); p.end(); }
}

static inline void calPrint(const char* name, const ServoLimit& c) {
  Serial.printf("  %-4s 物理 %3d~%3d  回中 %3d  (上抬可用 %d°, 下压可用 %d°)\n",
                name, c.lo, c.hi, c.home, c.hi - c.home, c.home - c.lo);
}

/*
 * servo_drv.h —— 舵机底层驱动（LEDC 50Hz 14bit）
 *
 * ★ 2026-11-15：原来写的 16bit 在 ESP32-S3 上必然失败 ——
 *   SOC_LEDC_TIMER_BIT_WIDTH = 14，16 > 14，ledcAttach 直接返回 false，
 *   引脚上永远 0V，表现就是舵机一动不动、且串口不报错。
 *   满量程相应从 65535 改成 16383。
 *
 * 与 esp32_robot.ino 里的 servoWrite 完全一致，脉宽公式也一样：
 *   duty = (500 + deg*2000/180) * 16383 / 20000
 * 也就是说 0°→500us、180°→2500us，标准 SG90 行程。
 *
 * 刻意不在这里做限位：限位是 servo_cal.h 的事。
 * 底层只负责"把角度变成脉冲"，上层负责"这个角度该不该给"。
 */

#pragma once
#include <Arduino.h>

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  #define LEDC_ATTACH(pin, freq, res)  ledcAttach(pin, freq, res)
  #define LEDC_WRITE(pin, duty)        ledcWrite(pin, duty)
#else
  static int _ledcCh = 0;
  static inline void ledcAttachCompat(int pin, int freq, int res) {
    ledcSetup(_ledcCh, freq, res); ledcAttachPin(pin, _ledcCh); _ledcCh++;
  }
  #define LEDC_ATTACH(pin, freq, res)  ledcAttachCompat(pin, freq, res)
  #define LEDC_WRITE(pin, duty)        ledcWrite(pin, duty)
#endif

/* 直接把物理角变成脉冲。调用前必须已 attach。 */
static inline void servoWriteDeg(int pin, int deg) {
  deg = constrain(deg, 0, 180);
  uint32_t duty = (uint32_t)((500L + deg * 2000L / 180L) * 16383L / 20000L);
  LEDC_WRITE(pin, duty);
}

static inline void servoAttach(int pin) {
  LEDC_ATTACH(pin, 50, 14);
}

/* 停止输出脉冲：舵机失去保持力矩，变"软"，不发热、不较劲。
   标定时很关键 —— 卡住了能立刻松手，而不是一直顶着壳。 */
static inline void servoRelease(int pin) {
  LEDC_WRITE(pin, 0);
}

/* 平滑走到目标角，每 stepMs 走 1°。
   返回 false 表示中途被串口输入打断（避免长距离扫动时没法回话）。 */
static inline bool servoGlideTo(int pin, int from, int to, int& outDeg, int stepMs = 15) {
  int step = (to >= from) ? 1 : -1;
  for (int d = from; d != to + step; d += step) {
    int v = constrain(d, 0, 180);
    servoWriteDeg(pin, v);
    outDeg = v;
    delay(stepMs);
    if (Serial.available()) return false;
  }
  return true;
}

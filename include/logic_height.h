#pragma once
#include <cmath>
#include <stdint.h>
#include "config.h"
#include "logic_median.h"

// Echo width and sample array -> water height. Pure arithmetic, no Arduino: the
// pinging lives in main.cpp, the numbers live here so they can be tested. This
// is the most safety-critical arithmetic in the firmware -- it decides what the
// operator is told the water is doing -- and it was untestable while it sat
// inline in the read loop.

// SOUND_CM_PER_US is the FULL speed of sound; the /2 is what halves it for the
// round trip. One definition, shared by the reading path and the boot
// diagnostic: while each carried its own literal, changing one left the
// diagnostic quietly reporting a different distance than the reading.
//
// 340 m/s is dry air at ~14 C. At a tropical 30 C sound travels 349.5 m/s, so
// every raw reading comes back ~2.8% short. Over this rig's narrow working span
// (35-40 cm of air) that is a near-constant ~1 cm, which is exactly what
// ULTRA_OFFSET_CM absorbs -- so calibrate the offset rather than compensating
// temperature here. An uncalibrated offset of 0 means ~1 cm of phantom water,
// against a WASPADA_ENTER of 3 cm.
static const float SOUND_CM_PER_US = 0.034f;

inline float usToCm(unsigned long w) { return (w * SOUND_CM_PER_US) / 2.0f; }

struct Ketinggian {
  float jarak_cm;
  float tinggi_cm;
  bool valid;
  const char *reason;   // "" | "timeout" | "too_close" | "out_of_range"
};

// s[0..n) are per-ping distances in cm; n is how many pings returned an echo.
// Distinguishes "too close" (blind zone) from "safe" -- v1.4 clamped a too-close
// reading to 0 cm, which reads as SAFE at the single most dangerous moment.
inline Ketinggian heightFrom(float *s, int n) {
  Ketinggian out;

  if (n <= 0) {
    out.jarak_cm = NAN; out.tinggi_cm = NAN; out.valid = false; out.reason = "timeout";
    return out;
  }

  // Correct to a true distance first, so JARAK_DASAR and SENSOR_BLIND_CM stay
  // in tape-measure units instead of this module's skewed ones.
  float jarak = medianOf(s, n) + ULTRA_OFFSET_CM;
  out.jarak_cm = jarak;

  if (jarak < SENSOR_BLIND_CM) {
    out.tinggi_cm = NAN; out.valid = false; out.reason = "too_close";
    return out;
  }
  // ULTRA_RANGE_SLACK_CM of slack, not 20 cm: the clamp below turns any
  // over-range distance into
  // 0 cm of water with valid=1, and 0 reads as "dry bed" -- the safest-looking
  // number this system can print. 5 cm covers measurement noise around an empty
  // rig; 60 cm on a 40 cm mount is a bad echo, and must be rejected, not floored.
  if (jarak > JARAK_DASAR + ULTRA_RANGE_SLACK_CM) {
    out.tinggi_cm = NAN; out.valid = false; out.reason = "out_of_range";
    return out;
  }

  float tinggi = JARAK_DASAR - jarak;
  out.tinggi_cm = (tinggi < 0) ? 0 : tinggi;
  out.valid = true;
  out.reason = "";
  return out;
}

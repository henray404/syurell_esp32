#pragma once
#include <stdint.h>
#include "config.h"

enum Level { AMAN = 0, WASPADA = 1, BAHAYA = 2 };

// Alert level with hysteresis and an asymmetric dwell time.
//
// v1.4 used one threshold for both directions, so water rippling around 30 cm
// flipped the state repeatedly and chattered the pump relay. Here entering and
// leaving a state use different thresholds, and de-escalation additionally has to
// stay stable for DWELL_DOWN_MS.
//
// Escalation is immediate and de-escalation is delayed on purpose: safety must
// not wait, but standing down should be sure.
class LevelFsm {
 public:
  LevelFsm() : cur_(AMAN), since_(0), pending_(AMAN), have_pending_(false) {}

  Level update(float height_cm, bool height_valid, float mm_per_hour, uint32_t now_ms) {
    // A failed reading is not evidence of safety. Hold, so a sensor fault cannot
    // switch the pump off mid-flood. Rain alone may still escalate.
    Level want = height_valid ? levelFor(height_cm, mm_per_hour)
                              : maxLevel(cur_, levelForRain(mm_per_hour));

    if (want > cur_) {                     // escalate now
      cur_ = want;
      have_pending_ = false;
      return cur_;
    }
    if (want == cur_) {
      have_pending_ = false;
      return cur_;
    }

    // want < cur_: candidate de-escalation, must persist for the dwell window.
    if (!have_pending_ || want != pending_) {
      pending_ = want;
      have_pending_ = true;
      since_ = now_ms;
      return cur_;
    }
    if (now_ms - since_ >= DWELL_DOWN_MS) {
      cur_ = pending_;
      have_pending_ = false;
    }
    return cur_;
  }

  Level current() const { return cur_; }

  static const char *name(Level l) {
    return l == BAHAYA ? "BAHAYA" : (l == WASPADA ? "WASPADA" : "AMAN");
  }

 private:
  static Level maxLevel(Level a, Level b) { return a > b ? a : b; }

  static Level levelForRain(float mm_per_hour) {
    if (mm_per_hour > RAIN_BAHAYA) return BAHAYA;
    if (mm_per_hour > RAIN_WASPADA) return WASPADA;
    return AMAN;
  }

  // Which level the raw inputs justify. Exit thresholds are applied here so a
  // state only drops once the reading is clearly below, not merely at, the edge.
  Level levelFor(float h, float mm_per_hour) const {
    Level byRain = levelForRain(mm_per_hour);
    Level byHeight;
    if (cur_ == BAHAYA)       byHeight = (h >= BAHAYA_EXIT)  ? BAHAYA
                                       : (h >= WASPADA_EXIT) ? WASPADA : AMAN;
    else if (cur_ == WASPADA) byHeight = (h > BAHAYA_ENTER)  ? BAHAYA
                                       : (h >= WASPADA_EXIT) ? WASPADA : AMAN;
    else                      byHeight = (h > BAHAYA_ENTER)  ? BAHAYA
                                       : (h > WASPADA_ENTER) ? WASPADA : AMAN;
    return maxLevel(byHeight, byRain);
  }

  Level cur_;
  uint32_t since_;
  Level pending_;
  bool have_pending_;
};

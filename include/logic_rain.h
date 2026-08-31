#pragma once
#include <stdint.h>
#include <string.h>
#include "config.h"

// Rainfall over a rolling RAIN_WINDOW_MIN-minute window, as one bin per minute.
//
// Replaces v1.4's `tips_this_minute * mm_per_tip * 60`, which extrapolated a
// single minute to an hour and reported 36 mm/h for two tips -- enough to trip a
// 30 mm/h threshold on a splash. Summing 60 one-minute bins IS the last hour's
// rainfall, no extrapolation involved.
class RainWindow {
 public:
  RainWindow() : head_(0) { memset(bins_, 0, sizeof(bins_)); }

  void addTips(uint16_t n) { bins_[head_] = (uint16_t)(bins_[head_] + n); }

  void advanceMinute() {
    head_ = (uint8_t)((head_ + 1) % kBins);
    bins_[head_] = 0;          // the bin rotating in is 60 minutes old; clear it
  }

  // Rain that fell inside the window, expressed as an hourly rate. The window
  // is shorter than an hour, so the total is scaled up to match -- measuring
  // intensity over a short interval, not extrapolating a single minute.
  float mmPerHour(float mmPerTip) const {
    uint32_t total = 0;
    for (uint8_t i = 0; i < kBins; ++i) total += bins_[i];
    return (float)total * mmPerTip * (60.0f / (float)kBins);
  }

  uint16_t lastMinuteTips() const { return bins_[head_]; }

 private:
  static const uint8_t kBins = RAIN_WINDOW_MIN;
  uint16_t bins_[kBins];
  uint8_t head_;
};

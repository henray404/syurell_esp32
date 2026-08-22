#pragma once
#include <stdint.h>
#include <stdio.h>
#include "logic_level.h"

// One logged minute. Raw measurements only -- rate of rise, acceleration and
// distance-to-threshold are deliberately NOT stored, because they are all
// recomputable from this series. Baking them in would break comparability with
// already-collected data the first time a window or smoothing choice changed.
struct LogRow {
  const char *ts_utc = "";
  uint32_t ts_epoch;
  float jarak_cm;
  float tinggi_cm;
  bool valid;
  uint8_t n_sampel;      // valid readings behind tinggi_cm: data quality in the record
  uint32_t tip_total;
  uint16_t tip_menit;
  float mm_per_jam;
  Level level;
  uint8_t pompa;
  const char *time_src = "";  // "rtc" | "ntp" | "none"
  int16_t rssi;
  const char *sms_status = "";
};

static const char *CSV_HEADER __attribute__((unused)) =
    "ts_utc,ts_epoch,jarak_cm,tinggi_cm,valid,n_sampel,tip_total,tip_menit,"
    "mm_per_jam,level,pompa,time_src,rssi,sms_status\n";

inline int formatRow(char *buf, int cap, const LogRow &r) {
  return snprintf(buf, (size_t)cap, "%s,%lu,%.1f,%.1f,%d,%u,%lu,%u,%.1f,%s,%u,%s,%d,%s\n",
                  r.ts_utc, (unsigned long)r.ts_epoch, r.jarak_cm, r.tinggi_cm,
                  r.valid ? 1 : 0, (unsigned)r.n_sampel, (unsigned long)r.tip_total,
                  (unsigned)r.tip_menit, r.mm_per_jam, LevelFsm::name(r.level),
                  (unsigned)r.pompa, r.time_src, (int)r.rssi, r.sms_status);
}

#pragma once
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "config.h"
#include "logic_csv.h"
#include "hw_time.h"

// SD is the source of truth. Rows are never deleted, and no upload outcome can
// remove or skip one. One file per day, so a corrupt file costs a day rather
// than the whole archive.
static char g_logPath[32];
static bool g_sdOk = false;

inline bool loggerBegin() {
  // Explicit pins: the ESP32's default VSPI SCK (GPIO18) is ECHO_PIN. Letting
  // SD.begin() fall back to the default bus would silently steal the
  // ultrasonic echo pin for SPI clock. See config.h's SD_SCK_PIN comment.
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  g_sdOk = SD.begin(SD_CS_PIN, SPI);
  if (g_sdOk) SD.mkdir("/data");
  return g_sdOk;
}

inline const char *currentLogPath() {
  const char *iso = nowIsoUtc();            // "YYYY-MM-DDTHH:MM:SSZ"
  snprintf(g_logPath, sizeof(g_logPath), "/data/%.10s.csv", iso);
  return g_logPath;
}

inline bool logRow(const LogRow &r) {
  if (!g_sdOk) return false;
  const char *path = currentLogPath();
  bool isNew = !SD.exists(path);

  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  if (isNew) f.print(CSV_HEADER);           // header once, at file creation

  char line[256];
  int n = formatRow(line, sizeof(line), r);
  if (n <= 0) { f.close(); return false; }
  size_t written = f.print(line);
  f.close();
  return written == (size_t)n;
}

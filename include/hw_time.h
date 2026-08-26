#pragma once
#include <Arduino.h>
#include <RTClib.h>
#include <time.h>

// All times UTC. Asia/Jakarta is UTC+7 with no DST, which makes local
// timestamps look harmless right up until they are joined against a UTC
// rainfall series and every correlation is silently shifted seven hours.
static RTC_DS3231 g_rtc;
static bool g_rtcOk = false;
static bool g_ntpOk = false;
static char g_isoBuf[24];

inline void timeBegin() {
  g_rtcOk = g_rtc.begin();
  if (!g_rtcOk) {
    Serial.println("[RTC] tidak terdeteksi di I2C (cek wiring SDA=21/SCL=22, VCC, GND)");
    return;
  }
  if (g_rtc.lostPower()) {
    // Battery dead or first power-up: the RTC value is meaningless. Leave it
    // marked bad rather than trusting it; NTP will correct it if WiFi appears.
    Serial.println("[RTC] terdeteksi tapi lostPower=true (baterai CR2032 soak/belum pernah di-set waktu)");
    g_rtcOk = false;
  }
}

// Call only when WiFi is connected.
inline bool timeSyncNtp() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");   // 0 offset = UTC
  struct tm t;
  if (!getLocalTime(&t, 10000)) return false;
  time_t epoch = mktime(&t);
  if (epoch < 1700000000L) return false;                 // sanity: before 2023 is wrong
  g_rtc.adjust(DateTime((uint32_t)epoch));
  g_rtcOk = true;
  g_ntpOk = true;
  return true;
}

inline uint32_t nowEpochUtc() {
  if (g_rtcOk) return g_rtc.now().unixtime();
  return 0;
}

inline const char *timeSource() {
  if (g_ntpOk) return "ntp";
  if (g_rtcOk) return "rtc";
  return "none";
}

// Returns "1970-01-01T00:00:00Z" when time is unknown, paired with
// timeSource()=="none" in the same row so a reader can tell a real timestamp
// from a placeholder. The station still logs: data with a known-doubtful
// timestamp is usable, data silently given a fake one is poison.
inline const char *nowIsoUtc() {
  time_t e = (time_t)nowEpochUtc();
  struct tm *g = gmtime(&e);
  strftime(g_isoBuf, sizeof(g_isoBuf), "%Y-%m-%dT%H:%M:%SZ", g);
  return g_isoBuf;
}

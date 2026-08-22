#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include "config_secrets.h"
#include "hw_logger.h"

// Upload is a convenience; SD is the source of truth. The cursor advances ONLY
// after the server confirms, so a network failure can never lose a row -- rows
// accumulate on the card and go out later.
static const char *kCursorPath = "/upload.cur";

inline void uploadBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

inline bool wifiUp() { return WiFi.status() == WL_CONNECTED; }

inline uint32_t readCursor() {
  File f = SD.open(kCursorPath, FILE_READ);
  if (!f) return 0;
  uint32_t v = (uint32_t)f.parseInt();
  f.close();
  return v;
}

inline void writeCursor(uint32_t v) {
  SD.remove(kCursorPath);
  File f = SD.open(kCursorPath, FILE_WRITE);
  if (!f) return;
  f.print(v);
  f.close();
}

// Sends up to max_rows unsent rows from today's file as JSON.
// Returns rows accepted, 0 if nothing to do, -1 on failure.
inline int uploadBatch(int max_rows) {
  if (!wifiUp()) return -1;

  const char *path = currentLogPath();
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;

  uint32_t cursor = readCursor();
  if (cursor > f.size()) cursor = 0;      // file rotated to a new day
  f.seek(cursor);

  String body = "{\"device\":\"";
  body += DEVICE_ID;
  body += "\",\"rows\":[";
  int count = 0;
  uint32_t newCursor = cursor;

  while (f.available() && count < max_rows) {
    String line = f.readStringUntil('\n');
    uint32_t after = f.position();
    if (line.length() == 0 || line.startsWith("ts_utc")) { newCursor = after; continue; }
    if (count > 0) body += ",";
    body += "{\"csv\":\"";
    body += line;
    body += "\"}";
    newCursor = after;
    ++count;
  }
  f.close();
  body += "]}";

  if (count == 0) return 0;

  HTTPClient http;
  http.begin(INGEST_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  http.end();

  // Advance only on an explicit 2xx. Anything else leaves the cursor alone so
  // the same rows are retried.
  if (code >= 200 && code < 300) {
    writeCursor(newCursor);
    return count;
  }
  return -1;
}

// =============================================
// SISTEM PENCEGAH BANJIR - ESP32
// v2.0 - median filter, rolling rain window, hysteresis, ISR tip counting,
//        verified SMS, DS3231+NTP timekeeping, SD logging, WiFi upload.
// Design: docs/superpowers/specs/2026-08-15-esp32-logging-design.md
// =============================================

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"
#include "config_secrets.h"
#include "logic_median.h"
#include "logic_rain.h"
#include "logic_level.h"
#include "logic_csv.h"
#include "hw_time.h"
#include "hw_logger.h"
#include "hw_upload.h"

// --- SIM800L ---
HardwareSerial sim800(2);

// --- STATE ---
static RainWindow rainWindow;
static LevelFsm    levelFsm;
static Level       lastLevel = AMAN;
static uint32_t    tipTotal  = 0;
static const char *lastSmsStatus = "";
static bool         ntpEverSynced = false;

// Readings accumulated within the current logging minute, reduced to a median
// at log time -- so the logged height is smoothed over the whole minute, not
// just whichever 5-second sample happened to land on the minute boundary.
static float   minuteSamples[MAX_MIN_SAMPLES];
static uint8_t minuteCount = 0;
static float   lastJarak = NAN;

// Rain tips are counted in the ISR with a debounce, not flagged for the loop to
// notice. v1.4 set a boolean and processed it once per loop; with a blocking
// delay(5000) plus a ~4s blocking SMS send, every tip inside that window
// collapsed into one -- undercounting hardest exactly when rain is heaviest.
static volatile uint32_t g_tipCount  = 0;
static volatile uint32_t g_lastTipUs = 0;

void IRAM_ATTR onTip() {
  uint32_t now = micros();
  if (now - g_lastTipUs > TIP_DEBOUNCE_US) {   // reed switches bounce for tens of ms
    g_tipCount++;
    g_lastTipUs = now;
  }
}

static uint32_t takeTipCount() {
  noInterrupts();
  uint32_t n = g_tipCount;
  g_tipCount = 0;
  interrupts();
  return n;
}

// --- SCHEDULER (millis, non-blocking) ---
static uint32_t tSensor = 0, tLog = 0, tUpload = 0, tNtp = 0, tSms = 0;



// =============================================
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RAIN_SENSOR_PIN, INPUT);
  pinMode(RAIN_GAUGE_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);

  attachInterrupt(digitalPinToInterrupt(RAIN_GAUGE_PIN), onTip, FALLING);

  sim800.begin(9600, SERIAL_8N1, 16, 17);
  delay(3000);

  timeBegin();
  if (!loggerBegin()) Serial.println("[WARN] SD tidak terdeteksi - data TIDAK akan tersimpan");
  uploadBegin();

  Serial.println("=== SISTEM BANJIR v2.0 AKTIF ===");
}

// =============================================
// BACA JARAK - JSN-SR04T (single ping)
// =============================================
static float pingOnce() {
  // 20 us trigger, not the datasheet-minimum 10 us: on this JSN-SR04T a 10 us
  // pulse produced no echo at all, a 20 us one reads every time.
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(20);
  digitalWrite(TRIG_PIN, LOW);

  // Poll the edges instead of pulseIn(); pulseIn() never caught this module's
  // echo on this board even when the pulse was demonstrably there.
  unsigned long tS = micros(), tRise = 0, tFall = 0;
  while (micros() - tS < ULTRA_ECHO_TIMEOUT_US) {
    if (digitalRead(ECHO_PIN)) { tRise = micros(); break; }
  }
  if (!tRise) return NAN;
  while (micros() - tRise < ULTRA_ECHO_TIMEOUT_US) {
    if (!digitalRead(ECHO_PIN)) { tFall = micros(); break; }
  }
  if (!tFall) return NAN;

  return ((tFall - tRise) * 0.034f) / 2.0f;
}

// =============================================
// BACA KETINGGIAN - median of ULTRA_SAMPLES pings
// A single wild reflection off moving water drags a mean but barely moves a
// median. Also distinguishes "too close" (blind zone) from "safe" -- v1.4
// clamped a too-close reading to 0 cm, which reads as SAFE at the single most
// dangerous moment.
// =============================================
struct Ketinggian {
  float jarak_cm;
  float tinggi_cm;
  bool valid;
  const char *reason;   // "" | "timeout" | "too_close" | "out_of_range"
};

static Ketinggian bacaKetinggian() {
  float s[ULTRA_SAMPLES];
  int n = 0;
  Serial.print("Ultrasonik    : ");
  for (int i = 0; i < ULTRA_SAMPLES; ++i) {
    float d = pingOnce();
    if (!isnan(d)) { s[n++] = d; Serial.print(d, 1); Serial.print(" "); }
    else             Serial.print("x ");   // x = ping ini timeout, bukan sensor mati total
    delay(ULTRA_PING_GAP_MS);   // module needs a full cycle before the next trigger
  }
  Serial.print("cm ("); Serial.print(n); Serial.print("/"); Serial.print(ULTRA_SAMPLES); Serial.println(" valid)");

  Ketinggian out;
  if (n == 0) {
    out.jarak_cm = NAN; out.tinggi_cm = NAN; out.valid = false; out.reason = "timeout";
    return out;
  }

  float jarak = medianOf(s, n);
  out.jarak_cm = jarak;

  if (jarak < SENSOR_BLIND_CM) {
    out.tinggi_cm = NAN; out.valid = false; out.reason = "too_close";
    return out;
  }
  if (jarak > JARAK_DASAR + 20.0f) {
    out.tinggi_cm = NAN; out.valid = false; out.reason = "out_of_range";
    return out;
  }

  float tinggi = JARAK_DASAR - jarak;
  out.tinggi_cm = (tinggi < 0) ? 0 : tinggi;
  out.valid = true;
  out.reason = "";
  return out;
}

// =============================================
// SMS - waits for the modem's actual reply instead of assuming success.
// v1.4 printed "[SMS] Terkirim" unconditionally without reading a single byte
// of the response, so a SIM with no credit or no signal reported success --
// a false confirmation on a safety system.
// =============================================
static bool waitFor(const char *want, uint32_t timeout_ms) {
  uint32_t start = millis();
  String acc;
  while (millis() - start < timeout_ms) {
    while (sim800.available()) {
      acc += (char)sim800.read();
      if (acc.indexOf(want) >= 0) return true;
      if (acc.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

// Returns "ok", "no_modem", "no_prompt", or "send_failed".
static const char *kirimSMS(const String &pesan) {
  sim800.println("AT");
  if (!waitFor("OK", 2000)) return "no_modem";

  sim800.println("AT+CMGF=1");
  if (!waitFor("OK", 2000)) return "no_modem";

  sim800.print("AT+CMGS=\"");
  sim800.print(NOMOR_TUJUAN);
  sim800.println("\"");
  if (!waitFor(">", 5000)) return "no_prompt";

  sim800.print(pesan);
  sim800.write(26);   // CTRL+Z ends the message
  if (!waitFor("+CMGS", 20000)) return "send_failed";
  return "ok";
}

// =============================================
// LOOP
// =============================================
void loop() {
  uint32_t now = millis();

  // ---- sensors + decision, every SENSOR_PERIOD_MS ----
  if (now - tSensor >= SENSOR_PERIOD_MS) {
    tSensor = now;

    bool hujanAda = (digitalRead(RAIN_SENSOR_PIN) == LOW);
    Ketinggian k  = bacaKetinggian();

    lastJarak = k.jarak_cm;
    if (k.valid && minuteCount < MAX_MIN_SAMPLES) minuteSamples[minuteCount++] = k.tinggi_cm;

    uint32_t tips = takeTipCount();
    if (tips) { rainWindow.addTips((uint16_t)tips); tipTotal += tips; }
    float mmPerJam = rainWindow.mmPerHour(MM_PER_TIP);

    Level lv = levelFsm.update(k.valid ? k.tinggi_cm : 0.0f, k.valid, mmPerJam, now);

    // --- Serial Monitor ---
    Serial.println("----------------------------");
    Serial.print("WiFi          : ");
    if (wifiUp()) { Serial.print("connected, RSSI "); Serial.println(WiFi.RSSI()); }
    else          { Serial.print("belum connect (status="); Serial.print(WiFi.status()); Serial.println(")"); }
    Serial.print("Hujan(sensor) : "); Serial.println(hujanAda ? "YA" : "TIDAK");
    if (k.valid) {
      Serial.print("Air           : "); Serial.print(k.tinggi_cm); Serial.println(" cm");
    } else {
      Serial.print("Air           : INVALID ("); Serial.print(k.reason); Serial.println(")");
    }
    Serial.print("Curah         : "); Serial.print(mmPerJam); Serial.println(" mm/jam");
    Serial.print("Status        : "); Serial.println(LevelFsm::name(lv));

    // Pump follows the level, driven on state change only -- not every loop
    // iteration, and the hysteresis in LevelFsm is what stops relay chatter.
    if (lv != lastLevel) {
      digitalWrite(RELAY_PIN, lv >= WASPADA ? HIGH : LOW);
    }

    // SMS: immediate, once, on every state transition (including back to
    // AMAN -- the FSM's hysteresis + dwell already prevent the chatter that
    // made v1.4's unlimited AMAN notification a spam/cost problem). While a
    // dangerous state persists, repeat at most every SMS_REPEAT_MS.
    bool shouldSend = (lv != lastLevel) ||
                       (lv != AMAN && now - tSms >= SMS_REPEAT_MS);
    if (shouldSend) {
      String pesan;
      if (lv == AMAN) {
        pesan = "[NORMAL] Sistem Banjir\nKondisi kembali aman.\nAir: " +
                (k.valid ? String(k.tinggi_cm, 1) : String("N/A")) +
                " cm | Pompa nonaktif.";
      } else {
        pesan = "[" + String(LevelFsm::name(lv)) + "] Sistem Banjir\nAir: " +
                (k.valid ? String(k.tinggi_cm, 1) : String("N/A")) +
                " cm | Hujan: " + String(mmPerJam, 1) +
                " mm/jam\nPompa " + (lv >= WASPADA ? "aktif." : "nonaktif.");
      }
      lastSmsStatus = kirimSMS(pesan);
      Serial.print("[SMS] Status: "); Serial.println(lastSmsStatus);
      tSms = now;
    }

    lastLevel = lv;
  }

  // ---- log one row per minute; also closes the rolling rain-window bin ----
  if (now - tLog >= LOG_PERIOD_MS) {
    tLog = now;

    LogRow row;
    row.ts_utc     = nowIsoUtc();
    row.ts_epoch   = nowEpochUtc();
    row.jarak_cm   = lastJarak;
    row.valid      = minuteCount > 0;
    row.tinggi_cm  = row.valid ? medianOf(minuteSamples, minuteCount) : 0.0f;
    row.n_sampel   = minuteCount;
    row.tip_total  = tipTotal;
    row.tip_menit  = rainWindow.lastMinuteTips();
    row.mm_per_jam = rainWindow.mmPerHour(MM_PER_TIP);
    row.level      = levelFsm.current();
    row.pompa      = (levelFsm.current() >= WASPADA) ? 1 : 0;
    row.time_src   = timeSource();
    row.rssi       = wifiUp() ? (int16_t)WiFi.RSSI() : 0;
    row.sms_status = lastSmsStatus;

    // Echo the exact CSV row to Serial too -- so time_src, rssi, tip_total,
    // pompa and sms_status are visible on the bench even without an SD card
    // or a way to read it off the field.
    char csvLine[256];
    formatRow(csvLine, sizeof(csvLine), row);
    Serial.print("[LOG] "); Serial.print(csvLine);

    if (!logRow(row)) Serial.println("[WARN] gagal tulis log ke SD");

    rainWindow.advanceMinute();
    minuteCount = 0;
    lastSmsStatus = "";
  }

  // ---- upload backlog, every UPLOAD_PERIOD_MS ----
  if (now - tUpload >= UPLOAD_PERIOD_MS) {
    if (wifiUp()) {
      tUpload = now;
      int sent = uploadBatch(60);
      if (sent > 0)      { Serial.print("[UPLOAD] terkirim "); Serial.print(sent); Serial.println(" baris"); }
      else if (sent == 0) Serial.println("[UPLOAD] tidak ada baris baru untuk dikirim");
      else                Serial.println("[UPLOAD] gagal (server tidak merespons / bukan 2xx) - cursor tidak maju, dicoba lagi nanti");
    } else {
      Serial.println("[UPLOAD] dilewati - WiFi belum connect");
    }
  }

  // ---- NTP resync. Retry every 30s until the FIRST sync succeeds (tNtp==0
  // at boot would otherwise mean the first attempt waits a full
  // NTP_PERIOD_MS -- 6 hours -- before ever trying). Once synced, fall back
  // to the slow periodic resync that only corrects RTC drift. ----
  uint32_t ntpInterval = ntpEverSynced ? NTP_PERIOD_MS : 30000UL;
  if (now - tNtp >= ntpInterval) {
    tNtp = now;
    if (wifiUp() && timeSyncNtp()) ntpEverSynced = true;
  }
}

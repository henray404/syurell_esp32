// =============================================
// SISTEM PENCEGAH BANJIR - ESP32
// v2.0 - median filter, rolling rain window, hysteresis, ISR tip counting,
//        verified SMS, DS3231+NTP timekeeping, SD logging, WiFi upload.
// Design: docs/laporan/02-dokumentasi-teknis.md (repo syurel-website)
// =============================================

#include <Arduino.h>
#include <HardwareSerial.h>
#include "config.h"
#include "config_secrets.h"
#include "logic_median.h"
#include "logic_height.h"
#include "hw_ultra.h"
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
static uint32_t   tRainMinute = 0;   // rotates the rain bins on real time

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



#if MODE_SMS_TEST
static const char *kirimSMS(const String &pesan);   // defined below, used by the test block

// Sends one AT command and prints whatever the modem actually answers, raw.
// Printing the reply rather than a pass/fail verdict is the point: a new SIM
// fails for reasons the firmware cannot name -- PIN locked, not registered, no
// 2G left on the operator -- and the modem says which one in its own words.
static String atAsk(const char *cmd, uint32_t wait_ms) {
  while (sim800.available()) sim800.read();      // drop stale bytes
  sim800.println(cmd);
  String acc;
  uint32_t start = millis();
  while (millis() - start < wait_ms) {
    while (sim800.available()) acc += (char)sim800.read();
    if (acc.indexOf("OK") >= 0 || acc.indexOf("ERROR") >= 0) break;
  }
  acc.trim();
  Serial.print(cmd); Serial.print("  ->  ");
  Serial.println(acc.length() ? acc : String("(tidak ada jawaban)"));
  return acc;
}
#endif

// =============================================
// SETUP
// =============================================
void setup() {
  Serial.begin(115200);

  ultraBegin();
  pinMode(RAIN_SENSOR_PIN, INPUT);
  pinMode(RAIN_GAUGE_PIN, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, LOW);

  attachInterrupt(digitalPinToInterrupt(RAIN_GAUGE_PIN), onTip, FALLING);

  sim800.begin(9600, SERIAL_8N1, 16, 17);
  delay(3000);

#if MODE_SMS_TEST
  Serial.println();
  Serial.println("=== MODE SMS TEST - sistem banjir TIDAK jalan ===");
  Serial.println();

  // The module ships in autobaud: it locks its rate onto the first few "AT"s it
  // sees, and a module already locked elsewhere answers nothing at 9600. Sweep
  // the common rates and knock several times at each before calling it dead.
  long bauds[] = {9600, 115200, 38400, 57600, 19200, 4800};
  long found = 0;
  for (unsigned i = 0; i < sizeof(bauds)/sizeof(bauds[0]) && !found; i++) {
    sim800.begin(bauds[i], SERIAL_8N1, 16, 17);
    delay(300);
    Serial.print("coba baud "); Serial.print(bauds[i]); Serial.print(" ... ");
    for (int k = 0; k < 5 && !found; k++) {
      while (sim800.available()) sim800.read();
      sim800.println("AT");
      uint32_t t0 = millis();
      String acc;
      while (millis() - t0 < 600) {
        while (sim800.available()) acc += (char)sim800.read();
        if (acc.indexOf("OK") >= 0) { found = bauds[i]; break; }
      }
    }
    Serial.println(found ? "JAWAB" : "diam");
  }
  if (found) {
    sim800.begin(found, SERIAL_8N1, 16, 17);
    delay(200);
    Serial.print(">> Modem menjawab di baud "); Serial.println(found);
    if (found != 9600) {
      Serial.println("   Bukan 9600. Ubah sim800.begin(...) di main.cpp ke baud ini,");
      Serial.println("   atau kunci modul: AT+IPR=9600 lalu AT&W.");
    }
  }

  String r = found ? String("OK") : String("");
  if (r.indexOf("OK") < 0) {
    Serial.println(">> GAGAL: modem tidak menjawab.");
    Serial.println("   Cek power (butuh 4V, arus 2A) dan TX->GPIO16 / RX->GPIO17.");
    Serial.println("   GND modul WAJIB nyambung ke GND ESP32.");
  } else {
    atAsk("AT+CPIN?", 3000);   // harus READY
    atAsk("AT+CSQ", 2000);     // angka pertama 10-31; 99 = tidak ada sinyal
    atAsk("AT+CREG?", 3000);   // ,1 atau ,5 = terdaftar
    atAsk("AT+COPS?", 5000);   // nama operator
    atAsk("AT+CMGF=1", 2000);  // mode teks

    Serial.println();
    Serial.print("Kirim SMS tes ke "); Serial.println(NOMOR_TUJUAN);
    const char *st = kirimSMS("[TES] Sistem Banjir - SIM card baru OK. Abaikan pesan ini.");
    Serial.print(">> Hasil kirim: "); Serial.println(st);
    if (strcmp(st, "ok") == 0) {
      Serial.println("   SMS diterima jaringan. Cek HP tujuan.");
    } else {
      Serial.println("   Gagal. no_modem=wiring/power, no_prompt=modem tolak nomor,");
      Serial.println("   send_failed=pulsa/registrasi/sinyal 2G.");
    }
  }

  Serial.println();
  Serial.println("--- terminal AT manual, ketik perintah lalu Enter ---");
  Serial.println("Set MODE_SMS_TEST 0 di config.h untuk kembali normal.");
  for (;;) {                       // never returns: loop() is not reached
    while (Serial.available())  sim800.write(Serial.read());
    while (sim800.available())  Serial.write(sim800.read());
  }
#endif

  timeBegin();
  if (!loggerBegin()) Serial.println("[WARN] SD tidak terdeteksi - data TIDAK akan tersimpan");
  uploadBegin();

  diagUltrasonik();
#if MODE_SIMULASI
  Serial.println("*** MODE SIMULASI AKTIF - ketinggian air HARDCODE, sensor diabaikan ***");
#endif
  Serial.println("=== SISTEM BANJIR v2.0 AKTIF ===");
}

// =============================================
// BACA KETINGGIAN - median of ULTRA_SAMPLES pings
// A single wild reflection off moving water drags a mean but barely moves a
// median. Also distinguishes "too close" (blind zone) from "safe" -- v1.4
// clamped a too-close reading to 0 cm, which reads as SAFE at the single most
// dangerous moment.
// =============================================
static Ketinggian bacaKetinggian() {
#if MODE_SIMULASI
  // Printed every cycle on purpose: a hardcoded reading that goes quiet is a
  // hardcoded reading someone ships by accident.
  Serial.print("[SIMULASI] tinggi air dipaksa ");
  Serial.print(SIMULASI_TINGGI_CM, 1);
  Serial.println(" cm - sensor TIDAK dibaca");
  Ketinggian sim;
  sim.tinggi_cm = SIMULASI_TINGGI_CM;
  sim.jarak_cm  = JARAK_DASAR - SIMULASI_TINGGI_CM;   // what the sensor would report
  sim.valid     = true;
  sim.reason    = "";
  return sim;
#else
  float s[ULTRA_SAMPLES];
  int n = 0;
  Serial.print("Ultrasonik    : ");
  for (int i = 0; i < ULTRA_SAMPLES; ++i) {
    // The echo width is the only thing the module actually reports; every cm
    // below is derived from it. Print it under DEBUG_SENSOR, because a width
    // that is identical ping after ping is the module's ring-down rather than
    // an echo, and no distance in cm can show you that.
    unsigned long w = pingWidthUs();
    if (w) {
      s[n++] = usToCm(w);
#if DEBUG_SENSOR
      Serial.print(w); Serial.print("us/");
#endif
      Serial.print(usToCm(w), 1); Serial.print(" ");
    } else {
      Serial.print("x ");   // x = ping ini timeout, bukan sensor mati total
    }
    delay(ULTRA_PING_GAP_MS);   // module needs a full cycle before the next trigger
  }
  Serial.print("cm ("); Serial.print(n); Serial.print("/"); Serial.print(ULTRA_SAMPLES); Serial.println(" valid)");

  Ketinggian k = heightFrom(s, n);   // median, offset and range gates: logic_height.h

#if DEBUG_SENSOR
  // heightFrom() runs medianOf(), which sorts s in place, so after the call
  // s[0] and s[n-1] are the extremes. Reading them is why this block sits here
  // and not before the call.
  if (n > 0) {
    Serial.print("  sebaran     : min "); Serial.print(s[0], 1);
    Serial.print("  med "); Serial.print(s[n / 2], 1);
    Serial.print("  max "); Serial.print(s[n - 1], 1);
    Serial.print("  delta "); Serial.print(s[n - 1] - s[0], 2);
    Serial.println(" cm");
  }
  // Every step from median to verdict, so a wrong height can be traced to the
  // step that produced it instead of guessing between offset and gate.
  Serial.print("  jarak       : ");
  if (n > 0) { Serial.print(s[n / 2], 1); Serial.print(" + offset "); Serial.print(ULTRA_OFFSET_CM, 1);
               Serial.print(" = "); Serial.print(k.jarak_cm, 1); Serial.print(" cm"); }
  else       { Serial.print("- (tidak ada echo)"); }
  Serial.print("   gerbang "); Serial.print(SENSOR_BLIND_CM, 1);
  Serial.print("-"); Serial.print(JARAK_DASAR + ULTRA_RANGE_SLACK_CM, 1);
  Serial.print(" cm -> "); Serial.println(k.valid ? "OK" : k.reason);
  if (ULTRA_OFFSET_CM == 0.0f) {
    Serial.println("  [!] ULTRA_OFFSET_CM masih 0 - tinggi air bisa lebih ~1 cm dari asli");
  }
#endif

  return k;
#endif
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
    // Ordered raw-to-derived: what each sensor reported, then what the system
    // concluded from it. Under DEBUG_SENSOR every input carries its raw form
    // alongside the cooked one, so a wrong verdict can be traced to the reading
    // that caused it without reflashing.
    Serial.println("----------------------------");
    // Distance is what the sensor measures; height is derived from it. Printed
    // even when the height is rejected: "Jarak 22.0 / Air INVALID (too_close)"
    // says the module answered but out of range, which a bare INVALID does not.
    Serial.print("Jarak         : ");
    if (isnan(k.jarak_cm)) Serial.println("- (tidak ada echo)");
    else { Serial.print(k.jarak_cm, 1); Serial.println(" cm"); }
    if (k.valid) {
      Serial.print("Air           : "); Serial.print(k.tinggi_cm); Serial.println(" cm");
    } else {
      Serial.print("Air           : INVALID ("); Serial.print(k.reason); Serial.println(")");
    }
    Serial.print("Hujan(sensor) : "); Serial.print(hujanAda ? "YA" : "TIDAK");
#if DEBUG_SENSOR
    // The raw pin state, because "TIDAK" and "sensor unplugged" look identical
    // otherwise -- the rain board is active-LOW and a floating input reads HIGH.
    Serial.print("  (pin "); Serial.print(RAIN_SENSOR_PIN);
    Serial.print("="); Serial.print(digitalRead(RAIN_SENSOR_PIN)); Serial.print(", aktif-LOW)");
#endif
    Serial.println();
    Serial.print("Curah         : "); Serial.print(mmPerJam); Serial.println(" mm/jam");
#if DEBUG_SENSOR
    // Tip counters behind that number. mm/jam moving without tips moving means
    // the window arithmetic is at fault, not the bucket.
    Serial.print("  tipping     : +"); Serial.print(tips);
    Serial.print(" siklus ini   menit "); Serial.print(rainWindow.lastMinuteTips());
    Serial.print("   total "); Serial.print(tipTotal);
    Serial.print("   jendela "); Serial.print(RAIN_WINDOW_MIN); Serial.println(" menit");
#endif
    Serial.print("Status        : "); Serial.print(LevelFsm::name(lv));
    Serial.print("   pompa "); Serial.println(lv >= WASPADA ? "ON" : "OFF");
#if DEBUG_SENSOR
    Serial.print("  ambang      : waspada "); Serial.print(WASPADA_ENTER, 1);
    Serial.print("/"); Serial.print(WASPADA_EXIT, 1);
    Serial.print("  bahaya "); Serial.print(BAHAYA_ENTER, 1);
    Serial.print("/"); Serial.print(BAHAYA_EXIT, 1); Serial.println(" cm (masuk/keluar)");
    Serial.print("Waktu         : "); Serial.print(nowIsoUtc());
    Serial.print("  src="); Serial.print(timeSource());
    Serial.print("   uptime "); Serial.print(now / 1000); Serial.println(" s");
    Serial.print("SD            : "); Serial.println(loggerOk() ? "ok" : "TIDAK ADA - data tidak tersimpan");
#endif
    Serial.print("WiFi          : ");
    if (wifiUp()) { Serial.print("connected, RSSI "); Serial.println(WiFi.RSSI()); }
    else          { Serial.print("belum connect (status="); Serial.print(WiFi.status()); Serial.println(")"); }

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

    if (!logRow(row)) {
      // No card to replay from, so the row goes out now or not at all.
      Serial.println("[WARN] gagal tulis log ke SD - kirim langsung tanpa SD");
      if (uploadRowDirect(csvLine)) Serial.println("[UPLOAD-LANGSUNG] terkirim 1 baris");
    }

    minuteCount = 0;
    lastSmsStatus = "";
  }

  // ---- rain window, every real minute ----
  // On its own clock, not the logging one: the bins ARE minutes, so rotating
  // them at the log period would rescale the whole window whenever that period
  // changes -- and the debug build logs every 10 s, which would quietly turn the
  // 60-minute window into a 6-minute one and under-report mm/jam sixfold.
  if (now - tRainMinute >= 60000UL) {
    tRainMinute = now;
    rainWindow.advanceMinute();
  }

  // ---- upload backlog, every UPLOAD_PERIOD_MS ----
  if (now - tUpload >= UPLOAD_PERIOD_MS) {
    if (wifiUp()) {
      tUpload = now;
      int sent = uploadBatch(60);
      if (sent > 0)      { Serial.print("[UPLOAD] terkirim "); Serial.print(sent); Serial.println(" baris"); }
      else if (sent == 0) Serial.println("[UPLOAD] tidak ada baris baru untuk dikirim");
      else                Serial.println("[UPLOAD] gagal (server tidak merespons / bukan 2xx) - cursor tidak maju, dicoba lagi nanti");
    } 
    // else {
    //   // Serial.println("[UPLOAD] dilewati - WiFi belum connect");
    
    // }
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

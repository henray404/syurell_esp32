#pragma once

// ---- pins ----
// Pins match the physical wiring table (Perangkat / Pin modul / ESP32 38-pin).
#define TRIG_PIN 5
#define ECHO_PIN 18
#define RAIN_SENSOR_PIN 34 // rain sensor DO
#define RAIN_GAUGE_PIN 13  // tipping bucket reed switch signal (actual wiring; wiring-table photo said GPIO27)
#define RELAY_PIN 26
#define SD_CS_PIN 33
// SD is wired on GPIO-matrix SPI pins, NOT the ESP32's default VSPI pins
// (SCK=18/MISO=19/MOSI=23). Default SCK=18 would collide with ECHO_PIN:
// SD.begin() reconfiguring GPIO18 as an SPI clock output right after setup()
// sets it INPUT for the ultrasonic echo would time out pulseIn() on every
// read no matter how the sensor is wired. The wiring table's SCK=14 avoids
// that collision already; DS3231 SDA/SCL use the ESP32's default I2C pins
// (21/22), matched by RTClib's Wire.begin() with no args.
#define SD_SCK_PIN 14
#define SD_MISO_PIN 19
#define SD_MOSI_PIN 23

// ---- calibration: MEASURE THESE AT THE SITE ----
// Distance from the sensor face down to the riverbed, in cm.
// 35 cm on the rig, after 26.5 cm put the module right at the edge of its blind
// zone -- it reported 22-24 cm for a bed that measured 26.5. At 35 cm a full
// 5 cm of water still leaves 30 cm of air, clear of the 25 cm blind zone.
// Re-measure with a tape whenever the sensor is remounted, and recalibrate
// ULTRA_OFFSET_CM afterwards: this number and that one are a pair.
#define JARAK_DASAR 35.0f
// Back to the datasheet's 25 cm after the 20 cm value proved actively unsafe.
// When the module gets no usable echo it does not time out -- it emits its
// transducer ring-down as the echo, a fixed ~1294 us pulse that decodes to
// ~22 cm no matter where the sensor is mounted. At SENSOR_BLIND_CM 20 that
// artefact passed as a real distance, so a dry rig logged JARAK_DASAR - 22 cm
// of water: pump on, BAHAYA SMS, permanently. 25 cm rejects it as "too_close".
// A persistent "too_close" on a rig with 30 cm of air means the sensor is not
// hearing its target -- a wiring/power/aim problem, not a shallow one.
#define SENSOR_BLIND_CM 25.0f
// Usable depth = JARAK_DASAR - SENSOR_BLIND_CM = 10 cm, against the rig's 5 cm
// maximum water level. test_height() fails if a remount ever leaves this below
// BAHAYA_ENTER, i.e. if the sensor goes blind before the alert can be reached.

// ---- SIMULASI: sensor ultrasonik DILEWATI ----
// Feeds a fixed water height instead of reading the sensor, so the alert, pump,
// SMS, SD and upload paths can be exercised while the ultrasonic module is out
// of action. Set MODE_SIMULASI to 0 to go back to the real sensor -- that is the
// ONLY change needed; nothing else in the code branches on this.
// 4.6 cm is above BAHAYA_ENTER (4.5), so the system sits in BAHAYA: pump on and
// an SMS on the first cycle.
// OFF since 2026-08-28: the ultrasonic module is back in service, so the rig
// reads its real level again. Every row logged while this was 1 carries a
// constant tinggi_cm 4.6 and jarak_cm 35.4 -- those are the hardcoded values
// above, not measurements, and no row from that period may be quoted as one.
#define MODE_SIMULASI 0
#define SIMULASI_TINGGI_CM 4.6f

// ---- SMS_TEST: bridge Serial <-> SIM800L, then stop ----
// Set to 1 to bring the board up as a bare AT terminal: setup() runs the pins
// and the modem UART, then loops forwarding bytes both ways and never returns.
// No sensors, no SD, no upload, no automatic SMS -- the point is to see the
// modem's own replies to a new SIM before trusting the alert path with it.
// Set back to 0 for normal operation.
#define MODE_SMS_TEST 0

// ---- alert thresholds, cm (enter / exit differ: hysteresis) ----
// Sized for the rig's 0-5 cm water range. The 1 cm enter/exit gap is wider than
// the ~0.5 cm ping-to-ping spread measured on this sensor, so a noisy reading
// near a threshold will not flap the alert state.
#define WASPADA_ENTER 3.0f
#define WASPADA_EXIT 2.0f
#define BAHAYA_ENTER 4.5f
#define BAHAYA_EXIT 3.5f
#define DWELL_DOWN_MS 60000UL // must be stable this long before de-escalating

// ---- rain ----
#define MM_PER_TIP 0.30f
#define TIP_DEBOUNCE_US 250000UL // reed switches bounce for tens of ms
// Length of the rolling rain window, in minutes. A tip stops counting once it
// is this old, so the reading falls back to 0 this many minutes after the rain
// stops -- at 60 it took a full hour to clear, which reads as "stuck".
// Shorter reacts faster but is noisier: mm/jam is scaled up by 60/this, so at 10
// one stray tip becomes 1.8 mm/jam instead of 0.3. Do not go below ~5, or a
// single splash extrapolates into an alert the way v1.4's 1-minute version did.
#define RAIN_WINDOW_MIN 10

#define RAIN_WASPADA 10.0f // mm/hour
#define RAIN_BAHAYA 30.0f

// ---- scheduling ----
#if MODE_SIMULASI
// Debug cadence. Five minutes per server round-trip makes debugging unusable,
// and uploading faster than rows are logged sends nothing -- so both drop
// together. Reverts to the real periods the moment MODE_SIMULASI is 0.
#define SENSOR_PERIOD_MS 3000UL
#define LOG_PERIOD_MS 10000UL
#define UPLOAD_PERIOD_MS 15000UL
#else
#define SENSOR_PERIOD_MS 5000UL
#define LOG_PERIOD_MS 60000UL
// 60 s, down from 300 s. What the operator waits for is the SUM of the two
// periods above it: a reading waits up to LOG_PERIOD_MS to become a row, then up
// to UPLOAD_PERIOD_MS to be sent. At 300 s that was up to ~6 minutes of staleness
// on a screen people read to decide whether to open a gate.
//
// 60 s is the floor worth having, not a compromise: rows appear once a minute, so
// polling faster than LOG_PERIOD_MS wakes the radio to send nothing. Going lower
// means lowering LOG_PERIOD_MS too, and that one is load-bearing -- one row is one
// bin of the rolling rain window (rainWindow.advanceMinute() in main.cpp), so
// mm/jam is computed wrong if rows stop being one minute apart.
//
// Cost: the WiFi radio wakes 5x more often. Fine on mains power, worth revisiting
// if the site ever runs on solar.
#define UPLOAD_PERIOD_MS 60000UL
#endif
#define NTP_PERIOD_MS 21600000UL
#define SMS_REPEAT_MS 300000UL

// ---- serial debug ----
// 1 = the Serial monitor prints every sensor primitive each cycle: raw echo
// widths per ping, ping-to-ping spread, the median/offset/gate arithmetic, and
// the rain and link counters. Costs a few lines of output every SENSOR_PERIOD_MS
// and nothing else -- no behaviour branches on it.
// The spread line is the one that matters for the ultrasonic: a real water
// surface jitters a few mm between pings, while the module's ring-down artefact
// repeats to the microsecond. Identical widths mean it is not hearing the water.
// Set to 0 once the sensor is trusted, to get the short monitor back.
#define DEBUG_SENSOR 1

// ---- sampling ----
#define ULTRA_SAMPLES 5    // median of this many, per reading
#define MAX_MIN_SAMPLES 16 // readings buffered per logged minute

// ---- ultrasonic calibration ----
// Added to every raw reading before it means a real distance. Back to 0 when the
// sensor left the 26.5 cm mount: the old 2.1 cm patched a blind-zone artefact
// there, not a genuine offset, so carrying it over would just skew the new mount.
// RECALIBRATE whenever the sensor is remounted: empty the rig, then either read
// the "Ultrasonik" line of the firmware, or run the probe
// (pio run -e probe -t upload) and press 'z', which prints this number.
//
// This knob is not optional. usToCm() assumes 340 m/s (dry air ~14 C); at a
// tropical 30 C sound is 2.8% faster, so raw readings land ~1 cm short and
// tinggi_cm ~1 cm high -- a third of WASPADA_ENTER (3.0 cm) invented out of air
// temperature alone. Over this rig's narrow working span the error is
// near-constant, so this additive offset cancels it; 0.0f leaves it uncancelled.
#define ULTRA_OFFSET_CM 0.0f // UNCALIBRATED at the 35 cm mount -- measure it

// Slack above JARAK_DASAR still accepted as an empty rig rather than rejected.
// Shared by the range gate in logic_height.h and the debug print in main.cpp so
// the monitor can never show a window the code does not actually enforce.
#define ULTRA_RANGE_SLACK_CM 5.0f

// ---- ultrasonic timing ----
// 60 ms is the JSN-SR04T's minimum measurement cycle; trigger it sooner and it
// ignores the trigger. Timeout 30 ms ~= 5 m of range, well past the tank.
#define ULTRA_ECHO_TIMEOUT_US 30000UL
#define ULTRA_PING_GAP_MS 70

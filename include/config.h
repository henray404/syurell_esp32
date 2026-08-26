#pragma once

// ---- pins ----
// Pins match the physical wiring table (Perangkat / Pin modul / ESP32 38-pin).
#define TRIG_PIN         5
#define ECHO_PIN         18
#define RAIN_SENSOR_PIN  34   // rain sensor DO
#define RAIN_GAUGE_PIN   13   // tipping bucket reed switch signal (actual wiring; wiring-table photo said GPIO27)
#define RELAY_PIN        26
#define SD_CS_PIN        33
// SD is wired on GPIO-matrix SPI pins, NOT the ESP32's default VSPI pins
// (SCK=18/MISO=19/MOSI=23). Default SCK=18 would collide with ECHO_PIN:
// SD.begin() reconfiguring GPIO18 as an SPI clock output right after setup()
// sets it INPUT for the ultrasonic echo would time out pulseIn() on every
// read no matter how the sensor is wired. The wiring table's SCK=14 avoids
// that collision already; DS3231 SDA/SCL use the ESP32's default I2C pins
// (21/22), matched by RTClib's Wire.begin() with no args.
#define SD_SCK_PIN       14
#define SD_MISO_PIN      19
#define SD_MOSI_PIN      23

// ---- calibration: MEASURE THESE AT THE SITE ----
// Distance from the sensor face down to the riverbed, in cm.
// 100 is a placeholder. A wrong value makes every height wrong.
#define JARAK_DASAR      100.0f
// JSN-SR04T cannot measure closer than ~25 cm. Choose JARAK_DASAR so BAHAYA is
// crossed BEFORE water enters that zone, or the sensor goes blind during a flood.
#define SENSOR_BLIND_CM  25.0f

// ---- alert thresholds, cm (enter / exit differ: hysteresis) ----
#define WASPADA_ENTER    30.0f
#define WASPADA_EXIT     25.0f
#define BAHAYA_ENTER     60.0f
#define BAHAYA_EXIT      55.0f
#define DWELL_DOWN_MS    60000UL   // must be stable this long before de-escalating

// ---- rain ----
#define MM_PER_TIP       0.30f
#define TIP_DEBOUNCE_US  250000UL  // reed switches bounce for tens of ms
#define RAIN_WASPADA     10.0f     // mm/hour
#define RAIN_BAHAYA      30.0f

// ---- scheduling ----
#define SENSOR_PERIOD_MS 5000UL
#define LOG_PERIOD_MS    60000UL
#define UPLOAD_PERIOD_MS 300000UL
#define NTP_PERIOD_MS    21600000UL
#define SMS_REPEAT_MS    300000UL

// ---- sampling ----
#define ULTRA_SAMPLES    5     // median of this many, per reading
#define MAX_MIN_SAMPLES  16    // readings buffered per logged minute

// ---- ultrasonic timing ----
// 60 ms is the JSN-SR04T's minimum measurement cycle; trigger it sooner and it
// ignores the trigger. Timeout 30 ms ~= 5 m of range, well past the tank.
#define ULTRA_ECHO_TIMEOUT_US  30000UL
#define ULTRA_PING_GAP_MS      70

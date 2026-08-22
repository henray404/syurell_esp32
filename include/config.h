#pragma once

// ---- pins ----
#define TRIG_PIN         5
#define ECHO_PIN         18
#define RAIN_SENSOR_PIN  34
#define RAIN_GAUGE_PIN   14
#define RELAY_PIN        26
#define SD_CS_PIN        15

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

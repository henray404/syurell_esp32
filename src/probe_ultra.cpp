// =============================================
// PROBE ULTRASONIK - JSN-SR04T, berdiri sendiri
//
// Build target terpisah dari firmware banjir: `pio run -e probe -t upload`.
// Tidak ada WiFi, SD, SMS, relay atau FSM di sini -- kalau salah satunya ikut,
// ia bisa memblokir loop dan pola pingnya berubah, dan probe yang mengukur
// modul dengan cara berbeda dari firmware tidak membuktikan apa pun tentang
// firmware. Karena itu pingnya diambil dari hw_ultra.h yang sama.
//
// Yang dicari di sini ada tiga:
//   1. Modul menjawab sama sekali atau tidak -- kolom `us`.
//   2. Jawabannya echo asli atau ring-down -- kolom `sd`/`span`. Permukaan air
//      asli bergoyang beberapa mm antar ping; ring-down modul mengulang lebar
//      pulsa yang sama persis. span 0 us artinya bukan echo.
//   3. Angka untuk ULTRA_OFFSET_CM -- tekan 'z' saat rig kosong.
// =============================================
#include <Arduino.h>
#include "config.h"
#include "hw_ultra.h"
#include "logic_height.h"
#include "logic_median.h"

// Ring of recent samples, for a median that does not need the whole run in RAM.
// 32 at ~14 ping/s is about two seconds of history -- long enough to smooth the
// water, short enough to follow a hand moved in front of the sensor.
static const int RING = 32;
static float ringCm[RING];
static int   ringN = 0, ringHead = 0;

// Running stats since the last reset. Kept as sums so the whole run counts,
// however long it is, without storing it.
static uint32_t nPing = 0, nEcho = 0, nTimeout = 0;
static double sumCm = 0, sumSqCm = 0;
static float  minCm = 0, maxCm = 0;
static unsigned long minUs = 0, maxUs = 0;

static const int BLOCK = 20;   // pings between summary blocks

static void resetStats() {
  nPing = nEcho = nTimeout = 0;
  sumCm = sumSqCm = 0;
  minCm = maxCm = 0;
  minUs = maxUs = 0;
  ringN = ringHead = 0;
}

static void header() {
  Serial.println();
  Serial.println("=== PROBE ULTRASONIK - JSN-SR04T ===");
  Serial.print("pin TRIG="); Serial.print(TRIG_PIN);
  Serial.print("  ECHO="); Serial.print(ECHO_PIN);
  Serial.print("  timeout="); Serial.print(ULTRA_ECHO_TIMEOUT_US);
  Serial.print("us  jeda="); Serial.print(ULTRA_PING_GAP_MS); Serial.println("ms");
  Serial.print("kalibrasi JARAK_DASAR="); Serial.print(JARAK_DASAR, 1);
  Serial.print("  BLIND="); Serial.print(SENSOR_BLIND_CM, 1);
  Serial.print("  OFFSET="); Serial.print(ULTRA_OFFSET_CM, 1);
  Serial.print("  slack="); Serial.println(ULTRA_RANGE_SLACK_CM, 1);
  Serial.println("tombol   r = reset statistik    z = usulan ULTRA_OFFSET_CM");
  Serial.println("--------------------------------------------------------");
  Serial.println("     #       us       cm      dev   status");
}

// 'z' is the whole point of the probe for calibration: it turns the run's mean
// into the number config.h wants. Only meaningful with the rig empty, so it
// says so rather than printing a figure someone pastes in from a full tank.
static void usulanOffset() {
  if (nEcho == 0) { Serial.println(">> belum ada echo, tidak bisa hitung offset"); return; }
  float mean = (float)(sumCm / nEcho);
  Serial.println();
  Serial.print(">> rata-rata "); Serial.print(mean, 2);
  Serial.print(" cm dari "); Serial.print(nEcho); Serial.println(" echo");
  Serial.print(">> ULTRA_OFFSET_CM = JARAK_DASAR - rata2 = ");
  Serial.print(JARAK_DASAR, 1); Serial.print(" - "); Serial.print(mean, 2);
  Serial.print(" = "); Serial.println(JARAK_DASAR - mean, 2);
  Serial.println(">> HANYA sah kalau rig kosong sampai dasar. Isi ke config.h:144.");
  Serial.println();
}

static void ringkasan() {
  if (nPing == 0) return;
  float mean = nEcho ? (float)(sumCm / nEcho) : NAN;
  // Population sd. Sample-vs-population is noise next to what this is used for:
  // telling a jittering surface (sd > ~0.05) from a frozen ring-down (sd == 0).
  float sd = 0;
  if (nEcho > 1) {
    double var = sumSqCm / nEcho - (double)mean * mean;
    sd = var > 0 ? sqrtf((float)var) : 0;
  }

  float tmp[RING];
  for (int i = 0; i < ringN; ++i) tmp[i] = ringCm[i];
  Ketinggian k = heightFrom(tmp, ringN);   // sorts tmp; same gates as firmware

  Serial.println("  ---------------------------------------------------");
  Serial.print("  echo     "); Serial.print(nEcho); Serial.print("/"); Serial.print(nPing);
  Serial.print("   timeout "); Serial.println(nTimeout);
  if (nEcho) {
    Serial.print("  us       min "); Serial.print(minUs);
    Serial.print("   max "); Serial.print(maxUs);
    Serial.print("   span "); Serial.println(maxUs - minUs);
    Serial.print("  cm       min "); Serial.print(minCm, 2);
    Serial.print("   max "); Serial.print(maxCm, 2);
    Serial.print("   span "); Serial.println(maxCm - minCm, 2);
    Serial.print("  rata2    "); Serial.print(mean, 2);
    Serial.print(" cm   sd "); Serial.print(sd, 3); Serial.println(" cm");
    Serial.print("  jarak    "); Serial.print(k.jarak_cm, 2);
    Serial.print(" cm (median ring + offset)   -> ");
    Serial.println(k.valid ? "OK" : k.reason);
    if (k.valid) { Serial.print("  tinggi   "); Serial.print(k.tinggi_cm, 2); Serial.println(" cm"); }
    // The one diagnosis a distance reading cannot give you on its own.
    if (maxUs == minUs && nEcho >= 5) {
      Serial.println("  [!] lebar pulsa TIDAK berubah sama sekali -> ring-down modul,");
      Serial.println("      bukan echo. Cek 5V/arus, solderan transduser, arah beam.");
    }
  } else {
    Serial.println("  [!] tidak ada echo sama sekali -> cek wiring TRIG/ECHO dan 5V");
  }
  Serial.println("  ---------------------------------------------------");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  ultraBegin();
  header();
  diagUltrasonik();          // mode-pad check + 3 raw widths, same as firmware
  resetStats();
  Serial.println("     #       us       cm      dev   status");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == 'r' || c == 'R') { resetStats(); Serial.println(">> statistik direset"); }
    if (c == 'z' || c == 'Z') usulanOffset();
  }

  unsigned long w = pingWidthUs();
  ++nPing;

  Serial.print("  ");
  if (nPing < 1000) Serial.print(" ");
  if (nPing < 100)  Serial.print(" ");
  if (nPing < 10)   Serial.print(" ");
  Serial.print(nPing);

  if (w) {
    float cm = usToCm(w);
    ++nEcho;
    sumCm += cm;
    sumSqCm += (double)cm * cm;
    if (nEcho == 1) { minCm = maxCm = cm; minUs = maxUs = w; }
    else {
      if (cm < minCm) minCm = cm;
      if (cm > maxCm) maxCm = cm;
      if (w < minUs) minUs = w;
      if (w > maxUs) maxUs = w;
    }
    ringCm[ringHead] = cm;
    ringHead = (ringHead + 1) % RING;
    if (ringN < RING) ++ringN;

    float mean = (float)(sumCm / nEcho);
    Serial.print("   "); Serial.print(w);
    Serial.print("   "); Serial.print(cm, 2);
    Serial.print("   "); if (cm - mean >= 0) Serial.print("+");
    Serial.print(cm - mean, 2);
    // Gate verdict per ping, not just per block: it is how you see the exact
    // distance where the module stops being believed.
    float one[1] = {cm};
    Ketinggian k = heightFrom(one, 1);
    Serial.print("   "); Serial.println(k.valid ? "OK" : k.reason);
  } else {
    ++nTimeout;
    Serial.println("        -        -       -   timeout");
  }

  if (nPing % BLOCK == 0) ringkasan();

  delay(ULTRA_PING_GAP_MS);   // module needs a full cycle before the next trigger
}

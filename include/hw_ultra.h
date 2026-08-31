#pragma once
#include <Arduino.h>
#include "config.h"
#include "logic_height.h"

// JSN-SR04T driver: pins, one ping, and the boot self-check. Shared by the
// flood firmware (src/main.cpp) and the standalone probe (src/probe_ultra.cpp)
// so the two can never drift into measuring the module differently -- a probe
// that pings unlike the firmware proves nothing about the firmware.
// The cm arithmetic lives in logic_height.h, which has no Arduino in it.

inline void ultraBegin() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
}

// =============================================
// BACA JARAK - JSN-SR04T (single ping)
// =============================================
inline unsigned long pingWidthUs() {
  // 30 us trigger, not the datasheet-minimum 10 us: on this JSN-SR04T a 10 us
  // pulse produced no echo at all. Widening it is safe -- the module latches on
  // the trigger's falling edge, so a longer HIGH only makes the pulse easier to
  // see, it does not stretch the measurement.
  // The echo from the previous cycle must be done before this one starts. If it
  // is still asserted, the rise-detection loop below latches onto its tail and
  // times a fragment of the old pulse -- a distance far shorter than the real
  // one, and a different fragment every ping.
  unsigned long tIdle = micros();
  while (digitalRead(ECHO_PIN)) {
    if (micros() - tIdle > ULTRA_ECHO_TIMEOUT_US) return 0;
  }

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(5);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(30);
  digitalWrite(TRIG_PIN, LOW);

  // Poll the edges instead of pulseIn(); pulseIn() never caught this module's
  // echo on this board even when the pulse was demonstrably there.
  unsigned long tS = micros(), tRise = 0, tFall = 0;
  while (micros() - tS < ULTRA_ECHO_TIMEOUT_US) {
    if (digitalRead(ECHO_PIN)) { tRise = micros(); break; }
  }
  if (!tRise) return 0;
  while (micros() - tRise < ULTRA_ECHO_TIMEOUT_US) {
    if (!digitalRead(ECHO_PIN)) { tFall = micros(); break; }
  }
  if (!tFall) return 0;

  return tFall - tRise;
}

// =============================================
// DIAGNOSA SEKALI SAAT BOOT
// A module stuck in UART/auto mode drives ECHO on its own schedule, so its
// "distance" is a fixed frame width that ignores the target. Watch the pin
// without triggering: any edge here means the mode pads are wrong, and no
// amount of calibration will help. Silence means the module only answers
// triggers, so a constant reading is an acoustic problem instead -- weak 5 V,
// a loose transducer lead, or the beam missing the bed.
// =============================================
inline void diagUltrasonik() {
  Serial.print("[DIAG] ECHO tanpa trigger (300 ms): ");
  int edges = 0;
  int prev = digitalRead(ECHO_PIN);
  unsigned long t0 = millis();
  while (millis() - t0 < 300) {
    int now = digitalRead(ECHO_PIN);
    if (now != prev) { ++edges; prev = now; }
  }
  Serial.print(edges); Serial.print(" transisi -> ");
  Serial.println(edges > 0 ? "MODUL MODE UART/AUTO (pad mode salah)"
                           : "diam (mode trigger benar)");

  // Same ping the main loop uses, but the pulse width is printed raw. A width
  // that never moves when the target does is the module's own ring-down, not an
  // echo off the riverbed.
  for (int i = 0; i < 3; ++i) {
    unsigned long w = pingWidthUs();
    Serial.print("[DIAG] lebar pulsa: ");
    if (w) { Serial.print(w); Serial.print(" us = "); Serial.print(usToCm(w), 1); Serial.println(" cm"); }
    else   Serial.println("timeout (tidak ada echo)");
    delay(ULTRA_PING_GAP_MS);
  }
}

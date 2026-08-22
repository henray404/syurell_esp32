#pragma once
#include <cmath>
#include <cstddef>

// Median of n floats. Sorts v in place. Returns NAN for n <= 0.
// Median rather than mean: a single spurious ultrasonic echo drags a mean but
// barely moves a median, and over moving water spurious echoes are routine.
inline float medianOf(float *v, int n) {
  if (n <= 0 || v == NULL) return NAN;
  for (int i = 1; i < n; ++i) {          // insertion sort; n is 5, keep it simple
    float key = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > key) {
      v[j + 1] = v[j];
      --j;
    }
    v[j + 1] = key;
  }
  return v[n / 2];
}

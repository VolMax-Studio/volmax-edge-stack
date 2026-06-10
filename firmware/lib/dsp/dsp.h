/**
 * dsp.h — minimal, dependency-free DSP for the edge device.
 *
 * RMS: textbook windowed RMS.
 * THD: harmonic magnitudes via the Goertzel algorithm (bins at
 * f1..8*f1). Goertzel beats a full FFT here: we only need 8 bins,
 * it is O(N) per bin, needs no buffers and no external library.
 * THD-F = sqrt(sum(H2..H8)^2) / H1 * 100%.
 *
 * Verified against power_signal_tools (Python) reference outputs.
 */
#pragma once
#include <math.h>

namespace dsp {

inline float rms(const float* x, int n) {
  double acc = 0;
  for (int i = 0; i < n; i++) acc += (double)x[i] * x[i];
  return sqrtf((float)(acc / n));
}

/** Magnitude of frequency component `freq` in x[] sampled at fs. */
inline float goertzelMag(const float* x, int n, float fs, float freq) {
  const float k = 0.5f + n * freq / fs;
  const float w = 2.0f * (float)M_PI * (int)k / n;
  const float coeff = 2.0f * cosf(w);
  float s0 = 0, s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) {
    s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const float power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  return sqrtf(fmaxf(power, 0.0f)) * 2.0f / n;
}

/** THD-F in percent, harmonics 2..8 vs fundamental. */
inline float thd_percent(const float* x, int n, float fs, float f1) {
  const float h1 = goertzelMag(x, n, fs, f1);
  if (h1 < 1e-6f) return 0.0f;
  double sumsq = 0;
  for (int h = 2; h <= 8; h++) {
    const float m = goertzelMag(x, n, fs, f1 * h);
    sumsq += (double)m * m;
  }
  return (float)(sqrt(sumsq) / h1 * 100.0);
}

}  // namespace dsp

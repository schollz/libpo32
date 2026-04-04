/*
 * Internal math primitives shared across po32 translation units.
 *
 * Keeps the po32_lut_* API surface, but uses standard <math.h>
 * functions directly (no lookup tables).
 */

#ifndef PO32_LUT_INTERNAL_H
#define PO32_LUT_INTERNAL_H

#include <math.h>

#define PO32_LUT_PI         3.14159265358979323846f
#define PO32_LUT_TWO_PI     6.28318530717958647692f
#define PO32_LUT_HALF_PI    1.57079632679489661923f
#define PO32_LUT_INV_TWO_PI 0.15915494309189533577f
#define PO32_LUT_LOG2_E     1.44269504088896340736f
#define PO32_LUT_LOG2_10    3.32192809488736234787f

static inline float po32_lut_sinf(float rad) {
  return sinf(rad);
}

static inline float po32_lut_cosf(float rad) {
  return cosf(rad);
}

static inline float po32_lut_exp2f(float x) {
  return exp2f(x);
}

static inline float po32_lut_log2f(float x) {
  return log2f(x);
}

static inline float po32_lut_powf(float base, float exponent) {
  if (base <= 0.0f)
    return 0.0f;
  return powf(base, exponent);
}

static inline float po32_lut_expf(float x) {
  return expf(x);
}

static inline float po32_lut_pow10f(float x) {
  return powf(10.0f, x);
}

static inline float po32_lut_logf(float x) {
  return logf(x);
}

static inline float po32_lut_log10f(float x) {
  return log10f(x);
}

#endif /* PO32_LUT_INTERNAL_H */

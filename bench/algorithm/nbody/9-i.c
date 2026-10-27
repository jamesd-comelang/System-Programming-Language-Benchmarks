/*
 * N-body, 5 bodies, AVX2 implementation in C.
 *
 * The Computer Language Benchmarks Game (nbody).
 * Based on the SIMD pair formulation used by the C++ 0-i.cpp implementation
 * in this repository; rewritten as C with explicit pair unrolling.
 *
 * Requires x86-64 AVX (the repository's -march=broadwell enables AVX2).
 * The first vector lane is padding; x, y, z occupy lanes 1, 2, 3.
 */
#include <immintrin.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PI 3.141592653589793
#define SOLAR_MASS (4.0 * PI * PI)
#define DAYS_PER_YEAR 365.24
#define NBODIES 5
#define PAIRS 10
#define DT 0.01

typedef struct {
    __m256d p[NBODIES];
    __m256d v[NBODIES];
    __m256d mass[NBODIES];
} system_t;

static inline __m256d rsqrt4(__m256d squared) {
    /* Float hardware reciprocal sqrt, then double-precision refinement.
       Mirrors the polynomial refinement in the C++ SIMD implementation. */
    __m128 approx = _mm_rsqrt_ps(_mm256_cvtpd_ps(squared));
    __m256d x = _mm256_cvtps_pd(approx);
    __m256d y = _mm256_mul_pd(_mm256_mul_pd(squared, x), x);
    __m256d a = _mm256_mul_pd(_mm256_mul_pd(y, _mm256_set1_pd(0.375)), y);
    __m256d b = _mm256_sub_pd(_mm256_mul_pd(y, _mm256_set1_pd(1.25)),
                              _mm256_set1_pd(1.875));
    return _mm256_mul_pd(x, _mm256_sub_pd(a, b));
}

static void init_system(system_t *s) {
    const double masses[NBODIES] = {
        SOLAR_MASS,
        9.54791938424326609e-04 * SOLAR_MASS,
        2.85885980666130812e-04 * SOLAR_MASS,
        4.36624404335156298e-05 * SOLAR_MASS,
        5.15138902046611451e-05 * SOLAR_MASS
    };
    const double position[NBODIES][3] = {
        { 0.0, 0.0, 0.0 },
        { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01 },
        { 8.34336671824457987e+00,  4.12479856412430479e+00, -4.03523417114321381e-01 },
        { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01 },
        { 1.53796971148509165e+01, -2.59193146099879641e+01,  1.79258772950371181e-01 }
    };
    const double velocity[NBODIES][3] = {
        { 0.0, 0.0, 0.0 },
        { 1.66007664274403694e-03 * DAYS_PER_YEAR,  7.69901118419740425e-03 * DAYS_PER_YEAR, -6.90460016972063023e-05 * DAYS_PER_YEAR },
        {-2.76742510726862411e-03 * DAYS_PER_YEAR,  4.99852801234917238e-03 * DAYS_PER_YEAR,  2.30417297573763929e-05 * DAYS_PER_YEAR },
        { 2.96460137564761618e-03 * DAYS_PER_YEAR,  2.37847173959480950e-03 * DAYS_PER_YEAR, -2.96589568540237556e-05 * DAYS_PER_YEAR },
        { 2.68067772490389322e-03 * DAYS_PER_YEAR,  1.62824170038242295e-03 * DAYS_PER_YEAR, -9.51592254519715870e-05 * DAYS_PER_YEAR }
    };

    __m256d momentum = _mm256_setzero_pd();
    for (int i = 0; i < NBODIES; ++i) {
        s->p[i] = _mm256_setr_pd(0.0, position[i][0], position[i][1], position[i][2]);
        s->v[i] = _mm256_setr_pd(0.0, velocity[i][0], velocity[i][1], velocity[i][2]);
        s->mass[i] = _mm256_set1_pd(masses[i]);
        momentum = _mm256_add_pd(momentum, _mm256_mul_pd(s->v[i], s->mass[i]));
    }
    s->v[0] = _mm256_mul_pd(momentum, _mm256_set1_pd(-1.0 / SOLAR_MASS));
}

#define PAIR_LIST(F) \
    F(0, 1, 0) F(1, 2, 0) F(2, 2, 1) F(3, 3, 0) F(4, 3, 1) \
    F(5, 3, 2) F(6, 4, 0) F(7, 4, 1) F(8, 4, 2) F(9, 4, 3)

#define MAKE_DELTA(k,i,j) delta[k] = _mm256_sub_pd(s->p[i], s->p[j]);

/* For 4 pair deltas, calculate all four squared norms at once.
   Each delta contains [0, dx, dy, dz], so horizontal adds produce
   [dx0^2, dx1^2, dy0^2+dz0^2, dy1^2+dz1^2]. */
static inline __m256d batch_rsqrt(const __m256d *delta) {
    const __m256d a = _mm256_mul_pd(delta[0], delta[0]);
    const __m256d b = _mm256_mul_pd(delta[1], delta[1]);
    const __m256d c = _mm256_mul_pd(delta[2], delta[2]);
    const __m256d d = _mm256_mul_pd(delta[3], delta[3]);
    const __m256d x = _mm256_hadd_pd(a, b);
    const __m256d y = _mm256_hadd_pd(c, d);
    const __m256d z = _mm256_add_pd(_mm256_permute2f128_pd(x, y, 0x21),
                                     _mm256_blend_pd(x, y, 0xC));
    return rsqrt4(z);
}

#define APPLY_FORCE(k,i,j) do { \
    __m256d force = _mm256_mul_pd(delta[k], _mm256_set1_pd(magnitudes[k])); \
    s->v[i] = _mm256_sub_pd(s->v[i], _mm256_mul_pd(force, s->mass[j])); \
    s->v[j] = _mm256_add_pd(s->v[j], _mm256_mul_pd(force, s->mass[i])); \
} while (0);

static inline void advance(system_t *s) {
    __m256d delta[12];
    _Alignas(32) double magnitudes[12];
    const __m256d dt = _mm256_set1_pd(DT);

    PAIR_LIST(MAKE_DELTA)
    /* The last two lanes are padding, but must be nonzero for rsqrt. */
    delta[10] = _mm256_set1_pd(1.0);
    delta[11] = _mm256_set1_pd(1.0);

    for (int k = 0; k < 12; k += 4) {
        __m256d inv = batch_rsqrt(delta + k);
        __m256d mag = _mm256_mul_pd(_mm256_mul_pd(inv, inv),
                                      _mm256_mul_pd(inv, dt));
        _mm256_store_pd(magnitudes + k, mag);
    }

    PAIR_LIST(APPLY_FORCE)
    for (int i = 0; i < NBODIES; ++i)
        s->p[i] = _mm256_add_pd(s->p[i], _mm256_mul_pd(s->v[i], dt));
}

static double energy(const system_t *s) {
    double result = 0.0;
    _Alignas(32) double p[NBODIES][4], v[NBODIES][4];
    double mass[NBODIES];
    for (int i = 0; i < NBODIES; ++i) {
        _mm256_store_pd(p[i], s->p[i]);
        _mm256_store_pd(v[i], s->v[i]);
        mass[i] = _mm256_cvtsd_f64(s->mass[i]);
    }
    for (int i = 0; i < NBODIES; ++i) {
        result += 0.5 * mass[i] * (v[i][1] * v[i][1] +
                                   v[i][2] * v[i][2] +
                                   v[i][3] * v[i][3]);
        for (int j = 0; j < i; ++j) {
            const double dx = p[i][1] - p[j][1];
            const double dy = p[i][2] - p[j][2];
            const double dz = p[i][3] - p[j][3];
            result -= mass[i] * mass[j] / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return result;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1000;
    if (n < 0) return EXIT_FAILURE;
    system_t s;
    init_system(&s);
    printf("%.9f\n", energy(&s));
    for (int i = 0; i < n; ++i) advance(&s);
    printf("%.9f\n", energy(&s));
    return EXIT_SUCCESS;
}

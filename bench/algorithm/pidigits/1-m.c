// Pi digits via Machin's formula, parallel (-m) version of
// bench/algorithm/pidigits/1.c (same fixed-point decimal arithmetic,
// pi = 16*arctan(1/5) - 4*arctan(1/239), no big-integer library).
//
// Parallelization: each arctan series is split across T NumCPU workers
// by stride, not by contiguous chunk. Worker t (0 <= t < T) handles the
// terms k = t, t+T, t+2T, ... . Its first value is p_t = p_0 / x^2^t
// (a short serial chain of at most T-1 divisions, computed once in
// main); each following step divides by x^2^T in a single pass. A
// contiguous split would NOT work: chunk c would have to start from
// p_0 / x^2^(c*S), costing c*S extra divisions, and the last chunk
// alone would cost as much as the whole serial run.
//
// Byte-identical output: every (k, term_k) pair is computed exactly
// once, by the same exact integer arithmetic as the serial code
// (floor(floor(a/b)/c) == floor(a/(b*c)) for non-negative integers, so
// the strided recurrence reproduces the serial p_k bit-for-bit), and
// each worker accumulates its terms into its own pos/neg partials.
// The final sums are exact integer additions, which are
// order-independent, so the combined result -- and the printed digits
// -- are identical to 1.c at every input.
//
// Thread count T is the number of online CPUs, capped at 7: the
// strided step divides by x^2^T in one unsigned __int128 pass, and
// for x=239, 239^2^7 is about 2.0e33, so the intermediate
// rem*BASE stays below 2^127, while 239^2^8 would overflow it.
// (The CI runners have 4 cores, so the cap never binds there.) A
// sysconf failure, a pthread_create failure, or a pthread_join
// failure is reported on stderr and exits nonzero before anything is
// printed; there is no serial fallback.
//
// Output format matches bench/algorithm/pidigits/8.go: groups of 10
// digits, "%s\t:%d\n", last group space-padded to width 10.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE 10000
#define GUARD 30
#define MAX_WORKERS 7

static int L; // number of limbs
static int *pow10_arr; // 10^digits, set by main

// Divide a[] (length L, little-endian base BASE) by d in place.
// Returns 1 if the result is non-zero.
static int div_small(int *a, int d) {
  long rem = 0;
  int nz = 0;
  for (int i = L - 1; i >= 0; i--) {
    long cur = rem * BASE + a[i];
    a[i] = (int)(cur / d);
    rem = cur % d;
    if (a[i]) nz = 1;
  }
  return nz;
}

// Divide a[] by a wide divisor d (up to < 2^127 / BASE) in place.
// Returns 1 if the result is non-zero. Used for the strided step,
// which divides by x^2^T: for x=239 and T<=7 the divisor is at most
// about 2.0e33, which does not fit in a long.
static int div_small_wide(int *a, unsigned __int128 d) {
  unsigned __int128 rem = 0;
  int nz = 0;
  for (int i = L - 1; i >= 0; i--) {
    unsigned __int128 cur = rem * BASE + (unsigned int)a[i];
    a[i] = (int)(cur / d);
    rem = cur % d;
    if (a[i]) nz = 1;
  }
  return nz;
}

static void add_into(int *dst, const int *src) {
  long carry = 0;
  for (int i = 0; i < L; i++) {
    long s = (long)dst[i] + src[i] + carry;
    dst[i] = (int)(s % BASE);
    carry = s / BASE;
  }
}

static void mul_small_into(int *dst, const int *src, int m) {
  long carry = 0;
  for (int i = 0; i < L; i++) {
    long s = (long)src[i] * m + carry;
    dst[i] = (int)(s % BASE);
    carry = s / BASE;
  }
}

// dst = a - b, assuming a >= b.
static void sub_into(int *dst, const int *a, const int *b) {
  long borrow = 0;
  for (int i = 0; i < L; i++) {
    long d = (long)a[i] - b[i] - borrow;
    if (d < 0) {
      d += BASE;
      borrow = 1;
    } else {
      borrow = 0;
    }
    dst[i] = (int)d;
  }
}

typedef struct {
  int x; // 5 or 239
  int t; // this worker's stride offset
  int T; // worker count
  unsigned __int128 x2powT; // (x*x)^T, the strided-step divisor
  int *p_start; // p_0 / x^2^t, computed by main (read-only here)
  int *pos; // this worker's positive partial sum (output)
  int *neg; // this worker's negative partial sum (output)
} WorkerArg;

// One strided slice of arctan(1/x): terms k = t, t+T, t+2T, ... .
static void *arctan_worker(void *arg) {
  WorkerArg *w = (WorkerArg *)arg;
  int *p = calloc((size_t)L, sizeof(int));
  int *term = calloc((size_t)L, sizeof(int));
  if (p == NULL || term == NULL) {
    // Allocation failure inside a worker: nothing has been printed
    // yet, so main still reports it; record it via a NULL marker.
    free(p);
    free(term);
    return NULL;
  }
  memcpy(p, w->p_start, (size_t)L * sizeof(int));
  for (int k = w->t;; k += w->T) {
    memcpy(term, p, (size_t)L * sizeof(int));
    div_small(term, 2 * k + 1);
    if (k % 2 == 0)
      add_into(w->pos, term);
    else
      add_into(w->neg, term);
    if (!div_small_wide(p, w->x2powT)) break;
  }
  free(p);
  free(term);
  return w; // non-NULL marks success
}

// arctan(1/x) * 10^digits, split into positive and negative partial
// sums (to avoid signed big arithmetic). Computed by T workers, each
// handling one strided slice of the term series; returns 0 on
// success, 1 if a worker could not be started or joined.
static int arctan_reciprocal_parallel(int x, int *pos, int *neg, int T) {
  int x2 = x * x;
  unsigned __int128 x2powT = 1;
  for (int i = 0; i < T; i++) x2powT *= (unsigned int)x2;

  // p_0 = 10^digits / x, then the per-worker starts
  // p_t = p_0 / x^2^t as a serial chain.
  int *p0 = calloc((size_t)L, sizeof(int));
  int *starts = calloc((size_t)T * (size_t)L, sizeof(int));
  int *pos_parts = calloc((size_t)T * (size_t)L, sizeof(int));
  int *neg_parts = calloc((size_t)T * (size_t)L, sizeof(int));
  pthread_t *threads = malloc((size_t)T * sizeof(pthread_t));
  WorkerArg *args = malloc((size_t)T * sizeof(WorkerArg));
  if (p0 == NULL || starts == NULL || pos_parts == NULL ||
      neg_parts == NULL || threads == NULL || args == NULL) {
    fprintf(stderr, "pidigits: out of memory\n");
    free(p0);
    free(starts);
    free(pos_parts);
    free(neg_parts);
    free(threads);
    free(args);
    return 1;
  }
  memcpy(p0, pow10_arr, (size_t)L * sizeof(int));
  div_small(p0, x);
  memcpy(starts, p0, (size_t)L * sizeof(int));
  for (int t = 1; t < T; t++) {
    memcpy(starts + (size_t)t * L, starts + (size_t)(t - 1) * L,
           (size_t)L * sizeof(int));
    div_small(starts + (size_t)t * L, x2);
  }

  int started = 0;
  for (int t = 0; t < T; t++) {
    args[t].x = x;
    args[t].t = t;
    args[t].T = T;
    args[t].x2powT = x2powT;
    args[t].p_start = starts + (size_t)t * L;
    args[t].pos = pos_parts + (size_t)t * L;
    args[t].neg = neg_parts + (size_t)t * L;
    int rc = pthread_create(&threads[t], NULL, arctan_worker, &args[t]);
    if (rc != 0) {
      fprintf(stderr, "pidigits: pthread_create failed for worker %d: %s\n",
              t, strerror(rc));
      for (int j = 0; j < started; j++) pthread_join(threads[j], NULL);
      free(p0);
      free(starts);
      free(pos_parts);
      free(neg_parts);
      free(threads);
      free(args);
      return 1;
    }
    started++;
  }
  int failed = 0;
  for (int t = 0; t < T; t++) {
    void *ret = NULL;
    int rc = pthread_join(threads[t], &ret);
    if (rc != 0) {
      fprintf(stderr, "pidigits: pthread_join failed for worker %d: %s\n",
              t, strerror(rc));
      failed = 1;
    } else if (ret == NULL) {
      fprintf(stderr, "pidigits: worker %d ran out of memory\n", t);
      failed = 1;
    }
  }
  if (!failed) {
    for (int t = 0; t < T; t++) {
      add_into(pos, pos_parts + (size_t)t * L);
      add_into(neg, neg_parts + (size_t)t * L);
    }
  }
  free(p0);
  free(starts);
  free(pos_parts);
  free(neg_parts);
  free(threads);
  free(args);
  return failed;
}

int main(int argc, char **argv) {
  int n = 27;
  if (argc > 1) n = atoi(argv[1]);
  int digits = n + GUARD;
  L = digits / 4 + 4;

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1) {
    fprintf(stderr, "pidigits: sysconf(_SC_NPROCESSORS_ONLN) failed: %s\n",
            strerror(errno));
    return 1;
  }
  int T = (int)ncpu;
  if (T > MAX_WORKERS) T = MAX_WORKERS;

  pow10_arr = calloc((size_t)L, sizeof(int));
  int q = digits / 4, r = digits % 4;
  if (r == 0)
    pow10_arr[q] = 1;
  else {
    int v = 1;
    for (int i = 0; i < r; i++) v *= 10;
    pow10_arr[q] = v;
  }

  int *s5p = calloc((size_t)L, sizeof(int));
  int *s5n = calloc((size_t)L, sizeof(int));
  int *s239p = calloc((size_t)L, sizeof(int));
  int *s239n = calloc((size_t)L, sizeof(int));
  if (arctan_reciprocal_parallel(5, s5p, s5n, T) != 0) return 1;
  if (arctan_reciprocal_parallel(239, s239p, s239n, T) != 0) return 1;

  int *tpos = calloc((size_t)L, sizeof(int));
  int *tneg = calloc((size_t)L, sizeof(int));
  int *tmp = calloc((size_t)L, sizeof(int));
  // total_pos = 16*s5p + 4*s239n ; total_neg = 16*s5n + 4*s239p
  mul_small_into(tmp, s5p, 16);
  add_into(tpos, tmp);
  mul_small_into(tmp, s239n, 4);
  add_into(tpos, tmp);
  mul_small_into(tmp, s5n, 16);
  add_into(tneg, tmp);
  mul_small_into(tmp, s239p, 4);
  add_into(tneg, tmp);

  int *pi = calloc((size_t)L, sizeof(int));
  sub_into(pi, tpos, tneg);

  // Convert to a decimal string.
  int hi = L - 1;
  while (hi > 0 && pi[hi] == 0) hi--;
  char *str = malloc((size_t)(digits + 8));
  int pos = 0;
  pos += sprintf(str + pos, "%d", pi[hi]);
  for (int i = hi - 1; i >= 0; i--) pos += sprintf(str + pos, "%04d", pi[i]);
  str[pos] = '\0';

  // Print the first n digits in groups of 10.
  for (int i = 0; i < n; i += 10) {
    if (i + 10 <= n) {
      printf("%.10s\t:%d\n", str + i, i + 10);
    } else {
      printf("%-*.*s\t:%d\n", 10, n - i, str + i, n);
    }
  }
  return 0;
}

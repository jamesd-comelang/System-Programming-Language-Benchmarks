// Pi digits via Machin's formula, in fixed-point decimal arithmetic:
//   pi = 16*arctan(1/5) - 4*arctan(1/239)
// arctan(1/x) is summed term by term, each term obtained from the
// previous one by division by a small integer, so no big-integer
// library is needed. 30 guard digits past the requested count make
// the truncation error from the small divisions invisible in the
// first n digits (verified against the _out files).
//
// Output format matches bench/algorithm/pidigits/8.go: groups of 10
// digits, "%s\t:%d\n", last group space-padded to width 10.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 10000
#define GUARD 30

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

// arctan(1/x) * 10^digits, split into positive and negative partial
// sums (to avoid signed big arithmetic).
static void arctan_reciprocal(int x, int *pos, int *neg) {
  int *p = calloc((size_t)L, sizeof(int));
  int *term = calloc((size_t)L, sizeof(int));
  // p = 10^digits / x, computed by starting from 10^digits and
  // dividing once by x (10^digits is built by main).
  memcpy(p, pow10_arr, (size_t)L * sizeof(int));
  div_small(p, x);
  int x2 = x * x;
  for (int k = 0;; k++) {
    memcpy(term, p, (size_t)L * sizeof(int));
    div_small(term, 2 * k + 1);
    if (k % 2 == 0)
      add_into(pos, term);
    else
      add_into(neg, term);
    if (!div_small(p, x2)) break;
  }
  free(p);
  free(term);
}

int main(int argc, char **argv) {
  int n = 27;
  if (argc > 1) n = atoi(argv[1]);
  int digits = n + GUARD;
  L = digits / 4 + 4;

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
  arctan_reciprocal(5, s5p, s5n);
  arctan_reciprocal(239, s239p, s239n);

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

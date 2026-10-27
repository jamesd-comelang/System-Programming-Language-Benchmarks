// Digits of e, in fixed-point decimal arithmetic:
//   e = sum_{k>=0} 1/k!
// The k-th term is obtained from the previous one by division by
// the small integer k, so no big-integer library is needed. 30
// guard digits past the requested count make the truncation error
// from the small divisions invisible in the first n digits
// (verified against the _out files).
//
// Output format matches bench/algorithm/edigits/1.go: groups of 10
// digits, "%s\t:%d\n", last group space-padded to width 10.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 10000
#define GUARD 30

static int L;

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

int main(int argc, char **argv) {
  int n = 27;
  if (argc > 1) n = atoi(argv[1]);
  int digits = n + GUARD;
  L = digits / 4 + 4;

  int *term = calloc((size_t)L, sizeof(int));
  int *sum = calloc((size_t)L, sizeof(int));
  // term = sum = 10^digits (the k=0 term).
  int q = digits / 4, r = digits % 4;
  if (r == 0) {
    term[q] = 1;
  } else {
    int v = 1;
    for (int i = 0; i < r; i++) v *= 10;
    term[q] = v;
  }
  memcpy(sum, term, (size_t)L * sizeof(int));

  for (int k = 1;; k++) {
    if (!div_small(term, k)) break;
    add_into(sum, term);
  }

  // Convert to a decimal string.
  int hi = L - 1;
  while (hi > 0 && sum[hi] == 0) hi--;
  char *str = malloc((size_t)(digits + 8));
  int pos = 0;
  pos += sprintf(str + pos, "%d", sum[hi]);
  for (int i = hi - 1; i >= 0; i--) pos += sprintf(str + pos, "%04d", sum[i]);
  str[pos] = '\0';

  for (int i = 0; i < n; i += 10) {
    if (i + 10 <= n) {
      printf("%.10s\t:%d\n", str + i, i + 10);
    } else {
      printf("%-*.*s\t:%d\n", 10, n - i, str + i, n);
    }
  }
  return 0;
}

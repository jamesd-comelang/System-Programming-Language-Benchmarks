// Fannkuch-redux, sequential C port of bench/algorithm/fannkuch-redux/1.go.
// The Go version splits the permutation range into even-sized chunks
// run in goroutines; running the whole range as one chunk gives the
// same checksum, because every chunk boundary falls on an even index
// and therefore starts with the same (positive) sign.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int n;
static long *fact;

int main(int argc, char **argv) {
  n = 12;
  if (argc > 1) n = atoi(argv[1]);

  fact = malloc((n + 1) * sizeof(long));
  fact[0] = 1;
  for (int i = 1; i <= n; i++) fact[i] = fact[i - 1] * i;

  int *p = malloc(n * sizeof(int));
  int *pp = malloc(n * sizeof(int));
  int *count = malloc(n * sizeof(int));

  for (int i = 0; i < n; i++) p[i] = i;
  // First permutation for index 0 is the identity; the general
  // index-to-permutation loop below is skipped for idxMin = 0.
  for (int i = n - 1, idx = 0; i > 0; i--) {
    int d = idx / (int)fact[i];
    count[i] = d;
    idx = idx % (int)fact[i];
    memcpy(pp, p, n * sizeof(int));
    for (int j = 0; j <= i; j++) {
      if (j + d <= i)
        p[j] = pp[j + d];
      else
        p[j] = pp[j + d - i - 1];
    }
  }

  long max_flips = 1, checksum = 0;
  for (long idx = 0, sign = 1; idx < fact[n]; idx++, sign = -sign) {
    int first = p[0];
    if (first != 0) {
      long flips = 1;
      if (p[first] != 0) {
        memcpy(pp, p, n * sizeof(int));
        int p0 = first;
        for (;;) {
          flips++;
          for (int i = 1, j = p0 - 1; i < j; i++, j--) {
            int t = pp[i];
            pp[i] = pp[j];
            pp[j] = t;
          }
          int t = pp[p0];
          pp[p0] = p0;
          p0 = t;
          if (pp[p0] == 0) break;
        }
      }
      if (max_flips < flips) max_flips = flips;
      checksum += sign * flips;
    }

    if (idx + 1 == fact[n]) break;
    // Next permutation (Go's sign-dependent rotation).
    if (sign > 0) {
      int t = p[0];
      p[0] = p[1];
      p[1] = t;
    } else {
      int t = p[1];
      p[1] = p[2];
      p[2] = t;
      for (int k = 2;; k++) {
        if (++count[k] <= k) break;
        count[k] = 0;
        for (int j = 0; j <= k; j++) p[j] = p[j + 1];
        p[k + 1] = first;
        first = p[0];
      }
    }
  }

  printf("%ld\nPfannkuchen(%d) = %ld\n", checksum, n, max_flips);
  free(p);
  free(pp);
  free(count);
  free(fact);
  return 0;
}

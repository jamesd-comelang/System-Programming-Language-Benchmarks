// Fannkuch-redux, parallel C port of bench/algorithm/fannkuch-redux/1.c.
//
// NumCPU-worker style: one POSIX thread per available CPU (not one
// thread per permutation), each running a contiguous slice of the
// permutation range with its own p/pp/count buffers, so no locking is
// needed. Per-worker results are combined in main (max of max flips,
// sum of checksums).
//
// Correctness note: the serial loop's sign is +1 at even permutation
// indices and -1 at odd ones, and the next-permutation step depends on
// the sign. Each worker therefore starts with the sign for its own
// first index (idxMin % 2 == 0 ? +1 : -1) and reconstructs the
// permutation at idxMin from the index, exactly as the serial code
// does for index 0. The per-worker loop body is otherwise the serial
// body verbatim, so output is byte-identical to 1.c.
//
// A thread-count lookup failure or a pthread_create/pthread_join
// failure is reported on stderr and exits nonzero; there is no
// silent fallback to fewer threads or to serial execution.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int n;
static long *fact;

typedef struct {
  long idx_min, idx_max;
  long max_flips;
  long checksum;
} WorkerArg;

static void fannkuch_range(long idx_min, long idx_max, long *max_out,
                           long *checksum_out) {
  int *p = malloc(n * sizeof(int));
  int *pp = malloc(n * sizeof(int));
  int *count = malloc(n * sizeof(int));
  if (p == NULL || pp == NULL || count == NULL) {
    fprintf(stderr, "fannkuch-redux: out of memory\n");
    exit(1);
  }

  for (int i = 0; i < n; i++) p[i] = i;
  // First permutation for index idx_min, as in the serial version.
  // (long rem: idx_min exceeds int range for n >= 13.)
  long rem = idx_min;
  for (int i = n - 1; i > 0; i--) {
    long d = rem / fact[i];
    count[i] = (int)d;
    rem = rem % fact[i];
    memcpy(pp, p, n * sizeof(int));
    for (int j = 0; j <= i; j++) {
      if (j + d <= i)
        p[j] = pp[j + d];
      else
        p[j] = pp[j + d - i - 1];
    }
  }

  // The sign at idx_min: +1 for even indices, -1 for odd ones,
  // matching the serial loop, which starts at +1 for index 0 and
  // alternates. The step below depends on the sign, so starting
  // from the parity of idx_min keeps every worker on the serial
  // permutation sequence.
  long max_flips = 1, checksum = 0;
  for (long idx = idx_min, sign = (idx_min % 2 == 0) ? 1 : -1;
       idx < idx_max; idx++, sign = -sign) {
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

  free(p);
  free(pp);
  free(count);
  *max_out = max_flips;
  *checksum_out = checksum;
}

static void *worker(void *arg) {
  WorkerArg *a = (WorkerArg *)arg;
  fannkuch_range(a->idx_min, a->idx_max, &a->max_flips, &a->checksum);
  return NULL;
}

int main(int argc, char **argv) {
  n = 12;
  if (argc > 1) n = atoi(argv[1]);

  fact = malloc((n + 1) * sizeof(long));
  if (fact == NULL) {
    fprintf(stderr, "fannkuch-redux: out of memory\n");
    return 1;
  }
  fact[0] = 1;
  for (int i = 1; i <= n; i++) fact[i] = fact[i - 1] * i;

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1) {
    fprintf(stderr, "fannkuch-redux: sysconf(_SC_NPROCESSORS_ONLN) failed: %s\n",
            strerror(errno));
    return 1;
  }

  long total = fact[n];
  pthread_t *threads = malloc((size_t)ncpu * sizeof(pthread_t));
  WorkerArg *args = malloc((size_t)ncpu * sizeof(WorkerArg));
  if (threads == NULL || args == NULL) {
    fprintf(stderr, "fannkuch-redux: out of memory\n");
    return 1;
  }
  for (long i = 0; i < ncpu; i++) {
    args[i].idx_min = i * total / ncpu;
    args[i].idx_max = (i + 1) * total / ncpu;
    args[i].max_flips = 1;
    args[i].checksum = 0;
    int rc = pthread_create(&threads[i], NULL, worker, &args[i]);
    if (rc != 0) {
      fprintf(stderr, "fannkuch-redux: pthread_create failed for worker %ld: %s\n",
              i, strerror(rc));
      return 1;
    }
  }
  for (long i = 0; i < ncpu; i++) {
    int rc = pthread_join(threads[i], NULL);
    if (rc != 0) {
      fprintf(stderr, "fannkuch-redux: pthread_join failed for worker %ld: %s\n",
              i, strerror(rc));
      return 1;
    }
  }

  long max_flips = 1, checksum = 0;
  for (long i = 0; i < ncpu; i++) {
    if (max_flips < args[i].max_flips) max_flips = args[i].max_flips;
    checksum += args[i].checksum;
  }

  printf("%ld\nPfannkuchen(%d) = %ld\n", checksum, n, max_flips);
  free(threads);
  free(args);
  free(fact);
  return 0;
}

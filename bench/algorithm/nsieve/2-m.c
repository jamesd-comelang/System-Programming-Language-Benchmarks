// The Computer Language Shootout
// http://shootout.alioth.debian.org/
// Precedent C entry modified by bearophile for speed and size, 31 Jan 2006
//
// Parallel (-m) variant of 1.c: segmented sieve with one POSIX thread
// per CPU (sysconf(_SC_NPROCESSORS_ONLN) workers, no cap, no fallback).
// The base primes up to sqrt(m) are sieved serially (tiny); each worker
// then sieves one contiguous segment [lo, hi) of [0, m) into its own
// byte buffer, marking multiples of every base prime, and counts the
// primes in its segment. Counts are summed in fixed worker order, so
// the total -- and the printed output -- is identical to 1.c.
//
// Marking starts at max(p*p, first multiple of p in the segment): every
// multiple of p below p*p already has a smaller prime factor, and p
// itself is never marked, exactly as in the serial loop (which marks
// from 2p and therefore never marks p). Every composite below m has a
// prime factor <= sqrt(m), so every composite in a segment is marked
// by exactly the base primes, and the per-segment counts add up to
// the serial count.
//
// On thread creation failure, or if the CPU count cannot be determined,
// the program reports the error on stderr and exits nonzero without
// printing a (partial) result for the current call.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned char boolean;

static size_t isqrt_sz(size_t n) {
  size_t r = 0;
  // Integer binary square root: largest r with r*r <= n. No floating
  // point, no overflow: r <= sqrt(n) <= 6426 for the bench inputs, and
  // the (r+1)*(r+1) comparison below is guarded by r+1 <= 2^32.
  size_t lo = 0, hi = n > 0 ? n : 1;
  while (lo <= hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (mid != 0 && mid > n / mid) {
      hi = mid - 1;
    } else {
      r = mid;
      lo = mid + 1;
    }
  }
  return r;
}

// Sieve [0, lim] serially; returns the primes <= lim in a malloc'd
// array (caller frees), with *out_count set. NULL on allocation failure.
static unsigned int *base_primes(size_t lim, size_t *out_count) {
  boolean *is_comp = (boolean *)calloc(lim + 1, sizeof(boolean));
  if (is_comp == NULL) return NULL;
  unsigned int *primes =
      (unsigned int *)malloc((lim + 1) * sizeof(unsigned int));
  if (primes == NULL) {
    free(is_comp);
    return NULL;
  }
  size_t count = 0;
  for (size_t i = 2; i <= lim; ++i) {
    if (!is_comp[i]) {
      primes[count++] = (unsigned int)i;
      if (i * i <= lim) {
        for (size_t j = i * i; j <= lim; j += i) is_comp[j] = 1;
      }
    }
  }
  free(is_comp);
  *out_count = count;
  return primes;
}

typedef struct {
  const unsigned int *primes;
  size_t nprimes;
  size_t lo, hi;
  unsigned int count;
  int failed;  // set if this worker could not allocate its segment
} SegArg;

static void *seg_worker(void *p) {
  SegArg *a = (SegArg *)p;
  a->count = 0;
  a->failed = 0;
  if (a->lo >= a->hi) return NULL;
  size_t len = a->hi - a->lo;
  boolean *flags = (boolean *)malloc(len * sizeof(boolean));
  if (flags == NULL) {
    a->failed = 1;
    return NULL;
  }
  memset(flags, 1, len);
  for (size_t k = 0; k < a->nprimes; ++k) {
    size_t prime = a->primes[k];
    size_t start = prime * prime;
    size_t first = (a->lo + prime - 1) / prime * prime;
    if (first > start) start = first;
    for (size_t j = start; j < a->hi; j += prime) flags[j - a->lo] = 0;
  }
  size_t first_counted = a->lo < 2 ? 2 : a->lo;
  for (size_t i = first_counted; i < a->hi; ++i)
    if (flags[i - a->lo]) ++a->count;
  free(flags);
  return NULL;
}

static int nsieve(int m, size_t workers) {
  size_t n = (size_t)m;
  size_t lim = isqrt_sz(n);
  size_t nprimes = 0;
  unsigned int *primes = base_primes(lim, &nprimes);
  if (primes == NULL) {
    fprintf(stderr, "nsieve: out of memory sieving base primes\n");
    return 1;
  }
  size_t chunk = (n + workers - 1) / workers;
  if (chunk == 0) chunk = 1;
  pthread_t *threads =
      (pthread_t *)malloc(workers * sizeof(pthread_t));
  SegArg *args = (SegArg *)malloc(workers * sizeof(SegArg));
  if (threads == NULL || args == NULL) {
    fprintf(stderr, "nsieve: out of memory allocating thread state\n");
    free(threads);
    free(args);
    free(primes);
    return 1;
  }
  size_t started = 0;
  int rc = 0;
  for (size_t w = 0; w < workers; ++w) {
    size_t lo = w * chunk;
    size_t hi = lo + chunk;
    if (hi > n) hi = n;
    args[w] = (SegArg){primes, nprimes, lo, hi, 0, 0};
    rc = pthread_create(&threads[w], NULL, seg_worker, &args[w]);
    if (rc != 0) {
      fprintf(stderr, "nsieve: pthread_create failed at worker %zu: %s\n",
              w, strerror(rc));
      break;
    }
    ++started;
  }
  for (size_t w = 0; w < started; ++w) pthread_join(threads[w], NULL);
  unsigned int count = 0;
  if (rc == 0) {
    for (size_t w = 0; w < workers; ++w) {
      if (args[w].failed) {
        fprintf(stderr, "nsieve: worker %zu could not allocate its segment\n",
                w);
        rc = 1;
        break;
      }
      count += args[w].count;
    }
  }
  free(threads);
  free(args);
  free(primes);
  if (rc != 0) return 1;
  printf("Primes up to %8u %8u\n", m, count);
  return 0;
}

int main(int argc, char **argv) {
  int m = atoi(argv[1]);
  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1) {
    fprintf(stderr, "nsieve: could not determine CPU count: %s\n",
            strerror(errno));
    return 1;
  }
  size_t workers = (size_t)ncpu;
  for (int i = 0; i < 3; i++) {
    if (nsieve(10000 << (m - i), workers) != 0) return 1;
  }
  return 0;
}

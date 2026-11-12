// Binary trees, NumCPU-worker parallel version of
// bench/algorithm/binarytrees/1.c (standalone C port of
// bench/algorithm/binarytrees/1.go).
//
// Work is split into chunks of near-equal size and claimed by
// sysconf(_SC_NPROCESSORS_ONLN) worker threads through an atomic
// counter. There are three kinds of chunk:
//
//   - the stretch tree (depth max+1, one tree),
//   - the long-lived tree (depth max, one tree),
//   - per depth row, the row's iterations split into 2*ncpu chunks
//     (fewer when a row has fewer iterations).
//
// Every row does the same total work (iterations * 2^(depth+1)
// nodes is constant across rows), so splitting each row into
// 2*ncpu chunks makes every chunk roughly the same size, and the
// atomic claim loop balances the load. Each worker owns a private
// grow-only arena (same bump layout as 1.c, sized up front for the
// largest tree that worker has built so far), so no locking is
// needed while building trees. Checksums are exact integer sums,
// so adding chunk partials in chunk order gives exactly the serial
// result; all printing is done by the main thread after the join,
// in the serial order, so output is byte-identical to 1.c.
//
// Thread count is the machine's online CPU count, capped only by
// the number of chunks (a worker with no chunk would have nothing
// to do); there is no fixed cap. A pthread_create failure joins
// the threads already started and exits 1 with a message; an
// arena allocation failure inside a worker prints a message and
// exits 1. Nothing is silently downgraded to serial.

// NOTE on bench/algorithm/binarytrees/2.c: that file is NOT a
// usable parallel option here. It needs APR (apr_pools.h), which
// is not part of the main C build (bench/include/c/app.rsp links
// only libc/libm), and its OpenMP parallel region shares one APR
// pool between the long-lived-tree thread and the row threads.
// This file is therefore a fresh parallel port of 1.c instead,
// using only libc/libm/pthreads, which the C build already links.

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Node {
  struct Node *left, *right;
} Node;

// Grow-only bump arena, one per worker thread. Capacity is raised
// on demand to fit the largest tree the worker has built so far
// (a tree of depth d has 2^(d+1)-1 nodes, so 2^(d+1) slots).
typedef struct {
  Node *buf;
  size_t cap;
  size_t used;
} Arena;

static void arena_ensure(Arena *a, int depth) {
  size_t need = (size_t)2 << depth;
  if (a->cap >= need) return;
  free(a->buf);
  a->buf = malloc(need * sizeof(Node));
  if (!a->buf) {
    fprintf(stderr, "binarytrees: arena allocation failed\n");
    exit(1);
  }
  a->cap = need;
  a->used = 0;
}

static Node *new_node(Arena *a) {
  if (a->used >= a->cap) {
    fprintf(stderr, "binarytrees: arena exhausted\n");
    exit(1);
  }
  Node *n = &a->buf[a->used++];
  n->left = NULL;
  n->right = NULL;
  return n;
}

static Node *bottom_up_tree(Arena *a, int depth) {
  Node *n = new_node(a);
  if (depth > 0) {
    n->left = bottom_up_tree(a, depth - 1);
    n->right = bottom_up_tree(a, depth - 1);
  }
  return n;
}

static long item_check(const Node *n) {
  if (n->left == NULL) return 1;
  return 1 + item_check(n->left) + item_check(n->right);
}

enum ChunkKind { CH_STRETCH, CH_LONGLIVED, CH_ROW };

typedef struct {
  int depth;   // tree depth for every tree in this chunk
  long count;  // number of trees to build and check
  int row;     // depth-row index for CH_ROW, -1 otherwise
} Chunk;

static long run_chunk(Arena *a, const Chunk *c) {
  arena_ensure(a, c->depth);
  long sum = 0;
  for (long i = 0; i < c->count; i++) {
    a->used = 0;
    sum += item_check(bottom_up_tree(a, c->depth));
  }
  return sum;
}

typedef struct {
  const Chunk *chunks;
  long nchunks;
  long *results;
  _Atomic long *next;
} WorkerArg;

static void *worker_main(void *p) {
  WorkerArg *w = (WorkerArg *)p;
  Arena a = {0};
  for (;;) {
    long idx = atomic_fetch_add(w->next, 1);
    if (idx >= w->nchunks) break;
    w->results[idx] = run_chunk(&a, &w->chunks[idx]);
  }
  free(a.buf);
  return NULL;
}

int main(int argc, char **argv) {
  const int min_depth = 4;
  int max_depth = 5;
  if (argc > 1) max_depth = atoi(argv[1]);
  if (min_depth + 2 > max_depth) max_depth = min_depth + 2;
  int stretch_depth = max_depth + 1;

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1) {
    fprintf(stderr, "binarytrees: could not determine CPU count\n");
    return 1;
  }

  int nrows = (max_depth - min_depth) / 2 + 1;
  long per_row = 2 * ncpu; // chunks per depth row
  // Upper bound on the chunk count: 2 single-tree chunks plus
  // per_row chunks per row.
  long chunk_cap = 2 + (long)nrows * per_row;
  Chunk *chunks = malloc((size_t)chunk_cap * sizeof(Chunk));
  long *results = malloc((size_t)chunk_cap * sizeof(long));
  int *row_first = malloc((size_t)(nrows + 1) * sizeof(int));
  if (!chunks || !results || !row_first) return 1;

  long nchunks = 0;
  chunks[nchunks++] = (Chunk){stretch_depth, 1, -1};
  chunks[nchunks++] = (Chunk){max_depth, 1, -1};
  for (int r = 0; r < nrows; r++) {
    int depth = min_depth + 2 * r;
    long iterations = 1L << (max_depth - depth + min_depth);
    row_first[r] = (int)nchunks;
    long nrow_chunks = iterations < per_row ? iterations : per_row;
    long base = iterations / nrow_chunks;
    long rem = iterations % nrow_chunks;
    for (long j = 0; j < nrow_chunks; j++) {
      long cnt = base + (j < rem ? 1 : 0);
      chunks[nchunks++] = (Chunk){depth, cnt, r};
    }
  }
  row_first[nrows] = (int)nchunks;

  long nworkers = ncpu < nchunks ? ncpu : nchunks;
  pthread_t *threads = malloc((size_t)nworkers * sizeof(pthread_t));
  if (!threads) return 1;
  _Atomic long next = 0;
  WorkerArg warg = {chunks, nchunks, results, &next};
  long started = 0;
  for (long i = 0; i < nworkers; i++) {
    int rc = pthread_create(&threads[i], NULL, worker_main, &warg);
    if (rc != 0) {
      fprintf(stderr, "binarytrees: pthread_create failed at worker %ld: %s\n",
              i, strerror(rc));
      for (long j = 0; j < started; j++) pthread_join(threads[j], NULL);
      return 1;
    }
    started++;
  }
  for (long i = 0; i < started; i++) pthread_join(threads[i], NULL);
  free(threads);

  printf("stretch tree of depth %d\t check: %ld\n", stretch_depth, results[0]);
  for (int r = 0; r < nrows; r++) {
    int depth = min_depth + 2 * r;
    long iterations = 1L << (max_depth - depth + min_depth);
    long check = 0;
    for (int i = row_first[r]; i < row_first[r + 1]; i++) check += results[i];
    printf("%ld\t trees of depth %d\t check: %ld\n", iterations, depth, check);
  }
  printf("long lived tree of depth %d\t check: %ld\n", max_depth, results[1]);
  free(chunks);
  free(results);
  free(row_first);
  return 0;
}

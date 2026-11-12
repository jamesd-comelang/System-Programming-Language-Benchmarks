// Merkle trees, parallel (-m) variant of bench/algorithm/merkletrees/1.c.
// A leaf's hash is 1; an internal node's hash is the sum of its
// children's hashes, so a tree of depth d has root hash 2^d. The
// tree is built for real (arena-allocated, as in the serial version)
// rather than shortcutting to the closed form.
//
// Parallelism: the per-depth loop builds many independent trees
// (iterations = 2^(max_depth - depth + 4)), so its iterations are
// split into contiguous chunks over NumCPU POSIX threads. Each
// worker owns a private arena and returns a partial sum; the main
// thread adds the partial sums in worker order, so the printed
// sums are byte-identical to the serial version. The single stretch
// tree and the long-lived tree stay serial. On pthread_create
// failure the program reports the failing worker and exits nonzero
// without printing that depth's line.

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Node {
  long hash; // -1 = not yet computed
  long value; // leaf value, valid only when is_leaf
  int is_leaf;
  struct Node *left, *right;
} Node;

typedef struct {
  Node *nodes;
  size_t used, cap;
} Arena;

static Node *make_tree(Arena *a, int depth) {
  if (a->used >= a->cap) {
    fprintf(stderr, "merkletrees: arena exhausted\n");
    exit(1);
  }
  Node *n = &a->nodes[a->used++];
  n->hash = -1;
  if (depth <= 0) {
    n->is_leaf = 1;
    n->value = 1;
    n->left = n->right = NULL;
  } else {
    n->is_leaf = 0;
    n->value = 0;
    n->left = make_tree(a, depth - 1);
    n->right = make_tree(a, depth - 1);
  }
  return n;
}

static long cal_hash(Node *n) {
  if (n->hash != -1) return n->hash;
  if (n->is_leaf) {
    n->hash = n->value;
  } else {
    n->hash = cal_hash(n->left) + cal_hash(n->right);
  }
  return n->hash;
}

static int check_tree(const Node *n) {
  if (n->hash == -1) return 0;
  if (n->is_leaf) return 1;
  return check_tree(n->left) && check_tree(n->right);
}

typedef struct {
  int depth;
  long first, count;
  long sum;
} WorkerArg;

static void *loop_worker(void *p) {
  WorkerArg *w = (WorkerArg *)p;
  Arena a;
  // A tree of depth d has 2^(d+1)-1 nodes; allocate 2^(d+1) slots.
  a.cap = (size_t)2 << w->depth;
  a.nodes = malloc(a.cap * sizeof(Node));
  if (!a.nodes) {
    fprintf(stderr, "merkletrees: worker arena allocation failed\n");
    exit(1);
  }
  long sum = 0;
  for (long i = 0; i < w->count; i++) {
    a.used = 0;
    Node *t = make_tree(&a, w->depth);
    sum += cal_hash(t);
  }
  free(a.nodes);
  w->sum = sum;
  return NULL;
}

// Builds `iterations` trees of the given depth across `nworkers`
// threads and returns their total root-hash sum. Exits nonzero
// (after joining any started threads) if a thread cannot be created.
static long parallel_sum(int depth, long iterations, int nworkers) {
  int workers = nworkers;
  if ((long)workers > iterations) workers = (int)iterations;
  pthread_t *threads = malloc((size_t)workers * sizeof(pthread_t));
  WorkerArg *args = malloc((size_t)workers * sizeof(WorkerArg));
  if (!threads || !args) {
    fprintf(stderr, "merkletrees: allocation failed\n");
    return -1;
  }
  long base = iterations / workers;
  long rem = iterations % workers;
  long first = 0;
  int started = 0;
  for (int i = 0; i < workers; i++) {
    long count = base + (i < rem ? 1 : 0);
    args[i] = (WorkerArg){depth, first, count, 0};
    first += count;
    int rc = pthread_create(&threads[i], NULL, loop_worker, &args[i]);
    if (rc != 0) {
      fprintf(stderr, "merkletrees: pthread_create failed at worker %d: %s\n",
              i, strerror(rc));
      for (int j = 0; j < started; j++) pthread_join(threads[j], NULL);
      free(threads);
      free(args);
      return -1;
    }
    started++;
  }
  long sum = 0;
  for (int i = 0; i < workers; i++) {
    pthread_join(threads[i], NULL);
    sum += args[i].sum;
  }
  free(threads);
  free(args);
  return sum;
}

int main(int argc, char **argv) {
  const int min_depth = 4;
  int max_depth = 5;
  if (argc > 1) max_depth = atoi(argv[1]);
  if (min_depth + 2 > max_depth) max_depth = min_depth + 2;
  int stretch_depth = max_depth + 1;

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1) {
    fprintf(stderr, "merkletrees: cannot determine CPU count\n");
    return 1;
  }

  Arena arena;
  arena.cap = (size_t)2 << stretch_depth;
  arena.nodes = malloc(arena.cap * sizeof(Node));
  if (!arena.nodes) return 1;

  arena.used = 0;
  Node *stretch = make_tree(&arena, stretch_depth);
  // Compute hash and check in sequence, not as printf arguments:
  // argument evaluation order is unspecified, and check_tree must
  // run after cal_hash has filled in every node's hash.
  long stretch_hash = cal_hash(stretch);
  int stretch_check = check_tree(stretch);
  printf("stretch tree of depth %d\t root hash: %ld check: %s\n",
         stretch_depth, stretch_hash, stretch_check ? "true" : "false");

  // Long-lived tree: take its hash/check now, as in binarytrees/1.c
  // the loop trees reuse the arena afterwards.
  arena.used = 0;
  Node *long_lived = make_tree(&arena, max_depth);
  long long_hash = cal_hash(long_lived);
  int long_check = check_tree(long_lived);

  for (int depth = min_depth; depth <= max_depth; depth += 2) {
    long iterations = 1L << (max_depth - depth + min_depth);
    long sum = parallel_sum(depth, iterations, (int)ncpu);
    if (sum < 0) {
      free(arena.nodes);
      return 1;
    }
    printf("%ld\t trees of depth %d\t root hash sum: %ld\n", iterations,
           depth, sum);
  }
  printf("long lived tree of depth %d\t root hash: %ld check: %s\n",
         max_depth, long_hash, long_check ? "true" : "false");
  free(arena.nodes);
  return 0;
}

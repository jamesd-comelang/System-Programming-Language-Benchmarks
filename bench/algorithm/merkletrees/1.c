// Merkle trees, standalone C port of bench/algorithm/merkletrees/1.go.
// A leaf's hash is 1; an internal node's hash is the sum of its
// children's hashes, so a tree of depth d has root hash 2^d. The
// tree is built for real (arena-allocated, as in the Go version)
// rather than shortcutting to the closed form.

#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
  long hash; // -1 = not yet computed
  long value; // leaf value, valid only when is_leaf
  int is_leaf;
  struct Node *left, *right;
} Node;

static Node *arena;
static size_t arena_used, arena_cap;

static Node *make_tree(int depth) {
  if (arena_used >= arena_cap) {
    fprintf(stderr, "merkletrees: arena exhausted\n");
    exit(1);
  }
  Node *n = &arena[arena_used++];
  n->hash = -1;
  if (depth <= 0) {
    n->is_leaf = 1;
    n->value = 1;
    n->left = n->right = NULL;
  } else {
    n->is_leaf = 0;
    n->value = 0;
    n->left = make_tree(depth - 1);
    n->right = make_tree(depth - 1);
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

int main(int argc, char **argv) {
  const int min_depth = 4;
  int max_depth = 5;
  if (argc > 1) max_depth = atoi(argv[1]);
  if (min_depth + 2 > max_depth) max_depth = min_depth + 2;
  int stretch_depth = max_depth + 1;

  arena_cap = (size_t)2 << stretch_depth;
  arena = malloc(arena_cap * sizeof(Node));
  if (!arena) return 1;

  arena_used = 0;
  Node *stretch = make_tree(stretch_depth);
  // Compute hash and check in sequence, not as printf arguments:
  // argument evaluation order is unspecified, and check_tree must
  // run after cal_hash has filled in every node's hash.
  long stretch_hash = cal_hash(stretch);
  int stretch_check = check_tree(stretch);
  printf("stretch tree of depth %d\t root hash: %ld check: %s\n",
         stretch_depth, stretch_hash, stretch_check ? "true" : "false");

  // Long-lived tree: take its hash/check now, as in binarytrees/1.c
  // the loop trees reuse the arena afterwards.
  arena_used = 0;
  Node *long_lived = make_tree(max_depth);
  long long_hash = cal_hash(long_lived);
  int long_check = check_tree(long_lived);

  for (int depth = min_depth; depth <= max_depth; depth += 2) {
    long iterations = 1L << (max_depth - depth + min_depth);
    long sum = 0;
    for (long i = 0; i < iterations; i++) {
      arena_used = 0;
      Node *t = make_tree(depth);
      sum += cal_hash(t);
    }
    printf("%ld\t trees of depth %d\t root hash sum: %ld\n", iterations,
           depth, sum);
  }
  printf("long lived tree of depth %d\t root hash: %ld check: %s\n",
         max_depth, long_hash, long_check ? "true" : "false");
  free(arena);
  return 0;
}

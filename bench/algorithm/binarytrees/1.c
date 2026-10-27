// Binary trees, standalone C port of bench/algorithm/binarytrees/1.go
// (the Go Authors' version, based on the C program by Kevin Carson).
//
// NOTE: bench/algorithm/binarytrees/2.c is NOT usable in the main C
// config: it depends on APR (apr_pools.h), which is not part of the
// C build (bench/include/c/app.rsp links only libc/libm). That is
// why 2.c is commented out in bench/bench_c.yaml. This file is a
// self-contained replacement: nodes are allocated from one reusable
// arena buffer, reset between trees, so no per-node malloc is needed.

#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
  struct Node *left, *right;
} Node;

static Node *arena;
static size_t arena_used, arena_cap;

static Node *new_node(void) {
  if (arena_used >= arena_cap) {
    fprintf(stderr, "binarytrees: arena exhausted\n");
    exit(1);
  }
  Node *n = &arena[arena_used++];
  n->left = NULL;
  n->right = NULL;
  return n;
}

static Node *bottom_up_tree(int depth) {
  Node *n = new_node();
  if (depth > 0) {
    n->left = bottom_up_tree(depth - 1);
    n->right = bottom_up_tree(depth - 1);
  }
  return n;
}

static long item_check(const Node *n) {
  if (n->left == NULL) return 1;
  return 1 + item_check(n->left) + item_check(n->right);
}

int main(int argc, char **argv) {
  const int min_depth = 4;
  int max_depth = 5;
  if (argc > 1) max_depth = atoi(argv[1]);
  if (min_depth + 2 > max_depth) max_depth = min_depth + 2;
  int stretch_depth = max_depth + 1;

  // Largest tree we will build has 2^(stretch_depth+1)-1 nodes.
  arena_cap = (size_t)2 << stretch_depth; // >= 2^(stretch_depth+1)
  arena = malloc(arena_cap * sizeof(Node));
  if (!arena) return 1;

  arena_used = 0;
  printf("stretch tree of depth %d\t check: %ld\n", stretch_depth,
         item_check(bottom_up_tree(stretch_depth)));

  // The checksum is taken now: later trees reuse the same arena and
  // would overwrite this tree's nodes, but only its checksum is ever
  // printed, at the end.
  arena_used = 0;
  long long_lived_check = item_check(bottom_up_tree(max_depth));

  for (int depth = min_depth; depth <= max_depth; depth += 2) {
    long iterations = 1L << (max_depth - depth + min_depth);
    long check = 0;
    for (long i = 0; i < iterations; i++) {
      arena_used = 0;
      check += item_check(bottom_up_tree(depth));
    }
    printf("%ld\t trees of depth %d\t check: %ld\n", iterations, depth, check);
  }
  printf("long lived tree of depth %d\t check: %ld\n", max_depth,
         long_lived_check);
  free(arena);
  return 0;
}

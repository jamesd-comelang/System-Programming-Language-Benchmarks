// LRU cache benchmark, C port of bench/algorithm/lru/1.go.
// The Go version uses an ordered map; this version keeps the same
// observable behaviour (hit/miss counts) with a hash table plus a
// doubly linked list ordered least- to most-recently used.
// RNG: LCG with A=1103515245, C=12345, M=2^31, seeds 0 and 1.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A 1103515245u
#define C 12345u
#define M (1u << 31)

static uint32_t rng_state;
static uint32_t lcg(void) {
  rng_state = (A * rng_state + C) % M;
  return rng_state;
}

typedef struct Node {
  uint32_t key, value;
  int prev, next; // indices into nodes[], -1 = none
  int used;
} Node;

static Node *nodes;
static int *table; // open addressing, stores node index+1, 0 = empty
static size_t table_mask;
static int head, tail; // head = least recently used
static int size_cap, used_count;

static size_t hash_key(uint32_t k) {
  // Knuth multiplicative hash.
  return (size_t)(k * 2654435761u) & table_mask;
}

static int table_find(uint32_t key) {
  size_t i = hash_key(key);
  while (table[i] != 0) {
    if (nodes[table[i] - 1].key == key) return table[i] - 1;
    i = (i + 1) & table_mask;
  }
  return -1;
}

static void table_insert(int idx) {
  size_t i = hash_key(nodes[idx].key);
  while (table[i] != 0) i = (i + 1) & table_mask;
  table[i] = idx + 1;
}

static void table_remove(uint32_t key) {
  size_t i = hash_key(key);
  while (table[i] != 0) {
    if (nodes[table[i] - 1].key == key) break;
    i = (i + 1) & table_mask;
  }
  if (table[i] == 0) return;
  table[i] = 0;
  // Rehash the following cluster.
  size_t j = (i + 1) & table_mask;
  while (table[j] != 0) {
    int idx = table[j] - 1;
    table[j] = 0;
    table_insert(idx);
    j = (j + 1) & table_mask;
  }
}

static void unlink_node(int idx) {
  int p = nodes[idx].prev, nx = nodes[idx].next;
  if (p >= 0)
    nodes[p].next = nx;
  else
    head = nx;
  if (nx >= 0)
    nodes[nx].prev = p;
  else
    tail = p;
  nodes[idx].prev = nodes[idx].next = -1;
}

static void move_to_tail(int idx) {
  if (tail == idx) return;
  unlink_node(idx);
  nodes[idx].prev = tail;
  nodes[idx].next = -1;
  if (tail >= 0) nodes[tail].next = idx;
  tail = idx;
  if (head < 0) head = idx;
}

// Returns 1 on hit.
static int lru_get(uint32_t key) {
  int idx = table_find(key);
  if (idx < 0) return 0;
  move_to_tail(idx);
  return 1;
}

static void lru_put(uint32_t key, uint32_t value) {
  int idx = table_find(key);
  if (idx >= 0) {
    nodes[idx].value = value;
    move_to_tail(idx);
    return;
  }
  if (used_count == size_cap) {
    // Evict the least recently used node and reuse its slot.
    idx = head;
    table_remove(nodes[idx].key);
    unlink_node(idx);
    used_count--;
  } else {
    // Find a free slot.
    for (idx = 0; idx < size_cap; idx++)
      if (!nodes[idx].used) break;
  }
  nodes[idx].used = 1;
  nodes[idx].key = key;
  nodes[idx].value = value;
  nodes[idx].prev = tail;
  nodes[idx].next = -1;
  if (tail >= 0) nodes[tail].next = idx;
  tail = idx;
  if (head < 0) head = idx;
  table_insert(idx);
  used_count++;
}

int main(int argc, char **argv) {
  int size = 100, n = 10000;
  if (argc > 1) size = atoi(argv[1]);
  if (argc > 2) n = atoi(argv[2]);
  size_cap = size;

  nodes = calloc((size_t)size, sizeof(Node));
  size_t tcap = 16;
  while (tcap < (size_t)size * 4) tcap *= 2;
  table = calloc(tcap, sizeof(int));
  table_mask = tcap - 1;
  head = tail = -1;

  uint32_t m = (uint32_t)size * 10;
  long hit = 0, missed = 0;
  rng_state = 0;
  uint32_t seed1 = 1;
  for (int i = 0; i < n; i++) {
    uint32_t n0 = lcg() % m;
    lru_put(n0, n0);
    // Second stream, seed 1, sharing the same LCG step.
    uint32_t saved = rng_state;
    rng_state = seed1;
    uint32_t n1 = lcg() % m;
    seed1 = rng_state;
    rng_state = saved;
    if (lru_get(n1))
      hit++;
    else
      missed++;
  }
  printf("%ld\n%ld\n", hit, missed);
  free(nodes);
  free(table);
  return 0;
}

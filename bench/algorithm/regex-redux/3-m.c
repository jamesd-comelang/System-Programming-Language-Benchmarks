// Regex-redux, parallel (-m) variant of bench/algorithm/regex-redux/2.c.
//
// Same literal-scan algorithm as 2.c (no regex engine), parallelized
// with NumCPU POSIX threads: one worker per online CPU, no
// one-thread-per-row. The pipeline is split into phases, each phase
// spread over the workers; the phases run in order, so the program
// uses exactly NumCPU threads at any time.
//
//   - clean: serial (one streaming pass over the input).
//   - variant counts: the input is split into NumCPU contiguous
//     chunks. Each worker scans its chunk and records every literal
//     hit (position + variant bitmask). The main thread then replays
//     the hits in global position order with the same per-variant
//     skip-ahead rule as the serial count (a match at p is counted
//     for variant v only if p >= next[v], then next[v] = p + 8).
//     Recording every hit first (instead of counting per chunk) is
//     what makes the chunking exact: a match in one chunk can change
//     which matches count in the next, and the replay sees the hits
//     in exactly the serial order.
//   - each substitution: the same record-then-replay shape. Each
//     worker scans its chunk and records every occurrence (start,
//     end) of that substitution's pattern -- every position where
//     the serial match predicate holds, including positions inside
//     a match the serial scan would have skipped over. The main
//     thread replays the occurrences in order with the serial
//     leftmost, non-overlapping rule (accept if start >= next, then
//     next = end), which reproduces the serial replace-all exactly.
//     The accepted matches are then split by count over the workers
//     and each worker emits its slice directly (gap copies +
//     replacement), so no pass re-scans its input. A worker whose
//     slice is empty still copies the gap between its neighbouring
//     accepted matches, so concatenating the workers' outputs in
//     order gives exactly the serial output.
//
// Output is byte-identical to 2.c (verified against 25000_out,
// 250000_out, and the serial binary on 2500000_in). A pthread_create
// failure is reported on stderr and exits nonzero; thread count is
// the online CPU count with no cap and no fallback.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  char *data;
  size_t len, cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra + 1 > b->cap) {
    while (b->len + extra + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 1024;
    b->data = realloc(b->data, b->cap);
  }
}

static void buf_append(Buf *b, const char *p, size_t n) {
  buf_reserve(b, n);
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
}

// Clean: drop ">header\n" lines and every "\n", Go-regex semantics
// (a '>' immediately followed by a newline is kept, the newline
// itself is dropped as a lone newline on the next step).
static void clean_seq(const char *in, size_t len, Buf *out) {
  size_t start = 0;
  size_t i = 0;
  while (i < len) {
    char c = in[i];
    if (c == '>') {
      // Header line: '>' + at least one non-newline char + newline.
      size_t j = i + 1;
      while (j < len && in[j] != '\n') j++;
      if (j < len && j > i + 1) {
        buf_append(out, in + start, i - start);
        i = j + 1;
        start = i;
        continue;
      }
      // No header match here ('>' at EOF, or '>' + '\n'):
      // keep '>' and let the newline be handled next round.
      i++;
    } else if (c == '\n') {
      buf_append(out, in + start, i - start);
      i++;
      start = i;
    } else {
      i++;
    }
  }
  buf_append(out, in + start, len - start);
}

static uint64_t pack8(const char *b) {
  return (uint64_t)(unsigned char)b[0] |
         (uint64_t)(unsigned char)b[1] << 8 |
         (uint64_t)(unsigned char)b[2] << 16 |
         (uint64_t)(unsigned char)b[3] << 24 |
         (uint64_t)(unsigned char)b[4] << 32 |
         (uint64_t)(unsigned char)b[5] << 40 |
         (uint64_t)(unsigned char)b[6] << 48 |
         (uint64_t)(unsigned char)b[7] << 56;
}

// Small open-addressing table for the 50 variant literals (128 slots,
// multiplicative hash, linear probe, key 0 = empty, which no literal
// packs to since every literal byte is nonzero ASCII). Built once
// before any worker starts; workers only read it.
typedef struct {
  uint64_t keys[128];
  uint16_t vals[128];
} LitTable;

static void lit_put(LitTable *t, uint64_t key, uint16_t v) {
  size_t h = (size_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> (64 - 7));
  for (;;) {
    if (t->keys[h] == 0) {
      t->keys[h] = key;
      t->vals[h] = v;
      return;
    }
    if (t->keys[h] == key) {
      t->vals[h] = (uint16_t)(t->vals[h] | v);
      return;
    }
    h = (h + 1) & 127;
  }
}

static int lit_get(LitTable *t, uint64_t key, uint16_t *v) {
  size_t h = (size_t)((key * UINT64_C(0x9E3779B97F4A7C15)) >> (64 - 7));
  for (;;) {
    if (t->keys[h] == 0) return 0;
    if (t->keys[h] == key) {
      *v = t->vals[h];
      return 1;
    }
    h = (h + 1) & 127;
  }
}

static const char *variant_literals[9][6] = {
    {"agggtaaa", "tttaccct", 0, 0, 0, 0},
    {"cgggtaaa", "ggggtaaa", "tgggtaaa", "tttaccca", "tttacccc",
     "tttacccg"},
    {"aaggtaaa", "acggtaaa", "atggtaaa", "tttaccat", "tttaccgt",
     "tttacctt"},
    {"agagtaaa", "agcgtaaa", "agtgtaaa", "tttacact", "tttacgct",
     "tttactct"},
    {"aggataaa", "aggctaaa", "aggttaaa", "tttaacct", "tttagcct",
     "tttatcct"},
    {"agggaaaa", "agggcaaa", "aggggaaa", "tttcccct", "tttgccct",
     "ttttccct"},
    {"agggtcaa", "agggtgaa", "agggttaa", "ttaaccct", "ttcaccct",
     "ttgaccct"},
    {"agggtaca", "agggtaga", "agggtata", "tataccct", "tctaccct",
     "tgtaccct"},
    {"agggtaac", "agggtaag", "agggtaat", "attaccct", "cttaccct",
     "gttaccct"},
};

// ---- parallel scaffolding ----

// Number of workers = online CPUs (no cap, no fallback; <= 0 is fatal).
static long num_workers(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) {
    fprintf(stderr, "regex-redux: cannot determine online CPU count\n");
    exit(1);
  }
  return n;
}

// Chunk [s, e) of [0, len) for worker w of n (contiguous, covering).
static void chunk_range(size_t len, long w, long n, size_t *s, size_t *e) {
  *s = (size_t)((uint64_t)w * (uint64_t)len / (uint64_t)n);
  *e = (size_t)((uint64_t)(w + 1) * (uint64_t)len / (uint64_t)n);
}

// Spawn n threads running fn(args[w]); on failure, join the ones
// already started and exit nonzero. Joins all threads on success.
static void run_workers(long n, void *(*fn)(void *), void **args) {
  pthread_t *threads = malloc((size_t)n * sizeof(pthread_t));
  long started = 0;
  for (long w = 0; w < n; w++) {
    int rc = pthread_create(&threads[w], NULL, fn, args[w]);
    if (rc != 0) {
      fprintf(stderr, "regex-redux: pthread_create failed at worker %ld: %s\n",
              w, strerror(rc));
      for (long j = 0; j < started; j++) pthread_join(threads[j], NULL);
      free(threads);
      exit(1);
    }
    started++;
  }
  for (long w = 0; w < n; w++) pthread_join(threads[w], NULL);
  free(threads);
}

// ---- variant counts ----

typedef struct {
  size_t pos;
  uint16_t mask;
} Hit;

typedef struct {
  Hit *hits;
  size_t len, cap;
} HitList;

static void hit_push(HitList *l, size_t pos, uint16_t mask) {
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 1024;
    l->hits = realloc(l->hits, l->cap * sizeof(Hit));
  }
  l->hits[l->len].pos = pos;
  l->hits[l->len].mask = mask;
  l->len++;
}

typedef struct {
  const char *seq;
  size_t len, start, end;
  LitTable *table;
  HitList *out;
} CountArg;

static void *count_worker(void *p) {
  CountArg *a = (CountArg *)p;
  for (size_t i = a->start; i < a->end && i + 8 <= a->len; i++) {
    uint16_t m;
    if (!lit_get(a->table, pack8(a->seq + i), &m)) continue;
    hit_push(a->out, i, m);
  }
  return NULL;
}

// ---- substitutions ----

typedef struct {
  size_t start, end;
} Occ;

typedef struct {
  Occ *occs;
  size_t len, cap;
} OccList;

static void occ_push(OccList *l, size_t start, size_t end) {
  if (l->len == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 1024;
    l->occs = realloc(l->occs, l->cap * sizeof(Occ));
  }
  l->occs[l->len].start = start;
  l->occs[l->len].end = end;
  l->len++;
}

static int match_sub1(const char *p) {
  return p[0] == 't' && p[1] == 'H' && p[2] == 'a' &&
         (p[3] == 'N' || p[3] == 't');
}

static int match_sub2(const char *p) {
  switch (p[0]) {
    case 'a': return p[1] == 'N' && p[2] == 'D';
    case 'c': return p[1] == 'a' && p[2] == 'N';
    case 'H': return p[1] == 'a' && (p[2] == 'D' || p[2] == 'S');
    case 'W': return p[1] == 'a' && p[2] == 'S';
    default: return 0;
  }
}

static int match_sub3(const char *p) {
  switch (p[0]) {
    case 'a': return p[1] == 'N' || p[1] == 'S' || p[1] == 't';
    case 'B': return p[1] == 'Y';
    default: return 0;
  }
}

// The five substitutions, in order: fixed-length literal (subst 0-2),
// '<' runs to the first '>' (subst 3), '|' runs of >= 1 non-'|'
// closed by a '|' (subst 4). Same predicates as 2.c.
static const char *subst_rep[5] = {"<4>", "<3>", "<2>", "|", "-"};
static const size_t subst_rep_len[5] = {3, 3, 3, 1, 1};
static const size_t subst_lit_len[5] = {4, 3, 2, 0, 0};

typedef struct {
  const char *in;
  size_t len, start, end;
  int which;
  OccList *out;
} ScanArg;

// Record every occurrence of substitution `which` with a start in
// [start, end). Reads may run past `end` (a fixed literal or a run
// closing in the next chunk) but never past `len`; the input is
// read-only, so this is safe. Every position is tested, including
// positions inside an occurrence the serial scan would skip -- the
// replay below discards those, which is exactly the serial rule.
static void *scan_worker(void *p) {
  ScanArg *a = (ScanArg *)p;
  const char *in = a->in;
  size_t len = a->len;
  switch (a->which) {
    case 0:
    case 1:
    case 2: {
      size_t lit = subst_lit_len[a->which];
      int (*match)(const char *) =
          a->which == 0 ? match_sub1 : a->which == 1 ? match_sub2 : match_sub3;
      for (size_t i = a->start; i < a->end && i + lit <= len; i++) {
        if (match(in + i)) occ_push(a->out, i, i + lit);
      }
      break;
    }
    case 3: {
      size_t i = a->start;
      while (i < a->end) {
        if (in[i] == '<') {
          size_t j = i + 1;
          while (j < len && in[j] != '>') j++;
          if (j < len) occ_push(a->out, i, j + 1);
        }
        i++;
      }
      break;
    }
    case 4: {
      size_t i = a->start;
      while (i < a->end) {
        if (in[i] == '|') {
          size_t j = i + 1;
          while (j < len && in[j] != '|') j++;
          if (j < len && j > i + 1) occ_push(a->out, i, j + 1);
        }
        i++;
      }
      break;
    }
  }
  return NULL;
}

typedef struct {
  const char *in;
  const Occ *acc;  // accepted matches, global order
  size_t lo, hi;   // this worker's slice of acc
  size_t nacc;
  size_t in_len;
  const char *rep;
  size_t rep_len;
  Buf *out;
} EmitArg;

// Emit the output for accepted matches [lo, hi). This worker's input
// span runs from the start of its first accepted match (or 0) to the
// start of the next worker's first accepted match (or in_len): the
// spans are delimited by match starts, so they tile [0, in_len)
// exactly, and the gap before a match is copied by the worker whose
// span ends at that match's start. Matches outside the slice cannot
// start inside this span (the replay accepted the first occurrence
// at or after `next`, so no occurrence starts in a gap), so gap
// copies + replacements here are exactly the serial output for this
// span, including the empty-slice case (a pure gap copy, or nothing
// when the span is empty because a neighbouring worker owns the gap).
static void *emit_worker(void *p) {
  EmitArg *a = (EmitArg *)p;
  size_t in_start = a->lo == 0 ? 0 : a->acc[a->lo].start;
  size_t in_end = a->hi >= a->nacc ? a->in_len : a->acc[a->hi].start;
  size_t prev = in_start;
  for (size_t k = a->lo; k < a->hi; k++) {
    buf_append(a->out, a->in + prev, a->acc[k].start - prev);
    buf_append(a->out, a->rep, a->rep_len);
    prev = a->acc[k].end;
  }
  buf_append(a->out, a->in + prev, in_end - prev);
  return NULL;
}

// One substitution pass over `cur`, parallelized. Returns the new
// buffer (caller frees `cur`); on zero matches returns a copy of `cur`.
static Buf subst_parallel(const Buf *cur, int which, long nworkers) {
  const char *in = cur->data;
  size_t len = cur->len;

  OccList *lists = calloc((size_t)nworkers, sizeof(OccList));
  ScanArg *sargs = malloc((size_t)nworkers * sizeof(ScanArg));
  void **pargs = malloc((size_t)nworkers * sizeof(void *));
  for (long w = 0; w < nworkers; w++) {
    size_t s, e;
    chunk_range(len, w, nworkers, &s, &e);
    sargs[w] = (ScanArg){in, len, s, e, which, &lists[w]};
    pargs[w] = &sargs[w];
  }
  run_workers(nworkers, scan_worker, pargs);

  // Replay in global order: accept the first occurrence at or after
  // `next`, exactly the serial leftmost, non-overlapping rule.
  size_t total = 0;
  for (long w = 0; w < nworkers; w++) total += lists[w].len;
  Occ *acc = malloc((total ? total : 1) * sizeof(Occ));
  size_t nacc = 0;
  size_t next = 0;
  for (long w = 0; w < nworkers; w++) {
    for (size_t k = 0; k < lists[w].len; k++) {
      size_t s = lists[w].occs[k].start, e = lists[w].occs[k].end;
      if (s >= next) {
        acc[nacc].start = s;
        acc[nacc].end = e;
        nacc++;
        next = e;
      }
    }
    free(lists[w].occs);
  }
  free(lists);
  free(sargs);
  free(pargs);

  Buf out = {0};
  if (nacc == 0) {
    buf_append(&out, in, len);
    free(acc);
    return out;
  }

  Buf *pieces = calloc((size_t)nworkers, sizeof(Buf));
  EmitArg *eargs = malloc((size_t)nworkers * sizeof(EmitArg));
  for (long w = 0; w < nworkers; w++) {
    size_t lo = (size_t)((uint64_t)w * (uint64_t)nacc / (uint64_t)nworkers);
    size_t hi =
        (size_t)((uint64_t)(w + 1) * (uint64_t)nacc / (uint64_t)nworkers);
    eargs[w] = (EmitArg){in,        acc,       lo,       hi,
                         nacc,     len,       subst_rep[which],
                         subst_rep_len[which], &pieces[w]};
  }
  // (re)build the arg pointer array for the emit workers
  void **epargs = malloc((size_t)nworkers * sizeof(void *));
  for (long w = 0; w < nworkers; w++) epargs[w] = &eargs[w];
  run_workers(nworkers, emit_worker, epargs);
  free(epargs);
  free(eargs);

  for (long w = 0; w < nworkers; w++) {
    buf_append(&out, pieces[w].data, pieces[w].len);
    free(pieces[w].data);
  }
  free(pieces);
  free(acc);
  return out;
}

int main(int argc, char **argv) {
  const char *path = "25000_in";
  if (argc > 1) path = argv[1];
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "regex-redux: cannot open %s\n", path);
    return 1;
  }
  Buf input = {0};
  char chunk[65536];
  size_t nread;
  while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0)
    buf_append(&input, chunk, nread);
  fclose(fp);
  size_t original_len = input.len;

  Buf seq = {0};
  clean_seq(input.data, input.len, &seq);
  size_t seq_len = seq.len;
  free(input.data);

  long nworkers = num_workers();

  static const char *variants[9] = {
      "agggtaaa|tttaccct",
      "[cgt]gggtaaa|tttaccc[acg]",
      "a[act]ggtaaa|tttacc[agt]t",
      "ag[act]gtaaa|tttac[agt]ct",
      "agg[act]taaa|ttta[agt]cct",
      "aggg[acg]aaa|ttt[cgt]ccct",
      "agggt[cgt]aa|tt[acg]accct",
      "agggta[cgt]a|t[acg]taccct",
      "agggtaa[cgt]|[acg]ttaccct",
  };

  LitTable table;
  memset(&table, 0, sizeof(table));
  for (int v = 0; v < 9; v++) {
    for (int k = 0; k < 6; k++) {
      const char *lit = variant_literals[v][k];
      if (!lit) break;
      lit_put(&table, pack8(lit), (uint16_t)(1u << v));
    }
  }

  // Variant counts, chunked over the workers.
  HitList *hlists = calloc((size_t)nworkers, sizeof(HitList));
  CountArg *cargs = malloc((size_t)nworkers * sizeof(CountArg));
  void **cpargs = malloc((size_t)nworkers * sizeof(void *));
  for (long w = 0; w < nworkers; w++) {
    size_t s, e;
    chunk_range(seq.len, w, nworkers, &s, &e);
    cargs[w] = (CountArg){seq.data, seq.len, s, e, &table, &hlists[w]};
    cpargs[w] = &cargs[w];
  }
  run_workers(nworkers, count_worker, cpargs);
  long counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  size_t next[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (long w = 0; w < nworkers; w++) {
    for (size_t k = 0; k < hlists[w].len; k++) {
      size_t pos = hlists[w].hits[k].pos;
      uint16_t m = hlists[w].hits[k].mask;
      for (int v = 0; v < 9; v++) {
        if ((m & (uint16_t)(1u << v)) != 0 && pos >= next[v]) {
          counts[v]++;
          next[v] = pos + 8;
        }
      }
    }
    free(hlists[w].hits);
  }
  free(hlists);
  free(cargs);
  free(cpargs);
  for (int i = 0; i < 9; i++) printf("%s %ld\n", variants[i], counts[i]);

  // Substitutions, one pass at a time, each parallelized.
  Buf cur = seq;
  for (int which = 0; which < 5; which++) {
    Buf nxt = subst_parallel(&cur, which, nworkers);
    free(cur.data);
    cur = nxt;
  }
  printf("\n%zu\n%zu\n%zu\n", original_len, seq_len, cur.len);
  free(cur.data);
  return 0;
}

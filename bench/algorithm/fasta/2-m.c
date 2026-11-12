// Fasta, parallel (-m) version of bench/algorithm/fasta/1.c.
//
// NumCPU-worker port: the whole output is built in one preallocated
// buffer, split into line-aligned chunks (multiples of 60 chars, so
// each chunk is a whole number of lines, plus the section's final
// partial line), one chunk per worker per section. Each worker writes
// only its own byte ranges, so the assembled output is in order by
// construction and no locking is needed; main writes the buffer once
// after joining all workers.
//
// The LCG is sequential by definition, so a worker starting mid-stream
// cannot just resume the global state: its starting state is computed
// by fast-forwarding the affine map x -> (IA*x + IC) % IM with binary
// exponentiation (exact integer arithmetic, all products < IM^2), which
// reproduces the serial draw sequence bit-for-bit. The repeat section
// is positional (char j is alu[j % slen]), so its chunks need no RNG
// state at all.
//
// Thread count is the number of online CPUs. A sysconf failure or a
// pthread_create failure is reported on stderr and exits nonzero
// before anything is printed.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#define WIDTH 60
#define IM 139968
#define IA 3877
#define IC 29573

typedef struct {
  double p;
  char c;
} AminoAcid;

static void accumulate(AminoAcid *g, int len) {
  for (int i = 1; i < len; i++) g[i].p += g[i - 1].p;
  for (int i = 0; i < len; i++) g[i].p *= IM;
}

/* LCG state after `steps` draws starting from `seed`: the one-step map
   x -> (IA*x + IC) % IM is affine, and affine maps compose, so the
   steps-th iterate is built by binary exponentiation. */
static uint32_t lcg_jump(uint32_t seed, uint64_t steps) {
  uint64_t ra = 1, rb = 0; /* result map x -> ra*x + rb */
  uint64_t ba = IA, bb = IC; /* base map: one LCG step */
  while (steps > 0) {
    if (steps & 1) {
      /* res = base after res */
      uint64_t na = (ba * ra) % IM;
      uint64_t nb = (ba * rb + bb) % IM;
      ra = na;
      rb = nb;
    }
    /* base = base after base */
    uint64_t na = (ba * ba) % IM;
    uint64_t nb = (ba * bb + bb) % IM;
    ba = na;
    bb = nb;
    steps >>= 1;
  }
  return (uint32_t)((ra * seed + rb) % IM);
}

/* One line-aligned chunk of one section. */
typedef struct {
  char *dst; /* first output byte of this chunk */
  long start; /* first char index within the section */
  long len; /* chars in this chunk (0 = empty) */
  uint32_t seed; /* LCG state before the first char (random only) */
  const AminoAcid *g; /* random only */
  int glen;
  const char *s; /* repeat only */
  int slen;
  int is_random;
} Chunk;

static void fill_chunk(Chunk *c) {
  if (c->len <= 0) return;
  char *p = c->dst;
  if (!c->is_random) {
    for (long k = 0; k < c->len; k++) {
      p[0] = c->s[(c->start + k) % c->slen];
      p++;
      if ((k + 1) % WIDTH == 0) *p++ = '\n';
    }
    if (c->len % WIDTH != 0) *p++ = '\n';
    return;
  }
  uint32_t last = c->seed;
  for (long k = 0; k < c->len; k++) {
    last = (last * IA + IC) % IM;
    double r = (double)last;
    char ch = c->g[c->glen - 1].c;
    for (int i = 0; i < c->glen; i++) {
      if (c->g[i].p >= r) {
        ch = c->g[i].c;
        break;
      }
    }
    *p++ = ch;
    if ((k + 1) % WIDTH == 0) *p++ = '\n';
  }
  if (c->len % WIDTH != 0) *p++ = '\n';
}

/* Line-aligned split of a section of `count` chars among `w` workers:
   worker i gets whole lines only, so its chunk is a whole number of
   lines (plus the section's final partial line for the last worker).
   start/len are char offsets within the section; out_off is the
   chunk's byte offset from the section start. */
static void partition(long count, long w, long i, long *start, long *len,
                      long *out_off) {
  long lines = (count + WIDTH - 1) / WIDTH;
  long per = lines / w;
  long rem = lines % w;
  long start_line = i * per + (i < rem ? i : rem);
  long lines_i = per + (i < rem ? 1 : 0);
  long s = start_line * WIDTH;
  if (s >= count) {
    *start = count;
    *len = 0;
    *out_off = s + start_line;
    return;
  }
  long l = lines_i * WIDTH;
  if (l > count - s) l = count - s;
  *start = s;
  *len = l;
  *out_off = s + start_line;
}

typedef struct {
  Chunk *chunks;
  int nchunks;
} WorkerArg;

static void *worker(void *p) {
  WorkerArg *a = (WorkerArg *)p;
  for (int i = 0; i < a->nchunks; i++) fill_chunk(&a->chunks[i]);
  return NULL;
}

static size_t section_bytes(long count) {
  return (size_t)count + (size_t)((count + WIDTH - 1) / WIDTH);
}

int main(int argc, char **argv) {
  int n = 1000;
  if (argc > 1) n = atoi(argv[1]);

  AminoAcid iub[] = {{0.27, 'a'}, {0.12, 'c'}, {0.12, 'g'}, {0.27, 't'},
                     {0.02, 'B'}, {0.02, 'D'}, {0.02, 'H'}, {0.02, 'K'},
                     {0.02, 'M'}, {0.02, 'N'}, {0.02, 'R'}, {0.02, 'S'},
                     {0.02, 'V'}, {0.02, 'W'}, {0.02, 'Y'}};
  AminoAcid homosapiens[] = {{0.3029549426680, 'a'},
                             {0.1979883004921, 'c'},
                             {0.1975473066391, 'g'},
                             {0.3015094502008, 't'}};
  accumulate(iub, 15);
  accumulate(homosapiens, 4);

  const char *alu =
      "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGG"
      "GAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGA"
      "CCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAAT"
      "ACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCA"
      "GCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGG"
      "AGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCC"
      "AGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA";
  const long alen = (long)strlen(alu);

  static const char h1[] = ">ONE Homo sapiens alu\n";
  static const char h2[] = ">TWO IUB ambiguity codes\n";
  static const char h3[] = ">THREE Homo sapiens frequency\n";
  const long c1 = 2L * n, c2 = 3L * n, c3 = 5L * n;
  const size_t b1 = section_bytes(c1), b2 = section_bytes(c2),
               b3 = section_bytes(c3);
  const size_t total = sizeof(h1) - 1 + b1 + sizeof(h2) - 1 + b2 +
                       sizeof(h3) - 1 + b3;

  long w = sysconf(_SC_NPROCESSORS_ONLN);
  if (w < 1) {
    fprintf(stderr, "fasta: sysconf(_SC_NPROCESSORS_ONLN) failed\n");
    return 1;
  }

  char *out = (char *)malloc(total);
  if (out == NULL) {
    fprintf(stderr, "fasta: out of memory for %zu bytes\n", total);
    return 1;
  }
  memcpy(out, h1, sizeof(h1) - 1);
  memcpy(out + sizeof(h1) - 1 + b1, h2, sizeof(h2) - 1);
  memcpy(out + sizeof(h1) - 1 + b1 + sizeof(h2) - 1 + b2, h3,
         sizeof(h3) - 1);

  /* The homosapiens section starts where the iub LCG left off. */
  uint32_t seed_homo = lcg_jump(42, (uint64_t)c2);

  Chunk *chunks = (Chunk *)malloc(sizeof(Chunk) * (size_t)(3 * w));
  WorkerArg *args =
      (WorkerArg *)malloc(sizeof(WorkerArg) * (size_t)w);
  pthread_t *threads =
      (pthread_t *)malloc(sizeof(pthread_t) * (size_t)w);
  if (chunks == NULL || args == NULL || threads == NULL) {
    fprintf(stderr, "fasta: out of memory for worker state\n");
    return 1;
  }
  const size_t o1 = sizeof(h1) - 1;
  const size_t o2 = o1 + b1 + sizeof(h2) - 1;
  const size_t o3 = o2 + b2 + sizeof(h3) - 1;
  for (long i = 0; i < w; i++) {
    long start, len, off;
    partition(c1, w, i, &start, &len, &off);
    chunks[3 * i + 0] = (Chunk){out + o1 + (size_t)off, start, len, 0,
                                NULL, 0, alu, (int)alen, 0};
    partition(c2, w, i, &start, &len, &off);
    chunks[3 * i + 1] =
        (Chunk){out + o2 + (size_t)off, start, len,
                lcg_jump(42, (uint64_t)start), iub, 15, NULL, 0, 1};
    partition(c3, w, i, &start, &len, &off);
    chunks[3 * i + 2] =
        (Chunk){out + o3 + (size_t)off, start, len,
                lcg_jump(seed_homo, (uint64_t)start), homosapiens, 4,
                NULL, 0, 1};
    args[i] = (WorkerArg){&chunks[3 * i], 3};
  }

  long spawned = 0;
  for (long i = 0; i < w; i++) {
    int rc = pthread_create(&threads[i], NULL, worker, &args[i]);
    if (rc != 0) {
      fprintf(stderr, "fasta: pthread_create failed at worker %ld: %s\n",
              i, strerror(rc));
      for (long j = 0; j < spawned; j++) pthread_join(threads[j], NULL);
      return 1;
    }
    spawned++;
  }
  for (long i = 0; i < w; i++) pthread_join(threads[i], NULL);

  if (fwrite(out, 1, total, stdout) != total) {
    fprintf(stderr, "fasta: short write to stdout\n");
    return 1;
  }
  free(out);
  free(chunks);
  free(args);
  free(threads);
  return 0;
}

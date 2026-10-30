// Mandelbrot, standalone C port of bench/algorithm/mandelbrot/1.go.
//
// NOTE: bench/algorithm/mandelbrot/1-ffi.c and 1-mffi.c are NOT
// usable in the main C config: they depend on OpenSSL (md5.h) and
// belong to bench_c_ffi. This file instead contains a small
// self-contained MD5 implementation (RFC 1321). Its round constants
// are computed from sin() at startup rather than tabulated, which
// avoids transcription errors in the 64-constant table; only the
// shift amounts and message schedule are tabulated, and those are
// short and easily checked. Only libc/libm are used.
//
// Output: "P4\n<size> <size>\n<md5 hex of the packed pixels>\n",
// exactly as in the Go version (the pixel rows are 8 pixels wide
// per byte, most-significant bit first).

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------- MD5 (RFC 1321) ----------------

typedef struct {
  uint32_t a, b, c, d;
  uint64_t len; // bytes processed so far, excluding padding
  uint8_t buf[64];
  size_t buflen;
} Md5;

static uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

static uint32_t md5_k[64];
static const int md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(Md5 *m, const uint8_t *p) {
  uint32_t w[16];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
           ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
  uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    uint32_t tmp = d;
    d = c;
    c = b;
    b = b + rol32(a + f + md5_k[i] + w[g], md5_s[i]);
    a = tmp;
  }
  m->a += a;
  m->b += b;
  m->c += c;
  m->d += d;
}

static void md5_init(Md5 *m) {
  static int k_done = 0;
  if (!k_done) {
    for (int i = 0; i < 64; i++)
      md5_k[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0);
    k_done = 1;
  }
  m->a = 0x67452301;
  m->b = 0xefcdab89;
  m->c = 0x98badcfe;
  m->d = 0x10325476;
  m->len = 0;
  m->buflen = 0;
}

static void md5_update(Md5 *m, const uint8_t *data, size_t len) {
  m->len += len;
  while (len > 0) {
    size_t take = 64 - m->buflen;
    if (take > len) take = len;
    memcpy(m->buf + m->buflen, data, take);
    m->buflen += take;
    data += take;
    len -= take;
    if (m->buflen == 64) {
      md5_block(m, m->buf);
      m->buflen = 0;
    }
  }
}

static void md5_final(Md5 *m, uint8_t out[16]) {
  uint64_t bitlen = m->len * 8;
  uint8_t pad = 0x80;
  md5_update(m, &pad, 1);
  uint8_t zero = 0;
  while (m->buflen != 56) md5_update(m, &zero, 1);
  uint8_t lb[8];
  for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bitlen >> (8 * i));
  // Append the length without counting it in m->len: feed it through
  // the block machinery directly.
  for (int i = 0; i < 8; i++) {
    m->buf[m->buflen++] = lb[i];
    if (m->buflen == 64) {
      md5_block(m, m->buf);
      m->buflen = 0;
    }
  }
  uint32_t v[4] = {m->a, m->b, m->c, m->d};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(v[i] >> (8 * j));
}

// ---------------- Mandelbrot ----------------

static void add8(double *a, double *b, double *r) {
  for (int i = 0; i < 8; i++) r[i] = a[i] + b[i];
}
static void minus8(double *a, double *b, double *r) {
  for (int i = 0; i < 8; i++) r[i] = a[i] - b[i];
}
static void mul8(double *a, double *b, double *r) {
  for (int i = 0; i < 8; i++) r[i] = a[i] * b[i];
}

// Returns the packed byte for 8 pixels, or 0 if all escaped.
static uint8_t mbrot8(double *cr, double civ) {
  double ci[8], zr[8] = {0}, zi[8] = {0}, tr[8] = {0}, ti[8] = {0},
         absz[8] = {0}, tmp[8] = {0};
  for (int i = 0; i < 8; i++) ci[i] = civ;
  for (int oi = 0; oi < 10; oi++) {
    for (int ij = 0; ij < 5; ij++) {
      add8(zr, zr, tmp);
      mul8(tmp, zi, tmp);
      add8(tmp, ci, zi);
      minus8(tr, ti, tmp);
      add8(tmp, cr, zr);
      mul8(zr, zr, tr);
      mul8(zi, zi, ti);
    }
    add8(tr, ti, absz);
    int terminate = 1;
    for (int i = 0; i < 8; i++)
      if (absz[i] <= 4.0) {
        terminate = 0;
        break;
      }
    if (terminate) return 0;
  }
  uint8_t accu = 0;
  for (int i = 0; i < 8; i++)
    if (absz[i] <= 4.0) accu |= (uint8_t)(0x80 >> i);
  return accu;
}

// FP-contract barrier: GCC's default -ffp-contract=fast fuses
// "x * inv - c" into a single FMA, which rounds differently from
// Go's strict IEEE arithmetic and flips chaotic boundary pixels
// (verified: without this, size 200 and 500 hashes differ; with it,
// they match). The multiply and subtract must stay separate
// operations, so they go through noinline calls. The app.rsp flags
// cannot be changed for one program, hence this in-source fix.
static __attribute__((noinline)) double mul_d(double a, double b) {
  return a * b;
}
static __attribute__((noinline)) double sub_d(double a, double b) {
  return a - b;
}

// Scheduler test: one POSIX thread per row (size threads for input size),
// derived from 1.c with the row loop parallelized. Each thread writes
// only its own row of the pixel buffer, so no locking is needed. On
// pthread_create failure the program reports the failing row and exits
// nonzero without printing a digest.
typedef struct {
  int row, chunk;
  double inv;
  double *xloc;
  uint8_t *pixels;
} RowArg;

static void *row_worker(void *p) {
  RowArg *a = (RowArg *)p;
  double ci = sub_d(mul_d((double)a->row, a->inv), 1.0);
  for (int i = 0; i < a->chunk; i++)
    a->pixels[(size_t)a->row * a->chunk + i] = mbrot8(&a->xloc[i * 8], ci);
  return NULL;
}

int main(int argc, char **argv) {
  int n = 200;
  if (argc > 1) n = atoi(argv[1]);
  int size = (n + 7) / 8 * 8;
  int chunk = size / 8;
  double inv = 2.0 / (double)size;

  double *xloc = malloc((size_t)chunk * 8 * sizeof(double));
  for (int i = 0; i < size; i++)
    xloc[i] = sub_d(mul_d((double)i, inv), 1.5);

  printf("P4\n%d %d\n", size, size);
  uint8_t *pixels = malloc((size_t)size * chunk);
  pthread_t *threads = malloc((size_t)size * sizeof(pthread_t));
  RowArg *args = malloc((size_t)size * sizeof(RowArg));
  for (int row = 0; row < size; row++) {
    args[row] = (RowArg){row, chunk, inv, xloc, pixels};
    int rc = pthread_create(&threads[row], NULL, row_worker, &args[row]);
    if (rc != 0) {
      fprintf(stderr, "mandelbrot: pthread_create failed at row %d: %s\n",
              row, strerror(rc));
      return 1;
    }
  }
  for (int row = 0; row < size; row++) pthread_join(threads[row], NULL);
  free(threads);
  free(args);

  Md5 m;
  md5_init(&m);
  md5_update(&m, pixels, (size_t)size * chunk);
  uint8_t digest[16];
  md5_final(&m, digest);
  for (int i = 0; i < 16; i++) printf("%02x", digest[i]);
  printf("\n");
  free(xloc);
  free(pixels);
  return 0;
}

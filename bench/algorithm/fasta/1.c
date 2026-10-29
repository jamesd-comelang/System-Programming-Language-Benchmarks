// Fasta, sequential C port of bench/algorithm/fasta/3-m.go.
// The Go version generates its random blocks under a mutex in
// order, so a sequential generator produces identical output.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 60
#define IM 139968
#define IA 3877
#define IC 29573

typedef struct {
  double p;
  char c;
} AminoAcid;

static uint32_t lastrandom = 42;

static void accumulate(AminoAcid *g, int len) {
  for (int i = 1; i < len; i++) g[i].p += g[i - 1].p;
  for (int i = 0; i < len; i++) g[i].p *= IM;
}

static void repeat_fasta(const char *s, int count) {
  int slen = (int)strlen(s);
  int pos = 0;
  while (count > 0) {
    int line = count < WIDTH ? count : WIDTH;
    for (int i = 0; i < line; i++) putchar(s[(pos + i) % slen]);
    putchar('\n');
    pos += line;
    if (pos >= slen) pos -= slen;
    count -= line;
  }
}

static void random_fasta(AminoAcid *g, int glen, int count) {
  int col = 0;
  while (count-- > 0) {
    lastrandom = (lastrandom * IA + IC) % IM;
    double r = (double)lastrandom;
    char c = g[glen - 1].c;
    for (int i = 0; i < glen; i++) {
      if (g[i].p >= r) {
        c = g[i].c;
        break;
      }
    }
    putchar(c);
    if (++col == WIDTH) {
      putchar('\n');
      col = 0;
    }
  }
  if (col != 0) putchar('\n');
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

  printf(">ONE Homo sapiens alu\n");
  repeat_fasta(alu, 2 * n);
  printf(">TWO IUB ambiguity codes\n");
  random_fasta(iub, 15, 3 * n);
  printf(">THREE Homo sapiens frequency\n");
  random_fasta(homosapiens, 4, 5 * n);
  return 0;
}

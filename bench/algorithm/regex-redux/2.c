// Regex-redux without the regex engine: a literal-scan port of
// bench/algorithm/regex-redux/4.go (which itself replaces Go 3.go's
// regexp passes). Every pattern in this problem is an alternation of
// fixed strings (or a bracket scan with a fixed shape), so each pass
// is re-implemented below as one literal scan that reproduces the
// regexp's leftmost, non-overlapping match semantics exactly:
//
//   - variant counts: all alternatives are 8-mers. One pass looks
//     each 8-mer up in a table of the 50 literals (grouped by
//     variant) and applies each variant's skip-ahead independently,
//     which is exactly what FindAll does per pattern. Summing
//     per-literal counts would NOT be equivalent: distinct literals
//     of one variant can match at overlapping positions, and
//     FindAll would skip the second after matching the first.
//   - substitutions 1-3: alternations of fixed strings of one
//     length (4, 3, 2), matched by first-byte dispatch in one scan.
//   - substitution 4 (<[^>]*>): '<' runs to the first '>'.
//   - substitution 5 (\|[^|][^|]*\|): '|' runs of >= 1 non-'|'
//     closed by a '|'; anything else is copied verbatim.
//   - clean ((>[^\n]+)?\n): a '>' line (at least one char before
//     the newline) or a lone newline is dropped; anything else,
//     including a '>' with no following newline or a '>' immediately
//     followed by a newline, is copied, matching the Go regex.
//
// Output is byte-identical to 1.c / 3.go on the test inputs.
// Stdlib-only (no regex, no GMP).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// packs to since every literal byte is nonzero ASCII).
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

static void count_variants(const char *seq, size_t len, long counts[9]) {
  LitTable table;
  memset(&table, 0, sizeof(table));
  for (int v = 0; v < 9; v++) {
    for (int k = 0; k < 6; k++) {
      const char *lit = variant_literals[v][k];
      if (!lit) break;
      lit_put(&table, pack8(lit), (uint16_t)(1u << v));
    }
  }
  size_t next[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i + 8 <= len; i++) {
    uint16_t m;
    if (!lit_get(&table, pack8(seq + i), &m)) continue;
    for (int v = 0; v < 9; v++) {
      if ((m & (uint16_t)(1u << v)) != 0 && i >= next[v]) {
        counts[v]++;
        next[v] = i + 8;
      }
    }
  }
}

// Replace-all helper shape shared by subst1-3: scan for fixed-length
// literals, copy the gaps in bulk, emit the replacement.
static void subst_fixed(const char *in, size_t len, const char *rep,
                        int lit_len, int (*match)(const char *p), Buf *out) {
  size_t start = 0;
  size_t i = 0;
  while (i < len) {
    if (i + (size_t)lit_len <= len && match(in + i)) {
      buf_append(out, in + start, i - start);
      buf_append(out, rep, strlen(rep));
      i += (size_t)lit_len;
      start = i;
    } else {
      i++;
    }
  }
  buf_append(out, in + start, len - start);
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

// '<' runs to the first '>' -> '|'; a '<' with no later '>' is copied.
static void subst4(const char *in, size_t len, Buf *out) {
  size_t start = 0;
  size_t i = 0;
  while (i < len) {
    if (in[i] == '<') {
      size_t j = i + 1;
      while (j < len && in[j] != '>') j++;
      if (j < len) {
        buf_append(out, in + start, i - start);
        buf_append(out, "|", 1);
        i = j + 1;
        start = i;
        continue;
      }
    }
    i++;
  }
  buf_append(out, in + start, len - start);
}

// '|' runs of >= 1 non-'|' closed by a '|' -> '-'; anything else copied.
static void subst5(const char *in, size_t len, Buf *out) {
  size_t start = 0;
  size_t i = 0;
  while (i < len) {
    if (in[i] == '|') {
      size_t j = i + 1;
      while (j < len && in[j] != '|') j++;
      if (j < len && j > i + 1) {
        buf_append(out, in + start, i - start);
        buf_append(out, "-", 1);
        i = j + 1;
        start = i;
        continue;
      }
    }
    i++;
  }
  buf_append(out, in + start, len - start);
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
  long counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  count_variants(seq.data, seq.len, counts);
  for (int i = 0; i < 9; i++) printf("%s %ld\n", variants[i], counts[i]);

  Buf cur = seq;
  Buf nxt = {0};
  subst_fixed(cur.data, cur.len, "<4>", 4, match_sub1, &nxt);
  free(cur.data);
  cur = nxt;
  nxt = (Buf){0};
  subst_fixed(cur.data, cur.len, "<3>", 3, match_sub2, &nxt);
  free(cur.data);
  cur = nxt;
  nxt = (Buf){0};
  subst_fixed(cur.data, cur.len, "<2>", 2, match_sub3, &nxt);
  free(cur.data);
  cur = nxt;
  nxt = (Buf){0};
  subst4(cur.data, cur.len, &nxt);
  free(cur.data);
  cur = nxt;
  nxt = (Buf){0};
  subst5(cur.data, cur.len, &nxt);
  free(cur.data);
  cur = nxt;
  printf("\n%zu\n%zu\n%zu\n", original_len, seq_len, cur.len);
  return 0;
}

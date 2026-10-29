// Regex-redux, C port of bench/algorithm/regex-redux/7-m.rs (and 3.go),
// using POSIX regex.h.
//
// The program reads the whole input file given as argv[1], strips
// header lines and newlines, counts the 9 variant patterns in the
// cleaned sequence, then applies the 5 substitutions in order and
// prints the variant counts, a blank line, and the original,
// cleaned, and final lengths.
//
// POSIX leftmost-longest matching coincides with the Rust/Go
// leftmost-first matching for these patterns: within each pattern
// the alternatives start with distinct characters (or, for the
// "<[^>]*>" and pipe patterns, greedy matching gives the same
// span). One pattern is rewritten to avoid a GNU extension: the
// Rust/Go pattern \|[^|][^|]*\| is written here as
// [|][^|][^|]*[|], since POSIX ERE treats \| as alternation.

#include <regex.h>
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

// Buffers are always NUL-terminated: regexec() works on C strings
// and would otherwise read past the end of the buffer into stale
// bytes left over from earlier, longer contents (which is what
// produced phantom matches in the substitution stages).
static void buf_append(Buf *b, const char *p, size_t n) {
  buf_reserve(b, n);
  memcpy(b->data + b->len, p, n);
  b->len += n;
  b->data[b->len] = '\0';
}

// Count non-overlapping matches of re in text[0..len).
static long count_matches(regex_t *re, const char *text, size_t len) {
  long count = 0;
  size_t off = 0;
  regmatch_t m;
  while (off <= len) {
    if (regexec(re, text + off, 1, &m, off > 0 ? REG_NOTBOL : 0) != 0)
      break;
    count++;
    size_t end = off + (size_t)m.rm_eo;
    // All patterns here match at least one byte, so this advances.
    off = end;
    if (m.rm_eo == m.rm_so) off++; // safety, unreachable for these
  }
  return count;
}

// Replace all non-overlapping matches of re in text with rep,
// appending the result to out.
static void replace_all(regex_t *re, const char *text, size_t len,
                        const char *rep, Buf *out) {
  size_t off = 0;
  regmatch_t m;
  while (off <= len) {
    if (regexec(re, text + off, 1, &m, off > 0 ? REG_NOTBOL : 0) != 0)
      break;
    buf_append(out, text + off, (size_t)m.rm_so);
    buf_append(out, rep, strlen(rep));
    size_t end = off + (size_t)m.rm_eo;
    off = end;
    if (m.rm_eo == m.rm_so) {
      // Zero-width match: copy one byte and move on (unreachable
      // for these patterns, kept for safety).
      if (off < len) buf_append(out, text + off, 1);
      off++;
    }
  }
  buf_append(out, text + off, len - off);
}

static void compile_or_die(regex_t *re, const char *pat) {
  int rc = regcomp(re, pat, REG_EXTENDED);
  if (rc != 0) {
    char err[256];
    regerror(rc, re, err, sizeof(err));
    fprintf(stderr, "regex-redux: cannot compile %s: %s\n", pat, err);
    exit(1);
  }
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

  // Clean: remove ">header\n" and every "\n".
  regex_t clean_re;
  compile_or_die(&clean_re, ">[^\n]*\n|\n");
  Buf seq = {0};
  replace_all(&clean_re, input.data, input.len, "", &seq);
  size_t seq_len = seq.len;
  regfree(&clean_re);
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
  for (int i = 0; i < 9; i++) {
    regex_t re;
    compile_or_die(&re, variants[i]);
    long c = count_matches(&re, seq.data, seq.len);
    printf("%s %ld\n", variants[i], c);
    regfree(&re);
  }

  static const char *sub_pats[5] = {
      "tHa[Nt]",
      "aND|caN|Ha[DS]|WaS",
      "a[NSt]|BY",
      "<[^>]*>",
      "[|][^|][^|]*[|]",
  };
  static const char *sub_reps[5] = {"<4>", "<3>", "<2>", "|", "-"};
  Buf cur = seq;
  Buf nxt = {0};
  for (int i = 0; i < 5; i++) {
    regex_t re;
    compile_or_die(&re, sub_pats[i]);
    nxt.len = 0;
    replace_all(&re, cur.data, cur.len, sub_reps[i], &nxt);
    regfree(&re);
    Buf tmp = cur;
    cur = nxt;
    nxt = tmp;
  }
  printf("\n%zu\n%zu\n%zu\n", original_len, seq_len, cur.len);
  return 0;
}

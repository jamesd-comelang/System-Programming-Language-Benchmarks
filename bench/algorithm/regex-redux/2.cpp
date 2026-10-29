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
// Output is byte-identical to 1.cpp / 3.go on the test inputs.
// Stdlib-only (no <regex>).

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// Clean: drop ">header\n" lines and every "\n", Go-regex semantics
// (a '>' immediately followed by a newline is kept, the newline
// itself is dropped as a lone newline on the next step).
static std::string clean_seq(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  size_t start = 0;
  size_t i = 0;
  while (i < in.size()) {
    char c = in[i];
    if (c == '>') {
      // Header line: '>' + at least one non-newline char + newline.
      size_t j = i + 1;
      while (j < in.size() && in[j] != '\n') j++;
      if (j < in.size() && j > i + 1) {
        out.append(in, start, i - start);
        i = j + 1;
        start = i;
        continue;
      }
      // No header match here ('>' at EOF, or '>' + '\n'):
      // keep '>' and let the newline be handled next round.
      i++;
    } else if (c == '\n') {
      out.append(in, start, i - start);
      i++;
      start = i;
    } else {
      i++;
    }
  }
  out.append(in, start, in.size() - start);
  return out;
}

static uint64_t pack8(const char *b) {
  return static_cast<uint64_t>(static_cast<unsigned char>(b[0])) |
         static_cast<uint64_t>(static_cast<unsigned char>(b[1])) << 8 |
         static_cast<uint64_t>(static_cast<unsigned char>(b[2])) << 16 |
         static_cast<uint64_t>(static_cast<unsigned char>(b[3])) << 24 |
         static_cast<uint64_t>(static_cast<unsigned char>(b[4])) << 32 |
         static_cast<uint64_t>(static_cast<unsigned char>(b[5])) << 40 |
         static_cast<uint64_t>(static_cast<unsigned char>(b[6])) << 48 |
         static_cast<uint64_t>(static_cast<unsigned char>(b[7])) << 56;
}

// Small open-addressing table for the 50 variant literals (128 slots,
// multiplicative hash, linear probe, key 0 = empty, which no literal
// packs to since every literal byte is nonzero ASCII).
struct LitTable {
  std::array<uint64_t, 128> keys{};
  std::array<uint16_t, 128> vals{};

  void put(uint64_t key, uint16_t v) {
    size_t h = static_cast<size_t>((key * UINT64_C(0x9E3779B97F4A7C15)) >>
                                   (64 - 7));
    for (;;) {
      if (keys[h] == 0) {
        keys[h] = key;
        vals[h] = v;
        return;
      }
      if (keys[h] == key) {
        vals[h] = static_cast<uint16_t>(vals[h] | v);
        return;
      }
      h = (h + 1) & 127;
    }
  }

  bool get(uint64_t key, uint16_t *v) const {
    size_t h = static_cast<size_t>((key * UINT64_C(0x9E3779B97F4A7C15)) >>
                                   (64 - 7));
    for (;;) {
      if (keys[h] == 0) return false;
      if (keys[h] == key) {
        *v = vals[h];
        return true;
      }
      h = (h + 1) & 127;
    }
  }
};

static const char *variant_literals[9][6] = {
    {"agggtaaa", "tttaccct", nullptr, nullptr, nullptr, nullptr},
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

static void count_variants(const std::string &seq, long counts[9]) {
  LitTable table;
  for (int v = 0; v < 9; v++) {
    for (int k = 0; k < 6; k++) {
      const char *lit = variant_literals[v][k];
      if (!lit) break;
      table.put(pack8(lit), static_cast<uint16_t>(1u << v));
    }
  }
  size_t next[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i + 8 <= seq.size(); i++) {
    uint16_t m;
    if (!table.get(pack8(seq.data() + i), &m)) continue;
    for (int v = 0; v < 9; v++) {
      if ((m & static_cast<uint16_t>(1u << v)) != 0 && i >= next[v]) {
        counts[v]++;
        next[v] = i + 8;
      }
    }
  }
}

// Replace-all helper shape shared by subst1-3: scan for fixed-length
// literals, copy the gaps in bulk, emit the replacement.
template <typename Match>
static std::string subst_fixed(const std::string &in, const char *rep,
                               size_t lit_len, Match match) {
  std::string out;
  out.reserve(in.size());
  size_t start = 0;
  size_t i = 0;
  while (i < in.size()) {
    if (i + lit_len <= in.size() && match(in.data() + i)) {
      out.append(in, start, i - start);
      out.append(rep);
      i += lit_len;
      start = i;
    } else {
      i++;
    }
  }
  out.append(in, start, in.size() - start);
  return out;
}

static bool match_sub1(const char *p) {
  return p[0] == 't' && p[1] == 'H' && p[2] == 'a' &&
         (p[3] == 'N' || p[3] == 't');
}

static bool match_sub2(const char *p) {
  switch (p[0]) {
    case 'a': return p[1] == 'N' && p[2] == 'D';
    case 'c': return p[1] == 'a' && p[2] == 'N';
    case 'H': return p[1] == 'a' && (p[2] == 'D' || p[2] == 'S');
    case 'W': return p[1] == 'a' && p[2] == 'S';
    default: return false;
  }
}

static bool match_sub3(const char *p) {
  switch (p[0]) {
    case 'a': return p[1] == 'N' || p[1] == 'S' || p[1] == 't';
    case 'B': return p[1] == 'Y';
    default: return false;
  }
}

// '<' runs to the first '>' -> '|'; a '<' with no later '>' is copied.
static std::string subst4(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  size_t start = 0;
  size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '<') {
      size_t j = i + 1;
      while (j < in.size() && in[j] != '>') j++;
      if (j < in.size()) {
        out.append(in, start, i - start);
        out.append("|");
        i = j + 1;
        start = i;
        continue;
      }
    }
    i++;
  }
  out.append(in, start, in.size() - start);
  return out;
}

// '|' runs of >= 1 non-'|' closed by a '|' -> '-'; anything else copied.
static std::string subst5(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  size_t start = 0;
  size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '|') {
      size_t j = i + 1;
      while (j < in.size() && in[j] != '|') j++;
      if (j < in.size() && j > i + 1) {
        out.append(in, start, i - start);
        out.append("-");
        i = j + 1;
        start = i;
        continue;
      }
    }
    i++;
  }
  out.append(in, start, in.size() - start);
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
  std::string input;
  char chunk[65536];
  size_t nread;
  while ((nread = fread(chunk, 1, sizeof(chunk), fp)) > 0)
    input.append(chunk, nread);
  fclose(fp);
  size_t original_len = input.size();

  std::string seq = clean_seq(input);
  size_t seq_len = seq.size();
  input.clear();
  input.shrink_to_fit();

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
  count_variants(seq, counts);
  for (int i = 0; i < 9; i++) printf("%s %ld\n", variants[i], counts[i]);

  std::string cur = seq;
  cur = subst_fixed(cur, "<4>", 4, match_sub1);
  cur = subst_fixed(cur, "<3>", 3, match_sub2);
  cur = subst_fixed(cur, "<2>", 2, match_sub3);
  cur = subst4(cur);
  cur = subst5(cur);
  printf("\n%zu\n%zu\n%zu\n", original_len, seq_len, cur.size());
  return 0;
}

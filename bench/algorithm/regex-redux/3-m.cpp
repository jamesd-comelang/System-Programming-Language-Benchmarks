// Regex-redux, parallel (-m) variant of bench/algorithm/regex-redux/2.cpp.
//
// Same literal-scan algorithm as 2.cpp (no <regex>), parallelized
// with NumCPU std::threads: one worker per hardware thread, no
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
//     slice is empty copies only the gap its span owns (spans are
//     delimited by match starts, so they tile the input exactly),
//     so concatenating the workers' outputs in order gives exactly
//     the serial output.
//
// Output is byte-identical to 2.cpp (verified against 25000_out,
// 250000_out, and the serial binary on 2500000_in). On thread-spawn
// failure the already-started threads are joined and the program
// exits nonzero without printing output; a zero hardware
// concurrency is reported the same way. No silent thread-count caps
// or fallbacks.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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
// packs to since every literal byte is nonzero ASCII). Built once
// before any worker starts; workers only read it.
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

// ---- parallel scaffolding ----

// Number of workers = hardware concurrency (no cap, no fallback;
// 0 is fatal).
static unsigned num_workers() {
  unsigned n = std::thread::hardware_concurrency();
  if (n == 0) {
    std::fprintf(stderr, "regex-redux: cannot determine hardware concurrency\n");
    std::exit(1);
  }
  return n;
}

// Chunk [s, e) of [0, len) for worker w of n (contiguous, covering).
static void chunk_range(size_t len, unsigned w, unsigned n, size_t *s,
                        size_t *e) {
  *s = static_cast<size_t>(static_cast<uint64_t>(w) * len / n);
  *e = static_cast<size_t>(static_cast<uint64_t>(w + 1) * len / n);
}

// Run fn(w) on n threads; on spawn failure join the started ones and
// exit nonzero. Returns false only via exit; kept simple on purpose.
template <typename Fn>
static void run_workers(unsigned n, Fn fn) {
  std::vector<std::thread> threads;
  threads.reserve(n);
  try {
    for (unsigned w = 0; w < n; w++) {
      threads.emplace_back([&, w]() { fn(w); });
    }
  } catch (const std::system_error &e) {
    std::fprintf(stderr, "regex-redux: thread spawn failed: %s\n", e.what());
    for (auto &t : threads) {
      if (t.joinable()) t.join();
    }
    std::exit(1);
  }
  for (auto &t : threads) t.join();
}

// ---- variant counts ----

struct Hit {
  size_t pos;
  uint16_t mask;
};

// ---- substitutions ----

struct Occ {
  size_t start, end;
};

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

// The five substitutions, in order: fixed-length literal (subst 0-2),
// '<' runs to the first '>' (subst 3), '|' runs of >= 1 non-'|'
// closed by a '|' (subst 4). Same predicates as 2.cpp.
static const char *subst_rep[5] = {"<4>", "<3>", "<2>", "|", "-"};
static const size_t subst_lit_len[5] = {4, 3, 2, 0, 0};

// Record every occurrence of substitution `which` with a start in
// [start, end). Reads may run past `end` (a fixed literal or a run
// closing in the next chunk) but never past `len`; the input is
// read-only, so this is safe. Every position is tested, including
// positions inside an occurrence the serial scan would skip -- the
// replay below discards those, which is exactly the serial rule.
static void scan_chunk(const std::string &in, size_t start, size_t end,
                       int which, std::vector<Occ> *out) {
  size_t len = in.size();
  const char *data = in.data();
  switch (which) {
    case 0:
    case 1:
    case 2: {
      size_t lit = subst_lit_len[which];
      auto match = which == 0 ? match_sub1 : which == 1 ? match_sub2 : match_sub3;
      for (size_t i = start; i < end && i + lit <= len; i++) {
        if (match(data + i)) out->push_back(Occ{i, i + lit});
      }
      break;
    }
    case 3: {
      size_t i = start;
      while (i < end) {
        if (data[i] == '<') {
          size_t j = i + 1;
          while (j < len && data[j] != '>') j++;
          if (j < len) out->push_back(Occ{i, j + 1});
        }
        i++;
      }
      break;
    }
    case 4: {
      size_t i = start;
      while (i < end) {
        if (data[i] == '|') {
          size_t j = i + 1;
          while (j < len && data[j] != '|') j++;
          if (j < len && j > i + 1) out->push_back(Occ{i, j + 1});
        }
        i++;
      }
      break;
    }
  }
}

// One substitution pass over `cur`, parallelized. On zero matches
// returns a copy of `cur`.
static std::string subst_parallel(const std::string &cur, int which,
                                  unsigned nworkers) {
  size_t len = cur.size();

  std::vector<std::vector<Occ>> lists(nworkers);
  run_workers(nworkers, [&](unsigned w) {
    size_t s, e;
    chunk_range(len, w, nworkers, &s, &e);
    scan_chunk(cur, s, e, which, &lists[w]);
  });

  // Replay in global order: accept the first occurrence at or after
  // `next`, exactly the serial leftmost, non-overlapping rule.
  size_t total = 0;
  for (unsigned w = 0; w < nworkers; w++) total += lists[w].size();
  std::vector<Occ> acc;
  acc.reserve(total);
  size_t next = 0;
  for (unsigned w = 0; w < nworkers; w++) {
    for (const Occ &o : lists[w]) {
      if (o.start >= next) {
        acc.push_back(o);
        next = o.end;
      }
    }
  }
  lists.clear();
  lists.shrink_to_fit();

  if (acc.empty()) return cur;

  // Split the accepted matches by count over the workers; each
  // worker emits its slice directly (gap copies + replacement).
  // Spans are delimited by match starts, so they tile [0, len).
  std::vector<std::string> pieces(nworkers);
  size_t nacc = acc.size();
  run_workers(nworkers, [&](unsigned w) {
    size_t lo = static_cast<size_t>(static_cast<uint64_t>(w) * nacc / nworkers);
    size_t hi =
        static_cast<size_t>(static_cast<uint64_t>(w + 1) * nacc / nworkers);
    size_t in_start = lo == 0 ? 0 : acc[lo].start;
    size_t in_end = hi >= nacc ? len : acc[hi].start;
    std::string &out = pieces[w];
    out.reserve(in_end - in_start);
    size_t prev = in_start;
    for (size_t k = lo; k < hi; k++) {
      out.append(cur, prev, acc[k].start - prev);
      out.append(subst_rep[which]);
      prev = acc[k].end;
    }
    out.append(cur, prev, in_end - prev);
  });

  std::string out;
  out.reserve(len);
  for (unsigned w = 0; w < nworkers; w++) out.append(pieces[w]);
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

  unsigned nworkers = num_workers();

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
  for (int v = 0; v < 9; v++) {
    for (int k = 0; k < 6; k++) {
      const char *lit = variant_literals[v][k];
      if (!lit) break;
      table.put(pack8(lit), static_cast<uint16_t>(1u << v));
    }
  }

  // Variant counts, chunked over the workers.
  std::vector<std::vector<Hit>> hlists(nworkers);
  run_workers(nworkers, [&](unsigned w) {
    size_t s, e;
    chunk_range(seq.size(), w, nworkers, &s, &e);
    for (size_t i = s; i < e && i + 8 <= seq.size(); i++) {
      uint16_t m;
      if (!table.get(pack8(seq.data() + i), &m)) continue;
      hlists[w].push_back(Hit{i, m});
    }
  });
  long counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  size_t next[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  for (unsigned w = 0; w < nworkers; w++) {
    for (const Hit &h : hlists[w]) {
      for (int v = 0; v < 9; v++) {
        if ((h.mask & static_cast<uint16_t>(1u << v)) != 0 &&
            h.pos >= next[v]) {
          counts[v]++;
          next[v] = h.pos + 8;
        }
      }
    }
  }
  hlists.clear();
  hlists.shrink_to_fit();
  for (int i = 0; i < 9; i++) printf("%s %ld\n", variants[i], counts[i]);

  // Substitutions, one pass at a time, each parallelized.
  std::string cur = seq;
  for (int which = 0; which < 5; which++) {
    cur = subst_parallel(cur, which, nworkers);
  }
  printf("\n%zu\n%zu\n%zu\n", original_len, seq_len, cur.size());
  return 0;
}

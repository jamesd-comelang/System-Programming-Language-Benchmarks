package main

// Regex-redux, parallel (-m) variant of bench/algorithm/regex-redux/4.go.
//
// Same literal-scan algorithm as 4.go (no regexp engine),
// parallelized with NumCPU goroutines: one worker per CPU, no
// one-goroutine-per-row. The pipeline is split into phases, each
// phase spread over the workers; the phases run in order, so the
// program uses exactly NumCPU workers at any time.
//
//   - clean: serial (one streaming pass over the input).
//   - variant counts: the input is split into NumCPU contiguous
//     chunks. Each worker scans its chunk and records every literal
//     hit (position + variant bitmask). The main goroutine then
//     replays the hits in global position order with the same
//     per-variant skip-ahead rule as the serial count (a match at p
//     is counted for variant v only if p >= next[v], then
//     next[v] = p + 8). Recording every hit first (instead of
//     counting per chunk) is what makes the chunking exact: a match
//     in one chunk can change which matches count in the next, and
//     the replay sees the hits in exactly the serial order.
//   - each substitution: the same record-then-replay shape. Each
//     worker scans its chunk and records every occurrence (start,
//     end) of that substitution's pattern -- every position where
//     the serial match predicate holds, including positions inside
//     a match the serial scan would have skipped over. The main
//     goroutine replays the occurrences in order with the serial
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
// Output is byte-identical to 4.go (verified against 25000_out,
// 250000_out, and the serial binary on 2500000_in). Worker count is
// runtime.NumCPU() with no cap and no fallback; a zero value exits
// nonzero. Goroutine creation has no failure return in Go, so there
// is no spawn-failure path to report (unlike the pthread/C++
// versions); each worker writes only its own slice, so no locking
// is needed. Under TinyGo, NumCPU() still reports the machine's
// CPU count but GOMAXPROCS is 1, so the goroutines run
// cooperatively on one thread: the variant is correct but
// effectively serial there (verified byte-identical).

import (
	"fmt"
	"os"
	"runtime"
	"sync"
)

var variantPatterns = []string{
	"agggtaaa|tttaccct",
	"[cgt]gggtaaa|tttaccc[acg]",
	"a[act]ggtaaa|tttacc[agt]t",
	"ag[act]gtaaa|tttac[agt]ct",
	"agg[act]taaa|ttta[agt]cct",
	"aggg[acg]aaa|ttt[cgt]ccct",
	"agggt[cgt]aa|tt[acg]accct",
	"agggta[cgt]a|t[acg]taccct",
	"agggtaa[cgt]|[acg]ttaccct",
}

var variantLiterals = [][]string{
	{"agggtaaa", "tttaccct"},
	{"cgggtaaa", "ggggtaaa", "tgggtaaa", "tttaccca", "tttacccc", "tttacccg"},
	{"aaggtaaa", "acggtaaa", "atggtaaa", "tttaccat", "tttaccgt", "tttacctt"},
	{"agagtaaa", "agcgtaaa", "agtgtaaa", "tttacact", "tttacgct", "tttactct"},
	{"aggataaa", "aggctaaa", "aggttaaa", "tttaacct", "tttagcct", "tttatcct"},
	{"agggaaaa", "agggcaaa", "aggggaaa", "tttcccct", "tttgccct", "ttttccct"},
	{"agggtcaa", "agggtgaa", "agggttaa", "ttaaccct", "ttcaccct", "ttgaccct"},
	{"agggtaca", "agggtaga", "agggtata", "tataccct", "tctaccct", "tgtaccct"},
	{"agggtaac", "agggtaag", "agggtaat", "attaccct", "cttaccct", "gttaccct"},
}

func pack8(b []byte) uint64 {
	return uint64(b[0]) | uint64(b[1])<<8 | uint64(b[2])<<16 | uint64(b[3])<<24 |
		uint64(b[4])<<32 | uint64(b[5])<<40 | uint64(b[6])<<48 | uint64(b[7])<<56
}

func cleanSeq(in []byte) []byte {
	// Unchanged bytes are flushed in bulk between dropped spans.
	out := make([]byte, 0, len(in))
	start := 0
	for i := 0; i < len(in); {
		c := in[i]
		if c == '>' {
			// Header line: '>' + at least one non-newline char + newline.
			j := i + 1
			for j < len(in) && in[j] != '\n' {
				j++
			}
			if j < len(in) && j > i+1 {
				out = append(out, in[start:i]...)
				i = j + 1
				start = i
				continue
			}
			// No header match here ('>' at EOF, or '>' + '\n'):
			// keep '>' and let the newline be handled next round.
			i++
		} else if c == '\n' {
			out = append(out, in[start:i]...)
			i++
			start = i
		} else {
			i++
		}
	}
	return append(out, in[start:]...)
}

// Small open-addressing table for the 50 variant literals. A Go map
// costs ~90 ns per lookup on this workload; this table (128 slots,
// multiplicative hash, linear probe, key 0 = empty, which no literal
// packs to since every literal byte is nonzero ASCII) is several
// times faster and keeps the single counting pass cheap. Built once
// before any worker starts; workers only read it.
type litTable struct {
	keys [128]uint64
	vals [128]uint16
}

func (t *litTable) put(key uint64, v uint16) {
	h := (key * 0x9E3779B97F4A7C15) >> (64 - 7)
	for {
		if t.keys[h] == 0 {
			t.keys[h] = key
			t.vals[h] = v
			return
		}
		if t.keys[h] == key {
			t.vals[h] |= v
			return
		}
		h = (h + 1) & 127
	}
}

func (t *litTable) get(key uint64) (uint16, bool) {
	h := (key * 0x9E3779B97F4A7C15) >> (64 - 7)
	for {
		if t.keys[h] == 0 {
			return 0, false
		}
		if t.keys[h] == key {
			return t.vals[h], true
		}
		h = (h + 1) & 127
	}
}

// ---- parallel scaffolding ----

// chunk returns [s, e) of [0, len) for worker w of n (contiguous,
// covering).
func chunkRange(length, w, n int) (int, int) {
	return w * length / n, (w + 1) * length / n
}

// ---- variant counts ----

type hit struct {
	pos  int
	mask uint16
}

// ---- substitutions ----

type occ struct {
	start, end int
}

// The five substitutions, in order: fixed-length literal (subst 0-2),
// '<' runs to the first '>' (subst 3), '|' runs of >= 1 non-'|'
// closed by a '|' (subst 4). Same predicates as 4.go.
var substReps = []string{"<4>", "<3>", "<2>", "|", "-"}
var substLitLens = []int{4, 3, 2, 0, 0}

func matchSub1(p []byte) bool {
	return p[0] == 't' && p[1] == 'H' && p[2] == 'a' &&
		(p[3] == 'N' || p[3] == 't')
}

func matchSub2(p []byte) bool {
	switch p[0] {
	case 'a':
		return p[1] == 'N' && p[2] == 'D'
	case 'c':
		return p[1] == 'a' && p[2] == 'N'
	case 'H':
		return p[1] == 'a' && (p[2] == 'D' || p[2] == 'S')
	case 'W':
		return p[1] == 'a' && p[2] == 'S'
	}
	return false
}

func matchSub3(p []byte) bool {
	switch p[0] {
	case 'a':
		return p[1] == 'N' || p[1] == 'S' || p[1] == 't'
	case 'B':
		return p[1] == 'Y'
	}
	return false
}

// scanChunk records every occurrence of substitution `which` with a
// start in [start, end). Reads may run past `end` (a fixed literal
// or a run closing in the next chunk) but never past len(in); the
// input is read-only, so this is safe. Every position is tested,
// including positions inside an occurrence the serial scan would
// skip -- the replay below discards those, which is exactly the
// serial rule.
func scanChunk(in []byte, start, end, which int) []occ {
	var out []occ
	switch which {
	case 0, 1, 2:
		lit := substLitLens[which]
		var match func([]byte) bool
		switch which {
		case 0:
			match = matchSub1
		case 1:
			match = matchSub2
		default:
			match = matchSub3
		}
		for i := start; i < end && i+lit <= len(in); i++ {
			if match(in[i:]) {
				out = append(out, occ{i, i + lit})
			}
		}
	case 3:
		i := start
		for i < end {
			if in[i] == '<' {
				j := i + 1
				for j < len(in) && in[j] != '>' {
					j++
				}
				if j < len(in) {
					out = append(out, occ{i, j + 1})
				}
			}
			i++
		}
	case 4:
		i := start
		for i < end {
			if in[i] == '|' {
				j := i + 1
				for j < len(in) && in[j] != '|' {
					j++
				}
				if j < len(in) && j > i+1 {
					out = append(out, occ{i, j + 1})
				}
			}
			i++
		}
	}
	return out
}

// substParallel runs one substitution pass over cur, parallelized.
// On zero matches it returns cur unchanged.
func substParallel(cur []byte, which, nworkers int) []byte {
	length := len(cur)

	lists := make([][]occ, nworkers)
	var wg sync.WaitGroup
	for w := 0; w < nworkers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			s, e := chunkRange(length, w, nworkers)
			lists[w] = scanChunk(cur, s, e, which)
		}(w)
	}
	wg.Wait()

	// Replay in global order: accept the first occurrence at or after
	// `next`, exactly the serial leftmost, non-overlapping rule.
	var acc []occ
	next := 0
	for w := 0; w < nworkers; w++ {
		for _, o := range lists[w] {
			if o.start >= next {
				acc = append(acc, o)
				next = o.end
			}
		}
	}
	if len(acc) == 0 {
		return cur
	}

	// Split the accepted matches by count over the workers; each
	// worker emits its slice directly (gap copies + replacement).
	// Spans are delimited by match starts, so they tile [0, len).
	nacc := len(acc)
	pieces := make([][]byte, nworkers)
	for w := 0; w < nworkers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			lo := w * nacc / nworkers
			hi := (w + 1) * nacc / nworkers
			inStart := 0
			if lo != 0 {
				inStart = acc[lo].start
			}
			inEnd := length
			if hi < nacc {
				inEnd = acc[hi].start
			}
			out := make([]byte, 0, inEnd-inStart)
			prev := inStart
			for k := lo; k < hi; k++ {
				out = append(out, cur[prev:acc[k].start]...)
				out = append(out, substReps[which]...)
				prev = acc[k].end
			}
			out = append(out, cur[prev:inEnd]...)
			pieces[w] = out
		}(w)
	}
	wg.Wait()

	out := make([]byte, 0, length)
	for w := 0; w < nworkers; w++ {
		out = append(out, pieces[w]...)
	}
	return out
}

func main() {
	fileName := "25000_in"
	if len(os.Args) > 1 {
		fileName = os.Args[1]
	}
	in, err := os.ReadFile(fileName)
	if err != nil {
		fmt.Fprintf(os.Stderr, "can't read input: %s\n", err)
		os.Exit(2)
	}
	originalLen := len(in)

	seq := cleanSeq(in)
	cleanedLen := len(seq)

	nworkers := runtime.NumCPU()
	if nworkers < 1 {
		fmt.Fprintf(os.Stderr, "regex-redux: cannot determine CPU count\n")
		os.Exit(2)
	}

	var table litTable
	for v, lits := range variantLiterals {
		for _, lit := range lits {
			table.put(pack8([]byte(lit)), 1<<uint(v))
		}
	}

	// Variant counts, chunked over the workers.
	hlists := make([][]hit, nworkers)
	var wg sync.WaitGroup
	for w := 0; w < nworkers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			s, e := chunkRange(len(seq), w, nworkers)
			var hits []hit
			for i := s; i < e && i+8 <= len(seq); i++ {
				if m, ok := table.get(pack8(seq[i:])); ok {
					hits = append(hits, hit{i, m})
				}
			}
			hlists[w] = hits
		}(w)
	}
	wg.Wait()
	var counts [9]int
	var next [9]int
	for w := 0; w < nworkers; w++ {
		for _, h := range hlists[w] {
			for v := 0; v < 9; v++ {
				if h.mask&(1<<uint(v)) != 0 && h.pos >= next[v] {
					counts[v]++
					next[v] = h.pos + 8
				}
			}
		}
	}
	for v, pat := range variantPatterns {
		fmt.Printf("%s %d\n", pat, counts[v])
	}

	// Substitutions, one pass at a time, each parallelized.
	seq = substParallel(seq, 0, nworkers)
	seq = substParallel(seq, 1, nworkers)
	seq = substParallel(seq, 2, nworkers)
	seq = substParallel(seq, 3, nworkers)
	seq = substParallel(seq, 4, nworkers)
	fmt.Printf("\n%d\n%d\n%d\n", originalLen, cleanedLen, len(seq))
}

package main

// Regex-redux without the regexp engine.
//
// 3.go runs 14 regexp passes over the cleaned sequence (1 clean + 9
// variant counts + 5 substitutions). Measured locally, Go's regexp
// engine costs 200-600 ms per 2.5 MB pass here: it has no literal
// prefilter for alternations, so every byte goes through NFA
// simulation. Every pattern in this problem, however, is an
// alternation of fixed strings (or a bracket scan with a fixed
// shape), so each pass is re-implemented below as one literal scan
// that reproduces the regexp's leftmost, non-overlapping match
// semantics exactly:
//
//   - variant counts: all alternatives are 8-mers. One pass looks
//     each 8-mer up in a table of the 50 literals (grouped by
//     variant) and applies each variant's skip-ahead independently,
//     which is exactly what FindAll does per pattern. Summing
//     per-literal bytes.Count calls would NOT be equivalent:
//     distinct literals of one variant can match at overlapping
//     positions (e.g. tttacccc at p and cgggtaaa at p+7), and
//     FindAll would skip the second after matching the first.
//   - substitutions 1-3: alternations of fixed strings of one
//     length (4, 3, 2), matched by first-byte dispatch in one scan.
//   - substitution 4 (<[^>]*>): '<' runs to the first '>'.
//   - substitution 5 (\|[^|][^|]*\|): '|' runs of >= 1 non-'|'
//     closed by a '|'; anything else is copied verbatim.
//   - clean ((>[^\n]+)?\n): a '>' line (at least one char before
//     the newline) or a lone newline is dropped; anything else,
//     including a '>' with no following newline or a '>' immediately
//     followed by a newline, is copied, matching the regex.
//
// Output is byte-identical to 3.go (verified against 25000_out and
// 250000_out, and against 3.go itself on 2500000_in).

import (
	"fmt"
	"os"
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
// times faster and keeps the single counting pass cheap.
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

func countVariants(seq []byte) [9]int {
	var table litTable
	for v, lits := range variantLiterals {
		for _, lit := range lits {
			table.put(pack8([]byte(lit)), 1<<uint(v))
		}
	}
	var counts [9]int
	var next [9]int
	for i := 0; i+8 <= len(seq); i++ {
		if m, ok := table.get(pack8(seq[i:])); ok {
			for v := 0; v < 9; v++ {
				if m&(1<<uint(v)) != 0 && i >= next[v] {
					counts[v]++
					next[v] = i + 8
				}
			}
		}
	}
	return counts
}

func subst1(in []byte) []byte {
	out := make([]byte, 0, len(in))
	start := 0
	for i := 0; i < len(in); {
		if i+4 <= len(in) && in[i] == 't' && in[i+1] == 'H' && in[i+2] == 'a' &&
			(in[i+3] == 'N' || in[i+3] == 't') {
			out = append(out, in[start:i]...)
			out = append(out, '<', '4', '>')
			i += 4
			start = i
		} else {
			i++
		}
	}
	return append(out, in[start:]...)
}

func subst2(in []byte) []byte {
	out := make([]byte, 0, len(in))
	start := 0
	for i := 0; i < len(in); {
		if i+3 <= len(in) {
			hit := false
			switch in[i] {
			case 'a':
				hit = in[i+1] == 'N' && in[i+2] == 'D'
			case 'c':
				hit = in[i+1] == 'a' && in[i+2] == 'N'
			case 'H':
				hit = in[i+1] == 'a' && (in[i+2] == 'D' || in[i+2] == 'S')
			case 'W':
				hit = in[i+1] == 'a' && in[i+2] == 'S'
			}
			if hit {
				out = append(out, in[start:i]...)
				out = append(out, '<', '3', '>')
				i += 3
				start = i
				continue
			}
		}
		i++
	}
	return append(out, in[start:]...)
}

func subst3(in []byte) []byte {
	out := make([]byte, 0, len(in))
	start := 0
	for i := 0; i < len(in); {
		if i+2 <= len(in) {
			hit := false
			switch in[i] {
			case 'a':
				hit = in[i+1] == 'N' || in[i+1] == 'S' || in[i+1] == 't'
			case 'B':
				hit = in[i+1] == 'Y'
			}
			if hit {
				out = append(out, in[start:i]...)
				out = append(out, '<', '2', '>')
				i += 2
				start = i
				continue
			}
		}
		i++
	}
	return append(out, in[start:]...)
}

func subst4(in []byte) []byte {
	out := make([]byte, 0, len(in))
	start := 0
	for i := 0; i < len(in); {
		if in[i] == '<' {
			j := i + 1
			for j < len(in) && in[j] != '>' {
				j++
			}
			if j < len(in) {
				out = append(out, in[start:i]...)
				out = append(out, '|')
				i = j + 1
				start = i
				continue
			}
		}
		i++
	}
	return append(out, in[start:]...)
}

func subst5(in []byte) []byte {
	out := make([]byte, 0, len(in))
	start := 0
	for i := 0; i < len(in); {
		if in[i] == '|' {
			j := i + 1
			for j < len(in) && in[j] != '|' {
				j++
			}
			if j < len(in) && j > i+1 {
				out = append(out, in[start:i]...)
				out = append(out, '-')
				i = j + 1
				start = i
				continue
			}
		}
		i++
	}
	return append(out, in[start:]...)
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

	counts := countVariants(seq)
	for v, pat := range variantPatterns {
		fmt.Printf("%s %d\n", pat, counts[v])
	}

	seq = subst1(seq)
	seq = subst2(seq)
	seq = subst3(seq)
	seq = subst4(seq)
	seq = subst5(seq)
	fmt.Printf("\n%d\n%d\n%d\n", originalLen, cleanedLen, len(seq))
}

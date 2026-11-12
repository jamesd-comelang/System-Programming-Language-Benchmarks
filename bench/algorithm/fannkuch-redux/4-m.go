/*
 * The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 *
 * contributed by Oleg Mazurov, June 2010
 * flag.Arg hack by Isaac Gouy
 * Stack allocation and atomic synchronization by Jan Pfeifer (2020)
 *
 * Parallel (-m) variant of bench/algorithm/fannkuch-redux/3.go:
 * NumCPU-worker style. Instead of 3.go's 720 recursively spawned
 * chunks with a fixed GOMAXPROCS(4), the permutation range is split
 * into runtime.NumCPU() contiguous slices, one goroutine per slice,
 * each with its own stack-allocated arrays and its own Result slot,
 * joined with a WaitGroup. No atomics and no channel are needed.
 *
 * Correctness note: the sign is +1 at even permutation indices and
 * -1 at odd ones, and the next-permutation step depends on the sign.
 * 3.go could always start each chunk with sign=true because its
 * chunk size is even, so every chunk starts at an even index. A
 * NumCPU split has no such guarantee, so each worker starts with
 * the sign for its own first index (idxMin%2 == 0) and reconstructs
 * the permutation at idxMin from the index, exactly as 3.go does.
 * The per-worker loop body is otherwise 3.go's body verbatim, so
 * output is byte-identical to 3.go.
 */

package main

import (
	"flag"
	"fmt"
	"log"
	"runtime"
	"strconv"
	"sync"
)

// Constant maximum size for N. Compile time known value allow us to allocate
// arrays in stack. It helps making it multiple of 64bits and memory aligned
// (hence 16).
const MAX_N = 16

type Result struct {
	maxFlips int
	checkSum int
}

var (
	n    = 12
	Fact []int
)

func fannkuch(idxMin, idxMax int, res *Result) {
	if idxMin >= idxMax {
		// Empty slice (only when Fact[n] < NumCPU()): contributes
		// the serial initial maxFlips and a zero checksum.
		*res = Result{1, 0}
		return
	}

	// Stack allocation.
	var p, pp, count [MAX_N]int

	// First permutation.
	for i := 0; i < n; i++ {
		p[i] = i
	}
	for i, idx := n-1, idxMin; i > 0; i-- {
		d := idx / Fact[i]
		count[i] = d
		idx = idx % Fact[i]

		copy(pp[:], p[:]) // One could copy only p[:n], but it is slower.
		for j := 0; j <= i; j++ {
			if j+d <= i {
				p[j] = pp[j+d]
			} else {
				p[j] = pp[j+d-i-1]
			}
		}
	}

	maxFlips := 1
	checkSum := 0

	// Sign at idxMin: +1 (true) for even indices, -1 (false) for
	// odd ones, matching the serial order. 3.go always started with
	// true because its even chunk size kept every chunk start even.
	for idx, sign := idxMin, idxMin%2 == 0; ; sign = !sign {

		// Count flips.
		first := p[0]
		if first != 0 {
			flips := 1
			if p[first] != 0 {
				copy(pp[:], p[:])
				p0 := first
				for {
					flips++
					for i, j := 1, p0-1; i < j; i, j = i+1, j-1 {
						pp[i], pp[j] = pp[j], pp[i]
					}
					t := pp[p0]
					pp[p0] = p0
					p0 = t
					if pp[p0] == 0 {
						break
					}
				}
			}
			if maxFlips < flips {
				maxFlips = flips
			}
			if sign {
				checkSum += flips
			} else {
				checkSum -= flips
			}
		}

		if idx++; idx == idxMax {
			break
		}

		// Next permutation.
		if sign {
			p[0], p[1] = p[1], first
		} else {
			p[1], p[2] = p[2], p[1]
			for k := 2; ; k++ {
				if count[k]++; count[k] <= k {
					break
				}
				count[k] = 0
				for j := 0; j <= k; j++ {
					p[j] = p[j+1]
				}
				p[k+1] = first
				first = p[0]
			}
		}
	}

	*res = Result{maxFlips, checkSum}
}

func printResult(n int, res int, chk int) {
	fmt.Printf("%d\nPfannkuchen(%d) = %d\n", chk, n, res)
}

func main() {
	flag.Parse()
	if flag.NArg() == 1 {
		n, _ = strconv.Atoi(flag.Arg(0))
	}
	if n > MAX_N {
		log.Fatalf("Max value accepted for N: %d", MAX_N)
	}
	nCPU := runtime.NumCPU()
	runtime.GOMAXPROCS(nCPU)

	Fact = make([]int, n+1)
	Fact[0] = 1
	for i := 1; i < len(Fact); i++ {
		Fact[i] = Fact[i-1] * i
	}

	results := make([]Result, nCPU)
	var wg sync.WaitGroup
	for i := 0; i < nCPU; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			idxMin := i * Fact[n] / nCPU
			idxMax := (i + 1) * Fact[n] / nCPU
			fannkuch(idxMin, idxMax, &results[i])
		}(i)
	}
	wg.Wait()

	res := 0
	chk := 0
	for i := 0; i < nCPU; i++ {
		if res < results[i].maxFlips {
			res = results[i].maxFlips
		}
		chk += results[i].checkSum
	}

	printResult(n, res, chk)
}

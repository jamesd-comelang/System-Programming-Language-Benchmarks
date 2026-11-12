package main

// Parallel (-m) variant of 1.go: segmented sieve with one goroutine
// per CPU (runtime.NumCPU() workers, no cap, no fallback). The base
// primes up to sqrt(n) are sieved serially (tiny); each goroutine then
// sieves one contiguous segment [lo, hi) of [0, n) into its own []bool
// buffer, marking multiples of every base prime, and counts the primes
// in its segment. Counts are summed in fixed worker order, so the total
// -- and the printed output -- is identical to 1.go.
//
// Marking starts at max(p*p, first multiple of p in the segment): every
// multiple of p below p*p already has a smaller prime factor, and p
// itself is never marked, exactly as in the serial loop (which marks
// from 2p and therefore never marks p). Every composite below n has a
// prime factor <= sqrt(n), so every composite in a segment is marked,
// and the per-segment counts add up to the serial count.
//
// TinyGo: goroutines and sync.WaitGroup compile and run there, but
// multi-goroutine runs of this sieve are non-deterministically wrong
// under TinyGo 0.43 (wrong counts at bench inputs 10 and 12, different
// on every run; workers=1 verified deterministic and correct), even
// though the segments and counts are disjoint and the code is
// race-clean under standard Go. TinyGo 0.43 also reports the host CPU
// count from runtime.NumCPU() (4 on this VM), not 1, so the worker
// count is forced to 1 when runtime.Compiler == "tinygo": under
// TinyGo this runs as a single worker -- same output, no parallelism.
// The parallel benefit is under the standard Go toolchain.

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
)

func isqrt(n int) int {
	lo, hi, r := 0, n, 0
	if n == 0 {
		hi = 1
	}
	for lo <= hi {
		mid := lo + (hi-lo)/2
		if mid != 0 && mid > n/mid {
			hi = mid - 1
		} else {
			r = mid
			lo = mid + 1
		}
	}
	return r
}

func basePrimes(lim int) []int {
	isComp := make([]bool, lim+1)
	primes := []int{}
	for i := 2; i <= lim; i++ {
		if !isComp[i] {
			primes = append(primes, i)
			if i*i <= lim {
				for j := i * i; j <= lim; j += i {
					isComp[j] = true
				}
			}
		}
	}
	return primes
}

func nsieve(n int) {
	primes := basePrimes(isqrt(n))
	workers := runtime.NumCPU()
	if runtime.Compiler == "tinygo" {
		// See the header comment: multi-goroutine runs are
		// non-deterministically wrong under TinyGo 0.43, and
		// NumCPU() reports the host count there, so force the
		// single-worker (serial-equivalent) path under TinyGo.
		workers = 1
	}
	chunk := (n + workers - 1) / workers
	if chunk == 0 {
		chunk = 1
	}
	counts := make([]int, workers)
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		lo := w * chunk
		hi := lo + chunk
		if hi > n {
			hi = n
		}
		wg.Add(1)
		go func(w, lo, hi int) {
			defer wg.Done()
			if lo >= hi {
				return
			}
			flags := make([]bool, hi-lo)
			for _, p := range primes {
				start := p * p
				first := (lo+p-1)/p*p
				if first > start {
					start = first
				}
				for j := start; j < hi; j += p {
					flags[j-lo] = true
				}
			}
			firstCounted := lo
			if firstCounted < 2 {
				firstCounted = 2
			}
			count := 0
			for i := firstCounted; i < hi; i++ {
				if !flags[i-lo] {
					count++
				}
			}
			counts[w] = count
		}(w, lo, hi)
	}
	wg.Wait()
	total := 0
	for _, c := range counts {
		total += c
	}
	fmt.Printf("Primes up to %8d %8d\n", n, total)
}

func main() {
	n := 4
	if len(os.Args) > 1 {
		n, _ = strconv.Atoi(os.Args[1])
	}
	for i := 0; i < 3; i++ {
		nsieve(10000 << (n - i))
	}
}

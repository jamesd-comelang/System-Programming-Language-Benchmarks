package main

// Mandelbrot, parallel variant of 1.go (same output: "P4" header plus
// the MD5 hex of the packed pixels).
//
// Why 1.go is slow: it processes 8 pixels at a time through [8]float64
// arrays and pointer-taking add/mul/minus helpers. Go does not
// auto-vectorize those loops the way Zig's @Vector or C/Rust LLVM
// output do, so 1.go pays scalar loop overhead per lane, and it only
// checks for all-escaped groups every 5 iterations. This variant
// keeps the exact same per-lane arithmetic (same operation order, so
// chaotic boundary pixels hash identically -- Go never fuses FP ops)
// but:
//
//   - the 8 lanes are fully unrolled scalar variables, which the
//     compiler keeps in registers instead of array slots, and
//   - the all-escaped check runs after EVERY iteration, not every
//     5th: once |z| > 2 it diverges monotonically here (|c| <=
//     sqrt(1.5^2 + 1^2) < 2), so an escaped group can never come
//     back, and the early return is exact, and
//   - rows are computed by runtime.NumCPU() goroutines pulling
//     32-row blocks from an atomic counter (block scheduling, not
//     contiguous chunks, because middle rows hold the set's
//     interior and take far longer than top/bottom rows).
//
// Single-threaded, the unrolled + per-iteration-check kernel alone is
// ~2-3x faster than 1.go locally; the goroutines add the usual
// multi-core factor on top. The SIMD gap to Zig/Rust/C cannot be
// closed in stdlib-only Go (no SIMD intrinsics), so parity with the
// fastest single-threaded SIMD versions is not expected -- this
// variant competes via parallelism instead, following the repo's
// -m parallel-variant precedent (e.g. spectral-norm/4-m.go).

import (
	"crypto/md5"
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
)

func mbrot8(cr *[8]float64, civ float64) byte {
	ci0, ci1, ci2, ci3, ci4, ci5, ci6, ci7 := civ, civ, civ, civ, civ, civ, civ, civ
	var zr0, zr1, zr2, zr3, zr4, zr5, zr6, zr7 float64
	var zi0, zi1, zi2, zi3, zi4, zi5, zi6, zi7 float64
	var tr0, tr1, tr2, tr3, tr4, tr5, tr6, tr7 float64
	var ti0, ti1, ti2, ti3, ti4, ti5, ti6, ti7 float64
	cr0, cr1, cr2, cr3, cr4, cr5, cr6, cr7 := cr[0], cr[1], cr[2], cr[3], cr[4], cr[5], cr[6], cr[7]
	for i := 0; i < 50; i++ {
		zi0 = (zr0+zr0)*zi0 + ci0
		zr0 = tr0 - ti0 + cr0
		tr0 = zr0 * zr0
		ti0 = zi0 * zi0
		zi1 = (zr1+zr1)*zi1 + ci1
		zr1 = tr1 - ti1 + cr1
		tr1 = zr1 * zr1
		ti1 = zi1 * zi1
		zi2 = (zr2+zr2)*zi2 + ci2
		zr2 = tr2 - ti2 + cr2
		tr2 = zr2 * zr2
		ti2 = zi2 * zi2
		zi3 = (zr3+zr3)*zi3 + ci3
		zr3 = tr3 - ti3 + cr3
		tr3 = zr3 * zr3
		ti3 = zi3 * zi3
		zi4 = (zr4+zr4)*zi4 + ci4
		zr4 = tr4 - ti4 + cr4
		tr4 = zr4 * zr4
		ti4 = zi4 * zi4
		zi5 = (zr5+zr5)*zi5 + ci5
		zr5 = tr5 - ti5 + cr5
		tr5 = zr5 * zr5
		ti5 = zi5 * zi5
		zi6 = (zr6+zr6)*zi6 + ci6
		zr6 = tr6 - ti6 + cr6
		tr6 = zr6 * zr6
		ti6 = zi6 * zi6
		zi7 = (zr7+zr7)*zi7 + ci7
		zr7 = tr7 - ti7 + cr7
		tr7 = zr7 * zr7
		ti7 = zi7 * zi7
		if tr0+ti0 > 4.0 && tr1+ti1 > 4.0 && tr2+ti2 > 4.0 && tr3+ti3 > 4.0 &&
			tr4+ti4 > 4.0 && tr5+ti5 > 4.0 && tr6+ti6 > 4.0 && tr7+ti7 > 4.0 {
			return 0
		}
	}
	var accu byte
	if tr0+ti0 <= 4.0 {
		accu |= 0x80
	}
	if tr1+ti1 <= 4.0 {
		accu |= 0x40
	}
	if tr2+ti2 <= 4.0 {
		accu |= 0x20
	}
	if tr3+ti3 <= 4.0 {
		accu |= 0x10
	}
	if tr4+ti4 <= 4.0 {
		accu |= 0x08
	}
	if tr5+ti5 <= 4.0 {
		accu |= 0x04
	}
	if tr6+ti6 <= 4.0 {
		accu |= 0x02
	}
	if tr7+ti7 <= 4.0 {
		accu |= 0x01
	}
	return accu
}

func main() {
	size := 200
	if len(os.Args) > 1 {
		if _size, err := strconv.Atoi(os.Args[1]); err == nil {
			size = _size
		}
	}
	size = (size + 7) / 8 * 8
	chunk_size := size / 8
	inv := 2.0 / float64(size)
	xloc := make([][8]float64, chunk_size)
	for i := 0; i < size; i++ {
		xloc[i/8][i%8] = float64(i)*inv - 1.5
	}
	fmt.Printf("P4\n%d %d\n", size, size)

	pixels := make([]byte, size*chunk_size)
	workers := runtime.NumCPU()
	if workers > size {
		workers = size
	}
	const blockRows = 32
	var next atomic.Int32
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				start := int(next.Add(blockRows) - blockRows)
				if start >= size {
					return
				}
				end := start + blockRows
				if end > size {
					end = size
				}
				for row := start; row < end; row++ {
					ci := float64(row)*inv - 1.0
					offset := row * chunk_size
					for i := 0; i < chunk_size; i++ {
						if r := mbrot8(&xloc[i], ci); r > 0 {
							pixels[offset+i] = r
						}
					}
				}
			}
		}()
	}
	wg.Wait()

	hasher := md5.New()
	hasher.Write(pixels)
	fmt.Printf("%x\n", hasher.Sum(nil))
}

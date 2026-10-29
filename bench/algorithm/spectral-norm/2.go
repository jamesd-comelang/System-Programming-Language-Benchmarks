package main

import (
	"flag"
	"fmt"
	"math"
	"strconv"
)

var n = 0

type Vec []float64

// times computes v = A * u. A(i,j) = (i+j)*(i+j+1)/2 + i + 1, evaluated
// incrementally: A(i,j+1) - A(i,j) = i+j+1, so each inner step is one
// float add for the index and one float add for the running sum, instead of
// two integer multiplies, an integer-to-float conversion and a division
// setup per element. All index values are integers below 2^53, so the
// float64 running index is exact and the results are bit-identical to the
// direct formula in 1.go.
func times(v, u Vec) {
	for i := range v {
		idx := float64(i*(i+1)/2 + i + 1)
		inc := float64(i + 1)
		a := 0.0
		for _, uj := range u {
			a += uj / idx
			idx += inc
			inc++
		}
		v[i] = a
	}
}

// timesTrans computes v = A^T * u. A(j,i) = (i+j)*(i+j+1)/2 + j + 1, so
// A(j+1,i) - A(j,i) = i+j+2.
func timesTrans(v, u Vec) {
	for i := range v {
		idx := float64(i*(i+1)/2 + 1)
		inc := float64(i + 2)
		a := 0.0
		for _, uj := range u {
			a += uj / idx
			idx += inc
			inc++
		}
		v[i] = a
	}
}

func aTimesTransp(v, u, x Vec) {
	times(x, u)
	timesTrans(v, x)
}

func main() {
	flag.Parse()
	if flag.NArg() > 0 {
		n, _ = strconv.Atoi(flag.Arg(0))
	}

	u := make(Vec, n)
	v := make(Vec, n)
	x := make(Vec, n)
	for i := range u {
		u[i] = 1
		v[i] = 1
	}
	for i := 0; i < 10; i++ {
		aTimesTransp(v, u, x)
		aTimesTransp(u, v, x)
	}
	var vBv, vv float64
	for i, vi := range v {
		vBv += u[i] * vi
		vv += vi * vi
	}
	fmt.Printf("%0.9f\n", math.Sqrt(vBv/vv))
}

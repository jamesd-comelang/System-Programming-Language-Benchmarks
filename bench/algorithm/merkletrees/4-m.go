package main

// Merkle trees, parallel (-m) variant of 3.go (same output).
//
// 3.go builds each tree for real (no closed-form shortcut: the hash
// is still computed by traversing the tree and check still walks
// it), but nodes live in one pre-sized []node slice per tree and
// children are int32 indices, not pointers or interfaces. The slice
// holds no pointers at all, so the GC has nothing to scan, and
// resetting an arena reuses its capacity across loop iterations
// instead of allocating per tree. The node pointer taken in make()
// stays valid across recursion only because the arena is pre-sized
// to 2^(depth+1) slots (a depth-d tree has 2^(depth+1)-1 nodes), so
// appends never reallocate mid-build. Real hashes are sums of 1s,
// hence >= 0, so -1 remains a safe "not computed" sentinel, as in
// 1.go/1.c.
//
// Parallelism: the per-depth loop builds many independent trees
// (iterations = 2^(maxDepth-depth+4)), so its iterations are split
// into contiguous chunks over runtime.NumCPU() goroutines. Each
// goroutine owns a private arena and returns a partial sum; main
// adds the partial sums in worker order, so the printed sums are
// byte-identical to the serial version. The single stretch tree and
// the long-lived tree stay serial.

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
)

type node struct {
	hash  int64
	value int64 // leaf value (1); unused for internal nodes
	leaf  bool
	left  int32
	right int32
}

type arena struct {
	nodes []node
	used  int
}

func (a *arena) reset(depth int) {
	// A full binary tree of depth d has 2^(d+1)-1 nodes.
	want := 1 << uint(depth+1)
	if cap(a.nodes) < want {
		a.nodes = make([]node, want)
	} else {
		a.nodes = a.nodes[:want]
	}
	a.used = 0
}

func (a *arena) make(depth int) int32 {
	id := int32(a.used)
	a.used++
	n := &a.nodes[id]
	n.hash = -1
	if depth <= 0 {
		n.leaf = true
		n.value = 1
		n.left = -1
		n.right = -1
	} else {
		n.leaf = false
		n.value = 0
		d := depth - 1
		n.left = a.make(d)
		n.right = a.make(d)
	}
	return id
}

func (a *arena) calHash(id int32) int64 {
	n := &a.nodes[id]
	if n.hash != -1 {
		return n.hash
	}
	if n.leaf {
		n.hash = n.value
	} else {
		n.hash = a.calHash(n.left) + a.calHash(n.right)
	}
	return n.hash
}

func (a *arena) check(id int32) bool {
	n := &a.nodes[id]
	if n.hash == -1 {
		return false
	}
	if n.leaf {
		return true
	}
	return a.check(n.left) && a.check(n.right)
}

const minDepth = 4

func main() {
	maxDepth := 5
	if len(os.Args) > 1 {
		if _n, err := strconv.Atoi(os.Args[1]); err == nil {
			maxDepth = _n
		}
	}

	if minDepth+2 > maxDepth {
		maxDepth = minDepth + 2
	}
	stretchDepth := maxDepth + 1

	workers := runtime.NumCPU()
	if workers < 1 {
		fmt.Fprintln(os.Stderr, "merkletrees: cannot determine CPU count")
		os.Exit(1)
	}

	var stretchArena arena
	stretchArena.reset(stretchDepth)
	stretchTree := stretchArena.make(stretchDepth)
	fmt.Printf("stretch tree of depth %d\t root hash: %d check: %t\n", stretchDepth, stretchArena.calHash(stretchTree), stretchArena.check(stretchTree))

	// The long-lived tree gets its own arena: the loop trees reuse
	// a separate arena, so they cannot clobber it.
	var longArena arena
	longArena.reset(maxDepth)
	longLivedTree := longArena.make(maxDepth)

	for depth := minDepth; depth <= maxDepth; depth += 2 {
		iterations := 1 << uint(maxDepth-depth+minDepth)
		nw := workers
		if nw > iterations {
			nw = iterations
		}
		partials := make([]int64, nw)
		base := iterations / nw
		rem := iterations % nw
		var wg sync.WaitGroup
		for w := 0; w < nw; w++ {
			// Every tree of a given depth is identical, so a worker
			// only needs its count, not its starting index.
			count := base
			if w < rem {
				count++
			}
			wg.Add(1)
			go func(w, count int) {
				defer wg.Done()
				var a arena
				var sum int64 = 0
				for i := 0; i < count; i++ {
					a.reset(depth)
					tree := a.make(depth)
					sum += a.calHash(tree)
				}
				partials[w] = sum
			}(w, count)
		}
		wg.Wait()
		var sum int64 = 0
		for _, p := range partials {
			sum += p
		}
		fmt.Printf("%d\t trees of depth %d\t root hash sum: %d\n", iterations, depth, sum)
	}
	fmt.Printf("long lived tree of depth %d\t root hash: %d check: %t\n", maxDepth, longArena.calHash(longLivedTree), longArena.check(longLivedTree))
}

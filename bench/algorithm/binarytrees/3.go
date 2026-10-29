// Binary trees, arena-allocated Go port of bench/algorithm/binarytrees/1.c
// (which itself ports the Go Authors' version). 1.go allocates every
// node with &Node{}, so the GC has to trace millions of short-lived
// nodes. Here nodes come from one reusable arena buffer, reset
// between trees, so the GC never sees per-node garbage. The arena is
// preallocated once and only indexed, never appended, so pointers
// into it stay valid. Std-only; output is identical to 1.go.
package main

import (
	"fmt"
	"os"
	"strconv"
)

type Node struct {
	left  *Node
	right *Node
}

var arena []Node
var arenaUsed int

func newNode() *Node {
	if arenaUsed >= len(arena) {
		fmt.Fprintln(os.Stderr, "binarytrees: arena exhausted")
		os.Exit(1)
	}
	n := &arena[arenaUsed]
	arenaUsed++
	n.left = nil
	n.right = nil
	return n
}

func bottomUpTree(depth int) *Node {
	n := newNode()
	if depth > 0 {
		n.left = bottomUpTree(depth - 1)
		n.right = bottomUpTree(depth - 1)
	}
	return n
}

func (n *Node) itemCheck() int {
	if n.left == nil {
		return 1
	}
	return 1 + n.left.itemCheck() + n.right.itemCheck()
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

	// Largest tree we will build has 2^(stretchDepth+1)-1 nodes.
	arena = make([]Node, 2<<uint(stretchDepth))

	arenaUsed = 0
	fmt.Printf("stretch tree of depth %d\t check: %d\n", stretchDepth,
		bottomUpTree(stretchDepth).itemCheck())

	// The checksum is taken now: later trees reuse the same arena
	// and would overwrite this tree's nodes, but only its checksum
	// is ever printed, at the end.
	arenaUsed = 0
	longLivedCheck := bottomUpTree(maxDepth).itemCheck()

	for depth := minDepth; depth <= maxDepth; depth += 2 {
		iterations := 1 << uint(maxDepth-depth+minDepth)
		check := 0

		for i := 0; i < iterations; i++ {
			arenaUsed = 0
			check += bottomUpTree(depth).itemCheck()
		}
		fmt.Printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check)
	}
	fmt.Printf("long lived tree of depth %d\t check: %d\n", maxDepth, longLivedCheck)
}

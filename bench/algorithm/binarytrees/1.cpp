// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Port of bench/algorithm/binarytrees/1.go (The Go Authors, based on
// C program by Kevin Carson). Same algorithm, same output format.
// Deviation: nodes are allocated from a per-tree bump arena instead of
// individual heap allocations (Go's GC frees trees implicitly); the
// tree shape, traversal order and counts are unchanged.

#include <cstdio>
#include <cstdlib>
#include <vector>

struct Node {
    Node* left = nullptr;
    Node* right = nullptr;
};

// Bump arena holding exactly one tree. Capacity is reserved up front
// (a tree of depth d has 2^(d+1)-1 nodes) so pointers stay stable.
struct Arena {
    std::vector<Node> pool;
    std::size_t next = 0;
    explicit Arena(int depth) { pool.resize((std::size_t(1) << (depth + 1)) - 1); }
    Node* alloc() { return &pool[next++]; }
};

Node* bottomUpTree(Arena& arena, int depth) {
    Node* n = arena.alloc();
    if (depth <= 0) {
        return n;
    }
    n->left = bottomUpTree(arena, depth - 1);
    n->right = bottomUpTree(arena, depth - 1);
    return n;
}

int itemCheck(Node* n) {
    if (n->left == nullptr) {
        return 1;
    }
    return 1 + itemCheck(n->left) + itemCheck(n->right);
}

const int minDepth = 4;

int main(int argc, char** argv) {
    int maxDepth = 5;
    if (argc > 1) {
        int parsed = std::atoi(argv[1]);
        // Go: only replaces maxDepth when Atoi succeeds. atoi returns 0
        // on garbage, which would then be clamped to minDepth+2 below,
        // matching Go's behaviour for the tested inputs.
        maxDepth = parsed;
    }

    if (minDepth + 2 > maxDepth) {
        maxDepth = minDepth + 2;
    }
    int stretchDepth = maxDepth + 1;

    int check;
    {
        Arena arena(stretchDepth);
        check = itemCheck(bottomUpTree(arena, stretchDepth));
    }
    std::printf("stretch tree of depth %d\t check: %d\n", stretchDepth, check);

    Arena longLivedArena(maxDepth);
    Node* longLivedTree = bottomUpTree(longLivedArena, maxDepth);

    for (int depth = minDepth; depth <= maxDepth; depth += 2) {
        int iterations = 1 << (maxDepth - depth + minDepth);
        check = 0;
        for (int i = 1; i <= iterations; i++) {
            Arena arena(depth);
            check += itemCheck(bottomUpTree(arena, depth));
        }
        std::printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check);
    }
    std::printf("long lived tree of depth %d\t check: %d\n", maxDepth,
                itemCheck(longLivedTree));
    return 0;
}

// Port of bench/algorithm/merkletrees/1.go. Same algorithm, same
// output format.
// Deviation: nodes are allocated from a per-tree bump arena instead of
// individual heap allocations; the tree shape, hash computation and
// check traversal are unchanged.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct Node {
    bool hasHash = false;
    std::int64_t hash = 0;
    bool isLeaf = false;
    std::int64_t value = 0;
    Node* left = nullptr;
    Node* right = nullptr;

    void calHash() {
        if (!hasHash) {
            if (isLeaf) {
                hash = value;
            } else {
                left->calHash();
                right->calHash();
                hash = left->hash + right->hash;
            }
            hasHash = true;
        }
    }

    bool check() const {
        if (!hasHash) {
            return false;
        }
        if (isLeaf) {
            return true;
        }
        return left->check() && right->check();
    }

    std::int64_t getHash() const { return hasHash ? hash : -1; }
};

// Bump arena holding exactly one tree. Capacity is reserved up front
// (a tree of depth d has 2^(d+1)-1 nodes) so pointers stay stable.
struct Arena {
    std::vector<Node> pool;
    std::size_t next = 0;
    explicit Arena(int depth) { pool.resize((std::size_t(1) << (depth + 1)) - 1); }
    Node* alloc() { return &pool[next++]; }
};

Node* makeTree(Arena& arena, int depth) {
    Node* n = arena.alloc();
    if (depth <= 0) {
        n->isLeaf = true;
        n->value = 1;
        return n;
    }
    int d = depth - 1;
    n->left = makeTree(arena, d);
    n->right = makeTree(arena, d);
    return n;
}

const int minDepth = 4;

int main(int argc, char** argv) {
    int maxDepth = 5;
    if (argc > 1) {
        maxDepth = std::atoi(argv[1]);
    }

    if (minDepth + 2 > maxDepth) {
        maxDepth = minDepth + 2;
    }
    int stretchDepth = maxDepth + 1;

    Arena stretchArena(stretchDepth);
    Node* stretchTree = makeTree(stretchArena, stretchDepth);
    stretchTree->calHash();
    std::printf("stretch tree of depth %d\t root hash: %lld check: %s\n",
                stretchDepth, (long long)stretchTree->getHash(),
                stretchTree->check() ? "true" : "false");

    Arena longLivedArena(maxDepth);
    Node* longLivedTree = makeTree(longLivedArena, maxDepth);

    for (int depth = minDepth; depth <= maxDepth; depth += 2) {
        int iterations = 1 << (maxDepth - depth + minDepth);
        std::int64_t sum = 0;
        for (int i = 1; i <= iterations; i++) {
            Arena arena(depth);
            Node* tree = makeTree(arena, depth);
            tree->calHash();
            sum += tree->getHash();
        }
        std::printf("%d\t trees of depth %d\t root hash sum: %lld\n", iterations,
                    depth, (long long)sum);
    }
    longLivedTree->calHash();
    std::printf("long lived tree of depth %d\t root hash: %lld check: %s\n",
                maxDepth, (long long)longLivedTree->getHash(),
                longLivedTree->check() ? "true" : "false");
    return 0;
}

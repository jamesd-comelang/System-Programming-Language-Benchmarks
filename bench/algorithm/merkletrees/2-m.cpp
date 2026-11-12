// Parallel (-m) variant of bench/algorithm/merkletrees/1.cpp.
// Same algorithm, same output format.
// Deviation: nodes are allocated from a per-tree bump arena instead of
// individual heap allocations; the tree shape, hash computation and
// check traversal are unchanged.
//
// Parallelism: the per-depth loop builds many independent trees
// (iterations = 2^(max_depth - depth + 4)), so its iterations are
// split into contiguous chunks over NumCPU std::threads. Each worker
// builds its trees in private per-tree arenas and returns a partial
// sum; the main thread adds the partial sums in worker order, so the
// printed sums are byte-identical to the serial version. The single
// stretch tree and the long-lived tree stay serial. On thread-spawn
// failure the already-started threads are joined and the program
// exits nonzero without printing that depth's line.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
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

    unsigned ncpu = std::thread::hardware_concurrency();
    if (ncpu == 0) {
        std::fprintf(stderr, "merkletrees: cannot determine CPU count\n");
        return 1;
    }

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
        int workers = (int)ncpu;
        if (workers > iterations) workers = iterations;
        std::vector<std::int64_t> partials((std::size_t)workers, 0);
        int base = iterations / workers;
        int rem = iterations % workers;
        std::vector<std::thread> threads;
        threads.reserve((std::size_t)workers);
        bool spawnFailed = false;
        try {
            for (int w = 0; w < workers; w++) {
                // Every tree of a given depth is identical, so a worker
                // only needs its count, not its starting index.
                int count = base + (w < rem ? 1 : 0);
                threads.emplace_back([&, w, count]() {
                    std::int64_t sum = 0;
                    for (int i = 0; i < count; i++) {
                        Arena arena(depth);
                        Node* tree = makeTree(arena, depth);
                        tree->calHash();
                        sum += tree->getHash();
                    }
                    partials[(std::size_t)w] = sum;
                });
            }
        } catch (const std::system_error& e) {
            std::fprintf(stderr, "merkletrees: thread spawn failed: %s\n",
                         e.what());
            spawnFailed = true;
        }
        for (auto& t : threads) t.join();
        if (spawnFailed) return 1;
        std::int64_t sum = 0;
        for (std::int64_t p : partials) sum += p;
        std::printf("%d\t trees of depth %d\t root hash sum: %lld\n", iterations,
                    depth, (long long)sum);
    }
    longLivedTree->calHash();
    std::printf("long lived tree of depth %d\t root hash: %lld check: %s\n",
                maxDepth, (long long)longLivedTree->getHash(),
                longLivedTree->check() ? "true" : "false");
    return 0;
}

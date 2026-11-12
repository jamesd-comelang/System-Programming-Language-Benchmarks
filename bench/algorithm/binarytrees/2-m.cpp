// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// NumCPU-worker parallel version of bench/algorithm/binarytrees/1.cpp
// (port of bench/algorithm/binarytrees/1.go). Same algorithm, same
// output format.
//
// Work is split into chunks of near-equal size and claimed by
// std::thread::hardware_concurrency() worker threads through an
// atomic counter. There are three kinds of chunk:
//
//   - the stretch tree (depth max+1, one tree),
//   - the long-lived tree (depth max, one tree),
//   - per depth row, the row's iterations split into 2*nworkers
//     chunks (fewer when a row has fewer iterations).
//
// Every row does the same total work (iterations * 2^(depth+1)
// nodes is constant across rows), so splitting each row into
// 2*nworkers chunks makes every chunk roughly the same size, and
// the atomic claim loop balances the load. Each tree is built in
// its own Arena exactly as in 1.cpp, so no state is shared between
// threads while building trees. Checksums are exact integer sums,
// so adding chunk partials in chunk order gives exactly the serial
// result; all printing is done by the main thread after the join,
// in the serial order, so output is byte-identical to 1.cpp.
//
// Thread count is the machine's hardware concurrency, capped only
// by the number of chunks (a worker with no chunk would have
// nothing to do); there is no fixed cap. If hardware_concurrency
// reports 0, or a thread fails to spawn, the program reports it
// and exits 1 instead of silently falling back to serial.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
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

enum class ChunkKind { kStretch, kLongLived, kRow };

struct Chunk {
    int depth;  // tree depth for every tree in this chunk
    int count;  // number of trees to build and check
    int row;    // depth-row index for kRow, -1 otherwise
};

int runChunk(const Chunk& c) {
    int sum = 0;
    for (int i = 0; i < c.count; i++) {
        Arena arena(c.depth);
        sum += itemCheck(bottomUpTree(arena, c.depth));
    }
    return sum;
}

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

    unsigned ncpu = std::thread::hardware_concurrency();
    if (ncpu == 0) {
        std::fprintf(stderr, "binarytrees: could not determine hardware concurrency\n");
        return 1;
    }

    int nrows = (maxDepth - minDepth) / 2 + 1;
    long perRow = 2L * (long)ncpu;  // chunks per depth row
    // Upper bound on the chunk count: 2 single-tree chunks plus
    // perRow chunks per row.
    std::vector<Chunk> chunks;
    chunks.reserve((std::size_t)(2 + nrows * perRow));
    std::vector<int> rowFirst((std::size_t)(nrows + 1), 0);

    chunks.push_back(Chunk{stretchDepth, 1, -1});
    chunks.push_back(Chunk{maxDepth, 1, -1});
    for (int r = 0; r < nrows; r++) {
        int depth = minDepth + 2 * r;
        int iterations = 1 << (maxDepth - depth + minDepth);
        rowFirst[(std::size_t)r] = (int)chunks.size();
        long nrowChunks = iterations < perRow ? iterations : perRow;
        long base = iterations / nrowChunks;
        long rem = iterations % nrowChunks;
        for (long j = 0; j < nrowChunks; j++) {
            int cnt = (int)(base + (j < rem ? 1 : 0));
            chunks.push_back(Chunk{depth, cnt, r});
        }
    }
    rowFirst[(std::size_t)nrows] = (int)chunks.size();

    std::vector<int> results(chunks.size(), 0);
    std::atomic<long> next{0};
    long nworkers = (long)ncpu < (long)chunks.size() ? (long)ncpu : (long)chunks.size();
    std::vector<std::thread> threads;
    threads.reserve((std::size_t)nworkers);
    try {
        for (long i = 0; i < nworkers; i++) {
            threads.emplace_back([&]() {
                for (;;) {
                    long idx = next.fetch_add(1);
                    if (idx >= (long)chunks.size()) break;
                    results[(std::size_t)idx] = runChunk(chunks[(std::size_t)idx]);
                }
            });
        }
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "binarytrees: thread spawn failed: %s\n", e.what());
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        return 1;
    }
    for (auto& t : threads) t.join();

    std::printf("stretch tree of depth %d\t check: %d\n", stretchDepth, results[0]);
    for (int r = 0; r < nrows; r++) {
        int depth = minDepth + 2 * r;
        int iterations = 1 << (maxDepth - depth + minDepth);
        int check = 0;
        for (int i = rowFirst[(std::size_t)r]; i < rowFirst[(std::size_t)(r + 1)]; i++) {
            check += results[(std::size_t)i];
        }
        std::printf("%d\t trees of depth %d\t check: %d\n", iterations, depth, check);
    }
    std::printf("long lived tree of depth %d\t check: %d\n", maxDepth, results[1]);
    return 0;
}

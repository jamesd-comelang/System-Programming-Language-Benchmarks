// Fasta, parallel (-m) version of bench/algorithm/fasta/1.cpp.
//
// NumCPU-worker port: the whole output is built in one preallocated
// std::string, split into line-aligned chunks (multiples of 60 chars,
// so each chunk is a whole number of lines, plus the section's final
// partial line), one chunk per worker per section. Each worker writes
// only its own byte ranges, so the assembled output is in order by
// construction and no locking is needed; main writes the string once
// after joining all workers.
//
// The LCG is sequential by definition, so a worker starting mid-stream
// cannot just resume the global state: its starting state is computed
// by fast-forwarding the affine map x -> (IA*x + IC) % IM with binary
// exponentiation (exact integer arithmetic, all products < IM^2), which
// reproduces the serial draw sequence bit-for-bit. The repeat section
// is positional (char j is alu[j % slen]), so its chunks need no RNG
// state at all. The per-character selection loop is unchanged from
// 1.cpp: first geneList entry with p >= r.
//
// Thread count is std::thread::hardware_concurrency(). A zero result
// or a thread spawn failure is reported on stderr and exits nonzero
// before anything is printed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

const int WIDTH = 60;
const std::uint32_t IM = 139968;
const std::uint32_t IA = 3877;
const std::uint32_t IC = 29573;

struct AminoAcid {
    double p;
    char c;
};

void accumulateProbabilities(std::vector<AminoAcid>& geneList) {
    for (std::size_t i = 1; i < geneList.size(); i++) {
        geneList[i].p += geneList[i - 1].p;
    }
    for (std::size_t i = 0; i < geneList.size(); i++) {
        geneList[i].p *= IM;
    }
}

// LCG state after `steps` draws starting from `seed`: the one-step map
// x -> (IA*x + IC) % IM is affine, and affine maps compose, so the
// steps-th iterate is built by binary exponentiation.
std::uint32_t lcgJump(std::uint32_t seed, std::uint64_t steps) {
    std::uint64_t ra = 1, rb = 0;  // result map x -> ra*x + rb
    std::uint64_t ba = IA, bb = IC;  // base map: one LCG step
    while (steps > 0) {
        if (steps & 1) {
            // res = base after res
            std::uint64_t na = (ba * ra) % IM;
            std::uint64_t nb = (ba * rb + bb) % IM;
            ra = na;
            rb = nb;
        }
        // base = base after base
        std::uint64_t na = (ba * ba) % IM;
        std::uint64_t nb = (ba * bb + bb) % IM;
        ba = na;
        bb = nb;
        steps >>= 1;
    }
    return (std::uint32_t)((ra * seed + rb) % IM);
}

// One line-aligned chunk of one section.
struct Chunk {
    char* dst;  // first output byte of this chunk
    long start;  // first char index within the section
    long len;  // chars in this chunk (0 = empty)
    std::uint32_t seed;  // LCG state before the first char (random only)
    const std::vector<AminoAcid>* genes;  // random only
    const std::string* repeat;  // repeat only
    bool isRandom;
};

void fillChunk(Chunk& c) {
    if (c.len <= 0) return;
    char* p = c.dst;
    if (!c.isRandom) {
        const std::string& s = *c.repeat;
        for (long k = 0; k < c.len; k++) {
            *p++ = s[(std::size_t)((c.start + k) % (long)s.size())];
            if ((k + 1) % WIDTH == 0) *p++ = '\n';
        }
        if (c.len % WIDTH != 0) *p++ = '\n';
        return;
    }
    std::uint32_t lastRandom = c.seed;
    for (long k = 0; k < c.len; k++) {
        lastRandom = (lastRandom * IA + IC) % IM;
        double r = (double)(int)lastRandom;
        for (const auto& v : *c.genes) {
            if (v.p >= r) {
                *p++ = v.c;
                break;
            }
        }
        if ((k + 1) % WIDTH == 0) *p++ = '\n';
    }
    if (c.len % WIDTH != 0) *p++ = '\n';
}

// Line-aligned split of a section of `count` chars among `w` workers:
// worker i gets whole lines only, so its chunk is a whole number of
// lines (plus the section's final partial line for the last worker).
// *start/*len are char offsets within the section; *outOff is the
// chunk's byte offset from the section start.
void partition(long count, long w, long i, long* start, long* len,
               long* outOff) {
    long lines = (count + WIDTH - 1) / WIDTH;
    long per = lines / w;
    long rem = lines % w;
    long startLine = i * per + (i < rem ? i : rem);
    long linesI = per + (i < rem ? 1 : 0);
    long s = startLine * WIDTH;
    if (s >= count) {
        *start = count;
        *len = 0;
        *outOff = s + startLine;
        return;
    }
    long l = linesI * WIDTH;
    if (l > count - s) l = count - s;
    *start = s;
    *len = l;
    *outOff = s + startLine;
}

std::size_t sectionBytes(long count) {
    return (std::size_t)count +
           (std::size_t)((count + WIDTH - 1) / WIDTH);
}

int main(int argc, char** argv) {
    int n = 1000;
    if (argc > 1) {
        n = std::atoi(argv[1]);
    }

    std::vector<AminoAcid> iub = {
        {0.27, 'a'}, {0.12, 'c'}, {0.12, 'g'}, {0.27, 't'},
        {0.02, 'B'}, {0.02, 'D'}, {0.02, 'H'}, {0.02, 'K'},
        {0.02, 'M'}, {0.02, 'N'}, {0.02, 'R'}, {0.02, 'S'},
        {0.02, 'V'}, {0.02, 'W'}, {0.02, 'Y'},
    };
    std::vector<AminoAcid> homosapiens = {
        {0.3029549426680, 'a'},
        {0.1979883004921, 'c'},
        {0.1975473066391, 'g'},
        {0.3015094502008, 't'},
    };

    accumulateProbabilities(iub);
    accumulateProbabilities(homosapiens);

    const std::string alu =
        "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGG"
        "GAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGA"
        "CCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAAT"
        "ACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCA"
        "GCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGG"
        "AGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCC"
        "AGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA";

    const std::string h1 = ">ONE Homo sapiens alu\n";
    const std::string h2 = ">TWO IUB ambiguity codes\n";
    const std::string h3 = ">THREE Homo sapiens frequency\n";
    const long c1 = 2L * n, c2 = 3L * n, c3 = 5L * n;
    const std::size_t b1 = sectionBytes(c1), b2 = sectionBytes(c2),
                      b3 = sectionBytes(c3);
    const std::size_t total =
        h1.size() + b1 + h2.size() + b2 + h3.size() + b3;

    unsigned w = std::thread::hardware_concurrency();
    if (w == 0) {
        std::fprintf(stderr,
                     "fasta: hardware_concurrency() returned 0\n");
        return 1;
    }

    std::string out(total, '\0');
    std::memcpy(out.data(), h1.data(), h1.size());
    std::memcpy(out.data() + h1.size() + b1, h2.data(), h2.size());
    std::memcpy(out.data() + h1.size() + b1 + h2.size() + b2, h3.data(),
                h3.size());

    // The homosapiens section starts where the iub LCG left off.
    std::uint32_t seedHomo = lcgJump(42, (std::uint64_t)c2);

    std::vector<Chunk> chunks((std::size_t)3 * w);
    const std::size_t o1 = h1.size();
    const std::size_t o2 = o1 + b1 + h2.size();
    const std::size_t o3 = o2 + b2 + h3.size();
    for (long i = 0; i < (long)w; i++) {
        long start, len, off;
        partition(c1, (long)w, i, &start, &len, &off);
        chunks[(std::size_t)(3 * i + 0)] =
            Chunk{out.data() + o1 + (std::size_t)off, start, len, 0,
                  nullptr, &alu, false};
        partition(c2, (long)w, i, &start, &len, &off);
        chunks[(std::size_t)(3 * i + 1)] = Chunk{
            out.data() + o2 + (std::size_t)off, start, len,
            lcgJump(42, (std::uint64_t)start), &iub, nullptr, true};
        partition(c3, (long)w, i, &start, &len, &off);
        chunks[(std::size_t)(3 * i + 2)] = Chunk{
            out.data() + o3 + (std::size_t)off, start, len,
            lcgJump(seedHomo, (std::uint64_t)start), &homosapiens,
            nullptr, true};
    }

    std::vector<std::thread> threads;
    threads.reserve(w);
    try {
        for (long i = 0; i < (long)w; i++) {
            threads.emplace_back([&, i]() {
                for (int k = 0; k < 3; k++) {
                    fillChunk(chunks[(std::size_t)(3 * i + k)]);
                }
            });
        }
    } catch (const std::system_error& e) {
        std::fprintf(stderr, "fasta: thread spawn failed: %s\n",
                     e.what());
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }
        return 1;
    }
    for (auto& t : threads) t.join();

    if (std::fwrite(out.data(), 1, out.size(), stdout) != out.size()) {
        std::fprintf(stderr, "fasta: short write to stdout\n");
        return 1;
    }
    return 0;
}

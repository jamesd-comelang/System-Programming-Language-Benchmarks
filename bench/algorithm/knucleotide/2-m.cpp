// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// C++ port of the Go (7-m.go) / Rust (8.rs) implementations in this directory:
// same algorithm, same output format. Stdlib only, C++20.
//
// Parallel (-m) variant of 1.cpp: the seven frequency/count jobs are each
// split across NumCPU workers (std::thread::hardware_concurrency()).
// Each job's scan is partitioned into contiguous chunks of k-mer end
// positions; every worker builds its own local unordered_map (seeded from
// the frame-1 elements preceding its chunk, so every k-mer is counted
// exactly once), and the partial maps are merged in fixed worker order.
// Counts are exact integers and the output sort (count desc, code desc)
// is unchanged, so the printed output is byte-identical to 1.cpp.
// No silent caps or fallbacks: if a thread cannot be spawned, the
// already-started threads are joined and the program exits nonzero.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// A = 0, C = 1, T = 2, G = 3; works for upper- and lower-case input.
inline uint8_t encode_byte(unsigned char c) {
    return static_cast<uint8_t>((c >> 1) & 3);
}

inline char decode_byte(uint8_t v) {
    static const char toChar[4] = {'A', 'C', 'T', 'G'};
    return toChar[v & 3];
}

// Read the FASTA section after the ">THREE" header line, encoded to 2-bit
// values. argv[1] is a file path (the harness passes the data file path).
std::vector<uint8_t> read_input(const char *fileName) {
    std::ifstream in(fileName, std::ios::binary);
    std::vector<uint8_t> data;
    data.reserve(65536);
    std::string line;
    bool found = false;
    while (std::getline(in, line)) {
        if (!found) {
            if (line.rfind(">THREE", 0) == 0) {
                found = true;
            }
            continue;
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        for (unsigned char c : line) {
            data.push_back(encode_byte(c));
        }
    }
    return data;
}

using Map = std::unordered_map<uint64_t, uint64_t>;

// Parallel scan: the k-mers ending at positions [frame-1, input.size())
// are split into contiguous chunks, one per worker. Each worker seeds its
// rolling code from the frame-1 elements before its chunk and counts only
// the k-mers ending inside its chunk, so every k-mer is counted exactly
// once. Partial maps are merged in fixed worker order; integer addition
// makes the merged counts exact. Throws std::system_error (after joining
// any started threads) if a thread cannot be spawned.
Map gen_freq_par(const std::vector<uint8_t> &input, size_t frame) {
    if (input.size() < frame) {
        return Map{};
    }
    unsigned workers = std::thread::hardware_concurrency();
    if (workers == 0) {
        // Parallelism unknown: one worker, i.e. the serial scan in a
        // single thread. (Not a spawn failure; nothing is hidden.)
        workers = 1;
    }
    const size_t first = frame - 1;  // first k-mer end position
    const size_t total = input.size() - first;
    const uint64_t mask = (frame >= 32) ? ~uint64_t{0} : ((uint64_t{1} << (2 * frame)) - 1);

    std::vector<Map> partial(workers);
    std::vector<std::thread> threads;
    threads.reserve(workers);
    try {
        for (unsigned w = 0; w < workers; ++w) {
            threads.emplace_back([&, w]() {
                const size_t lo = first + total * w / workers;
                const size_t hi = first + total * (w + 1) / workers;
                if (lo >= hi) {
                    return;
                }
                uint64_t code = 0;
                for (size_t i = lo - (frame - 1); i < lo; ++i) {
                    code = (code << 2) | input[i];
                }
                Map &local = partial[w];
                for (size_t i = lo; i < hi; ++i) {
                    code = ((code << 2) | input[i]) & mask;
                    ++local[code];
                }
            });
        }
    } catch (const std::system_error &e) {
        std::fprintf(stderr, "knucleotide: thread spawn failed: %s\n", e.what());
        for (auto &t : threads) {
            if (t.joinable()) t.join();
        }
        throw;
    }
    for (auto &t : threads) t.join();

    Map freq;
    for (unsigned w = 0; w < workers; ++w) {
        for (const auto &[code, count] : partial[w]) {
            freq[code] += count;
        }
    }
    return freq;
}

std::string code_to_string(uint64_t code, size_t frame) {
    std::string s(frame, ' ');
    for (size_t i = 0; i < frame; ++i) {
        s[frame - i - 1] = decode_byte(static_cast<uint8_t>(code & 3));
        code >>= 2;
    }
    return s;
}

uint64_t code_from_string(const std::string &s) {
    uint64_t code = 0;
    for (unsigned char c : s) {
        code = (code << 2) | encode_byte(c);
    }
    return code;
}

// Print the frequency table for the given frame, sorted by count
// descending (ties: code descending, matching the Rust port's reversed
// (count, code) sort), followed by a blank line.
void print_freq(const std::vector<uint8_t> &input, size_t frame) {
    Map freq = gen_freq_par(input, frame);
    std::vector<std::pair<uint64_t, uint64_t>> v(freq.begin(), freq.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first > b.first;
    });
    const float total = static_cast<float>(input.size() - frame + 1);
    for (const auto &[code, count] : v) {
        // float32 arithmetic, as in the Go/Rust ports.
        const float pct = 100.0f * static_cast<float>(count) / total;
        std::printf("%s %.3f\n", code_to_string(code, frame).c_str(),
                    static_cast<double>(pct));
    }
    std::printf("\n");
}

void print_count(const std::vector<uint8_t> &input, const std::string &pattern) {
    Map freq = gen_freq_par(input, pattern.size());
    uint64_t count = 0;
    auto it = freq.find(code_from_string(pattern));
    if (it != freq.end()) {
        count = it->second;
    }
    std::printf("%llu\t%s\n", static_cast<unsigned long long>(count),
                pattern.c_str());
}

}  // namespace

int main(int argc, char **argv) {
    const char *fileName = argc > 1 ? argv[1] : "25000_in";
    const std::vector<uint8_t> input = read_input(fileName);

    try {
        print_freq(input, 1);
        print_freq(input, 2);
        print_count(input, "GGT");
        print_count(input, "GGTA");
        print_count(input, "GGTATT");
        print_count(input, "GGTATTTTAATT");
        print_count(input, "GGTATTTTAATTTATAGT");
    } catch (const std::system_error &e) {
        std::fprintf(stderr, "knucleotide: %s\n", e.what());
        return 1;
    }
    return 0;
}

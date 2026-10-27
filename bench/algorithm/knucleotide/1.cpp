// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// C++ port of the Go (7.go) / Rust (8.rs) implementations in this directory:
// same algorithm, same output format. Stdlib only, C++20.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
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

Map gen_freq(const std::vector<uint8_t> &input, size_t frame) {
    Map freq;
    if (input.size() < frame) {
        return freq;
    }
    const uint64_t mask = (frame >= 32) ? ~uint64_t{0} : ((uint64_t{1} << (2 * frame)) - 1);
    uint64_t code = 0;
    for (size_t i = 0; i < frame - 1; ++i) {
        code = (code << 2) | input[i];
    }
    for (size_t i = frame - 1; i < input.size(); ++i) {
        code = ((code << 2) | input[i]) & mask;
        ++freq[code];
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
    Map freq = gen_freq(input, frame);
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
    Map freq = gen_freq(input, pattern.size());
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

    print_freq(input, 1);
    print_freq(input, 2);
    print_count(input, "GGT");
    print_count(input, "GGTA");
    print_count(input, "GGTATT");
    print_count(input, "GGTATTTTAATT");
    print_count(input, "GGTATTTTAATTTATAGT");
    return 0;
}

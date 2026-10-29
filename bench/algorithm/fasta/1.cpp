// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Port of bench/algorithm/fasta/3-m.go (The Go Authors, parallel
// implementation by Chris Bainbridge). Same algorithm, same output.
// Deviation: the Go version generates random blocks on several
// goroutines and reorders their output through a channel chain; the
// RNG draws and the printed order are both sequential in acquisition
// order, so this port generates the identical sequence
// single-threaded. No other change: same LCG, same cumulative
// double probabilities (times IM), same first-p-">="-r selection,
// same 60-column folding.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

const int WIDTH = 60;

struct AminoAcid {
    double p;
    char c;
};

void accumulateProbabilities(std::vector<AminoAcid>& geneList) {
    const double IM = 139968.0;
    for (std::size_t i = 1; i < geneList.size(); i++) {
        geneList[i].p += geneList[i - 1].p;
    }
    for (std::size_t i = 0; i < geneList.size(); i++) {
        geneList[i].p *= IM;
    }
}

// RepeatFasta: prints s cyclically, count chars, newline every WIDTH.
void repeatFasta(std::string& out, const std::string& s, int count) {
    std::string s2 = s + s.substr(0, WIDTH);
    std::size_t pos = 0;
    while (count > 0) {
        int line = count < WIDTH ? count : WIDTH;
        out.append(s2, pos, (std::size_t)line);
        out.push_back('\n');
        pos += (std::size_t)line;
        if (pos >= s.size()) {
            pos -= s.size();
        }
        count -= line;
    }
}

std::uint32_t lastRandom = 42;
const std::uint32_t IM = 139968;
const std::uint32_t IA = 3877;
const std::uint32_t IC = 29573;

// RandomFasta: weighted random selection, newline every WIDTH chars.
void randomFasta(std::string& out, const std::vector<AminoAcid>& geneList,
                 int count) {
    for (int i = 0; i < count; i++) {
        lastRandom = (lastRandom * IA + IC) % IM;
        double r = (double)(int)lastRandom;
        for (const auto& v : geneList) {
            if (v.p >= r) {
                out.push_back(v.c);
                break;
            }
        }
        if ((i + 1) % WIDTH == 0) {
            out.push_back('\n');
        }
    }
    if (count % WIDTH != 0) {
        out.push_back('\n');
    }
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

    std::string out;
    out.reserve((std::size_t)(10 * n) + (std::size_t)(10 * n / WIDTH) + 128);
    out += ">ONE Homo sapiens alu\n";
    repeatFasta(out, alu, 2 * n);
    out += ">TWO IUB ambiguity codes\n";
    randomFasta(out, iub, 3 * n);
    out += ">THREE Homo sapiens frequency\n";
    randomFasta(out, homosapiens, 5 * n);

    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

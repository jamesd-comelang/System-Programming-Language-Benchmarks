// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// C++ port of the Go (3.go) / Rust (6.rs, 7-m.rs) implementations in this
// directory: same patterns, same order, same output format. Stdlib only,
// C++20 (<regex>).

#include <fstream>
#include <iostream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const char *fileName) {
    std::ifstream in(fileName, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_matches(const std::string &text, const std::regex &re) {
    size_t n = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), re),
         end;
         it != end; ++it) {
        ++n;
    }
    return n;
}

}  // namespace

int main(int argc, char **argv) {
    const char *fileName = argc > 1 ? argv[1] : "25000_in";
    const std::string input = read_file(fileName);
    const size_t originalLen = input.size();

    // Strip FASTA headers and newlines, as in the Go/Rust ports.
    static const std::regex cleanRe("(>[^\n]+)?\n");
    const std::string sequence =
        std::regex_replace(input, cleanRe, std::string(""));
    const size_t cleanedLen = sequence.size();

    static const char *const variants[] = {
        "agggtaaa|tttaccct",
        "[cgt]gggtaaa|tttaccc[acg]",
        "a[act]ggtaaa|tttacc[agt]t",
        "ag[act]gtaaa|tttac[agt]ct",
        "agg[act]taaa|ttta[agt]cct",
        "aggg[acg]aaa|ttt[cgt]ccct",
        "agggt[cgt]aa|tt[acg]accct",
        "agggta[cgt]a|t[acg]taccct",
        "agggtaa[cgt]|[acg]ttaccct",
    };
    for (const char *pattern : variants) {
        const std::regex re(pattern);
        std::cout << pattern << ' ' << count_matches(sequence, re) << '\n';
    }

    static const std::pair<const char *, const char *> substs[] = {
        {"tHa[Nt]", "<4>"},
        {"aND|caN|Ha[DS]|WaS", "<3>"},
        {"a[NSt]|BY", "<2>"},
        {"<[^>]*>", "|"},
        {"\\|[^|][^|]*\\|", "-"},
    };
    std::string current = sequence;
    for (const auto &[pattern, replacement] : substs) {
        const std::regex re(pattern);
        // Replacements contain no '$', so default format handling is
        // equivalent to the literal replacement used by Go/Rust.
        current = std::regex_replace(current, re, replacement);
    }

    std::cout << '\n'
              << originalLen << '\n'
              << cleanedLen << '\n'
              << current.size() << '\n';
    return 0;
}

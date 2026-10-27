// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Port of bench/algorithm/mandelbrot/1.go. Same algorithm, same
// output: a "P4" header line, then the lowercase hex MD5 digest of
// the packed bitmap (not the bitmap itself, matching the Go program
// and the *_out files).
// Deviation: Go/Rust use a library MD5; C++ has no MD5 in the
// standard library, so MD5 (RFC 1321) is implemented below from
// scratch using only <cstdint>/<cmath>. The K constants are computed
// at startup as floor(abs(sin(i+1)) * 2^32), per the RFC definition,
// rather than transcribed.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- MD5 (RFC 1321), stdlib only ------------------------------------
namespace md5 {

inline std::uint32_t rotl(std::uint32_t x, int s) {
    return (x << s) | (x >> (32 - s));
}

const int kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

std::array<std::uint32_t, 64> kTable() {
    std::array<std::uint32_t, 64> k{};
    for (int i = 0; i < 64; i++) {
        k[i] = (std::uint32_t)std::floor(std::fabs(std::sin(i + 1)) * 4294967296.0);
    }
    return k;
}

std::string hexDigest(const std::uint8_t* data, std::size_t len) {
    static const std::array<std::uint32_t, 64> K = kTable();
    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe,
                  d0 = 0x10325476;

    // Padding: append 0x80, zeroes, then 64-bit little-endian bit length.
    std::vector<std::uint8_t> msg(data, data + len);
    std::uint64_t bitLen = (std::uint64_t)len * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0);
    }
    for (int i = 0; i < 8; i++) {
        msg.push_back((std::uint8_t)(bitLen >> (8 * i)));
    }

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            m[i] = (std::uint32_t)msg[off + 4 * i] |
                   ((std::uint32_t)msg[off + 4 * i + 1] << 8) |
                   ((std::uint32_t)msg[off + 4 * i + 2] << 16) |
                   ((std::uint32_t)msg[off + 4 * i + 3] << 24);
        }
        std::uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; i++) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            f = f + a + K[i] + m[g];
            a = d;
            d = c;
            c = b;
            b = b + rotl(f, kShift[i]);
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    const std::uint32_t words[4] = {a0, b0, c0, d0};
    char hex[33];
    static const char* digits = "0123456789abcdef";
    for (int w = 0; w < 4; w++) {
        for (int i = 0; i < 4; i++) {
            std::uint8_t byte = (std::uint8_t)(words[w] >> (8 * i));
            hex[w * 8 + i * 2] = digits[byte >> 4];
            hex[w * 8 + i * 2 + 1] = digits[byte & 0xf];
        }
    }
    hex[32] = '\0';
    return std::string(hex);
}

}  // namespace md5

// ---- Mandelbrot, ported operation-for-operation from 1.go -----------

using Vec8 = std::array<double, 8>;

inline void addVec(const Vec8& a, const Vec8& b, Vec8& r) {
    for (int i = 0; i < 8; i++) r[i] = a[i] + b[i];
}
inline void minusVec(const Vec8& a, const Vec8& b, Vec8& r) {
    for (int i = 0; i < 8; i++) r[i] = a[i] - b[i];
}
inline void mulVec(const Vec8& a, const Vec8& b, Vec8& r) {
    for (int i = 0; i < 8; i++) r[i] = a[i] * b[i];
}

// The Go/Rust sources round every multiply and add separately. GCC's
// default FP contraction under these flags fuses (zr+zr)*zi+ci and
// i*inv-1.5 into FMA instructions, which changes boundary pixels and
// therefore the digest (verified: FMA build gives dcd13031... for
// size 200, this build gives the expected b824dffc...). app.rsp is
// fixed, so contraction is disabled here for this translation unit's
// remaining functions (mbrot8 and main); the MD5 code above is
// integer-only and unaffected.
#pragma GCC optimize ("fp-contract=off")
std::uint8_t mbrot8(const Vec8& cr, double civ) {
    Vec8 ci{};
    ci.fill(civ);
    Vec8 zr{}, zi{}, tr{}, ti{}, absz{}, tmp{};
    for (int iter = 0; iter < 10; iter++) {
        for (int j = 0; j < 5; j++) {
            addVec(zr, zr, tmp);
            mulVec(tmp, zi, tmp);
            addVec(tmp, ci, zi);

            minusVec(tr, ti, tmp);
            addVec(tmp, cr, zr);

            mulVec(zr, zr, tr);
            mulVec(zi, zi, ti);
        }
        addVec(tr, ti, absz);
        bool terminate = true;
        for (int i = 0; i < 8; i++) {
            if (absz[i] <= 4.0) {
                terminate = false;
                break;
            }
        }
        if (terminate) {
            return 0;
        }
    }
    std::uint8_t accu = 0;
    for (int i = 0; i < 8; i++) {
        if (absz[i] <= 4.0) {
            accu = (std::uint8_t)(accu | (std::uint8_t)(0x80 >> i));
        }
    }
    return accu;
}

int main(int argc, char** argv) {
    int size = 200;
    if (argc > 1) {
        size = std::atoi(argv[1]);
    }
    size = (size + 7) / 8 * 8;
    int chunkSize = size / 8;
    double inv = 2.0 / (double)size;

    std::vector<Vec8> xloc((std::size_t)chunkSize);
    for (int i = 0; i < size; i++) {
        xloc[(std::size_t)(i / 8)][i % 8] = (double)i * inv - 1.5;
    }
    std::printf("P4\n%d %d\n", size, size);

    std::vector<std::uint8_t> pixels((std::size_t)size * (std::size_t)chunkSize, 0);
    for (int chunkId = 0; chunkId < size; chunkId++) {
        double ci = (double)chunkId * inv - 1.0;
        std::size_t offset = (std::size_t)chunkId * (std::size_t)chunkSize;
        for (int i = 0; i < chunkSize; i++) {
            std::uint8_t r = mbrot8(xloc[(std::size_t)i], ci);
            if (r > 0) {
                pixels[offset + (std::size_t)i] = r;
            }
        }
    }

    std::printf("%s\n", md5::hexDigest(pixels.data(), pixels.size()).c_str());
    return 0;
}

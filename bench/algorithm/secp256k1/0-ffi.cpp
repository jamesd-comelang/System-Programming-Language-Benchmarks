// secp256k1 point multiplication via libsecp256k1 (vendored in
// bench/include/libsecp256k1/), the same library Go's 0-ffi.go uses
// through its cgo bindings. C++ port of 0-ffi.c.
//
// Semantics match 1.cpp exactly: start at the generator G, n times
// (n = argv[1], default 1) replace the point by point * PRIVATE_KEY,
// then print "<x hex>,<y hex>" with minimal lowercase hex (no leading
// zeros, exactly like Rust's {:x} formatting).
//
// With the library this is n sequential secp256k1_ec_pubkey_tweak_mul
// calls — NOT one multiplication by key^n; the work benched is the
// same repeated scalar multiplication the 1-series performs.
//
// Note: the library's public header already has extern "C" guards, so
// it is included directly. The library sources themselves are compiled
// as C (see bench_cpp_libsecp.yaml's before_build step), never as C++.

#include <array>
#include <cstdio>
#include <cstdlib>

#include <secp256k1.h>

namespace {

const std::array<unsigned char, 32> kPrivateKey = {
    45, 238, 146, 112, 121, 40, 60, 60, 79, 202, 62, 249, 112, 255, 77, 56,
    182, 69, 146, 227, 254, 10, 176, 218, 217, 19, 45, 112, 181, 188, 118, 147};

// Print a 32-byte big-endian value as minimal lowercase hex.
void PrintMinHex(const unsigned char *b) {
  int i = 0;
  while (i < 31 && b[i] == 0) i++;
  std::printf("%x", b[i]);
  for (i++; i < 32; i++) std::printf("%02x", b[i]);
}

}  // namespace

int main(int argc, char **argv) {
  long n = argc > 1 ? std::atol(argv[1]) : 1;
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  if (!ctx) return 1;

  // Start at the generator: 1 * G.
  static const std::array<unsigned char, 32> kOne = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_create(ctx, &pub, kOne.data())) return 1;

  for (long i = 0; i < n; i++) {
    if (!secp256k1_ec_pubkey_tweak_mul(ctx, &pub, kPrivateKey.data()))
      return 1;
  }

  unsigned char out[65];
  size_t outlen = sizeof(out);
  if (!secp256k1_ec_pubkey_serialize(ctx, out, &outlen, &pub,
                                     SECP256K1_EC_UNCOMPRESSED))
    return 1;
  // out[0] == 0x04, out[1..32] == x, out[33..64] == y.
  PrintMinHex(out + 1);
  std::printf(",");
  PrintMinHex(out + 33);
  std::printf("\n");

  secp256k1_context_destroy(ctx);
  return 0;
}

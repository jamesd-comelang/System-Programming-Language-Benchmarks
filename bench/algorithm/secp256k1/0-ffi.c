// secp256k1 point multiplication via libsecp256k1 (vendored in
// bench/include/libsecp256k1/), the same library Go's 0-ffi.go uses
// through its cgo bindings.
//
// Semantics match 1.c exactly: start at the generator G, n times
// (n = argv[1], default 1) replace the point by point * PRIVATE_KEY,
// then print "<x hex>,<y hex>" with minimal lowercase hex (no leading
// zeros, exactly like Rust's {:x} formatting).
//
// With the library this is n sequential secp256k1_ec_pubkey_tweak_mul
// calls — NOT one multiplication by key^n; the work benched is the
// same repeated scalar multiplication the 1-series performs.

#include <stdio.h>
#include <stdlib.h>

#include <secp256k1.h>

static const unsigned char PRIVATE_KEY[32] = {
    45, 238, 146, 112, 121, 40, 60, 60, 79, 202, 62, 249, 112, 255, 77, 56,
    182, 69, 146, 227, 254, 10, 176, 218, 217, 19, 45, 112, 181, 188, 118, 147};

// Print a 32-byte big-endian value as minimal lowercase hex.
static void print_min_hex(const unsigned char *b) {
  int i = 0;
  while (i < 31 && b[i] == 0) i++;
  printf("%x", b[i]);
  for (i++; i < 32; i++) printf("%02x", b[i]);
}

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 1;
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  if (!ctx) return 1;

  // Start at the generator: 1 * G.
  static const unsigned char ONE[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                        0, 0, 0, 0, 0, 0, 0, 1};
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_create(ctx, &pub, ONE)) return 1;

  for (long i = 0; i < n; i++) {
    if (!secp256k1_ec_pubkey_tweak_mul(ctx, &pub, PRIVATE_KEY)) return 1;
  }

  unsigned char out[65];
  size_t outlen = sizeof(out);
  if (!secp256k1_ec_pubkey_serialize(ctx, out, &outlen, &pub,
                                     SECP256K1_EC_UNCOMPRESSED))
    return 1;
  // out[0] == 0x04, out[1..32] == x, out[33..64] == y.
  print_min_hex(out + 1);
  printf(",");
  print_min_hex(out + 33);
  printf("\n");

  secp256k1_context_destroy(ctx);
  return 0;
}

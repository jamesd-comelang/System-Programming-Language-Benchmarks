// secp256k1 point multiplication via the rust-bitcoin `secp256k1` crate
// (version 0.31, which bundles the C libsecp256k1 library and builds it
// with cc) — the same library Go's 0-ffi.go uses through its cgo
// bindings. The crate dependency lives in the dedicated
// bench/include/rust-libsecp/Cargo.toml, not the shared Rust one.
//
// Semantics match 1.rs exactly: start at the generator G, n times
// (n = argv[1], default 1) replace the point by point * PRIVATE_KEY,
// then print "<x hex>,<y hex>" with minimal lowercase hex (no leading
// zeros, exactly like Rust's {:x} formatting).
//
// With the library this is n sequential mul_tweak calls — NOT one
// multiplication by key^n; the work benched is the same repeated
// scalar multiplication the 1-series performs.

use secp256k1::{PublicKey, Scalar, Secp256k1, SecretKey};

const PRIVATE_KEY: [u8; 32] = [
    45, 238, 146, 112, 121, 40, 60, 60, 79, 202, 62, 249, 112, 255, 77, 56,
    182, 69, 146, 227, 254, 10, 176, 218, 217, 19, 45, 112, 181, 188, 118, 147,
];

// Print a 32-byte big-endian value as minimal lowercase hex.
fn print_min_hex(b: &[u8]) {
    let mut i = 0;
    while i < 31 && b[i] == 0 {
        i += 1;
    }
    print!("{:x}", b[i]);
    for k in (i + 1)..32 {
        print!("{:02x}", b[k]);
    }
}

fn main() {
    let n: usize = std::env::args_os()
        .nth(1)
        .and_then(|s| s.into_string().ok())
        .and_then(|s| s.parse().ok())
        .unwrap_or(1);
    let secp = Secp256k1::new();
    // Start at the generator: 1 * G.
    let mut one = [0u8; 32];
    one[31] = 1;
    let mut point =
        PublicKey::from_secret_key(&secp, &SecretKey::from_byte_array(one).unwrap());
    let tweak = Scalar::from_be_bytes(PRIVATE_KEY).unwrap();
    for _ in 0..n {
        point = point.mul_tweak(&secp, &tweak).unwrap();
    }
    let out = point.serialize_uncompressed();
    // out[0] == 0x04, out[1..33] == x, out[33..65] == y.
    print_min_hex(&out[1..33]);
    print!(",");
    print_min_hex(&out[33..65]);
    println!();
}

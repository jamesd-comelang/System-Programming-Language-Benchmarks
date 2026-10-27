// secp256k1, ported from 1.rs (which is itself a port of 1.ts).
//
// Curve: y^2 = x^3 + 7 over F_p, p = 2^256 - 2^32 - 977, generator and
// private key exactly as in 1.rs / 0-ffi.go. Point multiplication uses
// the same Jacobian formulas, the same endomorphism split
// (split_scalar_endo with A1/B1/A2 and BETA), and the same loop:
//   point = generator; repeat n times: point = point * PRIVATE_KEY
// Output: "<x hex>,<y hex>\n" with minimal (unpadded) lowercase hex,
// matching Rust's {:x} for IBig.
//
// Implementation notes (deviations from 1.rs, all output-identical):
// * Field arithmetic is done modulo p throughout (Fp, 4x64-bit
//   limbs), instead of carrying unreduced IBig products and calling
//   rem() at the exact points 1.rs does. Every rem() in 1.rs is a
//   reduction mod p, so reducing earlier changes no result; the
//   Jacobian points stored are already reduced in 1.rs as well.
// * Modular inversion (to_affine) uses Fermat's little theorem,
//   z^(p-2) mod p, instead of 1.rs's extended-GCD invert(). For
//   z != 0 mod p (prime p) both give the unique inverse; z == 0 is
//   special-cased to 0, matching 1.rs's invert(0) == 0, hence the
//   point at infinity maps to (0,0) as in 1.rs.
// * Scalar arithmetic for split_scalar_endo (mod the group order n,
//   div_nearest) uses a small signed big-integer over limb vectors;
//   the formulas, constants and comparisons are exactly 1.rs's.
// Stdlib only, C++20.

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

// ------------------------------------------------------- limb vectors
// Little-endian, trimmed (no trailing zero limbs; zero == empty).

using Vec = std::vector<uint64_t>;

void trim(Vec& v) {
  while (!v.empty() && v.back() == 0) v.pop_back();
}
int cmp_vec(const Vec& a, const Vec& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}
Vec add_vec(const Vec& a, const Vec& b) {
  Vec r;
  r.resize((a.size() > b.size() ? a.size() : b.size()) + 1, 0);
  unsigned __int128 carry = 0;
  for (size_t i = 0; i < r.size(); i++) {
    unsigned __int128 cur = carry;
    if (i < a.size()) cur += a[i];
    if (i < b.size()) cur += b[i];
    r[i] = static_cast<uint64_t>(cur);
    carry = cur >> 64;
  }
  trim(r);
  return r;
}
// Requires a >= b.
Vec sub_vec(const Vec& a, const Vec& b) {
  Vec r = a;
  uint64_t borrow = 0;
  for (size_t i = 0; i < r.size(); i++) {
    uint64_t ai = r[i];
    uint64_t bi = i < b.size() ? b[i] : 0;
    uint64_t tmp = ai - borrow;
    uint64_t next_borrow =
        (ai < borrow || tmp < bi) ? 1 : 0;
    r[i] = tmp - bi;
    borrow = next_borrow;
  }
  trim(r);
  return r;
}
Vec mul_vec(const Vec& a, const Vec& b) {
  if (a.empty() || b.empty()) return {};
  Vec r(a.size() + b.size(), 0);
  for (size_t i = 0; i < a.size(); i++) {
    uint64_t carry = 0;
    for (size_t j = 0; j < b.size(); j++) {
      unsigned __int128 cur = static_cast<unsigned __int128>(a[i]) * b[j] +
                              r[i + j] + carry;
      r[i + j] = static_cast<uint64_t>(cur);
      carry = static_cast<uint64_t>(cur >> 64);
    }
    r[i + b.size()] = carry;
  }
  trim(r);
  return r;
}
Vec mul_small(const Vec& a, uint64_t m) {
  if (m == 0 || a.empty()) return {};
  Vec r(a.size() + 1, 0);
  unsigned __int128 carry = 0;
  for (size_t i = 0; i < a.size(); i++) {
    unsigned __int128 cur = static_cast<unsigned __int128>(a[i]) * m + carry;
    r[i] = static_cast<uint64_t>(cur);
    carry = cur >> 64;
  }
  r[a.size()] = static_cast<uint64_t>(carry);
  trim(r);
  return r;
}
Vec add_small(const Vec& a, uint64_t v) {
  Vec r = a;
  if (r.empty()) r.push_back(0);
  unsigned __int128 cur = static_cast<unsigned __int128>(r[0]) + v;
  r[0] = static_cast<uint64_t>(cur);
  uint64_t carry = static_cast<uint64_t>(cur >> 64);
  size_t i = 1;
  while (carry && i < r.size()) {
    cur = static_cast<unsigned __int128>(r[i]) + carry;
    r[i] = static_cast<uint64_t>(cur);
    carry = static_cast<uint64_t>(cur >> 64);
    i++;
  }
  if (carry) r.push_back(carry);
  trim(r);
  return r;
}
int bitlen_vec(const Vec& a) {
  if (a.empty()) return 0;
  return static_cast<int>((a.size() - 1) * 64 + (64 - __builtin_clzll(a.back())));
}
Vec shl_bits(const Vec& a, int shift) {
  if (a.empty() || shift == 0) return a;
  int limb_shift = shift / 64, bit_shift = shift % 64;
  Vec r(a.size() + limb_shift + 1, 0);
  for (size_t i = 0; i < a.size(); i++) {
    unsigned __int128 cur = static_cast<unsigned __int128>(a[i]) << bit_shift;
    r[i + limb_shift] |= static_cast<uint64_t>(cur);
    r[i + limb_shift + 1] |= static_cast<uint64_t>(cur >> 64);
  }
  trim(r);
  return r;
}
Vec shr1_vec(const Vec& a) {
  if (a.empty()) return {};
  Vec r(a.size(), 0);
  for (size_t i = 0; i < a.size(); i++) {
    r[i] = a[i] >> 1;
    if (i + 1 < a.size()) r[i] |= a[i + 1] << 63;
  }
  trim(r);
  return r;
}
// Binary long division. Returns quotient; remainder in `rem`.
Vec divmod_vec(const Vec& a, const Vec& b, Vec& rem) {
  if (b.empty()) {
    std::cerr << "division by zero\n";
    std::exit(1);
  }
  if (cmp_vec(a, b) < 0) {
    rem = a;
    return {};
  }
  int shift = bitlen_vec(a) - bitlen_vec(b);
  Vec cur_b = shl_bits(b, shift);
  Vec cur_a = a;
  Vec q((static_cast<size_t>(shift) / 64) + 1, 0);
  for (int i = shift; i >= 0; i--) {
    if (cmp_vec(cur_b, cur_a) <= 0) {
      cur_a = sub_vec(cur_a, cur_b);
      q[static_cast<size_t>(i) / 64] |= (1ull << (i % 64));
    }
    cur_b = shr1_vec(cur_b);
  }
  trim(q);
  rem = cur_a;
  return q;
}
Vec mod_vec(const Vec& a, const Vec& b) {
  Vec r;
  (void)divmod_vec(a, b, r);
  return r;
}
Vec parse_dec(const std::string& s) {
  Vec v;
  for (char c : s) {
    if (c < '0' || c > '9') continue;
    v = add_small(mul_small(v, 10), static_cast<uint64_t>(c - '0'));
  }
  return v;
}
Vec parse_hex(const std::string& s) {
  Vec v;
  for (char c : s) {
    uint64_t d;
    if (c >= '0' && c <= '9')
      d = static_cast<uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = static_cast<uint64_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = static_cast<uint64_t>(c - 'A' + 10);
    else
      continue;
    v = add_small(mul_small(v, 16), d);
  }
  return v;
}

// Signed big integer (sign + magnitude), for split_scalar_endo.
struct SInt {
  bool neg = false;
  Vec mag;
  static SInt from_vec(const Vec& v) {
    SInt s;
    s.mag = v;
    trim(s.mag);
    return s;
  }
  static SInt from_neg_vec(const Vec& v) {
    SInt s = from_vec(v);
    if (!s.mag.empty()) s.neg = true;
    return s;
  }
  bool is_zero() const { return mag.empty(); }
  SInt negated() const {
    SInt s = *this;
    if (!s.is_zero()) s.neg = !s.neg;
    return s;
  }
};
SInt add_sint(const SInt& a, const SInt& b) {
  if (a.is_zero()) return b;
  if (b.is_zero()) return a;
  if (a.neg == b.neg) {
    SInt r;
    r.neg = a.neg;
    r.mag = add_vec(a.mag, b.mag);
    return r;
  }
  int c = cmp_vec(a.mag, b.mag);
  if (c == 0) return SInt{};
  SInt r;
  if (c > 0) {
    r.neg = a.neg;
    r.mag = sub_vec(a.mag, b.mag);
  } else {
    r.neg = b.neg;
    r.mag = sub_vec(b.mag, a.mag);
  }
  return r;
}
SInt sub_sint(const SInt& a, const SInt& b) { return add_sint(a, b.negated()); }
SInt mul_sint(const SInt& a, const SInt& b) {
  if (a.is_zero() || b.is_zero()) return SInt{};
  SInt r;
  r.neg = a.neg != b.neg;
  r.mag = mul_vec(a.mag, b.mag);
  return r;
}

// ------------------------------------------------------- field mod p
// p = 2^256 - 2^32 - 977. Fp values are always in [0, p).

struct Fp {
  std::array<uint64_t, 4> v{};
  bool is_zero() const {
    return v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0;
  }
};

Fp P_CONST;  // filled by init_constants()

Fp fp_from_vec(const Vec& a) {
  Fp r;
  for (size_t i = 0; i < 4 && i < a.size(); i++) r.v[i] = a[i];
  return r;
}
int cmp_fp(const Fp& a, const Fp& b) {
  for (int i = 3; i >= 0; i--)
    if (a.v[i] != b.v[i]) return a.v[i] < b.v[i] ? -1 : 1;
  return 0;
}
Fp fp_add(const Fp& a, const Fp& b) {
  std::array<uint64_t, 4> r{};
  unsigned __int128 carry = 0;
  for (int i = 0; i < 4; i++) {
    unsigned __int128 cur =
        static_cast<unsigned __int128>(a.v[i]) + b.v[i] + carry;
    r[i] = static_cast<uint64_t>(cur);
    carry = cur >> 64;
  }
  Fp out;
  out.v = r;
  if (carry) {
    // sum = 2^256 + r; sum - p = r + (2^256 - p) = r + (2^32 + 977).
    unsigned __int128 cur =
        static_cast<unsigned __int128>(r[0]) + (1ull << 32) + 977;
    out.v[0] = static_cast<uint64_t>(cur);
    carry = cur >> 64;
    for (int i = 1; i < 4 && carry; i++) {
      cur = static_cast<unsigned __int128>(r[i]) + carry;
      out.v[i] = static_cast<uint64_t>(cur);
      carry = cur >> 64;
    }
    return out;
  }
  if (cmp_fp(out, P_CONST) >= 0) {
    // out -= p
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
      uint64_t subt = P_CONST.v[i] + borrow;
      borrow = (out.v[i] < subt) ? 1 : 0;
      out.v[i] = out.v[i] - subt;
    }
  }
  return out;
}
Fp fp_sub(const Fp& a, const Fp& b) {
  if (cmp_fp(a, b) >= 0) {
    Fp out;
    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
      uint64_t subt = b.v[i] + borrow;
      borrow = (a.v[i] < subt) ? 1 : 0;
      out.v[i] = a.v[i] - subt;
    }
    return out;
  }
  // a - b + p = a + (p - b)
  Fp out;
  std::array<uint64_t, 4> pb{};
  uint64_t borrow = 0;
  for (int i = 0; i < 4; i++) {
    uint64_t subt = b.v[i] + borrow;
    borrow = (P_CONST.v[i] < subt) ? 1 : 0;
    pb[i] = P_CONST.v[i] - subt;
  }
  unsigned __int128 carry = 0;
  for (int i = 0; i < 4; i++) {
    unsigned __int128 cur =
        static_cast<unsigned __int128>(a.v[i]) + pb[i] + carry;
    out.v[i] = static_cast<uint64_t>(cur);
    carry = cur >> 64;
  }
  return out;
}
// Reduce an 8-limb product modulo p, using 2^256 == 2^32 + 977 (mod p).
Fp fp_reduce8(const std::array<uint64_t, 8>& prod) {
  // hi = prod[4..7], lo = prod[0..3]; t = lo + hi*C, C = 2^32+977.
  // hi*C = (hi << 32) + hi*977, as 5 limbs.
  std::array<uint64_t, 5> hc{};
  {
    // hi << 32
    std::array<uint64_t, 5> sh{};
    for (int i = 0; i < 4; i++) {
      sh[i] |= prod[4 + i] << 32;
      sh[i + 1] |= prod[4 + i] >> 32;
    }
    // hi * 977
    std::array<uint64_t, 5> mul{};
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
      unsigned __int128 cur =
          static_cast<unsigned __int128>(prod[4 + i]) * 977 + carry;
      mul[i] = static_cast<uint64_t>(cur);
      carry = cur >> 64;
    }
    mul[4] = static_cast<uint64_t>(carry);
    unsigned __int128 c2 = 0;
    for (int i = 0; i < 5; i++) {
      unsigned __int128 cur =
          static_cast<unsigned __int128>(sh[i]) + mul[i] + c2;
      hc[i] = static_cast<uint64_t>(cur);
      c2 = cur >> 64;
    }
  }
  // t = lo + hc (5 limbs)
  std::array<uint64_t, 5> t{};
  {
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
      unsigned __int128 cur =
          static_cast<unsigned __int128>(prod[i]) + hc[i] + carry;
      t[i] = static_cast<uint64_t>(cur);
      carry = cur >> 64;
    }
    unsigned __int128 cur = static_cast<unsigned __int128>(hc[4]) + carry;
    t[4] = static_cast<uint64_t>(cur);
  }
  // Fold once more: s = t_lo + t_hi*C, t_hi = t[4] (< 2^34).
  // t_hi*C fits in 2 limbs.
  unsigned __int128 thc = static_cast<unsigned __int128>(t[4]) * ((1ull << 32) + 977);
  std::array<uint64_t, 5> s{};
  {
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
      unsigned __int128 add =
          (i == 0) ? static_cast<unsigned __int128>(static_cast<uint64_t>(thc))
                   : (i == 1) ? static_cast<unsigned __int128>(
                                    static_cast<uint64_t>(thc >> 64))
                              : 0;
      unsigned __int128 cur =
          static_cast<unsigned __int128>(t[i]) + add + carry;
      s[i] = static_cast<uint64_t>(cur);
      carry = cur >> 64;
    }
    s[4] = static_cast<uint64_t>(carry);
  }
  // At most one subtraction of p: s < 2^256 + 2^65 < 2p.
  bool ge = (s[4] != 0) ||
            (cmp_fp(Fp{s[0], s[1], s[2], s[3]}, P_CONST) >= 0);
  Fp out;
  if (!ge) {
    out.v = {s[0], s[1], s[2], s[3]};
    return out;
  }
  // s - p, handling a possible 2^256 place (s[4] == 1).
  uint64_t borrow = 0;
  for (int i = 0; i < 4; i++) {
    uint64_t subt = P_CONST.v[i] + borrow;
    borrow = (s[i] < subt) ? 1 : 0;
    out.v[i] = s[i] - subt;
  }
  if (s[4] != 0) {
    // s = 2^256 + low; s - p = low + (2^256 - p) = low + C.
    // The limb loop above computed (low - p) mod 2^256 with borrow 1
    // out of the top limb; adding back 2^256 gives low - p + 2^256 =
    // low + C, which is exactly out + C after the borrow? Recompute
    // directly: result = low + C.
    unsigned __int128 carry = 0;
    for (int i = 0; i < 4; i++) {
      unsigned __int128 add = (i == 0) ? ((1ull << 32) + 977) : 0;
      unsigned __int128 cur =
          static_cast<unsigned __int128>(s[i]) + add + carry;
      out.v[i] = static_cast<uint64_t>(cur);
      carry = cur >> 64;
    }
  }
  return out;
}
Fp fp_mul(const Fp& a, const Fp& b) {
  std::array<uint64_t, 8> prod{};
  for (int i = 0; i < 4; i++) {
    uint64_t carry = 0;
    for (int j = 0; j < 4; j++) {
      unsigned __int128 cur = static_cast<unsigned __int128>(a.v[i]) * b.v[j] +
                              prod[i + j] + carry;
      prod[i + j] = static_cast<uint64_t>(cur);
      carry = static_cast<uint64_t>(cur >> 64);
    }
    prod[i + 4] = carry;
  }
  return fp_reduce8(prod);
}
Fp fp_pow(const Fp& base, const Fp& exp) {
  Fp result;
  result.v = {1, 0, 0, 0};
  Fp b = base;
  for (int i = 0; i < 256; i++) {
    if ((exp.v[i / 64] >> (i % 64)) & 1u) result = fp_mul(result, b);
    if (i != 255) b = fp_mul(b, b);
  }
  return result;
}
std::string fp_hex(const Fp& a) {
  if (a.is_zero()) return "0";
  int top = 3;
  while (top > 0 && a.v[top] == 0) top--;
  char buf[32];
  std::string out;
  std::snprintf(buf, sizeof(buf), "%llx",
                static_cast<unsigned long long>(a.v[top]));
  out += buf;
  for (int i = top - 1; i >= 0; i--) {
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(a.v[i]));
    out += buf;
  }
  return out;
}

// ------------------------------------------------------- Jacobian points

struct Point {
  Fp x, y;
};
struct Jac {
  Fp x, y, z;
  static Jac zero() {
    Jac j;
    j.x.v = {0, 0, 0, 0};
    j.y.v = {1, 0, 0, 0};
    j.z.v = {0, 0, 0, 0};
    return j;
  }
  static Jac from_affine(const Point& p) {
    Jac j;
    j.x = p.x;
    j.y = p.y;
    j.z.v = {1, 0, 0, 0};
    return j;
  }
  Point to_affine() const {
    if (z.is_zero()) return Point{Fp{}, Fp{}};
    Fp inv_z = fp_pow(z, fp_sub(P_CONST, fp_from_vec(Vec{2})));
    Fp inv_z2 = fp_mul(inv_z, inv_z);
    Point out;
    out.x = fp_mul(x, inv_z2);
    out.y = fp_mul(fp_mul(y, inv_z), inv_z2);
    return out;
  }
  Jac negate() const {
    Jac j;
    j.x = x;
    j.y = fp_sub(Fp{}, y);
    j.z = z;
    return j;
  }
  Jac double_pt() const {
    Fp a = fp_mul(x, x);
    Fp b = fp_mul(y, y);
    Fp c = fp_mul(b, b);
    Fp t = fp_mul(fp_add(x, b), fp_add(x, b));
    t = fp_sub(fp_sub(t, a), c);
    Fp d = fp_add(t, t);
    Fp e = fp_add(fp_add(a, a), a);
    Fp f = fp_mul(e, e);
    Fp x3 = fp_sub(fp_sub(f, d), d);
    Fp c8 = fp_add(fp_add(fp_add(c, c), fp_add(c, c)),
                   fp_add(fp_add(c, c), fp_add(c, c)));
    Fp y3 = fp_sub(fp_mul(e, fp_sub(d, x3)), c8);
    Fp z3 = fp_add(fp_mul(y, z), fp_mul(y, z));
    return Jac{x3, y3, z3};
  }
  Jac add_pt(const Jac& o) const {
    if (o.x.is_zero() || o.y.is_zero()) return *this;
    if (x.is_zero() || y.is_zero()) return o;
    Fp z1z1 = fp_mul(z, z);
    Fp z2z2 = fp_mul(o.z, o.z);
    Fp u1 = fp_mul(x, z2z2);
    Fp u2 = fp_mul(o.x, z1z1);
    Fp s1 = fp_mul(fp_mul(y, o.z), z2z2);
    Fp s2 = fp_mul(fp_mul(o.y, z), z1z1);
    Fp h = fp_sub(u2, u1);
    Fp r = fp_sub(s2, s1);
    if (h.is_zero()) {
      if (r.is_zero()) return double_pt();
      return zero();
    }
    Fp hh = fp_mul(h, h);
    Fp hhh = fp_mul(h, hh);
    Fp v = fp_mul(u1, hh);
    Fp x3 = fp_sub(fp_sub(fp_mul(r, r), hhh), fp_add(v, v));
    Fp y3 = fp_sub(fp_mul(r, fp_sub(v, x3)), fp_mul(s1, hhh));
    Fp z3 = fp_mul(fp_mul(z, o.z), h);
    return Jac{x3, y3, z3};
  }
};

// ------------------------------------------------------- scalar split

Vec N_VEC;       // group order
Vec PRIV_VEC;    // private key
Vec A1_VEC, A2_VEC, B1MAG_VEC;
Fp BETA_FP;
Fp GX_FP, GY_FP;

Vec div_nearest_vec(const Vec& a, const Vec& b) {
  // (a + b/2) / b, for positive a, b (1.rs div_nearest).
  Vec half = shr1_vec(b);
  Vec num = add_vec(a, half);
  Vec rem;
  return divmod_vec(num, b, rem);
}
Vec rem_n_sint(const SInt& s) {
  if (s.is_zero()) return {};
  Vec r = mod_vec(s.mag, N_VEC);
  if (!s.neg) return r;
  if (r.empty()) return {};
  return sub_vec(N_VEC, r);
}
struct Split {
  bool k1neg = false, k2neg = false;
  std::array<uint64_t, 4> k1{};
  std::array<uint64_t, 4> k2{};
};
std::array<uint64_t, 4> vec_to4(const Vec& v) {
  std::array<uint64_t, 4> r{};
  for (size_t i = 0; i < 4 && i < v.size(); i++) r[i] = v[i];
  return r;
}
Split split_scalar_endo(const Vec& k) {
  // 1.rs: b2 = a1; c1 = div_nearest(b2*k, n);
  // c2 = div_nearest(-b1*k, n); k1 = rem_n(k - c1*a1 - c2*a2);
  // k2 = rem_n(-c1*b1 - c2*b2); negate each if > 2^128.
  SInt ks = SInt::from_vec(k);
  SInt c1 = SInt::from_vec(div_nearest_vec(mul_vec(A1_VEC, k), N_VEC));
  SInt c2 = SInt::from_vec(div_nearest_vec(mul_vec(B1MAG_VEC, k), N_VEC));
  SInt k1s = sub_sint(
      sub_sint(ks, mul_sint(c1, SInt::from_vec(A1_VEC))),
      mul_sint(c2, SInt::from_vec(A2_VEC)));
  // -c1*b1 - c2*b2, with b1 = -B1MAG: (-c1)*b1 = c1*B1MAG.
  SInt k2s = sub_sint(mul_sint(c1, SInt::from_vec(B1MAG_VEC)),
                      mul_sint(c2, SInt::from_vec(A1_VEC)));
  Vec k1v = rem_n_sint(k1s);
  Vec k2v = rem_n_sint(k2s);
  Vec pow2_128{0, 0, 1};
  Split sp;
  sp.k1neg = cmp_vec(k1v, pow2_128) > 0;
  sp.k2neg = cmp_vec(k2v, pow2_128) > 0;
  if (sp.k1neg) k1v = sub_vec(N_VEC, k1v);
  if (sp.k2neg) k2v = sub_vec(N_VEC, k2v);
  sp.k1 = vec_to4(k1v);
  sp.k2 = vec_to4(k2v);
  return sp;
}
bool scalar_is_zero(const std::array<uint64_t, 4>& a) {
  return a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0;
}
void scalar_shr1(std::array<uint64_t, 4>& a) {
  for (int i = 0; i < 4; i++) {
    a[i] >>= 1;
    if (i + 1 < 4) a[i] |= a[i + 1] << 63;
  }
}

Jac mul_unsafe(const Jac& pt, const Vec& scalar) {
  Split sp = split_scalar_endo(scalar);
  std::array<uint64_t, 4> k1 = sp.k1, k2 = sp.k2;
  Jac k1p = Jac::zero();
  Jac k2p = Jac::zero();
  Jac d = pt;
  while (!scalar_is_zero(k1) || !scalar_is_zero(k2)) {
    if (k1[0] & 1u) k1p = k1p.add_pt(d);
    if (k2[0] & 1u) k2p = k2p.add_pt(d);
    d = d.double_pt();
    scalar_shr1(k1);
    scalar_shr1(k2);
  }
  if (sp.k1neg) k1p = k1p.negate();
  if (sp.k2neg) k2p = k2p.negate();
  Jac k2t;
  k2t.x = fp_mul(k2p.x, BETA_FP);
  k2t.y = k2p.y;
  k2t.z = k2p.z;
  return k1p.add_pt(k2t);
}
Point point_mul(const Point& p, const Vec& scalar) {
  return mul_unsafe(Jac::from_affine(p), scalar).to_affine();
}

void init_constants() {
  // p = 2^256 - (2^32 + 977): compute from 2^256 as 5 limbs.
  {
    Vec two256{0, 0, 0, 0, 1};
    Vec c{(1ull << 32) + 977};  // 2^32 + 977 fits in one limb
    Vec pvec = sub_vec(two256, c);
    P_CONST = fp_from_vec(pvec);
  }
  // n = 2^256 - 432420386565659656852420866394968145599
  {
    Vec two256{0, 0, 0, 0, 1};
    Vec d = parse_dec("432420386565659656852420866394968145599");
    N_VEC = sub_vec(two256, d);
  }
  PRIV_VEC = parse_dec(
      "20775598474904240222758871485654738649026525153462921990999819694398496339603");
  A1_VEC = parse_hex("3086d221a7d46bcde86c90e49284eb15");
  B1MAG_VEC = parse_hex("e4437ed6010e88286f547fa90abfe4c3");
  A2_VEC = parse_hex("114ca50f7a8e2f3f657c1108d9d44cfd8");
  BETA_FP = fp_from_vec(parse_hex(
      "7ae96a2b657c07106e64479eac3434e99cf0497512f58995c1396c28719501ee"));
  GX_FP = fp_from_vec(parse_dec(
      "55066263022277343669578718895168534326250603453777594175500187360389116729240"));
  GY_FP = fp_from_vec(parse_dec(
      "32670510020758816978083085130507043184471273380659243275938904335757337482424"));
}

}  // namespace

int main(int argc, char* argv[]) {
  int n = 1;
  if (argc > 1) n = std::stoi(argv[1]);
  init_constants();
  Point point{GX_FP, GY_FP};
  for (int i = 0; i < n; i++) point = point_mul(point, PRIV_VEC);
  std::cout << fp_hex(point.x) << ',' << fp_hex(point.y) << '\n';
  return 0;
}

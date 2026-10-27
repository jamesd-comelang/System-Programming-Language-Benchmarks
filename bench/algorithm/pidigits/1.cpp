/*
 * The Computer Language Benchmarks Game
 * https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
 *
 * Faithful port of bench/algorithm/pidigits/8.go (the "math/big"
 * spigot algorithm by the Go Authors, based on pidigits.c).
 * C++ has no stdlib bignum, so the big-integer arithmetic used by
 * the algorithm is implemented here with stdlib only (base 2^32
 * limbs, Knuth Algorithm D division). The algorithm itself --
 * next_term / extract_digit / next_digit / eliminate_digit and
 * the 10-digits-per-line output format -- is unchanged.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

// Unsigned big integer, little-endian base 2^32 limbs, no trailing
// zero limbs (empty == 0). All values used here are non-negative.
struct BigUint {
  std::vector<std::uint32_t> w;

  void trim() {
    while (!w.empty() && w.back() == 0) w.pop_back();
  }
  bool is_zero() const { return w.empty(); }

  static BigUint from_u64(std::uint64_t v) {
    BigUint r;
    if (v) {
      r.w.push_back(static_cast<std::uint32_t>(v));
      if (v >> 32) r.w.push_back(static_cast<std::uint32_t>(v >> 32));
    }
    return r;
  }
  // Low 64 bits; the quotients this is used for are single digits.
  std::uint64_t to_u64() const {
    std::uint64_t v = 0;
    if (!w.empty()) v = w[0];
    if (w.size() > 1) v |= static_cast<std::uint64_t>(w[1]) << 32;
    return v;
  }
};

int cmp_big(const BigUint &a, const BigUint &b) {
  if (a.w.size() != b.w.size()) return a.w.size() < b.w.size() ? -1 : 1;
  for (std::size_t i = a.w.size(); i-- > 0;) {
    if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
  }
  return 0;
}

BigUint add_big(const BigUint &a, const BigUint &b) {
  BigUint r;
  const std::size_t n = a.w.size() > b.w.size() ? a.w.size() : b.w.size();
  r.w.resize(n + 1, 0);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint64_t s = carry;
    if (i < a.w.size()) s += a.w[i];
    if (i < b.w.size()) s += b.w[i];
    r.w[i] = static_cast<std::uint32_t>(s);
    carry = s >> 32;
  }
  r.w[n] = static_cast<std::uint32_t>(carry);
  r.trim();
  return r;
}

// Requires a >= b.
BigUint sub_big(const BigUint &a, const BigUint &b) {
  BigUint r;
  r.w.resize(a.w.size(), 0);
  std::int64_t borrow = 0;
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    std::int64_t d = static_cast<std::int64_t>(a.w[i]) - borrow;
    if (i < b.w.size()) d -= b.w[i];
    if (d < 0) {
      d += (std::int64_t{1} << 32);
      borrow = 1;
    } else {
      borrow = 0;
    }
    r.w[i] = static_cast<std::uint32_t>(d);
  }
  r.trim();
  return r;
}

BigUint mul_small(const BigUint &a, std::uint64_t m) {
  if (a.is_zero() || m == 0) return BigUint{};
  BigUint r;
  r.w.resize(a.w.size() + 2, 0);
  unsigned __int128 carry = 0;
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    unsigned __int128 cur = static_cast<unsigned __int128>(a.w[i]) * m + carry;
    r.w[i] = static_cast<std::uint32_t>(cur);
    carry = cur >> 32;
  }
  std::size_t i = a.w.size();
  while (carry) {
    r.w[i++] = static_cast<std::uint32_t>(carry);
    carry >>= 32;
  }
  r.trim();
  return r;
}

BigUint shift_left_bits(const BigUint &a, unsigned s) {
  if (a.is_zero() || s == 0) return a;
  BigUint r;
  r.w.resize(a.w.size() + 1, 0);
  std::uint32_t carry = 0;
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    r.w[i] = (a.w[i] << s) | carry;
    carry = a.w[i] >> (32 - s);
  }
  r.w[a.w.size()] = carry;
  r.trim();
  return r;
}

BigUint shift_right_bits(const BigUint &a, unsigned s) {
  if (a.is_zero() || s == 0) return a;
  BigUint r;
  r.w.resize(a.w.size(), 0);
  std::uint32_t carry = 0;
  for (std::size_t i = a.w.size(); i-- > 0;) {
    r.w[i] = (a.w[i] >> s) | carry;
    carry = a.w[i] << (32 - s);
  }
  r.trim();
  return r;
}

BigUint mul_naive(const BigUint &a, const BigUint &b) {
  if (a.is_zero() || b.is_zero()) return BigUint{};
  BigUint r;
  r.w.assign(a.w.size() + b.w.size(), 0);
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    unsigned __int128 carry = 0;
    for (std::size_t j = 0; j < b.w.size(); ++j) {
      unsigned __int128 cur = static_cast<unsigned __int128>(r.w[i + j]) +
                              static_cast<unsigned __int128>(a.w[i]) * b.w[j] +
                              carry;
      r.w[i + j] = static_cast<std::uint32_t>(cur);
      carry = cur >> 32;
    }
    std::size_t k = i + b.w.size();
    while (carry) {
      unsigned __int128 cur = static_cast<unsigned __int128>(r.w[k]) + carry;
      r.w[k] = static_cast<std::uint32_t>(cur);
      carry = cur >> 32;
      ++k;
    }
  }
  r.trim();
  return r;
}

BigUint low_limbs(const BigUint &a, std::size_t n) {
  BigUint r;
  const std::size_t m = a.w.size() < n ? a.w.size() : n;
  r.w.assign(a.w.begin(), a.w.begin() + static_cast<std::ptrdiff_t>(m));
  return r;
}

BigUint high_limbs(const BigUint &a, std::size_t n) {
  if (a.w.size() <= n) return BigUint{};
  BigUint r;
  r.w.assign(a.w.begin() + static_cast<std::ptrdiff_t>(n), a.w.end());
  return r;
}

// Adds b shifted by `shift` limbs into r (r grows as needed).
void add_shifted(BigUint &r, const BigUint &b, std::size_t shift) {
  if (b.is_zero()) return;
  if (r.w.size() < b.w.size() + shift) r.w.resize(b.w.size() + shift, 0);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < b.w.size(); ++i) {
    std::uint64_t s = static_cast<std::uint64_t>(r.w[shift + i]) + b.w[i] + carry;
    r.w[shift + i] = static_cast<std::uint32_t>(s);
    carry = s >> 32;
  }
  std::size_t i = shift + b.w.size();
  while (carry) {
    if (i >= r.w.size()) r.w.push_back(0);
    std::uint64_t s = static_cast<std::uint64_t>(r.w[i]) + carry;
    r.w[i] = static_cast<std::uint32_t>(s);
    carry = s >> 32;
    ++i;
  }
  r.trim();
}

BigUint mul_big(const BigUint &a, const BigUint &b) {
  const std::size_t n =
      a.w.size() > b.w.size() ? a.w.size() : b.w.size();
  if (n < 48) return mul_naive(a, b);
  const std::size_t m = n / 2;
  const BigUint a0 = low_limbs(a, m), a1 = high_limbs(a, m);
  const BigUint b0 = low_limbs(b, m), b1 = high_limbs(b, m);
  const BigUint z0 = mul_big(a0, b0);
  const BigUint z2 = mul_big(a1, b1);
  const BigUint z1 = sub_big(sub_big(mul_big(add_big(a0, a1), add_big(b0, b1)), z0), z2);
  BigUint r = z0;
  add_shifted(r, z1, m);
  add_shifted(r, z2, 2 * m);
  return r;
}

// Division by a single limb; returns {quotient, remainder}.
std::pair<BigUint, std::uint32_t> div_small(const BigUint &a, std::uint32_t d) {
  BigUint q;
  q.w.resize(a.w.size(), 0);
  std::uint64_t rem = 0;
  for (std::size_t i = a.w.size(); i-- > 0;) {
    std::uint64_t cur = (rem << 32) | a.w[i];
    q.w[i] = static_cast<std::uint32_t>(cur / d);
    rem = cur % d;
  }
  q.trim();
  return {q, static_cast<std::uint32_t>(rem)};
}

// Knuth Algorithm D. Returns {quotient, remainder}.
std::pair<BigUint, BigUint> divmod_big(const BigUint &a, const BigUint &b) {
  if (cmp_big(a, b) < 0) return {BigUint{}, a};
  if (b.w.size() == 1) {
    auto [q, r] = div_small(a, b.w[0]);
    return {q, BigUint::from_u64(r)};
  }
  const std::size_t n = b.w.size();
  const std::size_t m = a.w.size() - n;
  const unsigned shift = static_cast<unsigned>(__builtin_clz(b.w.back()));
  const BigUint vn = shift_left_bits(b, shift);
  BigUint un = shift_left_bits(a, shift);
  if (un.w.size() < a.w.size() + 1) un.w.resize(a.w.size() + 1, 0);
  BigUint q;
  q.w.resize(m + 1, 0);
  const std::uint64_t base = std::uint64_t{1} << 32;
  for (std::size_t jj = m + 1; jj-- > 0;) {
    const std::size_t j = jj;
    unsigned __int128 num =
        (static_cast<unsigned __int128>(un.w[j + n]) << 32) | un.w[j + n - 1];
    unsigned __int128 qhat = num / vn.w[n - 1];
    unsigned __int128 rhat = num % vn.w[n - 1];
    while (qhat >= base ||
           qhat * vn.w[n - 2] >
               ((rhat << 32) | un.w[j + n - 2])) {
      --qhat;
      rhat += vn.w[n - 1];
      if (rhat >= base) break;
    }
    // Multiply and subtract.
    __int128 borrow = 0;
    for (std::size_t i = 0; i < n; ++i) {
      __int128 cur = static_cast<__int128>(un.w[i + j]) - borrow -
                     static_cast<__int128>(qhat) * vn.w[i];
      un.w[i + j] = static_cast<std::uint32_t>(cur);
      borrow = ((static_cast<__int128>(un.w[i + j]) - cur) >> 32);
    }
    __int128 cur = static_cast<__int128>(un.w[j + n]) - borrow;
    un.w[j + n] = static_cast<std::uint32_t>(cur);
    if (cur < 0) {
      // qhat was one too large; add back.
      --qhat;
      unsigned __int128 carry = 0;
      for (std::size_t i = 0; i < n; ++i) {
        unsigned __int128 s =
            static_cast<unsigned __int128>(un.w[i + j]) + vn.w[i] + carry;
        un.w[i + j] = static_cast<std::uint32_t>(s);
        carry = s >> 32;
      }
      un.w[j + n] =
          static_cast<std::uint32_t>(static_cast<unsigned __int128>(un.w[j + n]) + carry);
    }
    q.w[j] = static_cast<std::uint32_t>(qhat);
  }
  q.trim();
  BigUint r = shift_right_bits(un, shift);
  if (r.w.size() > n) r.w.resize(n);
  r.trim();
  return {q, r};
}

// --- Signed wrapper: Go's big.Int is signed, and accum goes
// negative (e.g. 6 - 3*3 = -3 after the first digit), so the
// spigot state needs a sign, not just a magnitude. ---
struct SBig {
  BigUint m;
  bool neg = false;
};

SBig s_from(const BigUint &v) { return SBig{v, false}; }
SBig s_neg(SBig v) {
  if (!v.m.is_zero()) v.neg = !v.neg;
  return v;
}

int cmp_s(const SBig &a, const SBig &b) {
  if (a.neg != b.neg) return a.neg ? -1 : 1;
  const int c = cmp_big(a.m, b.m);
  return a.neg ? -c : c;
}

SBig add_s(const SBig &a, const SBig &b) {
  if (a.neg == b.neg) {
    BigUint mag = add_big(a.m, b.m);
    return SBig{mag, a.neg && !mag.is_zero()};
  }
  const int c = cmp_big(a.m, b.m);
  if (c == 0) return SBig{};
  if (c > 0) return SBig{sub_big(a.m, b.m), a.neg};
  return SBig{sub_big(b.m, a.m), b.neg};
}

SBig sub_s(const SBig &a, const SBig &b) { return add_s(a, s_neg(b)); }

SBig mul_small_s(const SBig &a, std::uint64_t m) {
  BigUint mag = mul_small(a.m, m);
  return SBig{mag, a.neg && !mag.is_zero()};
}

// --- Spigot algorithm, ported from 8.go ---

SBig g_accum;
BigUint g_denom = BigUint::from_u64(1);
BigUint g_numer = BigUint::from_u64(1);

std::int64_t next_term(std::int64_t k) {
  for (;;) {
    ++k;
    const std::uint64_t y2 = static_cast<std::uint64_t>(k) * 2 + 1;
    g_accum = add_s(g_accum, s_from(mul_small(g_numer, 2)));
    g_accum = mul_small_s(g_accum, y2);
    g_denom = mul_small(g_denom, y2);
    g_numer = mul_small(g_numer, static_cast<std::uint64_t>(k));
    if (cmp_s(g_accum, s_from(g_numer)) > 0) return k;
  }
}

std::int64_t extract_digit(std::uint64_t nth) {
  // Go big.Int Div is Euclidean: for a positive divisor the
  // quotient is the floor, so a negative numerator with a
  // non-zero remainder rounds one further down than truncation.
  SBig t = add_s(s_from(mul_small(g_numer, nth)), g_accum);
  auto [q, r] = divmod_big(t.m, g_denom);
  std::uint64_t mag = q.to_u64();
  if (t.neg && !r.is_zero()) ++mag;
  return t.neg ? -static_cast<std::int64_t>(mag)
               : static_cast<std::int64_t>(mag);
}

std::pair<std::int64_t, std::int64_t> next_digit(std::int64_t k) {
  for (;;) {
    k = next_term(k);
    const std::int64_t d3 = extract_digit(3);
    const std::int64_t d4 = extract_digit(4);
    if (d3 == d4) return {d3, k};
  }
}

void eliminate_digit(std::int64_t d) {
  g_accum = sub_s(g_accum, s_from(mul_small(g_denom, static_cast<std::uint64_t>(d))));
  g_accum = mul_small_s(g_accum, 10);
  g_numer = mul_small(g_numer, 10);
}

long parse_long(const char *s, long fallback) {
  if (!s) return fallback;
  char *end = nullptr;
  const long v = std::strtol(s, &end, 10);
  return (end != s) ? v : fallback;
}

} // namespace

int main(int argc, char **argv) {
  // 8.go leaves n at 0 with no argument; the Rust port defaults to
  // 27, which is used here so a bare run still prints something.
  const long n = parse_long(argc > 1 ? argv[1] : nullptr, 27);
  std::string out;
  out.reserve(static_cast<std::size_t>(n > 0 ? n + n / 10 * 16 : 0));
  std::string line;
  line.reserve(10);
  std::int64_t k = 0;
  for (long i = 1; i <= n; ++i) {
    std::int64_t d = 0;
    std::tie(d, k) = next_digit(k);
    line.push_back(static_cast<char>('0' + d));
    if (line.size() == 10) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s\t:%ld\n", line.c_str(), i);
      out += buf;
      line.clear();
    }
    eliminate_digit(d);
  }
  if (!line.empty()) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%-10s\t:%ld\n", line.c_str(), n);
    out += buf;
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

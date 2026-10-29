/*
 * Binary-splitting sum of 1/k! terms, as in bench/algorithm/edigits/1.rs
 * (after tczajka/bigint-benchmark-rs). C++ has no stdlib bignum, so the
 * big-integer arithmetic is implemented here with stdlib only (base 2^32
 * limbs, Karatsuba multiplication).
 *
 * Difference from 1.cpp: the final phase. 1.cpp computes
 * floor(p * 10^(n-1) / q) with one giant Knuth division and then renders
 * the answer by repeated division by 10^9 -- both quadratic, and the
 * division's inner loop multiplies __int128 quotients. Here, since
 * 2 < p/q < 3, we write p = 2q + r (0 < r < q) and extract the answer
 * in base-10^9 chunks by long division: each chunk is one Knuth step
 * (one quotient limb, uint64 arithmetic) against a once-normalized
 * divisor, so the whole final phase is one mul_small pass plus one
 * divide-step pass per chunk instead of a full division plus a
 * separate quadratic base conversion. The integer computed is exactly
 * floor(p * 10^(n-1) / q), identical to 1.cpp's, so the output is
 * unchanged.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
};

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

// Requires m < 2^32 (all callers pass m <= 10^9), so every intermediate
// fits in uint64_t and no __int128 arithmetic is needed in this hot loop.
BigUint mul_small(const BigUint &a, std::uint64_t m) {
  if (a.is_zero() || m == 0) return BigUint{};
  BigUint r;
  r.w.resize(a.w.size() + 2, 0);
  std::uint64_t carry = 0;
  for (std::size_t i = 0; i < a.w.size(); ++i) {
    const std::uint64_t cur = static_cast<std::uint64_t>(a.w[i]) * m + carry;
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

// One Knuth step: an holds N+1 limbs (the normalized dividend, an < qn
// * 10^9), qn holds N limbs with its top limb >= 2^31. Returns the single
// quotient limb (< 10^9) and leaves the normalized remainder in an[0..N-1]
// (an[N] becomes 0). Requires N >= 2; see div_step_small for N == 1.
// The quotient estimate is corrected exactly as in Knuth Algorithm D, so
// the returned quotient is either exact or one too large, in which case
// the add-back below fixes it. Unlike a general Knuth loop, the borrow
// here is a full limb-sized value (the product qhat*qn spans N+1 limbs),
// which is why the per-limb subtraction tracks borrow in uint64_t.
std::uint32_t div_step(BigUint &an, const BigUint &qn) {
  const std::size_t n = qn.w.size();
  const std::uint64_t base = std::uint64_t{1} << 32;
  const std::uint64_t num =
      (static_cast<std::uint64_t>(an.w[n]) << 32) | an.w[n - 1];
  std::uint64_t qhat = num / qn.w[n - 1];
  std::uint64_t rhat = num % qn.w[n - 1];
  while (qhat >= base ||
         qhat * qn.w[n - 2] > ((rhat << 32) | an.w[n - 2])) {
    --qhat;
    rhat += qn.w[n - 1];
    if (rhat >= base) break;
  }
  std::uint64_t borrow = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint64_t prod = qhat * qn.w[i] + borrow;
    const std::uint64_t ai = an.w[i];
    an.w[i] = static_cast<std::uint32_t>(ai - prod);
    borrow = (prod <= ai) ? 0 : ((prod - ai + 0xFFFFFFFFull) >> 32);
  }
  std::uint32_t top = static_cast<std::uint32_t>(an.w[n] - borrow);
  // Knuth guarantees qhat is exact or one too large; the loop form also
  // covers the vanishingly rare off-by-two case (top then wraps to
  // 0xFFFFFFFE, still with its high bit set).
  while (top >= 0x80000000u) {
    --qhat;
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint64_t s =
          static_cast<std::uint64_t>(an.w[i]) + qn.w[i] + carry;
      an.w[i] = static_cast<std::uint32_t>(s);
      carry = s >> 32;
    }
    top = static_cast<std::uint32_t>(top + carry);
  }
  an.w[n] = top;
  return static_cast<std::uint32_t>(qhat);
}

// N == 1 fallback for div_step: the whole division fits in uint64_t.
std::uint32_t div_step_small(BigUint &an, const BigUint &qn) {
  const std::uint64_t num =
      (static_cast<std::uint64_t>(an.w[1]) << 32) | an.w[0];
  const std::uint64_t q = num / qn.w[0];
  an.w[0] = static_cast<std::uint32_t>(num - q * qn.w[0]);
  an.w[1] = 0;
  return static_cast<std::uint32_t>(q);
}

// a! * (1/(a+1)! + ... + 1/b!) as a fraction p / q.
// q = (a+1) * ... * b; see 1.rs for the derivation.
std::pair<BigUint, BigUint> sum_terms(long a, long b) {
  if (b == a + 1) {
    return {BigUint::from_u64(1), BigUint::from_u64(static_cast<std::uint64_t>(b))};
  }
  const long mid = (a + b) / 2;
  auto [p_left, q_left] = sum_terms(a, mid);
  auto [p_right, q_right] = sum_terms(mid, b);
  // p / q = (p_left * q_right + p_right) / (q_left * q_right)
  BigUint p = add_big(mul_big(p_left, q_right), p_right);
  BigUint q = mul_big(q_left, q_right);
  return {p, q};
}

// Find k such that log_10 k! >= n + 50 (Stirling's approximation),
// exactly as in 1.rs. The predicate is monotone in k.
long binary_search(long n) {
  auto test = [n](long k) -> bool {
    if (k <= 0) return false;
    const double kd = static_cast<double>(k);
    const double ln_k_factorial =
        kd * std::log(kd) - kd + 0.5 * std::log(2.0 * 3.14159265358979323846 * kd);
    return ln_k_factorial / 2.302585092994046 > static_cast<double>(n + 50);
  };
  long a = 0, b = 1;
  while (!test(b)) {
    a = b;
    b *= 2;
  }
  while (b - a > 1) {
    const long m = a + (b - a) / 2;
    if (test(m)) {
      b = m;
    } else {
      a = m;
    }
  }
  return b;
}

// n digits of e, as a decimal string (no decimal point).
// p/q = sum_{j=0}^{k-1} 1/j! lies in (2, 3), so with r = p - 2q the
// answer is "2" followed by the n-1 digits of floor(r * 10^(n-1) / q),
// extracted in base-10^9 chunks. Each chunk costs one mul_small pass
// and one div_step pass over the (normalized) remainder, which stays
// below qn throughout, so every quotient is < 10^9 by construction.
std::string calculate(long n) {
  if (n <= 1) return "2";
  const long k = binary_search(n);
  auto [p, q] = sum_terms(0, k - 1);
  p = add_big(p, q); // add 1/0! = 1
  const BigUint r = sub_big(p, mul_small(q, 2)); // 0 < r < q
  const std::size_t nlimbs = q.w.size();
  const unsigned shift = static_cast<unsigned>(__builtin_clz(q.w.back()));
  const BigUint qn = shift_left_bits(q, shift);
  BigUint rem = shift_left_bits(r, shift); // normalized remainder (< qn)
  const long rem_digits = n - 1;
  const long nchunks = (rem_digits + 8) / 9;
  const long first_digits = rem_digits - 9 * (nchunks - 1); // 1..9
  std::string out;
  out.reserve(static_cast<std::size_t>(n));
  out.push_back('2');
  auto append_chunk = [&out](std::uint32_t chunk, long width) {
    // Zero-padded to `width` digits (width <= 9, chunk < 10^width).
    char buf[32];
    for (long i = width - 1; i >= 0; --i) {
      buf[i] = static_cast<char>('0' + chunk % 10);
      chunk /= 10;
    }
    out.append(buf, static_cast<std::size_t>(width));
  };
  for (long c = 0; c < nchunks; ++c) {
    const long width = (c == 0) ? first_digits : 9;
    std::uint64_t scale = 1;
    for (long i = 0; i < width; ++i) scale *= 10;
    BigUint an = mul_small(rem, scale);
    an.w.resize(nlimbs + 1, 0);
    const std::uint32_t chunk =
        (nlimbs >= 2) ? div_step(an, qn) : div_step_small(an, qn);
    append_chunk(chunk, width);
    // Swap buffers instead of copying: an now holds the remainder in
    // limbs [0..nlimbs), with an[nlimbs] == 0.
    rem.w.swap(an.w);
    rem.w.resize(nlimbs);
  }
  return out;
}

long parse_long(const char *s, long fallback) {
  if (!s) return fallback;
  char *end = nullptr;
  const long v = std::strtol(s, &end, 10);
  return (end != s) ? v : fallback;
}

} // namespace

int main(int argc, char **argv) {
  const long n = parse_long(argc > 1 ? argv[1] : nullptr, 27);
  const std::string s = calculate(n);
  std::string out;
  out.reserve(static_cast<std::size_t>(n + n / 10 * 16));
  for (long i = 0; i < n; i += 10) {
    if (i + 10 <= n) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s\t:%ld\n", s.substr(static_cast<std::size_t>(i), 10).c_str(), i + 10);
      out += buf;
    } else {
      std::string line = s.substr(static_cast<std::size_t>(i));
      line.append(static_cast<std::size_t>(10 - (n % 10)), ' ');
      char buf[64];
      std::snprintf(buf, sizeof buf, "%s\t:%ld\n", line.c_str(), n);
      out += buf;
    }
  }
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

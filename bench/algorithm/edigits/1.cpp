/*
 * Faithful port of bench/algorithm/edigits/1.rs (binary-splitting
 * sum of 1/k! terms, after tczajka/bigint-benchmark-rs).
 * C++ has no stdlib bignum, so the big-integer arithmetic used by
 * the algorithm is implemented here with stdlib only (base 2^32
 * limbs, Karatsuba multiplication, Knuth Algorithm D division).
 * The algorithm itself -- Stirling-based term-count search,
 * sum_terms, and the 10-digits-per-line output format -- is
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

// Decimal rendering: repeated division by 10^9.
std::string to_string(const BigUint &a) {
  if (a.is_zero()) return "0";
  std::vector<std::uint32_t> chunks;
  BigUint cur = a;
  while (!cur.is_zero()) {
    auto [q, r] = div_small(cur, 1000000000u);
    chunks.push_back(r);
    cur = q;
  }
  std::string s = std::to_string(chunks.back());
  char buf[16];
  for (std::size_t i = chunks.size() - 1; i-- > 0;) {
    std::snprintf(buf, sizeof buf, "%09u", chunks[i]);
    s += buf;
  }
  return s;
}

BigUint pow_u32(std::uint32_t base, long exp) {
  BigUint result = BigUint::from_u64(1);
  BigUint b = BigUint::from_u64(base);
  while (exp > 0) {
    if (exp & 1) result = mul_big(result, b);
    exp >>= 1;
    if (exp) b = mul_big(b, b);
  }
  return result;
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
std::string calculate(long n) {
  const long k = binary_search(n);
  auto [p, q] = sum_terms(0, k - 1);
  p = add_big(p, q); // add 1/0! = 1
  BigUint answer = divmod_big(mul_big(p, pow_u32(10, n - 1)), q).first;
  return to_string(answer);
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

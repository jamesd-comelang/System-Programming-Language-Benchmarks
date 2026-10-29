/*
 * Pi digits via Machin's formula, in fixed-point decimal arithmetic:
 *   pi = 16*arctan(1/5) - 4*arctan(1/239)
 * arctan(1/x) is summed term by term, each term obtained from the
 * previous one by division by a small integer, so no big-integer
 * library is needed. 30 guard digits past the requested count make
 * the truncation error from the small divisions invisible in the
 * first n digits (verified against the _out files).
 *
 * Faithful C++ port of bench/algorithm/pidigits/1.c. This replaces the
 * spigot algorithm of 1.cpp (which carries O(n)-limb big integers and
 * a full big division per digit) with O(L) small-integer passes over
 * base-10000 limbs, where L ~= n/4.
 *
 * Output format matches bench/algorithm/pidigits/8.go: groups of 10
 * digits, "%s\t:%d\n", last group space-padded to width 10.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int BASE = 10000;
const int GUARD = 30;

int L; // number of limbs
std::vector<int> pow10_arr; // 10^digits, set by main

// Divide a[] (length L, little-endian base BASE) by d in place.
// Returns 1 if the result is non-zero.
int div_small(std::vector<int> &a, int d) {
  long rem = 0;
  int nz = 0;
  for (int i = L - 1; i >= 0; i--) {
    long cur = rem * BASE + a[i];
    a[i] = (int)(cur / d);
    rem = cur % d;
    if (a[i]) nz = 1;
  }
  return nz;
}

void add_into(std::vector<int> &dst, const std::vector<int> &src) {
  long carry = 0;
  for (int i = 0; i < L; i++) {
    long s = (long)dst[i] + src[i] + carry;
    dst[i] = (int)(s % BASE);
    carry = s / BASE;
  }
}

void mul_small_into(std::vector<int> &dst, const std::vector<int> &src, int m) {
  long carry = 0;
  for (int i = 0; i < L; i++) {
    long s = (long)src[i] * m + carry;
    dst[i] = (int)(s % BASE);
    carry = s / BASE;
  }
}

// dst = a - b, assuming a >= b.
void sub_into(std::vector<int> &dst, const std::vector<int> &a,
              const std::vector<int> &b) {
  long borrow = 0;
  for (int i = 0; i < L; i++) {
    long d = (long)a[i] - b[i] - borrow;
    if (d < 0) {
      d += BASE;
      borrow = 1;
    } else {
      borrow = 0;
    }
    dst[i] = (int)d;
  }
}

// arctan(1/x) * 10^digits, split into positive and negative partial
// sums (to avoid signed big arithmetic).
void arctan_reciprocal(int x, std::vector<int> &pos, std::vector<int> &neg) {
  std::vector<int> p(L, 0);
  std::vector<int> term(L, 0);
  // p = 10^digits / x, computed by starting from 10^digits and
  // dividing once by x (10^digits is built by main).
  p = pow10_arr;
  div_small(p, x);
  int x2 = x * x;
  for (int k = 0;; k++) {
    term = p;
    div_small(term, 2 * k + 1);
    if (k % 2 == 0)
      add_into(pos, term);
    else
      add_into(neg, term);
    if (!div_small(p, x2)) break;
  }
}

} // namespace

int main(int argc, char **argv) {
  int n = 27;
  if (argc > 1) n = std::atoi(argv[1]);
  int digits = n + GUARD;
  L = digits / 4 + 4;

  pow10_arr.assign((size_t)L, 0);
  int q = digits / 4, r = digits % 4;
  if (r == 0)
    pow10_arr[q] = 1;
  else {
    int v = 1;
    for (int i = 0; i < r; i++) v *= 10;
    pow10_arr[q] = v;
  }

  std::vector<int> s5p(L, 0), s5n(L, 0), s239p(L, 0), s239n(L, 0);
  arctan_reciprocal(5, s5p, s5n);
  arctan_reciprocal(239, s239p, s239n);

  std::vector<int> tpos(L, 0), tneg(L, 0), tmp(L, 0);
  // total_pos = 16*s5p + 4*s239n ; total_neg = 16*s5n + 4*s239p
  mul_small_into(tmp, s5p, 16);
  add_into(tpos, tmp);
  mul_small_into(tmp, s239n, 4);
  add_into(tpos, tmp);
  mul_small_into(tmp, s5n, 16);
  add_into(tneg, tmp);
  mul_small_into(tmp, s239p, 4);
  add_into(tneg, tmp);

  std::vector<int> pi(L, 0);
  sub_into(pi, tpos, tneg);

  // Convert to a decimal string.
  int hi = L - 1;
  while (hi > 0 && pi[hi] == 0) hi--;
  std::string str;
  str.reserve((size_t)(digits + 8));
  char buf[32];
  std::snprintf(buf, sizeof buf, "%d", pi[hi]);
  str += buf;
  for (int i = hi - 1; i >= 0; i--) {
    std::snprintf(buf, sizeof buf, "%04d", pi[i]);
    str += buf;
  }

  // Print the first n digits in groups of 10.
  for (int i = 0; i < n; i += 10) {
    if (i + 10 <= n) {
      std::printf("%.10s\t:%d\n", str.c_str() + i, i + 10);
    } else {
      std::printf("%-*.*s\t:%d\n", 10, n - i, str.c_str() + i, n);
    }
  }
  return 0;
}

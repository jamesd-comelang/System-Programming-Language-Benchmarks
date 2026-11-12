/*
 * Pi digits via Machin's formula, parallel (-m) version of
 * bench/algorithm/pidigits/2.cpp (itself a faithful C++ port of
 * bench/algorithm/pidigits/1.c: pi = 16*arctan(1/5) - 4*arctan(1/239)
 * in fixed-point decimal arithmetic, no big-integer library).
 *
 * Parallelization: each arctan series is split across T NumCPU
 * workers by stride, not by contiguous chunk. Worker t (0 <= t < T)
 * handles the terms k = t, t+T, t+2T, ... . Its first value is
 * p_t = p_0 / x^2^t (a short serial chain of at most T-1 divisions,
 * computed once in main); each following step divides by x^2^T in a
 * single pass. A contiguous split would NOT work: chunk c would have
 * to start from p_0 / x^2^(c*S), costing c*S extra divisions, and
 * the last chunk alone would cost as much as the whole serial run.
 *
 * Byte-identical output: every (k, term_k) pair is computed exactly
 * once, by the same exact integer arithmetic as the serial code
 * (floor(floor(a/b)/c) == floor(a/(b*c)) for non-negative integers,
 * so the strided recurrence reproduces the serial p_k bit-for-bit),
 * and each worker accumulates its terms into its own pos/neg
 * partials. The final sums are exact integer additions, which are
 * order-independent, so the combined result -- and the printed
 * digits -- are identical to 2.cpp at every input.
 *
 * Thread count T is std::thread::hardware_concurrency(), capped at
 * 7: the strided step divides by x^2^T in one unsigned __int128
 * pass, and for x=239, 239^2^7 is about 2.0e33, so the intermediate
 * rem*BASE stays below 2^127, while 239^2^8 would overflow it.
 * (The CI runners have 4 cores, so the cap never binds there.) A
 * zero hardware_concurrency() or a std::thread spawn failure is
 * reported on stderr and exits nonzero before anything is printed;
 * there is no serial fallback.
 *
 * Output format matches bench/algorithm/pidigits/8.go: groups of 10
 * digits, "%s\t:%d\n", last group space-padded to width 10.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

const int BASE = 10000;
const int GUARD = 30;
const unsigned MAX_WORKERS = 7;

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

// Divide a[] by a wide divisor d (up to < 2^127 / BASE) in place.
// Returns 1 if the result is non-zero. Used for the strided step,
// which divides by x^2^T: for x=239 and T<=7 the divisor is at most
// about 2.0e33, which does not fit in a long.
int div_small_wide(std::vector<int> &a, unsigned __int128 d) {
  unsigned __int128 rem = 0;
  int nz = 0;
  for (int i = L - 1; i >= 0; i--) {
    unsigned __int128 cur = rem * BASE + (unsigned int)a[i];
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

void mul_small_into(std::vector<int> &dst, const std::vector<int> &src,
                    int m) {
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

struct WorkerArg {
  int t; // this worker's stride offset
  int T; // worker count
  unsigned __int128 x2powT; // (x*x)^T, the strided-step divisor
  const std::vector<int> *p_start; // p_0 / x^2^t (read-only here)
  std::vector<int> *pos; // this worker's positive partial sum
  std::vector<int> *neg; // this worker's negative partial sum
};

// One strided slice of arctan(1/x): terms k = t, t+T, t+2T, ... .
void arctan_worker(const WorkerArg &w) {
  std::vector<int> p = *w.p_start;
  std::vector<int> term((size_t)L, 0);
  for (int k = w.t;; k += w.T) {
    term = p;
    div_small(term, 2 * k + 1);
    if (k % 2 == 0)
      add_into(*w.pos, term);
    else
      add_into(*w.neg, term);
    if (!div_small_wide(p, w.x2powT)) break;
  }
}

// arctan(1/x) * 10^digits, split into positive and negative partial
// sums (to avoid signed big arithmetic). Computed by T workers, each
// handling one strided slice of the term series; returns false if a
// worker could not be started.
bool arctan_reciprocal_parallel(int x, std::vector<int> &pos,
                                std::vector<int> &neg, unsigned T) {
  int x2 = x * x;
  unsigned __int128 x2powT = 1;
  for (unsigned i = 0; i < T; i++) x2powT *= (unsigned int)x2;

  // p_0 = 10^digits / x, then the per-worker starts
  // p_t = p_0 / x^2^t as a serial chain.
  std::vector<int> p0 = pow10_arr;
  div_small(p0, x);
  std::vector<std::vector<int>> starts(T, std::vector<int>((size_t)L, 0));
  std::vector<std::vector<int>> pos_parts(T,
                                           std::vector<int>((size_t)L, 0));
  std::vector<std::vector<int>> neg_parts(T,
                                           std::vector<int>((size_t)L, 0));
  starts[0] = p0;
  for (unsigned t = 1; t < T; t++) {
    starts[t] = starts[t - 1];
    div_small(starts[t], x2);
  }

  std::vector<WorkerArg> args(T);
  for (unsigned t = 0; t < T; t++) {
    args[t] = WorkerArg{(int)t, (int)T, x2powT, &starts[t], &pos_parts[t],
                        &neg_parts[t]};
  }
  std::vector<std::thread> threads;
  threads.reserve(T);
  try {
    for (unsigned t = 0; t < T; t++) {
      threads.emplace_back([&, t]() { arctan_worker(args[t]); });
    }
  } catch (const std::system_error &e) {
    std::fprintf(stderr, "pidigits: thread spawn failed: %s\n", e.what());
    for (auto &th : threads) th.join();
    return false;
  }
  for (auto &th : threads) th.join();
  for (unsigned t = 0; t < T; t++) {
    add_into(pos, pos_parts[t]);
    add_into(neg, neg_parts[t]);
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  int n = 27;
  if (argc > 1) n = std::atoi(argv[1]);
  int digits = n + GUARD;
  L = digits / 4 + 4;

  unsigned T = std::thread::hardware_concurrency();
  if (T == 0) {
    std::fprintf(stderr, "pidigits: hardware_concurrency() returned 0\n");
    return 1;
  }
  if (T > MAX_WORKERS) T = MAX_WORKERS;

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
  if (!arctan_reciprocal_parallel(5, s5p, s5n, T)) return 1;
  if (!arctan_reciprocal_parallel(239, s239p, s239n, T)) return 1;

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

// The Computer Language Shootout
// http://shootout.alioth.debian.org/
// Precedent C entry modified by bearophile for speed and size, 31 Jan 2006
// Converted to C++ by Paul Kitchin
//
// Parallel (-m) variant of 1.cpp: segmented sieve with one std::thread
// per CPU (std::thread::hardware_concurrency() workers, no cap, no
// fallback). The base primes up to sqrt(max) are sieved serially
// (tiny); each worker then sieves one contiguous segment [lo, hi) of
// [0, max) into its own byte buffer, marking multiples of every base
// prime, and counts the primes in its segment. Counts are summed in
// fixed worker order, so the total -- and the printed output -- is
// identical to 1.cpp.
//
// Marking starts at max(p*p, first multiple of p in the segment): every
// multiple of p below p*p already has a smaller prime factor, and p
// itself is never marked, exactly as in the serial loop (which marks
// from 2p and therefore never marks p). Every composite below max has
// a prime factor <= sqrt(max), so every composite in a segment is
// marked, and the per-segment counts add up to the serial count.
//
// Deviation from 1.cpp: per-segment storage is std::vector<std::uint8_t>
// (one byte per value) instead of std::vector<bool>. vector<bool> is
// bit-packed, so marking it is a read-modify-write bit operation per
// multiple and far slower; the byte buffer keeps the same marking
// semantics with the same per-value result.
//
// On thread spawn failure, or if the hardware concurrency cannot be
// determined, the already-started threads are joined and the program
// exits nonzero without printing a result for the current call.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

namespace {

std::size_t isqrt_sz(std::size_t n) {
  // Integer binary square root: largest r with r*r <= n.
  std::size_t r = 0;
  std::size_t lo = 0, hi = n > 0 ? n : 1;
  while (lo <= hi) {
    std::size_t mid = lo + (hi - lo) / 2;
    if (mid != 0 && mid > n / mid) {
      hi = mid - 1;
    } else {
      r = mid;
      lo = mid + 1;
    }
  }
  return r;
}

// Sieve [0, lim] serially; returns the primes <= lim.
std::vector<std::size_t> base_primes(std::size_t lim) {
  std::vector<std::uint8_t> is_comp(lim + 1, 0);
  std::vector<std::size_t> primes;
  for (std::size_t i = 2; i <= lim; ++i) {
    if (is_comp[i] == 0) {
      primes.push_back(i);
      if (i * i <= lim) {
        for (std::size_t j = i * i; j <= lim; j += i) is_comp[j] = 1;
      }
    }
  }
  return primes;
}

struct SegResult {
  std::size_t count = 0;
  bool failed = false;
};

void sieve_segment(const std::vector<std::size_t>& primes, std::size_t lo,
                   std::size_t hi, SegResult& out) {
  if (lo >= hi) {
    out.count = 0;
    return;
  }
  std::vector<std::uint8_t> flags(hi - lo, 1);
  for (std::size_t prime : primes) {
    std::size_t start = prime * prime;
    std::size_t first = (lo + prime - 1) / prime * prime;
    if (first > start) start = first;
    for (std::size_t j = start; j < hi; j += prime) flags[j - lo] = 0;
  }
  std::size_t first_counted = lo < 2 ? 2 : lo;
  std::size_t count = 0;
  for (std::size_t i = first_counted; i < hi; ++i)
    if (flags[i - lo] != 0) ++count;
  out.count = count;
}

bool nsieve(std::size_t max, unsigned int workers) {
  const std::vector<std::size_t> primes = base_primes(isqrt_sz(max));
  const std::size_t chunk = (max + workers - 1) / workers == 0
                                ? 1
                                : (max + workers - 1) / workers;
  std::vector<SegResult> results(workers);
  std::vector<std::thread> threads;
  threads.reserve(workers);
  try {
    for (unsigned int w = 0; w < workers; ++w) {
      const std::size_t lo = (std::size_t)w * chunk;
      std::size_t hi = lo + chunk;
      if (hi > max) hi = max;
      threads.emplace_back([&, w, lo, hi]() {
        try {
          sieve_segment(primes, lo, hi, results[w]);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "nsieve: worker %u failed: %s\n", w,
                       e.what());
          results[w].failed = true;
        }
      });
    }
  } catch (const std::system_error& e) {
    std::fprintf(stderr, "nsieve: thread spawn failed: %s\n", e.what());
    for (auto& t : threads)
      if (t.joinable()) t.join();
    return false;
  }
  for (auto& t : threads) t.join();
  std::size_t count = 0;
  for (const auto& r : results) {
    if (r.failed) return false;
    count += r.count;
  }
  std::cout << "Primes up to " << std::setw(8) << max << ' ' << std::setw(8)
            << count << '\n';
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <n>\n";
    return 1;
  }
  unsigned int count;
  {
    std::istringstream convertor(argv[1]);
    if (!(convertor >> count) || !convertor.eof()) {
      std::cerr << "usage: " << argv[0] << " <n>\n";
      std::cerr << "\tn must be an integer\n";
      return 1;
    }
  }
  unsigned int workers = std::thread::hardware_concurrency();
  if (workers == 0) {
    std::cerr << "nsieve: could not determine hardware concurrency\n";
    return 1;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    if (!nsieve(10000 << (count - i), workers)) return 1;
  }
}

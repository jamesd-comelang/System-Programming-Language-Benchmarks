/*
 * Faithful port of bench/algorithm/lru/1.go (and 1.rs): an LRU
 * cache driven by two LCG streams, printing hit/miss counts.
 * The Go version uses an ordered map; here the ordering is a
 * std::list (front = least recently used) plus an
 * std::unordered_map from key to list position, giving the same
 * Get/Put/eviction semantics.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <unordered_map>
#include <utility>

namespace {

// LCG from 1.go / 1.rs: r = (A*r + C) % M, all in uint32_t
// (unsigned overflow wraps exactly as in Go/Rust).
struct LCG {
  std::uint32_t r;
  explicit LCG(std::uint32_t seed) : r(seed) {}
  std::uint32_t next() {
    r = (1103515245u * r + 12345u) % (std::uint32_t{1} << 31);
    return r;
  }
};

struct LRU {
  std::size_t size;
  // Front = least recently used, back = most recently used.
  std::list<std::uint32_t> order;
  std::unordered_map<std::uint32_t, std::list<std::uint32_t>::iterator> pos;

  explicit LRU(std::size_t size_) : size(size_) {}

  // Returns true on a hit; a hit moves the key to the back.
  bool get(std::uint32_t key) {
    auto it = pos.find(key);
    if (it == pos.end()) return false;
    order.erase(it->second);
    order.push_back(key);
    it->second = --order.end();
    return true;
  }

  void put(std::uint32_t key) {
    auto it = pos.find(key);
    if (it != pos.end()) {
      order.erase(it->second);
      order.push_back(key);
      it->second = --order.end();
    } else if (size == 0) {
      // Degenerate size; 1.go/1.rs never exercise this (their
      // sizes are >= 10). Do nothing rather than evicting from
      // an empty cache.
    } else if (order.size() == size) {
      pos.erase(order.front());
      order.pop_front();
      order.push_back(key);
      pos[key] = --order.end();
    } else {
      order.push_back(key);
      pos[key] = --order.end();
    }
  }
};

long parse_long(const char *s, long fallback) {
  if (!s) return fallback;
  char *end = nullptr;
  const long v = std::strtol(s, &end, 10);
  return (end != s) ? v : fallback;
}

} // namespace

int main(int argc, char **argv) {
  const long size = parse_long(argc > 1 ? argv[1] : nullptr, 100);
  const long n = parse_long(argc > 2 ? argv[2] : nullptr, 10000);
  const std::uint32_t modular = static_cast<std::uint32_t>(size) * 10;
  LCG rng0(0), rng1(1);
  LRU lru(static_cast<std::size_t>(size));
  long hit = 0, missed = 0;
  for (long i = 0; i < n; ++i) {
    const std::uint32_t n0 = rng0.next() % modular;
    lru.put(n0);
    const std::uint32_t n1 = rng1.next() % modular;
    if (lru.get(n1)) {
      ++hit;
    } else {
      ++missed;
    }
  }
  std::printf("%ld\n%ld\n", hit, missed);
  return 0;
}

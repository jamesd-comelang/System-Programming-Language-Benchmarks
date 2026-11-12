// Parallel (-m) variant of 2.rs: segmented sieve with one OS thread
// per CPU (std::thread::available_parallelism() workers, no cap, no
// fallback -- if the count cannot be determined the program reports
// the error and exits nonzero instead of silently running serially).
// The base primes up to sqrt(n) are sieved serially (tiny); each
// scoped thread then sieves one contiguous segment [lo, hi) of [0, n)
// into its own bitvec, marking multiples of every base prime, and
// returns the prime count for its segment. Counts are summed in fixed
// worker order, so the total -- and the printed output -- is identical
// to 2.rs.
//
// Marking starts at max(p*p, first multiple of p in the segment): every
// multiple of p below p*p already has a smaller prime factor, and p
// itself is never marked, exactly as in the serial loop (which marks
// from 2p and therefore never marks p). Every composite below n has a
// prime factor <= sqrt(n), so every composite in a segment is marked.
//
// Counting uses the bitvec directly: bits for global 0 and 1 are never
// set (marking starts at p*p >= 4), so the number of primes in a
// segment is (hi - max(lo, 2)) minus the number of set bits in the
// segment's bitvec. If the OS refuses a thread, std's scoped spawn
// panics with a message and a nonzero exit, and no result is printed.

use bitvec::prelude::*;

fn isqrt(n: usize) -> usize {
    let mut lim = (n as f64).sqrt() as usize;
    while (lim + 1) * (lim + 1) <= n {
        lim += 1;
    }
    while lim * lim > n {
        lim -= 1;
    }
    lim
}

fn base_primes(lim: usize) -> Vec<usize> {
    let mut is_comp = vec![false; lim + 1];
    let mut primes = Vec::new();
    for i in 2..=lim {
        if !is_comp[i] {
            primes.push(i);
            if i * i <= lim {
                for j in (i * i..=lim).step_by(i) {
                    is_comp[j] = true;
                }
            }
        }
    }
    primes
}

fn nsieve(n: usize) {
    let primes = base_primes(isqrt(n));
    let workers = match std::thread::available_parallelism() {
        Ok(v) => v.get(),
        Err(e) => {
            eprintln!("nsieve: could not determine parallelism: {e}");
            std::process::exit(1);
        }
    };
    let chunk = n.div_ceil(workers).max(1);
    let counts: Vec<usize> = std::thread::scope(|s| {
        let mut handles = Vec::with_capacity(workers);
        for w in 0..workers {
            let lo = w * chunk;
            let hi = (lo + chunk).min(n);
            let primes = &primes;
            handles.push(s.spawn(move || -> usize {
                if lo >= hi {
                    return 0;
                }
                let len = hi - lo;
                let mut flags = bitvec![u32, LocalBits; 0; len];
                for &p in primes {
                    let mut start = p * p;
                    let first = (lo + p - 1) / p * p;
                    if first > start {
                        start = first;
                    }
                    while start < hi {
                        unsafe { flags.set_unchecked(start - lo, true) };
                        start += p;
                    }
                }
                let first_counted = lo.max(2);
                (hi - first_counted) - flags.count_ones()
            }));
        }
        handles
            .into_iter()
            .map(|h| h.join().expect("nsieve: worker thread failed"))
            .collect()
    });
    let count: usize = counts.iter().sum();
    println!("Primes up to {:8} {:8}", n, count);
}

fn main() {
    let n = std::env::args_os()
        .nth(1)
        .and_then(|s| s.into_string().ok())
        .and_then(|n| n.parse().ok())
        .unwrap_or(4);

    for i in 0..3 {
        nsieve(10000 << (n - i));
    }
}

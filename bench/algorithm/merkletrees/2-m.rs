// Parallel (-m) variant of bench/algorithm/merkletrees/1.rs.
// Same algorithm, same output format.
//
// Parallelism: the per-depth loop builds many independent trees
// (iterations = 2^(max_depth - depth + 4)), so its iterations are
// split into contiguous chunks over NumCPU scoped threads. Each
// worker builds its trees with the same Box-per-node Node::create
// and returns a partial sum; the main thread adds the partial sums
// in worker order, so the printed sums are byte-identical to the
// serial version. The single stretch tree and the long-lived tree
// stay serial. If the OS refuses a thread, std's scoped spawn
// panics with a message and a nonzero exit, and no line is printed
// for that depth; a worker panic likewise fails the join and exits
// nonzero.
struct Node {
    hash: Option<i64>,
    value: Option<i64>,
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

impl Node {
    pub fn is_leaf(&self) -> bool {
        self.value.is_some()
    }

    pub fn hash(&self) -> i64 {
        if let Some(h) = self.hash {
            h
        } else {
            -1
        }
    }

    pub fn cal_hash(&mut self) {
        if self.hash.is_none() {
            if self.is_leaf() {
                self.hash = self.value;
            } else {
                if let Some(left) = &mut self.left {
                    if let Some(right) = &mut self.right {
                        left.cal_hash();
                        right.cal_hash();
                        let h = left.hash() + right.hash();
                        self.hash = Some(h)
                    }
                }
            }
        }
    }

    pub fn check(&self) -> bool {
        if self.hash.is_some() {
            if self.is_leaf() {
                true
            } else {
                if let Some(left) = &self.left {
                    if let Some(right) = &self.right {
                        return left.check() && right.check();
                    }
                }
                false
            }
        } else {
            false
        }
    }

    pub fn create<'r>(depth: i32) -> Box<Node> {
        if depth > 0 {
            let d = depth - 1;
            Box::new(Node {
                hash: None,
                value: None,
                left: Some(Self::create(d)),
                right: Some(Self::create(d)),
            })
        } else {
            Box::new(Node {
                hash: None,
                left: None,
                right: None,
                value: Some(1),
            })
        }
    }
}

const MIN_DEPTH: i32 = 4;

fn main() {
    let n = std::env::args_os()
        .nth(1)
        .and_then(|s| s.into_string().ok())
        .and_then(|n| n.parse().ok())
        .unwrap_or(10);

    let max_depth = if MIN_DEPTH + 2 > n { MIN_DEPTH + 2 } else { n };
    {
        let depth = max_depth + 1;
        let mut stretch_tree = Node::create(max_depth + 1);
        stretch_tree.cal_hash();
        println!(
            "stretch tree of depth {}\t root hash: {} check: {}",
            depth,
            stretch_tree.hash(),
            stretch_tree.check()
        );
    }

    let n_cpu = num_cpus::get();
    if n_cpu < 1 {
        eprintln!("merkletrees: cannot determine CPU count");
        std::process::exit(1);
    }

    let mut long_lived_tree = Node::create(max_depth);

    for d in (MIN_DEPTH..max_depth + 1).step_by(2) {
        let iterations = 1 << ((max_depth - d + MIN_DEPTH) as u32);
        let workers = n_cpu.min(iterations as usize);
        let base = iterations / workers as i32;
        let rem = iterations % workers as i32;
        // Scoped threads: each worker builds `count` trees of depth
        // d and returns its partial sum. Every tree of a given depth
        // is identical, so a worker only needs its count.
        let sum = std::thread::scope(|s| {
            let mut handles = Vec::with_capacity(workers);
            for w in 0..workers {
                let count = base + if (w as i32) < rem { 1 } else { 0 };
                handles.push(s.spawn(move || {
                    let mut sum = 0i64;
                    for _i in 0..count {
                        let mut tree = Node::create(d);
                        tree.cal_hash();
                        sum += tree.hash();
                    }
                    sum
                }));
            }
            let mut sum = 0i64;
            for h in handles {
                match h.join() {
                    Ok(partial) => sum += partial,
                    Err(_) => {
                        eprintln!("merkletrees: worker thread failed");
                        std::process::exit(1);
                    }
                }
            }
            sum
        });
        println!(
            "{}\t trees of depth {}\t root hash sum: {}",
            iterations, d, sum
        );
    }

    long_lived_tree.cal_hash();
    println!(
        "long lived tree of depth {}\t root hash: {} check: {}",
        max_depth,
        long_lived_tree.hash(),
        long_lived_tree.check()
    );
}

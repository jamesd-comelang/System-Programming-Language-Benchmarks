// Binary trees, NumCPU-worker parallel version of
// bench/algorithm/binarytrees/6.rs (arena-allocated Rust port of
// bench/algorithm/binarytrees/1.c). Nodes live in one Vec,
// addressed by index (u32::MAX = none); the Vec is cleared between
// trees, so no per-node allocation or refcounting happens.
// Std-only plus rayon — unlike 4.rs, which uses static_rc and
// still allocates per node. Output is identical to 6.rs.
//
// Work is split into chunks of near-equal size and processed by a
// rayon thread pool with num_cpus::get() threads. There are three
// kinds of chunk:
//
//   - the stretch tree (depth max+1, one tree),
//   - the long-lived tree (depth max, one tree),
//   - per depth row, the row's iterations split into 2*nthreads
//     chunks (fewer when a row has fewer iterations).
//
// Every row does the same total work (iterations * 2^(depth+1)
// nodes is constant across rows), so splitting each row into
// 2*nthreads chunks makes every chunk roughly the same size, and
// rayon's work stealing balances the load. Each chunk builds its
// trees in its own Arena, sized for that chunk's tree depth, so no
// state is shared between threads while building trees.
// Checksums are exact integer sums, so adding chunk partials in
// chunk order gives exactly the serial result; par_iter().collect()
// preserves chunk order, and all printing is done by the main
// thread afterwards, in the serial order, so output is
// byte-identical to 6.rs.
//
// Thread count is the machine's CPU count, capped only by the
// number of chunks (a pool thread with no chunk would have nothing
// to do); there is no fixed cap. If the CPU count is 0 or the
// rayon pool fails to build, the program reports it and exits 1
// instead of silently falling back to serial.

use rayon::prelude::*;

struct Node {
    l: u32,
    r: u32,
}

const NONE: u32 = u32::MAX;

struct Arena {
    nodes: Vec<Node>,
}

impl Arena {
    fn new(cap: usize) -> Self {
        Arena {
            nodes: Vec::with_capacity(cap),
        }
    }

    fn new_node(&mut self) -> u32 {
        let idx = self.nodes.len() as u32;
        self.nodes.push(Node { l: NONE, r: NONE });
        idx
    }

    fn bottom_up_tree(&mut self, depth: i32) -> u32 {
        let idx = self.new_node();
        if depth > 0 {
            let l = self.bottom_up_tree(depth - 1);
            let r = self.bottom_up_tree(depth - 1);
            self.nodes[idx as usize].l = l;
            self.nodes[idx as usize].r = r;
        }
        idx
    }

    fn check(&self, idx: u32) -> i32 {
        let node = &self.nodes[idx as usize];
        if node.l == NONE {
            return 1;
        }
        1 + self.check(node.l) + self.check(node.r)
    }

    fn reset(&mut self) {
        self.nodes.clear();
    }
}

const MIN_DEPTH: i32 = 4;

struct Chunk {
    depth: i32, // tree depth for every tree in this chunk
    count: i32, // number of trees to build and check
}

fn run_chunk(c: &Chunk) -> i32 {
    // Largest tree in this chunk has 2^(depth+1)-1 nodes.
    let mut arena = Arena::new(2usize << (c.depth as usize));
    let mut sum = 0;
    for _ in 0..c.count {
        arena.reset();
        let tree = arena.bottom_up_tree(c.depth);
        sum += arena.check(tree);
    }
    sum
}

fn main() {
    let n = std::env::args_os()
        .nth(1)
        .and_then(|s| s.into_string().ok())
        .and_then(|n| n.parse().ok())
        .unwrap_or(10);

    let max_depth = if MIN_DEPTH + 2 > n { MIN_DEPTH + 2 } else { n };
    let stretch_depth = max_depth + 1;

    let n_threads = num_cpus::get();
    if n_threads == 0 {
        eprintln!("binarytrees: could not determine CPU count");
        std::process::exit(1);
    }

    let nrows = ((max_depth - MIN_DEPTH) / 2 + 1) as usize;
    let per_row = 2 * n_threads; // chunks per depth row
    let mut chunks: Vec<Chunk> = Vec::new();
    let mut row_first: Vec<usize> = Vec::with_capacity(nrows + 1);

    chunks.push(Chunk {
        depth: stretch_depth,
        count: 1,
    });
    chunks.push(Chunk {
        depth: max_depth,
        count: 1,
    });
    for r in 0..nrows {
        let d = MIN_DEPTH + 2 * (r as i32);
        let iterations = 1 << ((max_depth - d + MIN_DEPTH) as u32);
        row_first.push(chunks.len());
        let nrow_chunks = iterations.min(per_row as i32);
        let base = iterations / nrow_chunks;
        let rem = iterations % nrow_chunks;
        for j in 0..nrow_chunks {
            let cnt = base + if j < rem { 1 } else { 0 };
            chunks.push(Chunk { depth: d, count: cnt });
        }
    }
    row_first.push(chunks.len());

    let pool = match rayon::ThreadPoolBuilder::new()
        .num_threads(n_threads.min(chunks.len()))
        .build()
    {
        Ok(p) => p,
        Err(e) => {
            eprintln!("binarytrees: rayon thread pool failed: {e}");
            std::process::exit(1);
        }
    };
    // collect() preserves chunk order.
    let results: Vec<i32> = pool.install(|| {
        chunks.par_iter().map(run_chunk).collect()
    });

    println!(
        "stretch tree of depth {}\t check: {}",
        stretch_depth, results[0]
    );

    for r in 0..nrows {
        let d = MIN_DEPTH + 2 * (r as i32);
        let iterations = 1 << ((max_depth - d + MIN_DEPTH) as u32);
        let mut chk = 0;
        for i in row_first[r]..row_first[r + 1] {
            chk += results[i];
        }
        println!("{}\t trees of depth {}\t check: {}", iterations, d, chk)
    }

    println!(
        "long lived tree of depth {}\t check: {}",
        max_depth, results[1]
    );
}

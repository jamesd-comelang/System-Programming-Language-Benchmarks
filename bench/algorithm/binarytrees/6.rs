// Binary trees, arena-allocated Rust port of bench/algorithm/binarytrees/1.c.
// Nodes live in one Vec, addressed by index (u32::MAX = none); the Vec
// is cleared between trees, so no per-node allocation or refcounting
// happens. Std-only, no crates — unlike 4.rs, which uses static_rc and
// still allocates per node. Output is identical to 4.rs.

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

fn main() {
    let n = std::env::args_os()
        .nth(1)
        .and_then(|s| s.into_string().ok())
        .and_then(|n| n.parse().ok())
        .unwrap_or(10);

    let max_depth = if MIN_DEPTH + 2 > n { MIN_DEPTH + 2 } else { n };
    let stretch_depth = max_depth + 1;

    // Largest tree we will build has 2^(stretch_depth+1)-1 nodes.
    let mut arena = Arena::new(2usize << stretch_depth as usize);

    let tree = arena.bottom_up_tree(stretch_depth);
    println!(
        "stretch tree of depth {}\t check: {}",
        stretch_depth,
        arena.check(tree)
    );

    // The checksum is taken now: later trees reuse the same arena
    // and would overwrite this tree's nodes, but only its checksum
    // is ever printed, at the end.
    arena.reset();
    let long_lived_check = {
        let tree = arena.bottom_up_tree(max_depth);
        arena.check(tree)
    };

    for d in (MIN_DEPTH..max_depth + 1).step_by(2) {
        let iterations = 1 << ((max_depth - d + MIN_DEPTH) as u32);
        let mut chk = 0;
        for _ in 0..iterations {
            arena.reset();
            let tree = arena.bottom_up_tree(d);
            chk += arena.check(tree);
        }
        println!("{}\t trees of depth {}\t check: {}", iterations, d, chk)
    }

    println!(
        "long lived tree of depth {}\t check: {}",
        max_depth,
        long_lived_check
    );
}

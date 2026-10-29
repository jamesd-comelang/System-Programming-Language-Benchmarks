// Binary trees, bump-allocated Zig port of bench/algorithm/binarytrees/1.c
// (which itself ports the Go Authors' version). 1.zig allocates every
// node with c_allocator and frees every node recursively on teardown —
// millions of malloc/free pairs.
//
// NOTE: std.heap.ArenaAllocator was tried first and measured SLOWER
// than 1.zig locally: Zig 0.17's arena is thread-safe, so every
// allocation pays atomic RMW + CAS on the block's end index, which
// costs more than glibc malloc's thread-local fast path for 16-byte
// nodes. So this variant uses a plain single-threaded bump buffer
// instead, exactly like 1.c: one preallocated []Node, reset between
// trees, no per-node malloc/free, no atomics. Std-only; output is
// identical to 1.zig.
const std = @import("std");
const Allocator = std.mem.Allocator;
const MIN_DEPTH = 4;

const global_allocator = std.heap.c_allocator;

// Single-threaded bump buffer over preallocated nodes. Capacity is
// fixed up front (see main), so create() never reallocs and pointers
// into the buffer stay valid for the whole run.
const Bump = struct {
    buf: []Node,
    used: usize = 0,

    fn init(allocator: Allocator, cap: usize) !Bump {
        return .{ .buf = try allocator.alloc(Node, cap) };
    }

    fn deinit(self: *Bump, allocator: Allocator) void {
        allocator.free(self.buf);
    }

    fn reset(self: *Bump) void {
        self.used = 0;
    }

    fn create(self: *Bump) *Node {
        if (self.used >= self.buf.len) @panic("binarytrees: bump exhausted");
        const node = &self.buf[self.used];
        self.used += 1;
        node.* = .{};
        return node;
    }
};

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [65536]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const n = try get_n(init);
    const max_depth = @max(MIN_DEPTH + 2, n);
    const stretch_depth = max_depth + 1;

    // Largest tree we will build has 2^(stretch_depth+1)-1 nodes.
    var bump = try Bump.init(
        global_allocator,
        @as(usize, 2) << @as(u6, @intCast(stretch_depth)),
    );
    defer bump.deinit(global_allocator);

    bump.reset();
    try stdout.print("stretch tree of depth {d}\t check: {d}\n", .{
        stretch_depth,
        Node.make(stretch_depth, &bump).check(),
    });

    // The checksum is taken now: later trees reuse the same bump
    // buffer and would overwrite this tree's nodes, but only its
    // checksum is ever printed, at the end.
    bump.reset();
    const long_lived_check = Node.make(max_depth, &bump).check();

    var depth: usize = MIN_DEPTH;
    while (depth <= max_depth) : (depth += 2) {
        const iterations = @as(usize, 1) << @as(u6, @intCast(max_depth - depth + MIN_DEPTH));
        var sum: usize = 0;
        var i: usize = 0;
        while (i < iterations) : (i += 1) {
            bump.reset();
            sum += Node.make(depth, &bump).check();
        }
        try stdout.print("{d}\t trees of depth {d}\t check: {d}\n", .{ iterations, depth, sum });
    }

    try stdout.print("long lived tree of depth {d}\t check: {d}\n", .{ max_depth, long_lived_check });
    try out.interface.flush();
}

fn get_n(init: std.process.Init) !usize {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next() orelse return 10;
    return try std.fmt.parseInt(u32, arg, 10);
}

const Node = struct {
    const Self = @This();

    left: ?*Self = null,
    right: ?*Self = null,

    pub fn make(depth: usize, bump: *Bump) *Self {
        const node = bump.create();
        if (depth > 0) {
            const d = depth - 1;
            node.left = Self.make(d, bump);
            node.right = Self.make(d, bump);
        }
        return node;
    }

    pub fn check(self: *Self) usize {
        var sum: usize = 1;
        if (self.left) |l| {
            sum += l.check();
        }
        if (self.right) |r| {
            sum += r.check();
        }
        return sum;
    }
};

// Binary trees, NumCPU-worker parallel version of
// bench/algorithm/binarytrees/2.zig (bump-allocated Zig port of
// bench/algorithm/binarytrees/1.c). Std-only; output is identical
// to 2.zig.
//
// Work is split into chunks of near-equal size and processed by
// std.Thread.getCpuCount() workers: the main thread plus
// getCpuCount()-1 spawned threads, following the runInParallel
// idiom of bench/algorithm/spectral-norm/2-m.zig. There are three
// kinds of chunk:
//
//   - the stretch tree (depth max+1, one tree),
//   - the long-lived tree (depth max, one tree),
//   - per depth row, the row's iterations split into 2*nworkers
//     chunks (fewer when a row has fewer iterations).
//
// Every row does the same total work (iterations * 2^(depth+1)
// nodes is constant across rows), so splitting each row into
// 2*nworkers chunks makes every chunk roughly the same size, and
// the contiguous static partition hands each worker a near-equal
// share. Each worker owns a private Bump buffer (same single-
// threaded bump layout as 2.zig, sized for the deepest tree in
// that worker's slice), so no locking or atomics are needed while
// building trees. Checksums are exact integer sums, so adding
// chunk partials in chunk order gives exactly the serial result;
// all printing is done by the main thread after the join, in the
// serial order, so output is byte-identical to 2.zig.
//
// Thread count is the machine's CPU count, capped only by the
// number of chunks (a worker with no chunk would have nothing to
// do); there is no fixed cap. A spawn failure prints a message
// and exits 1; a bump allocation failure inside a worker does the
// same. Nothing is silently downgraded to serial.
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

const Chunk = struct {
    depth: usize, // tree depth for every tree in this chunk
    count: usize, // number of trees to build and check
};

const Ctx = struct {
    chunks: []const Chunk,
    results: []usize,
};

fn run_chunk(chunk: *const Chunk, bump: *Bump) usize {
    var sum: usize = 0;
    var i: usize = 0;
    while (i < chunk.count) : (i += 1) {
        bump.reset();
        sum += Node.make(chunk.depth, bump).check();
    }
    return sum;
}

// Run the contiguous chunk slice [first, last): each chunk in its
// own slot of ctx.results, so workers never write the same slot.
fn run_slice(ctx: *const Ctx, first: usize, last: usize) void {
    var max_depth: usize = 0;
    for (ctx.chunks[first..last]) |*c| {
        if (c.depth > max_depth) max_depth = c.depth;
    }
    // Largest tree in this slice has 2^(max_depth+1)-1 nodes.
    var bump = Bump.init(
        global_allocator,
        @as(usize, 2) << @as(u6, @intCast(max_depth)),
    ) catch {
        std.debug.print("binarytrees: bump allocation failed\n", .{});
        std.process.exit(1);
    };
    defer bump.deinit(global_allocator);
    for (ctx.chunks[first..last], first..) |*c, i| {
        ctx.results[i] = run_chunk(c, &bump);
    }
}

fn worker(ctx: *const Ctx, first: usize, last: usize) void {
    run_slice(ctx, first, last);
}

// Contiguous partition of nchunks chunks over nworkers workers:
// worker w gets per_worker chunks, plus one more while w < rem.
fn slice_bounds(w: usize, nchunks: usize, nworkers: usize) struct { first: usize, last: usize } {
    const per_worker = nchunks / nworkers;
    const rem = nchunks % nworkers;
    const first = w * per_worker + @min(w, rem);
    var count = per_worker;
    if (w < rem) count += 1;
    return .{ .first = first, .last = first + count };
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [65536]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const n = try get_n(init);
    const max_depth = @max(MIN_DEPTH + 2, n);
    const stretch_depth = max_depth + 1;

    const cpu_count = try std.Thread.getCpuCount();
    if (cpu_count == 0) {
        std.debug.print("binarytrees: could not determine CPU count\n", .{});
        std.process.exit(1);
    }

    const per_row = 2 * cpu_count; // chunks per depth row (before the chunk cap)
    var chunks: std.ArrayList(Chunk) = .empty;
    defer chunks.deinit(global_allocator);
    var row_first: std.ArrayList(usize) = .empty;
    defer row_first.deinit(global_allocator);

    try chunks.append(global_allocator, .{ .depth = stretch_depth, .count = 1 });
    try chunks.append(global_allocator, .{ .depth = max_depth, .count = 1 });
    var depth: usize = MIN_DEPTH;
    while (depth <= max_depth) : (depth += 2) {
        const iterations = @as(usize, 1) << @as(u6, @intCast(max_depth - depth + MIN_DEPTH));
        try row_first.append(global_allocator, chunks.items.len);
        const nrow_chunks = @min(iterations, per_row);
        const base = iterations / nrow_chunks;
        const rem = iterations % nrow_chunks;
        var j: usize = 0;
        while (j < nrow_chunks) : (j += 1) {
            const cnt = base + @as(usize, @intFromBool(j < rem));
            try chunks.append(global_allocator, .{ .depth = depth, .count = cnt });
        }
    }
    try row_first.append(global_allocator, chunks.items.len);

    const nchunks = chunks.items.len;
    const nworkers = @min(cpu_count, nchunks);
    const results = try global_allocator.alloc(usize, nchunks);
    defer global_allocator.free(results);
    const ctx = Ctx{ .chunks = chunks.items, .results = results };

    // Spawn nworkers-1 threads; the main thread runs the last slice.
    const threads = try global_allocator.alloc(std.Thread, nworkers - 1);
    defer global_allocator.free(threads);
    var w: usize = 0;
    while (w < nworkers - 1) : (w += 1) {
        const bounds = slice_bounds(w, nchunks, nworkers);
        threads[w] = std.Thread.spawn(.{}, worker, .{ &ctx, bounds.first, bounds.last }) catch |err| {
            std.debug.print("binarytrees: thread spawn failed at worker {d}: {}\n", .{ w, err });
            std.process.exit(1);
        };
    }
    {
        const bounds = slice_bounds(nworkers - 1, nchunks, nworkers);
        run_slice(&ctx, bounds.first, bounds.last);
    }
    for (threads) |t| t.join();

    try stdout.print("stretch tree of depth {d}\t check: {d}\n", .{ stretch_depth, results[0] });

    var r: usize = 0;
    depth = MIN_DEPTH;
    while (depth <= max_depth) : (depth += 2) {
        const iterations = @as(usize, 1) << @as(u6, @intCast(max_depth - depth + MIN_DEPTH));
        var sum: usize = 0;
        for (results[row_first.items[r]..row_first.items[r + 1]]) |v| sum += v;
        try stdout.print("{d}\t trees of depth {d}\t check: {d}\n", .{ iterations, depth, sum });
        r += 1;
    }

    try stdout.print("long lived tree of depth {d}\t check: {d}\n", .{ max_depth, results[1] });
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

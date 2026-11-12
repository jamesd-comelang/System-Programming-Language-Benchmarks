const std = @import("std");
const DynamicBitSet = std.bit_set.DynamicBitSet;

const global_allocator = std.heap.c_allocator;

// Parallel (-m) variant of 2.zig: segmented sieve with one OS thread
// per CPU (std.Thread.getCpuCount() workers, no cap, no fallback).
// The base primes up to sqrt(n) are sieved serially (tiny); each
// worker then sieves one contiguous segment [lo, hi) of [0, n) into
// its own DynamicBitSet, marking multiples of every base prime, and
// counts the primes in its segment. Counts are summed in fixed worker
// order, so the total -- and the printed output -- is identical to
// 2.zig.
//
// Marking starts at max(p*p, first multiple of p in the segment): every
// multiple of p below p*p already has a smaller prime factor, and p
// itself is never marked, exactly as in the serial loop (which marks
// from 2p and therefore never marks p). Every composite below n has a
// prime factor <= sqrt(n), so every composite in a segment is marked.
//
// Counting uses the bit set directly: bits for global 0 and 1 are never
// set (marking starts at p*p >= 4), so the number of primes in a
// segment is (hi - max(lo, 2)) minus the number of set bits in the
// segment's bit set. On thread spawn failure the program reports the
// failing worker and exits nonzero without printing a result for the
// current call; if the CPU count cannot be determined it likewise
// reports the error and exits instead of silently running serially.

fn isqrt(n: usize) usize {
    var lim: usize = @intFromFloat(@sqrt(@as(f64, @floatFromInt(n))));
    while ((lim + 1) * (lim + 1) <= n) lim += 1;
    while (lim * lim > n) lim -= 1;
    return lim;
}

fn nsieve(n: usize, stdout: *std.Io.Writer) !void {
    const lim = isqrt(n);
    var is_comp = try global_allocator.alloc(bool, lim + 1);
    defer global_allocator.free(is_comp);
    @memset(is_comp, false);
    // Base primes, kept in a small stack-friendly bound: there are
    // fewer than lim of them, so a heap slice of lim + 1 is ample.
    var primes = try global_allocator.alloc(usize, lim + 1);
    defer global_allocator.free(primes);
    var nprimes: usize = 0;
    var i: usize = 2;
    while (i <= lim) : (i += 1) {
        if (!is_comp[i]) {
            primes[nprimes] = i;
            nprimes += 1;
            if (i * i <= lim) {
                var j: usize = i * i;
                while (j <= lim) : (j += i) {
                    is_comp[j] = true;
                }
            }
        }
    }

    const workers = std.Thread.getCpuCount() catch |err| {
        std.debug.print("nsieve: could not determine CPU count: {}\n", .{err});
        std.process.exit(1);
    };
    const chunk: usize = @max((n + workers - 1) / workers, 1);
    const counts = try global_allocator.alloc(usize, workers);
    defer global_allocator.free(counts);
    @memset(counts, 0);
    var threads = try global_allocator.alloc(std.Thread, workers);
    defer global_allocator.free(threads);
    const ctx = Ctx{ .primes = primes[0..nprimes], .n = n, .chunk = chunk, .counts = counts };
    var w: usize = 0;
    while (w < workers) : (w += 1) {
        threads[w] = std.Thread.spawn(.{}, seg_worker, .{ &ctx, w }) catch |err| {
            std.debug.print("nsieve: thread spawn failed at worker {d}: {}\n", .{ w, err });
            std.process.exit(1);
        };
    }
    for (threads) |t| t.join();

    var count: usize = 0;
    for (counts) |c| count += c;
    try stdout.print("Primes up to {d:8} {d:8}\n", .{ n, count });
}

const Ctx = struct { primes: []usize, n: usize, chunk: usize, counts: []usize };

fn seg_worker(ctx: *const Ctx, w: usize) void {
    const lo = w * ctx.chunk;
    if (lo >= ctx.n) return;
    const hi = @min(lo + ctx.chunk, ctx.n);
    const len = hi - lo;
    var flags = DynamicBitSet.initEmpty(global_allocator, len) catch {
        // Allocation failure inside a worker: aborting is the only
        // report available here; the process exits nonzero with no
        // result printed. (An alloc failure also aborts via the
        // allocator in practice; this keeps the failure loud.)
        std.debug.print("nsieve: worker {d} could not allocate its segment\n", .{w});
        std.process.exit(1);
    };
    defer flags.deinit();
    for (ctx.primes) |p| {
        var start = p * p;
        const first = (lo + p - 1) / p * p;
        if (first > start) start = first;
        var j: usize = start;
        while (j < hi) : (j += p) {
            flags.set(j - lo);
        }
    }
    const first_counted = @max(lo, 2);
    ctx.counts[w] = (hi - first_counted) - flags.count();
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const n = try get_n(init);
    var i: u6 = 0;
    while (i < 3) : (i += 1) {
        const base: usize = 10000;
        try nsieve(base << (n - i), &out.interface);
    }
    try out.interface.flush();
}

fn get_n(init: std.process.Init) !u6 {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next();
    if (arg) |a| {
        return try std.fmt.parseInt(u6, a, 10);
    } else {
        return 4;
    }
}

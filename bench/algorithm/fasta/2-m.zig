// Fasta, parallel (-m) version of bench/algorithm/fasta/1.zig.
//
// NumCPU-worker port: the whole output is built in one preallocated
// buffer, split into line-aligned chunks (multiples of 60 chars, so
// each chunk is a whole number of lines, plus the section's final
// partial line), one chunk per worker per section. Each worker writes
// only its own byte ranges, so the assembled output is in order by
// construction and no locking is needed; main writes the buffer once
// after joining all workers.
//
// The LCG is sequential by definition, so a worker starting mid-stream
// cannot just resume the global state: its starting state is computed
// by fast-forwarding the affine map x -> (ia*x + ic) % im with binary
// exponentiation (exact integer arithmetic, all products < im^2), which
// reproduces the serial draw sequence bit-for-bit. The repeat section
// is positional (char j is sequence[j % len]), so its chunks need no
// RNG state at all. The per-character random draw and the cumulative
// probability selection loop are unchanged from 1.zig, including its
// `n <= r` counting form.
//
// Thread count is std.Thread.getCpuCount(). A spawn failure is reported
// on stderr and exits nonzero before anything is printed.

const std = @import("std");

const max_line_length = 60;

const im = 139968;
const ia = 3877;
const ic = 29573;

const global_allocator = std.heap.c_allocator;

const AminoAcid = struct {
    l: u8,
    p: f64,
};

// LCG state after `steps` draws starting from `seed`: the one-step map
// x -> (ia*x + ic) % im is affine, and affine maps compose, so the
// steps-th iterate is built by binary exponentiation.
fn lcgJump(seed: u32, steps: u64) u32 {
    var ra: u64 = 1; // result map x -> ra*x + rb
    var rb: u64 = 0;
    var ba: u64 = ia; // base map: one LCG step
    var bb: u64 = ic;
    var s = steps;
    while (s > 0) {
        if (s & 1 != 0) {
            // res = base after res
            const na = (ba * ra) % im;
            const nb = (ba * rb + bb) % im;
            ra = na;
            rb = nb;
        }
        // base = base after base
        const na = (ba * ba) % im;
        const nb = (ba * bb + bb) % im;
        ba = na;
        bb = nb;
        s >>= 1;
    }
    return @intCast((ra * seed + rb) % im);
}

fn nextRandom(seed: *u32, max: f64) f64 {
    seed.* = (seed.* * ia + ic) % im;
    return max * @as(f64, @floatFromInt(seed.*)) / @as(f64, @floatFromInt(im));
}

// Cumulative probabilities (times im), exactly as 1.zig computes them
// inside generateAndWrap, hoisted here so workers can share them.
fn cumulativeProbs(comptime nucleotides: []const AminoAcid) [nucleotides.len]f64 {
    var cum_prob: f64 = 0;
    var total: [nucleotides.len]f64 = undefined;
    for (nucleotides, 0..) |n, i| {
        cum_prob += n.p;
        total[i] = cum_prob * im;
    }
    return total;
}

const ChunkKind = enum { repeat, random };

const Chunk = struct {
    dst: []u8, // output bytes of this chunk
    start: usize, // first char index within the section
    len: usize, // chars in this chunk (0 = empty)
    seed: u32, // LCG state before the first char (random only)
    kind: ChunkKind,
    cum_prob_total: []const f64, // random only
    letters: []const u8, // random only
    sequence: []const u8, // repeat only
};

fn fillChunk(c: *const Chunk) void {
    if (c.len == 0) return;
    var p: usize = 0;
    switch (c.kind) {
        .repeat => {
            var k: usize = 0;
            while (k < c.len) : (k += 1) {
                c.dst[p] = c.sequence[(c.start + k) % c.sequence.len];
                p += 1;
                if ((k + 1) % max_line_length == 0) {
                    c.dst[p] = '\n';
                    p += 1;
                }
            }
            if (c.len % max_line_length != 0) {
                c.dst[p] = '\n';
                p += 1;
            }
        },
        .random => {
            var seed = c.seed;
            var k: usize = 0;
            while (k < c.len) : (k += 1) {
                const r = nextRandom(&seed, im);
                var idx: usize = 0;
                for (c.cum_prob_total) |n| {
                    if (n <= r) {
                        idx += 1;
                    }
                }
                c.dst[p] = c.letters[idx];
                p += 1;
                if ((k + 1) % max_line_length == 0) {
                    c.dst[p] = '\n';
                    p += 1;
                }
            }
            if (c.len % max_line_length != 0) {
                c.dst[p] = '\n';
                p += 1;
            }
        },
    }
}

// Line-aligned split of a section of `count` chars among `w` workers:
// worker i gets whole lines only, so its chunk is a whole number of
// lines (plus the section's final partial line for the last worker).
// Returns char start, char len, and byte offset from the section start.
fn partition(count: usize, w: usize, i: usize) struct { start: usize, len: usize, out_off: usize } {
    const lines = (count + max_line_length - 1) / max_line_length;
    const per = lines / w;
    const rem = lines % w;
    const start_line = i * per + @min(i, rem);
    const lines_i = per + @intFromBool(i < rem);
    const s = start_line * max_line_length;
    if (s >= count) {
        return .{ .start = count, .len = 0, .out_off = s + start_line };
    }
    const l = @min(lines_i * max_line_length, count - s);
    return .{ .start = s, .len = l, .out_off = s + start_line };
}

fn sectionBytes(count: usize) usize {
    return count + (count + max_line_length - 1) / max_line_length;
}

const Ctx = struct {
    chunks: []Chunk,
    workers: usize,
};

fn worker(ctx: *const Ctx, idx: usize) void {
    for (0..3) |k| {
        fillChunk(&ctx.chunks[idx * 3 + k]);
    }
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [65536]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const n = try get_n(init);
    const homo_sapiens_alu = "GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGGGAGGCCGAGGCGGGCGGATCACCTGAGGTC" ++
        "AGGAGTTCGAGACCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAATACAAAAATTAGCCGGGCG" ++
        "TGGTGGCGCGCGCCTGTAATCCCAGCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGGAGGCGG" ++
        "AGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCCAGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA";

    const iub_nucleotide_info = &[_]AminoAcid{
        AminoAcid{ .l = 'a', .p = 0.27 },
        AminoAcid{ .l = 'c', .p = 0.12 },
        AminoAcid{ .l = 'g', .p = 0.12 },
        AminoAcid{ .l = 't', .p = 0.27 },
        AminoAcid{ .l = 'B', .p = 0.02 },
        AminoAcid{ .l = 'D', .p = 0.02 },
        AminoAcid{ .l = 'H', .p = 0.02 },
        AminoAcid{ .l = 'K', .p = 0.02 },
        AminoAcid{ .l = 'M', .p = 0.02 },
        AminoAcid{ .l = 'N', .p = 0.02 },
        AminoAcid{ .l = 'R', .p = 0.02 },
        AminoAcid{ .l = 'S', .p = 0.02 },
        AminoAcid{ .l = 'V', .p = 0.02 },
        AminoAcid{ .l = 'W', .p = 0.02 },
        AminoAcid{ .l = 'Y', .p = 0.02 },
    };
    const homo_sapien_nucleotide_info = &[_]AminoAcid{
        AminoAcid{ .l = 'a', .p = 0.3029549426680 },
        AminoAcid{ .l = 'c', .p = 0.1979883004921 },
        AminoAcid{ .l = 'g', .p = 0.1975473066391 },
        AminoAcid{ .l = 't', .p = 0.3015094502008 },
    };
    const iub_cum = cumulativeProbs(iub_nucleotide_info);
    const homo_cum = cumulativeProbs(homo_sapien_nucleotide_info);
    const iub_letters = "acgtBDHKMNRSVWY";
    const homo_letters = "acgt";

    const h1 = ">ONE Homo sapiens alu\n";
    const h2 = ">TWO IUB ambiguity codes\n";
    const h3 = ">THREE Homo sapiens frequency\n";
    const c1 = 2 * n;
    const c2 = 3 * n;
    const c3 = 5 * n;
    const b1 = sectionBytes(c1);
    const b2 = sectionBytes(c2);
    const b3 = sectionBytes(c3);
    const total = h1.len + b1 + h2.len + b2 + h3.len + b3;

    const task_count = try std.Thread.getCpuCount();

    const buf = try global_allocator.alloc(u8, total);
    defer global_allocator.free(buf);
    @memcpy(buf[0..h1.len], h1);
    @memcpy(buf[h1.len + b1 .. h1.len + b1 + h2.len], h2);
    @memcpy(buf[h1.len + b1 + h2.len + b2 .. h1.len + b1 + h2.len + b2 + h3.len], h3);

    // The homosapiens section starts where the iub LCG left off.
    const seed_homo = lcgJump(42, c2);

    const chunks = try global_allocator.alloc(Chunk, 3 * task_count);
    defer global_allocator.free(chunks);
    const o1 = h1.len;
    const o2 = o1 + b1 + h2.len;
    const o3 = o2 + b2 + h3.len;
    for (0..task_count) |i| {
        const r = partition(c1, task_count, i);
        chunks[i * 3 + 0] = .{
            .dst = buf[o1 + r.out_off .. o1 + r.out_off + r.len + (r.len + max_line_length - 1) / max_line_length],
            .start = r.start,
            .len = r.len,
            .seed = 0,
            .kind = .repeat,
            .cum_prob_total = &.{},
            .letters = &.{},
            .sequence = homo_sapiens_alu,
        };
        const u = partition(c2, task_count, i);
        chunks[i * 3 + 1] = .{
            .dst = buf[o2 + u.out_off .. o2 + u.out_off + u.len + (u.len + max_line_length - 1) / max_line_length],
            .start = u.start,
            .len = u.len,
            .seed = lcgJump(42, u.start),
            .kind = .random,
            .cum_prob_total = &iub_cum,
            .letters = iub_letters,
            .sequence = &.{},
        };
        const v = partition(c3, task_count, i);
        chunks[i * 3 + 2] = .{
            .dst = buf[o3 + v.out_off .. o3 + v.out_off + v.len + (v.len + max_line_length - 1) / max_line_length],
            .start = v.start,
            .len = v.len,
            .seed = lcgJump(seed_homo, v.start),
            .kind = .random,
            .cum_prob_total = &homo_cum,
            .letters = homo_letters,
            .sequence = &.{},
        };
    }

    const ctx = Ctx{ .chunks = chunks, .workers = task_count };
    const threads = try global_allocator.alloc(std.Thread, task_count);
    defer global_allocator.free(threads);
    for (0..task_count) |i| {
        threads[i] = std.Thread.spawn(.{}, worker, .{ &ctx, i }) catch |err| {
            std.debug.print("fasta: thread spawn failed at worker {d}: {}\n", .{ i, err });
            std.process.exit(1);
        };
    }
    for (threads) |t| t.join();

    _ = try stdout.write(buf);
    try out.interface.flush();
}

fn get_n(init: std.process.Init) !usize {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next() orelse return 10;
    return try std.fmt.parseInt(u64, arg, 10);
}

// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Parallel (-m) variant of bench/algorithm/regex-redux/1.zig, using
// the same small matcher (literals, escapes, alternation, character
// classes, * and + on a single atom; leftmost, non-overlapping,
// alternatives in order, greedy quantifiers with backtracking).
// Parallelized with NumCPU OS threads: one worker per CPU, no
// one-thread-per-row. The pipeline is split into phases, each phase
// spread over the workers; the phases run in order, so the program
// uses exactly NumCPU threads at any time.
//
//   - clean: serial (one streaming pass over the input).
//   - variant counts: the input is split into NumCPU contiguous
//     chunks. Each worker scans its chunk for all 9 variants and
//     records every occurrence (start, end) per variant -- every
//     position where the matcher matches, including positions inside
//     a match the serial scan would have skipped over. The main
//     thread then replays each variant's occurrences in global
//     position order with the same skip-ahead rule as the serial
//     count (a match at p is counted only if p >= next, then
//     next = end). Recording every occurrence first (instead of
//     counting per chunk) is what makes the chunking exact: a match
//     in one chunk can change which matches count in the next, and
//     the replay sees the occurrences in exactly the serial order.
//   - each substitution: the same record-then-replay shape. Each
//     worker scans its chunk and records every occurrence (start,
//     end) of that substitution's pattern. The main thread replays
//     the occurrences in order with the serial leftmost,
//     non-overlapping rule (accept if start >= next, then
//     next = end), which reproduces the serial replace-all exactly.
//     The accepted matches are then split by count over the workers
//     and each worker emits its slice directly (gap copies +
//     replacement), so no pass re-runs the matcher to emit. A worker
//     whose slice is empty copies only the gap its span owns (spans
//     are delimited by match starts, so they tile the input
//     exactly), so concatenating the workers' outputs in order gives
//     exactly the serial output.
//
// Each worker parses its own copy of the patterns with its own
// arena (the main arena is not thread-safe) and reports allocation
// failures back to the main thread, which exits nonzero. A thread
// spawn failure is reported and exits nonzero, as is a failure to
// determine the CPU count. No silent thread-count caps or fallbacks.
//
// Output is byte-identical to 1.zig (verified against 25000_out,
// 250000_out, and the serial binary on 2500000_in).
//
// Usage: ./app <input-file>   (default "25000_in", as in 1.zig)
// Output format is exactly 1.zig's: one "<pattern> <count>" line per
// variant, then a blank line, then input length, cleaned length, and
// final length.

const std = @import("std");

const Atom = struct {
    // A single atom: either a literal byte, or a character class.
    // For a class, `set` holds its bytes and `negated` inverts the test.
    // `quant` is 0 = exactly one, 1 = *, 2 = +.
    lit: u8 = 0,
    set: []const u8 = &.{},
    negated: bool = false,
    is_class: bool = false,
    quant: u2 = 0,

    fn matches(self: Atom, b: u8) bool {
        if (!self.is_class) return b == self.lit;
        var in_set = false;
        for (self.set) |c| {
            if (c == b) {
                in_set = true;
                break;
            }
        }
        return if (self.negated) !in_set else in_set;
    }
};

const Alt = []const Atom;

// Parse one pattern string into alternatives. Parsed once at startup;
// the pattern strings are comptime constants, so the allocations below
// come from the arena and are never freed.
fn parsePattern(alloc: std.mem.Allocator, pattern: []const u8) ![]Alt {
    var alts: std.ArrayList(Alt) = .empty;
    var atoms: std.ArrayList(Atom) = .empty;
    var i: usize = 0;
    while (i < pattern.len) {
        const c = pattern[i];
        if (c == '|') {
            try alts.append(alloc, try atoms.toOwnedSlice(alloc));
            atoms = .empty;
            i += 1;
            continue;
        }
        var atom = Atom{};
        if (c == '[') {
            i += 1;
            var negated = false;
            if (i < pattern.len and pattern[i] == '^') {
                negated = true;
                i += 1;
            }
            const start = i;
            while (i < pattern.len and pattern[i] != ']') i += 1;
            atom = Atom{ .is_class = true, .set = pattern[start..i], .negated = negated };
            i += 1; // skip ']'
        } else if (c == '\\') {
            i += 1;
            atom = Atom{ .lit = pattern[i] };
            i += 1;
        } else {
            atom = Atom{ .lit = c };
            i += 1;
        }
        if (i < pattern.len and (pattern[i] == '*' or pattern[i] == '+')) {
            atom.quant = if (pattern[i] == '*') 1 else 2;
            i += 1;
        }
        try atoms.append(alloc, atom);
    }
    try alts.append(alloc, try atoms.toOwnedSlice(alloc));
    return alts.toOwnedSlice(alloc);
}

// Match one alternative starting at `pos`. Returns the end position of
// the match, or null. Greedy quantifiers try the longest repetition
// first and backtrack, so a following literal still gets its chance.
fn matchFrom(alt: Alt, ai: usize, text: []const u8, pos: usize) ?usize {
    if (ai >= alt.len) return pos;
    const atom = alt[ai];
    switch (atom.quant) {
        0 => {
            if (pos < text.len and atom.matches(text[pos])) {
                return matchFrom(alt, ai + 1, text, pos + 1);
            }
            return null;
        },
        1, 2 => {
            var max: usize = 0;
            while (pos + max < text.len and atom.matches(text[pos + max])) max += 1;
            var count: usize = max;
            while (true) {
                if (atom.quant == 2 and count == 0) break;
                if (matchFrom(alt, ai + 1, text, pos + count)) |end| return end;
                if (count == 0) break;
                count -= 1;
            }
            return null;
        },
        else => unreachable,
    }
}

// Try each alternative at `pos`, in order. Returns the match end, or null.
fn matchAt(alts: []Alt, text: []const u8, pos: usize) ?usize {
    for (alts) |alt| {
        if (matchFrom(alt, 0, text, pos)) |end| return end;
    }
    return null;
}

const Variant = struct { pattern: []const u8 };
const Subst = struct { pattern: []const u8, replacement: []const u8 };

const variant_patterns = [_][]const u8{
    "agggtaaa|tttaccct",
    "[cgt]gggtaaa|tttaccc[acg]",
    "a[act]ggtaaa|tttacc[agt]t",
    "ag[act]gtaaa|tttac[agt]ct",
    "agg[act]taaa|ttta[agt]cct",
    "aggg[acg]aaa|ttt[cgt]ccct",
    "agggt[cgt]aa|tt[acg]accct",
    "agggta[cgt]a|t[acg]taccct",
    "agggtaa[cgt]|[acg]ttaccct",
};

const subst_patterns = [_]Subst{
    .{ .pattern = "tHa[Nt]", .replacement = "<4>" },
    .{ .pattern = "aND|caN|Ha[DS]|WaS", .replacement = "<3>" },
    .{ .pattern = "a[NSt]|BY", .replacement = "<2>" },
    .{ .pattern = "<[^>]*>", .replacement = "|" },
    .{ .pattern = "\\|[^|][^|]*\\|", .replacement = "-" },
};

// ---- parallel scaffolding ----

const Occ = struct { start: usize, end: usize };

// Chunk [s, e) of [0, len) for worker w of n (contiguous, covering).
fn chunkRange(len: usize, w: usize, n: usize) struct { s: usize, e: usize } {
    return .{ .s = w * len / n, .e = (w + 1) * len / n };
}

fn fatal(comptime fmt: []const u8, args: anytype) noreturn {
    std.debug.print("regex-redux: " ++ fmt ++ "\n", args);
    std.process.exit(1);
}

// ---- variant counts ----

const CountCtx = struct {
    seq: []const u8,
    start: usize,
    end: usize,
    // Parsed variant patterns (9), owned by this worker's arena.
    alts: [9][]Alt = undefined,
    // Occurrence (start, end) per variant, in ascending start order.
    occs: [9]std.ArrayList(Occ),
    alloc: std.mem.Allocator,
    err: ?anyerror = null,

    fn run(ctx: *CountCtx) void {
        ctx.runErr() catch |err| {
            ctx.err = err;
        };
    }

    fn runErr(ctx: *CountCtx) !void {
        for (ctx.alts, 0..) |alts, v| {
            var pos = ctx.start;
            while (pos < ctx.end) : (pos += 1) {
                if (matchAt(alts, ctx.seq, pos)) |end| {
                    try ctx.occs[v].append(ctx.alloc, .{ .start = pos, .end = end });
                }
            }
        }
    }
};

// ---- substitutions ----

const ScanCtx = struct {
    in: []const u8,
    start: usize,
    end: usize,
    alts: []Alt,
    occs: std.ArrayList(Occ) = .empty,
    alloc: std.mem.Allocator,
    err: ?anyerror = null,

    fn run(ctx: *ScanCtx) void {
        ctx.runErr() catch |err| {
            ctx.err = err;
        };
    }

    fn runErr(ctx: *ScanCtx) !void {
        var pos = ctx.start;
        while (pos < ctx.end) : (pos += 1) {
            if (matchAt(ctx.alts, ctx.in, pos)) |end| {
                try ctx.occs.append(ctx.alloc, .{ .start = pos, .end = end });
            }
        }
    }
};

const EmitCtx = struct {
    in: []const u8,
    acc: []const Occ, // accepted matches, global order
    lo: usize,
    hi: usize,
    in_len: usize,
    rep: []const u8,
    out: std.ArrayList(u8) = .empty,
    alloc: std.mem.Allocator,
    err: ?anyerror = null,

    fn run(ctx: *EmitCtx) void {
        ctx.runErr() catch |err| {
            ctx.err = err;
        };
    }

    // Emit the output for accepted matches [lo, hi). This worker's
    // input span runs from the start of its first accepted match (or
    // 0) to the start of the next worker's first accepted match (or
    // in_len): the spans are delimited by match starts, so they tile
    // [0, in_len) exactly, and the gap before a match is copied by
    // the worker whose span ends at that match's start. Matches
    // outside the slice cannot start inside this span (the replay
    // accepted the first occurrence at or after `next`, so no
    // occurrence starts in a gap), so gap copies + replacements here
    // are exactly the serial output for this span, including the
    // empty-slice case.
    fn runErr(ctx: *EmitCtx) !void {
        const in_start: usize = if (ctx.lo == 0) 0 else ctx.acc[ctx.lo].start;
        const in_end: usize = if (ctx.hi >= ctx.acc.len) ctx.in_len else ctx.acc[ctx.hi].start;
        var prev = in_start;
        for (ctx.acc[ctx.lo..ctx.hi]) |o| {
            try ctx.out.appendSlice(ctx.alloc, ctx.in[prev..o.start]);
            try ctx.out.appendSlice(ctx.alloc, ctx.rep);
            prev = o.end;
        }
        try ctx.out.appendSlice(ctx.alloc, ctx.in[prev..in_end]);
    }
};

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const alloc = init.arena.allocator();

    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const file_name = arg_it.next() orelse "25000_in";

    const input = try std.Io.Dir.cwd().readFileAlloc(io, file_name, alloc, .unlimited);
    const input_len = input.len;

    // Clean: delete FASTA headers (">...<newline>") and all newlines.
    // This is the ">[^\n]*\n|\n" replacement of 7-m.rs, done here with a
    // direct scan (equivalent, and cheaper than running the matcher).
    var sequence: std.ArrayList(u8) = .empty;
    {
        var i: usize = 0;
        while (i < input.len) {
            if (input[i] == '>') {
                // Skip to (and including) the next newline, if there is one;
                // a '>' with no newline after it is kept, as in the regex.
                var j = i;
                while (j < input.len and input[j] != '\n') j += 1;
                if (j < input.len) {
                    i = j + 1;
                } else {
                    try sequence.append(alloc, input[i]);
                    i += 1;
                }
            } else if (input[i] == '\n') {
                i += 1;
            } else {
                try sequence.append(alloc, input[i]);
                i += 1;
            }
        }
    }
    const seq = sequence.items;
    const sequence_len = seq.len;

    const nworkers = std.Thread.getCpuCount() catch {
        fatal("cannot determine CPU count", .{});
    };
    if (nworkers == 0) fatal("CPU count is 0", .{});

    var out_buf: [65536]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);

    // Variant counts, chunked over the workers: worker w scans chunk
    // w for all 9 variants; the replay below combines the chunks.
    var counts: [9]usize = .{ 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    {
        const ctxs = try alloc.alloc(CountCtx, nworkers);
        const arenas = try alloc.alloc(std.heap.ArenaAllocator, nworkers);
        for (ctxs, arenas, 0..) |*ctx, *arena, w| {
            arena.* = std.heap.ArenaAllocator.init(std.heap.page_allocator);
            const walloc = arena.allocator();
            const r = chunkRange(seq.len, w, nworkers);
            ctx.* = .{ .seq = seq, .start = r.s, .end = r.e, .alloc = walloc, .occs = undefined };
            for (&ctx.occs) |*list| list.* = .empty;
            for (variant_patterns, 0..) |pattern, v| {
                ctx.alts[v] = try parsePattern(walloc, pattern);
            }
        }
        const threads = try alloc.alloc(std.Thread, nworkers);
        for (ctxs, threads, 0..) |*ctx, *t, w| {
            t.* = std.Thread.spawn(.{}, CountCtx.run, .{ctx}) catch {
                fatal("thread spawn failed at worker {d}", .{w});
            };
        }
        for (threads) |t| t.join();
        for (ctxs, 0..) |*ctx, w| {
            if (ctx.err) |err| fatal("count worker {d} failed: {}", .{ w, err });
        }
        // Replay per variant, in global position order.
        for (0..9) |v| {
            var next: usize = 0;
            for (ctxs) |*ctx| {
                for (ctx.occs[v].items) |o| {
                    if (o.start >= next) {
                        counts[v] += 1;
                        next = o.end;
                    }
                }
            }
        }
        for (arenas) |*arena| arena.deinit();
    }
    for (variant_patterns, counts) |pattern, count| {
        try out.interface.print("{s} {d}\n", .{ pattern, count });
    }

    // Substitutions, applied one at a time, in order, as in 1.zig.
    // Each pass is parallelized: scan (record occurrences), replay
    // (accept leftmost non-overlapping), emit (direct gap copies).
    var current: []u8 = seq;
    for (subst_patterns) |subst| {
        const in = current;
        const in_len = in.len;

        // Scan.
        const scan_ctxs = try alloc.alloc(ScanCtx, nworkers);
        const scan_arenas = try alloc.alloc(std.heap.ArenaAllocator, nworkers);
        for (scan_ctxs, scan_arenas, 0..) |*ctx, *arena, w| {
            arena.* = std.heap.ArenaAllocator.init(std.heap.page_allocator);
            const walloc = arena.allocator();
            const r = chunkRange(in_len, w, nworkers);
            const alts = try parsePattern(walloc, subst.pattern);
            ctx.* = .{ .in = in, .start = r.s, .end = r.e, .alts = alts, .alloc = walloc };
        }
        const scan_threads = try alloc.alloc(std.Thread, nworkers);
        for (scan_ctxs, scan_threads, 0..) |*ctx, *t, w| {
            t.* = std.Thread.spawn(.{}, ScanCtx.run, .{ctx}) catch {
                fatal("thread spawn failed at worker {d}", .{w});
            };
        }
        for (scan_threads) |t| t.join();
        for (scan_ctxs, 0..) |*ctx, w| {
            if (ctx.err) |err| fatal("scan worker {d} failed: {}", .{ w, err });
        }

        // Replay in global order: accept the first occurrence at or
        // after `next`, exactly the serial leftmost, non-overlapping rule.
        var acc: std.ArrayList(Occ) = .empty;
        var next: usize = 0;
        for (scan_ctxs) |*ctx| {
            for (ctx.occs.items) |o| {
                if (o.start >= next) {
                    try acc.append(alloc, o);
                    next = o.end;
                }
            }
        }
        for (scan_arenas) |*arena| arena.deinit();

        if (acc.items.len == 0) {
            // No matches: the output is the input, unchanged.
            continue;
        }

        // Emit: split the accepted matches by count over the workers.
        const nacc = acc.items.len;
        const emit_ctxs = try alloc.alloc(EmitCtx, nworkers);
        const emit_arenas = try alloc.alloc(std.heap.ArenaAllocator, nworkers);
        for (emit_ctxs, emit_arenas, 0..) |*ctx, *arena, w| {
            arena.* = std.heap.ArenaAllocator.init(std.heap.page_allocator);
            const walloc = arena.allocator();
            ctx.* = .{
                .in = in,
                .acc = acc.items,
                .lo = w * nacc / nworkers,
                .hi = (w + 1) * nacc / nworkers,
                .in_len = in_len,
                .rep = subst.replacement,
                .alloc = walloc,
            };
        }
        const emit_threads = try alloc.alloc(std.Thread, nworkers);
        for (emit_ctxs, emit_threads, 0..) |*ctx, *t, w| {
            t.* = std.Thread.spawn(.{}, EmitCtx.run, .{ctx}) catch {
                fatal("thread spawn failed at worker {d}", .{w});
            };
        }
        for (emit_threads) |t| t.join();
        for (emit_ctxs, 0..) |*ctx, w| {
            if (ctx.err) |err| fatal("emit worker {d} failed: {}", .{ w, err });
        }
        var next_buf: std.ArrayList(u8) = .empty;
        try next_buf.ensureTotalCapacityPrecise(alloc, in_len);
        for (emit_ctxs) |*ctx| {
            try next_buf.appendSlice(alloc, ctx.out.items);
        }
        for (emit_arenas) |*arena| arena.deinit();
        current = try next_buf.toOwnedSlice(alloc);
    }

    try out.interface.print("\n{d}\n{d}\n{d}\n", .{ input_len, sequence_len, current.len });
    try out.interface.flush();
}

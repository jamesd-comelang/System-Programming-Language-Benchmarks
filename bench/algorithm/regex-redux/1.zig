// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// Zig port of 7.rs (and 3.go, same program). Zig's standard library has no
// regex engine, so this file implements a small matcher that is sufficient
// for exactly the fixed pattern set this problem uses -- no general-purpose
// regex engine is attempted. Supported pattern features: literals, escapes
// (e.g. \|), alternation (|), character classes ([abc], [^abc]), and the *
// and + quantifiers on a single atom. Matching is leftmost, non-overlapping,
// alternatives tried in order, quantifiers greedy with backtracking -- which
// reproduces the regex crate's behaviour on these patterns (their
// alternatives are disjoint at the first differing character, and every
// quantified class is bounded by a following literal that cannot occur
// inside the class, so the match extent is unambiguous).
//
// Usage: ./app <input-file>   (default "25000_in", as in 7.rs)
// Output format is exactly 7.rs's: one "<pattern> <count>" line per
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

// Count non-overlapping matches of a pattern in `text`.
fn countMatches(alts: []Alt, text: []const u8) usize {
    var count: usize = 0;
    var pos: usize = 0;
    while (pos < text.len) {
        if (matchAt(alts, text, pos)) |end| {
            count += 1;
            pos = end;
        } else {
            pos += 1;
        }
    }
    return count;
}

// Replace all non-overlapping matches of a pattern with `replacement`.
fn replaceAll(alloc: std.mem.Allocator, alts: []Alt, text: []const u8, replacement: []const u8) ![]u8 {
    var out: std.ArrayList(u8) = .empty;
    var last: usize = 0;
    var pos: usize = 0;
    while (pos < text.len) {
        if (matchAt(alts, text, pos)) |end| {
            try out.appendSlice(alloc, text[last..pos]);
            try out.appendSlice(alloc, replacement);
            last = end;
            pos = end;
        } else {
            pos += 1;
        }
    }
    try out.appendSlice(alloc, text[last..]);
    return out.toOwnedSlice(alloc);
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

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const alloc = init.arena.allocator();

    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const file_name = arg_it.next() orelse "25000_in";

    const input = try std.Io.Dir.cwd().readFileAlloc(io, file_name, alloc, .unlimited);
    const input_len = input.len;

    // Clean: delete FASTA headers (">...<newline>") and all newlines.
    // This is the ">[^\n]*\n|\n" replacement of 7.rs, done here with a
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

    var out_buf: [65536]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);

    // Variant counts, in the same order and format as 7.rs.
    for (variant_patterns) |pattern| {
        const alts = try parsePattern(alloc, pattern);
        const count = countMatches(alts, seq);
        try out.interface.print("{s} {d}\n", .{ pattern, count });
    }

    // Substitutions, applied one at a time, in order, as in 7.rs.
    var current: []u8 = seq;
    for (subst_patterns) |subst| {
        const alts = try parsePattern(alloc, subst.pattern);
        current = try replaceAll(alloc, alts, current, subst.replacement);
    }

    try out.interface.print("\n{d}\n{d}\n{d}\n", .{ input_len, sequence_len, current.len });
    try out.interface.flush();
}

// Serial port of the permutation/flip algorithm of fannkuch-redux 3.go:
// factoradic chunking with sign alternation, and flip counting on a copy
// (pp) so the working permutation p survives counting and the next
// permutation is an O(1)-amortized swap/rotation instead of a full copy
// plus rotation (as in 1.zig). Single-threaded: the whole factorial range
// runs as one chunk. Permutation elements fit in u8; n is capped at 16
// like 3.go (bench inputs are 10 and 11).

const std = @import("std");

const max_n: usize = 16;

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const n = try get_n(init);
    if (n > max_n) return error.NTooLarge;

    var fact: [max_n + 1]usize = undefined;
    fact[0] = 1;
    for (1..n + 1) |i| fact[i] = fact[i - 1] * i;

    // +1 slots: the top-level rotation in the next-permutation step writes
    // p[n] and touches count[n]; both must exist even for n == max_n.
    var p: [max_n + 1]u8 = @splat(0);
    var pp: [max_n + 1]u8 = @splat(0);
    var count: [max_n + 1]u8 = @splat(0);
    for (0..n) |i| p[i] = @intCast(i);

    const idx_max = fact[n];
    var max_flips: i32 = 0;
    var checksum: i32 = 0;
    var idx: usize = 0;
    var sign = true;
    while (true) {
        // Count flips on a copy; p itself is untouched.
        const first = p[0];
        if (first != 0) {
            var flips: i32 = 1;
            if (p[first] != 0) {
                pp = p;
                var p0 = first;
                while (true) {
                    flips += 1;
                    var i: usize = 1;
                    var j: usize = @as(usize, p0) - 1;
                    while (i < j) {
                        std.mem.swap(u8, &pp[i], &pp[j]);
                        i += 1;
                        j -= 1;
                    }
                    const t = pp[p0];
                    pp[p0] = p0;
                    p0 = t;
                    if (pp[p0] == 0) break;
                }
            }
            if (flips > max_flips) max_flips = flips;
            if (sign) {
                checksum += flips;
            } else {
                checksum -= flips;
            }
        }

        idx += 1;
        if (idx == idx_max) break;

        // Next permutation.
        if (sign) {
            std.mem.swap(u8, &p[0], &p[1]);
        } else {
            std.mem.swap(u8, &p[1], &p[2]);
            var k: usize = 2;
            var first_mut = first;
            while (true) {
                count[k] += 1;
                if (count[k] <= k) break;
                count[k] = 0;
                for (0..k + 1) |j| p[j] = p[j + 1];
                p[k + 1] = first_mut;
                first_mut = p[0];
                k += 1;
            }
        }
        sign = !sign;
    }

    try stdout.print("{d}\nPfannkuchen({d}) = {d}\n", .{ checksum, n, max_flips });
    try out.interface.flush();
}

fn get_n(init: std.process.Init) !usize {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next();
    if (arg) |a| {
        return try std.fmt.parseInt(usize, a, 10);
    } else {
        return 10;
    }
}

const std = @import("std");
const DynamicBitSet = std.bit_set.DynamicBitSet;

const global_allocator = std.heap.c_allocator;

fn nsieve(n: usize, stdout: *std.Io.Writer) !void {
    var count: usize = 0;
    var flags = try DynamicBitSet.initEmpty(global_allocator, n);
    defer flags.deinit();
    var i: usize = 2;
    while (i < n) : (i += 1) {
        if (!flags.isSet(i)) {
            count += 1;
            var j: usize = i << 1;
            while (j < n) : (j += i) {
                flags.set(j);
            }
        }
    }
    try stdout.print("Primes up to {d:8} {d:8}\n", .{ n, count });
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

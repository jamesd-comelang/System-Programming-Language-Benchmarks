const std = @import("std");
const ArrayList = std.ArrayList;
const md5 = std.crypto.hash.Md5;

const VEC_SIZE = 8;
const Vec = @Vector(VEC_SIZE, f64);

const global_allocator = std.heap.c_allocator;

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const n = try get_n(init);
    const size = (n + VEC_SIZE - 1) / VEC_SIZE * VEC_SIZE;
    const chunk_size = size / VEC_SIZE;
    const inv = 2.0 / @as(f64, @floatFromInt(size));
    var xloc: ArrayList(Vec) = .empty;
    try xloc.ensureTotalCapacityPrecise(global_allocator, chunk_size);
    var i: usize = 0;
    while (i < chunk_size) : (i += 1) {
        const offset = i * VEC_SIZE;
        const v = Vec{
            init_xloc(offset, inv),
            init_xloc(offset + 1, inv),
            init_xloc(offset + 2, inv),
            init_xloc(offset + 3, inv),
            init_xloc(offset + 4, inv),
            init_xloc(offset + 5, inv),
            init_xloc(offset + 6, inv),
            init_xloc(offset + 7, inv),
        };
        try xloc.append(global_allocator, v);
    }

    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    try stdout.print("P4\n{d} {d}\n", .{ size, size });

    // Scheduler test: one OS thread per row (size threads for input
    // size), derived from 1.zig with the row loop parallelized. Each
    // thread writes only its own row of the preallocated pixel slice.
    // On spawn failure the program reports the failing row and exits
    // nonzero without printing a digest.
    const pixels = try global_allocator.alloc(u8, size * chunk_size);
    defer global_allocator.free(pixels);
    const ctx = Ctx{ .pixels = pixels, .xloc = xloc.items, .chunk_size = chunk_size, .inv = inv };
    var threads = try global_allocator.alloc(std.Thread, size);
    defer global_allocator.free(threads);
    var y: usize = 0;
    while (y < size) : (y += 1) {
        threads[y] = std.Thread.spawn(.{}, row_worker, .{ &ctx, y }) catch |err| {
            std.debug.print("mandelbrot: thread spawn failed at row {d}: {}\n", .{ y, err });
            std.process.exit(1);
        };
    }
    for (threads) |t| t.join();

    // try stdout.print("{}\n", .{pixels});
    var hash: [16]u8 = undefined;
    md5.hash(pixels, &hash, .{});
    const hex = std.fmt.bytesToHex(hash, .lower);
    try stdout.print("{s}\n", .{hex});
    try out.interface.flush();
}

const Ctx = struct { pixels: []u8, xloc: []Vec, chunk_size: usize, inv: f64 };

fn row_worker(ctx: *const Ctx, y: usize) void {
    const ci = @as(f64, @floatFromInt(y)) * ctx.inv - 1.0;
    const offset = y * ctx.chunk_size;
    var x: usize = 0;
    while (x < ctx.chunk_size) : (x += 1) {
        ctx.pixels[offset + x] = mbrot8(ctx.xloc[x], ci);
    }
}

fn mbrot8(cr: Vec, civ: f64) u8 {
    const ci = @as(Vec, @splat(civ));
    const zero: f64 = 0.0;
    var zr = @as(Vec, @splat(zero));
    var zi = @as(Vec, @splat(zero));
    var tr = @as(Vec, @splat(zero));
    var ti = @as(Vec, @splat(zero));
    var absz = @as(Vec, @splat(zero));

    var _i: u8 = 0;
    while (_i < 10) : (_i += 1) {
        var _j: u8 = 0;
        while (_j < 5) : (_j += 1) {
            zi = (zr + zr) * zi + ci;
            zr = tr - ti + cr;
            tr = zr * zr;
            ti = zi * zi;
        }
        absz = tr + ti;
        // Zig 0.17 forbids runtime vector indexing; view as an array.
        const absz_arr: [VEC_SIZE]f64 = @bitCast(absz);
        var terminate = true;
        var i: u8 = 0;
        while (i < VEC_SIZE) : (i += 1) {
            if (absz_arr[i] <= 4.0) {
                terminate = false;
                break;
            }
        }
        if (terminate) {
            return 0;
        }
    }
    const absz_final: [VEC_SIZE]f64 = @bitCast(absz);
    var accu: u8 = 0;
    var i: u8 = 0;
    while (i < VEC_SIZE) : (i += 1) {
        if (absz_final[i] <= 4.0) {
            const lhs: u8 = 0x80;
            accu |= (lhs >> @as(u3, @intCast(i)));
        }
    }
    return accu;
}

fn init_xloc(i: usize, inv: f64) f64 {
    return @as(f64, @floatFromInt(i)) * inv - 1.5;
}

fn get_n(init: std.process.Init) !usize {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next() orelse return 200;
    return try std.fmt.parseInt(usize, arg, 10);
}

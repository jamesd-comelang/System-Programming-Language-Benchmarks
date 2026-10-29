const std = @import("std");

const vec4 = @Vector(4, f64);
fn vec1to4(f: f64) vec4 {
    return @splat(f);
}

fn baseIdx(i: vec4) vec4 {
    @setFloatMode(.optimized);
    return i * (i + vec1to4(1)) * vec1to4(0.5) + vec1to4(1);
}

fn multAvGeneric(comptime transpose: bool, dst: []vec4, src: []const vec4) void {
    @setFloatMode(.optimized);
    const srcVals = std.mem.bytesAsSlice(f64, std.mem.sliceAsBytes(src));
    var ti = if (transpose) vec4{ 1, 2, 3, 4 } else vec4{ 0, 1, 2, 3 };
    for (dst) |*res| {
        var idx = if (transpose) baseIdx(ti - vec1to4(1)) else baseIdx(ti) + ti;
        var sum = vec1to4(0);
        for (srcVals, 0..) |u, j| {
            sum += vec1to4(u) / idx;
            idx += ti + vec1to4(@as(f64, @floatFromInt(j + 1)));
        }
        res.* = sum;
        ti += vec1to4(4);
    }
}

fn multAv(dst: []vec4, src: []const vec4) void {
    return multAvGeneric(false, dst, src);
}

fn multAtv(dst: []vec4, src: []const vec4) void {
    return multAvGeneric(true, dst, src);
}

fn zeroPad(v: []vec4, n: usize) void {
    // Padded lanes (past n) must stay zero: they are ignored by aggregation
    // but would otherwise pollute the next matvec as source columns.
    const scalars = std.mem.bytesAsSlice(f64, std.mem.sliceAsBytes(v));
    for (scalars[n..]) |*x| x.* = 0;
}

fn multAtAv(dest: []vec4, src: []const vec4, temp: []vec4, n: usize) void {
    std.debug.assert(dest.len == src.len and src.len == temp.len);
    multAv(temp, src);
    zeroPad(temp, n);
    multAtv(dest, temp);
    zeroPad(dest, n);
}

fn aggregateResults(u: []const vec4, v: []const vec4, n: usize) f64 {
    @setFloatMode(.optimized);
    const us = std.mem.bytesAsSlice(f64, std.mem.sliceAsBytes(u));
    const vs = std.mem.bytesAsSlice(f64, std.mem.sliceAsBytes(v));
    var vbv: f64 = 0;
    var vv: f64 = 0;
    for (us[0..n], vs[0..n]) |a, b| {
        vbv += a * b;
        vv += b * b;
    }
    return std.math.sqrt(vbv / vv);
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const n = try get_n(init);
    // Padded length: whole vec4 blocks; lanes past n stay zero (see zeroPad).
    const len = (n + @typeInfo(vec4).vector.len - 1) / @typeInfo(vec4).vector.len;

    const allocator = std.heap.c_allocator;

    const u = try allocator.alloc(vec4, len);
    defer allocator.free(u);
    for (u) |*i| i.* = vec1to4(1);
    zeroPad(u, n);

    const v = try allocator.alloc(vec4, len);
    defer allocator.free(v);
    for (v) |*i| i.* = vec1to4(1);

    const temp = try allocator.alloc(vec4, len);
    defer allocator.free(temp);

    var times: usize = 0;
    while (times < 10) : (times += 1) {
        multAtAv(v, u, temp, n);
        multAtAv(u, v, temp, n);
    }

    const res = aggregateResults(u, v, n);

    try stdout.print("{d:.9}\n", .{res});
    try out.interface.flush();
}

fn get_n(init: std.process.Init) !usize {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next();
    if (arg) |a| {
        return try std.fmt.parseInt(usize, a, 10);
    } else {
        return 100;
    }
}

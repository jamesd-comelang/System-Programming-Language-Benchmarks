const std = @import("std");

const vec4 = @Vector(4, f64);
fn vec1to4(f: f64) vec4 {
    return @splat(f);
}

fn runInParallel(tasks: []std.Thread, len: usize, comptime f: anytype, args: anytype) !void {
    const len_per_task = @divTrunc(len, tasks.len + 1);
    for (tasks, 0..) |*task, i| {
        const first = len_per_task * i;
        const last = first + len_per_task;
        task.* = try std.Thread.spawn(.{}, f, .{ first, last } ++ args);
    }
    @call(.auto, f, .{ tasks.len * len_per_task, len } ++ args);
    for (tasks) |*task| task.join();
}

fn baseIdx(i: vec4) vec4 {
    @setFloatMode(.optimized);
    return i * (i + vec1to4(1)) * vec1to4(0.5) + vec1to4(1);
}

fn multAvGeneric(comptime transpose: bool, first: usize, dst: []vec4, src: []const vec4) void {
    @setFloatMode(.optimized);
    const srcVals = std.mem.bytesAsSlice(f64, std.mem.sliceAsBytes(src));
    var ti = vec1to4(@as(f64, @floatFromInt(first * 4))) + if (transpose) vec4{ 1, 2, 3, 4 } else vec4{ 0, 1, 2, 3 };
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

fn multAv(first: usize, last: usize, dst: []vec4, src: []const vec4) void {
    return multAvGeneric(false, first, dst[first..last], src);
}

fn multAtv(first: usize, last: usize, dst: []vec4, src: []const vec4) void {
    return multAvGeneric(true, first, dst[first..last], src);
}

fn zeroPad(v: []vec4, n: usize) void {
    // Padded lanes (past n) must stay zero: they are ignored by aggregation
    // but would otherwise pollute the next matvec as source columns.
    // Called only on the main thread, after the parallel step joined.
    const scalars = std.mem.bytesAsSlice(f64, std.mem.sliceAsBytes(v));
    for (scalars[n..]) |*x| x.* = 0;
}

fn multAtAv(tasks: []std.Thread, dest: []vec4, src: []const vec4, temp: []vec4, n: usize) !void {
    std.debug.assert(dest.len == src.len and src.len == temp.len);
    try runInParallel(tasks, dest.len, multAv, .{ temp, src });
    zeroPad(temp, n);
    try runInParallel(tasks, dest.len, multAtv, .{ dest, temp });
    zeroPad(dest, n);
}

fn setOnes(first: usize, last: usize, dst: []vec4) void {
    for (dst[first..last]) |*v| v.* = vec1to4(1);
}

fn aggregateResults(first: usize, last: usize, u: []const vec4, v: []const vec4, total_vbv: *f64, total_vv: *f64) void {
    @setFloatMode(.optimized);
    var vbv = vec1to4(0);
    var vv = vec1to4(0);
    for (v[first..last], 0..) |f, i| {
        vbv += u[first + i] * f;
        vv += f * f;
    }
    _ = @atomicRmw(f64, total_vbv, .Add, @reduce(.Add, vbv), .seq_cst);
    _ = @atomicRmw(f64, total_vv, .Add, @reduce(.Add, vv), .seq_cst);
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
    const v = try allocator.alloc(vec4, len);
    defer allocator.free(v);
    const temp = try allocator.alloc(vec4, len);
    defer allocator.free(temp);

    const task_count = try std.Thread.getCpuCount();
    const tasks = try allocator.alloc(std.Thread, task_count - 1);
    defer allocator.free(tasks);

    try runInParallel(tasks, u.len, setOnes, .{u});
    zeroPad(u, n);

    var times: usize = 0;
    while (times < 10) : (times += 1) {
        try multAtAv(tasks, v, u, temp, n);
        try multAtAv(tasks, u, v, temp, n);
    }

    var vbv: f64 = 0;
    var vv: f64 = 0;
    try runInParallel(tasks, u.len, aggregateResults, .{ u, v, &vbv, &vv });
    const res = std.math.sqrt(vbv / vv);

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

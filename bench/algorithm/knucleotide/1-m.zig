const std = @import("std");

const gpa = std.heap.c_allocator;

const Code = struct {
    data: u64,

    pub inline fn encodeByte(c: u8) u2 {
        return @intCast((c >> 1) & 0b11);
    }

    pub inline fn makeMask(frame: usize) u64 {
        return (@as(u64, 1) << @as(u6, @intCast(2 * frame))) - 1;
    }

    pub inline fn push(self: *Code, c: u8, mask: u64) void {
        self.data = ((self.data << 2) | c) & mask;
    }

    pub fn fromStr(s: []const u8) Code {
        const mask = Code.makeMask(s.len);
        var res = Code{ .data = 0 };
        for (s) |c| {
            res.push(Code.encodeByte(c), mask);
        }
        return res;
    }

    pub fn toString(self: Code, frame: usize) ![]const u8 {
        var result: std.ArrayList(u8) = .empty;
        var code = self.data;
        for (0..frame) |_| {
            const c: u8 = switch (@as(u2, @truncate(code))) {
                Code.encodeByte('A') => 'A',
                Code.encodeByte('T') => 'T',
                Code.encodeByte('G') => 'G',
                Code.encodeByte('C') => 'C',
            };
            try result.append(gpa, c);
            code >>= 2;
        }
        std.mem.reverse(u8, result.items);
        return result.toOwnedSlice(gpa);
    }
};

pub fn readInput(init: std.process.Init) ![]const u8 {
    const io = init.io;
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const file_name = arg_it.next() orelse "25000_in";
    const raw = try std.Io.Dir.cwd().readFileAlloc(io, file_name, gpa, .unlimited);
    // Skip past the 3 header lines, exactly as the old streaming reader
    // did: consume up to and including the 3rd '>', then up to and
    // including the next newline.
    var i: usize = 0;
    var seen: usize = 0;
    while (i < raw.len and seen < 3) : (i += 1) {
        if (raw[i] == '>') seen += 1;
    }
    if (seen < 3) return error.EndOfStream;
    while (i < raw.len and raw[i] != '\n') : (i += 1) {}
    if (i >= raw.len) return error.EndOfStream;
    var buf = raw[i + 1 ..];
    // In place, remove all newlines from buf and encode nucleotides
    // using only the last 2 bits in each byte.
    {
        var j: usize = 0;
        for (buf) |c| {
            if (c != '\n') {
                // Gives a -> 0x00, c -> 0x01, g -> 0x03, t -> 0x02
                buf[j] = (c >> 1) & 0x03;
                j += 1;
            }
        }
        buf.len = j;
    }
    return buf;
}

const CodeContext = struct {
    pub fn eql(_: CodeContext, a: Code, b: Code) bool { return a.data == b.data; }
    pub fn hash(_: CodeContext, c: Code) u64 { return c.data ^ (c.data >> 7); }
};

const Map = std.HashMapUnmanaged(Code, u32, CodeContext, 45);

fn genMap(taskIndex: usize, from: usize, to: usize, seq: []const u8, frame: usize, maps: []Map) !void {
    const map = &maps[taskIndex];
    const mask = Code.makeMask(frame);
    var code: Code = .{ .data = 0 };
    for (seq[from..][0..frame - 1]) |e| code.push(e, mask);
    for (seq[frame - 1..][from .. to]) |e| {
        code.push(e, mask);
        const kv = try map.getOrPutValue(gpa, code, 0);
        kv.value_ptr.* += 1;
    }
}

const CountCode = struct {
    count: u32,
    code: Code,

    pub fn dsc(_: void, a: CountCode, b: CountCode) bool {
        const order = std.math.order(a.count, b.count);
        return order == .gt or (order == .eq and b.code.data > a.code.data);
    }
};

fn printMap(frame: usize, maps: []const Map, stdout: *std.Io.Writer) !void {
    const code_limit = 16;
    var counts: [code_limit]u32 = @splat(0);
    var total: u64 = 0;
    for (maps) |map| {
        var iter = map.iterator();
        while (iter.next()) |it| {
            const code = it.key_ptr.*;
            const count = it.value_ptr.*;
            total += count;
            counts[code.data] += count;
        }
    }

    // std.BoundedArray was removed in Zig 0.17; same fixed buffer + length.
    var cc_buf: [code_limit]CountCode = undefined;
    var cc_len: usize = 0;
    for (counts, 0..) |count, code_data| if (count > 0) {
        cc_buf[cc_len] = .{ .count = count, .code = .{ .data = @intCast(code_data) } };
        cc_len += 1;
    };
    const cc = cc_buf[0..cc_len];
    std.mem.sort(CountCode, cc, {}, CountCode.dsc);

    for (cc) |c| {
        try stdout.print("{!s} {d:.3}\n", .{
            c.code.toString(frame),
            @as(f32, @floatFromInt(c.count)) / @as(f32, @floatFromInt(total)) * 100.0,
        });
    }
    try stdout.print("\n", .{});
}

fn printOcc(occ: []const u8, maps: []const Map, stdout: *std.Io.Writer) !void {
    const code = Code.fromStr(occ);
    var total: u32 = 0;
    for (maps) |m| {
        if (m.get(code)) |count| total += count;
    }
    try stdout.print("{}\t{s}\n", .{ total, occ });
}

fn runInParallel(task_count: usize, len: usize, comptime f: anytype, args: anytype) !void {
    const tasks = try gpa.alloc(std.Thread, task_count - 1);
    defer gpa.free(tasks);
    const len_per_task = @divTrunc(len, task_count);
    for (tasks, 0..) |*task, i| {
        const first = len_per_task * i;
        const last = first + len_per_task;
        task.* = try std.Thread.spawn(.{}, f, .{ i, first, last } ++ args);
    }
    try @call(.auto, f, .{ tasks.len, tasks.len * len_per_task, len } ++ args);
    for (tasks) |*task| task.join();
}

fn genMaps(seq: []const u8, frame: usize, maps: []Map) !void {
    for (maps) |*m| m.clearAndFree(gpa);
    try runInParallel(maps.len, seq.len - (frame - 1), genMap, .{seq, frame, maps});
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [65536]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const seq = try readInput(init);
    const task_count = try std.Thread.getCpuCount();
    const maps = try gpa.alloc(Map, task_count);
    defer gpa.free(maps);
    @memset(maps, .empty);

    try genMaps(seq, 1, maps);
    try printMap(1, maps, stdout);
    try genMaps(seq, 2, maps);
    try printMap(2, maps, stdout);

    const occs = [_][]const u8{
        "GGT",
        "GGTA",
        "GGTATT",
        "GGTATTTTAATT",
        "GGTATTTTAATTTATAGT",
    };
    for (occs) |occ| {
        try genMaps(seq, occ.len, maps);
        try printOcc(occ, maps, stdout);
    }
    try out.interface.flush();
}

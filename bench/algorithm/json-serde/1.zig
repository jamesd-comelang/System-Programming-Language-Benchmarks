const std = @import("std");
const json = std.json;

// Port of the original 1.zig to Zig 0.17, with one deliberate change:
// Zig 0.17's Stringify calls a custom jsonStringify without emitting the
// pending ':' after the field name, so the old Geometry.jsonStringify
// trick no longer produces valid output. Serialization is therefore done
// manually here (same field order, same compact layout, same {d} float
// formatting as the original), while parsing still uses std.json.

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const stem = arg_it.next() orelse "sample";
    const file_name = try std.mem.concat(alloc, u8, &.{ stem, ".json" });

    var n: usize = 3;
    if (arg_it.next()) |arg| {
        n = try std.fmt.parseInt(usize, arg, 10);
    }

    const json_str = try std.Io.Dir.cwd().readFileAlloc(io, file_name, alloc, .unlimited);

    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);

    {
        const parsed = try json.parseFromSlice(GeoData, alloc, json_str, .{});
        defer parsed.deinit();
        var aw: std.Io.Writer.Allocating = .init(alloc);
        try writeGeoData(&aw.writer, parsed.value);
        try printHash(aw.written(), &out);
    }
    {
        var array: std.ArrayList(GeoData) = .empty;
        var i: usize = 0;
        while (i < n) : (i += 1) {
            const parsed = try json.parseFromSlice(GeoData, alloc, json_str, .{});
            // Deliberately not deiniting: array items borrow from each
            // parse's arena, as in the original.
            try array.append(alloc, parsed.value);
        }
        var aw: std.Io.Writer.Allocating = .init(alloc);
        try aw.writer.writeByte('[');
        for (array.items, 0..) |item, idx| {
            if (idx != 0) try aw.writer.writeByte(',');
            try writeGeoData(&aw.writer, item);
        }
        try aw.writer.writeByte(']');
        try printHash(aw.written(), &out);
    }
    try out.interface.flush();
}

fn writeJsonString(w: *std.Io.Writer, s: []const u8) !void {
    try w.writeByte('"');
    for (s) |c| {
        switch (c) {
            '"' => try w.writeAll("\\\""),
            '\\' => try w.writeAll("\\\\"),
            '\n' => try w.writeAll("\\n"),
            '\r' => try w.writeAll("\\r"),
            '\t' => try w.writeAll("\\t"),
            0x08 => try w.writeAll("\\b"),
            0x0c => try w.writeAll("\\f"),
            else => {
                if (c < 0x20) {
                    try w.print("\\u{x:0>4}", .{c});
                } else {
                    try w.writeByte(c);
                }
            },
        }
    }
    try w.writeByte('"');
}

fn writeGeoData(w: *std.Io.Writer, data: GeoData) !void {
    try w.writeAll("{\"type\":");
    try writeJsonString(w, data.type);
    try w.writeAll(",\"features\":[");
    for (data.features, 0..) |f, fi| {
        if (fi != 0) try w.writeByte(',');
        try w.writeAll("{\"type\":");
        try writeJsonString(w, f.type);
        try w.writeAll(",\"properties\":{\"name\":");
        try writeJsonString(w, f.properties.name);
        try w.writeAll("},\"geometry\":{\"type\":");
        try writeJsonString(w, f.geometry.type);
        try w.writeAll(",\"coordinates\":[");
        for (f.geometry.coordinates, 0..) |row, rowi| {
            if (rowi != 0) try w.writeByte(',');
            try w.writeByte('[');
            for (row, 0..) |col, coli| {
                if (coli != 0) try w.writeByte(',');
                try w.print("[{d},{d}]", .{ col[0], col[1] });
            }
            try w.writeByte(']');
        }
        try w.writeAll("]}}");
    }
    try w.writeAll("]}");
}

fn printHash(bytes: []const u8, out: anytype) !void {
    const Md5 = std.crypto.hash.Md5;
    var hash: [Md5.digest_length]u8 = undefined;
    Md5.hash(bytes, &hash, .{});
    for (hash) |b| {
        try out.interface.print("{x:0>2}", .{b});
    }
    try out.interface.print("\n", .{});
}

const GeoData = struct {
    type: []const u8,
    features: []const Feature,
};
const Feature = struct {
    type: []const u8,
    properties: Properties,
    geometry: Geometry,
};
const Properties = struct { name: []const u8 };
const Geometry = struct {
    type: []const u8,
    coordinates: []const []const [2]f64,
};

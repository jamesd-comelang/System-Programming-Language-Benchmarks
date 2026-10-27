const std = @import("std");

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next();
    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    if (arg) |a| {
        try out.interface.print("Hello world {s}!\n", .{a});
    } else {
        try out.interface.print("Hello world!\n", .{});
    }
    try out.interface.flush();
}

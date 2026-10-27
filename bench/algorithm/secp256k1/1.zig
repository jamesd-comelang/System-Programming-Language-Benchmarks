// secp256k1 scalar multiplication, ported from 1.rs (ibig) / 0.rs (k256).
//
// The curve is y^2 = x^3 + 7 over the field mod
//   p = 2^256 - 2^32 - 977,
// with the standard generator G. The program reads n from argv[1]
// (default 1, as in 1.rs), starts from G, and n times replaces the point
// by point * PRIVATE_KEY (the same private key as 1.rs / 0.rs), then
// prints the resulting affine point as "{x:x},{y:x}" -- lowercase hex
// with no leading zeros, exactly like Rust's {:x} formatting.
//
// Arithmetic is fixed 4 x u64 limbs, little-endian. Field multiplication
// uses the secp256k1 fast reduction: 2^256 == 2^32 + 977 (mod p), so a
// 512-bit product is folded until it fits in 256 bits, then one
// conditional subtraction of p. Point arithmetic is the Jacobian
// double/add from 1.rs (a = 0 curve), with plain double-and-add scalar
// multiplication instead of 1.rs's endomorphism split: the result is
// identical, and 256 steps per multiplication is fast enough here
// (the bench inputs go up to 2000 iterations).
//
// Usage: ./app <n>   (default 1)

const std = @import("std");

const F = [4]u64;

const P: F = .{ 0xfffffffefffffc2f, 0xffffffffffffffff, 0xffffffffffffffff, 0xffffffffffffffff };
const GX: F = .{ 0x59f2815b16f81798, 0x29bfcdb2dce28d9, 0x55a06295ce870b07, 0x79be667ef9dcbbac };
const GY: F = .{ 0x9c47d08ffb10d4b8, 0xfd17b448a6855419, 0x5da4fbfc0e1108a8, 0x483ada7726a3c465 };
// Private key, little-endian limbs of
// 20775598474904240222758871485654738649026525153462921990999819694398496339603
// (same bytes as 0.rs's PRIVATE_KEY, big-endian on the wire).
const PRIV: F = .{ 0xd9132d70b5bc7693, 0xb64592e3fe0ab0da, 0x4fca3ef970ff4d38, 0x2dee927079283c3c };

fn feIsZero(a: F) bool {
    return a[0] == 0 and a[1] == 0 and a[2] == 0 and a[3] == 0;
}

fn eql(a: F, b: F) bool {
    return a[0] == b[0] and a[1] == b[1] and a[2] == b[2] and a[3] == b[3];
}

// Compare: -1, 0, 1.
fn cmp(a: F, b: F) i32 {
    var i: usize = 4;
    while (i > 0) {
        i -= 1;
        if (a[i] != b[i]) return if (a[i] < b[i]) -1 else 1;
    }
    return 0;
}

fn addRaw(a: F, b: F) struct { v: F, carry: u64 } {
    var v: F = undefined;
    var carry: u64 = 0;
    for (0..4) |i| {
        const s: u128 = @as(u128, a[i]) + b[i] + carry;
        v[i] = @truncate(s);
        carry = @truncate(s >> 64);
    }
    return .{ .v = v, .carry = carry };
}

fn subRaw(a: F, b: F) struct { v: F, borrow: u64 } {
    var v: F = undefined;
    var borrow: u128 = 0;
    for (0..4) |i| {
        const b_ext: u128 = @as(u128, b[i]) + borrow;
        if (@as(u128, a[i]) >= b_ext) {
            v[i] = @truncate(@as(u128, a[i]) - b_ext);
            borrow = 0;
        } else {
            v[i] = @truncate(((@as(u128, 1) << 64) + a[i]) - b_ext);
            borrow = 1;
        }
    }
    return .{ .v = v, .borrow = @truncate(borrow) };
}

fn addMod(a: F, b: F) F {
    const s = addRaw(a, b);
    if (s.carry == 0) {
        if (cmp(s.v, P) >= 0) {
            return subRaw(s.v, P).v;
        }
        return s.v;
    }
    // Sum is 2^256 + s.v, and 2^256 == C (mod p) with C = 2^32 + 977,
    // so the result is (s.v + C) mod p. Note that subtracting p from
    // s.v alone would be wrong when s.v + C >= p.
    const C: F = .{ 0x1000003d1, 0, 0, 0 };
    const t = addRaw(s.v, C);
    if (t.carry == 1) {
        // s.v + C >= 2^256, so (s.v + C) mod p = (t.v + C) mod p,
        // and t.v + C < 2 * C < p.
        return addRaw(t.v, C).v;
    }
    if (cmp(t.v, P) >= 0) {
        return subRaw(t.v, P).v;
    }
    return t.v;
}

fn subMod(a: F, b: F) F {
    const s = subRaw(a, b);
    if (s.borrow == 1) {
        return addRaw(s.v, P).v;
    }
    return s.v;
}

fn mulSmall(a: F, m: u64) [5]u64 {
    var v: [5]u64 = .{ 0, 0, 0, 0, 0 };
    var carry: u64 = 0;
    for (0..4) |i| {
        const p: u128 = @as(u128, a[i]) * m + carry;
        v[i] = @truncate(p);
        carry = @truncate(p >> 64);
    }
    v[4] = carry;
    return v;
}

// Add src (5 limbs) into dst starting at limb `off`, dst has 9 limbs.
fn addInto(dst: *[9]u64, src: [5]u64, off: usize) void {
    var carry: u64 = 0;
    for (0..5) |i| {
        const s: u128 = @as(u128, dst[off + i]) + src[i] + carry;
        dst[off + i] = @truncate(s);
        carry = @truncate(s >> 64);
    }
    var i = off + 5;
    while (carry == 1 and i < 9) : (i += 1) {
        const s: u128 = @as(u128, dst[i]) + carry;
        dst[i] = @truncate(s);
        carry = @truncate(s >> 64);
    }
}

// Fold a value < 2^576 (9 limbs) down with 2^256 == 2^32 + 977 (mod p).
// Returns a value < 2^256; the caller applies the final conditional
// subtraction of p.
fn fold(v_in: [9]u64) F {
    var v = v_in;
    var guard: usize = 0;
    while (guard < 16) : (guard += 1) {
        // Is there anything above limb 3?
        var high_zero = true;
        for (4..9) |i| {
            if (v[i] != 0) {
                high_zero = false;
                break;
            }
        }
        if (high_zero) break;
        // high = v[4..9]; new = v[0..4] + high * (2^32 + 977)
        const high: F = .{ v[4], v[5], v[6], v[7] };
        const high_extra: u64 = v[8];
        var nv: [9]u64 = .{ v[0], v[1], v[2], v[3], 0, 0, 0, 0, 0 };
        // high * 2^32: shift left by 32 bits (limb shift 0, bit shift 32)
        var shifted: [5]u64 = .{ 0, 0, 0, 0, 0 };
        var carry: u64 = 0;
        for (0..4) |i| {
            const w: u128 = (@as(u128, high[i]) << 32) | carry;
            shifted[i] = @truncate(w);
            carry = @truncate(w >> 64);
        }
        shifted[4] = carry;
        addInto(&nv, shifted, 0);
        // high * 977
        addInto(&nv, mulSmall(high, 977), 0);
        // high_extra (bits >= 512) * (2^32 + 977): high_extra < 2^64 here
        // only if the input ever had limb 8 set; fold() inputs from mulMod
        // have limb 8 == 0, but handle it anyway via two more adds at
        // limb offset 0 of the shifted product (extra * 2^32 is 2 limbs,
        // extra * 977 is 2 limbs) -- both fit in the 9-limb buffer.
        if (high_extra != 0) {
            // high_extra * 2^32, as a 2-limb value.
            var sh: [5]u64 = .{ 0, 0, 0, 0, 0 };
            const w: u128 = @as(u128, high_extra) << 32;
            sh[0] = @truncate(w);
            sh[1] = @truncate(w >> 64);
            addInto(&nv, sh, 0);
            var m: [5]u64 = .{ 0, 0, 0, 0, 0 };
            const p: u128 = @as(u128, high_extra) * 977;
            m[0] = @truncate(p);
            m[1] = @truncate(p >> 64);
            addInto(&nv, m, 0);
        }
        v = nv;
    }
    return .{ v[0], v[1], v[2], v[3] };
}

fn mulMod(a: F, b: F) F {
    // 512-bit product in 8 limbs, then fold.
    var prod: [9]u64 = .{ 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (0..4) |i| {
        var carry: u64 = 0;
        for (0..4) |j| {
            const s: u128 = @as(u128, prod[i + j]) + @as(u128, a[i]) * b[j] + carry;
            prod[i + j] = @truncate(s);
            carry = @truncate(s >> 64);
        }
        // NOTE: `carry` here is a full 64-bit value (the high half of the
        // last accumulated product), not just 0/1 -- it must be added in
        // full, or the top limb of the product is lost.
        var k = i + 4;
        var c = carry;
        while (c != 0 and k < 9) : (k += 1) {
            const s: u128 = @as(u128, prod[k]) + c;
            prod[k] = @truncate(s);
            c = @truncate(s >> 64);
        }
    }
    const folded = fold(prod);
    if (cmp(folded, P) >= 0) {
        return subRaw(folded, P).v;
    }
    return folded;
}

fn sqrMod(a: F) F {
    return mulMod(a, a);
}

fn powMod(base: F, exp: F) F {
    var result: F = .{ 1, 0, 0, 0 };
    var b = base;
    for (0..256) |bit| {
        if ((exp[bit / 64] >> @intCast(bit % 64)) & 1 == 1) {
            result = mulMod(result, b);
        }
        b = sqrMod(b);
    }
    return result;
}

fn invert(a: F) F {
    // Fermat: a^(p-2) mod p. p - 2 limbs:
    const pm2: F = .{ 0xfffffffefffffc2d, 0xffffffffffffffff, 0xffffffffffffffff, 0xffffffffffffffff };
    return powMod(a, pm2);
}

const JPoint = struct {
    x: F,
    y: F,
    z: F,

    fn zero() JPoint {
        return .{ .x = .{ 0, 0, 0, 0 }, .y = .{ 1, 0, 0, 0 }, .z = .{ 0, 0, 0, 0 } };
    }

    fn fromAffine(x: F, y: F) JPoint {
        return .{ .x = x, .y = y, .z = .{ 1, 0, 0, 0 } };
    }

    fn isZero(self: JPoint) bool {
        return feIsZero(self.z);
    }

    fn toAffine(self: JPoint) struct { x: F, y: F } {
        const inv_z = invert(self.z);
        const inv_z2 = sqrMod(inv_z);
        const x = mulMod(self.x, inv_z2);
        const y = mulMod(mulMod(self.y, inv_z), inv_z2);
        return .{ .x = x, .y = y };
    }

    fn negate(self: JPoint) JPoint {
        return .{ .x = self.x, .y = subMod(.{ 0, 0, 0, 0 }, self.y), .z = self.z };
    }

    fn double(self: JPoint) JPoint {
        // From 1.rs (a = 0).
        const a = sqrMod(self.x);
        const b = sqrMod(self.y);
        const c = sqrMod(b);
        const xb = addMod(self.x, b);
        const d = mulMod(subMod(subMod(sqrMod(xb), a), c), .{ 2, 0, 0, 0 });
        const e = mulMod(a, .{ 3, 0, 0, 0 });
        const f = sqrMod(e);
        const x3 = subMod(f, mulMod(d, .{ 2, 0, 0, 0 }));
        const y3 = subMod(mulMod(e, subMod(d, x3)), mulMod(c, .{ 8, 0, 0, 0 }));
        const z3 = mulMod(mulMod(self.y, self.z), .{ 2, 0, 0, 0 });
        return .{ .x = x3, .y = y3, .z = z3 };
    }

    fn add(self: JPoint, other: JPoint) JPoint {
        if (other.isZero()) return self;
        if (self.isZero()) return other;
        const z1z1 = sqrMod(self.z);
        const z2z2 = sqrMod(other.z);
        const uu1 = mulMod(self.x, z2z2);
        const uu2 = mulMod(other.x, z1z1);
        const s1 = mulMod(mulMod(self.y, other.z), z2z2);
        const s2 = mulMod(mulMod(other.y, self.z), z1z1);
        const h = subMod(uu2, uu1);
        const r = subMod(s2, s1);
        if (feIsZero(h)) {
            if (feIsZero(r)) {
                return self.double();
            } else {
                return JPoint.zero();
            }
        }
        const hh = sqrMod(h);
        const hhh = mulMod(h, hh);
        const v = mulMod(uu1, hh);
        const x3 = subMod(subMod(sqrMod(r), hhh), mulMod(v, .{ 2, 0, 0, 0 }));
        const y3 = subMod(mulMod(r, subMod(v, x3)), mulMod(s1, hhh));
        const z3 = mulMod(mulMod(self.z, other.z), h);
        return .{ .x = x3, .y = y3, .z = z3 };
    }

    fn mul(self: JPoint, scalar: F) JPoint {
        var p = JPoint.zero();
        var d = self;
        for (0..256) |bit| {
            if ((scalar[bit / 64] >> @intCast(bit % 64)) & 1 == 1) {
                p = p.add(d);
            }
            d = d.double();
        }
        return p;
    }
};

// Print a field element as lowercase hex with no leading zeros, like
// Rust's {:x} for a big integer. Returns via the writer.
fn printHex(out: anytype, v: F) !void {
    // Find the most significant non-zero limb.
    var top: i32 = 3;
    while (top > 0 and v[@intCast(top)] == 0) top -= 1;
    if (top == 0 and v[0] == 0) {
        try out.interface.print("0", .{});
        return;
    }
    try out.interface.print("{x}", .{v[@intCast(top)]});
    var i: i32 = top - 1;
    while (i >= 0) : (i -= 1) {
        try out.interface.print("{x:0>16}", .{v[@intCast(i)]});
    }
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    var n: usize = 1;
    if (arg_it.next()) |arg| {
        n = std.fmt.parseInt(usize, arg, 10) catch 1;
    }

    var px = GX;
    var py = GY;
    for (0..n) |_| {
        const jp = JPoint.fromAffine(px, py).mul(PRIV);
        const aff = jp.toAffine();
        px = aff.x;
        py = aff.y;
    }

    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try printHex(&out, px);
    try out.interface.print(",", .{});
    try printHex(&out, py);
    try out.interface.print("\n", .{});
    try out.interface.flush();
}

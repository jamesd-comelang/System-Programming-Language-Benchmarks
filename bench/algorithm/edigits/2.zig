const std = @import("std");
const bigint = std.math.big.int;
const math = std.math;
const global_allocator = std.heap.c_allocator;
const Limb = std.math.big.Limb;
const DoubleLimb = std.math.big.DoubleLimb;

const Pair = struct {
    p: bigint.Managed,
    q: bigint.Managed,
    pub fn deinit(self: *Pair) void {
        defer self.p.deinit();
        defer self.q.deinit();
    }
};

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    var out_buf: [4096]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const stdout = &out.interface;
    const n = try get_n(init);
    if (n <= 0) return;
    const n_usize: usize = @intCast(n);

    // Digit string (n digits, no decimal point).
    const digits = try global_allocator.alloc(u8, n_usize);
    defer global_allocator.free(digits);
    if (n == 1) {
        digits[0] = '2';
    } else {
        try calculate_digits(n, digits);
    }

    var i: usize = 0;
    while (i < n_usize) {
        var sb: [10]u8 = .{ ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
        var j: usize = i;
        while (j < n_usize and j < i + 10) {
            sb[j - i] = digits[j];
            j += 1;
        }
        if (i + 10 <= n_usize) {
            try stdout.print("{s}\t:{d}\n", .{ sb, i + 10 });
        } else {
            try stdout.print("{s}\t:{d}\n", .{ sb, n });
        }
        i += 10;
    }
    try out.interface.flush();
}

// Fill `digits` (length n >= 2) with the first n digits of e.
// Binary splitting (as in 1.zig) gives p/q = sum_{j=0}^{k-1} 1/j!, which
// lies in (2, 3). With r = p - 2q the answer is "2" followed by the
// n-1 digits of floor(r * 10^(n-1) / q), extracted here in base-10^9
// chunks by long division against a once-normalized divisor, on raw
// limb slices. 1.zig instead computes p * 10^(n-1) / q with one giant
// Knuth division and then renders the answer with std's toString,
// whose base-10 path divides by 10^9 once per output chunk over the
// full limb array -- both quadratic, and the combination is what makes
// 1.zig 18-40x slower than the GMP-based C version. The integer
// computed here is exactly floor(p * 10^(n-1) / q), identical to
// 1.zig's, so the output is unchanged.
fn calculate_digits(n: i32, digits: []u8) !void {
    const k = binary_search(n);
    var pair = try sum_terms(0, k - 1);
    defer pair.deinit();

    var answer = try bigint.Managed.init(global_allocator);
    defer answer.deinit();
    try bigint.Managed.add(&answer, &pair.p, &pair.q); // add 1/0! = 1
    var two = try bigint.Managed.initSet(global_allocator, 2);
    defer two.deinit();
    var two_q = try bigint.Managed.init(global_allocator);
    defer two_q.deinit();
    try bigint.Managed.mul(&two_q, &pair.q, &two);
    var r = try bigint.Managed.init(global_allocator);
    defer r.deinit();
    try bigint.Managed.sub(&r, &answer, &two_q); // 0 < r < q
    std.debug.assert(r.toConst().positive);

    const q_limbs = pair.q.toConst().limbs;
    const r_limbs = r.toConst().limbs;
    const nlimbs = q_limbs.len;
    std.debug.assert(nlimbs >= 1);
    // Normalized divisor: top limb >= 2^63.
    const shift: usize = @clz(q_limbs[nlimbs - 1]);
    const qn = try global_allocator.alloc(Limb, nlimbs);
    defer global_allocator.free(qn);
    shift_left(qn, q_limbs, shift);

    // Two working buffers (nlimbs + 1 each); the remainder lives in the
    // first nlimbs of one of them and the buffers swap each chunk, so
    // no per-chunk allocation or copying happens in the digit loop.
    var buf_a = try global_allocator.alloc(Limb, nlimbs + 1);
    defer global_allocator.free(buf_a);
    var buf_b = try global_allocator.alloc(Limb, nlimbs + 1);
    defer global_allocator.free(buf_b);
    shift_left(buf_a[0..nlimbs], r_limbs, shift); // normalized remainder
    var rem: []Limb = buf_a[0..nlimbs];
    var an: []Limb = buf_b;

    const rem_digits: i32 = n - 1;
    const nchunks: usize = @intCast(@divFloor(rem_digits + 8, 9));
    const first_digits: i32 = rem_digits - 9 * (@as(i32, @intCast(nchunks)) - 1); // 1..9
    digits[0] = '2';
    var pos: usize = 1;
    var c: usize = 0;
    while (c < nchunks) : (c += 1) {
        const width: i32 = if (c == 0) first_digits else 9;
        var scale: Limb = 1;
        var t: i32 = 0;
        while (t < width) : (t += 1) {
            scale *= 10;
        }
        const used = mul_small(an, rem, scale);
        @memset(an[used..], 0);
        const chunk = if (nlimbs >= 2) div_step(an, qn) else div_step_small(an, qn);
        // Zero-padded to `width` digits (width <= 9, chunk < 10^width).
        var tmp: [9]u8 = undefined;
        var v = chunk;
        var j: i32 = width;
        while (j > 0) {
            j -= 1;
            tmp[@intCast(j)] = '0' + @as(u8, @intCast(v % 10));
            v /= 10;
        }
        @memcpy(digits[pos..][0..@intCast(width)], tmp[0..@intCast(width)]);
        pos += @intCast(width);
        // Swap buffers: an now holds the remainder in limbs [0..nlimbs).
        const tmp_buf = buf_a;
        buf_a = buf_b;
        buf_b = tmp_buf;
        rem = buf_a[0..nlimbs];
        an = buf_b;
    }
    std.debug.assert(pos == digits.len);
}

// dst[i] = src[i] << shift, little-endian limbs; dst.len >= src.len, and
// any dst limbs past src.len are filled from the shift carry (zero when
// the shifted value fits, which holds for the normalized remainder).
fn shift_left(dst: []Limb, src: []const Limb, shift: usize) void {
    if (shift == 0) {
        @memcpy(dst[0..src.len], src);
        @memset(dst[src.len..], 0);
        return;
    }
    const s: u6 = @intCast(shift);
    const inv: u6 = @intCast(64 - shift);
    var carry: Limb = 0;
    var i: usize = 0;
    while (i < src.len) : (i += 1) {
        dst[i] = (src[i] << s) | carry;
        carry = src[i] >> inv;
    }
    while (i < dst.len) : (i += 1) {
        dst[i] = carry;
        carry = 0;
    }
}

// dst = src * m (m <= 10^9, so every intermediate fits in DoubleLimb).
// Returns the number of used limbs (trailing zeros trimmed).
fn mul_small(dst: []Limb, src: []const Limb, m: Limb) usize {
    var carry: DoubleLimb = 0;
    var i: usize = 0;
    while (i < src.len) : (i += 1) {
        const cur: DoubleLimb = @as(DoubleLimb, src[i]) * m + carry;
        // @truncate, not @intCast: cur exceeds Limb here by design, and
        // @intCast on an out-of-range value is UB with safety off -- the
        // optimizer then assumes carry is always 0 and deletes it.
        dst[i] = @truncate(cur);
        carry = cur >> 64;
    }
    while (carry != 0) {
        dst[i] = @intCast(carry);
        carry >>= 64;
        i += 1;
    }
    while (i > 0 and dst[i - 1] == 0) {
        i -= 1;
    }
    return i;
}

// One Knuth step: an holds nlimbs + 1 limbs (the normalized dividend,
// an < qn * 10^9), qn holds nlimbs limbs with its top limb >= 2^63.
// Returns the single quotient limb (< 10^9) and leaves the normalized
// remainder in an[0..nlimbs) (an[nlimbs] becomes 0). Requires nlimbs >= 2.
fn div_step(an: []Limb, qn: []const Limb) Limb {
    const n = qn.len;
    const num: DoubleLimb = (@as(DoubleLimb, an[n]) << 64) | an[n - 1];
    var qhat: DoubleLimb = num / qn[n - 1];
    var rhat: DoubleLimb = num % qn[n - 1];
    const base: DoubleLimb = @as(DoubleLimb, 1) << 64;
    while (qhat >= base or qhat * qn[n - 2] > ((rhat << 64) | an[n - 2])) {
        qhat -= 1;
        rhat += qn[n - 1];
        if (rhat >= base) break;
    }
    // an[n] < 10^9 < 2^63 <= qn[n-1], so qhat < 2^32 after correction.
    const q: Limb = @intCast(qhat);
    var borrow: DoubleLimb = 0;
    for (0..n) |i| {
        const prod: DoubleLimb = @as(DoubleLimb, q) * qn[i] + borrow;
        const ai: DoubleLimb = an[i];
        an[i] = @truncate(ai -% prod);
        borrow = if (prod <= ai) 0 else (prod - ai + std.math.maxInt(Limb)) >> 64;
    }
    var top: Limb = an[n] -% @as(Limb, @intCast(borrow));
    // Knuth guarantees qhat is exact or one too large; the loop form
    // also covers the vanishingly rare off-by-two case.
    while (top >= 0x8000_0000_0000_0000) {
        qhat -= 1;
        var carry: Limb = 0;
        for (0..n) |i| {
            const s: DoubleLimb = @as(DoubleLimb, an[i]) + qn[i] + carry;
            an[i] = @truncate(s);
            carry = @intCast(s >> 64);
        }
        top +%= carry;
    }
    an[n] = top;
    return @intCast(qhat);
}

// nlimbs == 1 fallback for div_step: the division fits in DoubleLimb.
fn div_step_small(an: []Limb, qn: []const Limb) Limb {
    const num: DoubleLimb = (@as(DoubleLimb, an[1]) << 64) | an[0];
    const qq = num / qn[0];
    an[0] = @intCast(num - qq * qn[0]);
    an[1] = 0;
    return @intCast(qq);
}

fn sum_terms(a: i32, b: i32) anyerror!Pair {
    if (b == a + 1) {
        const p = try bigint.Managed.initSet(global_allocator, 1);
        const q = try bigint.Managed.initSet(global_allocator, b);
        return Pair{
            .p = p,
            .q = q,
        };
    }
    const mid: i32 = @divFloor((a + b), 2);
    var pair_left: Pair = try sum_terms(a, mid);
    defer pair_left.deinit();
    var pair_right: Pair = try sum_terms(mid, b);
    defer pair_right.deinit();
    var left = try bigint.Managed.init(global_allocator);
    try bigint.Managed.mul(&left, &pair_left.p, &pair_right.q);
    try bigint.Managed.add(&left, &left, &pair_right.p);
    var right = try bigint.Managed.init(global_allocator);
    try bigint.Managed.mul(&right, &pair_left.q, &pair_right.q);
    return Pair{
        .p = left,
        .q = right,
    };
}

fn binary_search(n: i32) i32 {
    var a: i32 = 0;
    var b: i32 = 1;
    while (!test_k(n, b)) {
        a = b;
        b *= 2;
    }
    while (b - a > 1) {
        const m: i32 = @divFloor(a + b, 2);
        if (test_k(n, m)) {
            b = m;
        } else {
            a = m;
        }
    }
    return b;
}

fn test_k(n: i32, k: i32) bool {
    if (k < 0) {
        return false;
    }
    const float_k: f64 = @floatFromInt(k);
    const float_n: f64 = @floatFromInt(n);
    // Stirling: ln k! ~= k ln k - k + 0.5 ln(2 pi k). (1.zig drops the
    // 0.5 ln k term, which only overestimates k slightly; the correct
    // form from 1.rs / 1.c is used here.)
    const ln_k_factorial = float_k * (math.log(f64, math.e, float_k) - 1.0) + 0.5 * math.log(f64, math.e, math.tau * float_k);
    const log_10_k_factorial = ln_k_factorial / math.ln10;
    return log_10_k_factorial >= float_n + 50.0;
}

fn get_n(init: std.process.Init) !i32 {
    var arg_it = std.process.Args.Iterator.init(init.minimal.args);
    _ = arg_it.skip();
    const arg = arg_it.next();
    if (arg) |a| {
        return try std.fmt.parseInt(i32, a, 10);
    } else {
        return 27;
    }
}

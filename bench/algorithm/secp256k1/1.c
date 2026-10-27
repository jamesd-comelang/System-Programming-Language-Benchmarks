// secp256k1 point multiplication, C port of
// bench/algorithm/secp256k1/1.rs (which uses ibig big integers).
//
// The program starts at the generator G and, n times (n = argv[1],
// default 1), replaces the point by point * PRIVATE_KEY, then
// prints the affine point as "<x hex>,<y hex>" with no leading
// zeros, exactly like Rust's {:x} formatting.
//
// Field arithmetic is fixed 256-bit (4 x uint64 limbs, little
// endian) modulo p = 2^256 - 2^32 - 977, reduced with the
// pseudo-Mersenne fold 2^256 == 2^32 + 977 (mod p). Jacobian
// coordinates use the same double/add formulas as the Rust
// version; scalar multiplication is plain double-and-add (the
// Rust version's endomorphism split is only an optimization and
// gives the same point). Inversion for the final conversion uses
// Fermat's little theorem, z^(p-2), so no extended-GCD code is
// needed. Only libc is used; unsigned __int128 carries limb
// products (GCC/Clang).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t u64;
typedef unsigned __int128 u128;

// p = FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFE FFFFFC2F
static const u64 P_LIMBS[4] = {0xFFFFFFFEFFFFFC2Full, 0xFFFFFFFFFFFFFFFFull,
                               0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
// C = 2^32 + 977, the value of 2^256 mod p.
#define FOLD_C 4294968273ull

typedef struct {
  u64 v[4];
} Fe;

// Forward declaration: fe_add uses the generic reducer.
static void fe_reduce(u64 *buf);

static int fe_is_zero(const Fe *a) {
  return a->v[0] == 0 && a->v[1] == 0 && a->v[2] == 0 && a->v[3] == 0;
}

static int fe_ge_p(const Fe *a) {
  for (int i = 3; i >= 0; i--) {
    if (a->v[i] != P_LIMBS[i]) return a->v[i] > P_LIMBS[i];
  }
  return 1;
}

// a -= p, caller guarantees a >= p.
static void fe_sub_p(Fe *a) {
  u64 borrow = 0;
  for (int i = 0; i < 4; i++) {
    u128 d = (u128)a->v[i] - P_LIMBS[i] - borrow;
    a->v[i] = (u64)d;
    borrow = (d >> 64) != 0;
  }
}

static void fe_add(Fe *r, const Fe *a, const Fe *b) {
  // Sum into a 5-limb buffer and let fe_reduce fold the carry limb
  // (2^256 == FOLD_C mod p); a plain conditional subtraction of p
  // would be wrong when the sum carries past 2^256.
  u64 buf[9] = {0};
  u64 carry = 0;
  for (int i = 0; i < 4; i++) {
    u128 s = (u128)a->v[i] + b->v[i] + carry;
    buf[i] = (u64)s;
    carry = (u64)(s >> 64);
  }
  buf[4] = carry;
  fe_reduce(buf);
  memcpy(r->v, buf, sizeof(r->v));
}

static void fe_sub(Fe *r, const Fe *a, const Fe *b) {
  // Wrapped difference t = a - b mod 2^256. If it borrowed, the
  // true value is a - b (negative) and the representative is
  // a - b + p = t - 2^256 + p = t - FOLD_C (since 2^256 - p =
  // FOLD_C). So subtract FOLD_C from t; if that borrows again,
  // add p back (the result is then < p).
  u64 borrow = 0;
  u64 t[4];
  for (int i = 0; i < 4; i++) {
    u128 d = (u128)a->v[i] - b->v[i] - borrow;
    t[i] = (u64)d;
    borrow = (d >> 64) != 0;
  }
  if (borrow) {
    u64 borrow2 = 0;
    {
      u128 d = (u128)t[0] - FOLD_C - borrow2;
      t[0] = (u64)d;
      borrow2 = (d >> 64) != 0;
    }
    for (int i = 1; i < 4; i++) {
      u128 d = (u128)t[i] - borrow2;
      t[i] = (u64)d;
      borrow2 = (d >> 64) != 0;
    }
    if (borrow2) {
      u64 carry = 0;
      for (int i = 0; i < 4; i++) {
        u128 s = (u128)t[i] + P_LIMBS[i] + carry;
        t[i] = (u64)s;
        carry = (u64)(s >> 64);
      }
    }
  }
  memcpy(r->v, t, sizeof(t));
}

static void fe_mul_u32(Fe *r, const Fe *a, uint32_t m); // defined below,
// after fe_mul, to reuse the fully reducing multiplication.

// Reduce a 9-limb little-endian value mod p, in place, by folding
// every limb at index >= 4 down through 2^256 == FOLD_C (mod p).
static void fe_reduce(u64 *buf) {
  for (int iter = 0; iter < 12; iter++) {
    int high = 0;
    for (int i = 4; i < 9; i++)
      if (buf[i]) high = 1;
    if (!high) break;
    // h = buf[4..8]; tmp = h * FOLD_C; buf[0..3] += tmp; clear h.
    u64 h[5];
    for (int i = 0; i < 5; i++) {
      h[i] = buf[4 + i];
      buf[4 + i] = 0;
    }
    u64 carry = 0;
    for (int i = 0; i < 6; i++) {
      u64 hv = i < 5 ? h[i] : 0;
      u128 t = (u128)hv * FOLD_C + carry;
      u128 s = (u128)buf[i] + (u64)t;
      buf[i] = (u64)s;
      carry = (u64)(t >> 64) + (u64)(s >> 64);
    }
    // Any carry past limb 8 is folded by the next iteration's
    // high check only if it lands at index >= 4; a carry into
    // buf[6..8] is already covered since those are >= 4. A carry
    // out of buf[8] would be lost, but the values here are far too
    // small for that (h < 2^320, h*C < 2^353, buf stays < 2^360
    // only in the first fold; subsequent folds are much smaller).
    (void)carry;
  }
  Fe r;
  memcpy(r.v, buf, sizeof(r.v));
  while (fe_ge_p(&r)) fe_sub_p(&r);
  memcpy(buf, r.v, sizeof(r.v));
}

static void fe_mul(Fe *r, const Fe *a, const Fe *b) {
  u64 prod[9] = {0};
  for (int i = 0; i < 4; i++) {
    u64 carry = 0;
    for (int j = 0; j < 4; j++) {
      u128 t = (u128)a->v[i] * b->v[j] + prod[i + j] + carry;
      prod[i + j] = (u64)t;
      carry = (u64)(t >> 64);
    }
    int k = i + 4;
    while (carry) {
      u128 t = (u128)prod[k] + carry;
      prod[k] = (u64)t;
      carry = (u64)(t >> 64);
      k++;
    }
  }
  fe_reduce(prod);
  memcpy(r->v, prod, sizeof(r->v));
}

static void fe_sqr(Fe *r, const Fe *a) { fe_mul(r, a, a); }

static void fe_mul_u32(Fe *r, const Fe *a, uint32_t m) {
  Fe mf = {{m, 0, 0, 0}};
  fe_mul(r, a, &mf);
}

// z^(p-2) mod p.
static void fe_inv(Fe *r, const Fe *z) {
  // p - 2 limbs (little-endian).
  static const u64 exp[4] = {0xFFFFFFFEFFFFFC2Dull, 0xFFFFFFFFFFFFFFFFull,
                             0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
  Fe result = {{1, 0, 0, 0}};
  Fe base = *z;
  for (int bit = 255; bit >= 0; bit--) {
    Fe sq;
    fe_sqr(&sq, &result);
    result = sq;
    if ((exp[bit / 64] >> (bit % 64)) & 1) {
      Fe pr;
      fe_mul(&pr, &result, &base);
      result = pr;
    }
  }
  *r = result;
}

// Parse a decimal string into a field element.
static void fe_from_dec(Fe *r, const char *s) {
  memset(r, 0, sizeof(*r));
  for (; *s; s++) {
    Fe t;
    fe_mul_u32(&t, r, 10);
    Fe d = {{(u64)(*s - '0'), 0, 0, 0}};
    fe_add(r, &t, &d);
  }
}

static void fe_print_hex(const Fe *a) {
  int started = 0;
  for (int i = 3; i >= 0; i--) {
    if (!started) {
      if (a->v[i] == 0) continue;
      printf("%llx", (unsigned long long)a->v[i]);
      started = 1;
    } else {
      printf("%016llx", (unsigned long long)a->v[i]);
    }
  }
  if (!started) printf("0");
}

// ---------------- Jacobian points ----------------

typedef struct {
  Fe x, y, z;
} JPoint;

static JPoint jpoint_zero(void) {
  JPoint p;
  memset(&p.x, 0, sizeof(p.x));
  p.y.v[0] = 1;
  p.y.v[1] = p.y.v[2] = p.y.v[3] = 0;
  memset(&p.z, 0, sizeof(p.z));
  return p;
}

static JPoint jpoint_double(const JPoint *p) {
  Fe a, b, c, d, e, f, x3, y3, z3, t1, t2;
  fe_sqr(&a, &p->x); // a = x^2
  fe_sqr(&b, &p->y); // b = y^2
  fe_sqr(&c, &b);    // c = b^2
  fe_add(&t1, &p->x, &b);
  fe_sqr(&t1, &t1);
  fe_sub(&t1, &t1, &a);
  fe_sub(&t1, &t1, &c);
  fe_add(&d, &t1, &t1); // d = 2*((x+b)^2 - a - c)
  fe_mul_u32(&e, &a, 3); // e = 3a
  fe_sqr(&f, &e);        // f = e^2
  fe_add(&t1, &d, &d);
  fe_sub(&x3, &f, &t1); // x3 = f - 2d
  fe_sub(&t1, &d, &x3);
  fe_mul(&t2, &e, &t1);
  fe_mul_u32(&t1, &c, 8);
  fe_sub(&y3, &t2, &t1); // y3 = e*(d - x3) - 8c
  fe_mul(&t1, &p->y, &p->z);
  fe_add(&z3, &t1, &t1); // z3 = 2*y*z
  JPoint r = {x3, y3, z3};
  return r;
}

static JPoint jpoint_add(const JPoint *p, const JPoint *q) {
  if (fe_is_zero(&q->x) || fe_is_zero(&q->y)) return *p;
  if (fe_is_zero(&p->x) || fe_is_zero(&p->y)) return *q;
  Fe z1z1, z2z2, u1, u2, s1, s2, h, r_, hh, hhh, v, x3, y3, z3, t1, t2;
  fe_sqr(&z1z1, &p->z);
  fe_sqr(&z2z2, &q->z);
  fe_mul(&t1, &p->x, &z2z2);
  u1 = t1;
  fe_mul(&t1, &q->x, &z1z1);
  u2 = t1;
  fe_mul(&t1, &p->y, &q->z);
  fe_mul(&s1, &t1, &z2z2);
  fe_mul(&t1, &q->y, &p->z);
  fe_mul(&s2, &t1, &z1z1);
  fe_sub(&h, &u2, &u1);
  fe_sub(&r_, &s2, &s1);
  if (fe_is_zero(&h)) {
    if (fe_is_zero(&r_)) return jpoint_double(p);
    return jpoint_zero();
  }
  fe_sqr(&hh, &h);
  fe_mul(&hhh, &h, &hh);
  fe_mul(&v, &u1, &hh);
  fe_sqr(&t1, &r_);
  fe_sub(&t1, &t1, &hhh);
  fe_add(&t2, &v, &v);
  fe_sub(&x3, &t1, &t2);
  fe_sub(&t1, &v, &x3);
  fe_mul(&t2, &r_, &t1);
  fe_mul(&t1, &s1, &hhh);
  fe_sub(&y3, &t2, &t1);
  fe_mul(&t1, &p->z, &q->z);
  fe_mul(&z3, &t1, &h);
  JPoint res = {x3, y3, z3};
  return res;
}

static void jpoint_to_affine(Fe *ox, Fe *oy, const JPoint *p) {
  Fe inv_z, inv_z2, t;
  fe_inv(&inv_z, &p->z);
  fe_sqr(&inv_z2, &inv_z);
  fe_mul(ox, &p->x, &inv_z2);
  fe_mul(&t, &inv_z, &inv_z2);
  fe_mul(oy, &p->y, &t);
}

int main(int argc, char **argv) {
  int n = 1;
  if (argc > 1) n = atoi(argv[1]);

  Fe gx, gy, priv;
  fe_from_dec(&gx,
              "55066263022277343669578718895168534326250603453777594175500187360389116729240");
  fe_from_dec(&gy,
              "32670510020758816978083085130507043184471273380659243275938904335757337482424");
  fe_from_dec(&priv,
              "20775598474904240222758871485654738649026525153462921990999819694398496339603");

  Fe px = gx, py = gy;
  for (int iter = 0; iter < n; iter++) {
    // Scalar-multiply the current affine point by priv.
    JPoint base = {px, py, {{1, 0, 0, 0}}};
    JPoint acc = jpoint_zero();
    for (int bit = 255; bit >= 0; bit--) {
      acc = jpoint_double(&acc);
      if ((priv.v[bit / 64] >> (bit % 64)) & 1) acc = jpoint_add(&acc, &base);
    }
    jpoint_to_affine(&px, &py, &acc);
  }
  fe_print_hex(&px);
  printf(",");
  fe_print_hex(&py);
  printf("\n");
  return 0;
}

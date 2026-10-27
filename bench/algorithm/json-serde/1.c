// JSON serde, standalone C port of bench/algorithm/json-serde/1.go.
//
// Reads <file>.json (a GeoJSON FeatureCollection), parses only the
// fields the Go struct keeps (type, features[].type,
// features[].properties.name, features[].geometry.type and
// .coordinates), re-serializes in struct order with Go's
// encoding/json float and string rules, and prints the MD5 of that
// serialization, then the MD5 of an array of n copies of it.
// The MD5 implementation is self-contained (RFC 1321, round
// constants computed from sin() as in mandelbrot/1.c); only
// libc/libm are used.
//
// Argument handling matches the bench harness: argv[1] is the file
// stem (the harness passes a temp-dir path like .../sample and the
// program appends ".json"); if argv[1] already ends in ".json" it
// is used as-is. argv[2] is the repeat count (default 10).

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------- MD5 (RFC 1321) ----------------

typedef struct {
  uint32_t a, b, c, d;
  uint64_t len;
  uint8_t buf[64];
  size_t buflen;
} Md5;

static uint32_t rol32(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
static uint32_t md5_k[64];
static const int md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

static void md5_block(Md5 *m, const uint8_t *p) {
  uint32_t w[16];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
           ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
  uint32_t a = m->a, b = m->b, c = m->c, d = m->d;
  for (int i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    uint32_t tmp = d;
    d = c;
    c = b;
    b = b + rol32(a + f + md5_k[i] + w[g], md5_s[i]);
    a = tmp;
  }
  m->a += a;
  m->b += b;
  m->c += c;
  m->d += d;
}

static void md5_init(Md5 *m) {
  static int k_done = 0;
  if (!k_done) {
    for (int i = 0; i < 64; i++)
      md5_k[i] = (uint32_t)(fabs(sin((double)(i + 1))) * 4294967296.0);
    k_done = 1;
  }
  m->a = 0x67452301;
  m->b = 0xefcdab89;
  m->c = 0x98badcfe;
  m->d = 0x10325476;
  m->len = 0;
  m->buflen = 0;
}

static void md5_update(Md5 *m, const uint8_t *data, size_t len) {
  m->len += len;
  while (len > 0) {
    size_t take = 64 - m->buflen;
    if (take > len) take = len;
    memcpy(m->buf + m->buflen, data, take);
    m->buflen += take;
    data += take;
    len -= take;
    if (m->buflen == 64) {
      md5_block(m, m->buf);
      m->buflen = 0;
    }
  }
}

static void md5_final(Md5 *m, uint8_t out[16]) {
  uint64_t bitlen = m->len * 8;
  uint8_t pad = 0x80;
  md5_update(m, &pad, 1);
  uint8_t zero = 0;
  while (m->buflen != 56) md5_update(m, &zero, 1);
  for (int i = 0; i < 8; i++) {
    m->buf[m->buflen++] = (uint8_t)(bitlen >> (8 * i));
    if (m->buflen == 64) {
      md5_block(m, m->buf);
      m->buflen = 0;
    }
  }
  uint32_t v[4] = {m->a, m->b, m->c, m->d};
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(v[i] >> (8 * j));
}

static void print_md5_hex(Md5 *m) {
  uint8_t d[16];
  md5_final(m, d);
  for (int i = 0; i < 16; i++) printf("%02x", d[i]);
  printf("\n");
}

// ---------------- JSON parsing ----------------

typedef struct {
  const char *p;
} Parser;

static void skip_ws(Parser *ps) {
  while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
    ps->p++;
}

static void buf_putc(char **buf, size_t *len, size_t *cap, char c) {
  if (*len + 1 >= *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *buf = realloc(*buf, *cap);
  }
  (*buf)[(*len)++] = c;
}

static void buf_put_utf8(char **buf, size_t *len, size_t *cap,
                         unsigned cp) {
  if (cp < 0x80) {
    buf_putc(buf, len, cap, (char)cp);
  } else if (cp < 0x800) {
    buf_putc(buf, len, cap, (char)(0xC0 | (cp >> 6)));
    buf_putc(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    buf_putc(buf, len, cap, (char)(0xE0 | (cp >> 12)));
    buf_putc(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
  } else {
    buf_putc(buf, len, cap, (char)(0xF0 | (cp >> 18)));
    buf_putc(buf, len, cap, (char)(0x80 | ((cp >> 12) & 0x3F)));
    buf_putc(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_putc(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
  }
}

// Parse a JSON string (cursor on '"'), return a malloc'd UTF-8 string.
static char *parse_string(Parser *ps) {
  char *buf = NULL;
  size_t len = 0, cap = 0;
  ps->p++; // opening quote
  while (*ps->p && *ps->p != '"') {
    if (*ps->p == '\\') {
      ps->p++;
      char e = *ps->p++;
      switch (e) {
      case '"': buf_putc(&buf, &len, &cap, '"'); break;
      case '\\': buf_putc(&buf, &len, &cap, '\\'); break;
      case '/': buf_putc(&buf, &len, &cap, '/'); break;
      case 'b': buf_putc(&buf, &len, &cap, '\b'); break;
      case 'f': buf_putc(&buf, &len, &cap, '\f'); break;
      case 'n': buf_putc(&buf, &len, &cap, '\n'); break;
      case 'r': buf_putc(&buf, &len, &cap, '\r'); break;
      case 't': buf_putc(&buf, &len, &cap, '\t'); break;
      case 'u': {
        unsigned cp = (unsigned)strtoul(ps->p, NULL, 16);
        ps->p += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' &&
            ps->p[1] == 'u') {
          unsigned lo = (unsigned)strtoul(ps->p + 2, NULL, 16);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ps->p += 6;
          }
        }
        buf_put_utf8(&buf, &len, &cap, cp);
        break;
      }
      default: buf_putc(&buf, &len, &cap, e); break;
      }
    } else {
      buf_putc(&buf, &len, &cap, *ps->p++);
    }
  }
  if (*ps->p == '"') ps->p++;
  buf_putc(&buf, &len, &cap, '\0');
  return buf;
}

static void skip_value(Parser *ps) {
  skip_ws(ps);
  if (*ps->p == '"') {
    char *s = parse_string(ps);
    free(s);
  } else if (*ps->p == '{') {
    ps->p++;
    skip_ws(ps);
    if (*ps->p == '}') {
      ps->p++;
      return;
    }
    for (;;) {
      skip_ws(ps);
      char *k = parse_string(ps);
      free(k);
      skip_ws(ps);
      if (*ps->p == ':') ps->p++;
      skip_value(ps);
      skip_ws(ps);
      if (*ps->p == ',') {
        ps->p++;
        continue;
      }
      if (*ps->p == '}') ps->p++;
      break;
    }
  } else if (*ps->p == '[') {
    ps->p++;
    skip_ws(ps);
    if (*ps->p == ']') {
      ps->p++;
      return;
    }
    for (;;) {
      skip_value(ps);
      skip_ws(ps);
      if (*ps->p == ',') {
        ps->p++;
        continue;
      }
      if (*ps->p == ']') ps->p++;
      break;
    }
  } else {
    while (*ps->p && *ps->p != ',' && *ps->p != '}' && *ps->p != ']' &&
           !isspace((unsigned char)*ps->p))
      ps->p++;
  }
}

// ---------------- GeoData ----------------

typedef struct {
  double x, y;
} Point;

typedef struct {
  Point *pts;
  size_t npts, cap;
} Polygon;

typedef struct {
  char *type; // feature type
  char *name;
  char *gtype; // geometry type
  Polygon *polys;
  size_t npolys, capp;
} Feature;

static char *g_type = NULL;
static Feature *features = NULL;
static size_t nfeat = 0, capfeat = 0;

static void poly_add_pt(Polygon *poly, double x, double y) {
  if (poly->npts == poly->cap) {
    poly->cap = poly->cap ? poly->cap * 2 : 64;
    poly->pts = realloc(poly->pts, poly->cap * sizeof(Point));
  }
  poly->pts[poly->npts].x = x;
  poly->pts[poly->npts].y = y;
  poly->npts++;
}

static void parse_geometry(Parser *ps, Feature *f) {
  skip_ws(ps);
  if (*ps->p != '{') {
    skip_value(ps);
    return;
  }
  ps->p++;
  skip_ws(ps);
  if (*ps->p == '}') {
    ps->p++;
    return;
  }
  for (;;) {
    skip_ws(ps);
    char *k = parse_string(ps);
    skip_ws(ps);
    if (*ps->p == ':') ps->p++;
    if (strcmp(k, "type") == 0) {
      skip_ws(ps);
      f->gtype = parse_string(ps);
    } else if (strcmp(k, "coordinates") == 0) {
      skip_ws(ps);
      if (*ps->p == '[') {
        ps->p++;
        skip_ws(ps);
        if (*ps->p != ']') {
          for (;;) {
            // One polygon: array of [x, y] points.
            if (f->npolys == f->capp) {
              f->capp = f->capp ? f->capp * 2 : 16;
              f->polys = realloc(f->polys, f->capp * sizeof(Polygon));
            }
            Polygon *poly = &f->polys[f->npolys];
            memset(poly, 0, sizeof(*poly));
            f->npolys++;
            skip_ws(ps);
            if (*ps->p == '[') {
              ps->p++;
              skip_ws(ps);
              if (*ps->p != ']') {
                for (;;) {
                  skip_ws(ps);
                  double x = 0, y = 0;
                  if (*ps->p == '[') {
                    ps->p++;
                    skip_ws(ps);
                    x = strtod(ps->p, (char **)&ps->p);
                    skip_ws(ps);
                    if (*ps->p == ',') ps->p++;
                    skip_ws(ps);
                    y = strtod(ps->p, (char **)&ps->p);
                    skip_ws(ps);
                    if (*ps->p == ']') ps->p++;
                  } else {
                    skip_value(ps);
                  }
                  poly_add_pt(poly, x, y);
                  skip_ws(ps);
                  if (*ps->p == ',') {
                    ps->p++;
                    continue;
                  }
                  if (*ps->p == ']') ps->p++;
                  break;
                }
              } else {
                ps->p++;
              }
            } else {
              skip_value(ps);
            }
            skip_ws(ps);
            if (*ps->p == ',') {
              ps->p++;
              continue;
            }
            if (*ps->p == ']') ps->p++;
            break;
          }
        } else {
          ps->p++;
        }
      } else {
        skip_value(ps);
      }
    } else {
      skip_value(ps);
    }
    free(k);
    skip_ws(ps);
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == '}') ps->p++;
    break;
  }
}

static void parse_feature(Parser *ps) {
  if (nfeat == capfeat) {
    capfeat = capfeat ? capfeat * 2 : 16;
    features = realloc(features, capfeat * sizeof(Feature));
  }
  Feature *f = &features[nfeat];
  memset(f, 0, sizeof(*f));
  nfeat++;
  skip_ws(ps);
  if (*ps->p != '{') {
    skip_value(ps);
    return;
  }
  ps->p++;
  skip_ws(ps);
  if (*ps->p == '}') {
    ps->p++;
    return;
  }
  for (;;) {
    skip_ws(ps);
    char *k = parse_string(ps);
    skip_ws(ps);
    if (*ps->p == ':') ps->p++;
    if (strcmp(k, "type") == 0) {
      skip_ws(ps);
      f->type = parse_string(ps);
    } else if (strcmp(k, "properties") == 0) {
      skip_ws(ps);
      if (*ps->p == '{') {
        ps->p++;
        skip_ws(ps);
        if (*ps->p != '}') {
          for (;;) {
            skip_ws(ps);
            char *pk = parse_string(ps);
            skip_ws(ps);
            if (*ps->p == ':') ps->p++;
            if (strcmp(pk, "name") == 0) {
              skip_ws(ps);
              f->name = parse_string(ps);
            } else {
              skip_value(ps);
            }
            free(pk);
            skip_ws(ps);
            if (*ps->p == ',') {
              ps->p++;
              continue;
            }
            if (*ps->p == '}') ps->p++;
            break;
          }
        } else {
          ps->p++;
        }
      } else {
        skip_value(ps);
      }
    } else if (strcmp(k, "geometry") == 0) {
      parse_geometry(ps, f);
    } else {
      skip_value(ps);
    }
    free(k);
    skip_ws(ps);
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == '}') ps->p++;
    break;
  }
}

static void parse_geo(Parser *ps) {
  skip_ws(ps);
  if (*ps->p != '{') return;
  ps->p++;
  skip_ws(ps);
  if (*ps->p == '}') {
    ps->p++;
    return;
  }
  for (;;) {
    skip_ws(ps);
    char *k = parse_string(ps);
    skip_ws(ps);
    if (*ps->p == ':') ps->p++;
    if (strcmp(k, "type") == 0) {
      skip_ws(ps);
      g_type = parse_string(ps);
    } else if (strcmp(k, "features") == 0) {
      skip_ws(ps);
      if (*ps->p == '[') {
        ps->p++;
        skip_ws(ps);
        if (*ps->p != ']') {
          for (;;) {
            parse_feature(ps);
            skip_ws(ps);
            if (*ps->p == ',') {
              ps->p++;
              continue;
            }
            if (*ps->p == ']') ps->p++;
            break;
          }
        } else {
          ps->p++;
        }
      } else {
        skip_value(ps);
      }
    } else {
      skip_value(ps);
    }
    free(k);
    skip_ws(ps);
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == '}') ps->p++;
    break;
  }
}

// ---------------- Serialization (Go encoding/json rules) ----------------

typedef struct {
  char *buf;
  size_t len, cap;
} Str;

static void str_putc(Str *s, char c) {
  if (s->len + 1 >= s->cap) {
    s->cap = s->cap ? s->cap * 2 : 256;
    s->buf = realloc(s->buf, s->cap);
  }
  s->buf[s->len++] = c;
}

static void str_puts(Str *s, const char *t) {
  while (*t) str_putc(s, *t++);
}

// Go json string escaping: ", \, control chars, plus <, >, & as
// \u003c etc, and U+2028/U+2029.
static void str_put_escaped(Str *s, const char *t) {
  str_putc(s, '"');
  for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
    unsigned char c = *p;
    switch (c) {
    case '"': str_puts(s, "\\\""); break;
    case '\\': str_puts(s, "\\\\"); break;
    case '\n': str_puts(s, "\\n"); break;
    case '\r': str_puts(s, "\\r"); break;
    case '\t': str_puts(s, "\\t"); break;
    case '<': str_puts(s, "\\u003c"); break;
    case '>': str_puts(s, "\\u003e"); break;
    case '&': str_puts(s, "\\u0026"); break;
    default:
      if (c < 0x20) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        str_puts(s, tmp);
      } else if (c == 0xE2 && p[1] == 0x80 &&
                 (p[2] == 0xA8 || p[2] == 0xA9)) {
        str_puts(s, p[2] == 0xA8 ? "\\u2028" : "\\u2029");
        p += 2;
      } else {
        str_putc(s, (char)c);
      }
      break;
    }
  }
  str_putc(s, '"');
}

// Go json float: shortest digits that round-trip; 'f' format for
// 1e-6 <= |v| < 1e21, otherwise 'e' (with e-09 -> e-9 cleanup).
static void str_put_float(Str *s, double v) {
  if (v == 0) {
    // Go prints "-0" for negative zero, "0" otherwise.
    str_puts(s, signbit(v) ? "-0" : "0");
    return;
  }
  // Find the shortest digit string via %e at increasing precision.
  char ebuf[64];
  char digits[32];
  int ndig = 0, exp10 = 0;
  for (int prec = 1; prec <= 17; prec++) {
    snprintf(ebuf, sizeof(ebuf), "%.*e", prec - 1, v);
    double back = strtod(ebuf, NULL);
    if (back == v) {
      // ebuf looks like "-d.dddde+XX" (sign optional).
      char *epos = strchr(ebuf, 'e');
      exp10 = atoi(epos + 1);
      int di = 0;
      for (char *p = ebuf; p < epos; p++) {
        if (*p == '-' || *p == '.' || *p == '+') continue;
        digits[di++] = *p;
      }
      // Strip trailing zeros (shortest digits never need them, but
      // %e at this precision may have produced some).
      while (di > 1 && digits[di - 1] == '0') di--;
      ndig = di;
      break;
    }
  }
  int neg = v < 0;
  double av = neg ? -v : v;
  if (av >= 1e-6 && av < 1e21) {
    if (neg) str_putc(s, '-');
    if (exp10 >= 0) {
      // Integer part: digits[0..exp10], then fraction.
      for (int i = 0; i <= exp10; i++)
        str_putc(s, i < ndig ? digits[i] : '0');
      if (ndig > exp10 + 1) {
        str_putc(s, '.');
        for (int i = exp10 + 1; i < ndig; i++) str_putc(s, digits[i]);
      }
    } else {
      str_puts(s, "0.");
      for (int i = 0; i < -exp10 - 1; i++) str_putc(s, '0');
      for (int i = 0; i < ndig; i++) str_putc(s, digits[i]);
    }
  } else {
    if (neg) str_putc(s, '-');
    str_putc(s, digits[0]);
    if (ndig > 1) {
      str_putc(s, '.');
      for (int i = 1; i < ndig; i++) str_putc(s, digits[i]);
    }
    char tmp[16];
    // Go's strconv 'e' uses at least two exponent digits; the json
    // encoder then collapses e-09 to e-9 (negative only).
    if (exp10 >= 0)
      snprintf(tmp, sizeof(tmp), "e+%02d", exp10);
    else if (exp10 >= -9)
      snprintf(tmp, sizeof(tmp), "e-%d", -exp10);
    else
      snprintf(tmp, sizeof(tmp), "e-%02d", -exp10);
    str_puts(s, tmp);
  }
}

static void serialize_geo(Str *s) {
  str_puts(s, "{\"type\":");
  str_put_escaped(s, g_type ? g_type : "");
  str_puts(s, ",\"features\":[");
  for (size_t fi = 0; fi < nfeat; fi++) {
    Feature *f = &features[fi];
    if (fi) str_putc(s, ',');
    str_puts(s, "{\"type\":");
    str_put_escaped(s, f->type ? f->type : "");
    str_puts(s, ",\"properties\":{\"name\":");
    str_put_escaped(s, f->name ? f->name : "");
    str_puts(s, "},\"geometry\":{\"type\":");
    str_put_escaped(s, f->gtype ? f->gtype : "");
    str_puts(s, ",\"coordinates\":[");
    for (size_t pi = 0; pi < f->npolys; pi++) {
      if (pi) str_putc(s, ',');
      str_putc(s, '[');
      Polygon *poly = &f->polys[pi];
      for (size_t qi = 0; qi < poly->npts; qi++) {
        if (qi) str_putc(s, ',');
        str_putc(s, '[');
        str_put_float(s, poly->pts[qi].x);
        str_putc(s, ',');
        str_put_float(s, poly->pts[qi].y);
        str_putc(s, ']');
      }
      str_putc(s, ']');
    }
    str_puts(s, "]}}");
  }
  str_puts(s, "]}");
}

int main(int argc, char **argv) {
  const char *stem = "sample";
  int n = 10;
  if (argc > 1) stem = argv[1];
  if (argc > 2) n = atoi(argv[2]);

  char path[4096];
  size_t sl = strlen(stem);
  if (sl >= 5 && strcmp(stem + sl - 5, ".json") == 0)
    snprintf(path, sizeof(path), "%s", stem);
  else
    snprintf(path, sizeof(path), "%s.json", stem);
  FILE *fp = fopen(path, "rb");
  if (!fp) {
    fprintf(stderr, "json-serde: cannot open %s\n", path);
    return 1;
  }
  fseek(fp, 0, SEEK_END);
  long fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  char *text = malloc((size_t)fsize + 1);
  if (fread(text, 1, (size_t)fsize, fp) != (size_t)fsize) {
    fclose(fp);
    return 1;
  }
  fclose(fp);
  text[fsize] = '\0';

  Parser ps = {text};
  parse_geo(&ps);

  Str s = {0};
  serialize_geo(&s);

  Md5 m;
  md5_init(&m);
  md5_update(&m, (uint8_t *)s.buf, s.len);
  print_md5_hex(&m);

  md5_init(&m);
  md5_update(&m, (const uint8_t *)"[", 1);
  for (int i = 0; i < n; i++) {
    if (i) md5_update(&m, (const uint8_t *)",", 1);
    md5_update(&m, (uint8_t *)s.buf, s.len);
  }
  md5_update(&m, (const uint8_t *)"]", 1);
  print_md5_hex(&m);
  return 0;
}

// json-serde, ported from 1.go and 2.rs (same output format).
//
// Behaviour (matching both sources):
//   argv[1] = file stem (Go/Rust append ".json"; a trailing ".json" is
//             also accepted, so a full file path works too),
//   argv[2] = count, default 10 (file default "sample").
//   Parse the file once, serialise the single GeoData value in struct
//   field order with compact separators, print its MD5 (lower hex).
//   Then parse the file `count` more times, serialise the array of
//   those values, print its MD5.
// Number serialisation follows 2.rs's MyF64: an integral value is
// emitted as an integer; otherwise the shortest round-trip decimal
// (std::to_chars, general format), which matches serde_json/Go
// encoding for the values in these data files.
// MD5 and the JSON parser are self-contained; stdlib only, C++20.

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ---------------------------------------------------------------- MD5
// RFC 1321 MD5, self-contained.

namespace {

class Md5 {
 public:
  Md5() { reset(); }
  void reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xefcdab89u;
    h_[2] = 0x98badcfeu;
    h_[3] = 0x10325476u;
    len_ = 0;
    buf_len_ = 0;
  }
  void update(const uint8_t* data, size_t size) {
    len_ += size;
    while (size > 0) {
      size_t take = 64 - buf_len_;
      if (take > size) take = size;
      std::memcpy(buf_ + buf_len_, data, take);
      buf_len_ += take;
      data += take;
      size -= take;
      if (buf_len_ == 64) {
        compress(buf_);
        buf_len_ = 0;
      }
    }
  }
  void update(const std::string& s) {
    update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  std::array<uint8_t, 16> finish() {
    uint64_t bit_len = len_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++)
      len_bytes[i] = static_cast<uint8_t>((bit_len >> (8 * i)) & 0xffu);
    // Feed the length bytes without changing len_ bookkeeping further:
    // update() above already advanced len_, but that no longer matters.
    for (int i = 0; i < 8; i++) {
      buf_[buf_len_++] = len_bytes[i];
      if (buf_len_ == 64) {
        compress(buf_);
        buf_len_ = 0;
      }
    }
    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 4; i++)
      for (int j = 0; j < 4; j++)
        out[4 * i + j] = static_cast<uint8_t>((h_[i] >> (8 * j)) & 0xffu);
    return out;
  }

 private:
  static uint32_t rotl(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
  void compress(const uint8_t* block) {
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
        0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
        0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
        0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
        0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
        0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
        0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
        0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
    static const int s[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
      m[i] = static_cast<uint32_t>(block[4 * i]) |
             (static_cast<uint32_t>(block[4 * i + 1]) << 8) |
             (static_cast<uint32_t>(block[4 * i + 2]) << 16) |
             (static_cast<uint32_t>(block[4 * i + 3]) << 24);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
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
      b = b + rotl(a + f + k[i] + m[g], s[i]);
      a = tmp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
  }
  uint32_t h_[4];
  uint64_t len_ = 0;
  uint8_t buf_[64];
  size_t buf_len_ = 0;
};

std::string md5_hex(const std::string& s) {
  Md5 md;
  md.update(s);
  auto digest = md.finish();
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (uint8_t byte : digest) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0xf]);
  }
  return out;
}

void print_hash(const std::string& s) {
  std::cout << md5_hex(s) << '\n';
}

// ---------------------------------------------------------------- Data

struct Properties {
  std::string name;
};
struct Geometry {
  std::string type;
  std::vector<std::vector<std::array<double, 2>>> coordinates;
};
struct Feature {
  std::string type;
  Properties properties;
  Geometry geometry;
};
struct GeoData {
  std::string type;
  std::vector<Feature> features;
};

// ---------------------------------------------------------------- JSON parser
// Minimal parser for the GeoData schema; unknown object keys are skipped
// with a generic value parser.

struct Parser {
  std::string_view s;
  size_t pos = 0;

  [[noreturn]] void fail(const char* msg) {
    std::cerr << "json parse error at " << pos << ": " << msg << '\n';
    std::exit(1);
  }
  void ws() {
    while (pos < s.size() &&
           (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
      pos++;
  }
  char peek() {
    ws();
    if (pos >= s.size()) fail("unexpected end");
    return s[pos];
  }
  char get() {
    char c = peek();
    pos++;
    return c;
  }
  void expect(char c) {
    if (get() != c) fail("unexpected character");
  }
  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xc0u | (cp >> 6)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xe0u | (cp >> 12)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    } else {
      out.push_back(static_cast<char>(0xf0u | (cp >> 18)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
      out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
    }
  }
  uint32_t hex4() {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
      if (pos >= s.size()) fail("bad \\u escape");
      char c = s[pos++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        fail("bad hex digit");
    }
    return v;
  }
  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos >= s.size()) fail("unterminated string");
      char c = s[pos++];
      if (c == '"') break;
      if (c == '\\') {
        if (pos >= s.size()) fail("bad escape");
        char e = s[pos++];
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = hex4();
            if (cp >= 0xd800 && cp <= 0xdbff && pos + 1 < s.size() &&
                s[pos] == '\\' && s[pos + 1] == 'u') {
              pos += 2;
              uint32_t lo = hex4();
              if (lo >= 0xdc00 && lo <= 0xdfff)
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
            }
            append_utf8(out, cp);
            break;
          }
          default: fail("bad escape");
        }
      } else {
        out.push_back(c);
      }
    }
    return out;
  }
  double parse_number() {
    ws();
    size_t start = pos;
    if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) pos++;
    while (pos < s.size() &&
           ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' ||
            s[pos] == 'e' || s[pos] == 'E' || s[pos] == '-' || s[pos] == '+'))
      pos++;
    if (start == pos) fail("expected number");
    std::string tok(s.substr(start, pos - start));
    return std::strtod(tok.c_str(), nullptr);
  }
  void skip_value() {
    char c = peek();
    if (c == '"') {
      (void)parse_string();
    } else if (c == '{') {
      expect('{');
      if (peek() == '}') {
        pos++;
        return;
      }
      while (true) {
        (void)parse_string();
        expect(':');
        skip_value();
        char d = get();
        if (d == '}') break;
        if (d != ',') fail("expected , or }");
      }
    } else if (c == '[') {
      expect('[');
      if (peek() == ']') {
        pos++;
        return;
      }
      while (true) {
        skip_value();
        char d = get();
        if (d == ']') break;
        if (d != ',') fail("expected , or ]");
      }
    } else if (c == 't') {
      if (s.substr(pos, 4) != "true") fail("bad literal");
      pos += 4;
    } else if (c == 'f') {
      if (s.substr(pos, 5) != "false") fail("bad literal");
      pos += 5;
    } else if (c == 'n') {
      if (s.substr(pos, 4) != "null") fail("bad literal");
      pos += 4;
    } else {
      (void)parse_number();
    }
  }
  // Parses an object, calling `on_key(key)` for each key; the callback
  // must consume the value (or call skip_value()).
  template <typename F>
  void parse_object(F&& on_key) {
    expect('{');
    if (peek() == '}') {
      pos++;
      return;
    }
    while (true) {
      std::string key = parse_string();
      expect(':');
      on_key(std::move(key));
      char d = get();
      if (d == '}') break;
      if (d != ',') fail("expected , or }");
    }
  }
  Properties parse_properties() {
    Properties p;
    parse_object([&](std::string key) {
      if (key == "name")
        p.name = parse_string();
      else
        skip_value();
    });
    return p;
  }
  Geometry parse_geometry() {
    Geometry g;
    parse_object([&](std::string key) {
      if (key == "type") {
        g.type = parse_string();
      } else if (key == "coordinates") {
        expect('[');
        if (peek() != ']') {
          while (true) {
            std::vector<std::array<double, 2>> ring;
            expect('[');
            if (peek() != ']') {
              while (true) {
                expect('[');
                double x = parse_number();
                expect(',');
                double y = parse_number();
                expect(']');
                ring.push_back({x, y});
                char d = get();
                if (d == ']') break;
                if (d != ',') fail("expected , or ]");
              }
            } else {
              pos++;
            }
            g.coordinates.push_back(std::move(ring));
            char d = get();
            if (d == ']') break;
            if (d != ',') fail("expected , or ]");
          }
        } else {
          pos++;
        }
      } else {
        skip_value();
      }
    });
    return g;
  }
  Feature parse_feature() {
    Feature f;
    parse_object([&](std::string key) {
      if (key == "type")
        f.type = parse_string();
      else if (key == "properties")
        f.properties = parse_properties();
      else if (key == "geometry")
        f.geometry = parse_geometry();
      else
        skip_value();
    });
    return f;
  }
  GeoData parse_geo() {
    GeoData d;
    parse_object([&](std::string key) {
      if (key == "type") {
        d.type = parse_string();
      } else if (key == "features") {
        expect('[');
        if (peek() != ']') {
          while (true) {
            d.features.push_back(parse_feature());
            char c = get();
            if (c == ']') break;
            if (c != ',') fail("expected , or ]");
          }
        } else {
          pos++;
        }
      } else {
        skip_value();
      }
    });
    return d;
  }
};

// ---------------------------------------------------------------- Serialiser

void append_json_string(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[c >> 4]);
          out.push_back(hex[c & 0xf]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void append_number(std::string& out, double v) {
  // MyF64 (2.rs): integral values are emitted as integers.
  if (std::trunc(v) == v && std::fabs(v) < 9.0e18) {
    char buf[32];
    auto res = std::to_chars(buf, buf + sizeof(buf),
                             static_cast<long long>(v));
    out.append(buf, res.ptr);
    return;
  }
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, res.ptr);
}

std::string to_json_string(const GeoData& d) {
  std::string out;
  out += "{\"type\":";
  append_json_string(out, d.type);
  out += ",\"features\":[";
  bool first_f = true;
  for (const auto& f : d.features) {
    if (!first_f) out.push_back(',');
    first_f = false;
    out += "{\"type\":";
    append_json_string(out, f.type);
    out += ",\"properties\":{\"name\":";
    append_json_string(out, f.properties.name);
    out += "},\"geometry\":{\"type\":";
    append_json_string(out, f.geometry.type);
    out += ",\"coordinates\":[";
    bool first_r = true;
    for (const auto& ring : f.geometry.coordinates) {
      if (!first_r) out.push_back(',');
      first_r = false;
      out.push_back('[');
      bool first_p = true;
      for (const auto& pt : ring) {
        if (!first_p) out.push_back(',');
        first_p = false;
        out.push_back('[');
        append_number(out, pt[0]);
        out.push_back(',');
        append_number(out, pt[1]);
        out.push_back(']');
      }
      out.push_back(']');
    }
    out += "]}}";
  }
  out += "]}";
  return out;
}

std::string to_json_string(const std::vector<GeoData>& arr) {
  std::string out = "[";
  bool first = true;
  for (const auto& d : arr) {
    if (!first) out.push_back(',');
    first = false;
    out += to_json_string(d);
  }
  out.push_back(']');
  return out;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "cannot open " << path << '\n';
    std::exit(1);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string file_name = "sample";
  if (argc > 1) file_name = argv[1];
  int n = 10;
  if (argc > 2) n = std::stoi(argv[2]);
  std::string path = file_name;
  if (path.size() < 5 || path.substr(path.size() - 5) != ".json")
    path += ".json";
  std::string json_str = read_file(path);

  {
    Parser p{json_str, 0};
    GeoData data = p.parse_geo();
    print_hash(to_json_string(data));
  }
  std::vector<GeoData> array;
  if (n > 0) array.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    Parser p{json_str, 0};
    array.push_back(p.parse_geo());
  }
  print_hash(to_json_string(array));
  return 0;
}

// pjson v1 spec conformance driver — C++ counterpart of
// rust/psio/src/bin/pjson_conformance_driver.rs.
//
// Reads a fixture JSON on stdin in the format documented at
// conformance/README.md, runs encode + decode + JSON projection
// against the post-audit pjson v1 wire format
// (docs/pjson-spec.md), and reports a verdict.
//
// Self-contained: no dependency on the pre-audit pjson library
// code in include/psio/pjson*.hpp. The two drivers (this one and
// the Rust binary) consume the same fixture files and must
// produce byte-identical wire output.
//
// CLI:
//   pjson_conformance_driver --check     < fixture.json   exit 0 = pass
//   pjson_conformance_driver --xvalidate < fixture.json   wire+json on stdout
//
// Phase 1 scope: tag dispatch, null, bool, uint_inline,
// nint_inline, uint, negint to 128-bit magnitude, ieee_float
// widths 16/32/64/128, decimal with all four varscale tiers.
//
// binary128 widening uses the vendored Berkeley SoftFloat-3e
// subset at cpp/external/softfloat/psio_softfloat.h.

#define PSIO_SOFTFLOAT_IMPL
#include <psio_softfloat.h>

// xxhash for §5.3 prefilter hash. The vendored single-header is at
// cpp/external/xxhash/. XXH_INLINE_ALL pulls the impls into this TU.
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <charconv>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pjson_conformance {

// ── Spec-defined Value model ───────────────────────────────────────

// 128-bit unsigned: __int128 is GCC/Clang; we use a manual struct
// for portability and so the type is a regular value type that fits
// in any std::variant arm.
struct U128 {
   std::uint64_t lo{};
   std::uint64_t hi{};

   bool operator==(const U128&) const = default;
   bool is_zero() const noexcept { return lo == 0 && hi == 0; }
   bool fits_u64() const noexcept { return hi == 0; }
};

// 128-bit signed for the decimal mantissa.
struct I128 {
   std::uint64_t lo{};
   std::int64_t  hi{};   // sign bit lives in hi's MSB

   bool operator==(const I128&) const = default;
   bool is_zero() const noexcept { return lo == 0 && hi == 0; }
   bool is_negative() const noexcept { return hi < 0; }
};

struct Null {
   bool operator==(const Null&) const = default;
};
struct Bool {
   bool value{};
   bool operator==(const Bool&) const = default;
};
struct Uint {
   U128 value{};
   bool operator==(const Uint&) const = default;
};
struct NegInt {
   U128 magnitude{};   // value = −magnitude; magnitude > 0
   bool operator==(const NegInt&) const = default;
};
struct Float {
   std::uint8_t  width_log2{};   // 1=binary16, 2=binary32, 3=binary64, 4=binary128
   U128          bits{};         // raw IEEE little-endian bit pattern
   bool operator==(const Float&) const = default;
};
struct Decimal {
   I128         mantissa{};
   std::int32_t scale{};
   bool operator==(const Decimal&) const = default;
};

// `Array` holds a recursive `std::vector<Value>`. The forward-declared
// body lets the variant be sized (Array is a wrapper around a single
// shared_ptr) while still letting Array's body refer back to Value
// once the variant is fully declared.
struct ArrayBody;
struct Array {
   std::shared_ptr<ArrayBody> body;
   bool operator==(const Array& other) const;
};

// Typed homogeneous array (§5.1.1). element_code ∈ 0..9 maps to
// i8/i16/i32/i64/u8/u16/u32/u64/f32/f64. raw is N × element_size
// bytes, little-endian. Leaf type — no recursion.
struct TypedArray {
   std::uint8_t              element_code{};
   std::vector<std::uint8_t> raw;
   bool operator==(const TypedArray&) const = default;
};

// String (§4.9). UTF-8 bytes plus encoding flag.
struct String {
   std::uint8_t              encoding_flag{};   // 0 = raw_text, 1 = escape_form
   std::vector<std::uint8_t> content;
   bool operator==(const String&) const = default;
};

// Bytes (§4.10). Raw octets with JSON-emit encoding hint.
struct Bytes {
   std::uint8_t              encoding_hint{};   // 0=b64, 1=hex, 2=b58, 3=b64url
   std::vector<std::uint8_t> content;
   bool operator==(const Bytes&) const = default;
};

// NumericString (§4.8) wraps a numeric inner. Recursive type — uses
// the same forward-declared body trick as Array/Object/RowArray.
struct NumericStringBody;
struct NumericString {
   std::shared_ptr<NumericStringBody> body;
   bool operator==(const NumericString& other) const;
};

// Extension (§4.11). Leaf type — opaque body, no recursion.
struct Extension {
   std::uint8_t              subtype{};   // 0..15
   std::vector<std::uint8_t> bytes;       // opaque body
   bool operator==(const Extension&) const = default;
};

// Object (§5.2) — recursive, like Array. ObjectBody is forward-
// declared so the variant can be sized.
struct ObjectBody;
struct Object {
   std::shared_ptr<ObjectBody> body;
   bool operator==(const Object& other) const;
};

// RowArray (§5.2.1) — homogeneous-shape array of objects. Same
// recursion pattern as Array/Object.
struct RowArrayBody;
struct RowArray {
   std::shared_ptr<RowArrayBody> body;
   bool operator==(const RowArray& other) const;
};

using Value = std::variant<Null, Bool, Uint, NegInt, Float, Decimal,
                            Array, TypedArray, Object, RowArray, String, Bytes,
                            NumericString, Extension>;

struct ArrayBody {
   std::vector<Value> children;
   bool operator==(const ArrayBody&) const = default;
};

inline bool Array::operator==(const Array& other) const {
   if (!body && !other.body) return true;
   if (!body || !other.body) return false;
   return body->children == other.body->children;
}

inline Array make_array(std::vector<Value> children) {
   Array a;
   a.body = std::make_shared<ArrayBody>();
   a.body->children = std::move(children);
   return a;
}

struct ObjectBody {
   std::vector<std::pair<std::string, Value>> entries;
   bool operator==(const ObjectBody&) const = default;
};

inline bool Object::operator==(const Object& other) const {
   if (!body && !other.body) return true;
   if (!body || !other.body) return false;
   return body->entries == other.body->entries;
}

inline Object make_object(std::vector<std::pair<std::string, Value>> entries) {
   Object o;
   o.body = std::make_shared<ObjectBody>();
   o.body->entries = std::move(entries);
   return o;
}

struct RowArrayBody {
   std::vector<std::string>          keys;
   std::vector<std::vector<Value>>   rows;
   bool operator==(const RowArrayBody&) const = default;
};

inline bool RowArray::operator==(const RowArray& other) const {
   if (!body && !other.body) return true;
   if (!body || !other.body) return false;
   return body->keys == other.body->keys && body->rows == other.body->rows;
}

inline RowArray make_row_array(std::vector<std::string> keys,
                                std::vector<std::vector<Value>> rows) {
   RowArray r;
   r.body = std::make_shared<RowArrayBody>();
   r.body->keys = std::move(keys);
   r.body->rows = std::move(rows);
   return r;
}

struct NumericStringBody {
   Value inner;
   bool operator==(const NumericStringBody&) const = default;
};

inline bool NumericString::operator==(const NumericString& other) const {
   if (!body && !other.body) return true;
   if (!body || !other.body) return false;
   return body->inner == other.body->inner;
}

inline NumericString make_numeric_string(Value inner) {
   NumericString ns;
   ns.body = std::make_shared<NumericStringBody>();
   ns.body->inner = std::move(inner);
   return ns;
}

inline bool is_numeric_value(const Value& v) {
   return std::holds_alternative<Uint>(v)
       || std::holds_alternative<NegInt>(v)
       || std::holds_alternative<Float>(v)
       || std::holds_alternative<Decimal>(v);
}

// ── Base-N encoding helpers for §4.10 bytes JSON emit ──────────────

inline std::string b64_encode(const std::vector<std::uint8_t>& bytes) {
   static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   std::string out;
   out.reserve((bytes.size() + 2) / 3 * 4);
   std::size_t i = 0;
   for (; i + 3 <= bytes.size(); i += 3) {
      std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16)
                      | (static_cast<std::uint32_t>(bytes[i + 1]) << 8)
                      |  static_cast<std::uint32_t>(bytes[i + 2]);
      out += A[(n >> 18) & 0x3F];
      out += A[(n >> 12) & 0x3F];
      out += A[(n >>  6) & 0x3F];
      out += A[ n        & 0x3F];
   }
   const std::size_t rem = bytes.size() - i;
   if (rem == 1) {
      std::uint32_t n = static_cast<std::uint32_t>(bytes[i]) << 16;
      out += A[(n >> 18) & 0x3F];
      out += A[(n >> 12) & 0x3F];
      out += "==";
   } else if (rem == 2) {
      std::uint32_t n = (static_cast<std::uint32_t>(bytes[i]) << 16)
                      | (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
      out += A[(n >> 18) & 0x3F];
      out += A[(n >> 12) & 0x3F];
      out += A[(n >>  6) & 0x3F];
      out += '=';
   }
   return out;
}

inline std::string b64url_encode(const std::vector<std::uint8_t>& bytes) {
   std::string s = b64_encode(bytes);
   while (!s.empty() && s.back() == '=') s.pop_back();
   for (char& c : s) {
      if      (c == '+') c = '-';
      else if (c == '/') c = '_';
   }
   return s;
}

inline std::vector<std::uint8_t> b64_decode(std::string_view s) {
   auto val = [](char c) -> int {
      if ('A' <= c && c <= 'Z') return c - 'A';
      if ('a' <= c && c <= 'z') return 26 + c - 'a';
      if ('0' <= c && c <= '9') return 52 + c - '0';
      if (c == '+') return 62;
      if (c == '/') return 63;
      return -1;
   };
   std::vector<std::uint8_t> out;
   std::uint32_t buf = 0;
   int bits = 0;
   for (char c : s) {
      if (c == '=' || c == ' ' || c == '\n' || c == '\t' || c == '\r') continue;
      const int v = val(c);
      if (v < 0) throw std::runtime_error{"b64_decode: invalid char"};
      buf = (buf << 6) | static_cast<std::uint32_t>(v);
      bits += 6;
      if (bits >= 8) {
         bits -= 8;
         out.push_back(static_cast<std::uint8_t>((buf >> bits) & 0xFF));
      }
   }
   return out;
}

inline std::vector<std::uint8_t> b64url_decode(std::string_view s) {
   std::string std_b64;
   std_b64.reserve(s.size() + 3);
   for (char c : s) {
      if      (c == '-') std_b64.push_back('+');
      else if (c == '_') std_b64.push_back('/');
      else                std_b64.push_back(c);
   }
   while (std_b64.size() % 4 != 0) std_b64.push_back('=');
   return b64_decode(std_b64);
}

inline std::vector<std::uint8_t> base58_decode(std::string_view s) {
   static const char A[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
   auto val = [](char c) -> int {
      for (int i = 0; i < 58; ++i) if (A[i] == c) return i;
      return -1;
   };
   std::size_t zeros = 0;
   while (zeros < s.size() && s[zeros] == '1') ++zeros;
   // Output is bounded by payload * log(58)/log(256) ≈ 0.733.
   // Reserve up front so the mul-add loop never reallocates.
   const std::size_t payload = s.size() - zeros;
   std::vector<std::uint8_t> acc;
   acc.reserve((payload * 733) / 1000 + 1);
   // Internal accumulator is little-endian (append on carry-out).
   // Reversed once at the end to produce big-endian output.
   for (std::size_t i = zeros; i < s.size(); ++i) {
      int v = val(s[i]);
      if (v < 0) throw std::runtime_error{"base58_decode: invalid char"};
      std::uint32_t carry = static_cast<std::uint32_t>(v);
      for (auto it = acc.begin(); it != acc.end(); ++it) {
         carry += static_cast<std::uint32_t>(*it) * 58u;
         *it = static_cast<std::uint8_t>(carry & 0xFF);
         carry >>= 8;
      }
      while (carry > 0) {
         acc.push_back(static_cast<std::uint8_t>(carry & 0xFF));
         carry >>= 8;
      }
   }
   std::reverse(acc.begin(), acc.end());
   std::vector<std::uint8_t> out;
   out.reserve(zeros + acc.size());
   out.resize(zeros, 0);
   out.insert(out.end(), acc.begin(), acc.end());
   return out;
}

inline std::string hex_encode(const std::vector<std::uint8_t>& bytes) {
   static const char d[] = "0123456789abcdef";
   std::string out;
   out.reserve(bytes.size() * 2);
   for (std::uint8_t b : bytes) {
      out += d[b >> 4];
      out += d[b & 0xF];
   }
   return out;
}

inline std::string base58_encode(const std::vector<std::uint8_t>& bytes) {
   static const char A[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
   if (bytes.empty()) return {};
   std::size_t zeros = 0;
   while (zeros < bytes.size() && bytes[zeros] == 0) ++zeros;
   std::vector<std::uint8_t> input(bytes);
   std::vector<std::uint8_t> digits;
   while (true) {
      bool all_zero = true;
      for (auto v : input) if (v != 0) { all_zero = false; break; }
      if (all_zero) break;
      std::uint32_t carry = 0;
      for (auto& b : input) {
         std::uint32_t cur = (carry << 8) | static_cast<std::uint32_t>(b);
         b = static_cast<std::uint8_t>(cur / 58);
         carry = cur % 58;
      }
      digits.push_back(static_cast<std::uint8_t>(carry));
   }
   std::string out;
   out.reserve(zeros + digits.size());
   for (std::size_t i = 0; i < zeros; ++i) out += '1';
   for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
      out += A[*it];
   }
   return out;
}

// Per-character JSON-string escape pass (RFC 8259). Used for
// raw_text strings and object keys.
inline void json_escape_into(std::string_view text, std::string& out) {
   for (unsigned char c : text) {
      switch (c) {
         case '"':  out += "\\\""; break;
         case '\\': out += "\\\\"; break;
         case '\n': out += "\\n";  break;
         case '\r': out += "\\r";  break;
         case '\t': out += "\\t";  break;
         case '\b': out += "\\b";  break;
         case '\f': out += "\\f";  break;
         default:
            if (c < 0x20) {
               char buf[8];
               std::snprintf(buf, sizeof buf, "\\u%04x", c);
               out += buf;
            } else {
               out += static_cast<char>(c);
            }
      }
   }
}

// §4.8 numeric_string requires inner ∈ codes 2..7. Validation helper
// is defined later (after the recursive Object/RowArray bodies).

// §4.8 / §7.1 numeric-string lift detection. Returns true if `s` is a
// canonical-form JSON number string (suitable for lifting); writes
// the parsed mantissa and scale to the out-params.
//   - matches  ^-?(0|[1-9][0-9]*)(\.[0-9]*[1-9])?$
//   - rejects  "-0", "1.0", "1.50", "01", "00123", "1e5", "+1", "1.", ".5"
// On success, scale ≤ 0; mantissa fits i128.  scale == 0 means integer.
inline bool parse_canonical_json_number_string(
    std::string_view s, std::string& mantissa_str_out, std::int32_t& scale_out)
{
   if (s.empty()) return false;
   std::size_t i = 0;
   bool negative = (s[i] == '-');
   if (negative) ++i;
   if (i >= s.size()) return false;

   // Integer part: 0 OR [1-9][0-9]*
   const std::size_t int_start = i;
   if (s[i] == '0') {
      ++i;
   } else if (s[i] >= '1' && s[i] <= '9') {
      ++i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
   } else return false;
   const std::size_t int_end = i;

   // Optional fractional: . then digits, must end in non-zero
   std::size_t frac_start = i, frac_end = i;
   if (i < s.size() && s[i] == '.') {
      ++i;
      frac_start = i;
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
      frac_end = i;
      if (frac_end == frac_start) return false;          // empty fractional
      if (s[frac_end - 1] == '0')   return false;        // trailing zero
   }
   if (i != s.size()) return false;                      // trailing chars

   // Reject "-0"
   if (negative && (int_end - int_start) == 1 && s[int_start] == '0' && frac_start == frac_end)
      return false;

   // Build mantissa string (sign + int + frac digits, no decimal point).
   mantissa_str_out.clear();
   if (negative) mantissa_str_out += '-';
   mantissa_str_out.append(s.substr(int_start, int_end - int_start));
   if (frac_end > frac_start) {
      mantissa_str_out.append(s.substr(frac_start, frac_end - frac_start));
      scale_out = -static_cast<std::int32_t>(frac_end - frac_start);
   } else {
      scale_out = 0;
   }
   return true;
}

// §5.3 — 8-bit prefilter hash. Strip key from the last `.` onward
// (e.g. "amount.decimal" → "amount") then XXH3-64 → low byte.
inline std::uint8_t key_hash8(std::string_view key) noexcept {
   const auto dot = key.rfind('.');
   const auto stripped = (dot == std::string_view::npos) ? key : key.substr(0, dot);
   const auto h = XXH3_64bits(stripped.data(), stripped.size());
   return static_cast<std::uint8_t>(h & 0xFF);
}

// §5.1.1 — element_size in bytes for each typed-array element_code.
// Returns 0 for codes outside 0..9 so callers can produce the
// appropriate (encode vs decode) error themselves.
inline std::size_t typed_array_element_size(std::uint8_t code) noexcept {
   switch (code) {
      case 0: case 4:           return 1;  // i8, u8
      case 1: case 5:           return 2;  // i16, u16
      case 2: case 6: case 8:   return 4;  // i32, u32, f32
      case 3: case 7: case 9:   return 8;  // i64, u64, f64
      default:                  return 0;
   }
}

// ── Errors ─────────────────────────────────────────────────────────

class EncodeError : public std::runtime_error {
public:
   using std::runtime_error::runtime_error;
};

class DecodeError : public std::runtime_error {
public:
   using std::runtime_error::runtime_error;
};

// ── Helpers ────────────────────────────────────────────────────────

// Smallest LE byte representation of a U128, leading zeros stripped.
// Returns empty for value 0.
static std::vector<std::uint8_t> u128_le_minimal(U128 n) {
   std::vector<std::uint8_t> out;
   out.reserve(16);
   for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(n.lo >> (8 * i)));
   for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(n.hi >> (8 * i)));
   while (!out.empty() && out.back() == 0) out.pop_back();
   return out;
}

static std::vector<std::uint8_t> u128_le_minimal_at_least_1(U128 n) {
   auto v = u128_le_minimal(n);
   if (v.empty()) v.push_back(0);
   return v;
}

// Minimal byte count for `n` little-endian (≥ 1). Returns the same
// value as `u128_le_minimal_at_least_1(n).size()` without allocating.
static inline std::size_t u128_minimal_byte_count(U128 n) noexcept {
   if (n.is_zero()) return 1;
   const std::uint64_t hi = n.hi;
   if (hi != 0) {
      // bits used in hi: 64 - clz(hi). total = 64 + that, in bytes.
      const unsigned bits = 64u - static_cast<unsigned>(__builtin_clzll(hi)) + 64u;
      return (bits + 7u) / 8u;
   }
   const unsigned bits = 64u - static_cast<unsigned>(__builtin_clzll(n.lo));
   return (bits + 7u) / 8u;
}

// §15.2.1 canonical NaN bit-pattern rewrite. If `bits` (interpreted
// as a width-`width_log2` IEEE float) is a NaN, returns the canonical
// quiet-NaN-with-zero-payload pattern at that width. Non-NaN values
// (including ±Inf, ±0, finite) pass through verbatim.
static U128 canonicalize_nan_bits(std::uint8_t width_log2, U128 bits) noexcept {
   U128 exp_mask{}, mant_mask{}, canon{};
   switch (width_log2) {
      case 1:
         exp_mask  = U128{0x7C00ULL, 0};
         mant_mask = U128{0x03FFULL, 0};
         canon     = U128{0x7E00ULL, 0};
         break;
      case 2:
         exp_mask  = U128{0x7F800000ULL, 0};
         mant_mask = U128{0x007FFFFFULL, 0};
         canon     = U128{0x7FC00000ULL, 0};
         break;
      case 3:
         exp_mask  = U128{0x7FF0000000000000ULL, 0};
         mant_mask = U128{0x000FFFFFFFFFFFFFULL, 0};
         canon     = U128{0x7FF8000000000000ULL, 0};
         break;
      case 4:
         exp_mask  = U128{0, 0x7FFF000000000000ULL};
         mant_mask = U128{0xFFFFFFFFFFFFFFFFULL, 0x0000FFFFFFFFFFFFULL};
         canon     = U128{0, 0x7FFF800000000000ULL};
         break;
      default: return bits;
   }
   const U128 exp_bits { bits.lo & exp_mask.lo,  bits.hi & exp_mask.hi  };
   const U128 mant_bits{ bits.lo & mant_mask.lo, bits.hi & mant_mask.hi };
   const bool exp_all_ones = (exp_bits == exp_mask);
   const bool mant_nonzero = !mant_bits.is_zero();
   if (exp_all_ones && mant_nonzero) return canon;
   return bits;
}

// C-002 helpers: smallest bit-exact ieee width for a finite float.
//
// Returns (width_log2, bits_at_that_width) for `bits` interpreted at
// `from_w`. Values that are NaN or ±Inf or ±0 reduce to width 1
// (binary16). Otherwise narrowing trials run f64↔f32 and f64↔f16.
// Inputs at width 4 (binary128) only narrow to f64 if the value
// fits exactly; otherwise stay at f128.
static bool float_zero_at(std::uint8_t w, U128 bits) noexcept {
   switch (w) {
      case 1: return (bits.lo & 0x7FFFu) == 0;
      case 2: return (bits.lo & 0x7FFF'FFFFu) == 0;
      case 3: return (bits.lo & 0x7FFF'FFFF'FFFF'FFFFull) == 0;
      case 4: return (bits.lo == 0) && ((bits.hi & 0x7FFF'FFFF'FFFF'FFFFull) == 0);
      default: return false;
   }
}
static bool float_inf_at(std::uint8_t w, U128 bits) noexcept {
   switch (w) {
      case 1: return (bits.lo & 0x7FFFu) == 0x7C00u;
      case 2: return (bits.lo & 0x7FFF'FFFFu) == 0x7F80'0000u;
      case 3: return (bits.lo & 0x7FFF'FFFF'FFFF'FFFFull) == 0x7FF0'0000'0000'0000ull;
      case 4: return (bits.lo == 0)
                   && ((bits.hi & 0x7FFF'FFFF'FFFF'FFFFull) == 0x7FFF'0000'0000'0000ull);
      default: return false;
   }
}
static bool float_nan_at(std::uint8_t w, U128 bits) noexcept {
   switch (w) {
      case 1: {
         std::uint64_t exp  = bits.lo & 0x7C00u;
         std::uint64_t mant = bits.lo & 0x03FFu;
         return exp == 0x7C00u && mant != 0;
      }
      case 2: {
         std::uint64_t exp  = bits.lo & 0x7F80'0000ull;
         std::uint64_t mant = bits.lo & 0x007F'FFFFull;
         return exp == 0x7F80'0000ull && mant != 0;
      }
      case 3: {
         std::uint64_t exp  = bits.lo & 0x7FF0'0000'0000'0000ull;
         std::uint64_t mant = bits.lo & 0x000F'FFFF'FFFF'FFFFull;
         return exp == 0x7FF0'0000'0000'0000ull && mant != 0;
      }
      case 4: {
         std::uint64_t exp  = bits.hi & 0x7FFF'0000'0000'0000ull;
         bool mant_nz = (bits.lo != 0)
                     || ((bits.hi & 0x0000'FFFF'FFFF'FFFFull) != 0);
         return exp == 0x7FFF'0000'0000'0000ull && mant_nz;
      }
      default: return false;
   }
}

// Returns std::nullopt unless the f64 `f` is bit-exactly representable
// in binary16; on success, returns the f16 bit pattern.
static std::optional<std::uint16_t> f64_to_f16_exact(double f) noexcept {
   std::uint64_t bits;
   std::memcpy(&bits, &f, sizeof bits);
   const std::uint16_t sign = static_cast<std::uint16_t>((bits >> 63) & 1);
   const int           exp_f64  = static_cast<int>((bits >> 52) & 0x7FF);
   const std::uint64_t mant_f64 = bits & 0x000F'FFFF'FFFF'FFFFull;

   if (exp_f64 == 0) {
      if (mant_f64 == 0) return static_cast<std::uint16_t>(sign << 15);
      return std::nullopt;     // f64 subnormal — punt
   }
   if (exp_f64 == 0x7FF) return std::nullopt;  // Inf/NaN handled earlier

   const int exp_unbiased = exp_f64 - 1023;
   if (exp_unbiased < -14 || exp_unbiased > 15) return std::nullopt;
   if ((mant_f64 & ((1ull << 42) - 1)) != 0)    return std::nullopt;
   const std::uint16_t mant_f16 = static_cast<std::uint16_t>(mant_f64 >> 42);
   const std::uint16_t exp_f16  = static_cast<std::uint16_t>(exp_unbiased + 15);
   return static_cast<std::uint16_t>((sign << 15) | (exp_f16 << 10) | mant_f16);
}

static double f16_bits_to_f64_local(std::uint16_t b) noexcept {
   const std::uint64_t sign = (b >> 15) & 1;
   const std::uint64_t exp  = (b >> 10) & 0x1F;
   const std::uint64_t mant = b & 0x03FF;
   const std::uint64_t sign_bit = sign << 63;
   if (exp == 0 && mant == 0) {
      double r;
      std::memcpy(&r, &sign_bit, sizeof r);
      return r;
   }
   if (exp == 0x1F) {
      const std::uint64_t bits = sign_bit | 0x7FF0'0000'0000'0000ull
                              | (mant ? 0x0008'0000'0000'0000ull : 0);
      double r;
      std::memcpy(&r, &bits, sizeof r);
      return r;
   }
   const int exp_unbiased = static_cast<int>(exp) - 15;
   const std::uint64_t exp_f64 = static_cast<std::uint64_t>(exp_unbiased + 1023) << 52;
   const std::uint64_t bits_f64 = sign_bit | exp_f64 | (mant << 42);
   double r;
   std::memcpy(&r, &bits_f64, sizeof r);
   return r;
}

// f128 → f64 if bit-exact; std::nullopt otherwise.
static std::optional<double> f128_bits_to_f64_exact(U128 bits) noexcept {
   const std::uint64_t sign = (bits.hi >> 63) & 1;
   const int exp_f128 = static_cast<int>((bits.hi >> 48) & 0x7FFF);
   // mantissa = bits.lo (low 64) || (bits.hi & ((1<<48)-1)) (high 48 bits)
   const std::uint64_t mant_hi = bits.hi & ((1ull << 48) - 1);
   const std::uint64_t mant_lo = bits.lo;

   if (exp_f128 == 0) {
      if (mant_hi == 0 && mant_lo == 0) {
         double r;
         std::uint64_t z = sign << 63;
         std::memcpy(&r, &z, sizeof r);
         return r;
      }
      return std::nullopt;
   }
   if (exp_f128 == 0x7FFF) return std::nullopt;

   const int exp_unbiased = exp_f128 - 16383;
   if (exp_unbiased < -1022 || exp_unbiased > 1023) return std::nullopt;
   // f64 has 52 mantissa bits. f128 has 112 (48 hi + 64 lo). Need
   // top 52 = mant_hi[47..0] then top 4 of mant_lo[63..60]. That
   // means low 60 bits of mant_lo and any nonzero in deeper places
   // must be zero for bit-exactness.
   if ((mant_lo & ((1ull << 60) - 1)) != 0) return std::nullopt;
   const std::uint64_t mant_f64 = (mant_hi << 4) | (mant_lo >> 60);
   const std::uint64_t exp_f64 = static_cast<std::uint64_t>(exp_unbiased + 1023) << 52;
   const std::uint64_t out = (sign << 63) | exp_f64 | mant_f64;
   double r;
   std::memcpy(&r, &out, sizeof r);
   return r;
}

static std::pair<std::uint8_t, U128>
canonical_float_width(std::uint8_t from_w, U128 bits) noexcept {
   if (float_zero_at(from_w, bits)) {
      // ±0 → width 1, sign bit only.
      const std::size_t top_bit_index = 8u * (1u << from_w) - 1u;
      const std::uint64_t sign = (top_bit_index < 64)
         ? ((bits.lo >> top_bit_index) & 1u)
         : ((bits.hi >> (top_bit_index - 64)) & 1u);
      return {1, U128{sign << 15, 0}};
   }
   if (float_inf_at(from_w, bits)) {
      const std::size_t top_bit_index = 8u * (1u << from_w) - 1u;
      const std::uint64_t sign = (top_bit_index < 64)
         ? ((bits.lo >> top_bit_index) & 1u)
         : ((bits.hi >> (top_bit_index - 64)) & 1u);
      return {1, U128{(sign << 15) | 0x7C00u, 0}};
   }
   if (float_nan_at(from_w, bits)) {
      return {1, U128{0x7E00u, 0}};
   }

   // Lift to f64 for narrowing trials.
   std::optional<double> as_f64;
   switch (from_w) {
      case 1: as_f64 = f16_bits_to_f64_local(static_cast<std::uint16_t>(bits.lo & 0xFFFFu)); break;
      case 2: {
         std::uint32_t b = static_cast<std::uint32_t>(bits.lo & 0xFFFF'FFFFu);
         float f;
         std::memcpy(&f, &b, sizeof f);
         as_f64 = static_cast<double>(f);
         break;
      }
      case 3: {
         double f;
         std::memcpy(&f, &bits.lo, sizeof f);
         as_f64 = f;
         break;
      }
      case 4: as_f64 = f128_bits_to_f64_exact(bits); break;
      default: return {from_w, bits};
   }
   if (!as_f64.has_value()) return {from_w, bits};
   const double f = *as_f64;

   // Try f16.
   if (auto h = f64_to_f16_exact(f)) {
      return {1, U128{static_cast<std::uint64_t>(*h), 0}};
   }
   // Try f32.
   const float f32_val = static_cast<float>(f);
   if (static_cast<double>(f32_val) == f) {
      std::uint32_t b32;
      std::memcpy(&b32, &f32_val, sizeof b32);
      return {2, U128{static_cast<std::uint64_t>(b32), 0}};
   }
   // Stay at f64.
   std::uint64_t b64;
   std::memcpy(&b64, &f, sizeof b64);
   return {3, U128{b64, 0}};
}

// §4.7.2 / D-007 picker. Choose Decimal or ieee_float (smallest
// width) for a JSON-source fractional, given its (mantissa, scale)
// canonical form and the parsed double `f`. Tie → ieee.
static Value decimal_or_ieee_pick(I128 mantissa, std::int32_t scale, double f);

static U128 read_u128_le(std::span<const std::uint8_t> bytes) {
   U128 v{};
   for (std::size_t i = 0; i < bytes.size() && i < 16; ++i) {
      if (i < 8) v.lo |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
      else       v.hi |= static_cast<std::uint64_t>(bytes[i]) << (8 * (i - 8));
   }
   return v;
}

// Zigzag for i32 (varscale).
static std::uint32_t zigzag_encode_i32(std::int32_t v) noexcept {
   return static_cast<std::uint32_t>((v << 1) ^ (v >> 31));
}
static std::int32_t zigzag_decode_u32(std::uint32_t z) noexcept {
   return static_cast<std::int32_t>((z >> 1) ^ -static_cast<std::int32_t>(z & 1));
}

// Zigzag for I128 (decimal mantissa).
//   zz = (v << 1) ^ (v >> 127) — interpret as unsigned bits.
static U128 zigzag_encode_i128(I128 v) noexcept {
   // Represent v as a 128-bit unsigned bag of bits.
   U128 bits{v.lo, static_cast<std::uint64_t>(v.hi)};
   // (v << 1):
   U128 shifted{bits.lo << 1, (bits.hi << 1) | (bits.lo >> 63)};
   // (v >> 127), arithmetic — fills with sign bit:
   std::uint64_t sign_mask = v.is_negative()
                                ? std::numeric_limits<std::uint64_t>::max()
                                : 0;
   U128 sign_extend{sign_mask, sign_mask};
   // XOR:
   return U128{shifted.lo ^ sign_extend.lo, shifted.hi ^ sign_extend.hi};
}

static I128 zigzag_decode_u128(U128 z) noexcept {
   //   v = (z >> 1) ^ -(z & 1)
   U128 shifted{(z.lo >> 1) | (z.hi << 63), z.hi >> 1};
   std::uint64_t lsb_mask = (z.lo & 1) ? std::numeric_limits<std::uint64_t>::max() : 0;
   U128 mask{lsb_mask, lsb_mask};
   return I128{shifted.lo ^ mask.lo,
               static_cast<std::int64_t>(shifted.hi ^ mask.hi)};
}

// ── Encode (§12 reference algorithm) ───────────────────────────────

// 2-bit-prefix variable-length unsigned integer (§4.7.1 byte-tier
// scheme, no zigzag). The primitive shared by varscale (§4.7.1
// signed) and §5.4 long-key excess (§5.4 unsigned). Capacity:
//   1 byte: 0..63          (6-bit payload)
//   2 byte: 0..16383       (14-bit)
//   3 byte: 0..4_194_303   (22-bit)
//   4 byte: 0..1_073_741_823 (30-bit)
inline void varuint_encode(std::uint32_t value, std::vector<std::uint8_t>& out) {
   std::uint32_t total_bytes;
   if      (value < (1u <<  6)) total_bytes = 1;
   else if (value < (1u << 14)) total_bytes = 2;
   else if (value < (1u << 22)) total_bytes = 3;
   else if (value < (1u << 30)) total_bytes = 4;
   else throw EncodeError{"varuint overflow"};
   out.push_back(static_cast<std::uint8_t>(((total_bytes - 1) << 6) | (value & 0x3F)));
   std::uint32_t shifted = value >> 6;
   for (std::uint32_t i = 1; i < total_bytes; ++i) {
      out.push_back(static_cast<std::uint8_t>(shifted & 0xFF));
      shifted >>= 8;
   }
}

struct VaruintResult { std::uint32_t value; std::size_t used; };

inline VaruintResult varuint_decode(std::span<const std::uint8_t> buf) {
   if (buf.empty()) throw DecodeError{"varuint first byte"};
   const std::size_t total = static_cast<std::size_t>((buf[0] >> 6) + 1);
   if (buf.size() < total) throw DecodeError{"varuint body"};
   std::uint32_t v = static_cast<std::uint32_t>(buf[0] & 0x3F);
   for (std::size_t i = 1; i < total; ++i) {
      v |= static_cast<std::uint32_t>(buf[i]) << (6 + 8 * (i - 1));
   }
   return {v, total};
}

// §4.7.1 varscale — signed wrapper over varuint via zigzag.
static void varscale_encode(std::int32_t scale, std::vector<std::uint8_t>& out) {
   varuint_encode(zigzag_encode_i32(scale), out);
}

static std::vector<std::uint8_t> encode(const Value& v);

static void encode_into(const Value& v, std::vector<std::uint8_t>& out) {
   std::visit([&](auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, Null>) {
         out.push_back(0x00);
      } else if constexpr (std::is_same_v<T, Bool>) {
         out.push_back(arg.value ? 0x11 : 0x10);
      } else if constexpr (std::is_same_v<T, Uint>) {
         if (arg.value.fits_u64() && arg.value.lo <= 15) {
            // uint_inline §4.3
            out.push_back(static_cast<std::uint8_t>(0x20 | arg.value.lo));
         } else {
            // uint §4.5
            const auto bytes = u128_le_minimal(arg.value);
            const std::size_t bc = bytes.size();
            if (bc == 0 || bc > 16) throw EncodeError{"uint magnitude"};
            out.push_back(static_cast<std::uint8_t>(0x40 | (bc - 1)));
            out.insert(out.end(), bytes.begin(), bytes.end());
         }
      } else if constexpr (std::is_same_v<T, NegInt>) {
         if (arg.magnitude.is_zero()) {
            throw EncodeError{"negint magnitude 0 reserved"};
         }
         if (arg.magnitude.fits_u64() && arg.magnitude.lo <= 15) {
            // nint_inline §4.4
            out.push_back(static_cast<std::uint8_t>(0x30 | arg.magnitude.lo));
         } else {
            // negint §4.5
            const auto bytes = u128_le_minimal(arg.magnitude);
            const std::size_t bc = bytes.size();
            if (bc == 0 || bc > 16) throw EncodeError{"negint magnitude"};
            out.push_back(static_cast<std::uint8_t>(0x50 | (bc - 1)));
            out.insert(out.end(), bytes.begin(), bytes.end());
         }
      } else if constexpr (std::is_same_v<T, Float>) {
         // §4.6 — width selector in low nibble bits 2..0; bit 3 reserved.
         if (arg.width_log2 < 1 || arg.width_log2 > 4) {
            throw EncodeError{"ieee_float width_log2"};
         }
         const std::size_t byte_count = std::size_t{1} << arg.width_log2;
         out.push_back(static_cast<std::uint8_t>(0x60 | arg.width_log2));
         // §15.2.1 — rewrite any NaN bits to canonical quiet-NaN-zero
         // pattern at this width before emission. Non-NaN passes through.
         const U128 emit_bits = canonicalize_nan_bits(arg.width_log2, arg.bits);
         std::uint8_t buf[16];
         for (std::size_t i = 0; i < 16; ++i) {
            buf[i] = (i < 8)
                        ? static_cast<std::uint8_t>(emit_bits.lo >> (8 * i))
                        : static_cast<std::uint8_t>(emit_bits.hi >> (8 * (i - 8)));
         }
         out.insert(out.end(), buf, buf + byte_count);
      } else if constexpr (std::is_same_v<T, Decimal>) {
         const auto zz = zigzag_encode_i128(arg.mantissa);
         const auto m_bytes = u128_le_minimal_at_least_1(zz);
         const std::size_t bc = m_bytes.size();
         if (bc > 16) throw EncodeError{"decimal mantissa"};
         out.push_back(static_cast<std::uint8_t>(0x70 | (bc - 1)));
         out.insert(out.end(), m_bytes.begin(), m_bytes.end());
         varscale_encode(arg.scale, out);
      } else if constexpr (std::is_same_v<T, Object>) {
         // §5.2 single object.
         const auto& entries = arg.body ? arg.body->entries
                                        : std::vector<std::pair<std::string, Value>>{};
         const std::size_t n = entries.size();
         if (n > 0xFFFF) throw EncodeError{"object count > 65535 (LIM-001)"};

         std::vector<std::uint8_t> value_data;
         std::vector<std::size_t>  offsets;
         std::vector<std::uint8_t> key_size_bytes;
         offsets.reserve(n);
         key_size_bytes.reserve(n);
         for (const auto& [key, child] : entries) {
            offsets.push_back(value_data.size());
            const std::size_t klen = key.size();
            if (klen < 0xFF) {
               key_size_bytes.push_back(static_cast<std::uint8_t>(klen));
               value_data.insert(value_data.end(), key.begin(), key.end());
            } else {
               key_size_bytes.push_back(0xFF);
               const std::size_t excess = klen - 0xFF;
               if (excess > std::numeric_limits<std::uint32_t>::max())
                  throw EncodeError{"key length overflow (LIM-003)"};
               varuint_encode(static_cast<std::uint32_t>(excess), value_data);
               value_data.insert(value_data.end(), key.begin(), key.end());
            }
            encode_into(child, value_data);
         }
         const std::size_t value_data_size = value_data.size();

         std::uint8_t slot_w_code;
         std::size_t  slot_w;
         if      (value_data_size <= 0xFF)         { slot_w_code = 0; slot_w = 1; }
         else if (value_data_size <= 0xFFFF)       { slot_w_code = 1; slot_w = 2; }
         else if (value_data_size <= 0xFF'FFFF)    { slot_w_code = 2; slot_w = 3; }
         else if (value_data_size <= 0xFFFF'FFFFu) { slot_w_code = 3; slot_w = 4; }
         else throw EncodeError{"value_data > u32 (LIM-002)"};

         out.push_back(0xC0);
         out.push_back(slot_w_code);
         out.insert(out.end(), value_data.begin(), value_data.end());
         // hash[N]
         for (const auto& [key, _] : entries) {
            out.push_back(key_hash8(key));
         }
         // slot[N]: offset_LE (slot_w bytes) + key_size_byte (1 byte)
         for (std::size_t i = 0; i < n; ++i) {
            const std::size_t off = offsets[i];
            for (std::size_t b = 0; b < slot_w; ++b) {
               out.push_back(static_cast<std::uint8_t>(off >> (8 * b)));
            }
            out.push_back(key_size_bytes[i]);
         }
         out.push_back(static_cast<std::uint8_t>(n & 0xFF));
         out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
      } else if constexpr (std::is_same_v<T, String>) {
         if (arg.encoding_flag > 1)
            throw EncodeError{"string encoding_flag must be 0 or 1"};
         out.push_back(static_cast<std::uint8_t>(0x90 | arg.encoding_flag));
         out.insert(out.end(), arg.content.begin(), arg.content.end());
      } else if constexpr (std::is_same_v<T, Bytes>) {
         if (arg.encoding_hint > 3)
            throw EncodeError{"bytes encoding_hint must be 0..3"};
         out.push_back(static_cast<std::uint8_t>(0xA0 | arg.encoding_hint));
         out.insert(out.end(), arg.content.begin(), arg.content.end());
      } else if constexpr (std::is_same_v<T, NumericString>) {
         if (!arg.body) throw EncodeError{"numeric_string body null"};
         if (!is_numeric_value(arg.body->inner))
            throw EncodeError{"numeric_string inner must be numeric (codes 2..7)"};
         out.push_back(0x80);
         encode_into(arg.body->inner, out);
      } else if constexpr (std::is_same_v<T, Extension>) {
         if (arg.subtype > 15)
            throw EncodeError{"extension subtype must be 0..15"};
         out.push_back(static_cast<std::uint8_t>(0xD0 | arg.subtype));
         out.insert(out.end(), arg.bytes.begin(), arg.bytes.end());
      } else if constexpr (std::is_same_v<T, RowArray>) {
         // §5.2.1 row_array.
         const auto& keys = arg.body ? arg.body->keys : std::vector<std::string>{};
         const auto& rows = arg.body ? arg.body->rows : std::vector<std::vector<Value>>{};
         const std::size_t k = keys.size();
         const std::size_t n = rows.size();
         if (k > std::numeric_limits<std::uint32_t>::max())
            throw EncodeError{"row_array K too large"};
         if (n > 0xFFFF) throw EncodeError{"row_array count > 65535 (LIM-001)"};
         for (const auto& row : rows) {
            if (row.size() != k) throw EncodeError{"row_array row arity mismatch"};
         }
         // Shared keys area + slots
         std::vector<std::uint8_t> keys_area;
         std::vector<std::uint32_t> shared_slots;
         shared_slots.reserve(k);
         for (const auto& key : keys) {
            const std::uint32_t off = static_cast<std::uint32_t>(keys_area.size());
            if (off > 0x00FF'FFFFu) throw EncodeError{"row_array key_offset > u24"};
            if (key.size() > 0xFF) throw EncodeError{"row_array shared key > 255 bytes"};
            shared_slots.push_back(
                (static_cast<std::uint32_t>(key.size()) << 24) | (off & 0x00FF'FFFFu));
            keys_area.insert(keys_area.end(), key.begin(), key.end());
         }
         // Per-record value_data + offsets
         std::vector<std::vector<std::uint8_t>> rec_value_data;
         std::vector<std::vector<std::size_t>>  rec_offsets;
         rec_value_data.reserve(n);
         rec_offsets.reserve(n);
         for (const auto& row : rows) {
            std::vector<std::uint8_t> vd;
            std::vector<std::size_t>  offs;
            offs.reserve(k);
            for (const auto& v : row) {
               offs.push_back(vd.size());
               encode_into(v, vd);
            }
            rec_value_data.push_back(std::move(vd));
            rec_offsets.push_back(std::move(offs));
         }
         std::size_t max_vd = 0;
         for (const auto& vd : rec_value_data)
            if (vd.size() > max_vd) max_vd = vd.size();

         std::uint8_t slot_w_code, recoff_w_code;
         std::size_t  slot_w, recoff_w;
         auto pick = [](std::size_t s, std::uint8_t& code, std::size_t& w) {
            if      (s <= 0xFF)         { code = 0; w = 1; }
            else if (s <= 0xFFFF)       { code = 1; w = 2; }
            else if (s <= 0xFF'FFFF)    { code = 2; w = 3; }
            else if (s <= 0xFFFF'FFFFu) { code = 3; w = 4; }
            else throw EncodeError{"row_array value_data > u32"};
         };
         pick(max_vd, slot_w_code, slot_w);

         // Compose records body
         std::vector<std::uint8_t> records_body;
         std::vector<std::size_t>  record_offsets;
         record_offsets.reserve(n);
         for (std::size_t i = 0; i < n; ++i) {
            record_offsets.push_back(records_body.size());
            const auto& vd = rec_value_data[i];
            const auto& offs = rec_offsets[i];
            records_body.insert(records_body.end(), vd.begin(), vd.end());
            for (std::size_t off : offs) {
               for (std::size_t b = 0; b < slot_w; ++b)
                  records_body.push_back(static_cast<std::uint8_t>(off >> (8 * b)));
            }
         }
         pick(records_body.size(), recoff_w_code, recoff_w);

         out.push_back(0xC1);
         out.push_back(static_cast<std::uint8_t>(slot_w_code | (recoff_w_code << 2)));
         varuint_encode(static_cast<std::uint32_t>(k), out);
         for (std::uint32_t s : shared_slots) {
            for (int b = 0; b < 4; ++b)
               out.push_back(static_cast<std::uint8_t>(s >> (8 * b)));
         }
         for (const auto& key : keys) out.push_back(key_hash8(key));
         out.insert(out.end(), keys_area.begin(), keys_area.end());
         out.insert(out.end(), records_body.begin(), records_body.end());
         for (std::size_t off : record_offsets) {
            for (std::size_t b = 0; b < recoff_w; ++b)
               out.push_back(static_cast<std::uint8_t>(off >> (8 * b)));
         }
         out.push_back(static_cast<std::uint8_t>(n & 0xFF));
         out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
      } else if constexpr (std::is_same_v<T, TypedArray>) {
         // §5.1.1 typed homogeneous array.
         const std::size_t esize = typed_array_element_size(arg.element_code);
         if (esize == 0) throw EncodeError{"typed_array element_code"};
         if (arg.raw.size() % esize != 0)
            throw EncodeError{"typed_array raw size not multiple of element size"};
         const std::size_t n = arg.raw.size() / esize;
         if (n > 0xFFFF) throw EncodeError{"typed_array count > 65 535"};
         out.push_back(static_cast<std::uint8_t>(0xB0 | (arg.element_code + 1)));
         out.insert(out.end(), arg.raw.begin(), arg.raw.end());
         out.push_back(static_cast<std::uint8_t>(n & 0xFF));
         out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
      } else if constexpr (std::is_same_v<T, Array>) {
         // §5.1 generic array.
         const auto& children = arg.body ? arg.body->children
                                         : std::vector<Value>{};
         const std::size_t n = children.size();
         if (n > 0xFFFF) throw EncodeError{"array count > 65535 (LIM-001)"};

         std::vector<std::uint8_t> value_data;
         std::vector<std::size_t>  offsets;
         offsets.reserve(n);
         for (const auto& child : children) {
            offsets.push_back(value_data.size());
            encode_into(child, value_data);
         }
         const std::size_t value_data_size = value_data.size();

         std::uint8_t slot_w_code;
         std::size_t  slot_w;
         if      (value_data_size <= 0xFF)         { slot_w_code = 0; slot_w = 1; }
         else if (value_data_size <= 0xFFFF)       { slot_w_code = 1; slot_w = 2; }
         else if (value_data_size <= 0xFF'FFFF)    { slot_w_code = 2; slot_w = 3; }
         else if (value_data_size <= 0xFFFF'FFFFu) { slot_w_code = 3; slot_w = 4; }
         else throw EncodeError{"value_data > u32 (LIM-002)"};

         out.push_back(0xB0);
         out.push_back(slot_w_code);
         out.insert(out.end(), value_data.begin(), value_data.end());
         for (std::size_t off : offsets) {
            for (std::size_t i = 0; i < slot_w; ++i) {
               out.push_back(static_cast<std::uint8_t>(off >> (8 * i)));
            }
         }
         out.push_back(static_cast<std::uint8_t>(n & 0xFF));
         out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
      }
   }, v);
}

// Conservative upper bound for the encoded byte count. Used only
// to reserve output capacity in `encode` — never under-reports for
// typical inputs.
static std::size_t estimate_encoded_size(const Value& v) {
   return std::visit([](const auto& arg) -> std::size_t {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, Null> || std::is_same_v<T, Bool>) {
         return 1;
      } else if constexpr (std::is_same_v<T, Uint>) {
         if (arg.value.fits_u64() && arg.value.lo <= 15) return 1;
         return 1 + u128_minimal_byte_count(arg.value);
      } else if constexpr (std::is_same_v<T, NegInt>) {
         if (arg.magnitude.fits_u64() && arg.magnitude.lo <= 15) return 1;
         return 1 + u128_minimal_byte_count(arg.magnitude);
      } else if constexpr (std::is_same_v<T, Float>) {
         return 1u + (std::size_t{1} << arg.width_log2);
      } else if constexpr (std::is_same_v<T, Decimal>) {
         return 1 + u128_minimal_byte_count(zigzag_encode_i128(arg.mantissa)) + 4;
      } else if constexpr (std::is_same_v<T, String>) {
         return 1 + arg.content.size();
      } else if constexpr (std::is_same_v<T, Bytes>) {
         return 1 + arg.content.size();
      } else if constexpr (std::is_same_v<T, NumericString>) {
         return 1 + (arg.body ? estimate_encoded_size(arg.body->inner) : 1);
      } else if constexpr (std::is_same_v<T, Extension>) {
         return 1 + arg.bytes.size();
      } else if constexpr (std::is_same_v<T, Array>) {
         std::size_t body = 0;
         if (arg.body) for (const auto& c : arg.body->children) body += estimate_encoded_size(c);
         const std::size_t n = arg.body ? arg.body->children.size() : 0;
         return 4 + body + 4 * n;
      } else if constexpr (std::is_same_v<T, TypedArray>) {
         return 1 + 5 + arg.raw.size();
      } else if constexpr (std::is_same_v<T, Object>) {
         std::size_t body = 0;
         if (arg.body) for (const auto& [k, v] : arg.body->entries)
            body += 1 + 4 + k.size() + estimate_encoded_size(v) + 4 + 1;
         return 4 + body;
      } else if constexpr (std::is_same_v<T, RowArray>) {
         if (!arg.body) return 16;
         std::size_t key_bytes = 0;
         for (const auto& k : arg.body->keys) key_bytes += k.size() + 5;
         std::size_t body = 0;
         for (const auto& row : arg.body->rows)
            for (const auto& c : row) body += estimate_encoded_size(c);
         const std::size_t n_records = arg.body->rows.size();
         const std::size_t cells_per = arg.body->keys.size();
         return 16 + key_bytes + body + 4 * cells_per * n_records;
      }
      return 16;
   }, v);
}

static std::vector<std::uint8_t> encode(const Value& v) {
   std::vector<std::uint8_t> out;
   out.reserve(estimate_encoded_size(v));
   encode_into(v, out);
   return out;
}

// ── Decode (§10 reference algorithm) ───────────────────────────────

struct VarscaleResult {
   std::int32_t value;
   std::size_t  used_bytes;
};

static VarscaleResult varscale_decode(std::span<const std::uint8_t> buf) {
   const auto vu = varuint_decode(buf);
   return {zigzag_decode_u32(vu.value), vu.used};
}

// LIM-006: cap recursive container nesting at MAX_DECODE_DEPTH so
// adversarial wires can't blow the parser's stack via runaway
// recursion. Threaded as an explicit parameter (matching Rust's
// `decode_at_depth`) to avoid thread-local fetch overhead per call.
static constexpr unsigned MAX_DECODE_DEPTH = 256;

static Value decode_at_depth(std::span<const std::uint8_t> buf, unsigned depth);
static Value decode_generic_array(std::span<const std::uint8_t> buf, unsigned depth);
static Value decode_typed_array(std::span<const std::uint8_t> buf,
                                std::uint8_t element_code);
static Value decode_object(std::span<const std::uint8_t> buf, unsigned depth);
static Value decode_row_array(std::span<const std::uint8_t> buf, unsigned depth);

static Value decode(std::span<const std::uint8_t> buf) {
   return decode_at_depth(buf, 0);
}

static Value decode_at_depth(std::span<const std::uint8_t> buf, unsigned depth) {
   if (depth >= MAX_DECODE_DEPTH)
      throw DecodeError{"nesting depth exceeded (LIM-006)"};
   if (buf.empty()) throw DecodeError{"empty buffer"};
   const std::uint8_t tag  = buf[0];
   const std::uint8_t high = tag >> 4;
   const std::uint8_t low  = tag & 0x0F;

   switch (high) {
      case 0:
         if (low != 0) throw DecodeError{"reserved low_nibble for null"};
         return Null{};
      case 1:
         if      (low == 0) return Bool{false};
         else if (low == 1) return Bool{true};
         throw DecodeError{"reserved low_nibble for bool"};
      case 2:
         return Uint{U128{low, 0}};
      case 3:
         if (low == 0) throw DecodeError{"nint_inline low_nibble 0 reserved"};
         return NegInt{U128{low, 0}};
      case 4: {
         const std::size_t bc = static_cast<std::size_t>(low) + 1;
         if (buf.size() < 1 + bc) throw DecodeError{"uint magnitude truncated"};
         return Uint{read_u128_le(buf.subspan(1, bc))};
      }
      case 5: {
         const std::size_t bc = static_cast<std::size_t>(low) + 1;
         if (buf.size() < 1 + bc) throw DecodeError{"negint magnitude truncated"};
         const auto mag = read_u128_le(buf.subspan(1, bc));
         if (mag.is_zero()) throw DecodeError{"negint with all-zero payload reserved"};
         return NegInt{mag};
      }
      case 6: {
         if (low & 0x08) throw DecodeError{"reserved low_nibble for ieee_float (bit 3)"};
         const std::uint8_t width_log2 = low & 0x07;
         if (width_log2 < 1 || width_log2 > 4) {
            throw DecodeError{"ieee_float bad width selector"};
         }
         const std::size_t byte_count = std::size_t{1} << width_log2;
         if (buf.size() < 1 + byte_count) throw DecodeError{"ieee_float payload truncated"};
         return Float{width_log2, read_u128_le(buf.subspan(1, byte_count))};
      }
      case 7: {
         const std::size_t bc = static_cast<std::size_t>(low) + 1;
         if (buf.size() < 1 + bc) throw DecodeError{"decimal mantissa truncated"};
         const U128 zz = read_u128_le(buf.subspan(1, bc));
         const I128 mantissa = zigzag_decode_u128(zz);
         const auto scale_result = varscale_decode(buf.subspan(1 + bc));
         return Decimal{mantissa, scale_result.value};
      }
      case 11: {
         // §5.1/§5.1.1 array dispatch.
         if (low == 0) {
            return decode_generic_array(buf, depth);
         }
         if (low >= 1 && low <= 10) {
            return decode_typed_array(buf, static_cast<std::uint8_t>(low - 1));
         }
         throw DecodeError{"reserved low_nibble for array"};
      }
      case 12: {
         if (low == 0) return decode_object(buf, depth);
         if (low == 1) return decode_row_array(buf, depth);
         throw DecodeError{"reserved low_nibble for object"};
      }
      case 9: {
         // §4.9 string.
         if (low > 1) throw DecodeError{"reserved low_nibble for string"};
         String s;
         s.encoding_flag = low;
         s.content.assign(buf.begin() + 1, buf.end());
         return s;
      }
      case 10: {
         // §4.10 bytes.
         if (low > 3) throw DecodeError{"reserved low_nibble for bytes"};
         Bytes b;
         b.encoding_hint = low;
         b.content.assign(buf.begin() + 1, buf.end());
         return b;
      }
      case 8: {
         // §4.8 numeric_string. low_nibble must be 0; body is an
         // inner numeric value (codes 2..7).
         if (low != 0) throw DecodeError{"reserved low_nibble for numeric_string"};
         Value inner = decode_at_depth(buf.subspan(1), depth + 1);
         if (!is_numeric_value(inner))
            throw DecodeError{"numeric_string inner must be numeric (codes 2..7)"};
         return make_numeric_string(std::move(inner));
      }
      case 13: {
         // §4.11 extension. low_nibble = subtype id (0..15);
         // remainder of buf is opaque body.
         Extension e;
         e.subtype = static_cast<std::uint8_t>(low);
         e.bytes.assign(buf.begin() + 1, buf.end());
         return e;
      }
      case 14: case 15:
         throw DecodeError{std::string{"reserved tag 0x"} +
                           static_cast<char>("0123456789ABCDEF"[high]) + "0"};
   }
   throw DecodeError{"unreachable"};
}

// §5.1 generic-array decode. `buf` starts at the tag byte (0xB0).
static Value decode_generic_array(std::span<const std::uint8_t> buf, unsigned depth) {
   if (buf.size() < 4) throw DecodeError{"array minimum size"};
   const std::uint8_t width_byte = buf[1];
   const std::size_t  slot_w_code = width_byte & 0x03;
   if ((width_byte & 0xFC) != 0) {
      throw DecodeError{"reserved high bits in array width byte"};
   }
   const std::size_t  slot_w = slot_w_code + 1;
   const std::size_t  n = static_cast<std::size_t>(buf[buf.size() - 2])
                       | (static_cast<std::size_t>(buf[buf.size() - 1]) << 8);
   const std::size_t  overhead = 4 + slot_w * n;
   if (buf.size() < overhead) throw DecodeError{"array slots/count truncated"};
   const std::size_t  value_data_size = buf.size() - overhead;

   std::size_t expected_slot_w;
   if      (value_data_size <= 0xFF)         expected_slot_w = 1;
   else if (value_data_size <= 0xFFFF)       expected_slot_w = 2;
   else if (value_data_size <= 0xFF'FFFF)    expected_slot_w = 3;
   else                                       expected_slot_w = 4;
   if (slot_w < expected_slot_w) {
      throw DecodeError{"slot width too small for value_data"};
   }

   const std::size_t value_data_start = 2;
   const std::size_t slot_table_start = value_data_start + value_data_size;

   auto read_slot = [&](std::size_t i) -> std::size_t {
      std::size_t pos = slot_table_start + i * slot_w;
      std::size_t off = 0;
      for (std::size_t b = 0; b < slot_w; ++b) {
         off |= static_cast<std::size_t>(buf[pos + b]) << (8 * b);
      }
      return off;
   };

   std::vector<Value> children;
   children.reserve(n);
   for (std::size_t i = 0; i < n; ++i) {
      const std::size_t off = read_slot(i);
      const std::size_t next_off = (i + 1 < n) ? read_slot(i + 1) : value_data_size;
      if (off > value_data_size || next_off < off || next_off > value_data_size) {
         throw DecodeError{"array slot offset OOB or non-monotonic"};
      }
      const std::size_t child_size = next_off - off;
      auto child_span = buf.subspan(value_data_start + off, child_size);
      children.push_back(decode_at_depth(child_span, depth + 1));
   }
   return make_array(std::move(children));
}

// §5.2.1 row_array decode. `buf` starts at the tag byte (0xC1).
static Value decode_row_array(std::span<const std::uint8_t> buf, unsigned depth) {
   if (buf.size() < 5) throw DecodeError{"row_array minimum size"};
   const std::uint8_t width_byte = buf[1];
   const std::size_t  slot_w_code   = width_byte & 0x03;
   const std::size_t  recoff_w_code = (width_byte >> 2) & 0x03;
   if ((width_byte & 0xF0) != 0)
      throw DecodeError{"reserved high bits in row_array width byte"};
   const std::size_t slot_w = slot_w_code + 1;
   const std::size_t recoff_w = recoff_w_code + 1;
   const std::size_t n = static_cast<std::size_t>(buf[buf.size() - 2])
                       | (static_cast<std::size_t>(buf[buf.size() - 1]) << 8);

   const auto k_vu = varuint_decode(buf.subspan(2));
   const std::size_t k = static_cast<std::size_t>(k_vu.value);
   std::size_t pos = 2 + k_vu.used;

   const std::size_t need_slots = k * 4;
   if (buf.size() < pos + need_slots) throw DecodeError{"row_array shared key slots truncated"};
   std::vector<std::pair<std::uint32_t, std::uint8_t>> key_slots;
   key_slots.reserve(k);
   for (std::size_t i = 0; i < k; ++i) {
      const std::size_t sp = pos + i * 4;
      const std::uint32_t s = static_cast<std::uint32_t>(buf[sp])
                            | (static_cast<std::uint32_t>(buf[sp + 1]) << 8)
                            | (static_cast<std::uint32_t>(buf[sp + 2]) << 16)
                            | (static_cast<std::uint32_t>(buf[sp + 3]) << 24);
      key_slots.emplace_back(s & 0x00FF'FFFFu,
                             static_cast<std::uint8_t>(s >> 24));
   }
   pos += need_slots;

   if (buf.size() < pos + k) throw DecodeError{"row_array hash array truncated"};
   const std::size_t hash_start = pos;
   pos += k;

   std::size_t total_key_size = 0;
   for (const auto& [_off, ksz] : key_slots) total_key_size += ksz;
   if (buf.size() < pos + total_key_size) throw DecodeError{"row_array shared keys area truncated"};
   const std::size_t keys_area_start = pos;
   pos += total_key_size;

   std::vector<std::string> keys;
   keys.reserve(k);
   for (std::size_t i = 0; i < k; ++i) {
      const auto [koff, ksz] = key_slots[i];
      if (static_cast<std::size_t>(koff) + ksz > total_key_size)
         throw DecodeError{"row_array key slot OOB"};
      const auto* p = reinterpret_cast<const char*>(buf.data() + keys_area_start + koff);
      std::string key(p, ksz);
      if (buf[hash_start + i] != key_hash8(key))
         throw DecodeError{"row_array hash byte mismatch"};
      keys.push_back(std::move(key));
   }

   if (buf.size() < 2 + n * recoff_w) throw DecodeError{"row_array record_offsets"};
   const std::size_t record_offsets_end   = buf.size() - 2;
   const std::size_t record_offsets_start = record_offsets_end - n * recoff_w;
   if (record_offsets_start < pos) throw DecodeError{"row_array record_offsets overlap header"};
   const std::size_t records_body_start = pos;
   const std::size_t records_body_size  = record_offsets_start - records_body_start;

   auto read_recoff = [&](std::size_t i) -> std::size_t {
      const std::size_t p = record_offsets_start + i * recoff_w;
      std::size_t v = 0;
      for (std::size_t b = 0; b < recoff_w; ++b)
         v |= static_cast<std::size_t>(buf[p + b]) << (8 * b);
      return v;
   };

   std::vector<std::vector<Value>> rows;
   rows.reserve(n);
   for (std::size_t i = 0; i < n; ++i) {
      const std::size_t rec_off  = read_recoff(i);
      const std::size_t next_off = (i + 1 < n) ? read_recoff(i + 1) : records_body_size;
      if (rec_off > records_body_size || next_off < rec_off || next_off > records_body_size)
         throw DecodeError{"row_array record offset OOB or non-monotonic"};
      const std::size_t rec_size = next_off - rec_off;
      if (rec_size < k * slot_w)
         throw DecodeError{"row_array record too small for slot table"};
      const std::size_t value_data_size = rec_size - k * slot_w;
      const std::size_t rec_start = records_body_start + rec_off;
      const std::size_t slot_table_start = rec_start + value_data_size;

      auto read_slot = [&](std::size_t j) -> std::size_t {
         const std::size_t p = slot_table_start + j * slot_w;
         std::size_t v = 0;
         for (std::size_t b = 0; b < slot_w; ++b)
            v |= static_cast<std::size_t>(buf[p + b]) << (8 * b);
         return v;
      };

      std::vector<Value> row;
      row.reserve(k);
      for (std::size_t j = 0; j < k; ++j) {
         const std::size_t off = read_slot(j);
         const std::size_t next_field_off = (j + 1 < k) ? read_slot(j + 1) : value_data_size;
         if (off > value_data_size || next_field_off < off || next_field_off > value_data_size)
            throw DecodeError{"row_array slot offset OOB or non-monotonic"};
         row.push_back(decode_at_depth(buf.subspan(rec_start + off, next_field_off - off), depth + 1));
      }
      rows.push_back(std::move(row));
   }

   return make_row_array(std::move(keys), std::move(rows));
}

// §5.2 object decode. `buf` starts at the tag byte (0xC0).
static Value decode_object(std::span<const std::uint8_t> buf, unsigned depth) {
   if (buf.size() < 4) throw DecodeError{"object minimum size"};
   const std::uint8_t width_byte = buf[1];
   const std::size_t  slot_w_code = width_byte & 0x03;
   if ((width_byte & 0xFC) != 0)
      throw DecodeError{"reserved high bits in object width byte"};
   const std::size_t slot_w = slot_w_code + 1;
   const std::size_t n = static_cast<std::size_t>(buf[buf.size() - 2])
                       | (static_cast<std::size_t>(buf[buf.size() - 1]) << 8);
   const std::size_t overhead = 4 + n + (slot_w + 1) * n;
   if (buf.size() < overhead) throw DecodeError{"object slots/hash/count truncated"};
   const std::size_t value_data_size = buf.size() - overhead;

   const std::size_t value_data_start = 2;
   const std::size_t hash_table_start = value_data_start + value_data_size;
   const std::size_t slot_table_start = hash_table_start + n;

   auto read_slot_offset = [&](std::size_t i) -> std::size_t {
      const std::size_t pos = slot_table_start + i * (slot_w + 1);
      std::size_t off = 0;
      for (std::size_t b = 0; b < slot_w; ++b)
         off |= static_cast<std::size_t>(buf[pos + b]) << (8 * b);
      return off;
   };
   auto read_slot_key_size = [&](std::size_t i) -> std::uint8_t {
      return buf[slot_table_start + i * (slot_w + 1) + slot_w];
   };

   std::vector<std::pair<std::string, Value>> entries;
   entries.reserve(n);
   for (std::size_t i = 0; i < n; ++i) {
      const std::size_t off       = read_slot_offset(i);
      const std::uint8_t ksize_b  = read_slot_key_size(i);
      const std::size_t next_off  = (i + 1 < n) ? read_slot_offset(i + 1) : value_data_size;
      if (off > value_data_size || next_off < off || next_off > value_data_size)
         throw DecodeError{"object slot offset OOB or non-monotonic"};
      const std::size_t entry_size = next_off - off;
      auto entry_span = buf.subspan(value_data_start + off, entry_size);

      std::string key;
      std::size_t prefix_size, key_size;
      if (ksize_b != 0xFF) {
         key_size = ksize_b;
         if (key_size > entry_span.size())
            throw DecodeError{"object key bytes truncated"};
         key.assign(reinterpret_cast<const char*>(entry_span.data()), key_size);
         prefix_size = 0;
      } else {
         const auto vu = varuint_decode(entry_span);
         prefix_size = vu.used;
         key_size = std::size_t{0xFF} + vu.value;
         if (prefix_size + key_size > entry_span.size())
            throw DecodeError{"object long-key bytes truncated"};
         key.assign(reinterpret_cast<const char*>(entry_span.data() + prefix_size), key_size);
      }

      const std::uint8_t stored_hash = buf[hash_table_start + i];
      if (stored_hash != key_hash8(key))
         throw DecodeError{"object hash byte mismatch"};

      const std::size_t child_start = prefix_size + key_size;
      auto child_span = entry_span.subspan(child_start);
      entries.emplace_back(std::move(key), decode_at_depth(child_span, depth + 1));
   }
   return make_object(std::move(entries));
}

// §5.1.1 typed-array decode. `buf` starts at the tag byte; the
// caller has already extracted element_code from the low nibble.
static Value decode_typed_array(std::span<const std::uint8_t> buf,
                                std::uint8_t element_code) {
   const std::size_t esize = typed_array_element_size(element_code);
   if (esize == 0) throw DecodeError{"typed_array element_code out of range"};
   if (buf.size() < 3) throw DecodeError{"typed_array minimum size"};
   const std::size_t n = static_cast<std::size_t>(buf[buf.size() - 2])
                       | (static_cast<std::size_t>(buf[buf.size() - 1]) << 8);
   const std::size_t body_len = n * esize;
   const std::size_t expected = 1 + body_len + 2;
   if (buf.size() != expected) throw DecodeError{"typed_array size mismatch"};
   TypedArray ta;
   ta.element_code = element_code;
   ta.raw.assign(buf.begin() + 1, buf.begin() + 1 + body_len);
   return ta;
}

// ── JSON projection ────────────────────────────────────────────────

// binary16 → f64 widening (psio-style: NaN canonicalized with sign
// preserved; subnormals normalized).
static double f16_bits_to_f64(std::uint16_t bits) noexcept {
   const std::uint64_t sign = (bits >> 15) & 1;
   const int          exp  = (bits >> 10) & 0x1F;
   const std::uint64_t mant = bits & 0x3FF;
   const std::uint64_t sign_bit = sign << 63;
   union { std::uint64_t u; double d; } u;
   if (exp == 0) {
      if (mant == 0) {
         u.u = sign_bit;
         return u.d;
      }
      // subnormal: normalize
      std::uint64_t m = mant;
      int e = -14;
      while ((m & 0x400) == 0) { m <<= 1; --e; }
      m &= 0x3FF;
      const std::uint64_t new_exp = static_cast<std::uint64_t>(e + 1023) << 52;
      u.u = sign_bit | new_exp | (m << (52 - 10));
      return u.d;
   }
   if (exp == 0x1F) {
      const std::uint64_t new_exp = std::uint64_t{0x7FF} << 52;
      u.u = sign_bit | new_exp | (mant << (52 - 10));
      return u.d;
   }
   const std::uint64_t new_exp = static_cast<std::uint64_t>(exp - 15 + 1023) << 52;
   u.u = sign_bit | new_exp | (mant << (52 - 10));
   return u.d;
}

// Render a U128 as decimal text without leading zeros.
static std::string u128_to_decimal(U128 n) {
   if (n.is_zero()) return "0";
   std::string out;
   while (!n.is_zero()) {
      // long-division by 10
      U128 quot{};
      std::uint64_t r = 0;
      for (int i = 1; i >= 0; --i) {
         std::uint64_t hi_lo = (i == 1) ? n.hi : n.lo;
         // Process 32-bit halves for portability without __int128.
         std::uint64_t high_part = (r << 32) | (hi_lo >> 32);
         std::uint64_t high_q    = high_part / 10;
         std::uint64_t high_r    = high_part % 10;
         std::uint64_t low_part  = (high_r << 32) | (hi_lo & 0xFFFFFFFFull);
         std::uint64_t low_q     = low_part / 10;
         std::uint64_t low_r     = low_part % 10;
         std::uint64_t q_word    = (high_q << 32) | low_q;
         if (i == 1) quot.hi = q_word;
         else        quot.lo = q_word;
         r = low_r;
      }
      out.push_back(static_cast<char>('0' + r));
      n = quot;
   }
   std::reverse(out.begin(), out.end());
   return out;
}

static std::string format_decimal(I128 mantissa, std::int32_t scale) {
   const bool negative = mantissa.is_negative();
   U128 mag;
   if (negative) {
      // two's complement: -v = ~v + 1
      U128 x{static_cast<std::uint64_t>(mantissa.lo),
             static_cast<std::uint64_t>(mantissa.hi)};
      x.lo = ~x.lo;
      x.hi = ~x.hi;
      mag.lo = x.lo + 1;
      mag.hi = x.hi + (mag.lo == 0 ? 1 : 0);  // carry
   } else {
      mag.lo = mantissa.lo;
      mag.hi = static_cast<std::uint64_t>(mantissa.hi);
   }
   std::string mag_str = u128_to_decimal(mag);
   const std::string sign = negative ? "-" : "";
   if (scale == 0) {
      return sign + mag_str;
   }
   if (scale > 0) {
      std::string s = sign + mag_str + std::string(static_cast<std::size_t>(scale), '0');
      return s;
   }
   const std::size_t neg = static_cast<std::size_t>(-scale);
   if (neg < mag_str.size()) {
      const std::size_t dot = mag_str.size() - neg;
      return sign + mag_str.substr(0, dot) + "." + mag_str.substr(dot);
   }
   return sign + "0." + std::string(neg - mag_str.size(), '0') + mag_str;
}

// ── §7.5 JSON emitter options ──────────────────────────────────────

enum class IntStringMode { Never, LargeOnly, All };

struct EmitOptions {
   bool          pretty           = false;
   std::uint8_t  indent           = 2;     // 0 = use tabs
   IntStringMode int_string_mode  = IntStringMode::Never;
};

inline constexpr U128 JS_MAX_SAFE_INTEGER{(1ull << 53) - 1, 0};

inline bool quote_int_for_mode(const U128& mag, IntStringMode mode) {
   switch (mode) {
      case IntStringMode::Never:     return false;
      case IntStringMode::All:       return true;
      case IntStringMode::LargeOnly:
         return mag.hi != 0 || mag.lo > JS_MAX_SAFE_INTEGER.lo;
   }
   return false;
}

inline void write_indent(const EmitOptions& opts, std::size_t depth, std::string& out) {
   if (!opts.pretty) return;
   out += '\n';
   if (opts.indent == 0) {
      for (std::size_t i = 0; i < depth; ++i) out += '\t';
   } else {
      for (std::size_t i = 0; i < depth * opts.indent; ++i) out += ' ';
   }
}

static std::string render_json(const Value& v);
static void render_with_opts_into(const Value& v, const EmitOptions& opts,
                                   std::size_t depth, std::string& out);

static std::string render_json_with(const Value& v, const EmitOptions& opts) {
   std::string out;
   render_with_opts_into(v, opts, 0, out);
   return out;
}

static std::string render_json(const Value& v) {
   return std::visit([&](auto&& arg) -> std::string {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, Null>) {
         return "null";
      } else if constexpr (std::is_same_v<T, Bool>) {
         return arg.value ? "true" : "false";
      } else if constexpr (std::is_same_v<T, Uint>) {
         return u128_to_decimal(arg.value);
      } else if constexpr (std::is_same_v<T, NegInt>) {
         return "-" + u128_to_decimal(arg.magnitude);
      } else if constexpr (std::is_same_v<T, Float>) {
         double f = std::numeric_limits<double>::quiet_NaN();
         union { std::uint64_t u; double d; float f32; } u;
         if      (arg.width_log2 == 3) { u.u = arg.bits.lo; f = u.d; }
         else if (arg.width_log2 == 2) {
            std::uint32_t bits32 = static_cast<std::uint32_t>(arg.bits.lo);
            std::memcpy(&u.f32, &bits32, sizeof(u.f32));
            f = static_cast<double>(u.f32);
         }
         else if (arg.width_log2 == 1) {
            f = f16_bits_to_f64(static_cast<std::uint16_t>(arg.bits.lo));
         }
         else if (arg.width_log2 == 4) {
            // binary128 → binary64 via vendored SoftFloat subset.
            std::uint8_t buf[16];
            for (int i = 0; i < 8; ++i) {
               buf[i]     = static_cast<std::uint8_t>(arg.bits.lo >> (8 * i));
               buf[i + 8] = static_cast<std::uint8_t>(arg.bits.hi >> (8 * i));
            }
            f = psio_softfloat_f128_to_f64(buf);
         }
         if (std::isnan(f))                       return "NaN";
         if (f ==  std::numeric_limits<double>::infinity())  return "Infinity";
         if (f == -std::numeric_limits<double>::infinity())  return "-Infinity";
         if (std::trunc(f) == f && std::abs(f) < 1e16) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.0f", f);
            return buf;
         }
         char buf[32];
         std::snprintf(buf, sizeof buf, "%g", f);
         return buf;
      } else if constexpr (std::is_same_v<T, Decimal>) {
         return format_decimal(arg.mantissa, arg.scale);
      } else if constexpr (std::is_same_v<T, Array>) {
         std::string s = "[";
         const auto& children = arg.body ? arg.body->children
                                         : std::vector<Value>{};
         bool first = true;
         for (const auto& c : children) {
            if (!first) s += ",";
            first = false;
            s += render_json(c);
         }
         s += "]";
         return s;
      } else if constexpr (std::is_same_v<T, String>) {
         std::string s = "\"";
         std::string_view text(reinterpret_cast<const char*>(arg.content.data()),
                                arg.content.size());
         if (arg.encoding_flag == 0) {
            json_escape_into(text, s);
         } else {
            s += text;
         }
         s += "\"";
         return s;
      } else if constexpr (std::is_same_v<T, Bytes>) {
         std::string body;
         switch (arg.encoding_hint) {
            case 0: body = b64_encode(arg.content); break;
            case 1: body = hex_encode(arg.content); break;
            case 2: body = base58_encode(arg.content); break;
            case 3: body = b64url_encode(arg.content); break;
            default: body = "<bad bytes hint>";
         }
         return "\"" + body + "\"";
      } else if constexpr (std::is_same_v<T, NumericString>) {
         if (!arg.body) return "<bad numeric_string>";
         return "\"" + render_json(arg.body->inner) + "\"";
      } else if constexpr (std::is_same_v<T, Object>) {
         std::string s = "{";
         const auto& entries = arg.body ? arg.body->entries
                                        : std::vector<std::pair<std::string, Value>>{};
         bool first = true;
         for (const auto& [key, value] : entries) {
            if (!first) s += ",";
            first = false;
            s += "\"";
            json_escape_into(key, s);
            s += "\":";
            s += render_json(value);
         }
         s += "}";
         return s;
      } else if constexpr (std::is_same_v<T, RowArray>) {
         std::string s = "[";
         if (arg.body) {
            const auto& keys = arg.body->keys;
            const auto& rows = arg.body->rows;
            bool first_row = true;
            for (const auto& row : rows) {
               if (!first_row) s += ",";
               first_row = false;
               s += "{";
               bool first_field = true;
               for (std::size_t j = 0; j < keys.size(); ++j) {
                  if (!first_field) s += ",";
                  first_field = false;
                  s += "\"";
                  json_escape_into(keys[j], s);
                  s += "\":";
                  s += render_json(row[j]);
               }
               s += "}";
            }
         }
         s += "]";
         return s;
      } else if constexpr (std::is_same_v<T, TypedArray>) {
         const std::size_t esize = typed_array_element_size(arg.element_code);
         if (esize == 0) return "<bad typed_array>";
         const std::size_t n = arg.raw.size() / esize;
         std::string s = "[";
         for (std::size_t i = 0; i < n; ++i) {
            if (i > 0) s += ",";
            const std::uint8_t* el = arg.raw.data() + i * esize;
            char buf[40];
            switch (arg.element_code) {
               case 0: {
                  std::int8_t v;
                  std::memcpy(&v, el, 1);
                  std::snprintf(buf, sizeof buf, "%d", v);
                  s += buf;
                  break;
               }
               case 1: {
                  std::int16_t v;
                  std::memcpy(&v, el, 2);
                  std::snprintf(buf, sizeof buf, "%d", v);
                  s += buf;
                  break;
               }
               case 2: {
                  std::int32_t v;
                  std::memcpy(&v, el, 4);
                  std::snprintf(buf, sizeof buf, "%d", v);
                  s += buf;
                  break;
               }
               case 3: {
                  std::int64_t v;
                  std::memcpy(&v, el, 8);
                  std::snprintf(buf, sizeof buf, "%lld",
                                static_cast<long long>(v));
                  s += buf;
                  break;
               }
               case 4: s += std::to_string(el[0]); break;
               case 5: {
                  std::uint16_t v;
                  std::memcpy(&v, el, 2);
                  s += std::to_string(v);
                  break;
               }
               case 6: {
                  std::uint32_t v;
                  std::memcpy(&v, el, 4);
                  s += std::to_string(v);
                  break;
               }
               case 7: {
                  std::uint64_t v;
                  std::memcpy(&v, el, 8);
                  s += std::to_string(v);
                  break;
               }
               case 8: {
                  float fv;
                  std::memcpy(&fv, el, 4);
                  const double d = static_cast<double>(fv);
                  if (std::isnan(d)) { s += "NaN"; break; }
                  if (d ==  std::numeric_limits<double>::infinity())  { s += "Infinity";  break; }
                  if (d == -std::numeric_limits<double>::infinity())  { s += "-Infinity"; break; }
                  if (std::trunc(d) == d && std::abs(d) < 1e16) {
                     std::snprintf(buf, sizeof buf, "%.0f", d);
                  } else {
                     std::snprintf(buf, sizeof buf, "%g", d);
                  }
                  s += buf;
                  break;
               }
               case 9: {
                  double d;
                  std::memcpy(&d, el, 8);
                  if (std::isnan(d)) { s += "NaN"; break; }
                  if (d ==  std::numeric_limits<double>::infinity())  { s += "Infinity";  break; }
                  if (d == -std::numeric_limits<double>::infinity())  { s += "-Infinity"; break; }
                  if (std::trunc(d) == d && std::abs(d) < 1e16) {
                     std::snprintf(buf, sizeof buf, "%.0f", d);
                  } else {
                     std::snprintf(buf, sizeof buf, "%g", d);
                  }
                  s += buf;
                  break;
               }
            }
         }
         s += "]";
         return s;
      } else if constexpr (std::is_same_v<T, Extension>) {
         // §4.11. Default JSON projection for an unknown extension is a
         // self-describing envelope: {"__pjson_ext":{"subtype":N,"bytes_b64":"..."}}
         std::string s = "{\"__pjson_ext\":{\"subtype\":";
         s += std::to_string(static_cast<unsigned>(arg.subtype));
         s += ",\"bytes_b64\":\"";
         s += b64_encode(arg.bytes);
         s += "\"}}";
         return s;
      }
   }, v);
}

// §7.5 emitter with options. Differs from `render_json` only for:
//   - bare integers (Uint, NegInt) — quoted per `int_string_mode`
//   - aggregates (Array, Object, RowArray) — when `pretty=true`,
//     each element / entry on its own indented line
// `numeric_string` and `bytes` are always quoted regardless of mode;
// `Float`, `Decimal`, `String`, `TypedArray` re-use the simple path.
static void render_with_opts_into(const Value& v, const EmitOptions& opts,
                                   std::size_t depth, std::string& out) {
   std::visit([&](auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, Null>)  { out += "null"; }
      else if constexpr (std::is_same_v<T, Bool>)  {
         out += arg.value ? "true" : "false";
      }
      else if constexpr (std::is_same_v<T, Uint>) {
         const bool quoted = quote_int_for_mode(arg.value, opts.int_string_mode);
         if (quoted) out += '"';
         out += u128_to_decimal(arg.value);
         if (quoted) out += '"';
      }
      else if constexpr (std::is_same_v<T, NegInt>) {
         const bool quoted = quote_int_for_mode(arg.magnitude, opts.int_string_mode);
         if (quoted) out += '"';
         out += '-';
         out += u128_to_decimal(arg.magnitude);
         if (quoted) out += '"';
      }
      else if constexpr (std::is_same_v<T, Array>) {
         const auto& children = arg.body ? arg.body->children
                                         : std::vector<Value>{};
         if (children.empty()) { out += "[]"; return; }
         out += '[';
         for (std::size_t i = 0; i < children.size(); ++i) {
            if (i > 0) out += ',';
            write_indent(opts, depth + 1, out);
            render_with_opts_into(children[i], opts, depth + 1, out);
         }
         write_indent(opts, depth, out);
         out += ']';
      }
      else if constexpr (std::is_same_v<T, Object>) {
         const auto& entries = arg.body ? arg.body->entries
                                        : std::vector<std::pair<std::string, Value>>{};
         if (entries.empty()) { out += "{}"; return; }
         out += '{';
         for (std::size_t i = 0; i < entries.size(); ++i) {
            if (i > 0) out += ',';
            write_indent(opts, depth + 1, out);
            out += '"';
            json_escape_into(entries[i].first, out);
            out += "\":";
            if (opts.pretty) out += ' ';
            render_with_opts_into(entries[i].second, opts, depth + 1, out);
         }
         write_indent(opts, depth, out);
         out += '}';
      }
      else if constexpr (std::is_same_v<T, RowArray>) {
         if (!arg.body || arg.body->rows.empty()) { out += "[]"; return; }
         const auto& keys = arg.body->keys;
         out += '[';
         for (std::size_t i = 0; i < arg.body->rows.size(); ++i) {
            if (i > 0) out += ',';
            write_indent(opts, depth + 1, out);
            out += '{';
            for (std::size_t j = 0; j < keys.size(); ++j) {
               if (j > 0) out += ',';
               write_indent(opts, depth + 2, out);
               out += '"';
               json_escape_into(keys[j], out);
               out += "\":";
               if (opts.pretty) out += ' ';
               render_with_opts_into(arg.body->rows[i][j], opts, depth + 2, out);
            }
            write_indent(opts, depth + 1, out);
            out += '}';
         }
         write_indent(opts, depth, out);
         out += ']';
      }
      else {
         // Float, Decimal, String, NumericString, Bytes, TypedArray
         // are all unaffected by the §7.5 options — fall back to the
         // simple renderer.
         out += render_json(v);
      }
   }, v);
}

// ── Canonical encoding helpers ─────────────────────────────────────

// Multiply `n` by 5^k with i128 overflow detection. Returns false on
// overflow — the picker treats that as "not exactly representable".
static bool mul_pow5_i128(I128& n, std::uint32_t k) noexcept {
   for (std::uint32_t i = 0; i < k; ++i) {
      // n *= 5  with overflow check.
      __int128 lhs = (static_cast<__int128>(n.hi) << 64)
                   | static_cast<__int128>(n.lo);
      __int128 prod;
      if (__builtin_mul_overflow(lhs, static_cast<__int128>(5), &prod))
         return false;
      n.lo = static_cast<std::uint64_t>(prod);
      n.hi = static_cast<std::int64_t>(prod >> 64);
   }
   return true;
}

static bool i128_shl(I128& n, std::uint32_t k) noexcept {
   if (k >= 128) return false;
   __int128 v = (static_cast<__int128>(n.hi) << 64)
              | static_cast<__int128>(n.lo);
   __int128 shifted = v << k;
   if ((shifted >> k) != v) return false;     // overflowed bits out
   n.lo = static_cast<std::uint64_t>(shifted);
   n.hi = static_cast<std::int64_t>(shifted >> 64);
   return true;
}

// True iff `f` is bit-exactly equal to mantissa·10^scale. Uses
// exact integer arithmetic on f64's underlying p·2^e form. Mirror
// of the Rust `decimal_f64_roundtrips`.
static bool decimal_f64_roundtrips(I128 mantissa, std::int32_t scale, double f) {
   if (!std::isfinite(f)) return false;
   std::uint64_t bits;
   std::memcpy(&bits, &f, sizeof bits);
   const bool sign_neg = ((bits >> 63) & 1u) != 0;
   const int  exp      = static_cast<int>((bits >> 52) & 0x7FFu);
   const std::uint64_t frac = bits & 0x000F'FFFF'FFFF'FFFFull;
   if (exp == 0x7FF) return false;

   I128 p{};
   int  e;
   if (exp == 0) {
      if (frac == 0) {
         // ±0 matches mantissa == 0 only.
         return mantissa.lo == 0 && mantissa.hi == 0;
      }
      p = I128{frac, 0};
      e = -1074;
   } else {
      p = I128{(1ull << 52) | frac, 0};
      e = exp - 1023 - 52;
   }
   if (sign_neg) {
      // Two's-complement negate.
      __int128 v = (static_cast<__int128>(p.hi) << 64)
                 | static_cast<__int128>(p.lo);
      v = -v;
      p.lo = static_cast<std::uint64_t>(v);
      p.hi = static_cast<std::int64_t>(v >> 64);
   }

   // Build both sides of the equation
   //   m · 10^s == p · 2^e
   // by absorbing 5^k onto whichever side holds it, then shifting
   // by 2^k onto the other.
   I128 m_side = mantissa;
   I128 p_side = p;
   std::int64_t shift_on_m = 0;
   if (scale >= 0) {
      if (!mul_pow5_i128(m_side, static_cast<std::uint32_t>(scale))) return false;
      shift_on_m = static_cast<std::int64_t>(scale) - static_cast<std::int64_t>(e);
   } else {
      const std::uint32_t s_abs = static_cast<std::uint32_t>(-scale);
      if (!mul_pow5_i128(p_side, s_abs)) return false;
      shift_on_m = -(static_cast<std::int64_t>(e) + static_cast<std::int64_t>(s_abs));
   }

   if (shift_on_m >= 0) {
      if (shift_on_m >= 128) return false;
      if (!i128_shl(m_side, static_cast<std::uint32_t>(shift_on_m))) return false;
      return m_side == p_side;
   } else {
      const std::uint32_t k = static_cast<std::uint32_t>(-shift_on_m);
      if (k >= 128) return false;
      if (!i128_shl(p_side, k)) return false;
      return m_side == p_side;
   }
}

static Value decimal_or_ieee_pick(I128 mantissa, std::int32_t scale, double f) {
   // Decimal candidate size: tag (1) + zigzag mantissa minimal bytes
   // + varscale (1..4).
   const auto zz = zigzag_encode_i128(mantissa);
   const std::size_t m_bc = u128_minimal_byte_count(zz);
   const std::uint32_t abs_scale = static_cast<std::uint32_t>(
      scale < 0 ? -static_cast<std::int64_t>(scale) : scale);
   const std::size_t scale_bc =
        (abs_scale <=          31u) ? 1
      : (abs_scale <=        8191u) ? 2
      : (abs_scale <=    2'097'151u) ? 3
      :                                4;
   const std::size_t decimal_size = 1 + m_bc + scale_bc;

   // Verify f exactly equals mantissa·10^scale before considering
   // ieee_float. Anything inexact (e.g., 0.1, 10^23) MUST encode as
   // decimal to preserve identity.
   if (!decimal_f64_roundtrips(mantissa, scale, f)) {
      Decimal d;
      d.mantissa = mantissa;
      d.scale = scale;
      return d;
   }

   // Find smallest bit-exact ieee width for f.
   union { std::uint64_t u; double f; } u;
   u.f = f;
   auto [w, bits] = canonical_float_width(3, U128{u.u, 0});
   const std::size_t ieee_size = 1u + (1u << w);

   if (decimal_size < ieee_size) {
      Decimal d;
      d.mantissa = mantissa;
      d.scale = scale;
      return d;
   }
   return Float{w, bits};
}

// ── Hex helpers ────────────────────────────────────────────────────

static std::vector<std::uint8_t> parse_hex(std::string_view s) {
   while (!s.empty() && (s.front() == ' ' || s.front() == '\n' || s.front() == '\t')) s.remove_prefix(1);
   while (!s.empty() && (s.back()  == ' ' || s.back()  == '\n' || s.back()  == '\t')) s.remove_suffix(1);
   if (s.size() % 2 != 0) {
      throw std::runtime_error{"hex string length not even"};
   }
   auto digit = [](char c) -> int {
      if ('0' <= c && c <= '9') return c - '0';
      if ('a' <= c && c <= 'f') return 10 + c - 'a';
      if ('A' <= c && c <= 'F') return 10 + c - 'A';
      throw std::runtime_error{std::string{"not a hex digit: "} + c};
   };
   std::vector<std::uint8_t> out;
   out.reserve(s.size() / 2);
   for (std::size_t i = 0; i < s.size(); i += 2) {
      out.push_back(static_cast<std::uint8_t>((digit(s[i]) << 4) | digit(s[i + 1])));
   }
   return out;
}

static std::string to_hex(std::span<const std::uint8_t> bytes) {
   static const char d[] = "0123456789abcdef";
   std::string out;
   out.reserve(bytes.size() * 2);
   for (auto b : bytes) {
      out.push_back(d[b >> 4]);
      out.push_back(d[b & 0xF]);
   }
   return out;
}

// ── Minimal JSON parser for fixture files ──────────────────────────
//
// The fixture format documented at conformance/README.md is a small
// subset of JSON: objects with string keys, strings (with the basic
// escape set), numbers (integer + decimal, no scientific notation in
// fixtures), bool, null, arrays. We hand-roll the parser to keep the
// driver self-contained without vendoring a 26k-line single-header
// JSON library.

struct JNode;
using JArray  = std::vector<JNode>;
using JObject = std::vector<std::pair<std::string, JNode>>;

// JFloat preserves the source token alongside the parsed double so
// the §4.7.2 / D-007 picker on JSON ingress can run the canonical-form
// path (mantissa, scale) instead of just the f64 bits.
//
// The token is a non-owning view into the JParser's source buffer.
// JNode is consumed before the source `std::string` goes out of
// scope (see the fixture-parsing flow in `parse_fixture` and the
// `from_json(text)` wrapper), so the view is always live during use.
// This avoids one std::string allocation per fractional JSON number.
struct JFloat {
   double           f{};
   std::string_view token;
};
struct JNode {
   std::variant<std::nullptr_t, bool, JFloat, std::int64_t, std::string,
                JArray, JObject>
       v;
};

class JParser {
public:
   explicit JParser(std::string_view s) : s_(s) {}

   JNode parse() {
      skip_ws();
      auto out = parse_value();
      skip_ws();
      if (pos_ != s_.size()) {
         throw std::runtime_error{"json: trailing data"};
      }
      return out;
   }

private:
   void skip_ws() {
      while (pos_ < s_.size() &&
             (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\t' ||
              s_[pos_] == '\r'))
         ++pos_;
   }

   char peek() {
      if (pos_ >= s_.size()) throw std::runtime_error{"json: unexpected EOF"};
      return s_[pos_];
   }
   char eat() {
      if (pos_ >= s_.size()) throw std::runtime_error{"json: unexpected EOF"};
      return s_[pos_++];
   }
   bool eat_if(char c) {
      if (pos_ < s_.size() && s_[pos_] == c) { ++pos_; return true; }
      return false;
   }
   void expect(char c) {
      if (eat() != c) throw std::runtime_error{std::string{"json: expected "} + c};
   }

   JNode parse_value() {
      skip_ws();
      char c = peek();
      if (c == '"') return JNode{parse_string()};
      if (c == '{') return JNode{parse_object()};
      if (c == '[') return JNode{parse_array()};
      if (c == 't' || c == 'f') return JNode{parse_bool()};
      if (c == 'n') { parse_keyword("null"); return JNode{nullptr}; }
      // number: optional minus, digits, optional . digits, optional e[+-]digits
      return parse_number();
   }

   void parse_keyword(std::string_view kw) {
      for (char c : kw) expect(c);
   }

   bool parse_bool() {
      if (peek() == 't') { parse_keyword("true"); return true; }
      parse_keyword("false");
      return false;
   }

   std::string parse_string() {
      expect('"');
      std::string out;
      while (true) {
         char c = eat();
         if (c == '"') return out;
         if (c == '\\') {
            char esc = eat();
            switch (esc) {
               case '"':  out.push_back('"');  break;
               case '\\': out.push_back('\\'); break;
               case '/':  out.push_back('/');  break;
               case 'n':  out.push_back('\n'); break;
               case 'r':  out.push_back('\r'); break;
               case 't':  out.push_back('\t'); break;
               case 'b':  out.push_back('\b'); break;
               case 'f':  out.push_back('\f'); break;
               default:
                  throw std::runtime_error{std::string{"json: unsupported escape \\"} + esc};
            }
         } else {
            out.push_back(c);
         }
      }
   }

   JNode parse_number() {
      const std::size_t start = pos_;
      if (peek() == '-') ++pos_;
      while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
      bool is_float = false;
      if (pos_ < s_.size() && s_[pos_] == '.') {
         is_float = true;
         ++pos_;
         while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
      }
      if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
         is_float = true;
         ++pos_;
         if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
         while (pos_ < s_.size() && (s_[pos_] >= '0' && s_[pos_] <= '9')) ++pos_;
      }
      std::string_view token = s_.substr(start, pos_ - start);
      if (is_float) {
         double v;
         auto [ptr, ec] = std::from_chars(token.data(),
                                          token.data() + token.size(), v);
         if (ec != std::errc()) throw std::runtime_error{"json: bad number"};
         return JNode{JFloat{v, token}};
      }
      // Integer path — try i64 first; on overflow fall through to JFloat
      // to preserve the source token for downstream u128 parsing.
      std::int64_t iv;
      auto [ptr, ec] = std::from_chars(token.data(),
                                       token.data() + token.size(), iv);
      if (ec == std::errc()) return JNode{iv};
      double v;
      auto [_p, _e] = std::from_chars(token.data(),
                                      token.data() + token.size(), v);
      if (_e != std::errc()) throw std::runtime_error{"json: bad number"};
      return JNode{JFloat{v, token}};
   }

   JArray parse_array() {
      expect('[');
      JArray out;
      skip_ws();
      if (eat_if(']')) return out;
      while (true) {
         out.push_back(parse_value());
         skip_ws();
         if (eat_if(']')) return out;
         expect(',');
      }
   }

   JObject parse_object() {
      expect('{');
      JObject out;
      skip_ws();
      if (eat_if('}')) return out;
      while (true) {
         skip_ws();
         std::string key = parse_string();
         skip_ws();
         expect(':');
         out.emplace_back(std::move(key), parse_value());
         skip_ws();
         if (eat_if('}')) return out;
         expect(',');
      }
   }

   std::string_view s_;
   std::size_t      pos_{0};
};

// Helpers to navigate JNodes.
static const JNode* obj_get(const JObject& o, std::string_view k) {
   for (const auto& [key, val] : o) {
      if (key == k) return &val;
   }
   return nullptr;
}

static const JObject& as_object(const JNode& n) {
   if (auto* o = std::get_if<JObject>(&n.v)) return *o;
   throw std::runtime_error{"json: expected object"};
}

static const std::string& as_string(const JNode& n) {
   if (auto* s = std::get_if<std::string>(&n.v)) return *s;
   throw std::runtime_error{"json: expected string"};
}

static bool as_bool(const JNode& n) {
   if (auto* b = std::get_if<bool>(&n.v)) return *b;
   throw std::runtime_error{"json: expected bool"};
}

// ── JSON ingress (§4.8 / §7.1) ────────────────────────────────────

// Build a pjson Value from the canonical-form check, returning the
// appropriate Uint/NegInt/Decimal. Caller provides the parsed
// mantissa string and scale; this synthesizes the Value.
inline Value canonical_string_to_numeric(std::string_view mantissa_str, std::int32_t scale) {
   const bool negative = !mantissa_str.empty() && mantissa_str[0] == '-';
   if (scale == 0) {
      // Integer
      if (negative) {
         // |v|  — strip the leading '-' then parse as u128.
         U128 mag{};
         for (std::size_t k = 1; k < mantissa_str.size(); ++k) {
            std::uint64_t lo_old = mag.lo;
            mag.lo *= 10;
            std::uint64_t carry = (static_cast<__uint128_t>(lo_old) * 10) >> 64;
            mag.hi = mag.hi * 10 + carry;
            std::uint64_t add = static_cast<std::uint64_t>(mantissa_str[k] - '0');
            std::uint64_t before = mag.lo;
            mag.lo += add;
            if (mag.lo < before) ++mag.hi;
         }
         return NegInt{mag};
      }
      U128 v{};
      for (char c : mantissa_str) {
         std::uint64_t lo_old = v.lo;
         v.lo *= 10;
         std::uint64_t carry = (static_cast<__uint128_t>(lo_old) * 10) >> 64;
         v.hi = v.hi * 10 + carry;
         std::uint64_t add = static_cast<std::uint64_t>(c - '0');
         std::uint64_t before = v.lo;
         v.lo += add;
         if (v.lo < before) ++v.hi;
      }
      return Uint{v};
   }
   // Decimal
   I128 mantissa{};
   if (negative) {
      // Parse |mantissa| as u128 then two's-complement.
      U128 mag{};
      for (std::size_t k = 1; k < mantissa_str.size(); ++k) {
         std::uint64_t lo_old = mag.lo;
         mag.lo *= 10;
         std::uint64_t carry = (static_cast<__uint128_t>(lo_old) * 10) >> 64;
         mag.hi = mag.hi * 10 + carry;
         std::uint64_t add = static_cast<std::uint64_t>(mantissa_str[k] - '0');
         std::uint64_t before = mag.lo;
         mag.lo += add;
         if (mag.lo < before) ++mag.hi;
      }
      U128 neg{~mag.lo, ~mag.hi};
      neg.lo += 1;
      if (neg.lo == 0) neg.hi += 1;
      mantissa = I128{neg.lo, static_cast<std::int64_t>(neg.hi)};
   } else {
      U128 v{};
      for (char c : mantissa_str) {
         std::uint64_t lo_old = v.lo;
         v.lo *= 10;
         std::uint64_t carry = (static_cast<__uint128_t>(lo_old) * 10) >> 64;
         v.hi = v.hi * 10 + carry;
         std::uint64_t add = static_cast<std::uint64_t>(c - '0');
         std::uint64_t before = v.lo;
         v.lo += add;
         if (v.lo < before) ++v.hi;
      }
      mantissa = I128{v.lo, static_cast<std::int64_t>(v.hi)};
   }
   return Decimal{mantissa, scale};
}

// JNode → pjson Value transcoder. Implements §4.8 numeric_string lift
// detection on string nodes; everything else maps directly.
//
// Honors the "host type controls" rule (E-005): byte inspection of
// strings happens ONLY here, on the JSON-source path.
static Value from_json_node(const JNode& n) {
   if (std::holds_alternative<std::nullptr_t>(n.v)) return Null{};
   if (auto* b = std::get_if<bool>(&n.v)) return Bool{*b};
   if (auto* i = std::get_if<std::int64_t>(&n.v)) {
      if (*i >= 0) {
         return Uint{U128{static_cast<std::uint64_t>(*i), 0}};
      }
      const std::uint64_t mag = static_cast<std::uint64_t>(-(*i));
      return NegInt{U128{mag, 0}};
   }
   if (auto* jf = std::get_if<JFloat>(&n.v)) {
      // §4.7.2 / D-007 / C-002. Run the full picker using the source
      // token so the canonical-form decimal candidate is available.
      // Falls back to width-minimizing ieee_float when the token is
      // not in canonical decimal form (e.g., sci-notation).
      std::string mant_str;
      std::int32_t scale = 0;
      if (parse_canonical_json_number_string(jf->token, mant_str, scale)
          && mant_str.find('.') == std::string::npos
          && mant_str.find('e') == std::string::npos) {
         // Got a canonical (mantissa_str, scale). Build Decimal candidate
         // and run the picker.
         Value dec_v = canonical_string_to_numeric(mant_str, scale);
         if (auto* dec = std::get_if<Decimal>(&dec_v)) {
            return decimal_or_ieee_pick(dec->mantissa, dec->scale, jf->f);
         }
         // Integer-valued canonical (no fractional, no scale<0): just
         // return the integer.
         return dec_v;
      }
      // Non-canonical token (sci-notation, etc.) — minimize ieee width.
      union { std::uint64_t u; double f; } u;
      u.f = jf->f;
      auto [w, bits] = canonical_float_width(3, U128{u.u, 0});
      return Float{w, bits};
   }
   if (auto* s = std::get_if<std::string>(&n.v)) {
      // §4.8 / §7.1 numeric_string lift detection.
      std::string mantissa_str;
      std::int32_t scale = 0;
      if (parse_canonical_json_number_string(*s, mantissa_str, scale)) {
         Value inner = canonical_string_to_numeric(mantissa_str, scale);
         // Reject NegInt(0) — should be impossible since the canonical
         // parser rejects "-0", but guard anyway.
         if (auto* ni = std::get_if<NegInt>(&inner); ni && ni->magnitude.is_zero()) {
            // Fall through to plain string.
         } else {
            return make_numeric_string(std::move(inner));
         }
      }
      String str;
      str.encoding_flag = 0;            // raw_text
      str.content.assign(s->begin(), s->end());
      return str;
   }
   if (auto* arr = std::get_if<JArray>(&n.v)) {
      std::vector<Value> children;
      children.reserve(arr->size());
      for (const auto& c : *arr) children.push_back(from_json_node(c));
      // §5.2.1.5 / RA-003: lift homogeneous array-of-object to row_array.
      if (!children.empty()) {
         std::vector<std::string> first_keys;
         bool homogeneous = true;
         for (std::size_t i = 0; i < children.size(); ++i) {
            const auto* obj = std::get_if<Object>(&children[i]);
            if (!obj || !obj->body) { homogeneous = false; break; }
            const auto& entries = obj->body->entries;
            if (i == 0) {
               if (entries.empty()) { homogeneous = false; break; }
               first_keys.reserve(entries.size());
               for (const auto& [k, _v] : entries) first_keys.push_back(k);
            } else {
               if (entries.size() != first_keys.size()) { homogeneous = false; break; }
               for (std::size_t j = 0; j < entries.size(); ++j) {
                  if (entries[j].first != first_keys[j]) {
                     homogeneous = false; break;
                  }
               }
               if (!homogeneous) break;
            }
         }
         if (homogeneous) {
            std::vector<std::vector<Value>> rows;
            rows.reserve(children.size());
            for (auto& c : children) {
               auto* obj = std::get_if<Object>(&c);
               std::vector<Value> row;
               row.reserve(first_keys.size());
               for (auto& [_k, val] : obj->body->entries) {
                  row.push_back(std::move(val));
               }
               rows.push_back(std::move(row));
            }
            return make_row_array(std::move(first_keys), std::move(rows));
         }
      }
      return make_array(std::move(children));
   }
   if (auto* obj = std::get_if<JObject>(&n.v)) {
      // §4.11 envelope detection: a sole-key object {"__pjson_ext": {...}}
      // round-trips back into an Extension.
      if (obj->size() == 1 && (*obj)[0].first == "__pjson_ext") {
         const JNode& inner = (*obj)[0].second;
         if (auto* iobj = std::get_if<JObject>(&inner.v)) {
            std::optional<std::int64_t> sub;
            std::optional<std::string>  b64;
            for (const auto& [ik, iv] : *iobj) {
               if (ik == "subtype") {
                  if (auto* p = std::get_if<std::int64_t>(&iv.v)) sub = *p;
               } else if (ik == "bytes_b64") {
                  if (auto* p = std::get_if<std::string>(&iv.v)) b64 = *p;
               }
            }
            if (sub && b64 && *sub >= 0 && *sub <= 15) {
               Extension e;
               e.subtype = static_cast<std::uint8_t>(*sub);
               e.bytes   = b64_decode(*b64);
               return e;
            }
         }
      }
      std::vector<std::pair<std::string, Value>> entries;
      entries.reserve(obj->size());
      for (const auto& [k, v] : *obj) {
         // §7.4 / J-013: a key ending in a suffix-vocabulary term tells
         // the ingress to treat the JSON string value as opaque binary
         // with the matching encoding hint. Suffix is preserved on key.
         std::optional<std::uint8_t> hint;
         if      (k.ends_with(".b64"))    hint = 0;
         else if (k.ends_with(".hex"))    hint = 1;
         else if (k.ends_with(".base58")) hint = 2;
         else if (k.ends_with(".b64u"))   hint = 3;
         if (hint.has_value()) {
            if (auto* s = std::get_if<std::string>(&v.v)) {
               try {
                  std::vector<std::uint8_t> raw;
                  switch (*hint) {
                     case 0: raw = b64_decode(*s); break;
                     case 1: raw = parse_hex(*s); break;
                     case 2: raw = base58_decode(*s); break;
                     case 3: raw = b64url_decode(*s); break;
                  }
                  Bytes b;
                  b.encoding_hint = *hint;
                  b.content = std::move(raw);
                  entries.emplace_back(k, std::move(b));
                  continue;
               } catch (...) {
                  // Decode failed — fall through to plain string mapping.
               }
            }
         }
         entries.emplace_back(k, from_json_node(v));
      }
      return make_object(std::move(entries));
   }
   throw std::runtime_error{"from_json_node: unhandled JNode variant"};
}

inline Value from_json(std::string_view text) {
   JParser p{text};
   JNode root = p.parse();
   return from_json_node(root);
}

// ── Fixture parsing ────────────────────────────────────────────────

struct Fixture {
   std::string                  id;
   std::optional<Value>         input_value;
   std::optional<std::string>   input_json;     // §4.8 / §7.1 ingress path
   std::string                  wire_hex;
   std::optional<std::string>   json_compact;
   // §7.5 emitter-option assertions.
   std::optional<std::string>   json_pretty;             // pretty=true, indent=2
   std::optional<std::string>   json_pretty_tab;         // pretty=true, indent_tab=true (EM-003)
   std::optional<std::string>   json_pretty_indent_4;    // pretty=true, indent=4
   std::optional<std::string>   json_int_string_largeonly;
   std::optional<std::string>   json_int_string_all;
   bool                         must_round_trip = false;
   bool                         must_validate   = false;
   bool                         must_reject     = false;
};

// Parse an unsigned integer-or-string into U128. Accepts decimal
// digits as either a JSON number (i64-fitting) or quoted decimal.
static U128 parse_u128(const JNode& v) {
   std::string s;
   if (auto* str = std::get_if<std::string>(&v.v)) s = *str;
   else if (auto* i = std::get_if<std::int64_t>(&v.v)) s = std::to_string(*i);
   else if (auto* d = std::get_if<JFloat>(&v.v)) {
      char buf[40];
      std::snprintf(buf, sizeof buf, "%.0f", d->f);
      s = buf;
   } else throw std::runtime_error{"json: expected unsigned integer or string"};

   if (s.empty()) throw std::runtime_error{"empty u128 string"};
   if (s[0] == '-') throw std::runtime_error{"negative value in u128 parse"};

   U128 out{};
   for (char c : s) {
      if (c < '0' || c > '9') throw std::runtime_error{"non-digit in u128"};
      // out = out * 10 + (c - '0')
      // multiply by 10:
      std::uint64_t lo_old = out.lo;
      out.lo = out.lo * 10;
      // overflow into hi: x*10 = x*8 + x*2; carry from low's top-3 + top-1 bits.
      std::uint64_t carry = (static_cast<__uint128_t>(lo_old) * 10) >> 64;
      out.hi = out.hi * 10 + carry;
      // add digit:
      std::uint64_t add = static_cast<std::uint64_t>(c - '0');
      std::uint64_t before = out.lo;
      out.lo += add;
      if (out.lo < before) ++out.hi;
   }
   return out;
}

// Parse a signed integer-or-string into I128.
static I128 parse_i128(const JNode& v) {
   std::string s;
   if (auto* str = std::get_if<std::string>(&v.v)) s = *str;
   else if (auto* i = std::get_if<std::int64_t>(&v.v)) s = std::to_string(*i);
   else throw std::runtime_error{"json: expected signed integer or string"};

   bool negative = false;
   if (!s.empty() && s[0] == '-') { negative = true; s.erase(s.begin()); }
   U128 mag = parse_u128(JNode{s});
   if (!negative) {
      if (mag.hi >> 63) throw std::runtime_error{"i128 positive overflow"};
      return I128{mag.lo, static_cast<std::int64_t>(mag.hi)};
   }
   // Two's complement of mag.
   U128 x{~mag.lo, ~mag.hi};
   x.lo += 1;
   if (x.lo == 0) x.hi += 1;
   return I128{x.lo, static_cast<std::int64_t>(x.hi)};
}

static U128 parse_hex_to_u128(std::string_view s) {
   while (s.size() >= 2 && (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")) {
      s.remove_prefix(2);
   }
   U128 out{};
   for (char c : s) {
      int d;
      if      ('0' <= c && c <= '9') d = c - '0';
      else if ('a' <= c && c <= 'f') d = 10 + c - 'a';
      else if ('A' <= c && c <= 'F') d = 10 + c - 'A';
      else throw std::runtime_error{"hex digit"};
      // out = out << 4 | d
      out.hi = (out.hi << 4) | (out.lo >> 60);
      out.lo = (out.lo << 4) | static_cast<std::uint64_t>(d);
   }
   return out;
}

static Value parse_value_from_json(const JNode& j) {
   const auto& obj = as_object(j);
   const auto* k_node = obj_get(obj, "kind");
   if (!k_node) throw std::runtime_error{"input_value missing 'kind'"};
   const auto& kind = as_string(*k_node);

   if (kind == "null") return Null{};
   if (kind == "bool") {
      const auto* v = obj_get(obj, "value");
      if (!v) throw std::runtime_error{"bool missing 'value'"};
      return Bool{as_bool(*v)};
   }
   if (kind == "uint") {
      const auto* v = obj_get(obj, "value");
      if (!v) throw std::runtime_error{"uint missing 'value'"};
      return Uint{parse_u128(*v)};
   }
   if (kind == "int") {
      const auto* v = obj_get(obj, "value");
      if (!v) throw std::runtime_error{"int missing 'value'"};
      I128 i = parse_i128(*v);
      if (!i.is_negative()) return Uint{U128{i.lo, static_cast<std::uint64_t>(i.hi)}};
      // magnitude = -i
      U128 raw{i.lo, static_cast<std::uint64_t>(i.hi)};
      U128 negated{~raw.lo, ~raw.hi};
      negated.lo += 1;
      if (negated.lo == 0) negated.hi += 1;
      return NegInt{negated};
   }
   if (kind == "float") {
      const auto* w_node = obj_get(obj, "width");
      if (!w_node) throw std::runtime_error{"float missing 'width'"};
      std::int64_t w = std::get<std::int64_t>(w_node->v);
      std::uint8_t width_log2;
      switch (w) {
         case 16:  width_log2 = 1; break;
         case 32:  width_log2 = 2; break;
         case 64:  width_log2 = 3; break;
         case 128: width_log2 = 4; break;
         default: throw std::runtime_error{"float width not in {16,32,64,128}"};
      }
      const auto* b = obj_get(obj, "bits_hex");
      if (!b) throw std::runtime_error{"float missing 'bits_hex'"};
      U128 bits = parse_hex_to_u128(as_string(*b));
      return Float{width_log2, bits};
   }
   if (kind == "decimal") {
      const auto* m = obj_get(obj, "mantissa");
      if (!m) throw std::runtime_error{"decimal missing 'mantissa'"};
      I128 mantissa = parse_i128(*m);
      const auto* sn = obj_get(obj, "scale");
      if (!sn) throw std::runtime_error{"decimal missing 'scale'"};
      std::int64_t scale = std::get<std::int64_t>(sn->v);
      return Decimal{mantissa, static_cast<std::int32_t>(scale)};
   }
   if (kind == "array") {
      const auto* children_node = obj_get(obj, "children");
      if (!children_node) throw std::runtime_error{"array missing 'children'"};
      const auto* arr = std::get_if<JArray>(&children_node->v);
      if (!arr) throw std::runtime_error{"array 'children' must be an array"};
      std::vector<Value> children;
      children.reserve(arr->size());
      for (const auto& cj : *arr) {
         children.push_back(parse_value_from_json(cj));
      }
      return make_array(std::move(children));
   }
   if (kind == "object") {
      const auto* entries_node = obj_get(obj, "entries");
      if (!entries_node) throw std::runtime_error{"object missing 'entries'"};
      const auto* arr = std::get_if<JArray>(&entries_node->v);
      if (!arr) throw std::runtime_error{"object 'entries' must be array"};
      std::vector<std::pair<std::string, Value>> entries;
      entries.reserve(arr->size());
      for (const auto& ej : *arr) {
         const auto* eobj = std::get_if<JObject>(&ej.v);
         if (!eobj) throw std::runtime_error{"object entry must be object"};
         const auto* k = obj_get(*eobj, "key");
         if (!k) throw std::runtime_error{"object entry missing 'key'"};
         const auto* v = obj_get(*eobj, "value");
         if (!v) throw std::runtime_error{"object entry missing 'value'"};
         entries.emplace_back(as_string(*k), parse_value_from_json(*v));
      }
      return make_object(std::move(entries));
   }
   if (kind == "extension") {
      const auto* sub_node = obj_get(obj, "subtype");
      if (!sub_node) throw std::runtime_error{"extension missing 'subtype'"};
      std::int64_t sub = std::get<std::int64_t>(sub_node->v);
      if (sub < 0 || sub > 15)
         throw std::runtime_error{"extension subtype must be 0..15"};
      const auto* bn = obj_get(obj, "bytes_hex");
      std::vector<std::uint8_t> body;
      if (bn) body = parse_hex(as_string(*bn));
      Extension e;
      e.subtype = static_cast<std::uint8_t>(sub);
      e.bytes   = std::move(body);
      return e;
   }
   if (kind == "numeric_string") {
      const auto* inner_node = obj_get(obj, "inner");
      if (!inner_node) throw std::runtime_error{"numeric_string missing 'inner'"};
      Value inner = parse_value_from_json(*inner_node);
      return make_numeric_string(std::move(inner));
   }
   if (kind == "bytes") {
      const auto* enc_node = obj_get(obj, "encoding");
      if (!enc_node) throw std::runtime_error{"bytes missing 'encoding'"};
      const std::string& enc = as_string(*enc_node);
      std::uint8_t hint;
      if      (enc == "base64")    hint = 0;
      else if (enc == "hex")       hint = 1;
      else if (enc == "base58")    hint = 2;
      else if (enc == "base64url") hint = 3;
      else throw std::runtime_error{std::string{"bytes encoding '"} + enc + "' not in {base64,hex,base58,base64url}"};
      const auto* bytes_node = obj_get(obj, "bytes_hex");
      if (!bytes_node) throw std::runtime_error{"bytes missing 'bytes_hex'"};
      auto raw = parse_hex(as_string(*bytes_node));
      Bytes b;
      b.encoding_hint = hint;
      b.content = std::move(raw);
      return b;
   }
   if (kind == "string") {
      const auto* enc_node = obj_get(obj, "encoding");
      if (!enc_node) throw std::runtime_error{"string missing 'encoding'"};
      const std::string& enc = as_string(*enc_node);
      std::uint8_t flag;
      if      (enc == "raw_text")    flag = 0;
      else if (enc == "escape_form") flag = 1;
      else throw std::runtime_error{std::string{"string encoding '"} + enc + "' must be raw_text or escape_form"};
      const auto* text_node = obj_get(obj, "text");
      if (!text_node) throw std::runtime_error{"string missing 'text'"};
      const std::string& text = as_string(*text_node);
      String s;
      s.encoding_flag = flag;
      s.content.assign(text.begin(), text.end());
      return s;
   }
   if (kind == "row_array") {
      const auto* keys_node = obj_get(obj, "keys");
      if (!keys_node) throw std::runtime_error{"row_array missing 'keys'"};
      const auto* keys_arr = std::get_if<JArray>(&keys_node->v);
      if (!keys_arr) throw std::runtime_error{"row_array 'keys' must be array"};
      std::vector<std::string> keys;
      keys.reserve(keys_arr->size());
      for (const auto& kj : *keys_arr) keys.push_back(as_string(kj));

      const auto* rows_node = obj_get(obj, "rows");
      if (!rows_node) throw std::runtime_error{"row_array missing 'rows'"};
      const auto* rows_arr = std::get_if<JArray>(&rows_node->v);
      if (!rows_arr) throw std::runtime_error{"row_array 'rows' must be array"};
      std::vector<std::vector<Value>> rows;
      rows.reserve(rows_arr->size());
      for (const auto& rj : *rows_arr) {
         const auto* cells = std::get_if<JArray>(&rj.v);
         if (!cells) throw std::runtime_error{"row_array row must be array"};
         std::vector<Value> row;
         row.reserve(cells->size());
         for (const auto& c : *cells) row.push_back(parse_value_from_json(c));
         rows.push_back(std::move(row));
      }
      return make_row_array(std::move(keys), std::move(rows));
   }
   if (kind == "typed_array") {
      const auto* ec_node = obj_get(obj, "element_code");
      if (!ec_node) throw std::runtime_error{"typed_array missing 'element_code'"};
      std::int64_t code64 = std::get<std::int64_t>(ec_node->v);
      if (code64 < 0 || code64 > 9)
         throw std::runtime_error{"typed_array element_code out of range"};
      auto code = static_cast<std::uint8_t>(code64);
      const std::size_t esize = typed_array_element_size(code);
      const auto* els_node = obj_get(obj, "elements");
      if (!els_node) throw std::runtime_error{"typed_array missing 'elements'"};
      const auto* els = std::get_if<JArray>(&els_node->v);
      if (!els) throw std::runtime_error{"typed_array 'elements' must be array"};
      TypedArray ta;
      ta.element_code = code;
      ta.raw.reserve(els->size() * esize);
      auto append_le = [&](std::uint64_t v, std::size_t n) {
         for (std::size_t i = 0; i < n; ++i) {
            ta.raw.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
         }
      };
      for (const auto& e : *els) {
         switch (code) {
            case 0: case 1: case 2: case 3: {
               // signed
               std::int64_t v;
               if (auto* p = std::get_if<std::int64_t>(&e.v)) v = *p;
               else if (auto* p = std::get_if<JFloat>(&e.v)) v = static_cast<std::int64_t>(p->f);
               else throw std::runtime_error{"typed_array signed element not integer"};
               append_le(static_cast<std::uint64_t>(v), esize);
               break;
            }
            case 4: case 5: case 6: case 7: {
               std::uint64_t v;
               if (auto* p = std::get_if<std::int64_t>(&e.v)) v = static_cast<std::uint64_t>(*p);
               else if (auto* p = std::get_if<JFloat>(&e.v)) v = static_cast<std::uint64_t>(p->f);
               else throw std::runtime_error{"typed_array unsigned element not integer"};
               append_le(v, esize);
               break;
            }
            case 8: {
               double d;
               if (auto* p = std::get_if<JFloat>(&e.v)) d = p->f;
               else if (auto* p = std::get_if<std::int64_t>(&e.v)) d = static_cast<double>(*p);
               else throw std::runtime_error{"typed_array f32 element not number"};
               float f = static_cast<float>(d);
               std::uint32_t bits;
               std::memcpy(&bits, &f, 4);
               append_le(bits, 4);
               break;
            }
            case 9: {
               double d;
               if (auto* p = std::get_if<JFloat>(&e.v)) d = p->f;
               else if (auto* p = std::get_if<std::int64_t>(&e.v)) d = static_cast<double>(*p);
               else throw std::runtime_error{"typed_array f64 element not number"};
               std::uint64_t bits;
               std::memcpy(&bits, &d, 8);
               append_le(bits, 8);
               break;
            }
         }
      }
      return ta;
   }
   throw std::runtime_error{std::string{"input_value kind '"} + kind +
                            "' not supported yet"};
}

static Fixture parse_fixture(std::string_view json_text) {
   JParser p{json_text};
   JNode root = p.parse();
   const auto& o = as_object(root);

   Fixture f;
   if (const auto* n = obj_get(o, "id")) f.id = as_string(*n);
   else f.id = "<unnamed>";
   if (const auto* n = obj_get(o, "wire_hex")) f.wire_hex = as_string(*n);
   else throw std::runtime_error{"fixture missing wire_hex"};
   if (const auto* n = obj_get(o, "json_compact")) f.json_compact = as_string(*n);
   if (const auto* n = obj_get(o, "must_round_trip")) f.must_round_trip = as_bool(*n);
   if (const auto* n = obj_get(o, "must_validate"))   f.must_validate   = as_bool(*n);
   if (const auto* n = obj_get(o, "must_reject"))     f.must_reject     = as_bool(*n);

   if (const auto* iv = obj_get(o, "input_value")) {
      if (!std::holds_alternative<std::nullptr_t>(iv->v)) {
         f.input_value = parse_value_from_json(*iv);
      }
   }
   if (const auto* ij = obj_get(o, "input_json")) {
      if (auto* s = std::get_if<std::string>(&ij->v)) {
         f.input_json = *s;
      }
   }
   if (const auto* n = obj_get(o, "json_pretty"))
      if (auto* s = std::get_if<std::string>(&n->v)) f.json_pretty = *s;
   if (const auto* n = obj_get(o, "json_pretty_tab"))
      if (auto* s = std::get_if<std::string>(&n->v)) f.json_pretty_tab = *s;
   if (const auto* n = obj_get(o, "json_pretty_indent_4"))
      if (auto* s = std::get_if<std::string>(&n->v)) f.json_pretty_indent_4 = *s;
   if (const auto* n = obj_get(o, "json_int_string_largeonly"))
      if (auto* s = std::get_if<std::string>(&n->v)) f.json_int_string_largeonly = *s;
   if (const auto* n = obj_get(o, "json_int_string_all"))
      if (auto* s = std::get_if<std::string>(&n->v)) f.json_int_string_all = *s;
   return f;
}

// ── --check / --xvalidate harness ──────────────────────────────────

static std::string check(const Fixture& f) {
   const auto wire = parse_hex(f.wire_hex);

   if (f.must_reject) {
      try {
         (void)decode(wire);
         return std::string{"expected reject but decoded successfully"};
      } catch (const DecodeError&) {
         return {};   // ok
      }
   }

   Value decoded;
   try {
      decoded = decode(wire);
   } catch (const std::exception& e) {
      return std::string{"decode failed: "} + e.what();
   }

   if (f.input_value.has_value()) {
      if (!(decoded == *f.input_value)) {
         return "decode mismatch: structural inequality with expected input_value";
      }
   }

   if (f.must_round_trip) {
      try {
         auto re_encoded = encode(decoded);
         if (re_encoded != wire) {
            return "round-trip mismatch: re-encode produced " + to_hex(re_encoded) +
                   " (expected " + f.wire_hex + ")";
         }
         if (f.input_value.has_value()) {
            auto e2 = encode(*f.input_value);
            if (e2 != wire) {
               return "encode-from-input mismatch: " + to_hex(e2);
            }
         }
         if (f.input_json.has_value()) {
            try {
               Value v = from_json(*f.input_json);
               auto e3 = encode(v);
               if (e3 != wire) {
                  return "encode-from-json mismatch: " + to_hex(e3) +
                         " (expected " + f.wire_hex + ")";
               }
            } catch (const std::exception& e) {
               return std::string{"from_json failed: "} + e.what();
            }
         }
      } catch (const std::exception& e) {
         return std::string{"re-encode failed: "} + e.what();
      }
   }

   if (f.json_compact.has_value()) {
      const std::string got = render_json(decoded);
      if (got != *f.json_compact) {
         return "json mismatch: expected " + *f.json_compact + ", got " + got;
      }
   }
   // §7.5 emitter-option checks.
   if (f.json_pretty.has_value()) {
      EmitOptions opts;
      opts.pretty = true;
      opts.indent = 2;
      const std::string got = render_json_with(decoded, opts);
      if (got != *f.json_pretty) {
         return "json_pretty mismatch: expected\n  " + *f.json_pretty +
                "\n  got: " + got;
      }
   }
   if (f.json_pretty_tab.has_value()) {
      EmitOptions opts;
      opts.pretty = true;
      opts.indent = 0;   // tab mode (EM-003)
      const std::string got = render_json_with(decoded, opts);
      if (got != *f.json_pretty_tab) {
         return "json_pretty_tab mismatch: expected\n  " + *f.json_pretty_tab +
                "\n  got: " + got;
      }
   }
   if (f.json_pretty_indent_4.has_value()) {
      EmitOptions opts;
      opts.pretty = true;
      opts.indent = 4;
      const std::string got = render_json_with(decoded, opts);
      if (got != *f.json_pretty_indent_4) {
         return "json_pretty_indent_4 mismatch: expected\n  " + *f.json_pretty_indent_4 +
                "\n  got: " + got;
      }
   }
   if (f.json_int_string_largeonly.has_value()) {
      EmitOptions opts;
      opts.int_string_mode = IntStringMode::LargeOnly;
      const std::string got = render_json_with(decoded, opts);
      if (got != *f.json_int_string_largeonly) {
         return "json_int_string_largeonly mismatch: expected " +
                *f.json_int_string_largeonly + ", got " + got;
      }
   }
   if (f.json_int_string_all.has_value()) {
      EmitOptions opts;
      opts.int_string_mode = IntStringMode::All;
      const std::string got = render_json_with(decoded, opts);
      if (got != *f.json_int_string_all) {
         return "json_int_string_all mismatch: expected " +
                *f.json_int_string_all + ", got " + got;
      }
   }

   return {};   // ok
}

// ── Self-test mode (T-016, T-017, V-003 — predicate properties) ───
//
// Verifies the §3 type-class predicates hold for every possible high
// nibble (0..15) and that the dispatch sub-bits within is_integer
// (codes 2..5) match the spec's stated layout (`bit 0 = sign`,
// `bit 1 = inline form`).
//
// The properties are true by construction in the C++ match dispatch;
// this mode codifies them as runtime checks so a future change that
// breaks the layout fails loud. Mirror of Rust's
// `tag_byte_predicates_match_spec_section_3`.
static int self_test() {
   int failed = 0;
   auto check = [&](bool cond, const char* msg) {
      if (!cond) { std::fprintf(stderr, "  FAIL: %s\n", msg); ++failed; }
   };

   for (std::uint8_t high = 0; high <= 15; ++high) {
      const bool is_atom              = high <= 1;
      const bool is_integer           = high >= 2 && high <= 5;
      const bool is_real              = high >= 6 && high <= 7;
      const bool is_numeric_value     = high >= 2 && high <= 7;
      const bool is_number_projectable= high >= 2 && high <= 8;
      const bool is_json_string_emit  = high >= 8 && high <= 10;
      const bool is_aggregate         = high >= 11 && high <= 12;
      const bool is_extension         = high == 13;
      const bool is_reserved          = high >= 14;

      // Mutual exclusion among the 7 disjoint top-level classes.
      const int class_count =
          (is_atom ? 1 : 0) + (is_integer ? 1 : 0) + (is_real ? 1 : 0) +
          (is_json_string_emit ? 1 : 0) + (is_aggregate ? 1 : 0) +
          (is_extension ? 1 : 0) + (is_reserved ? 1 : 0);
      char m1[64];
      std::snprintf(m1, sizeof m1, "high %u class_count==1", high);
      check(class_count == 1, m1);

      // Composite predicates derive from primitives.
      char m2[64];
      std::snprintf(m2, sizeof m2, "high %u is_numeric_value composite", high);
      check(is_numeric_value == (is_integer || is_real), m2);
      char m3[64];
      std::snprintf(m3, sizeof m3, "high %u is_number_projectable composite", high);
      check(is_number_projectable == (is_numeric_value || high == 8), m3);

      // V-003: reserved-tag fast-path predicate.
      const std::uint8_t tag = static_cast<std::uint8_t>(high << 4);
      const bool predicate = ((tag >> 4) >= 14);
      char m4[64];
      std::snprintf(m4, sizeof m4, "high %u V-003 predicate", high);
      check(predicate == is_reserved, m4);
   }

   // T-017: bit pattern within is_integer (codes 2..5).
   //   2 = 0010: unsigned, inline
   //   3 = 0011: signed,   inline
   //   4 = 0100: unsigned, full
   //   5 = 0101: signed,   full
   for (std::uint8_t high = 2; high <= 5; ++high) {
      const bool sign_bit   = (high & 0x01) != 0;
      const bool inline_bit = (high & 0x02) != 0;
      const bool expected_signed = (high == 3 || high == 5);
      const bool expected_inline = (high == 2 || high == 3);
      char m1[64];
      std::snprintf(m1, sizeof m1, "code %u sign bit", high);
      check(sign_bit == expected_signed, m1);
      char m2[64];
      std::snprintf(m2, sizeof m2, "code %u inline bit", high);
      check(inline_bit == expected_inline, m2);
   }

   // Round-trip the predicates against the driver's actual decode
   // dispatch: every reserved high nibble must throw, defined arms
   // must NOT throw a "reserved" error (they may throw truncated).
   for (std::uint8_t high = 0; high <= 15; ++high) {
      const std::uint8_t tag = static_cast<std::uint8_t>(high << 4);
      const std::uint8_t one_byte[1] = {tag};
      try {
         (void)decode({one_byte, 1});
         // Some dispatch arms succeed on a 1-byte tag (e.g. 0x00 = null,
         // 0x10 = false, etc.); that's fine for non-reserved.
         char m[64];
         std::snprintf(m, sizeof m, "high %u should not be reserved", high);
         check(high < 14, m);
      } catch (const DecodeError& e) {
         const std::string what{e.what()};
         const bool says_reserved = what.find("reserved tag 0x") != std::string::npos;
         char m[64];
         std::snprintf(m, sizeof m, "high %u reserved iff says_reserved", high);
         check(says_reserved == (high >= 14), m);
      }
   }

   if (failed == 0) {
      std::printf("self-test: PASSED\n");
      return 0;
   }
   std::fprintf(stderr, "self-test: %d FAILED\n", failed);
   return 1;
}

static std::string xvalidate(const Fixture& f) {
   const auto wire = parse_hex(f.wire_hex);
   if (f.must_reject) {
      try {
         (void)decode(wire);
         return "reject:<DID-NOT-REJECT>";
      } catch (const DecodeError& e) {
         return std::string{"reject:"} + e.what();
      }
   }
   const auto v = decode(wire);
   const auto re_wire = encode(v);
   const auto json = render_json(v);
   return "wire:" + to_hex(re_wire) + " json:" + json;
}

}   // namespace pjson_conformance

// ── main ───────────────────────────────────────────────────────────

int main(int argc, char** argv) {
   if (argc < 2) {
      std::fprintf(stderr, "usage: %s --check|--xvalidate|--self-test < fixture.json\n",
                   argv[0]);
      return 2;
   }
   const std::string mode{argv[1]};
   if (mode == "--self-test") {
      return pjson_conformance::self_test();
   }
   if (mode != "--check" && mode != "--xvalidate" && mode != "--emit-wire") {
      std::fprintf(stderr, "usage: %s --check|--xvalidate|--emit-wire < fixture.json\n",
                   argv[0]);
      return 2;
   }

   // Slurp stdin.
   std::string input{
       std::istreambuf_iterator<char>(std::cin),
       std::istreambuf_iterator<char>()};

   pjson_conformance::Fixture f;
   try {
      f = pjson_conformance::parse_fixture(input);
   } catch (const std::exception& e) {
      std::fprintf(stderr, "fixture parse: %s\n", e.what());
      return 2;
   }

   if (mode == "--emit-wire") {
      // Authoring helper: encode the fixture's input_value (or input_json
      // if input_value is absent) and print the wire bytes as hex.
      try {
         pjson_conformance::Value v;
         if (f.input_value.has_value()) v = *f.input_value;
         else if (f.input_json.has_value())
            v = pjson_conformance::from_json(*f.input_json);
         else {
            std::fprintf(stderr, "--emit-wire: fixture has no input_value or input_json\n");
            return 2;
         }
         auto wire = pjson_conformance::encode(v);
         std::printf("%s\n", pjson_conformance::to_hex(wire).c_str());
         return 0;
      } catch (const std::exception& e) {
         std::fprintf(stderr, "FAIL [%s]: %s\n", f.id.c_str(), e.what());
         return 1;
      }
   }
   if (mode == "--check") {
      const std::string err = pjson_conformance::check(f);
      if (err.empty()) return 0;
      std::fprintf(stderr, "FAIL [%s]: %s\n", f.id.c_str(), err.c_str());
      return 1;
   } else {
      try {
         std::printf("%s\n", pjson_conformance::xvalidate(f).c_str());
         return 0;
      } catch (const std::exception& e) {
         std::fprintf(stderr, "FAIL [%s]: %s\n", f.id.c_str(), e.what());
         return 1;
      }
   }
}

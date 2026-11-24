#pragma once
// The record types the FlatBuffers tests share. They mirror
// tests/flatbuf/interop.fbs member for member (flatbuf_interop_tests.cpp
// exchanges buffers of them with the official library).
//
// At global scope: an annotation specializes a psio template, which C++
// only allows from an enclosing namespace.

#include <psio/annotate.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

enum class Color : std::uint8_t
{
   red,
   green,
   blue
};

struct Vec3
{
   float       x = 0, y = 0, z = 0;
   friend bool operator==(const Vec3&, const Vec3&) = default;
};
PSIO_REFLECT(Vec3, definitionWillNotChange(), x, y, z)

struct Mixed
{
   std::uint8_t                a = 0;
   std::uint64_t               b = 0;
   std::uint16_t               c = 0;
   std::array<std::int32_t, 3> d{};
   Vec3                        v;
   bool                        e = false;
   friend bool                 operator==(const Mixed&, const Mixed&) = default;
};
PSIO_REFLECT(Mixed, definitionWillNotChange(), a, b, c, d, v, e)

struct Point
{
   std::int32_t x = 0, y = 0;
   friend bool  operator==(const Point&, const Point&) = default;
};
PSIO_REFLECT(Point, x, y)

struct Profile
{
   std::uint64_t id = 0;
   std::string   name;
   std::string   email;
   std::uint32_t age      = 0;
   bool          verified = false;
   friend bool   operator==(const Profile&, const Profile&) = default;
};
PSIO_REFLECT(Profile, id, name, email, age, verified)

struct Item
{
   std::string   product;
   std::uint32_t qty        = 1;
   double        unit_price = 0;
   friend bool   operator==(const Item&, const Item&) = default;
};
PSIO_REFLECT(Item, product, qty, unit_price)

struct Card
{
   std::string   number;
   std::uint16_t cvv = 0;
   friend bool   operator==(const Card&, const Card&) = default;
};
PSIO_REFLECT(Card, number, cvv)

struct Wire
{
   std::string iban;
   friend bool operator==(const Wire&, const Wire&) = default;
};
PSIO_REFLECT(Wire, iban)

using Payment = std::variant<std::monostate, Card, Wire>;
using Refund  = std::variant<Card, Wire>;

struct Order
{
   std::uint64_t                        id = 0;
   Profile                              customer;
   std::vector<Item>                    items;
   double                               total = 0;
   std::optional<std::string>           note;
   Payment                              payment;
   Vec3                                 position;
   std::optional<Vec3>                  origin;
   std::vector<Vec3>                    path;
   std::vector<std::string>             tags;
   Color                                color = Color::green;
   std::optional<std::int32_t>          discount;
   std::set<std::string>                labels;
   std::map<std::string, std::uint32_t> stock;
   std::map<std::uint32_t, Item>        by_id;
   std::vector<std::uint8_t>            blob;
   std::vector<bool>                    flags;
   std::array<std::uint16_t, 4>         quad{};
   std::vector<Color>                   colors;
   std::set<std::uint32_t>              ids;
   Refund                               refund;
   Mixed                                mixed;
   friend bool                          operator==(const Order&, const Order&) = default;
};
PSIO_REFLECT(Order, id, customer, items, total, note, payment, position, origin, path, tags, color,
             discount, labels, stock, by_id, blob, flags, quad, colors, ids, refund, mixed)

struct Tree
{
   std::int32_t      value = 0;
   std::string       name;
   std::vector<Tree> children;
   friend bool       operator==(const Tree&, const Tree&) = default;
};
PSIO_REFLECT(Tree, value, name, children)

struct Scalars
{
   bool          b   = false;
   std::int8_t   i8  = 0;
   std::uint8_t  u8  = 0;
   std::int16_t  i16 = 0;
   std::uint16_t u16 = 0;
   std::int32_t  i32 = 0;
   std::uint32_t u32 = 0;
   std::int64_t  i64 = 0;
   std::uint64_t u64 = 0;
   float         f   = 0;
   double        d   = 0;
   Color         c   = Color::red;
   char          ch  = 0;
   friend bool   operator==(const Scalars&, const Scalars&) = default;
};
PSIO_REFLECT(Scalars, b, i8, u8, i16, u16, i32, u32, i64, u64, f, d, c, ch)

struct Defaults
{
   std::int16_t level = 7;
   bool         on    = true;
   Color        c     = Color::green;
   double       ratio = 0.5;
   friend bool  operator==(const Defaults&, const Defaults&) = default;
};
PSIO_REFLECT(Defaults, level, on, c, ratio)

struct Renumbered
{
   std::int32_t c = 0;
   std::string  a;
   std::int64_t b = 0;
   friend bool  operator==(const Renumbered&, const Renumbered&) = default;
};
PSIO_REFLECT(Renumbered, attr(c, field<3>), attr(a, field<1>), attr(b, field<2>))

struct WithDeprecated
{
   std::int32_t a   = 0;
   std::int32_t old = 0;
   std::int32_t c   = 0;
   friend bool  operator==(const WithDeprecated&, const WithDeprecated&) = default;
};
PSIO_REFLECT(WithDeprecated, a, attr(old, deprecated_spec{.version = "2"}), c)

struct SparseRow
{
   std::int32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
   friend bool  operator==(const SparseRow&, const SparseRow&) = default;
};
PSIO_REFLECT(SparseRow, a, b, c, d, e, f)

struct Sparse
{
   std::vector<SparseRow> rows;
   friend bool            operator==(const Sparse&, const Sparse&) = default;
};
PSIO_REFLECT(Sparse, rows)

// Shapes declared here only (not in tests/flatbuf/interop.fbs);
// conformance/flatbuf-vectors.json is read by C++ and Rust through these
// and the ones above.
struct Numbers
{
   bool          b   = false;
   std::int8_t   i8  = 0;
   std::uint8_t  u8  = 0;
   std::int16_t  i16 = 0;
   std::uint16_t u16 = 0;
   std::int32_t  i32 = 0;
   std::uint32_t u32 = 0;
   std::int64_t  i64 = 0;
   std::uint64_t u64 = 0;
   float         f32 = 0;
   double        f64 = 0;
   friend bool   operator==(const Numbers&, const Numbers&) = default;
};
PSIO_REFLECT(Numbers, b, i8, u8, i16, u16, i32, u32, i64, u64, f32, f64)

struct Entry
{
   std::string   name;
   std::uint32_t count = 0;
   double        price = 0;
   friend bool   operator==(const Entry&, const Entry&) = default;
};
PSIO_REFLECT(Entry, name, count, price)

struct Basket
{
   std::uint64_t              id = 0;
   Profile                    owner;
   std::vector<Entry>         entries;
   std::vector<std::string>   tags;
   std::vector<std::uint16_t> codes;
   std::vector<std::uint8_t>  bytes;
   double                     total = 0;
   bool                       paid  = false;
   friend bool                operator==(const Basket&, const Basket&) = default;
};
PSIO_REFLECT(Basket, id, owner, entries, tags, codes, bytes, total, paid)

// A vector of 8-byte elements (alignment of the elements, not only of the
// length).
struct Longs
{
   std::vector<std::uint64_t> values;
   std::vector<double>        reals;
   friend bool                operator==(const Longs&, const Longs&) = default;
};
PSIO_REFLECT(Longs, values, reals)

inline Profile sample_profile()
{
   return Profile{42, "Alice Example", "alice@example.com", 37, true};
}

inline Order sample_order()
{
   Order o;
   o.id       = 1001;
   o.customer = sample_profile();
   o.items    = {{"widget", 3, 9.99}, {"gadget", 1, 24.5}, {"", 2, 0.0}};
   o.total    = 54.47;
   o.note     = "leave at the door";
   o.payment  = Card{"4111", 123};
   o.position = {1.0f, 2.0f, 3.0f};
   o.origin   = Vec3{-1.0f, -2.0f, -3.0f};
   o.path     = {{0, 0, 0}, {1, 1.5f, 2}, {4, 5, 6}};
   o.tags     = {"a", "", "longer tag"};
   o.color    = Color::blue;
   o.discount = -15;
   o.labels   = {"gift", "rush"};
   o.stock    = {{"bolt", 17}, {"nut", 0}, {"washer", 250}};
   o.by_id    = {{0, {"zero", 1, 0}}, {7, {"seven", 7, 7.5}}, {4000000000u, {"big", 0, 1}}};
   o.blob     = {0, 1, 2, 253, 254, 255, 7};
   o.flags    = {true, false, true};
   o.quad     = {1, 2, 3, 65535};
   o.colors   = {Color::red, Color::blue};
   o.ids      = {3, 40, 500};
   o.refund   = Card{"refund", 1};
   o.mixed    = Mixed{200, 0x0102030405060708ull, 65000, {-1, 2, -3}, {0.5f, 1.5f, 2.5f}, true};
   return o;
}

// A small Order that still has one of everything: the base of the mutated
// rows of conformance/flatbuf-vectors.json.
inline Order compact_order()
{
   Order o;
   o.id       = 7;
   o.customer = Profile{1, "Al", "", 30, true};
   o.items    = {{"w", 3, 1.5}};
   o.note     = "n";
   o.payment  = Card{"41", 9};
   o.origin   = Vec3{1.0f, 0.0f, -1.0f};
   o.path     = {{1, 2, 3}};
   o.tags     = {"t", ""};
   o.color    = Color::blue;
   o.discount = 5;
   o.labels   = {"a", "b"};
   o.stock    = {{"k", 1}, {"m", 2}};
   o.by_id    = {{1, {"one", 1, 0}}, {9, {"nine", 2, 0.5}}};
   o.blob     = {1, 2, 3};
   o.flags    = {true, false};
   o.quad     = {1, 2, 3, 4};
   o.colors   = {Color::green};
   o.ids      = {1, 2};
   o.refund   = Wire{"FR"};
   o.mixed    = Mixed{1, 2, 3, {4, 5, 6}, {7, 8, 9}, true};
   return o;
}

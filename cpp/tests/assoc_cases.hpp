// std::pair, maps and sets as values: the types shared by assoc_tests.cpp
// and the exception-disabled run (checked_noexceptions.cpp), and the cases
// that must report through codec_status — never a throw, never an abort.
//
// run_assoc_cases() returns 0, or the number of the first case that did not
// report what it must.

#pragma once

#include <psio/annotate.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace assoc_cases
{
   using Bytes = std::vector<char>;

   // A record key: ordered by its key bytes (x, then y), not by any `<`.
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      friend bool  operator==(const Point&, const Point&) = default;
      // Deliberately NOT the key order: the codec must not rely on it.
      friend bool operator<(const Point& a, const Point& b)
      {
         return std::tie(a.y, a.x) < std::tie(b.y, b.x);
      }
   };
   PSIO_REFLECT(Point, x, y)

   // A key type with a custom text form: "L<id>". On the wire a record of
   // one u32; its key is the u32.
   struct Label
   {
      std::uint32_t id = 0;
      friend bool   operator==(const Label&, const Label&) = default;
      friend bool   operator<(const Label& a, const Label& b) { return a.id < b.id; }
   };
   PSIO_REFLECT(Label, id)
   struct LabelText
   {
      static bool parse(std::span<const char> in, Label& out) noexcept
      {
         if (in.size() < 2 || in.size() > 10 || in[0] != 'L' || (in[1] == '0' && in.size() > 2))
            return false;
         std::uint64_t v = 0;
         for (std::size_t i = 1; i < in.size(); ++i)
         {
            if (in[i] < '0' || in[i] > '9')
               return false;
            v = v * 10 + static_cast<std::uint64_t>(in[i] - '0');
         }
         if (v > 0xffffffffu)
            return false;
         out.id = static_cast<std::uint32_t>(v);
         return true;
      }
      static std::size_t packsize(const Label& v)
      {
         std::string s;
         encode(v, s);
         return s.size();
      }
      static void  encode(const Label& v, std::string& s) { s += 'L' + std::to_string(v.id); }
      static Label decode(std::span<const char> in)
      {
         Label v;
         (void)parse(in, v);
         return v;
      }
      static psio::codec_status validate(std::span<const char> in) noexcept
      {
         Label v;
         return parse(in, v) ? psio::codec_ok()
                             : psio::codec_fail("Label: expected L<number>", 0, "Label");
      }
      static psio::codec_status validate_strict(std::span<const char> in) noexcept
      {
         return validate(in);
      }
   };
}  // namespace assoc_cases
PSIO_ADAPTER(assoc_cases::Label, psio::text_category, assoc_cases::LabelText)

namespace assoc_cases
{
   // Orders doubles by their bits: +0 and -0 are distinct elements here,
   // but have one key.
   struct BitLess
   {
      bool operator()(double a, double b) const noexcept
      {
         std::uint64_t x, y;
         std::memcpy(&x, &a, 8);
         std::memcpy(&y, &b, 8);
         return x < y;
      }
   };
   // Merges keys the encoding distinguishes (same parity).
   struct ParityLess
   {
      bool operator()(std::uint32_t a, std::uint32_t b) const noexcept { return a % 2 < b % 2; }
   };
   struct Descending
   {
      bool operator()(const std::string& a, const std::string& b) const noexcept { return a > b; }
   };

   // Every container, key kind and position.
   struct Book
   {
      std::map<std::string, std::uint32_t>                               counts;
      std::unordered_map<std::uint32_t, std::string>                     names;
      std::set<std::int16_t>                                             ids;
      std::unordered_set<std::string>                                    tags;
      std::map<std::pair<std::uint8_t, std::string>, std::vector<Point>> index;
      std::map<Label, std::set<std::uint8_t>>                            labelled;
      std::pair<std::uint16_t, std::string>                              head;
      std::optional<std::map<std::int64_t, bool>>                        maybe;
      std::vector<std::set<double>>                                      series;
      std::map<std::uint8_t, std::uint8_t>                               limited;  // max 2
      std::map<Point, std::string>                                       points;
      friend bool operator==(const Book&, const Book&) = default;
   };
   PSIO_REFLECT(Book, counts, names, ids, tags, index, labelled, head, maybe, series,
                attr(limited, max<2>), points)

   // The same value spelled with the lists the maps and sets are.
   struct BookLists
   {
      std::vector<std::tuple<std::string, std::uint32_t>> counts;
      std::vector<std::tuple<std::uint32_t, std::string>> names;
      std::vector<std::int16_t>                           ids;
      std::vector<std::string>                            tags;
      std::vector<std::tuple<std::tuple<std::uint8_t, std::string>, std::vector<Point>>> index;
      std::vector<std::tuple<Label, std::vector<std::uint8_t>>>                          labelled;
      std::tuple<std::uint16_t, std::string>                                             head;
      std::optional<std::vector<std::tuple<std::int64_t, bool>>>                         maybe;
      std::vector<std::vector<double>>                                                   series;
      std::vector<std::tuple<std::uint8_t, std::uint8_t>>                                limited;
      std::vector<std::tuple<Point, std::string>>                                        points;
      friend bool operator==(const BookLists&, const BookLists&) = default;
   };
   PSIO_REFLECT(BookLists, counts, names, ids, tags, index, labelled, head, maybe, series,
                attr(limited, max<2>), points)

   inline Book sample()
   {
      Book b;
      b.counts   = {{"pear", 3}, {"apple", 1}, {"", 9}, {"apple pie", 2}};
      b.names    = {{70000, "big"}, {1, "one"}, {256, "two-five-six"}, {255, "ff"}};
      b.ids      = {300, -1, 0, -300, 7};
      b.tags     = {"z", "a", "mm"};
      b.index    = {{{2, "b"}, {{1, 2}}}, {{1, "z"}, {}}, {{1, "a"}, {{-1, -2}, {3, 4}}}};
      b.labelled = {{Label{10}, {3, 0, 255}}, {Label{9}, {}}};
      b.head     = {7, "head"};
      b.maybe    = std::map<std::int64_t, bool>{{5, true}, {-5, false}, {0, true}};
      b.series   = {{1.5, -2.0, 0.0}, {}, {-0.0}};
      b.limited  = {{9, 1}, {2, 2}};
      b.points   = {{Point{1, -1}, "se"}, {Point{-1, 1}, "nw"}, {Point{1, 1}, "ne"}};
      return b;
   }
   // sample() as lists, in canonical order.
   inline BookLists sample_lists()
   {
      BookLists l;
      l.counts   = {{"", 9}, {"apple", 1}, {"apple pie", 2}, {"pear", 3}};
      l.names    = {{1, "one"}, {255, "ff"}, {256, "two-five-six"}, {70000, "big"}};
      l.ids      = {-300, -1, 0, 7, 300};
      l.tags     = {"a", "mm", "z"};
      l.index    = {{{1, "a"}, {{-1, -2}, {3, 4}}}, {{1, "z"}, {}}, {{2, "b"}, {{1, 2}}}};
      l.labelled = {{Label{9}, {}}, {Label{10}, {0, 3, 255}}};
      l.head     = {7, "head"};
      l.maybe    = std::vector<std::tuple<std::int64_t, bool>>{{-5, false}, {0, true}, {5, true}};
      l.series   = {{-2.0, 0.0, 1.5}, {}, {-0.0}};
      l.limited  = {{2, 2}, {9, 1}};
      l.points   = {{Point{-1, 1}, "nw"}, {Point{1, -1}, "se"}, {Point{1, 1}, "ne"}};
      return l;
   }

   inline bool says(const psio::codec_status& status, std::string_view what)
   {
      return !status.ok() && status.error().what.find(what) != std::string_view::npos;
   }

   // Every checking pssz reader refuses `frame` as a T, saying `what`.
   template <typename T>
   bool pssz_rejects(const Bytes& frame, std::string_view what)
   {
      const std::span<const char> in{frame};
      bool                        unknown = false;
      T                           out{};
      return says(psio::validate<T>(psio::pssz{}, in), what) &&
             says(psio::validate_strict<T>(psio::pssz{}, in), what) &&
             says(psio::validate_pssz_compatible<T>(psio::pssz{}, in, unknown), what) &&
             says(psio::decode_pssz_checked(psio::pssz{}, in, out, unknown), what);
   }
   template <typename T>
   bool pssz_accepts(const Bytes& frame, const T& value)
   {
      const std::span<const char> in{frame};
      bool                        unknown = false;
      T                           out{};
      return psio::validate<T>(psio::pssz{}, in).ok() &&
             psio::validate_pssz_compatible<T>(psio::pssz{}, in, unknown).ok() && !unknown &&
             psio::decode_pssz_checked(psio::pssz{}, in, out, unknown).ok() && out == value;
   }
   template <typename T>
   bool text_rejects(std::string_view text, std::string_view what)
   {
      const std::span<const char> in{text};
      T                           out{};
      for (const auto parser : {psio::json_parser::text, psio::json_parser::simdjson,
                                psio::json_parser::simdjson_index})
         if (!says(psio::decode_json_checked(psio::json{}, in, out, parser), what) ||
             !says(psio::decode_json_checked(psio::ijson{}, in, out, parser), what) ||
             !says(psio::validate_json<T>(psio::json{}, in, parser), what) ||
             !says(psio::validate_json<T>(psio::ijson{}, in, parser), what))
            return false;
      return true;
   }

   // Checked readers on every truncation and on single-byte corruptions of
   // `frame`: any answer, and whatever is accepted is a value its own
   // encoder writes and its own reader reads back. Returns false otherwise.
   template <typename T>
   bool survives_garbage(const Bytes& frame)
   {
      bool canonical = true;
      auto feed      = [&](const Bytes& bytes)
      {
         const std::span<const char> in{bytes};
         T                           out{};
         bool                        unknown = false;
         if (psio::decode_pssz_checked(psio::pssz{}, in, out, unknown).ok())
         {
            Bytes again;
            T     back{};
            canonical &= psio::encode_pssz_checked(psio::pssz{}, out, again).ok() &&
                         psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{again},
                                                   back, unknown)
                            .ok() &&
                         back == out;
         }
         (void)psio::validate<T>(psio::pssz{}, in).ok();
         (void)psio::validate_strict<T>(psio::pssz{}, in).ok();
         (void)psio::validate<T>(psio::pssz_<2>{}, in).ok();
         if (psio::validate<T>(psio::key{}, in).ok())
            canonical &= psio::encode(psio::key{}, psio::decode<T>(psio::key{}, in)) == bytes;
         T text_out{};
         (void)psio::decode_json_checked(psio::json{}, in, text_out).ok();
         (void)psio::decode_json_checked(psio::ijson{}, in, text_out, psio::json_parser::text).ok();
      };
      for (std::size_t n = 0; n <= frame.size(); ++n)
         feed(Bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(n)));
      for (std::size_t i = 0; i < frame.size(); ++i)
         for (unsigned char flip : {0x01, 0x02, 0x80, 0xff})
         {
            Bytes bad = frame;
            bad[i]    = static_cast<char>(static_cast<unsigned char>(bad[i]) ^ flip);
            feed(bad);
         }
      return canonical;
   }

   inline int run_assoc_cases()
   {
      namespace ir = psio::pssz_ir;
      const Book      book  = sample();
      const BookLists lists = sample_lists();

      // ── pssz: the encoding is the sorted list's ───────────────────────
      Bytes frame;
      if (!psio::encode_pssz_checked(psio::pssz{}, book, frame).ok()) return 1;
      Bytes list_frame;
      if (!psio::encode_pssz_checked(psio::pssz{}, lists, list_frame).ok()) return 2;
      if (frame != list_frame) return 3;
      if (!pssz_accepts(frame, book)) return 4;
      {
         // validate_strict works on maps, sets and pairs with exceptions
         // disabled too (it needs no materialisation for them). Book itself
         // has a member length_bound, which a guest build does not check
         // strictly.
         using Plain = std::map<std::string, std::pair<std::set<std::int32_t>, bool>>;
         const Plain plain{{"b", {{2, -2}, true}}, {"a", {{}, false}}};
         const Bytes bytes = psio::encode(psio::pssz{}, plain);
         if (!psio::validate_strict<Plain>(psio::pssz{}, std::span<const char>{bytes}).ok())
            return 60;
         if (!pssz_accepts(bytes, plain)) return 61;
#if defined(__cpp_exceptions) && PSIO_EXCEPTIONS_ENABLED
         if (!psio::validate_strict<Book>(psio::pssz{}, std::span<const char>{frame}).ok())
            return 62;
#endif
      }

      // ── pssz: order and uniqueness are checked by every reader ────────
      {
         BookLists bad = lists;
         std::swap(bad.counts[1], bad.counts[2]);
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "map entries are not in strictly"))
            return 5;
         bad = lists;
         bad.ids.push_back(300);  // a duplicate of the last element
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "set elements are not in strictly"))
            return 6;
         bad = lists;
         std::swap(bad.names[1], bad.names[2]);  // 256 before 255: sorted as LE bytes
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "map entries")) return 7;
         bad = lists;
         std::swap(bad.points[0], bad.points[1]);  // a record key
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "map entries")) return 8;
         bad = lists;
         std::swap(std::get<1>(bad.labelled[1])[0], std::get<1>(bad.labelled[1])[1]);  // nested set
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "set elements")) return 9;
         bad        = lists;
         bad.series = {{0.0, -0.0}};  // one key twice
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "set elements")) return 10;
         bad = lists;
         std::swap((*bad.maybe)[0], (*bad.maybe)[2]);
         if (!pssz_rejects<Book>(psio::encode(psio::pssz{}, bad), "map entries")) return 11;
         bad = lists;
         bad.limited.emplace_back(10, 0);  // three entries, bound 2
         if (!pssz_rejects<Book>(psio::detail::pssz_impl::encode_ignoring_length_bounds(
                                    psio::pssz{}, bad),
                                 "length_bound"))
            return 12;
      }

      // ── pssz: a value with two equal keys has no encoding ─────────────
      {
         const std::set<double, BitLess> zeros{0.0, -0.0};
         Bytes                           out{'k'};
         if (!says(psio::encode_pssz_checked(psio::pssz{}, zeros, out), "duplicate elements") ||
             out != Bytes{'k'})
            return 13;
         std::string text = "k";
         if (!says(psio::encode_json_checked(psio::json{}, zeros, text), "duplicate elements") ||
             text != "k")
            return 14;
         Book over    = book;
         over.limited = {{1, 1}, {2, 2}, {3, 3}};
         if (!says(psio::encode_pssz_checked(psio::pssz{}, over, out), "length_bound")) return 15;
         if (!says(psio::encode_json_checked(psio::ijson{}, over, text), "length_bound")) return 16;
      }

      // ── key ────────────────────────────────────────────────────────────
      {
         const Bytes key = psio::encode(psio::key{}, book.index);
         if (key != psio::encode(psio::key{}, lists.index)) return 20;
         if (!psio::validate<std::remove_cvref_t<decltype(book.index)>>(psio::key{}, std::span<const char>{key}).ok())
            return 21;
         auto bad = lists.index;
         std::swap(bad[0], bad[1]);
         const Bytes unsorted = psio::encode(psio::key{}, bad);
         if (!says(psio::validate<std::remove_cvref_t<decltype(book.index)>>(psio::key{}, std::span<const char>{unsorted}),
                   "map entries are not in strictly"))
            return 22;
         const Bytes octets = psio::encode(psio::key{}, std::vector<std::uint8_t>{3, 3});
         if (!says(psio::validate<std::set<std::uint8_t>>(psio::key{}, std::span<const char>{octets}),
                   "set elements are not in strictly"))
            return 23;
      }

      // ── json / ijson ───────────────────────────────────────────────────
      const std::string json  = psio::encode(psio::json{}, book);
      const std::string ijson = psio::encode(psio::ijson{}, book);
      {
         Book back;
         if (!psio::decode_json_checked(psio::json{}, std::span<const char>{json}, back).ok() ||
             !(back == book))
            return 30;
         if (!psio::decode_json_checked(psio::ijson{}, std::span<const char>{ijson}, back).ok() ||
             !(back == book))
            return 31;
         using Counts = std::map<std::string, std::uint32_t>;
         if (!text_rejects<Counts>(R"({"a":1,"b":2,"a":3})", "duplicate map key")) return 32;
         if (!text_rejects<std::set<std::int16_t>>("[1,2,1]", "duplicate set element")) return 33;
         if (!text_rejects<std::map<std::uint32_t, bool>>("[[1,true],[1,false]]",
                                                          "duplicate map key"))
            return 34;
         if (!text_rejects<std::set<double>>("[0.0,-0.0]", "duplicate set element")) return 35;
         {
            // Only a decode has a container that can merge keys.
            const std::string_view                     two = "[[1,true],[3,false]]";
            std::map<std::uint32_t, bool, ParityLess> merging;
            if (!says(psio::decode_json_checked(psio::json{}, std::span<const char>{two}, merging),
                      "merged distinct keys"))
               return 36;
            if (!psio::validate_json<std::map<std::uint32_t, bool, ParityLess>>(
                    psio::json{}, std::span<const char>{two})
                    .ok())
               return 36;
         }
         if (!text_rejects<std::map<Label, int>>(R"({"L1":1,"x":2})", "Label")) return 37;
         if (!text_rejects<Counts>(R"([["a",1]])", "")) return 38;
         if (!text_rejects<std::map<std::uint32_t, bool>>(R"({"1":true})", "")) return 39;
      }

      // ── schema-driven ──────────────────────────────────────────────────
      {
         psio::SchemaBuilder builder;
         builder.insert<Book>("Book");
         const psio::Schema                schema = std::move(builder).build();
         const psio::schema_types::AnyType root{psio::schema_types::Type{"Book"}};
         ir::CustomHandlers                handlers;
         handlers.add<Label>();
         if (!ir::validate_pssz(schema, root, std::span<const char>{frame}).ok()) return 40;
         std::string text;
         if (!ir::pssz_to_json(schema, root, psio::ijson{}, std::span<const char>{frame}, text,
                               nullptr, &handlers)
                 .ok() ||
             text != ijson)
            return 41;
         Bytes back;
         if (!ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{json}, back,
                               nullptr, psio::json_parser::automatic, &handlers)
                 .ok() ||
             back != frame)
            return 42;
         BookLists bad = lists;
         std::swap(bad.counts[0], bad.counts[3]);
         const Bytes unsorted = psio::encode(psio::pssz{}, bad);
         if (!says(ir::validate_pssz(schema, root, std::span<const char>{unsorted}),
                   "map entries are not in strictly"))
            return 43;
         text.clear();
         if (ir::pssz_to_json(schema, root, psio::json{}, std::span<const char>{unsorted}, text)
                .ok())
            return 44;
         std::string dup = json;
         const auto  at  = dup.find("\"apple\":1");
         if (at == std::string::npos) return 45;
         dup.replace(at, 9, "\"pear\":1");
         back.clear();
         if (!says(ir::json_to_pssz(schema, root, psio::json{}, std::span<const char>{dup}, back,
                                    nullptr, psio::json_parser::automatic, &handlers),
                   "duplicate map key"))
            return 46;
         ir::Value value;
         bool      unknown = false;
         if (!ir::decode_auto(schema, root, std::span<const char>{frame}, value, unknown).ok())
            return 47;
         Bytes key;
         if (!ir::to_key(schema, root, value, key).ok() || key != psio::encode(psio::key{}, book))
            return 48;
      }

      // ── hostile input ──────────────────────────────────────────────────
      if (!survives_garbage<Book>(frame)) return 50;
      if (!survives_garbage<Book>(psio::encode(psio::key{}, book))) return 51;
      if (!survives_garbage<Book>(Bytes(json.begin(), json.end()))) return 52;
      if (!survives_garbage<Book>(Bytes(ijson.begin(), ijson.end()))) return 53;
      return 0;
   }
}  // namespace assoc_cases

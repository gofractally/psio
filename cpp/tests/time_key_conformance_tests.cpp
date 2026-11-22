// Consumes conformance/time-key-vectors.json, the time / key fixture shared
// with cargo test (rust/psio/tests/time_key_conformance.rs) and the JS tests
// (js/src/test/time-key-conformance.test.ts). Every row is checked against
// the typed entry points of pssz, key, json and ijson.

#include "time_key_conformance_types.hpp"

#include <psio/pssz_ir_json.hpp>
#include <psio/schema_ir.hpp>

#include <catch.hpp>

#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

using namespace tk_conf;

namespace
{
   const Fixture& fixture()
   {
      static const Fixture fx = [] {
         std::ifstream     in(PSIO_TIME_KEY_VECTORS, std::ios::binary);
         std::stringstream buf;
         buf << in.rdbuf();
         const std::string text = buf.str();
         Fixture           out;
         auto st = psio::decode_json_checked(psio::json{}, std::span<const char>{text}, out);
         if (!st.ok())
            FAIL("cannot read " PSIO_TIME_KEY_VECTORS ": " << st.error().what);
         return out;
      }();
      return fx;
   }

   template <typename T>
   std::optional<T> from_pssz(const std::optional<std::string>& hex)
   {
      if (!hex)
         return std::nullopt;
      const auto bytes = from_hex(*hex);
      if (!bytes || !psio::validate<T>(psio::pssz{}, std::span<const char>{*bytes}).ok())
         return std::nullopt;
      return psio::decode<T>(psio::pssz{}, std::span<const char>{*bytes});
   }

   template <typename Fmt, typename T>
   std::optional<T> from_text(const std::string& text)
   {
      T out{};
      if (!psio::decode_json_checked(Fmt{}, std::span<const char>{text}, out).ok())
         return std::nullopt;
      if (!psio::validate<T>(Fmt{}, std::span<const char>{text}).ok())
         return std::nullopt;
      return out;
   }

   // "" when the row holds; otherwise what failed.
   template <typename T>
   std::string check_row(const Row& row)
   {
      if (row.kind == "value")
      {
         const auto v = from_pssz<T>(row.pssz);
         if (!v) return "pssz does not decode";
         if (to_hex(psio::encode(psio::pssz{}, *v)) != *row.pssz) return "pssz re-encode";
         const auto key = psio::encode(psio::key{}, *v);
         if (to_hex(key) != row.key) return "key bytes";
         if (psio::size_of(psio::key{}, *v) != key.size()) return "key size";
         if (!psio::validate<T>(psio::key{}, std::span<const char>{key}).ok()) return "key validate";
         if (!(psio::decode<T>(psio::key{}, std::span<const char>{key}) == *v))
         {
            // The one lossy case: -0.0 decodes as +0.0.
            if (psio::encode(psio::key{}, psio::decode<T>(psio::key{}, std::span<const char>{key})) != key)
               return "key decode";
         }
         std::string json, ijson;
         const bool  has_json  = psio::encode_json_checked(psio::json{}, *v, json).ok();
         const bool  has_ijson = psio::encode_json_checked(psio::ijson{}, *v, ijson).ok();
         if (has_json != row.json.has_value() || has_ijson != row.ijson.has_value())
            return "text availability";
         if (has_json && (json != *row.json || ijson != *row.ijson)) return "text";
         if (has_json && (!(from_text<psio::json, T>(json) == v) ||
                          !(from_text<psio::ijson, T>(ijson) == v)))
            return "text decode";
         return "";
      }
      if (row.kind == "text-accept")
      {
         const auto want = from_pssz<T>(row.pssz);
         if (!want) return "pssz does not decode";
         if (!(from_text<psio::json, T>(*row.text) == want)) return "json read";
         if (!(from_text<psio::ijson, T>(*row.text) == want)) return "ijson read";
         if (psio::encode(psio::json{}, *want) != *row.json) return "canonical text";
         return "";
      }
      if (row.kind == "text-reject")
      {
         if (from_text<psio::json, T>(*row.text) || from_text<psio::ijson, T>(*row.text))
            return "accepted";
         return "";
      }
      if (row.kind == "key-reject")
      {
         const auto bytes = from_hex(*row.key);
         if (!bytes) return "hex";
         if (psio::validate<T>(psio::key{}, std::span<const char>{*bytes}).ok()) return "accepted";
         bool threw = false;
         try
         {
            (void)psio::decode<T>(psio::key{}, std::span<const char>{*bytes});
         }
         catch (const psio::codec_exception&)
         {
            threw = true;
         }
         return threw ? "" : "decode did not fail";
      }
      if (row.kind == "key-order")
      {
         std::vector<char> last;
         std::optional<T>  last_value;
         for (std::size_t i = 0; i < row.values->size(); ++i)
         {
            const auto v = from_pssz<T>((*row.values)[i]);
            if (!v) return "pssz does not decode";
            auto key = psio::encode(psio::key{}, *v);
            if (i && !bytes_less(last, key)) return "keys not ascending at " + std::to_string(i);
            // (A type without a value order, such as one holding a nested
            // value, has key-order-bits rows instead.)
            if constexpr (requires(const T& a, const T& b) { a < b; })
            {
               if (i && !(*last_value < *v))
                  return "values not ascending at " + std::to_string(i);
            }
            else
               return "key-order row for a type without a value order";
            last       = std::move(key);
            last_value = v;
         }
         return "";
      }
      if (row.kind == "key-order-bits")
      {
         // Key order where `<` has no answer (NaNs): the keys ascend, and
         // each key gives the exact bit pattern back.
         std::vector<char> last;
         for (std::size_t i = 0; i < row.values->size(); ++i)
         {
            const auto v = from_pssz<T>((*row.values)[i]);
            if (!v) return "pssz does not decode";
            if (to_hex(psio::encode(psio::pssz{}, *v)) != (*row.values)[i]) return "pssz bits";
            auto key = psio::encode(psio::key{}, *v);
            if (i && !bytes_less(last, key)) return "keys not ascending at " + std::to_string(i);
            if (!psio::validate<T>(psio::key{}, std::span<const char>{key}).ok())
               return "key validate";
            if (to_hex(psio::encode(psio::pssz{},
                                    psio::decode<T>(psio::key{}, std::span<const char>{key}))) !=
                (*row.values)[i])
               return "key does not give the bits back";
            last = std::move(key);
         }
         return "";
      }
      if (row.kind == "key-prefix")
      {
         if constexpr (is_composite_v<T>)
         {
            const auto v = from_pssz<T>(row.pssz);
            if (!v) return "pssz does not decode";
            const auto p = prefix_of(*v, *row.count);
            if (to_hex(p) != *row.prefix) return "prefix bytes";
            const auto b = psio::key_prefix_upper_bound(p);
            if (b.has_value() != row.bound.has_value() || (b && to_hex(*b) != *row.bound))
               return "bound";
            const auto full = psio::encode(psio::key{}, *v);
            if (p.size() > full.size() || !std::equal(p.begin(), p.end(), full.begin()))
               return "not a prefix of the full key";
            return "";
         }
         else
            return "key-prefix row for a non-composite type";
      }
      return "unknown kind " + row.kind;
   }

   std::string check_bound(const Row& row)
   {
      const auto p = from_hex(*row.prefix);
      if (!p) return "hex";
      const auto b = psio::key_prefix_upper_bound(*p);
      if (b.has_value() != row.bound.has_value() || (b && to_hex(*b) != *row.bound)) return "bound";
      return "";
   }

   using Dispatch = std::map<std::string, std::function<std::string(const Row&)>>;
   const Dispatch& dispatch()
   {
      static const Dispatch table = [] {
         Dispatch out;
         for_each_type([&]<typename T>(std::type_identity<T>) {
            out.emplace(type_id<T>(), [](const Row& row) { return check_row<T>(row); });
         });
         out.emplace("bytes", check_bound);
         return out;
      }();
      return table;
   }
}  // namespace

TEST_CASE("time/key conformance: every vector holds", "[time][key][conformance]")
{
   const auto& fx = fixture();
   REQUIRE(fx.version == 1);
   REQUIRE(fx.vectors.size() >= 500);
   std::set<std::string>              names;
   std::map<std::string, std::size_t> kinds;
   std::set<std::string>              types;
   for (const auto& row : fx.vectors)
   {
      INFO(row.name);
      CHECK(names.insert(row.name).second);
      const auto it = dispatch().find(row.type);
      REQUIRE(it != dispatch().end());
      CHECK(it->second(row) == "");
      ++kinds[row.kind];
      types.insert(row.type);
   }
   for (const char* kind :
        {"value", "text-accept", "text-reject", "key-reject", "key-order", "key-order-bits",
         "key-prefix", "bound"})
   {
      INFO(kind);
      CHECK(kinds[kind] > 0);
   }
   CHECK(kinds.size() == 8);
   // Every declared type has rows, and the fixture lists exactly our types.
   std::size_t declared = 0;
   for_each_type([&]<typename T>(std::type_identity<T>) {
      INFO(type_id<T>());
      CHECK(types.count(type_id<T>()) == 1);
      ++declared;
   });
   CHECK(fx.types.size() == declared);
}

// The same rows through the schema-driven C++ codecs (pssz_ir.hpp /
// pssz_ir_json.hpp), using nothing but the Schema IR recorded in the fixture
// — what a reader in another language has to work from.
TEST_CASE("time/key conformance: the Schema IR drives pssz and text", "[time][schema][conformance]")
{
   namespace ir   = psio::pssz_ir;
   const auto& fx = fixture();
   const auto  ir_bytes = from_hex(fx.schema_ir);
   REQUIRE(ir_bytes.has_value());
   REQUIRE(psio::validate<psio::Schema>(psio::pssz{}, std::span<const char>{*ir_bytes}).ok());
   const auto schema = psio::decode<psio::Schema>(psio::pssz{}, std::span<const char>{*ir_bytes});

   std::size_t values = 0, accepted = 0, rejected = 0;
   for (const auto& row : fx.vectors)
   {
      INFO(row.name);
      const psio::schema_types::AnyType root{psio::schema_types::Type{row.type}};
      auto to_text = [&](auto fmt, const std::vector<char>& bytes, std::string& out) {
         return ir::pssz_to_json(schema, root, fmt, std::span<const char>{bytes}, out).ok();
      };
      auto to_pssz = [&](auto fmt, const std::string& text, std::vector<char>& out) {
         return ir::json_to_pssz(schema, root, fmt, std::span<const char>{text}, out).ok();
      };
      if (row.kind == "value")
      {
         const auto bytes = *from_hex(*row.pssz);
         CHECK(ir::validate_pssz(schema, root, std::span<const char>{bytes}).ok());
         // The schema-driven key encoder writes the typed key.
         {
            ir::Value         dynamic;
            bool              unknown = false;
            std::vector<char> key;
            CHECK(ir::decode_auto(schema, root, std::span<const char>{bytes}, dynamic, unknown)
                     .ok());
            CHECK(ir::to_key(schema, root, dynamic, key).ok());
            CHECK(to_hex(key) == *row.key);
         }
         std::string json, ijson;
         CHECK(to_text(psio::json{}, bytes, json) == row.json.has_value());
         CHECK(to_text(psio::ijson{}, bytes, ijson) == row.ijson.has_value());
         if (row.json)
         {
            CHECK(json == *row.json);
            CHECK(ijson == *row.ijson);
            std::vector<char> back;
            CHECK(to_pssz(psio::json{}, json, back));
            CHECK(to_hex(back) == *row.pssz);
            back.clear();
            CHECK(to_pssz(psio::ijson{}, ijson, back));
            CHECK(to_hex(back) == *row.pssz);
         }
         ++values;
      }
      else if (row.kind == "text-accept")
      {
         std::vector<char> a, b;
         CHECK(to_pssz(psio::json{}, *row.text, a));
         CHECK(to_pssz(psio::ijson{}, *row.text, b));
         CHECK(to_hex(a) == *row.pssz);
         CHECK(to_hex(b) == *row.pssz);
         ++accepted;
      }
      else if (row.kind == "text-reject")
      {
         std::vector<char> out;
         CHECK_FALSE(to_pssz(psio::json{}, *row.text, out));
         CHECK_FALSE(to_pssz(psio::ijson{}, *row.text, out));
         ++rejected;
      }
   }
   CHECK(values > 150);
   CHECK(accepted > 30);
   CHECK(rejected > 60);
}

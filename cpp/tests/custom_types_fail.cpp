// Must not compile: misuse of transparent(), invariant(...) / PSIO_INVARIANT
// and of a custom text form on a type with no name. Each CT_FAIL_* macro
// selects one case (cpp/tests/CMakeLists.txt); without any the file compiles
// (control).

#include <psio/graphql.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace app
{
   inline psio::codec_status ok(const auto&) noexcept { return psio::codec_ok(); }

   // ── transparent() ──────────────────────────────────────────────────────
   struct Wrap
   {
      std::uint64_t a = 0;
#if defined(CT_FAIL_TRANSPARENT_TWO_MEMBERS)
      std::uint64_t b = 0;
      PSIO_REFLECT(Wrap, transparent(), a, b)
#elif defined(CT_FAIL_TRANSPARENT_NO_MEMBER)
      PSIO_REFLECT(Wrap, transparent())
#elif defined(CT_FAIL_TRANSPARENT_DWNC)
      PSIO_REFLECT(Wrap, transparent(), definitionWillNotChange(), a)
#elif defined(CT_FAIL_TRANSPARENT_MAX_FIELDS)
      PSIO_REFLECT(Wrap, transparent(), maxFields(1), a)
#elif defined(CT_FAIL_TRANSPARENT_MAX_DYNAMIC_DATA)
      PSIO_REFLECT(Wrap, transparent(), maxDynamicData(64), a)
#elif defined(CT_FAIL_TRANSPARENT_MEMBER_ATTR)
      PSIO_REFLECT(Wrap, transparent(), attr(a, json_string))
#elif defined(CT_FAIL_TRANSPARENT_TWICE)
      PSIO_REFLECT(Wrap, transparent(), transparent(), a)
#else
      PSIO_REFLECT(Wrap, transparent(), a)
#endif
   };
#if defined(CT_FAIL_TRANSPARENT_FIELD_ATTRS)
   PSIO_FIELD_ATTRS(Wrap, a, psio::json_string)
#elif defined(CT_FAIL_TRANSPARENT_AND_TYPE_ATTRS)
   PSIO_TYPE_ATTRS(Wrap, psio::transparent)
#elif defined(CT_FAIL_TRANSPARENT_TYPE_ATTRS_DWNC)
   PSIO_TYPE_ATTRS(Wrap, psio::definition_will_not_change{})
#endif
   static_assert(psio::is_transparent_v<Wrap>);

   struct Bytes
   {
      std::vector<std::uint8_t> data;
      PSIO_REFLECT(Bytes, transparent(), data)
   };

#if defined(CT_FAIL_TRANSPARENT_OPTIONAL)
   struct Maybe
   {
      std::optional<std::uint32_t> value;
      PSIO_REFLECT(Maybe, transparent(), value)
   };
   static_assert(psio::is_transparent_v<Maybe>);
#endif

#if defined(CT_FAIL_TRANSPARENT_LENGTH_BOUND)
   // A length_bound belongs to a string or a list member, not to a member
   // of a named type: bound the type.
   struct Holder
   {
      Bytes key;
      PSIO_REFLECT(Holder, attr(key, max<8>))
   };
   inline const auto use_holder = psio::encode(psio::pssz{}, Holder{});
#endif

#if defined(CT_FAIL_TRANSPARENT_GRAPHQL_RECORD)
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      PSIO_REFLECT(Point, x, y)
   };
   struct Position
   {
      Point at;
      PSIO_REFLECT(Position, transparent(), at)
   };
   struct Query
   {
      Position where;
      PSIO_REFLECT(Query, where)
   };
   inline const auto use_graphql = psio::graphql::schema<Query>::sdl();
#endif

   // ── invariant(...) ─────────────────────────────────────────────────────
   struct Checked
   {
      std::uint32_t n = 0;
      static psio::codec_status even(const Checked& c) noexcept
      {
         return c.n % 2 == 0 ? psio::codec_ok() : psio::codec_fail("odd", 0, "Checked");
      }
      static psio::codec_status throwing(const Checked&) { return psio::codec_ok(); }
      static bool               boolean(const Checked&) noexcept { return true; }
      static psio::codec_status other(const std::string&) noexcept { return psio::codec_ok(); }
#if defined(CT_FAIL_INVARIANT_TWICE)
      PSIO_REFLECT(Checked, n, invariant(even), invariant(even))
#elif defined(CT_FAIL_INVARIANT_NOT_NOEXCEPT)
      PSIO_REFLECT(Checked, n, invariant(throwing))
#elif defined(CT_FAIL_INVARIANT_RETURN_TYPE)
      PSIO_REFLECT(Checked, n, invariant(boolean))
#elif defined(CT_FAIL_INVARIANT_ARGUMENT)
      PSIO_REFLECT(Checked, n, invariant(other))
#else
      PSIO_REFLECT(Checked, n, invariant(even))
#endif
   };
   inline psio::codec_status free_check(const Checked&) noexcept { return psio::codec_ok(); }
   inline psio::codec_status free_throwing(const std::variant<int, std::string>&)
   {
      return psio::codec_ok();
   }
   using Alias = std::variant<int, std::string>;
   using Number = std::uint64_t;
}  // namespace app

#if defined(CT_FAIL_INVARIANT_REFLECT_AND_MACRO)
PSIO_INVARIANT(app::Checked, app::free_check)
#elif defined(CT_FAIL_INVARIANT_MACRO_TWICE)
PSIO_INVARIANT(app::Alias, app::ok)
PSIO_INVARIANT(app::Alias, app::ok)
#elif defined(CT_FAIL_INVARIANT_MACRO_NOT_NOEXCEPT)
PSIO_INVARIANT(app::Alias, app::free_throwing)
#elif defined(CT_FAIL_INVARIANT_ARITHMETIC)
PSIO_INVARIANT(app::Number, app::ok)
#elif defined(CT_FAIL_INVARIANT_NO_SCHEMA_ID)
// A variant has no name of its own: the schema needs one.
PSIO_INVARIANT(app::Alias, app::ok)
inline const auto use_schema = psio::SchemaBuilder{}.insert<app::Alias>("Alias").build();
#elif defined(CT_FAIL_INVARIANT_AFTER_USE)
static_assert(!psio::has_invariant_v<app::Alias>);
PSIO_INVARIANT(app::Alias, app::ok)
#endif
static_assert(psio::has_invariant_v<app::Checked>);
inline const auto use_checked = psio::check_invariant(app::Checked{}).ok();

// ── a custom text form on a type with no name ─────────────────────────────
namespace app
{
   using Either = std::variant<std::uint8_t, std::uint16_t>;
   struct EitherText
   {
#if !defined(CT_FAIL_TEXT_NO_SCHEMA_ID)
      static constexpr std::string_view schema_id = "Either";
#endif
      static std::size_t        packsize(const Either&) { return 1; }
      static void               encode(const Either&, std::string& s) { s += "e"; }
      static Either             decode(std::span<const char>) { return {}; }
      static psio::codec_status validate(std::span<const char>) noexcept { return psio::codec_ok(); }
      static psio::codec_status validate_strict(std::span<const char>) noexcept
      {
         return psio::codec_ok();
      }
   };
}  // namespace app
PSIO_ADAPTER(app::Either, psio::text_category, app::EitherText)
inline const auto use_either = psio::SchemaBuilder{}.insert<app::Either>("Either").build();

int main() { return 0; }

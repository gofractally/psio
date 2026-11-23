#pragma once
//
// psio/pssz_ir_compare.hpp — comparing two types of two Schema IRs at run
// time: are they the same type (`equivalent`), and may one replace the other
// (`evolves_from`)?
//
//    namespace ir = psio::pssz_ir;
//    if (auto r = ir::evolves_from(installed, "Row", proposed, "Row"); !r)
//       abort(r.path + ": " + r.reason);          // ".tags[]: the length bound ..."
//    if (!ir::equivalent(installed, keyTypeA, proposed, keyTypeB, ir::compare::wire)) ...
//    ir::evolves_from(installed, installedInterface, proposed, proposedInterface);
//    ir::evolves_from(installed, proposed);       // every interface, by name
//
// `evolves_from(old, Old, new, New)` is the run-time twin of the compile-time
// `psio::pssz_evolves_from<Old, New>` (pssz.hpp) and applies the rules of
// docs/pssz-spec.md 7.5 / 7.6 / 16 to two types that are only known as
// schemas: New's reader reads EVERY value Old's writer can produce, with the
// same bytes; Old's reader reads the values of New that are also values of
// Old (it reports appended fields as unknown). `equivalent` is the symmetric
// relation: the two types have the same values and the same bytes, and no
// revision of one is permitted that is not permitted of the other.
//
// Nothing here throws; the result is a value. Both work in a build without
// exceptions (a wasm guest).
//
// ── What is compared ─────────────────────────────────────────────────────
//
// The two types are compared as RESOLVED types, node by node, in the
// automatic width profile (psio::pssz). A named alias (`Type{"Row"}`, an
// `@N` interning name) is followed and its name is not compared: the order
// of `Schema::types`, the names of the types and how a schema shares
// subtrees are not part of a type. Recursive types are compared
// co-inductively.
//
// compare::wire — the bytes, and which bytes are values:
//
//   * the kind of every node: integer (bits and signedness), float (size),
//     bool, string, bytes, bit vector / bit list, list, array (length),
//     optional (and whether it has a selector byte), variant, tuple, record,
//     map, set, nested value. A lone `char` (`Custom{u8, "char"}`) IS the
//     8-bit unsigned integer, and the element of a byte list is not looked
//     at: `Custom{List<Custom{u8, "char"}>, "hex"}` of older schemas and
//     `Custom{List<u8>, "hex"}` are one type (docs/spec-evolution-notes.md
//     E-011). A plain `Tuple` and an extensible `Object` are the same record
//     on the wire; a `Struct` is a `definitionWillNotChange` `Object`;
//   * raw strings ("string"), UTF-8 strings ("utf8") and bytes ("hex") are
//     three different types although the bytes of a value may be equal;
//   * length bounds, as the range [min, max] a position admits, wherever it
//     is written: in the type (`BoundedList`, `Array`, a bit list's bound)
//     or on the member (`lengthBound`). `BoundedList<T, 8>` and a `List<T>`
//     member with `lengthBound = "max=8"` are the same;
//   * every offset width: of each record and tuple, and of each list, array,
//     map and set whose elements are not fixed-size, derived as the codec
//     derives it (the explicit `maxDynamicData`, else the inferred bound,
//     under the cap of the enclosing record);
//   * `definitionWillNotChange`, `maxDynamicData`, `maxFields`;
//   * whether a value is boxed (`Custom{.., "box"}`: it changes the widths
//     of what holds it);
//   * the key type of a map / element type of a set, which must be
//     equivalent in both relations (another key type is another order).
//
// compare::contract (the default) — also what a consumer of the TEXT forms
// and of the type's identity sees (docs/pssz-spec.md 7.6: "Rename a field
// ... consumers using reflected names will break"):
//
//   * the names of record members and of variant alternatives, and an
//     alternative's `ijsonName`;
//   * custom ids (`Custom{.., id}` whose id is not part of the binary form:
//     a time presentation, a type with a text adapter or an invariant): the
//     same ids in the same order on both sides;
//   * enums: an `Enum` on both sides or on neither; `evolves_from` lets New
//     ADD enumerators (every old name keeps its value), `equivalent` wants
//     the same set;
//   * the text shape of a record: an object, a named argument list
//     (`tuple = "named"`), or positional (`Tuple`, `tuple = "positional"`);
//   * the member attributes `jsonInt` / `ijsonInt`, and whether a variant is
//     `untagged`.
//
// Never compared: `doc` / `description` / `visible` attributes, package,
// worlds and uses, and — because the Schema IR does not record them — the
// member annotations `sorted` / `unique` / `unique_keys`, `as<Tag>`
// overrides and binary adapters. For types that differ only in those the
// compile-time check is the stricter one. A transparent type is a named
// alias in the IR, so its name is not compared either (the compile-time
// check does compare it).
//
// ── evolves_from: the rules (docs/pssz-spec.md 7.5 / 7.6) ────────────────
//
//   * a record's (tuple's, argument list's) members are Old's, in order,
//     followed only by optional members; a `definitionWillNotChange` record
//     gains none and keeps that status;
//   * no offset width moves, at any depth (an appended field, a widened
//     bound or a larger new alternative can move one: reserve room with
//     `maxDynamicData`);
//   * a length bound may only widen: New's [min, max] contains Old's;
//   * `maxDynamicData`: New's cap admits everything Old's writer can
//     produce — at least Old's cap, or, when Old has none, at least Old's
//     inferred bound. `maxFields`: New has no more fields than Old's cap
//     and neither raises nor drops it;
//   * a variant keeps its alternatives in order and may gain new ones at
//     the end;
//   * a nested value's inner type evolves by these rules as a root of its
//     own; the payload of an optional, the element of a list / array and
//     the mapped value of a map evolve by them in place.
//
// ── Functions, interfaces, whole schemas ─────────────────────────────────
//
//   Func       the argument list — the record `argument_list(func)`
//              (pssz_ir.hpp) of its parameters with the function's
//              `maxFields` / `maxDynamicData` — and the result (present on
//              both sides or on neither; a `result.lengthBound` attribute
//              is its bound). `evolves_from`: both evolve Old -> New, so a
//              new callee accepts every call an old caller makes, and a
//              stored result (a trace) of the old function is a value of
//              the new result type. An old caller reads a new result under
//              the usual one-directional rule. The two functions' names are
//              not compared: the caller chose the pair.
//   Interface  functions are matched by NAME, in any order: every function
//              of Old exists in New and evolves (`equivalent`: the same set
//              of names). Likewise every name in `type_names`.
//   Schema     interfaces are matched by name: every interface of Old
//              exists in New and evolves (`equivalent`: the same set).
//
// ── The result ───────────────────────────────────────────────────────────
//
// `compatibility{ok, code, reason, path}`. `code` is a stable identifier of
// the rule (the table below; shared with the other languages through
// conformance/evolution-vectors.json), `reason` a sentence for a person,
// `path` where from the root: `.field`, `[]` (an element; a map entry's key
// and value are `[].0` / `[].1`), `.2` (a tuple element), `<Alt>` (an
// alternative), `?` (an optional's payload), `(nested)`, and for functions
// `name(args)` / `name(result)`. The root itself is `(root)`.
//
//   malformed-schema        a schema is not well-formed (path: which side)
//   type-changed            another kind of node
//   boxed-changed           boxed on one side only
//   custom-id-changed       contract: custom ids differ
//   enum-changed            contract: enum on one side, or its enumerators
//   integer-changed  float-changed  fixed-size-changed  bit-count-changed
//   array-length-changed    utf8-changed     optional-form-changed
//   bound-tightened         evolves_from: a length bound does not contain
//                           the old one
//   bound-changed           equivalent: the length bounds differ
//   width-changed           an offset width differs
//   key-type-changed        the key of a map / element of a set
//   alternatives-removed    alternative-count-changed (equivalent)
//   alternative-renamed     contract
//   dwnc-changed            fields-removed   field-count-changed (equivalent)
//   dwnc-gained-fields      field-renamed (contract)
//   appended-field-not-optional
//   max-dynamic-data        max-fields
//   text-form-changed       contract: record text shape, jsonInt/ijsonInt,
//                           ijsonName
//   result-changed          a result on one side only
//   function-removed  function-added  type-removed  type-added
//   interface-removed interface-added

#include <psio/pssz_ir.hpp>

#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace psio::pssz_ir
{
// What `equivalent` / `evolves_from` compare (see the header comment).
enum class compare : std::uint8_t
{
   wire,      // bytes, and which bytes are values
   contract,  // wire, and names / custom ids / enums / text forms
};

// The outcome of a comparison: compatible, or the first rule broken.
struct compatibility
{
   bool ok = true;
   std::string_view code{};  // static storage; empty when ok
   std::string reason{};
   std::string path{};
   explicit operator bool() const noexcept { return ok; }
   // The same as a codec_status: ok, or "path: reason" (owned text).
   codec_status status() const
   {
      if (ok)
         return codec_ok();
      return codec_fail_owned(path + ": " + reason, 0, "pssz-ir");
   }
};

namespace detail
{
class Comparer
{
   using AnyType = schema_types::AnyType;
   using Schema = schema_types::Schema;
   const Schema &schema_a, &schema_b;
   Codec<0> codec_a, codec_b;
   const bool names;
   compatibility out;
   std::string path;
   // Synthesized argument lists: alive (and at one address) for the walk.
   std::deque<AnyType> made;
   std::size_t nesting = 0;

   struct Pos
   {
      std::optional<std::uint64_t> cap;  // the cap of the nearest enclosing record
      std::optional<LengthBound> bound;  // of the member holding the position
   };
   static bool same(const std::optional<LengthBound> &a, const std::optional<LengthBound> &b)
   {
      if (!a || !b)
         return !a && !b;
      return a->exact == b->exact && a->max == b->max && a->min == b->min;
   }
   struct Seen
   {
      const AnyType *a, *b;
      Pos pa, pb;
      bool exact;
   };
   std::vector<Seen> seen;

   enum class Kind
   {
      integer,
      floating,
      boolean,
      string,
      bytes,
      bitvector,
      bitlist,
      map,
      set,
      option,
      list,
      array,
      variant,
      record,
      nested,
      unsupported,
   };
   static const char *kind_name(Kind kind)
   {
      switch (kind)
      {
      case Kind::integer: return "integer";
      case Kind::floating: return "float";
      case Kind::boolean: return "bool";
      case Kind::string: return "string";
      case Kind::bytes: return "bytes";
      case Kind::bitvector: return "bit vector";
      case Kind::bitlist: return "bit list";
      case Kind::map: return "map";
      case Kind::set: return "set";
      case Kind::option: return "optional";
      case Kind::list: return "list";
      case Kind::array: return "array";
      case Kind::variant: return "variant";
      case Kind::record: return "record";
      case Kind::nested: return "nested value";
      default: return "unsupported type";
      }
   }
   // A type with the nodes that add nothing to its bytes taken off.
   struct Peeled
   {
      const AnyType *wire = nullptr;
      bool boxed = false;
      std::vector<std::string_view> ids;
      const schema_types::Enum *enumeration = nullptr;
      Kind kind = Kind::unsupported;
   };

   struct PathScope
   {
      std::string &path;
      std::size_t size;
      PathScope(std::string &path, std::string_view step) : path(path), size(path.size())
      {
         path += step;
      }
      ~PathScope() { path.resize(size); }
   };

   bool fail(std::string_view code, std::string reason)
   {
      if (out.ok)
      {
         out.ok = false;
         out.code = code;
         out.reason = std::move(reason);
         out.path = path.empty() ? std::string{"(root)"} : path;
      }
      return false;
   }
   bool malformed(bool first, const codec_status &status)
   {
      return fail("malformed-schema", std::string{first ? "the first schema: " : "the second schema: "} +
                                          std::string{status.error().what});
   }
   bool good(bool first, const codec_status &status)
   {
      return status.ok() ? true : malformed(first, status);
   }
   static std::string number(std::optional<std::uint64_t> n)
   {
      return n ? std::to_string(*n) : std::string{"unbounded"};
   }

   static std::optional<std::string_view> attribute_value(
       const std::vector<schema_types::Attribute> &values, std::string_view name)
   {
      if (auto found = find_attribute(values, name); found && found->value)
         return std::string_view{*found->value};
      return std::nullopt;
   }

   bool peel(bool first, const AnyType &input, Peeled &p)
   {
      const Schema &schema = first ? schema_a : schema_b;
      const Codec<0> &codec = first ? codec_a : codec_b;
      const AnyType *at = &input;
      for (std::size_t hops = 0; hops <= 4 * schema.types.size() + 64; ++hops)
      {
         if (auto named = std::get_if<schema_types::Enum>(&at->value))
         {
            if (!good(first, codec.enum_underlying(*named)))
               return false;
            if (!p.enumeration)
               p.enumeration = named;
            at = named->type.get();
            continue;
         }
         if (auto custom = std::get_if<schema_types::Custom>(&at->value);
             custom && !wire_custom_id(custom->id))
         {
            if (!custom->type.get())
               return fail("malformed-schema", "null schema box");
            if (custom->id == "box")
               p.boxed = true;
            else
               p.ids.push_back(custom->id);
            at = custom->type.get();
            continue;
         }
         if (std::holds_alternative<schema_types::Type>(at->value))
         {
            const AnyType *next;
            if (!good(first, codec.alias(*at, next)))
               return false;
            at = next;
            continue;
         }
         p.wire = at;
         p.kind = kind_of(*at);
         return true;
      }
      return fail("malformed-schema", "cyclic named aliases");
   }
   static Kind kind_of(const AnyType &type)
   {
      return std::visit(
          [](const auto &t) -> Kind
          {
             using T = std::decay_t<decltype(t)>;
             if constexpr (std::is_same_v<T, schema_types::Int>)
                return Kind::integer;
             else if constexpr (std::is_same_v<T, schema_types::Float>)
                return Kind::floating;
             else if constexpr (std::is_same_v<T, schema_types::Option>)
                return Kind::option;
             else if constexpr (std::is_same_v<T, schema_types::List> ||
                                std::is_same_v<T, schema_types::BoundedList>)
                return Kind::list;
             else if constexpr (std::is_same_v<T, schema_types::Array>)
                return Kind::array;
             else if constexpr (std::is_same_v<T, schema_types::Variant>)
                return Kind::variant;
             else if constexpr (std::is_same_v<T, schema_types::Tuple> ||
                                std::is_same_v<T, schema_types::Object> ||
                                std::is_same_v<T, schema_types::Struct>)
                return Kind::record;
             else if constexpr (std::is_same_v<T, schema_types::Nested>)
                return Kind::nested;
             else if constexpr (std::is_same_v<T, schema_types::Custom>)
             {
                if (t.id == "bool")
                   return Kind::boolean;
                if (t.id == "char")
                   return Kind::integer;
                if (text_adapter_id(t.id))
                   return Kind::string;
                if (t.id == "hex")
                   return Kind::bytes;
                if (t.id == "bitvector")
                   return Kind::bitvector;
                if (t.id == "bitlist")
                   return Kind::bitlist;
                if (t.id == "map")
                   return Kind::map;
                if (t.id == "set")
                   return Kind::set;
                return Kind::unsupported;
             }
             else
                return Kind::unsupported;
          },
          type.value);
   }

   // The lengths a position admits: the bound of its container type (a
   // List has none, a BoundedList a maximum, an Array one length) and of
   // the member that holds it.
   struct Range
   {
      std::uint64_t min = 0;
      std::optional<std::uint64_t> max;
   };
   static Range range_of(const AnyType *container, const std::optional<LengthBound> &bound)
   {
      Range r;
      if (container)
      {
         if (auto list = std::get_if<schema_types::BoundedList>(&container->value))
            r.max = list->maxCount;
         else if (auto array = std::get_if<schema_types::Array>(&container->value))
            r.min = *(r.max = array->len);
      }
      if (bound)
      {
         const auto lower = bound->exact ? bound->exact : bound->min;
         const auto upper = bound->exact ? bound->exact : bound->max;
         if (lower && *lower > r.min)
            r.min = *lower;
         if (upper && (!r.max || *upper < *r.max))
            r.max = upper;
      }
      return r;
   }
   static std::string range_text(const Range &r)
   {
      return "[" + std::to_string(r.min) + ", " + number(r.max) + "]";
   }
   bool ranges(const Range &was, const Range &is, bool exact, const char *what)
   {
      if (exact)
      {
         if (was.min != is.min || was.max != is.max)
            return fail("bound-changed", std::string{"the length bound of "} + what + " differs (" +
                                             range_text(was) + " / " + range_text(is) + ")");
         return true;
      }
      if (is.min > was.min || (is.max && (!was.max || *is.max < *was.max)))
         return fail("bound-tightened", std::string{"the length bound of "} + what +
                                            " was tightened (" + range_text(was) + " -> " +
                                            range_text(is) + ")");
      return true;
   }
   bool width(bool first, const AnyType &type, const Pos &pos, std::size_t &w)
   {
      Codec<0> &codec = first ? codec_a : codec_b;
      codec.active_cap = pos.cap;
      auto status = codec.width_of(type, w, pos.bound ? pos.bound->count() : std::nullopt);
      codec.active_cap.reset();
      return good(first, status);
   }
   bool widths(const AnyType &a, const AnyType &b, const Pos &pa, const Pos &pb, const char *what)
   {
      std::size_t was, is;
      if (!width(true, a, pa, was) || !width(false, b, pb, is))
         return false;
      if (was != is)
         return fail("width-changed", std::string{"the offset width of "} + what + " differs (" +
                                          std::to_string(was) + " / " + std::to_string(is) + ")");
      return true;
   }
   // The container a string / bytes adapter sits on.
   bool container_of(bool first, const schema_types::Custom &custom, const AnyType *&container)
   {
      const Codec<0> &codec = first ? codec_a : codec_b;
      return good(first, codec.direct(custom.type, container));
   }

   // ── Records ───────────────────────────────────────────────────────────
   enum class Text
   {
      object,
      named_arguments,
      positional,
   };
   struct RecordShape
   {
      std::vector<const schema_types::Box<AnyType> *> types;
      std::vector<const schema_types::Member *> members;  // empty for a Tuple
      const std::vector<schema_types::Attribute> *attributes = nullptr;
      bool stable = false;
      bool tuple = false;  // a Tuple node: no cap scope of its own
      Text text = Text::object;
   };
   static RecordShape record_shape(const AnyType &type)
   {
      RecordShape s;
      if (auto tuple = std::get_if<schema_types::Tuple>(&type.value))
      {
         s.tuple = true;
         s.text = Text::positional;
         for (const auto &member : tuple->members)
            s.types.push_back(&member);
         return s;
      }
      auto members = [&](const auto &record)
      {
         s.attributes = &record.attributes;
         for (const auto &member : record.members)
         {
            s.types.push_back(&member.type);
            s.members.push_back(&member);
         }
      };
      if (auto fixed = std::get_if<schema_types::Struct>(&type.value))
      {
         members(*fixed);
         s.stable = true;
      }
      else if (auto object = std::get_if<schema_types::Object>(&type.value))
      {
         members(*object);
         s.stable = dwnc(object->attributes);
         if (auto form = attribute_value(object->attributes, "tuple"))
            s.text = *form == "named" ? Text::named_arguments : Text::positional;
      }
      return s;
   }
   std::string member_step(const RecordShape &shape, std::size_t i) const
   {
      if (!shape.members.empty() && shape.text != Text::positional)
         return "." + shape.members[i]->name;
      return "." + std::to_string(i);
   }

   bool record(const Peeled &a, const Peeled &b, const Pos &pa, const Pos &pb, bool exact)
   {
      const RecordShape sa = record_shape(*a.wire), sb = record_shape(*b.wire);
      if (sa.stable != sb.stable)
         return fail("dwnc-changed", "definitionWillNotChange on one side only");
      if (names && sa.text != sb.text)
         return fail("text-form-changed",
                     "the text shape of the record differs (an object, a named argument list or "
                     "positional)");
      const std::size_t count = sa.types.size();
      if (exact && sb.types.size() != count)
         return fail("field-count-changed", "the number of fields differs (" +
                                                std::to_string(count) + " / " +
                                                std::to_string(sb.types.size()) + ")");
      if (sb.types.size() < count)
         return fail("fields-removed", "fields were removed (" + std::to_string(count) + " -> " +
                                           std::to_string(sb.types.size()) + ")");
      if (sa.stable && sb.types.size() != count)
         return fail("dwnc-gained-fields", "a definitionWillNotChange record gained fields");
      // A record's width is its own wherever it appears; a tuple's is also
      // bounded by the cap around it.
      if (!sa.stable && !widths(*a.wire, *b.wire, pa, pb, "the record"))
         return false;
      // The members lie beneath the record's own cap, or beneath none; a
      // Tuple node has none of its own and passes on the one around it.
      Pos below_a{pa.cap, std::nullopt}, below_b{pb.cap, std::nullopt};
      std::optional<std::uint64_t> cap_a, cap_b, fields_a, fields_b;
      if (sa.attributes)
      {
         if (!good(true, parse_cap(*sa.attributes, cap_a)) ||
             !good(true, parse_max_fields(*sa.attributes, fields_a)))
            return false;
      }
      if (sb.attributes)
      {
         if (!good(false, parse_cap(*sb.attributes, cap_b)) ||
             !good(false, parse_max_fields(*sb.attributes, fields_b)))
            return false;
      }
      if (!sa.tuple)
         below_a.cap = cap_a;
      if (!sb.tuple)
         below_b.cap = cap_b;
      for (std::size_t i = 0; i < count; ++i)
      {
         PathScope scope(path, member_step(sa, i));
         Pos ma = below_a, mb = below_b;
         if (!sa.members.empty() &&
             !good(true, parse_length_bound(sa.members[i]->attributes, ma.bound)))
            return false;
         if (!sb.members.empty() &&
             !good(false, parse_length_bound(sb.members[i]->attributes, mb.bound)))
            return false;
         if (names && !sa.members.empty() && !sb.members.empty())
         {
            if (sa.text != Text::positional && sa.members[i]->name != sb.members[i]->name)
            {
               // (reported at the record: the member has two names)
               path.resize(scope.size);
               return fail("field-renamed", "field " + std::to_string(i) +
                                                " was renamed or reordered (\"" +
                                                sa.members[i]->name + "\" -> \"" +
                                                sb.members[i]->name + "\")");
            }
            for (std::string_view form : {"jsonInt", "ijsonInt"})
               if (attribute_value(sa.members[i]->attributes, form) !=
                   attribute_value(sb.members[i]->attributes, form))
                  return fail("text-form-changed",
                              std::string{form} + " differs (an integer as a number or a string)");
         }
         if (!sa.types[i]->get() || !sb.types[i]->get())
            return fail("malformed-schema", "null schema box");
         if (!walk(**sa.types[i], **sb.types[i], ma, mb, exact))
            return false;
      }
      // An appended member must be able to be absent.
      for (std::size_t i = count; i < sb.types.size(); ++i)
      {
         if (!sb.types[i]->get())
            return fail("malformed-schema", "null schema box");
         Layout info;
         if (!good(false, codec_b.layout(**sb.types[i], info)))
            return false;
         if (!info.optional)
            return fail("appended-field-not-optional",
                        "the appended field " +
                            (sb.members.empty() || sb.text == Text::positional
                                 ? std::to_string(i)
                                 : "\"" + sb.members[i]->name + "\"") +
                            " is not optional (old data has no value for it)");
      }
      // Caps (docs/pssz-spec.md 7.5, 16).
      if (exact)
      {
         if (cap_a != cap_b)
            return fail("max-dynamic-data", "maxDynamicData differs (" + number(cap_a) + " / " +
                                                number(cap_b) + "; unbounded: none)");
         if (fields_a != fields_b)
            return fail("max-fields", "maxFields differs (" + number(fields_a) + " / " +
                                          number(fields_b) + "; unbounded: none)");
         return true;
      }
      if (cap_b)
      {
         std::optional<std::uint64_t> limit = cap_a;
         if (!limit)
         {
            std::optional<std::size_t> bound;
            std::vector<const AnyType *> active;
            if (!good(true, codec_a.bound_of(*a.wire, nullptr, false, bound, active)))
               return false;
            if (!bound)
               return fail("max-dynamic-data", "maxDynamicData (" + std::to_string(*cap_b) +
                                                   ") was added to a record of unbounded size");
            limit = *bound;
         }
         if (*limit > *cap_b)
            return fail("max-dynamic-data",
                        cap_a ? "maxDynamicData was lowered (" + std::to_string(*cap_a) + " -> " +
                                    std::to_string(*cap_b) + ")"
                              : "maxDynamicData (" + std::to_string(*cap_b) +
                                    ") is below the bound inferred for the old record (" +
                                    std::to_string(*limit) + ")");
      }
      if (fields_a)
      {
         if (sb.types.size() > *fields_a)
            return fail("max-fields", "more fields (" + std::to_string(sb.types.size()) +
                                          ") than the original's maxFields (" +
                                          std::to_string(*fields_a) + ")");
         if (!fields_b)
            return fail("max-fields",
                        "maxFields (" + std::to_string(*fields_a) + ") was dropped");
         if (*fields_b > *fields_a)
            return fail("max-fields", "maxFields was raised (" + std::to_string(*fields_a) +
                                          " -> " + std::to_string(*fields_b) + ")");
      }
      return true;
   }

   bool enums(const Peeled &a, const Peeled &b, bool exact)
   {
      if (!a.enumeration != !b.enumeration)
         return fail("enum-changed", "an enum on one side only");
      if (!a.enumeration)
         return true;
      const auto &was = a.enumeration->members, &is = b.enumeration->members;
      auto has = [](const std::vector<schema_types::Enumerator> &all,
                    const schema_types::Enumerator &e)
      {
         for (const auto &other : all)
            if (other.name == e.name)
               return other.value == e.value;
         return false;
      };
      for (const auto &e : was)
         if (!has(is, e))
            return fail("enum-changed",
                        "the enumerator \"" + e.name + "\" was removed or renumbered");
      if (exact)
         for (const auto &e : is)
            if (!has(was, e))
               return fail("enum-changed", "the enumerator \"" + e.name + "\" is on one side only");
      return true;
   }

 public:
   Comparer(const Schema &a, const Schema &b, compare what)
       : schema_a(a), schema_b(b), codec_a(a), codec_b(b), names(what == compare::contract)
   {
   }
   compatibility result() && { return std::move(out); }
   bool failed() const { return !out.ok; }

   // Both types are well-formed (and of a version this library reads).
   bool check(const AnyType &a, const AnyType &b)
   {
      return good(true, codec_a.metadata(a)) && good(false, codec_b.metadata(b));
   }

   bool walk(const AnyType &ta, const AnyType &tb, const Pos &pa, const Pos &pb, bool exact)
   {
      Peeled a, b;
      if (!peel(true, ta, a) || !peel(false, tb, b))
         return false;
      if (a.boxed != b.boxed)
         return fail("boxed-changed", "a boxed value on one side only");
      if (names)
      {
         if (a.ids != b.ids)
            return fail("custom-id-changed",
                        "a custom type on one side only, or under another id (\"" +
                            (a.ids.empty() ? std::string{} : std::string{a.ids.front()}) +
                            "\" / \"" +
                            (b.ids.empty() ? std::string{} : std::string{b.ids.front()}) + "\")");
         if (!enums(a, b, exact))
            return false;
      }
      // (a recursive type: the pair is being, or has been, compared)
      for (const auto &known : seen)
         if (known.a == a.wire && known.b == b.wire && known.exact == exact &&
             known.pa.cap == pa.cap && known.pb.cap == pb.cap && same(known.pa.bound, pa.bound) &&
             same(known.pb.bound, pb.bound))
            return true;
      seen.push_back({a.wire, b.wire, pa, pb, exact});
      if (a.kind != b.kind)
         return fail("type-changed", std::string{"the type differs ("} + kind_name(a.kind) +
                                         " / " + kind_name(b.kind) + ")");
      if (a.kind == Kind::unsupported)
         return fail("malformed-schema", "a type the codec does not support");
      if (++nesting > 512)
         return fail("malformed-schema", "type nesting too deep");
      struct Leave
      {
         std::size_t &n;
         ~Leave() { --n; }
      } leave{nesting};

      Layout la, lb;
      if (!good(true, codec_a.layout(*a.wire, la)) || !good(false, codec_b.layout(*b.wire, lb)))
         return false;
      const Pos below_a{pa.cap, std::nullopt}, below_b{pb.cap, std::nullopt};
      auto fixed_size = [&]
      {
         if (la.fixed != lb.fixed || (la.fixed && la.size != lb.size))
            return fail("fixed-size-changed",
                        "a fixed-size type on one side and a variable-size type on the other, or "
                        "another fixed size");
         return true;
      };
      switch (a.kind)
      {
      case Kind::integer:
      {
         auto integer = [](const AnyType &type)
         {
            if (auto i = std::get_if<schema_types::Int>(&type.value))
               return *i;
            return schema_types::Int{8, false};  // "char"
         };
         if (integer(*a.wire) != integer(*b.wire))
            return fail("integer-changed", "the integer type differs (bits or signedness)");
         return true;
      }
      case Kind::floating:
         if (la.size != lb.size)
            return fail("float-changed", "the float type differs");
         return true;
      case Kind::boolean:
         return true;
      case Kind::bitvector:
      case Kind::bitlist:
      {
         std::size_t na, nb;
         bool list;
         if (!good(true, codec_a.bit_shape(std::get<schema_types::Custom>(a.wire->value), na,
                                           list)) ||
             !good(false, codec_b.bit_shape(std::get<schema_types::Custom>(b.wire->value), nb,
                                            list)))
            return false;
         if (a.kind == Kind::bitvector)
         {
            if (na != nb)
               return fail("bit-count-changed", "the bit count differs (" + std::to_string(na) +
                                                    " / " + std::to_string(nb) + ")");
            return true;
         }
         return ranges({0, na}, {0, nb}, exact, "the bit list");
      }
      case Kind::string:
      case Kind::bytes:
      {
         const auto &ca = std::get<schema_types::Custom>(a.wire->value);
         const auto &cb = std::get<schema_types::Custom>(b.wire->value);
         if (ca.id != cb.id)
            return fail("utf8-changed",
                        "utf8 on one side only (a raw string and a UTF-8 string are different "
                        "types)");
         if (!fixed_size())
            return false;
         const AnyType *container_a, *container_b;
         if (!container_of(true, ca, container_a) || !container_of(false, cb, container_b))
            return false;
         return ranges(range_of(container_a, pa.bound), range_of(container_b, pb.bound), exact,
                       a.kind == Kind::string ? "the string" : "the bytes");
      }
      case Kind::nested:
      {
         if (!ranges(range_of(nullptr, pa.bound), range_of(nullptr, pb.bound), exact,
                     "the nested value"))
            return false;
         const auto &na = std::get<schema_types::Nested>(a.wire->value);
         const auto &nb = std::get<schema_types::Nested>(b.wire->value);
         if (!na.type.get() || !nb.type.get())
            return fail("malformed-schema", "null schema box");
         // The inner value is a root of its own: no cap or bound reaches it.
         PathScope scope(path, "(nested)");
         return walk(*na.type, *nb.type, {}, {}, exact);
      }
      case Kind::option:
      {
         const auto &oa = std::get<schema_types::Option>(a.wire->value);
         const auto &ob = std::get<schema_types::Option>(b.wire->value);
         if (!oa.type.get() || !ob.type.get())
            return fail("malformed-schema", "null schema box");
         bool empty_a, empty_b;
         if (!good(true, codec_a.can_be_empty(*oa.type, empty_a)) ||
             !good(false, codec_b.can_be_empty(*ob.type, empty_b)))
            return false;
         if (empty_a != empty_b)
            return fail("optional-form-changed",
                        "the optional's presence form differs (a selector byte or not)");
         // The member's bound and the cap reach the payload.
         PathScope scope(path, "?");
         return walk(*oa.type, *ob.type, pa, pb, exact);
      }
      case Kind::list:
      case Kind::array:
      {
         auto element = [](const AnyType &type) -> const schema_types::Box<AnyType> &
         {
            if (auto list = std::get_if<schema_types::List>(&type.value))
               return list->type;
            if (auto bounded = std::get_if<schema_types::BoundedList>(&type.value))
               return bounded->type;
            return std::get<schema_types::Array>(type.value).type;
         };
         if (a.kind == Kind::array)
         {
            const auto len_a = std::get<schema_types::Array>(a.wire->value).len;
            const auto len_b = std::get<schema_types::Array>(b.wire->value).len;
            if (len_a != len_b)
               return fail("array-length-changed", "the array length differs (" +
                                                       std::to_string(len_a) + " / " +
                                                       std::to_string(len_b) + ")");
         }
         else if (!ranges(range_of(a.wire, pa.bound), range_of(b.wire, pb.bound), exact,
                          "the list"))
            return false;
         const auto &ea = element(*a.wire), &eb = element(*b.wire);
         if (!ea.get() || !eb.get())
            return fail("malformed-schema", "null schema box");
         Layout elem_a, elem_b;
         if (!good(true, codec_a.layout(*ea, elem_a)) || !good(false, codec_b.layout(*eb, elem_b)))
            return false;
         if (!(elem_a.fixed && elem_b.fixed) &&
             !widths(*a.wire, *b.wire, pa, pb, a.kind == Kind::array ? "the array" : "the list"))
            return false;
         PathScope scope(path, "[]");
         return walk(*ea, *eb, below_a, below_b, exact);
      }
      case Kind::map:
      case Kind::set:
      {
         Codec<0>::AssocShape sa, sb;
         if (!good(true, codec_a.assoc_shape(std::get<schema_types::Custom>(a.wire->value), sa)) ||
             !good(false, codec_b.assoc_shape(std::get<schema_types::Custom>(b.wire->value), sb)))
            return false;
         const char *what = a.kind == Kind::map ? "the map" : "the set";
         if (!ranges(range_of(sa.list, pa.bound), range_of(sb.list, pb.bound), exact, what))
            return false;
         Layout elem_a, elem_b;
         if (!good(true, codec_a.layout(**sa.element, elem_a)) ||
             !good(false, codec_b.layout(**sb.element, elem_b)))
            return false;
         if (!(elem_a.fixed && elem_b.fixed) && !widths(*sa.list, *sb.list, pa, pb, what))
            return false;
         // Another key type is another order (docs/pssz-spec.md 5.9.4):
         // the keys are the same type, in both relations.
         {
            PathScope scope(path, a.kind == Kind::map ? "[].0" : "[]");
            if (!walk(*sa.key, *sb.key, below_a, below_b, true))
            {
               out.code = "key-type-changed";
               out.reason = std::string{"the key type of "} + what + " differs: " + out.reason;
               return false;
            }
         }
         if (a.kind == Kind::set)
            return true;
         // The entry is a tuple (key, value): its width, then the value.
         const AnyType *entry_a, *entry_b;
         if (!good(true, codec_a.alias(**sa.element, entry_a)) ||
             !good(false, codec_b.alias(**sb.element, entry_b)))
            return false;
         {
            PathScope scope(path, "[]");
            if (!widths(*entry_a, *entry_b, below_a, below_b, "the map entry"))
               return false;
         }
         PathScope scope(path, "[].1");
         return walk(*sa.value, *sb.value, below_a, below_b, exact);
      }
      case Kind::variant:
      {
         const auto &va = std::get<schema_types::Variant>(a.wire->value);
         const auto &vb = std::get<schema_types::Variant>(b.wire->value);
         if (exact && va.members.size() != vb.members.size())
            return fail("alternative-count-changed",
                        "the number of alternatives differs (" +
                            std::to_string(va.members.size()) + " / " +
                            std::to_string(vb.members.size()) + ")");
         if (vb.members.size() < va.members.size())
            return fail("alternatives-removed", "variant alternatives were removed (" +
                                                    std::to_string(va.members.size()) + " -> " +
                                                    std::to_string(vb.members.size()) + ")");
         if (names && !find_attribute(va.attributes, "untagged") !=
                          !find_attribute(vb.attributes, "untagged"))
            return fail("text-form-changed", "an untagged variant on one side only");
         for (std::size_t i = 0; i < va.members.size(); ++i)
         {
            const auto &ma = va.members[i], &mb = vb.members[i];
            if (names && ma.name != mb.name)
               return fail("alternative-renamed", "alternative " + std::to_string(i) +
                                                      " was renamed or reordered (\"" + ma.name +
                                                      "\" -> \"" + mb.name + "\")");
            PathScope scope(path, "<" + ma.name + ">");
            if (!ma.type.get() || !mb.type.get())
               return fail("malformed-schema", "null schema box");
            if (!walk(*ma.type, *mb.type, below_a, below_b, exact))
               return false;
            if (names && attribute_value(ma.attributes, "ijsonName") !=
                             attribute_value(mb.attributes, "ijsonName"))
               return fail("text-form-changed", "the I-JSON name of the alternative differs");
         }
         return true;
      }
      case Kind::record:
         if (!fixed_size())
            return false;
         return record(a, b, pa, pb, exact);
      default:
         return fail("malformed-schema", "a type the codec does not support");
      }
   }

   bool types(const AnyType &a, const AnyType &b, bool exact)
   {
      return check(a, b) && walk(a, b, {}, {}, exact);
   }

   bool function(const schema_types::Func &fa, const schema_types::Func &fb, bool exact)
   {
      {
         const AnyType &args_a = made.emplace_back(argument_list(fa));
         const AnyType &args_b = made.emplace_back(argument_list(fb));
         PathScope scope(path, "(args)");
         if (!types(args_a, args_b, exact))
            return false;
      }
      PathScope scope(path, "(result)");
      if (!fa.result != !fb.result)
         return fail("result-changed", "a result on one side only");
      if (!fa.result)
         return true;
      if (!fa.result->get() || !fb.result->get())
         return fail("malformed-schema", "null schema box");
      auto bound = [&](bool first, const schema_types::Func &f, Pos &pos)
      {
         std::vector<schema_types::Attribute> own;
         if (auto found = find_attribute(f.attributes, "result.lengthBound"))
            own.push_back({"lengthBound", found->value});
         return good(first, parse_length_bound(own, pos.bound));
      };
      Pos pa, pb;
      if (!bound(true, fa, pa) || !bound(false, fb, pb))
         return false;
      return check(**fa.result, **fb.result) && walk(**fa.result, **fb.result, pa, pb, exact);
   }

   bool interface_pair(const schema_types::Interface &ia, const schema_types::Interface &ib, bool exact)
   {
      auto find = [](const schema_types::Interface &in,
                     std::string_view name) -> const schema_types::Func *
      {
         for (const auto &f : in.funcs)
            if (f.name == name)
               return &f;
         return nullptr;
      };
      for (const auto &f : ia.funcs)
      {
         PathScope scope(path, f.name);
         const auto *other = find(ib, f.name);
         if (!other)
            return fail("function-removed", "the function was removed");
         if (!function(f, *other, exact))
            return false;
      }
      if (exact)
         for (const auto &f : ib.funcs)
            if (!find(ia, f.name))
            {
               PathScope scope(path, f.name);
               return fail("function-added", "the function is on one side only");
            }
      auto has = [](const schema_types::Interface &in, std::string_view name)
      {
         return std::find(in.type_names.begin(), in.type_names.end(), name) !=
                in.type_names.end();
      };
      for (const auto &name : ia.type_names)
      {
         PathScope scope(path, "type:" + name);
         if (!has(ib, name))
            return fail("type-removed", "the type was removed from the interface");
         const AnyType reference{schema_types::Type{name}};
         if (!types(reference, reference, exact))
            return false;
      }
      if (exact)
         for (const auto &name : ib.type_names)
            if (!has(ia, name))
            {
               PathScope scope(path, "type:" + name);
               return fail("type-added", "the type is on one side only");
            }
      return true;
   }

   bool schemas(bool exact)
   {
      auto find = [](const Schema &schema, std::string_view name) -> const schema_types::Interface *
      {
         for (const auto &in : schema.interfaces)
            if (in.name == name)
               return &in;
         return nullptr;
      };
      for (const auto &in : schema_a.interfaces)
      {
         PathScope scope(path, in.name + "/");
         const auto *other = find(schema_b, in.name);
         if (!other)
            return fail("interface-removed", "the interface was removed");
         if (!interface_pair(in, *other, exact))
            return false;
      }
      if (exact)
         for (const auto &in : schema_b.interfaces)
            if (!find(schema_a, in.name))
            {
               PathScope scope(path, in.name + "/");
               return fail("interface-added", "the interface is on one side only");
            }
      return true;
   }
};
} // namespace detail

// ── Two types ──────────────────────────────────────────────────────────────
//
// `type_a` / `type_b` are types of their schemas: an AnyType taken from one
// (`*schema.get("Row")`, a member's type, a table's row type), or a name —
// `equivalent(a, "Row", b, "Row")` — which converts to the reference
// `Type{"Row"}`.

// Whether the two are the same type: the same values, the same bytes, the
// same permitted revisions (and, under compare::contract, the same text).
[[nodiscard]] inline compatibility equivalent(const schema_types::Schema &schema_a,
                                              const schema_types::AnyType &type_a,
                                              const schema_types::Schema &schema_b,
                                              const schema_types::AnyType &type_b,
                                              compare what = compare::contract)
{
   detail::Comparer c(schema_a, schema_b, what);
   c.types(type_a, type_b, true);
   return std::move(c).result();
}
// Whether `new_type` may replace `old_type`: its reader reads every value
// the old writer can produce (docs/pssz-spec.md 7.5 / 7.6).
[[nodiscard]] inline compatibility evolves_from(const schema_types::Schema &old_schema,
                                                const schema_types::AnyType &old_type,
                                                const schema_types::Schema &new_schema,
                                                const schema_types::AnyType &new_type,
                                                compare what = compare::contract)
{
   detail::Comparer c(old_schema, new_schema, what);
   c.types(old_type, new_type, false);
   return std::move(c).result();
}

// ── Two functions: argument list and result ────────────────────────────────
[[nodiscard]] inline compatibility equivalent(const schema_types::Schema &schema_a,
                                              const schema_types::Func &func_a,
                                              const schema_types::Schema &schema_b,
                                              const schema_types::Func &func_b,
                                              compare what = compare::contract)
{
   detail::Comparer c(schema_a, schema_b, what);
   c.function(func_a, func_b, true);
   return std::move(c).result();
}
[[nodiscard]] inline compatibility evolves_from(const schema_types::Schema &old_schema,
                                                const schema_types::Func &old_func,
                                                const schema_types::Schema &new_schema,
                                                const schema_types::Func &new_func,
                                                compare what = compare::contract)
{
   detail::Comparer c(old_schema, new_schema, what);
   c.function(old_func, new_func, false);
   return std::move(c).result();
}

// ── Two interfaces: every function and every listed type, by name ──────────
[[nodiscard]] inline compatibility equivalent(const schema_types::Schema &schema_a,
                                              const schema_types::Interface &interface_a,
                                              const schema_types::Schema &schema_b,
                                              const schema_types::Interface &interface_b,
                                              compare what = compare::contract)
{
   detail::Comparer c(schema_a, schema_b, what);
   c.interface_pair(interface_a, interface_b, true);
   return std::move(c).result();
}
[[nodiscard]] inline compatibility evolves_from(const schema_types::Schema &old_schema,
                                                const schema_types::Interface &old_interface,
                                                const schema_types::Schema &new_schema,
                                                const schema_types::Interface &new_interface,
                                                compare what = compare::contract)
{
   detail::Comparer c(old_schema, new_schema, what);
   c.interface_pair(old_interface, new_interface, false);
   return std::move(c).result();
}

// ── Two whole schemas: every interface, by name ────────────────────────────
[[nodiscard]] inline compatibility equivalent(const schema_types::Schema &schema_a,
                                              const schema_types::Schema &schema_b,
                                              compare what = compare::contract)
{
   detail::Comparer c(schema_a, schema_b, what);
   c.schemas(true);
   return std::move(c).result();
}
[[nodiscard]] inline compatibility evolves_from(const schema_types::Schema &old_schema,
                                                const schema_types::Schema &new_schema,
                                                compare what = compare::contract)
{
   detail::Comparer c(old_schema, new_schema, what);
   c.schemas(false);
   return std::move(c).result();
}
} // namespace psio::pssz_ir

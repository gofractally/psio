// Phase 2 — annotation system tests.

#include <psio/annotate.hpp>
#include <psio/max_size.hpp>
#include <psio/reflect.hpp>
#include <psio/wrappers.hpp>

#include <catch.hpp>

#include <cstdint>
#include <tuple>

namespace {

   struct Foo
   {
      int         a;
      std::string b;
   };

}  // namespace

// Desugared form: specialize psio::annotate<&Foo::a> and <&Foo::b>
// at namespace scope. This is exactly what the (future) PSIO_ATTRS
// macro will expand to; writing it by hand works today.

template <>
inline constexpr auto psio::annotate<&Foo::a> = std::tuple{
   psio::length_bound{.exact = 48},
   psio::field_num_spec{.value = 1},
};

template <>
inline constexpr auto psio::annotate<&Foo::b> = std::tuple{
   psio::utf8_spec{.max = 256, .field_num = 2},
};

template <>
inline constexpr auto psio::annotate<psio::type<Foo>{}> = std::tuple{
   psio::definition_will_not_change{},
};

TEST_CASE("annotate<X> defaults to empty tuple", "[annotate]")
{
   struct Uncharted
   {
      int z;
   };
   STATIC_REQUIRE(std::tuple_size_v<decltype(psio::annotate<&Uncharted::z>)> == 0);
}

TEST_CASE("annotate<X> round-trips the spec tuple", "[annotate]")
{
   constexpr auto a_anns = psio::annotate<&Foo::a>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(a_anns)> == 2);

   constexpr auto b_anns = psio::annotate<&Foo::b>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(b_anns)> == 1);

   constexpr auto foo_type_anns = psio::annotate<psio::type<Foo>{}>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(foo_type_anns)> == 1);
}

TEST_CASE("find_spec returns the spec by type, or nullopt", "[annotate]")
{
   constexpr auto anns = psio::annotate<&Foo::a>;

   auto lb = psio::find_spec<psio::length_bound>(anns);
   REQUIRE(lb.has_value());
   REQUIRE(lb->exact.has_value());
   REQUIRE(*lb->exact == 48);

   auto fn = psio::find_spec<psio::field_num_spec>(anns);
   REQUIRE(fn.has_value());
   REQUIRE(fn->value == 1);

   auto us = psio::find_spec<psio::utf8_spec>(anns);
   REQUIRE(!us.has_value());
}

TEST_CASE("has_spec_v is a compile-time predicate", "[annotate]")
{
   using A = std::remove_cvref_t<decltype(psio::annotate<&Foo::a>)>;
   using B = std::remove_cvref_t<decltype(psio::annotate<&Foo::b>)>;

   STATIC_REQUIRE((psio::has_spec_v<psio::length_bound, A>));
   STATIC_REQUIRE((!psio::has_spec_v<psio::utf8_spec, A>));

   STATIC_REQUIRE((psio::has_spec_v<psio::utf8_spec, B>));
   STATIC_REQUIRE((!psio::has_spec_v<psio::length_bound, B>));
}

TEST_CASE("Spec category tags separate static from runtime specs",
          "[annotate][category]")
{
   STATIC_REQUIRE(psio::is_runtime_spec_v<psio::length_bound>);
   STATIC_REQUIRE(psio::is_runtime_spec_v<psio::utf8_spec>);
   STATIC_REQUIRE(psio::is_runtime_spec_v<psio::sorted_spec>);

   STATIC_REQUIRE(psio::is_static_spec_v<psio::field_num_spec>);
   STATIC_REQUIRE(psio::is_static_spec_v<psio::skip_spec>);
   STATIC_REQUIRE(psio::is_static_spec_v<psio::definition_will_not_change>);
}

TEST_CASE("Shape applicability trait answers both ways",
          "[annotate][applies_to]")
{
   // length_bound applies to VariableSequence / ByteString / Bitfield.
   STATIC_REQUIRE(
      (psio::spec_applies_to_shape_v<psio::length_bound,
                                      psio::VariableSequenceShape>));
   STATIC_REQUIRE(
      (psio::spec_applies_to_shape_v<psio::length_bound,
                                      psio::ByteStringShape>));
   STATIC_REQUIRE(
      (!psio::spec_applies_to_shape_v<psio::length_bound,
                                       psio::RecordShape>));

   // field_num_spec has no applies_to → permissive.
   STATIC_REQUIRE(
      (psio::spec_applies_to_shape_v<psio::field_num_spec,
                                      psio::RecordShape>));
   STATIC_REQUIRE(
      (psio::spec_applies_to_shape_v<psio::field_num_spec,
                                      psio::PrimitiveShape>));
}

TEST_CASE("Convenience factories build the right spec values", "[annotate]")
{
   STATIC_REQUIRE((psio::spec::bytes<48>.exact.value() == 48));
   STATIC_REQUIRE((psio::spec::max_size<1024>.max.value() == 1024));
   STATIC_REQUIRE((psio::spec::min_size<4>.min.value() == 4));
   STATIC_REQUIRE((psio::spec::utf8<256>.max == 256));
   STATIC_REQUIRE((psio::spec::hex<32>.bytes == 32));
   STATIC_REQUIRE((psio::spec::field<3>.value == 3));
}

TEST_CASE("operator| composes spec values into a tuple", "[annotate]")
{
   constexpr auto combined = psio::spec::bytes<48> | psio::spec::field<1>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(combined)> == 2);
   STATIC_REQUIRE(std::get<0>(combined).exact.value() == 48);
   STATIC_REQUIRE(std::get<1>(combined).value == 1);

   // Chain of 3.
   constexpr auto triple =
       psio::spec::bytes<32> | psio::spec::field<2> | psio::spec::sorted;
   STATIC_REQUIRE(std::tuple_size_v<decltype(triple)> == 3);
   STATIC_REQUIRE(std::get<2>(triple).unique == false);
   STATIC_REQUIRE(std::get<2>(triple).ascending == true);
}

TEST_CASE("Type-level annotations route through psio::type<T>{}",
          "[annotate]")
{
   constexpr auto type_anns = psio::annotate<psio::type<Foo>{}>;
   auto dwnc =
       psio::find_spec<psio::definition_will_not_change>(type_anns);
   REQUIRE(dwnc.has_value());
}

// ── PSIO_ATTRS macro coverage ─────────────────────────────────────────

namespace macro_attrs {
   struct Widget {
      std::int32_t id;
      std::string  label;
      std::int64_t amount;
   };
}

// Single-field form.
PSIO_FIELD_ATTRS(macro_attrs::Widget, id,
   psio::field_num_spec{.value = 7})

// Multi-field form via parenthesized entries. Spec expressions that
// contain commas (designated-init lists, operator| chains) must be
// wrapped in their own parens so the preprocessor's argument splitter
// sees one spec per entry.
PSIO_ATTRS(macro_attrs::Widget,
   (label, (psio::utf8_spec{.max = 128, .field_num = 2}
          | psio::length_bound{.max = 128})),
   (amount, (psio::field_num_spec{.value = 3})))

// Type-level form.
PSIO_TYPE_ATTRS(macro_attrs::Widget, psio::definition_will_not_change{})

TEST_CASE("PSIO_FIELD_ATTRS emits a member annotation", "[annotate][macro]")
{
   constexpr auto anns = psio::annotate<&macro_attrs::Widget::id>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(anns)> == 1);
   auto f = psio::find_spec<psio::field_num_spec>(anns);
   REQUIRE(f.has_value());
   REQUIRE(f->value == 7);
}

TEST_CASE("PSIO_ATTRS routes (field, specs) entries", "[annotate][macro]")
{
   constexpr auto label_anns = psio::annotate<&macro_attrs::Widget::label>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(label_anns)> == 2);
   auto u = psio::find_spec<psio::utf8_spec>(label_anns);
   REQUIRE(u.has_value());
   REQUIRE(u->max == 128);

   constexpr auto amount_anns = psio::annotate<&macro_attrs::Widget::amount>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(amount_anns)> == 1);
   auto f = psio::find_spec<psio::field_num_spec>(amount_anns);
   REQUIRE(f->value == 3);
}

TEST_CASE("PSIO_TYPE_ATTRS emits a type-level annotation",
          "[annotate][macro]")
{
   constexpr auto type_anns =
      psio::annotate<psio::type<macro_attrs::Widget>{}>;
   auto d =
      psio::find_spec<psio::definition_will_not_change>(type_anns);
   REQUIRE(d.has_value());
}

// ── length_bound on optional<T> propagates to inner T ────────────────
//
// The applicability check unwraps transparent wrappers — a spec attached
// to an `optional<U>` field is also accepted if it would apply to U.
// This allows annotation-form bounds to reach into common wrapper types
// without separate codec plumbing per wrapper.

struct OptBoundForm {
   std::optional<std::string>                title;
   std::optional<std::vector<std::uint32_t>> tags;
};
PSIO_REFLECT(OptBoundForm, title, tags)
PSIO_FIELD_ATTRS(OptBoundForm, title,
   psio::length_bound{.max = 64})
PSIO_FIELD_ATTRS(OptBoundForm, tags,
   psio::length_bound{.max = 16})

// ── PSIO_REFLECT keyword dispatch (attr / definitionWillNotChange) ──

struct AttrSyntaxRecord
{
   std::int32_t                 id     = 0;
   std::string                  label;
   std::vector<std::uint32_t>   tags;
   std::optional<std::string>   note;
};
PSIO_REFLECT(AttrSyntaxRecord,
   id,
   attr(label, max<63>),
   attr(tags,  max<255> | field<7>),
   attr(note,  max<255>))

struct DwncRecord
{
   std::uint64_t a = 0;
   std::uint64_t b = 0;
};
PSIO_REFLECT(DwncRecord, a, b, definitionWillNotChange())

TEST_CASE("PSIO_REFLECT attr(...) registers field + annotation",
          "[annotate][reflect][attr]")
{
   using R = psio::reflect<AttrSyntaxRecord>;
   STATIC_REQUIRE(R::member_count == 4);

   // Bare field — no annotation.
   constexpr auto id_anns = psio::annotate<&AttrSyntaxRecord::id>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(id_anns)> == 0);

   // attr(label, max<63>) — single spec.
   constexpr auto label_anns = psio::annotate<&AttrSyntaxRecord::label>;
   auto label_lb = psio::find_spec<psio::length_bound>(label_anns);
   REQUIRE(label_lb.has_value());
   REQUIRE(label_lb->max == 63);

   // attr(tags, max<255> | field<7>) — composed specs.
   constexpr auto tags_anns = psio::annotate<&AttrSyntaxRecord::tags>;
   auto tags_lb = psio::find_spec<psio::length_bound>(tags_anns);
   auto tags_fn = psio::find_spec<psio::field_num_spec>(tags_anns);
   REQUIRE(tags_lb.has_value());
   REQUIRE(tags_lb->max == 255);
   REQUIRE(tags_fn.has_value());
   REQUIRE(tags_fn->value == 7);

   // attr(note, max<255>) — annotation reaches the inner string via
   // the optional unwrap rule (landed earlier).
   constexpr auto note_anns = psio::annotate<&AttrSyntaxRecord::note>;
   auto note_lb = psio::find_spec<psio::length_bound>(note_anns);
   REQUIRE(note_lb.has_value());
   REQUIRE(note_lb->max == 255);
}

TEST_CASE("PSIO_REFLECT definitionWillNotChange() emits type-level dwnc",
          "[annotate][reflect][dwnc]")
{
   using R = psio::reflect<DwncRecord>;
   // Field count is 2 — definitionWillNotChange() is not a field.
   STATIC_REQUIRE(R::member_count == 2);

   constexpr auto type_anns =
      psio::annotate<psio::type<DwncRecord>{}>;
   auto dwnc =
      psio::find_spec<psio::definition_will_not_change>(type_anns);
   REQUIRE(dwnc.has_value());
   STATIC_REQUIRE(psio::is_dwnc_v<DwncRecord>);
}

// ── New type-level caps: maxFields(N) / maxDynamicData(N) ────────────────
//
// Types at file scope because PSIO_REFLECT cats the bare identifier
// into its helper struct name.

struct CapOnlyRecord { std::uint32_t a, b, c; };
PSIO_REFLECT(CapOnlyRecord, a, b, c, maxFields(8))

struct FullyBoundedRecord
{
   std::uint64_t                slot;
   std::vector<std::uint8_t>    payload;
};
// All three type-level keywords in one PSIO_REFLECT — exercises the
// tuple_cat aggregator path.  Field-level attr(...) coexists.
PSIO_REFLECT(FullyBoundedRecord, slot, attr(payload, max<256>),
             definitionWillNotChange(),
             maxFields(8),
             maxDynamicData(4096))

TEST_CASE("PSIO_REFLECT maxFields(N) emits max_fields_spec", "[annotate][reflect][caps]")
{
   using T = CapOnlyRecord;
   STATIC_REQUIRE(psio::reflect<T>::member_count == 3);
   constexpr auto a = psio::annotate<psio::type<T>{}>;
   auto mf = psio::find_spec<psio::max_fields_spec>(a);
   REQUIRE(mf.has_value());
   REQUIRE(mf->value == 8);
   REQUIRE(psio::max_fields_v<T>.has_value());
   REQUIRE(psio::max_fields_v<T>.value() == 8);
   // No other type-level annotations attached.
   REQUIRE_FALSE(psio::is_dwnc_v<T>);
   REQUIRE_FALSE(psio::max_dynamic_data_v<T>.has_value());
}

TEST_CASE("PSIO_REFLECT combines dwnc + maxFields + maxDynamicData",
          "[annotate][reflect][caps]")
{
   using T = FullyBoundedRecord;
   STATIC_REQUIRE(psio::reflect<T>::member_count == 2);
   constexpr auto a = psio::annotate<psio::type<T>{}>;

   // All three type-level specs land in the aggregated tuple.
   REQUIRE(psio::find_spec<psio::definition_will_not_change>(a).has_value());
   auto mf = psio::find_spec<psio::max_fields_spec>(a);
   REQUIRE(mf.has_value());
   REQUIRE(mf->value == 8);
   auto md = psio::find_spec<psio::max_dynamic_data_spec>(a);
   REQUIRE(md.has_value());
   REQUIRE(md->value == 4096);

   // Convenience accessors.
   STATIC_REQUIRE(psio::is_dwnc_v<T>);
   REQUIRE(psio::max_fields_v<T> == std::optional<std::size_t>{8});
   REQUIRE(psio::max_dynamic_data_v<T> ==
           std::optional<std::size_t>{4096});

   // Field-level annotation (attr(payload, max<256>)) still attaches —
   // type-level aggregator must not interfere with per-field emit.
   constexpr auto payload_anns =
      psio::annotate<&T::payload>;
   auto payload_lb = psio::find_spec<psio::length_bound>(payload_anns);
   REQUIRE(payload_lb.has_value());
   REQUIRE(payload_lb->max == 256);
}

// ── Foundation traits: all_dwnc_v / effective_max_*_v ───────────────────

struct LeafDwnc { std::uint32_t a = 0; };
PSIO_REFLECT(LeafDwnc, a, definitionWillNotChange())

struct LeafExt  { std::uint32_t a = 0; };
PSIO_REFLECT(LeafExt, a)  // no DWNC

struct WrapsDwnc
{
   LeafDwnc                inner;
   std::vector<LeafDwnc>   list;
   std::optional<LeafDwnc> maybe;
};
PSIO_REFLECT(WrapsDwnc, inner, list, maybe, definitionWillNotChange())

struct WrapsExt
{
   LeafDwnc inner_dwnc;
   LeafExt  inner_ext;   // breaks the transitive DWNC chain
};
PSIO_REFLECT(WrapsExt, inner_dwnc, inner_ext, definitionWillNotChange())

TEST_CASE("all_dwnc_v: primitives and strings are leaves", "[annotate][caps]")
{
   STATIC_REQUIRE(psio::all_dwnc_v<std::uint32_t>);
   STATIC_REQUIRE(psio::all_dwnc_v<bool>);
   STATIC_REQUIRE(psio::all_dwnc_v<std::byte>);
   STATIC_REQUIRE(psio::all_dwnc_v<std::string>);
   // Containers of leaves recurse → still true.
   STATIC_REQUIRE(psio::all_dwnc_v<std::vector<std::uint64_t>>);
   STATIC_REQUIRE(psio::all_dwnc_v<std::optional<float>>);
}

TEST_CASE("all_dwnc_v: reflected DWNC composition", "[annotate][caps]")
{
   STATIC_REQUIRE(psio::all_dwnc_v<LeafDwnc>);
   STATIC_REQUIRE_FALSE(psio::all_dwnc_v<LeafExt>);
   // WrapsDwnc has DWNC type-level + DWNC inner / vector<DWNC> /
   // optional<DWNC> members — all paths recurse to true.
   STATIC_REQUIRE(psio::all_dwnc_v<WrapsDwnc>);
   // WrapsExt is itself DWNC but contains a non-DWNC member → the
   // transitive chain breaks.
   STATIC_REQUIRE_FALSE(psio::all_dwnc_v<WrapsExt>);
}

TEST_CASE("effective_max_fields_v: explicit cap wins downward",
          "[annotate][caps]")
{
   // CapOnlyRecord has 3 reflected fields and maxFields(8).  The cap
   // is GREATER than member_count → effective is min(8, 3) = 3.
   REQUIRE(psio::effective_max_fields_v<CapOnlyRecord> ==
           std::optional<std::size_t>{3});
   // FullyBoundedRecord has 2 reflected fields and maxFields(8).
   // min(8, 2) = 2.
   REQUIRE(psio::effective_max_fields_v<FullyBoundedRecord> ==
           std::optional<std::size_t>{2});
   // Type without a cap: just member_count.
   REQUIRE(psio::effective_max_fields_v<LeafDwnc> ==
           std::optional<std::size_t>{1});
}

TEST_CASE("effective_max_dynamic_v: cap and inferred bound interplay",
          "[annotate][caps]")
{
   // FullyBoundedRecord has maxDynamicData(4096) + attr(payload, max<256>).
   // The per-field max=256 is tighter than the type-level 4096, so
   // max_encoded_size<T> ≤ 256 + (slot/header overhead) < 4096.
   // effective_max_dynamic_v should pick the inferred (smaller) bound.
   constexpr auto eff = psio::effective_max_dynamic_v<FullyBoundedRecord>;
   REQUIRE(eff.has_value());
   REQUIRE(*eff <= 4096);

   // LeafDwnc: no cap, inferred bound is its fixed encoded size.  Some
   // value must come back (a DWNC u32 record has a finite bound).
   constexpr auto leaf = psio::effective_max_dynamic_v<LeafDwnc>;
   REQUIRE(leaf.has_value());
}

TEST_CASE("length_bound on optional<T> reaches the inner T",
          "[annotate][optional]")
{
   // Compiling these effective_annotations_for instantiations exercises
   // the static_assert in all_specs_apply.  Before the fix this failed
   // with "spec is attached to a field whose shape is not in the spec's
   // applies_to set" because OptionalShape isn't in length_bound's
   // applies_to.  After the fix, applicability sees through optional<>
   // and finds the inner ByteString / VariableSequence shape.
   using TitleEff = psio::effective_annotations_for<
      OptBoundForm, std::optional<std::string>, &OptBoundForm::title>;
   constexpr auto title_lb =
      psio::find_spec<psio::length_bound>(TitleEff::value);
   REQUIRE(title_lb.has_value());
   REQUIRE(title_lb->max == 64);

   using TagsEff = psio::effective_annotations_for<
      OptBoundForm, std::optional<std::vector<std::uint32_t>>,
      &OptBoundForm::tags>;
   constexpr auto tags_lb =
      psio::find_spec<psio::length_bound>(TagsEff::value);
   REQUIRE(tags_lb.has_value());
   REQUIRE(tags_lb->max == 16);
}

// ── WIT attribute specs (final, canonical, unique_keys, flags, ...) ──

#include <map>
#include <set>
#include <string>
#include <unordered_map>

struct WitAttrRecord
{
   std::int32_t v = 0;
};
PSIO_REFLECT(WitAttrRecord, v)
PSIO_TYPE_ATTRS(WitAttrRecord,
   psio::canonical | psio::final_v
   | psio::since_spec{.version = "0.2.0"})

TEST_CASE("WIT attribute specs available + composable",
          "[annotate][wit_attrs]")
{
   STATIC_REQUIRE(std::is_same_v<
      decltype(psio::canonical), const psio::canonical_spec>);
   STATIC_REQUIRE(std::is_same_v<
      decltype(psio::final_v), const psio::final_spec>);

   constexpr auto type_anns =
      psio::annotate<psio::type<WitAttrRecord>{}>;
   REQUIRE(psio::find_spec<psio::canonical_spec>(type_anns)
              .has_value());
   REQUIRE(psio::find_spec<psio::final_spec>(type_anns)
              .has_value());
   auto since = psio::find_spec<psio::since_spec>(type_anns);
   REQUIRE(since.has_value());
   REQUIRE(std::string_view{since->version} == "0.2.0");
}

TEST_CASE("std::map carries sorted + unique_keys via inherent_annotations",
          "[annotate][inherent]")
{
   using M = std::map<std::int32_t, std::string>;
   constexpr auto anns = psio::inherent_annotations<M>::value;
   auto srt = psio::find_spec<psio::sorted_spec>(anns);
   auto uk  = psio::find_spec<psio::unique_keys_spec>(anns);
   REQUIRE(srt.has_value());
   REQUIRE(srt->unique == true);
   REQUIRE(uk.has_value());
}

TEST_CASE("std::unordered_map carries unique_keys only", "[annotate][inherent]")
{
   using M = std::unordered_map<std::int32_t, std::string>;
   constexpr auto anns = psio::inherent_annotations<M>::value;
   REQUIRE(psio::find_spec<psio::unique_keys_spec>(anns).has_value());
   REQUIRE_FALSE(psio::find_spec<psio::sorted_spec>(anns).has_value());
}

TEST_CASE("std::set carries sorted + unique_keys", "[annotate][inherent]")
{
   using S = std::set<std::int64_t>;
   constexpr auto anns = psio::inherent_annotations<S>::value;
   REQUIRE(psio::find_spec<psio::sorted_spec>(anns).has_value());
   REQUIRE(psio::find_spec<psio::unique_keys_spec>(anns).has_value());
}

TEST_CASE("std::u8string carries utf8", "[annotate][inherent]")
{
   constexpr auto anns =
      psio::inherent_annotations<std::u8string>::value;
   REQUIRE(psio::find_spec<psio::utf8_spec>(anns).has_value());
}

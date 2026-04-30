// psch_tests.cpp — round-trip + lookup tests for the psch schema format.

#include <psio/psch.hpp>

#define CATCH_CONFIG_FAST_COMPILE
#include <catch.hpp>

#include <string>
#include <string_view>
#include <vector>

using psio::psch::field_lookup;
using psio::psch::kind;
using psio::psch::view;
using psio::psch::writer;

TEST_CASE("psch: round-trip a flat container schema",
          "[psch][roundtrip]")
{
   writer w;
   auto u32 = w.add_u32();
   auto str = w.add_bytes();
   auto p   = w.add_container({{"name", str}, {"age", u32}});

   auto bytes = w.finalize(p);
   REQUIRE(bytes.size() > 0);

   view v(bytes.data(), bytes.size());
   CHECK(v.type_count() == 3);
   CHECK(v.root_type_id() == p);
   CHECK(v.type_kind(p) == kind::container);
   CHECK(v.container_field_count(p) == 2);

   // field_by_index: order is preserved.
   auto f0 = v.field_by_index(p, 0);
   CHECK(f0.first == "name");
   CHECK(f0.second == str);
   auto f1 = v.field_by_index(p, 1);
   CHECK(f1.first == "age");
   CHECK(f1.second == u32);

   // field_by_name: O(1) PHF lookup returns ordered_index + type_id.
   auto fn_name = v.field_by_name(p, "name");
   REQUIRE(fn_name.has_value());
   CHECK(fn_name->ordered_index == 0);
   CHECK(fn_name->type_id == str);

   auto fn_age = v.field_by_name(p, "age");
   REQUIRE(fn_age.has_value());
   CHECK(fn_age->ordered_index == 1);
   CHECK(fn_age->type_id == u32);

   // Out-of-set names: PHF may hash to a populated slot but the verify
   // step rejects them.
   CHECK_FALSE(v.field_by_name(p, "missing").has_value());
   CHECK_FALSE(v.field_by_name(p, "score").has_value());
}

TEST_CASE("psch: nested container/vector/list types resolve via type_id",
          "[psch][nested]")
{
   writer w;
   auto u8_t  = w.add_u8();
   auto u32_t = w.add_u32();
   auto str_t = w.add_bytes();

   // bytes_n[48] for a BLS pubkey-sized field.
   auto pubkey = w.add_bytes_n(48);

   auto validator = w.add_container(
      {{"pubkey", pubkey}, {"effective_balance", u32_t}});

   auto validators = w.add_list(validator);  // List<Validator>

   auto root = w.add_container(
      {{"finalized_root", w.add_bytes_n(32)},
       {"validators", validators}});

   auto bytes = w.finalize(root);
   view v(bytes.data(), bytes.size());

   // Walk the chain: root → "validators" → list → elem → "pubkey" →
   // bytes_n[48].
   auto v_field = v.field_by_name(root, "validators");
   REQUIRE(v_field.has_value());
   auto list_id = v_field->type_id;
   CHECK(v.type_kind(list_id) == kind::list_);

   auto elem_id = v.list_elem_type(list_id);
   CHECK(v.type_kind(elem_id) == kind::container);

   auto pk_field = v.field_by_name(elem_id, "pubkey");
   REQUIRE(pk_field.has_value());
   auto pk_id = pk_field->type_id;
   CHECK(v.type_kind(pk_id) == kind::bytes_n);
   CHECK(v.bytes_n_length(pk_id) == 48);
}

TEST_CASE("psch: PHF builds for K up to 32 fields",
          "[psch][phf]")
{
   writer w;
   auto u32_t = w.add_u32();

   // 32 fields with unique short names.
   std::vector<std::pair<std::string_view, std::uint16_t>> fields;
   std::vector<std::string> names_storage;
   names_storage.reserve(32);
   for (int i = 0; i < 32; ++i)
   {
      names_storage.push_back("f" + std::to_string(i));
      fields.push_back({std::string_view(names_storage.back()), u32_t});
   }
   auto big = w.add_container(fields);

   auto bytes = w.finalize(big);
   view v(bytes.data(), bytes.size());
   CHECK(v.container_field_count(big) == 32);

   // All 32 names resolve correctly.
   for (int i = 0; i < 32; ++i)
   {
      auto fn = v.field_by_name(big, names_storage[i]);
      REQUIRE(fn.has_value());
      CHECK(fn->ordered_index == static_cast<std::uint8_t>(i));
   }

   // Misses still cleanly report not-found.
   CHECK_FALSE(v.field_by_name(big, "f99").has_value());
   CHECK_FALSE(v.field_by_name(big, "").has_value());
}

TEST_CASE("psch: vector has elem + length",
          "[psch][vector]")
{
   writer w;
   auto u32_t = w.add_u32();
   auto vec   = w.add_vector(u32_t, 16);

   auto bytes = w.finalize(vec);
   view v(bytes.data(), bytes.size());
   CHECK(v.type_kind(vec) == kind::vector_);
   auto [elem, len] = v.vector_elem_and_length(vec);
   CHECK(elem == u32_t);
   CHECK(len == 16);
}

TEST_CASE("psch: union variants",
          "[psch][union]")
{
   writer w;
   auto u32_t = w.add_u32();
   auto str_t = w.add_bytes();
   auto bln_t = w.add_bool();

   std::vector<std::uint16_t> variants{u32_t, str_t, bln_t};
   auto u = w.add_union(variants);

   auto bytes = w.finalize(u);
   view v(bytes.data(), bytes.size());
   CHECK(v.type_kind(u) == kind::union_);
   CHECK(v.union_variant_count(u) == 3);
   CHECK(v.union_variant(u, 0) == u32_t);
   CHECK(v.union_variant(u, 1) == str_t);
   CHECK(v.union_variant(u, 2) == bln_t);
}

TEST_CASE("psch: sized for typical schemas",
          "[psch][size]")
{
   // Small schema (2 types in a container with 4 fields):
   writer w;
   auto u32_t = w.add_u32();
   auto str_t = w.add_bytes();
   auto p     = w.add_container({{"name", str_t},
                                 {"id", u32_t},
                                 {"active", w.add_bool()},
                                 {"version", u32_t}});
   auto bytes = w.finalize(p);

   // ~80 bytes with compact slots: header + 5 type entries + 1
   // container block (3 + 8 + 4*4 slots = 27) + name_pool ~17.
   CHECK(bytes.size() < 100);

   // Verify the schema picked compact slot mode (small enough).
   view v(bytes.data(), bytes.size());
   CHECK(v.compact_slots());
   CHECK(v.slot_stride() == 4);
}

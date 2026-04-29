// native_view tests: zero-cost shim that adapts a live const T& to the
// view<T, Fmt, Store> accessor surface.
//
// Covers:
//   * native_view(const T&) constructs view<T, native, const_borrow>.
//   * record_field_span<native> returns spans pointing at live memory.
//   * Arithmetic and string field access through the view returns the
//     same values as direct member access on the live object.
//   * String access aliases the live std::string's data (zero-copy).
//   * view_of<V, T> concept matches view<T, F, S> across formats.

#include <psio/native_view.hpp>
#include <psio/reflect.hpp>
#include <psio/storage.hpp>
#include <psio/view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <string_view>

struct NvUser
{
   std::uint64_t id;
   std::string   name;
   std::uint64_t group_id;
};
PSIO_REFLECT(NvUser, id, name, group_id)

struct NvAddr
{
   std::string   street;
   std::uint32_t zip;
};
PSIO_REFLECT(NvAddr, street, zip)

struct NvPerson
{
   std::uint64_t id;
   NvAddr        home;
};
PSIO_REFLECT(NvPerson, id, home)


TEST_CASE("native_view: arithmetic field access returns by-value copies",
          "[native_view]")
{
   NvUser u{42, "alice", 100};
   auto   v = psio::native_view(u);

   STATIC_REQUIRE(decltype(v)::storage_kind == psio::storage::const_borrow);
   REQUIRE(v.template get<0>() == 42);
   REQUIRE(v.template get<2>() == 100);
}

TEST_CASE("native_view: std::string field aliases the live string buffer",
          "[native_view]")
{
   NvUser u{7, "longname-needs-no-allocation", 0};
   auto   v = psio::native_view(u);

   auto sv = v.template get<1>().view_();
   REQUIRE(sv.size() == u.name.size());
   REQUIRE(sv.data() == u.name.data());
}

TEST_CASE("native_view: nested reflected struct returns sub-view at member's address",
          "[native_view]")
{
   NvPerson p{1, NvAddr{"Main St", 12345}};
   auto     v = psio::native_view(p);

   REQUIRE(v.template get<0>() == 1);
   auto h = v.template get<1>();
   REQUIRE(h.template get<0>().view_().data() == p.home.street.data());
   REQUIRE(h.template get<1>() == 12345);
}

TEST_CASE("native_view: independent live objects yield independent views",
          "[native_view]")
{
   NvUser a{1, "alice", 100};
   NvUser b{2, "bob",   200};

   auto va = psio::native_view(a);
   auto vb = psio::native_view(b);

   REQUIRE(va.template get<0>() == 1);
   REQUIRE(vb.template get<0>() == 2);
   REQUIRE(va.template get<1>().view_() != vb.template get<1>().view_());
}

TEST_CASE("view_of<V, T>: matches view<T, native, ...> and rejects non-views",
          "[native_view][view_of]")
{
   NvUser u{1, "alice", 100};
   auto   v = psio::native_view(u);

   STATIC_REQUIRE(psio::view_of<decltype(v), NvUser>);
   STATIC_REQUIRE(psio::is_native_view_v<decltype(v)>);

   STATIC_REQUIRE_FALSE(psio::view_of<int, NvUser>);
   STATIC_REQUIRE_FALSE(psio::view_of<NvUser, NvUser>);
   STATIC_REQUIRE_FALSE(psio::view_of<NvUser&, NvUser>);
   STATIC_REQUIRE_FALSE(psio::view_of<decltype(v), NvAddr>);
}

namespace {

   // Generic extractor written once against view_of<NvUser>.
   template <psio::view_of<NvUser> V>
   auto sort_key(V v)
   {
      return std::make_pair(std::string(v.template get<1>().view_()),
                            v.template get<2>());
   }

}  // namespace

TEST_CASE("view_of extractor: written once, runs on native views",
          "[native_view][view_of][extractor]")
{
   NvUser u{42, "alice", 100};
   auto   v = psio::native_view(u);

   auto k = sort_key(v);
   REQUIRE(k.first == "alice");
   REQUIRE(k.second == 100);
}

TEST_CASE("native_view: view's data span covers the live object's bytes",
          "[native_view]")
{
   NvUser u{42, "alice", 100};
   auto   v = psio::native_view(u);

   auto raw = psio::bytes(v);
   REQUIRE(raw.size() == sizeof(NvUser));
   REQUIRE(reinterpret_cast<const NvUser*>(raw.data()) == &u);
}

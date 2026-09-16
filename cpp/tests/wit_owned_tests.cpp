#include <catch.hpp>
#include <psio/wit_owned.hpp>
#include <psio/wview.hpp>
#include <psio/wit_gen.hpp>
#include <psio/wit_constexpr.hpp>

struct OwnedRecord { std::string name; std::vector<uint32_t> values; };
PSIO_REFLECT(OwnedRecord, name, values)
struct OwnedInterface {
   static wit::string text();
   static wit::vector<uint32_t> numbers();
};
PSIO_INTERFACE(OwnedInterface, types(), funcs(func(text), func(numbers)))

TEST_CASE("WIT buffers retain their allocation across ownership transfers") {
   wit::string first{"hello"};
   auto* allocation = first.data();
   wit::string second = std::move(first);
   CHECK(first.empty());
   CHECK(second.data() == allocation);
   CHECK(second.view() == "hello");
   auto raw = second.release();
   CHECK(second.empty());
   auto third = wit::string::adopt(raw.ptr, raw.len);
   CHECK(third.data() == allocation);
   third[0] = 'H';
   CHECK(third.view() == "Hello");

   uint32_t source[] = {1, 2, 3};
   wit::vector<uint32_t> list{std::span<const uint32_t>{source}};
   auto* elements = list.data();
   auto moved = std::move(list);
   CHECK(list.empty());
   CHECK(moved.view().data() == elements);
   CHECK(moved[2] == 3);
   auto buffer = moved.release();
   auto adopted = wit::vector<uint32_t>::adopt(buffer.ptr, buffer.len);
   CHECK(adopted.data() == elements);
}

TEST_CASE("WIT borrowed projections refer directly to native field storage") {
   OwnedRecord record{"borrow me", {4, 5, 6}};
   psio::WViewImpl<OwnedRecord> view{record};
   CHECK(view.get<0>().data() == record.name.data());
   CHECK(view.get<1>().data() == record.values.data());
   CHECK(view.proxy().name() == record.name);
   CHECK(view.promote().values == record.values);
}

TEST_CASE("Owned records expose borrowed const fields") {
   wit::val<OwnedRecord> record;
   record.name() = wit::string{"owned"};
   uint32_t values[] = {7, 8};
   record.values() = wit::vector<uint32_t>{std::span<const uint32_t>{values}};
   const auto& borrowed = record;
   CHECK(borrowed.name().data() == record.name().data());
   CHECK(borrowed.values().data() == record.values().data());
}

TEST_CASE("Owned buffer interfaces emit their schema types in both WIT generators") {
   auto text = psio::generate_wit_text<OwnedInterface>("test:owned@1.0.0");
   CHECK(text.find("text: func() -> string") != std::string::npos);
   CHECK(text.find("numbers: func() -> list<u32>") != std::string::npos);
   constexpr auto literal = psio::constexpr_wit::interface_text<OwnedInterface>();
   CHECK(text.find(std::string{literal.first.data(), literal.second}) != std::string::npos);
}

struct NamedFields { std::string proxy; uint32_t get; };
PSIO_REFLECT(NamedFields, proxy, get)
struct NamedCalls { static uint32_t call(uint32_t x); static uint32_t proxy(uint32_t x); };
PSIO_INTERFACE(NamedCalls, types(), funcs(func(call, x), func(proxy, x)))
struct NamedDispatch {
   template <size_t I, typename Function> uint32_t call(uint32_t x) { return x + I; }
};
TEST_CASE("Named projections do not reserve ordinary field or method names") {
   NamedFields value{"view", 42};
   psio::WViewImpl<NamedFields> view{value};
   CHECK(view.proxy().proxy() == "view");
   CHECK(view.proxy().get() == 42);
   psio::detail::interface_info<NamedCalls>::proxy<NamedDispatch> api;
   CHECK(api.call(42) == 42);
   CHECK(api.proxy(42) == 43);
}

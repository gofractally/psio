// Packing in place: psio::nested_of / nested_as / packed / packed_as and
// psio::method_args_packed / method_args_nested.
//
// A member that is the packed bytes of a value (a psio::nested<X>, a
// psio::shared_pssz<X>, a byte list holding an encoding) is written from the
// value, straight into the buffer of the enclosing value. Proved here:
//
//   - the bytes are those of the two-step form (pack the inner value, then
//     the outer one around its bytes), for nested members, byte-list
//     members and method arguments, to any depth;
//   - the heap: three layers around a 1 MiB payload peak at the size of the
//     result, in ONE allocation — the two-step form peaks at three times
//     that, in three;
//   - the inner value is checked where it is encoded.

#include <catch.hpp>

#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <cstdlib>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// ── Allocation counting ──────────────────────────────────────────────────
namespace
{
   std::size_t live = 0, peak = 0, allocations = 0;

   struct heap_window
   {
      std::size_t before;
      heap_window() : before(live)
      {
         peak        = live;
         allocations = 0;
      }
      // The most that was live at once beside what was live at the start.
      [[nodiscard]] std::size_t peak_bytes() const { return peak - before; }
      [[nodiscard]] std::size_t count() const { return allocations; }
   };
}  // namespace

void* operator new(std::size_t n)
{
   auto* p = static_cast<std::size_t*>(std::malloc(n + 16));
   if (!p)
      throw std::bad_alloc{};
   *p = n;
   live += n;
   ++allocations;
   if (live > peak)
      peak = live;
   return reinterpret_cast<char*>(p) + 16;
}
void operator delete(void* p) noexcept
{
   if (!p)
      return;
   auto* q = reinterpret_cast<std::size_t*>(static_cast<char*>(p) - 16);
   live -= *q;
   std::free(q);
}
void operator delete(void* p, std::size_t) noexcept
{
   operator delete(p);
}

namespace
{
   // ── Nested members ────────────────────────────────────────────────────
   struct Inner
   {
      std::vector<char> data;
      friend bool       operator==(const Inner&, const Inner&) = default;
   };
   PSIO_REFLECT(Inner, data)
   struct Middle
   {
      std::uint32_t       tag = 0;
      psio::nested<Inner> inner;
   };
   PSIO_REFLECT(Middle, tag, inner)
   struct Outer
   {
      std::uint32_t        tag = 0;
      psio::nested<Middle> middle;
   };
   PSIO_REFLECT(Outer, tag, middle)
   // The same record with a shared member: the same wire.
   struct OuterShared
   {
      std::uint32_t             tag = 0;
      psio::shared_pssz<Middle> middle;
   };
   PSIO_REFLECT(OuterShared, tag, middle)

   // The records that hold the value instead of its bytes.
   template <typename I>
   struct MiddleOf
   {
      std::uint32_t tag = 0;
      I             inner;
      PSIO_REFLECT(MiddleOf, tag, inner)
   };
   template <typename I>
   MiddleOf(std::uint32_t, I) -> MiddleOf<I>;
   template <typename M>
   struct OuterOf
   {
      std::uint32_t tag = 0;
      M             middle;
      PSIO_REFLECT(OuterOf, tag, middle)
   };
   template <typename M>
   OuterOf(std::uint32_t, M) -> OuterOf<M>;

   // ── A call inside a call (psibase's Action) ───────────────────────────
   struct Action
   {
      std::uint64_t     sender  = 0;
      std::uint64_t     service = 0;
      std::uint64_t     method  = 0;
      std::vector<char> rawData;   // the packed arguments
      friend bool       operator==(const Action&, const Action&) = default;
   };
   // (A schema name on the owning record, as psibase's Action has, is not
   // part of its layout.)
   PSIO_REFLECT(Action, schemaId("Action"), sender, service, method, rawData)
   // The action with its arguments typed.
   template <typename Args>
   struct TypedAction
   {
      std::uint64_t      sender  = 0;
      std::uint64_t      service = 0;
      std::uint64_t      method  = 0;
      psio::nested<Args> rawData;
      PSIO_REFLECT(TypedAction, sender, service, method, rawData)
   };
   // The action that holds a source of its arguments.
   template <typename Args>
   struct ActionOf
   {
      std::uint64_t sender  = 0;
      std::uint64_t service = 0;
      std::uint64_t method  = 0;
      Args          rawData;
      PSIO_REFLECT(ActionOf, sender, service, method, rawData)
   };
   template <typename Args>
   ActionOf(std::uint64_t, std::uint64_t, std::uint64_t, Args) -> ActionOf<Args>;

   struct Transaction
   {
      std::uint32_t     expiration = 0;
      std::vector<char> payload;
      friend bool       operator==(const Transaction&, const Transaction&) = default;
   };
   PSIO_REFLECT(Transaction, expiration, payload)

   struct Quantity
   {
      std::uint64_t value = 0;
      Quantity()          = default;
      Quantity(std::uint64_t v) : value(v) {}
      explicit Quantity(double) : value(~std::uint64_t{0}) {}
      PSIO_REFLECT(Quantity, value)
   };

   struct Transact
   {
      void execTrx(psio::pssz_view<const Transaction> trx, bool speculative);
      void pay(Quantity amount, std::string memo);
      PSIO_REFLECT(Transact, method(execTrx, trx, speculative),
                   method(pay, amount, attr(memo, utf8 | max<8>), maxDynamicData(64)))
   };
   struct XRun
   {
      std::uint64_t post(std::uint8_t mode, Action action, std::uint32_t timeout,
                         std::uint64_t callback);
      PSIO_REFLECT(XRun, method(post, mode, action, timeout, callback))
   };
   struct HttpServer
   {
      void sendProds(const Action& action);
      PSIO_REFLECT(HttpServer, method(sendProds, action))
   };

   // A source built from temporaries and returned: nothing it refers to is
   // gone by the time it is encoded.
   auto exec_action(const std::vector<char>& packed_trx)
   {
      return ActionOf{1, 2, 3,
                      psio::method_args_packed<Transact, 0>(
                         psio::pssz_view<Transaction>{packed_trx}, true)};
   }
   auto post_action(const std::vector<char>& packed_trx)
   {
      return ActionOf{4, 5, 6,
                      psio::method_args_packed<XRun, 0>(std::uint8_t{2}, exec_action(packed_trx),
                                                        1000, 77)};
   }

   // A bounded payload and one with an invariant.
   struct Small
   {
      std::string text;
      PSIO_REFLECT(Small, attr(text, utf8 | max<4>))
   };
   struct Envelope
   {
      std::uint32_t       id = 0;
      psio::nested<Small> body;
      PSIO_REFLECT(Envelope, id, body)
   };
   struct Capped
   {
      std::uint32_t     id = 0;
      std::vector<char> body;
      PSIO_REFLECT(Capped, id, attr(body, max<16>))
   };
   template <typename B>
   struct CappedOf
   {
      std::uint32_t id = 0;
      B             body;
      PSIO_REFLECT(CappedOf, id, attr(body, max<16>))
   };
   template <typename B>
   struct EnvelopeOf
   {
      std::uint32_t id = 0;
      B             body;
      PSIO_REFLECT(EnvelopeOf, id, body)
   };
   template <typename B>
   EnvelopeOf(std::uint32_t, B) -> EnvelopeOf<B>;

   // What the sources are, to the type system.
   static_assert(std::is_same_v<psio::wire_type_t<decltype(psio::nested_of(Inner{}))>,
                                psio::nested<Inner>>);
   static_assert(std::is_same_v<psio::wire_type_t<decltype(psio::packed(Inner{}))>,
                                std::vector<char>>);
   static_assert(psio::packs_as<MiddleOf<psio::nested_ref<Inner>>, Middle>);
   static_assert(
      psio::packs_as<OuterOf<psio::nested_ref<Middle, MiddleOf<psio::nested_ref<Inner>>>>, Outer>);
   static_assert(psio::packs_as<OuterOf<psio::nested_ref<Middle>>, OuterShared>);
   static_assert(psio::packs_as<ActionOf<psio::packed_ref<Transaction>>, Action>);
   static_assert(!psio::packs_as<ActionOf<psio::nested_ref<Transaction>>, Action>);
   static_assert(psio::packs_as<ActionOf<psio::nested_ref<Transaction>>, TypedAction<Transaction>>);
   static_assert(!psio::packs_as<MiddleOf<psio::nested_ref<Middle>>, Middle>);
   // An lvalue is referred to, an rvalue held.
   static_assert(std::is_same_v<decltype(psio::nested_of(std::declval<const Inner&>())),
                                psio::nested_ref<Inner, const Inner&>>);
   static_assert(std::is_same_v<decltype(psio::nested_of(std::declval<Inner&>())),
                                psio::nested_ref<Inner, const Inner&>>);
   static_assert(std::is_same_v<decltype(psio::nested_of(Inner{})), psio::nested_ref<Inner, Inner>>);
   static_assert(std::is_same_v<decltype(psio::packed(std::string_view{})),
                                psio::packed_ref<std::string, std::string_view>>);
}  // namespace

TEST_CASE("in place: nested members have the bytes of the two-step encoding", "[pssz][in_place]")
{
   const Inner inner{{'a', 'b', 'c'}};
   const auto  two_step = psio::to_pssz(
       Outer{1, psio::nested<Middle>::from(Middle{2, psio::nested<Inner>::from(inner)})});

   const auto direct = psio::to_pssz(OuterOf{1, psio::nested_of(MiddleOf{2, psio::nested_of(inner)})});
   CHECK(direct == two_step);
   CHECK(psio::validate_pssz<Outer>(direct).ok());
   CHECK(psio::validate_strict<Outer>(psio::pssz{}, direct).ok());
   CHECK(psio::from_pssz<Outer>(direct).middle.decode().inner.decode() == inner);

   // Every way of encoding takes the source.
   const auto source = OuterOf{1, psio::nested_of(MiddleOf{2, psio::nested_of(inner)})};
   CHECK(psio::pssz_size(source) == two_step.size());
   CHECK((psio::to_pssz_as<Outer>(source) == two_step));
   std::vector<char> appended{'x'};
   psio::to_pssz(source, appended);
   CHECK(std::vector<char>(appended.begin() + 1, appended.end()) == two_step);
   std::vector<char> checked;
   CHECK(psio::encode_pssz_checked(psio::pssz{}, source, checked).ok());
   CHECK(checked == two_step);
   CHECK(psio::encode(psio::pssz32{}, source) ==
         psio::encode(psio::pssz32{},
                      Outer{1, psio::nested<Middle>::from(
                                  Middle{2, psio::nested<Inner>::from(inner)})}));
   // A source validates as what it is encoded as.
   CHECK(psio::validate_pssz<std::remove_const_t<decltype(source)>>(two_step).ok());

   // Standing alone a nested value is its inner value; so is its source.
   CHECK(psio::to_pssz(psio::nested_of(inner)) == psio::to_pssz(inner));
   CHECK(psio::nested_of(inner).size() == psio::pssz_size(inner));
   // nested_as names the type; a view is written verbatim.
   const auto inner_bytes = psio::to_pssz(inner);
   CHECK(psio::to_pssz(MiddleOf{2, psio::nested_as<Inner>(psio::pssz_view<Inner>{inner_bytes})}) ==
         psio::to_pssz(Middle{2, psio::nested<Inner>::from(inner)}));
   // In a list and in an optional.
   const std::vector<psio::nested<Inner>> list{psio::nested<Inner>::from(inner),
                                               psio::nested<Inner>::from(Inner{})};
   const Inner                            empty;
   CHECK(psio::to_pssz(std::vector{psio::nested_of(inner), psio::nested_of(empty)}) ==
         psio::to_pssz(list));
   CHECK(psio::to_pssz(std::optional{psio::nested_of(inner)}) ==
         psio::to_pssz(std::optional{psio::nested<Inner>::from(inner)}));
}

TEST_CASE("in place: three nested layers of 1 MiB are one allocation of the result's size",
          "[pssz][in_place][alloc]")
{
   constexpr std::size_t size = 1 << 20;
   const Inner           inner{std::vector<char>(size, 'x')};

   std::size_t two_step_peak = 0, two_step_count = 0, direct_peak = 0, direct_count = 0;
   std::vector<char> two_step, direct;
   {
      heap_window heap;
      two_step = psio::to_pssz(
          Outer{1, psio::nested<Middle>::from(Middle{2, psio::nested<Inner>::from(inner)})});
      two_step_peak  = heap.peak_bytes();
      two_step_count = heap.count();
   }
   {
      heap_window heap;
      direct       = psio::to_pssz(OuterOf{1, psio::nested_of(MiddleOf{2, psio::nested_of(inner)})});
      direct_peak  = heap.peak_bytes();
      direct_count = heap.count();
   }
   INFO("two-step: peak " << two_step_peak << " bytes in " << two_step_count
                          << " allocations; in place: peak " << direct_peak << " bytes in "
                          << direct_count);
   // (Compared outside the macro: Catch would print a megabyte.)
   const bool same_bytes = direct == two_step;
   CHECK(same_bytes);
   CHECK(direct.size() == size + 32);
   // In place: the result, and nothing else.
   CHECK(direct_count == 1);
   CHECK(direct_peak == direct.size());
   // The two-step form holds every layer at once.
   CHECK(two_step_count >= 3);
   CHECK(two_step_peak >= 3 * size);
}

TEST_CASE("in place: a call inside a call inside a call", "[pssz][in_place][method][alloc]")
{
   constexpr std::size_t size = 1 << 20;
   const Transaction     trx{7, std::vector<char>(size, 't')};
   const auto            packed_trx = psio::to_pssz(trx);
   const psio::pssz_view<Transaction> view{packed_trx};

   // As it is built today: the arguments, the action around them, the
   // arguments of the call that takes the action, the action around those.
   std::vector<char> layered;
   std::size_t       layered_peak = 0, layered_count = 0;
   {
      heap_window heap;
      Action exec{1, 2, 3, psio::method_args_to_pssz<Transact, 0>(view, true)};
      layered = psio::to_pssz(
         Action{4, 5, 6, psio::method_args_to_pssz<XRun, 0>(std::uint8_t{2}, exec, 1000, 77)});
      layered_peak  = heap.peak_bytes();
      layered_count = heap.count();
   }
   // In place.
   std::vector<char> direct;
   std::size_t       direct_peak = 0, direct_count = 0;
   {
      heap_window heap;
      direct = psio::to_pssz(ActionOf{
         4, 5, 6,
         psio::method_args_packed<XRun, 0>(
            std::uint8_t{2},
            ActionOf{1, 2, 3, psio::method_args_packed<Transact, 0>(view, true)}, 1000, 77)});
      direct_peak  = heap.peak_bytes();
      direct_count = heap.count();
   }
   INFO("layered: peak " << layered_peak << " bytes in " << layered_count
                         << " allocations; in place: peak " << direct_peak << " bytes in "
                         << direct_count);
   const bool same_bytes = direct == layered;
   CHECK(same_bytes);
   CHECK(direct_count == 1);
   CHECK(direct_peak == direct.size());
   CHECK(direct.size() < size + 256);
   CHECK(layered_count >= 3);
   CHECK(layered_peak >= 3 * size);

   // What arrives is the call it would have been.
   REQUIRE(psio::validate_pssz<Action>(direct).ok());
   const auto outer = psio::from_pssz<Action>(direct);
   REQUIRE(psio::validate_method_args<XRun, 0>(outer.rawData).ok());
   const auto post_args =
      psio::from_pssz<psio::reflect<XRun>::method_arg_tuple<0>>(outer.rawData);
   CHECK(std::get<0>(post_args) == 2);
   CHECK(std::get<2>(post_args) == 1000);
   const Action& exec = std::get<1>(post_args);
   CHECK(exec.sender == 1);
   REQUIRE(psio::validate_method_args<Transact, 0>(exec.rawData).ok());
   const auto exec_args =
      psio::from_pssz<psio::reflect<Transact>::method_arg_tuple<0>>(exec.rawData);
   const bool same_trx = std::get<0>(exec_args) == trx;
   CHECK(same_trx);
   CHECK(std::get<1>(exec_args));

   // Built by functions that return sources made of temporaries.
   const bool same_built = psio::to_pssz(post_action(packed_trx)) == layered;
   CHECK(same_built);
   // As the argument of a further call: still one buffer.
   std::vector<char> sent;
   std::size_t       sent_count = 0, sent_peak = 0;
   {
      heap_window heap;
      sent       = psio::to_pssz(ActionOf{
         8, 9, 10, psio::method_args_packed<HttpServer, 0>(post_action(packed_trx))});
      sent_count = heap.count();
      sent_peak  = heap.peak_bytes();
   }
   CHECK(sent_count == 1);
   CHECK(sent_peak == sent.size());
   const bool same_sent =
      sent == psio::to_pssz(Action{8, 9, 10,
                                   psio::method_args_to_pssz<HttpServer, 0>(
                                      psio::from_pssz<Action>(layered))});
   CHECK(same_sent);
   // method_args_to_pssz takes the source as the argument it stands for.
   const bool same_args =
      psio::method_args_to_pssz<HttpServer, 0>(post_action(packed_trx)) ==
      psio::method_args_to_pssz<HttpServer, 0>(psio::from_pssz<Action>(layered));
   CHECK(same_args);
}

TEST_CASE("in place: typed arguments (a nested argument tuple)", "[pssz][in_place][method]")
{
   using Args = psio::reflect<Transact>::method_arg_tuple<1>;
   const auto typed = psio::to_pssz(
      TypedAction<Args>{1, 2, 3, psio::nested<Args>::from(Args{Quantity{std::uint64_t{5}}, "memo"})});
   CHECK(psio::to_pssz(ActionOf{1, 2, 3, psio::method_args_nested<Transact, 1>(5, "memo")}) ==
         typed);
   CHECK(psio::to_pssz(ActionOf{1, 2, 3, psio::method_args_packed<Transact, 1>(5, "memo")}) ==
         typed);
   CHECK(typed ==
         psio::to_pssz(Action{1, 2, 3, psio::method_args_to_pssz<Transact, 1>(5, "memo")}));
   // Arguments are converted as a call converts them: implicitly (5 is
   // Quantity(std::uint64_t), never the explicit Quantity(double)).
   const auto args = psio::from_pssz<Args>(
      psio::from_pssz<Action>(
         psio::to_pssz(ActionOf{1, 2, 3, psio::method_args_packed<Transact, 1>(5, "memo")}))
         .rawData);
   CHECK(std::get<0>(args).value == 5);
   static_assert(
      psio::packs_as<decltype(ActionOf{1, 2, 3, psio::method_args_nested<Transact, 1>(5, "memo")}),
                     TypedAction<Args>>);
   static_assert(
      psio::packs_as<decltype(ActionOf{1, 2, 3, psio::method_args_packed<Transact, 1>(5, "memo")}),
                     Action>);
}

TEST_CASE("in place: the inner value is checked where it is encoded", "[pssz][in_place]")
{
   const Small ok{"abcd"}, long_text{"abcde"}, not_utf8{"\xff"};
   CHECK(psio::to_pssz(EnvelopeOf{1, psio::nested_of(ok)}) ==
         psio::to_pssz(Envelope{1, psio::nested<Small>::from(ok)}));
   // A value with no encoding fails the enclosing encode ...
   CHECK_THROWS_AS(psio::to_pssz(EnvelopeOf{1, psio::nested_of(long_text)}), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_pssz(EnvelopeOf{1, psio::nested_of(not_utf8)}), psio::codec_exception);
   CHECK_THROWS_AS(psio::to_pssz(EnvelopeOf{1, psio::packed(long_text)}), psio::codec_exception);
   // ... and writes nothing.
   std::vector<char> out{'x'};
   CHECK_FALSE(
      psio::encode_pssz_checked(psio::pssz{}, EnvelopeOf{1, psio::nested_of(long_text)}, out).ok());
   CHECK(out == std::vector<char>{'x'});
   CHECK_THROWS_AS(psio::to_pssz(EnvelopeOf{1, psio::nested_of(long_text)}, out),
                   psio::codec_exception);
   CHECK(out == std::vector<char>{'x'});
   // Arguments outside the method's bounds or cap.
   CHECK_THROWS_AS(
      psio::to_pssz(ActionOf{1, 2, 3, psio::method_args_packed<Transact, 1>(5, "123456789")}),
      psio::codec_exception);

   // A bound on the member counts the bytes written in place.
   const Inner three{{'a', 'b', 'c'}}, nine{std::vector<char>(9, 'z')};
   CHECK(psio::to_pssz(CappedOf<psio::packed_ref<Inner>>{1, psio::packed(three)}) ==
         psio::to_pssz(Capped{1, psio::to_pssz(three)}));
   CHECK_THROWS_AS(psio::to_pssz(CappedOf<psio::packed_ref<Inner>>{1, psio::packed(nine)}),
                   psio::codec_exception);
}

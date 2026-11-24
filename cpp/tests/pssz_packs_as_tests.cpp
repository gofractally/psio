// psio::packs_as<U, T>: "to_pssz(const U&) yields the bytes of a T", decided
// at compile time. Every pair the concept accepts is also checked at run
// time: the bytes are equal, and validate as T.

#include <catch.hpp>

#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace
{
   struct Trace
   {
      std::uint32_t code = 0;
      std::string   error;
      friend bool   operator==(const Trace&, const Trace&) = default;
   };
   PSIO_REFLECT(Trace, code, error)

   // The owning row.
   struct Rec
   {
      std::uint64_t id = 0;
      Trace         trace;
      friend bool   operator==(const Rec&, const Rec&) = default;
   };
   PSIO_REFLECT(Rec, id, trace)
   // The same members, by view.
   struct RecView
   {
      std::uint64_t          id = 0;
      psio::pssz_view<Trace> trace;
   };
   PSIO_REFLECT(RecView, id, trace)
   // By reference, and with another member name.
   struct RecRef
   {
      std::uint64_t                       key = 0;
      std::reference_wrapper<const Trace> t;
   };
   PSIO_REFLECT(RecRef, key, t)
   // A record of non-owning members inside a record of non-owning members.
   struct TraceText
   {
      std::uint32_t    code = 0;
      std::string_view error;
   };
   PSIO_REFLECT(TraceText, code, error)
   struct RecDeep
   {
      std::uint64_t id = 0;
      TraceText     trace;
   };
   PSIO_REFLECT(RecDeep, id, trace)

   // Not a Rec.
   struct Wrong
   {
      std::string   id;
      std::uint32_t n = 0;
   };
   PSIO_REFLECT(Wrong, id, n)
   struct Short
   {
      std::uint64_t id = 0;
   };
   PSIO_REFLECT(Short, id)
   struct Longer
   {
      std::uint64_t              id = 0;
      Trace                      trace;
      std::optional<std::string> note;
   };
   PSIO_REFLECT(Longer, id, trace, note)
   struct Narrow
   {
      std::uint32_t id = 0;
      Trace         trace;
   };
   PSIO_REFLECT(Narrow, id, trace)
   struct Frozen
   {
      std::uint64_t id = 0;
      Trace         trace;
   };
   PSIO_REFLECT(Frozen, definitionWillNotChange(), id, trace)
   struct Capped
   {
      std::uint64_t id = 0;
      Trace         trace;
      PSIO_REFLECT(Capped, id, trace, maxDynamicData(200))
   };

   // Bounds are part of the layout.
   struct Tags
   {
      std::vector<std::string> tags;
      PSIO_REFLECT(Tags, attr(tags, max<4> | element(max<8>)))
   };
   struct TagsView
   {
      std::span<const std::string_view> tags;
      PSIO_REFLECT(TagsView, attr(tags, max<4> | element(max<8>)))
   };
   struct TagsLoose
   {
      std::span<const std::string_view> tags;
      PSIO_REFLECT(TagsLoose, tags)
   };

   // A fixed record and its bytes.
   struct Point
   {
      std::int32_t x = 0;
      std::int32_t y = 0;
      friend bool  operator==(const Point&, const Point&) = default;
   };
   PSIO_REFLECT(Point, definitionWillNotChange(), x, y)

   struct Svc
   {
      void put(Rec row, std::uint32_t n) {}
      void tag(std::vector<std::string> tags) {}
      PSIO_REFLECT(Svc, method(put, row, n, maxDynamicData(4096)), method(tag, tags))
   };

   // ── What holds ────────────────────────────────────────────────────────
   // The value, and every source of it.
   static_assert(psio::packs_as<Rec, Rec>);
   static_assert(psio::packs_as<const Rec&, Rec>);
   static_assert(psio::packs_as<psio::pssz_view<Rec>, Rec>);
   static_assert(psio::packs_as<psio::pssz_view<const Rec>, Rec>);
   static_assert(psio::packs_as<std::reference_wrapper<const Rec>, Rec>);
   static_assert(psio::packs_as<std::string_view, std::string>);
   static_assert(psio::packs_as<const char*, std::string>);
   static_assert(psio::packs_as<std::span<const std::uint32_t>, std::vector<std::uint32_t>>);
   static_assert(psio::packs_as<std::tuple<const std::uint64_t&, const Trace&>,
                                std::tuple<std::uint64_t, Trace>>);
   static_assert(psio::packs_as<std::tuple<std::uint64_t, psio::pssz_view<Trace>>,
                                std::tuple<std::uint64_t, Trace>>);
   // A record of views / references / string views for the owning record.
   static_assert(psio::packs_as<RecView, Rec>);
   static_assert(psio::packs_as<RecRef, Rec>);
   static_assert(psio::packs_as<RecDeep, Rec>);
   static_assert(psio::packs_as<Rec, RecView>);  // symmetric: same bytes
   static_assert(psio::packs_as<RecView, RecDeep>);
   static_assert(psio::packs_as<TagsView, Tags>);
   // Through containers.
   static_assert(psio::packs_as<std::vector<RecView>, std::vector<Rec>>);
   static_assert(psio::packs_as<std::span<const RecView>, std::vector<Rec>>);
   static_assert(psio::packs_as<std::optional<RecDeep>, std::optional<Rec>>);
   static_assert(psio::packs_as<std::array<RecView, 2>, std::array<Rec, 2>>);
   static_assert(psio::packs_as<std::variant<std::uint8_t, RecView>,
                                std::variant<std::uint8_t, Rec>>);
   static_assert(psio::packs_as<std::tuple<RecView, std::string_view>,
                                std::tuple<Rec, std::string>>);
   // A packed value is, as a whole value, the bytes of what it holds.
   static_assert(psio::packs_as<psio::shared_pssz<Rec>, Rec>);
   static_assert(psio::packs_as<psio::pssz_buffer<Rec>, Rec>);
   static_assert(psio::packs_as<psio::nested<Rec>, Rec>);
   static_assert(psio::packs_as<Rec, psio::shared_pssz<Rec>>);
   static_assert(psio::packs_as<psio::shared_pssz<Rec>, psio::nested<Rec>>);
   static_assert(psio::packs_as<psio::shared_pssz<RecView>, Rec>);
   // ... and as a member it is a packed value, which a plain one is not.
   static_assert(psio::packs_as<std::tuple<psio::shared_pssz<Rec>>, std::tuple<psio::nested<Rec>>>);
   static_assert(!psio::packs_as<std::tuple<psio::shared_pssz<Rec>>, std::tuple<Rec>>);

   // ── What does not ─────────────────────────────────────────────────────
   static_assert(!psio::packs_as<Wrong, Rec>);
   static_assert(!psio::packs_as<Short, Rec>);    // fewer members
   static_assert(!psio::packs_as<Longer, Rec>);   // more members
   static_assert(!psio::packs_as<Rec, Longer>);
   static_assert(!psio::packs_as<Narrow, Rec>);   // another integer
   static_assert(!psio::packs_as<Frozen, Rec>);   // no header
   static_assert(!psio::packs_as<Capped, Rec>);   // another offset width
   static_assert(!psio::packs_as<TagsLoose, Tags>);  // other bounds
   static_assert(!psio::packs_as<std::uint32_t, std::uint64_t>);
   static_assert(!psio::packs_as<std::int32_t, std::uint32_t>);
   static_assert(!psio::packs_as<std::string, std::vector<char>>);
   static_assert(!psio::packs_as<std::tuple<std::uint64_t, Trace>, Rec>);  // a tuple is not a record
   static_assert(!psio::packs_as<std::array<std::int32_t, 2>, Point>);
   static_assert(!psio::packs_as<std::vector<Wrong>, std::vector<Rec>>);
   static_assert(!psio::packs_as<std::optional<Rec>, Rec>);

   // ── to_pssz_as<T> takes exactly the sources of T ──────────────────────
   using Args = psio::reflect<Svc>::method_arg_tuple<0>;
   static_assert(psio::pssz_source_of<RecView, Rec>);
   static_assert(!psio::pssz_source_of<Wrong, Rec>);
   // An annotated tuple is written from the tuple of its elements.
   static_assert(psio::pssz_source_of<std::tuple<const RecView&, std::uint32_t>, Args>);
   static_assert(psio::pssz_source_of<Args, Args>);
   static_assert(!psio::pssz_source_of<std::tuple<Wrong, std::uint32_t>, Args>);
   static_assert(!psio::pssz_source_of<std::tuple<RecView>, Args>);
   // A proxy call.
   static_assert(psio::method_arguments<Svc, 0, RecView, int>);
   static_assert(psio::method_arguments<Svc, 0, psio::shared_pssz<Rec>&, int>);
   static_assert(!psio::method_arguments<Svc, 0, Wrong, int>);

   // What psibase's Table<T>::put(const U&) is: constrained at compile time.
   template <typename T, typename U>
      requires psio::packs_as<U, T>
   std::vector<char> put(const U& row)
   {
      return psio::to_pssz(row);
   }
   template <typename T, typename U>
   concept can_put = requires(const U& u) { put<T>(u); };
   static_assert(can_put<Rec, RecView>);
   static_assert(can_put<Rec, psio::pssz_view<Rec>>);
   static_assert(!can_put<Rec, Wrong>);

   // The run-time meaning of a true answer.
   template <typename T, typename U>
   void same_bytes(const U& source, const T& value)
   {
      static_assert(psio::packs_as<U, T>);
      const auto want = psio::to_pssz(value);
      REQUIRE(psio::to_pssz(source) == want);
      REQUIRE((psio::to_pssz_as<T>(source) == want));
      REQUIRE((psio::pssz_size_as<T>(source) == want.size()));
      REQUIRE(psio::validate_pssz<T>(psio::to_pssz(source)).ok());
   }
}  // namespace

TEST_CASE("packs_as: a true answer means the same bytes", "[pssz][packs_as]")
{
   const Trace trace{3, "e"};
   const Rec   rec{9, trace};
   const auto  tb = psio::to_pssz(trace);
   const auto  rb = psio::to_pssz(rec);
   std::uint64_t id = 9;

   same_bytes<Rec>(rec, rec);
   same_bytes<Rec>(RecView{9, psio::pssz_view<Trace>{tb}}, rec);
   same_bytes<Rec>(RecRef{9, std::cref(trace)}, rec);
   same_bytes<Rec>(RecDeep{9, TraceText{3, "e"}}, rec);
   same_bytes<Rec>(psio::pssz_view<Rec>{rb}, rec);
   same_bytes<Rec>(std::cref(rec), rec);
   same_bytes<Rec>(psio::shared_pssz<Rec>{rec}, rec);
   same_bytes<Rec>(psio::pssz_buffer<Rec>{rec}, rec);
   same_bytes<Rec>(psio::nested<Rec>{rec}, rec);
   same_bytes<std::tuple<std::uint64_t, Trace>>(std::tie(id, trace), std::tuple{id, trace});
   same_bytes<std::vector<Rec>>(
      std::vector<RecView>{{9, psio::pssz_view<Trace>{tb}}, {9, psio::pssz_view<Trace>{tb}}},
      std::vector<Rec>{rec, rec});
   same_bytes<std::optional<Rec>>(std::optional<RecDeep>{RecDeep{9, TraceText{3, "e"}}},
                                  std::optional<Rec>{rec});
   same_bytes<std::variant<std::uint8_t, Rec>>(
      std::variant<std::uint8_t, RecView>{RecView{9, psio::pssz_view<Trace>{tb}}},
      std::variant<std::uint8_t, Rec>{rec});
   same_bytes<std::string>(std::string_view{"abc"}, std::string{"abc"});

   std::vector<std::string_view> tags{"a", "bb"};
   same_bytes<Tags>(TagsView{tags}, Tags{{"a", "bb"}});

   REQUIRE(put<Rec>(RecView{9, psio::pssz_view<Trace>{tb}}) == rb);
}

TEST_CASE("packs_as: sources of a record as method arguments", "[pssz][packs_as][method]")
{
   const Trace trace{3, "e"};
   const Rec   rec{9, trace};
   const auto  tb   = psio::to_pssz(trace);
   const auto  want = psio::to_pssz(Args{rec, 5u});

   REQUIRE((psio::method_args_to_pssz<Svc, 0>(rec, 5) == want));
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(RecView{9, psio::pssz_view<Trace>{tb}}, 5) == want));
   REQUIRE((psio::method_args_to_pssz<Svc, 0>(RecDeep{9, TraceText{3, "e"}}, 5u) == want));
   const RecView view{9, psio::pssz_view<Trace>{tb}};
   REQUIRE((psio::to_pssz_as<Args>(std::forward_as_tuple(view, 5u)) == want));
   REQUIRE(psio::validate_method_args<Svc, 0>(want).ok());

   // A list of string views for a list of strings.
   std::vector<std::string_view> tags{"a", "bb"};
   REQUIRE((psio::method_args_to_pssz<Svc, 1>(tags) ==
            psio::to_pssz(psio::reflect<Svc>::method_arg_tuple<1>{{"a", "bb"}})));
}

// ── Recursive records ────────────────────────────────────────────────────
//
// A record that contains itself: directly (a list of itself), and through a
// list of variants whose alternative is the record (psibase's ActionTrace /
// InnerTrace). packs_as, pssz_evolves_from and everything built on them
// (to_pssz_as, method arguments, a Table<T>::put(U) constraint) compare
// such types.
namespace
{
   struct Node
   {
      std::string       name;
      std::vector<Node> children;
      friend bool       operator==(const Node&, const Node&) = default;
   };
   PSIO_REFLECT(Node, name, children)
   struct NodeRef
   {
      std::string_view     name;
      std::vector<NodeRef> children;
   };
   PSIO_REFLECT(NodeRef, name, children)
   // Not the same layout, at depth: the count is of another size.
   struct NodeWide
   {
      std::string           name;
      std::vector<NodeWide> children;
      std::uint64_t         extra = 0;
   };
   PSIO_REFLECT(NodeWide, name, children, extra)
   // Node with a trailing optional: it evolves from Node, and does not pack
   // as it.
   struct Node2
   {
      std::string                  name;
      std::vector<Node2>           children;
      std::optional<std::uint32_t> weight;
   };
   PSIO_REFLECT(Node2, name, children, weight)

   struct Console
   {
      std::string text;
      friend bool operator==(const Console&, const Console&) = default;
   };
   PSIO_REFLECT(Console, text)
   struct Inner;
   struct Action
   {
      std::uint64_t         sender = 0;
      std::vector<char>     raw;
      std::vector<Inner>    inner;
      std::optional<std::string> error;
      friend bool           operator==(const Action&, const Action&) = default;
   };
   struct Inner
   {
      std::variant<Console, Action> inner;
      friend bool operator==(const Inner&, const Inner&) = default;
   };
   PSIO_REFLECT(Action, sender, raw, inner, error)
   PSIO_REFLECT(Inner, inner)

   struct InnerRef;
   struct ActionRef
   {
      std::uint64_t                   sender = 0;
      std::span<const char>           raw;
      std::vector<InnerRef>           inner;
      std::optional<std::string_view> error;
   };
   struct ConsoleRef
   {
      std::string_view text;
   };
   PSIO_REFLECT(ConsoleRef, text)
   struct InnerRef
   {
      std::variant<ConsoleRef, ActionRef> inner;
   };
   PSIO_REFLECT(ActionRef, sender, raw, inner, error)
   PSIO_REFLECT(InnerRef, inner)

   // A row that holds the recursive record, and the same by reference.
   struct Success
   {
      std::uint64_t id = 0;
      Action        trace;
   };
   PSIO_REFLECT(Success, id, trace)
   struct SuccessRef
   {
      std::uint64_t id = 0;
      ActionRef     trace;
   };
   PSIO_REFLECT(SuccessRef, id, trace)
   struct SuccessView
   {
      std::uint64_t            id = 0;
      psio::pssz_view<Action>  trace;
   };
   PSIO_REFLECT(SuccessView, id, trace)

   struct Tracer
   {
      void record(Action trace, std::uint32_t n);
      PSIO_REFLECT(Tracer, method(record, trace, n))
   };

   static_assert(psio::packs_as<Node, Node>);
   static_assert(psio::packs_as<NodeRef, Node>);
   static_assert(psio::packs_as<Node, NodeRef>);
   static_assert(psio::packs_as<std::vector<NodeRef>, std::vector<Node>>);
   static_assert(!psio::packs_as<NodeWide, Node>);
   static_assert(!psio::packs_as<Node, NodeWide>);
   static_assert(!psio::packs_as<Node2, Node>);
   static_assert(psio::pssz_evolves_from<Node, Node2>);
   static_assert(!psio::pssz_evolves_from<Node2, Node>);
   static_assert(psio::pssz_evolves_from<Node, Node>);

   static_assert(psio::packs_as<ActionRef, Action>);
   static_assert(psio::packs_as<Action, ActionRef>);
   static_assert(psio::packs_as<InnerRef, Inner>);
   static_assert(psio::packs_as<SuccessRef, Success>);
   static_assert(psio::packs_as<SuccessView, Success>);
   static_assert(!psio::packs_as<Action, Node>);
   static_assert(!psio::packs_as<InnerRef, Action>);
   static_assert(psio::pssz_source_of<ActionRef, Action>);
}  // namespace

TEST_CASE("packs_as: recursive records", "[pssz][packs_as][recursive]")
{
   const NodeRef ref{"a", {NodeRef{"b", {}}, NodeRef{"c", {NodeRef{"d", {}}}}}};
   const Node    node{"a", {Node{"b", {}}, Node{"c", {Node{"d", {}}}}}};
   same_bytes<Node>(ref, node);
   REQUIRE(put<Node>(ref) == psio::to_pssz(node));

   const Action action{
      1, {'x', 'y'},
      {Inner{Console{"hello"}}, Inner{Action{2, {}, {Inner{Console{"deep"}}}, "failed"}}},
      std::nullopt};
   const std::string_view hello = "hello", deep = "deep", failed = "failed";
   const std::vector<char> raw{'x', 'y'};
   ActionRef               inner_action{2, {}, {}, failed};
   inner_action.inner.push_back(InnerRef{ConsoleRef{deep}});
   ActionRef action_ref{1, raw, {}, std::nullopt};
   action_ref.inner.push_back(InnerRef{ConsoleRef{hello}});
   action_ref.inner.push_back(InnerRef{inner_action});
   same_bytes<Action>(action_ref, action);
   REQUIRE(psio::from_pssz<Action>(psio::to_pssz(action_ref)) == action);

   const auto trace_bytes = psio::to_pssz(action);
   const auto want        = psio::to_pssz(Success{7, action});
   REQUIRE(put<Success>(SuccessRef{7, action_ref}) == want);
   REQUIRE(put<Success>(SuccessView{7, psio::pssz_view<Action>{trace_bytes}}) == want);
   // As a method argument: written as it stands.
   REQUIRE((psio::method_args_to_pssz<Tracer, 0>(action_ref, 5) ==
            psio::method_args_to_pssz<Tracer, 0>(action, 5)));
}

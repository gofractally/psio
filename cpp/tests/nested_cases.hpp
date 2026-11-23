// psio::nested<T> through every status-returning entry point of the core
// formats. Shared by a Catch test and by an executable built with
// exceptions and RTTI disabled (where a throw would be an abort): no case
// here may throw or abort, on valid or on hostile input.
//
// run() returns the number of checks that did not hold (each is printed).

#pragma once

#include <psio/annotate.hpp>
#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/pssz_view.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace nested_cases
{
   struct Transfer
   {
      std::string                to;
      std::uint64_t              amount = 0;
      std::optional<std::string> memo;
      friend bool operator==(const Transfer&, const Transfer&) = default;
      PSIO_REFLECT(Transfer, to, amount, memo)
   };
   // What a newer writer of Transfer sends.
   struct TransferV2
   {
      std::string                  to;
      std::uint64_t                amount = 0;
      std::optional<std::string>   memo;
      std::optional<std::uint32_t> fee;
      PSIO_REFLECT(TransferV2, to, amount, memo, fee)
   };
   static_assert(psio::pssz_evolves_from<Transfer, TransferV2>);

   // The same member three ways: typed, typed with deferred validation, and
   // the bytes another party sees.
   struct Action
   {
      std::uint64_t          sender = 0;
      std::uint32_t          method = 0;
      psio::nested<Transfer> args;
      friend bool operator==(const Action&, const Action&) = default;
      PSIO_REFLECT(Action, sender, method, args)
   };
   struct ActionV2
   {
      std::uint64_t            sender = 0;
      std::uint32_t            method = 0;
      psio::nested<TransferV2> args;
      PSIO_REFLECT(ActionV2, sender, method, args)
   };
   struct DeferredAction
   {
      std::uint64_t                                                sender = 0;
      std::uint32_t                                                method = 0;
      psio::nested<Transfer, psio::nested_validation::deferred> args;
      PSIO_REFLECT(DeferredAction, sender, method, args)
   };
   struct RawAction
   {
      std::uint64_t             sender = 0;
      std::uint32_t             method = 0;
      std::vector<std::uint8_t> args;
      PSIO_REFLECT(RawAction, sender, method, args)
   };

   // Every position a nested value can take.
   struct Shapes
   {
      psio::nested<std::uint32_t>                                 scalar;
      psio::nested<std::string>                                   text;
      psio::nested<psio::nested<Transfer>>                        twice;
      std::optional<psio::nested<Transfer>>                       maybe;
      std::vector<psio::nested<Transfer>>                         many;
      std::variant<std::uint8_t, psio::nested<Transfer>>          either;
      std::tuple<std::uint8_t, psio::nested<std::string>>         tuple;
      friend bool operator==(const Shapes&, const Shapes&) = default;
      PSIO_REFLECT(Shapes, scalar, text, twice, maybe, many, either, tuple)
   };
   struct RawShapes
   {
      std::vector<std::uint8_t>                               scalar;
      std::vector<std::uint8_t>                               text;
      std::vector<std::uint8_t>                               twice;
      std::optional<std::vector<std::uint8_t>>                maybe;
      std::vector<std::vector<std::uint8_t>>                  many;
      std::variant<std::uint8_t, std::vector<std::uint8_t>>   either;
      std::tuple<std::uint8_t, std::vector<std::uint8_t>>     tuple;
      PSIO_REFLECT(RawShapes, scalar, text, twice, maybe, many, either, tuple)
   };

   // A member length_bound counts the bytes of the inner encoding.
   struct Capped
   {
      psio::nested<std::string>                note;
      std::optional<psio::nested<std::string>> maybe;
      PSIO_REFLECT(Capped, attr(note, max<8>), attr(maybe, max<4>))
   };
   struct RawCapped
   {
      std::vector<std::uint8_t>                note;
      std::optional<std::vector<std::uint8_t>> maybe;
      PSIO_REFLECT(RawCapped, attr(note, max<8>), attr(maybe, max<4>))
   };
   static_assert(psio::max_encoded_size<Capped>() == psio::max_encoded_size<RawCapped>());
   static_assert(psio::auto_pssz_width_v<Capped> == psio::auto_pssz_width_v<RawCapped>);
   static_assert(psio::auto_pssz_width_v<Capped> == 1);

   // The same, where the inner value's own length and its encoded byte count
   // differ. A record has no length of its own (Point is 9 bytes); a list of
   // u16 has half as many elements as bytes. The schema IR names the member
   // type through an alias ("nested_Point"), so the schema-driven codec has
   // to see the Nested node behind it.
   struct Point
   {
      std::uint32_t x = 0;
      std::uint32_t y = 0;
      PSIO_REFLECT(Point, x, y)
   };
   struct PointFits
   {
      psio::nested<Point>                at;
      std::optional<psio::nested<Point>> maybe;
      PSIO_REFLECT(PointFits, attr(at, max<9>), attr(maybe, max<9>))
   };
   struct PointOver
   {
      psio::nested<Point>                at;
      std::optional<psio::nested<Point>> maybe;
      PSIO_REFLECT(PointOver, attr(at, max<8>), attr(maybe, max<8>))
   };
   using Words = std::vector<std::uint16_t>;
   // Elements <= max < bytes: the bytes are over the bound.
   struct WordsMax
   {
      psio::nested<Words>                words;
      std::optional<psio::nested<Words>> maybe;
      PSIO_REFLECT(WordsMax, attr(words, max<5>), attr(maybe, max<5>))
   };
   // Elements < min <= bytes: the bytes satisfy the bound.
   struct WordsMin
   {
      psio::nested<Words>                words;
      std::optional<psio::nested<Words>> maybe;
      PSIO_REFLECT(WordsMin,
                   attr(words, length_bound{.min = 6}),
                   attr(maybe, length_bound{.min = 6}))
   };

   // A type that nests itself: depth across the nesting boundary.
   struct Chain
   {
      std::optional<psio::nested<Chain>> next;
      PSIO_REFLECT(Chain, next)
   };

   // Records that put a Chain one and two node levels down.
   struct Limit
   {
      Chain chain;
      PSIO_REFLECT(Limit, chain)
   };
   struct Over
   {
      Limit limit;
      PSIO_REFLECT(Over, limit)
   };

   // A key holding a nested value.
   struct Keyed
   {
      std::uint32_t          table = 0;
      psio::nested<Transfer> row;
      PSIO_REFLECT(Keyed, table, row)
   };
   struct RawKeyed
   {
      std::uint32_t             table = 0;
      std::vector<std::uint8_t> row;
      PSIO_REFLECT(RawKeyed, table, row)
   };

   // Width, size and evolution: a nested value is a byte list.
   static_assert(!psio::max_encoded_size<psio::nested<std::uint32_t>>().has_value());
   static_assert(psio::auto_pssz_width_v<psio::nested<std::uint32_t>> == 4);
   static_assert(!psio::max_encoded_size<Action>().has_value());
   static_assert(psio::pssz_evolves_from<psio::nested<Transfer>, psio::nested<TransferV2>>);
   static_assert(psio::pssz_evolves_from<Action, ActionV2>);
   static_assert(psio::pssz_evolves_from<Action, DeferredAction>);
   static_assert(!psio::pssz_evolves_from<psio::nested<Transfer>, psio::nested<std::string>>);
   static_assert(!psio::pssz_evolves_from<Action, RawAction>);
   static_assert(psio::detail::is_nested_v<psio::nested<int>>);
   static_assert(!psio::detail::is_nested_v<std::vector<char>>);

   using Bytes = std::vector<char>;

   inline int& failures()
   {
      static int count = 0;
      return count;
   }
#define NESTED_CHECK(...)                                                                \
   do                                                                                     \
   {                                                                                      \
      if (!(__VA_ARGS__))                                                                 \
      {                                                                                   \
         ++::nested_cases::failures();                                                    \
         std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);   \
      }                                                                                   \
   } while (false)

   template <typename T>
   Bytes pssz_of(const T& value)
   {
      Bytes out;
      NESTED_CHECK(psio::encode_pssz_checked(psio::pssz{}, value, out).ok());
      return out;
   }
   inline std::vector<std::uint8_t> octets(const Bytes& bytes)
   {
      return {bytes.begin(), bytes.end()};
   }
   template <typename T>
   bool fails_with(const psio::codec_status& status, T&& what)
   {
      return !status.ok() && status.error().what == std::string_view{what};
   }
   template <typename T>
   bool valid(std::span<const char> bytes)
   {
      return psio::validate<T>(psio::pssz{}, bytes).ok();
   }
   // Exact validation succeeds. Without exceptions a type whose strict
   // check needs materialisation (a tuple, a utf8_string, ...) reports
   // "unsupported" instead: that status is what must come back there.
   template <typename T>
   bool strict(std::span<const char> bytes)
   {
      const auto status = psio::validate_strict<T>(psio::pssz{}, bytes);
#if defined(__cpp_exceptions)
      return status.ok();
#else
      if (psio::detail::vstrict::can_validate_without_materialization<T>())
         return status.ok();
      return !status.ok() &&
             status.error().what ==
                std::string_view{"pssz: guest strict semantic validation unsupported"};
#endif
   }
   template <typename T>
   bool strict_rejects(std::span<const char> bytes)
   {
      const auto status = psio::validate_strict<T>(psio::pssz{}, bytes);
      return !status.ok() &&
             status.error().what !=
                std::string_view{"pssz: guest strict semantic validation unsupported"};
   }

   inline Transfer sample_transfer() { return Transfer{"bob", 5, std::string{"rent"}}; }

   // ── The value type ──────────────────────────────────────────────────────
   inline void value_type()
   {
      const Transfer transfer = sample_transfer();
      const Bytes    inner    = pssz_of(transfer);

      const auto n = psio::nested<Transfer>::from(transfer);
      NESTED_CHECK(Bytes(n.bytes().begin(), n.bytes().end()) == inner);
      NESTED_CHECK(n.size() == inner.size());
      NESTED_CHECK(n == psio::nested<Transfer>(transfer));
      NESTED_CHECK(n == psio::nested{transfer});
      NESTED_CHECK(n.validate().ok());
      NESTED_CHECK(n.validate_strict().ok());
      NESTED_CHECK(n.decode() == transfer);
      NESTED_CHECK(n.view().template get<0>().view_() == "bob");
      NESTED_CHECK(n.view().template get<1>() == 5u);

      // The default is the encoding of T{}.
      NESTED_CHECK(psio::nested<Transfer>{} == psio::nested<Transfer>::from(Transfer{}));
      NESTED_CHECK(psio::nested<std::string>{}.size() == 0);
      NESTED_CHECK(psio::nested<std::uint32_t>{}.size() == 4);

      // checked: valid bytes are adopted, anything else leaves `out` alone.
      psio::nested<Transfer> out;
      const auto             before = out;
      NESTED_CHECK(psio::nested<Transfer>::checked(std::span<const char>{inner}, out).ok());
      NESTED_CHECK(out == n);
      out = before;
      // (Cut inside the fixed region: a shorter last string would be valid.)
      Bytes truncated(inner.begin(), inner.begin() + 5);
      NESTED_CHECK(
         !psio::nested<Transfer>::checked(std::span<const char>{truncated}, out).ok());
      NESTED_CHECK(out == before);
      NESTED_CHECK(!psio::nested<Transfer>::checked(Bytes{1, 2, 3}, out).ok());
      NESTED_CHECK(out == before);
      NESTED_CHECK(psio::nested<Transfer>::checked(Bytes(inner), out).ok());
      NESTED_CHECK(out == n);

      // unchecked adopts anything; the holder validates.
      const auto bad = psio::nested<Transfer>::unchecked(Bytes{1, 2, 3});
      NESTED_CHECK(!bad.validate().ok());
      NESTED_CHECK(!bad.validate_strict().ok());
      Transfer decoded;
      NESTED_CHECK(!bad.decode_checked(decoded).ok());
      NESTED_CHECK(n.decode_checked(decoded).ok());
      NESTED_CHECK(decoded == transfer);

      // from_checked reports a value with no encoding.
      psio::nested<psio::utf8_string<>> text;
      const psio::utf8_string<>         not_utf8{std::string(1, char(0xff))};
      NESTED_CHECK(!psio::nested<psio::utf8_string<>>::from_checked(not_utf8, text).ok());
      NESTED_CHECK(text.size() == 0);

      // Unknown appended fields of a newer writer: valid, reported, and
      // rejected by the exact validation.
      const auto newer = psio::nested<Transfer>::unchecked(
         pssz_of(TransferV2{"bob", 5, std::string{"rent"}, 7u}));
      bool unknown = false;
      NESTED_CHECK(newer.validate(unknown).ok() && unknown);
      NESTED_CHECK(!newer.validate_strict().ok());
      NESTED_CHECK(newer.decode_checked(decoded, &unknown).ok() && unknown);
      NESTED_CHECK(decoded == transfer);

      // release gives the bytes up.
      auto moved = psio::nested<Transfer>::from(transfer);
      NESTED_CHECK(std::move(moved).release() == inner);
   }

   // ── pssz: the bytes of a byte list ──────────────────────────────────────
   inline Shapes sample_shapes()
   {
      const Transfer t = sample_transfer();
      Shapes         s;
      s.scalar = psio::nested<std::uint32_t>::from(0xdeadbeefu);
      s.text   = psio::nested<std::string>::from("hello");
      s.twice  = psio::nested<psio::nested<Transfer>>::from(psio::nested<Transfer>::from(t));
      s.maybe  = psio::nested<Transfer>::from(t);
      s.many   = {psio::nested<Transfer>::from(t), psio::nested<Transfer>::from(Transfer{})};
      s.either = psio::nested<Transfer>::from(t);
      s.tuple  = {7, psio::nested<std::string>::from("")};
      return s;
   }
   inline RawShapes raw_of(const Shapes& s)
   {
      RawShapes r;
      r.scalar = octets(Bytes(s.scalar.bytes().begin(), s.scalar.bytes().end()));
      r.text   = octets(Bytes(s.text.bytes().begin(), s.text.bytes().end()));
      r.twice  = octets(Bytes(s.twice.bytes().begin(), s.twice.bytes().end()));
      if (s.maybe)
         r.maybe = octets(Bytes(s.maybe->bytes().begin(), s.maybe->bytes().end()));
      for (const auto& n : s.many)
         r.many.push_back(octets(Bytes(n.bytes().begin(), n.bytes().end())));
      if (auto n = std::get_if<1>(&s.either))
         r.either = octets(Bytes(n->bytes().begin(), n->bytes().end()));
      else
         r.either = std::get<0>(s.either);
      r.tuple = {std::get<0>(s.tuple), octets(Bytes(std::get<1>(s.tuple).bytes().begin(),
                                                    std::get<1>(s.tuple).bytes().end()))};
      return r;
   }

   inline void pssz_bytes()
   {
      const Transfer transfer = sample_transfer();
      const Bytes    inner    = pssz_of(transfer);
      const Action   action{1, 2, psio::nested<Transfer>::from(transfer)};
      const Bytes    bytes = pssz_of(action);

      // Byte-for-byte the record with a vector<u8> member.
      NESTED_CHECK(bytes == pssz_of(RawAction{1, 2, octets(inner)}));
      NESTED_CHECK(bytes == psio::encode(psio::pssz{}, action));
      NESTED_CHECK(psio::size_of(psio::pssz{}, action) == bytes.size());
      NESTED_CHECK(valid<Action>(bytes) && strict<Action>(bytes));
      Action back;
      bool   unknown = true;
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bytes}, back,
                                             unknown)
                      .ok());
      NESTED_CHECK(!unknown && back == action);
      NESTED_CHECK(back.args.decode() == transfer);
      NESTED_CHECK(psio::decode<Action>(psio::pssz{}, std::span<const char>{bytes}) == action);

      // The other side reads the same bytes as a byte list, and the reverse.
      RawAction raw;
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bytes}, raw,
                                             unknown)
                      .ok());
      NESTED_CHECK(raw.args == octets(inner));

      // Every position, against the byte-list form.
      const Shapes shapes       = sample_shapes();
      const Bytes  shapes_bytes = pssz_of(shapes);
      NESTED_CHECK(shapes_bytes == pssz_of(raw_of(shapes)));
      NESTED_CHECK(valid<Shapes>(shapes_bytes) && strict<Shapes>(shapes_bytes));
      Shapes shapes_back;
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{shapes_bytes},
                                             shapes_back, unknown)
                      .ok());
      NESTED_CHECK(shapes_back == shapes);
      NESTED_CHECK(shapes_back.twice.decode().decode() == sample_transfer());

      // Explicit-width profiles: the member is the same byte list, and the
      // inner encoding stays in the automatic profile.
      const Bytes narrow = psio::encode(psio::pssz_<2>{}, action);
      NESTED_CHECK(narrow == psio::encode(psio::pssz_<2>{}, RawAction{1, 2, octets(inner)}));
      NESTED_CHECK(psio::validate<Action>(psio::pssz_<2>{}, std::span<const char>{narrow}).ok());
      NESTED_CHECK(
         psio::validate_strict<Action>(psio::pssz_<2>{}, std::span<const char>{narrow}).ok());
      NESTED_CHECK(psio::decode<Action>(psio::pssz_<2>{}, std::span<const char>{narrow}) ==
                   action);

      // The optional of a nested value carries a selector, as an optional
      // byte list does.
      NESTED_CHECK(pssz_of(std::optional<psio::nested<std::string>>{}) == Bytes{0});
      NESTED_CHECK(pssz_of(std::optional{psio::nested<std::string>::from("")}) == Bytes{1});

      // Views.
      const psio::view<Action, psio::pssz> view{std::span<const char>{bytes}};
      const auto                           args = view.template get<2>();
      NESTED_CHECK(Bytes(args.bytes().begin(), args.bytes().end()) == inner);
      NESTED_CHECK(args.get().template get<0>().view_() == "bob");
      NESTED_CHECK((*args).template get<1>() == 5u);
      const psio::view<Shapes, psio::pssz> sv{std::span<const char>{shapes_bytes}};
      NESTED_CHECK(sv.template get<0>().get().get() == 0xdeadbeefu);
      NESTED_CHECK(sv.template get<1>().get().view_() == "hello");
      NESTED_CHECK(sv.template get<2>().get().get().template get<0>().view_() == "bob");
      NESTED_CHECK((*sv.template get<3>()).get().template get<1>() == 5u);
      NESTED_CHECK(sv.template get<4>()[0].get().template get<0>().view_() == "bob");
   }

   // ── pssz: validation descends ───────────────────────────────────────────
   inline void pssz_validation()
   {
      const Bytes inner = pssz_of(sample_transfer());
      auto        with  = [](Bytes args) { return pssz_of(RawAction{1, 2, octets(args)}); };
      bool        unknown = false;
      Action      out;

      // Inner bytes that are not a Transfer.
      const Bytes garbage = with(Bytes{1, 2, 3});
      NESTED_CHECK(valid<RawAction>(garbage) && !valid<Action>(garbage));
      NESTED_CHECK(strict_rejects<Action>(garbage));
      NESTED_CHECK(!psio::validate_pssz_compatible<Action>(psio::pssz{},
                                                           std::span<const char>{garbage},
                                                           unknown)
                       .ok());
      NESTED_CHECK(!psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{garbage}, out,
                                              unknown)
                       .ok());
      NESTED_CHECK(
         !psio::validate<Action>(psio::pssz_<4>{}, std::span<const char>{garbage}).ok());

      // Empty inner bytes are not a Transfer either (a record has a header).
      NESTED_CHECK(!valid<Action>(with({})));

      // Trailing bytes after the inner value. (A record's last variable
      // member owns the rest of its span, so the case is a fixed-size
      // inner value.)
      {
         RawShapes r   = raw_of(sample_shapes());
         r.scalar.push_back(0);
         NESTED_CHECK(valid<RawShapes>(pssz_of(r)) && !valid<Shapes>(pssz_of(r)));
      }

      // Inner unknown fields: accepted and reported by the compatible
      // readers, rejected by the exact one.
      const Bytes newer = with(pssz_of(TransferV2{"bob", 5, std::string{"rent"}, 7u}));
      NESTED_CHECK(valid<Action>(newer));
      NESTED_CHECK(psio::validate_pssz_compatible<Action>(psio::pssz{},
                                                          std::span<const char>{newer}, unknown)
                      .ok() &&
                   unknown);
      NESTED_CHECK(fails_with(
         psio::validate_strict<Action>(psio::pssz{}, std::span<const char>{newer}),
         "pssz: unknown fields in exact validation"));
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{newer}, out,
                                             unknown)
                      .ok() &&
                   unknown);
      NESTED_CHECK(out.args.decode() == sample_transfer());
      // The decoded member still holds the writer's bytes.
      NESTED_CHECK(pssz_of(out) == newer);

      // A raw inner string is any bytes, in strict mode too.
      const Bytes raw_text = with(pssz_of(Transfer{std::string(1, char(0xff)), 1, {}}));
      NESTED_CHECK(valid<Action>(raw_text) && !strict_rejects<Action>(raw_text));

      // A nested value two levels down is checked the same way.
      {
         RawShapes r = raw_of(sample_shapes());
         r.twice     = octets(pssz_of(octets(Bytes{9, 9})));
         NESTED_CHECK(valid<RawShapes>(pssz_of(r)) && !valid<Shapes>(pssz_of(r)));
         r.twice = octets(pssz_of(octets(pssz_of(TransferV2{"a", 1, {}, 2u}))));
         NESTED_CHECK(valid<Shapes>(pssz_of(r)) && strict_rejects<Shapes>(pssz_of(r)));
         r       = raw_of(sample_shapes());
         r.many.push_back(octets(Bytes{0x7f}));
         NESTED_CHECK(valid<RawShapes>(pssz_of(r)) && !valid<Shapes>(pssz_of(r)));
         r        = raw_of(sample_shapes());
         r.either = octets(Bytes{0x7f});
         NESTED_CHECK(!valid<Shapes>(pssz_of(r)));
         r       = raw_of(sample_shapes());
         r.maybe = octets(Bytes{0x7f});
         NESTED_CHECK(!valid<Shapes>(pssz_of(r)));
         r       = raw_of(sample_shapes());
         r.tuple = {1, octets(Bytes(3, 'x'))};
         NESTED_CHECK(valid<Shapes>(pssz_of(r)));
      }

      // Deferred: the enclosing readers treat the bytes as opaque; the
      // holder validates.
      NESTED_CHECK(valid<DeferredAction>(garbage) && strict<DeferredAction>(garbage));
      NESTED_CHECK(strict<DeferredAction>(newer));
      DeferredAction deferred;
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{garbage},
                                             deferred, unknown)
                      .ok() &&
                   !unknown);
      NESTED_CHECK(!deferred.args.validate().ok());
      NESTED_CHECK(pssz_of(deferred) == garbage);
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{with(inner)},
                                             deferred, unknown)
                      .ok());
      NESTED_CHECK(deferred.args.validate().ok() &&
                   deferred.args.decode() == sample_transfer());

      // A member length_bound counts the bytes of the inner encoding.
      Capped capped;
      capped.note = psio::nested<std::string>::from("12345678");
      NESTED_CHECK(valid<Capped>(pssz_of(capped)));
      capped.maybe = psio::nested<std::string>::from("1234");
      NESTED_CHECK(valid<Capped>(pssz_of(capped)));
      Bytes ignored;
      capped.note = psio::nested<std::string>::from("123456789");
      NESTED_CHECK(fails_with(psio::encode_pssz_checked(psio::pssz{}, capped, ignored),
                              "pssz: member length_bound violated"));
      NESTED_CHECK(fails_with(
         psio::validate<Capped>(
            psio::pssz{}, std::span<const char>{psio::detail::pssz_impl::
                                                   encode_ignoring_length_bounds(psio::pssz{},
                                                                                 capped)}),
         "pssz: member length_bound violated"));
      capped.note  = psio::nested<std::string>::from("1");
      capped.maybe = psio::nested<std::string>::from("12345");
      NESTED_CHECK(!psio::encode_pssz_checked(psio::pssz{}, capped, ignored).ok());
      NESTED_CHECK(!valid<Capped>(
         psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, capped)));
   }

   // ── pssz: node depth across the nesting boundary ────────────────────────
   //
   // A Chain is a record at depth d; its optional field is at d + 1, the
   // nested value (the Some payload) at d + 2, and the inner record, one
   // level below the nested member, at d + 3.
   inline Chain chain_value(std::size_t links)
   {
      Chain chain;
      for (std::size_t i = 0; i < links; ++i)
      {
         Chain outer;
         outer.next = psio::nested<Chain>::from(chain);
         chain      = std::move(outer);
      }
      return chain;
   }
   inline void pssz_depth()
   {
      static_assert(psio::kMaxValidationDepth == 64);
      // 21 links: the innermost record is at depth 63. One record around it
      // puts it at 64, the deepest node accepted; two, at 65.
      const Chain chain    = chain_value(21);
      const Bytes at_limit = pssz_of(Limit{chain});
      // The encoder does not walk the bytes a nested value holds, so it
      // writes this frame; every reader counts the levels and rejects it.
      const Bytes over = pssz_of(Over{Limit{chain}});
      NESTED_CHECK(valid<Chain>(pssz_of(chain)));
      NESTED_CHECK(valid<Limit>(at_limit) && strict<Limit>(at_limit));
      NESTED_CHECK(fails_with(psio::validate<Over>(psio::pssz{}, std::span<const char>{over}),
                              "pssz: max depth exceeded"));
      NESTED_CHECK(fails_with(
         psio::validate_strict<Over>(psio::pssz{}, std::span<const char>{over}),
         "pssz: max depth exceeded"));
      Over  out;
      Limit limit_out;
      bool  unknown = false;
      NESTED_CHECK(
         !psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{over}, out, unknown)
             .ok());
      NESTED_CHECK(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{at_limit},
                                             limit_out, unknown)
                      .ok());
      // The nested member itself must leave room for the inner root.
      {
         // tuple nesting puts the member at depth 64: no level is left.
         // (Reached through the walkers' entry points: 64 wrappers around
         // a member are not worth spelling.)
         auto status = psio::detail::pssz_impl::check_encode_depth_impl(
            psio::nested<std::string>{}, psio::kMaxValidationDepth);
         NESTED_CHECK(fails_with(status, "pssz: max depth exceeded"));
         status = psio::detail::pssz_impl::check_encode_depth_impl(
            psio::nested<std::string>{}, psio::kMaxValidationDepth - 1);
         NESTED_CHECK(status.ok());
         // Deferred: a list of bytes, whose elements are the level below.
         using D = psio::nested<std::string, psio::nested_validation::deferred>;
         NESTED_CHECK(psio::detail::pssz_impl::check_encode_depth_impl(
                         D{}, psio::kMaxValidationDepth)
                         .ok());
         NESTED_CHECK(!psio::detail::pssz_impl::check_encode_depth_impl(
                          D::from("x"), psio::kMaxValidationDepth)
                          .ok());
         namespace pi = psio::detail::pssz_impl;
         const Bytes empty;
         NESTED_CHECK(!pi::validate_value<4, true, psio::nested<std::string>>(
                          empty, 0, 0, psio::kMaxValidationDepth)
                          .ok());
         NESTED_CHECK(pi::validate_value<4, true, psio::nested<std::string>>(
                         empty, 0, 0, psio::kMaxValidationDepth - 1)
                         .ok());
         NESTED_CHECK(pi::validate_value<4, true, D>(empty, 0, 0, psio::kMaxValidationDepth).ok());
      }
      // The schema-driven validator counts the same levels.
      const auto schema =
         psio::SchemaBuilder{}.insert<Limit>("Limit").insert<Over>("Over").build();
      NESTED_CHECK(psio::pssz_ir::validate_pssz(schema, "Limit", at_limit).ok());
      NESTED_CHECK(!psio::pssz_ir::validate_pssz(schema, "Over", over).ok());
      // ...and does not count what it does not open.
      NESTED_CHECK(psio::pssz_ir::validate_pssz(schema, "Over", over, nullptr,
                                                nullptr, psio::pssz_ir::nested_bytes::opaque)
                      .ok());
   }

   // ── key ─────────────────────────────────────────────────────────────────
   inline void key_format()
   {
      const Bytes inner = pssz_of(sample_transfer());
      const Keyed keyed{7, psio::nested<Transfer>::from(sample_transfer())};
      const auto  key = psio::to_key(keyed);
      NESTED_CHECK(Bytes(key.begin(), key.end()) ==
                   [&] {
                      const auto raw = psio::to_key(RawKeyed{7, octets(inner)});
                      return Bytes(raw.begin(), raw.end());
                   }());
      NESTED_CHECK(psio::size_of(psio::key{}, keyed) == key.size());
      const std::span<const char> span{key.data(), key.size()};
      NESTED_CHECK(psio::validate<Keyed>(psio::key{}, span).ok());
      NESTED_CHECK(psio::validate_strict<Keyed>(psio::key{}, span).ok());

      // A key whose bytes are not the inner type.
      const auto bad = psio::to_key(RawKeyed{7, {1, 2, 3}});
      const std::span<const char> bad_span{bad.data(), bad.size()};
      NESTED_CHECK(psio::validate<RawKeyed>(psio::key{}, bad_span).ok());
      NESTED_CHECK(fails_with(psio::validate<Keyed>(psio::key{}, bad_span),
                              "key: nested bytes are not a valid encoding of their type"));
      // Unknown inner fields: a value, but not under the exact validation.
      const auto newer =
         psio::to_key(RawKeyed{7, octets(pssz_of(TransferV2{"a", 1, {}, 2u}))});
      const std::span<const char> newer_span{newer.data(), newer.size()};
      NESTED_CHECK(psio::validate<Keyed>(psio::key{}, newer_span).ok());
      NESTED_CHECK(!psio::validate_strict<Keyed>(psio::key{}, newer_span).ok());
      // Deferred: any bytes.
      NESTED_CHECK(psio::validate<std::tuple<std::uint32_t,
                                             psio::nested<Transfer,
                                                          psio::nested_validation::deferred>>>(
                      psio::key{}, bad_span)
                      .ok());
      // Truncated framing.
      NESTED_CHECK(!psio::validate<Keyed>(psio::key{}, span.first(span.size() - 1)).ok());
   }

   // ── json / ijson ────────────────────────────────────────────────────────
   template <typename Fmt>
   void text_format(std::string_view expected)
   {
      const Action action{1, 2, psio::nested<Transfer>::from(sample_transfer())};
      std::string  text;
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, action, text).ok());
      NESTED_CHECK(text == expected);
      NESTED_CHECK(psio::validate<Action>(Fmt{}, std::span<const char>{text}).ok());
      NESTED_CHECK(psio::validate_strict<Action>(Fmt{}, std::span<const char>{text}).ok());
      Action back;
      NESTED_CHECK(psio::decode_json_checked(Fmt{}, std::span<const char>{text}, back).ok());
      NESTED_CHECK(back == action);

      // Every position.
      const Shapes shapes = sample_shapes();
      text.clear();
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, shapes, text).ok());
      Shapes shapes_back;
      NESTED_CHECK(
         psio::decode_json_checked(Fmt{}, std::span<const char>{text}, shapes_back).ok());
      NESTED_CHECK(shapes_back == shapes);

      // Text that is not the inner type.
      const std::string_view wrong = R"({"sender":1,"method":2,"args":"0A0B"})";
      NESTED_CHECK(!psio::validate<Action>(Fmt{}, std::span<const char>{wrong}).ok());
      NESTED_CHECK(!psio::decode_json_checked(Fmt{}, std::span<const char>{wrong}, back).ok());
      const std::string_view missing = R"({"sender":1,"method":2,"args":{"to":"bob"}})";
      NESTED_CHECK(!psio::validate<Action>(Fmt{}, std::span<const char>{missing}).ok());

      // Bytes that are not the inner type have no text.
      Action bad{1, 2, psio::nested<Transfer>::unchecked(Bytes{1, 2, 3})};
      text = "keep";
      NESTED_CHECK(fails_with(psio::encode_json_checked(Fmt{}, bad, text),
                              "json: nested bytes are not a valid encoding of their type"));
      NESTED_CHECK(text == "keep");

      // Text round trips are value round trips: a newer writer's appended
      // fields are not carried through text.
      Action newer{1, 2,
                   psio::nested<Transfer>::unchecked(
                      pssz_of(TransferV2{"bob", 5, std::string{"rent"}, 7u}))};
      text.clear();
      NESTED_CHECK(psio::encode_json_checked(Fmt{}, newer, text).ok());
      NESTED_CHECK(text == expected);
      NESTED_CHECK(psio::decode_json_checked(Fmt{}, std::span<const char>{text}, back).ok());
      NESTED_CHECK(back == action && !(back == newer));

      // A value that has no encoding is not a nested value, even when only
      // validating.
      using Text = std::tuple<psio::nested<Capped>>;
      const std::string_view long_note = R"([{"note":"123456789","maybe":null}])";
      Text                   parsed;
      NESTED_CHECK(!psio::validate<Text>(Fmt{}, std::span<const char>{long_note}).ok());
      NESTED_CHECK(
         !psio::decode_json_checked(Fmt{}, std::span<const char>{long_note}, parsed).ok());
      const std::string_view short_note = R"([{"note":"12345678","maybe":"1234"}])";
      NESTED_CHECK(psio::validate<Text>(Fmt{}, std::span<const char>{short_note}).ok());
      NESTED_CHECK(
         psio::decode_json_checked(Fmt{}, std::span<const char>{short_note}, parsed).ok());
      NESTED_CHECK(std::get<0>(parsed).decode().note.decode() == "12345678");
   }

   // ── Schema IR and the schema-driven codecs ──────────────────────────────
   inline void schema_driven()
   {
      namespace ir = psio::schema_types;
      namespace rt = psio::pssz_ir;
      const auto schema = psio::SchemaBuilder{}
                             .insert<Action>("Action")
                             .insert<DeferredAction>("DeferredAction")
                             .insert<Shapes>("Shapes")
                             .insert<Capped>("Capped")
                             .build();
      // The node: Nested{Transfer}; the validation policy is not recorded.
      auto member_type = [&](std::string_view type, std::string_view member) -> const ir::AnyType* {
         for (const auto& named : schema.types)
            if (named.name == type)
               if (auto object = std::get_if<ir::Object>(&named.type->value))
                  for (const auto& m : object->members)
                     if (m.name == member)
                        return m.type->resolve(schema);
         return nullptr;
      };
      const auto* args = member_type("Action", "args");
      NESTED_CHECK(args && std::holds_alternative<ir::Nested>(args->value));
      const auto* deferred = member_type("DeferredAction", "args");
      NESTED_CHECK(args && deferred && *args == *deferred);
      // A member length_bound is the lengthBound attribute, never a bounded
      // node.
      for (const auto& named : schema.types)
         if (named.name == "Capped")
         {
            const auto& members = std::get<ir::Object>(named.type->value).members;
            NESTED_CHECK(
               std::holds_alternative<ir::Nested>(members[0].type->resolve(schema)->value));
            NESTED_CHECK(members[0].attributes.size() == 1 &&
                         members[0].attributes[0].name == "lengthBound" &&
                         members[0].attributes[0].value == std::string{"max=8"});
            NESTED_CHECK(
               std::holds_alternative<ir::Option>(members[1].type->resolve(schema)->value));
            NESTED_CHECK(members[1].attributes.size() == 1 &&
                         members[1].attributes[0].value == std::string{"max=4"});
         }
      std::size_t width = 0;
      NESTED_CHECK(rt::auto_width(schema, "Capped", width).ok() && width == 1);

      const Action action{1, 2, psio::nested<Transfer>::from(sample_transfer())};
      const Bytes  bytes = pssz_of(action);

      // Validate, decode to the inner dynamic value, re-encode canonically.
      NESTED_CHECK(rt::validate_pssz(schema, "Action", bytes).ok());
      rt::Value value;
      bool      unknown = true;
      NESTED_CHECK(rt::decode_auto(schema, "Action", bytes, value, unknown).ok() && !unknown);
      const auto* record = std::get_if<rt::Record>(&value.value);
      NESTED_CHECK(record && record->size() == 3 &&
                   std::holds_alternative<rt::Record>((*record)[2].second.value));
      Bytes again;
      NESTED_CHECK(rt::encode_auto(schema, "Action", value, again).ok() && again == bytes);

      // Opaque: the bytes, untouched.
      NESTED_CHECK(rt::decode_auto(schema, "Action", bytes, value, unknown,
                                   nullptr, rt::nested_bytes::opaque)
                      .ok());
      record = std::get_if<rt::Record>(&value.value);
      NESTED_CHECK(record && std::holds_alternative<rt::Bytes>((*record)[2].second.value));
      again.clear();
      NESTED_CHECK(
         rt::encode_auto(schema, "Action", value, again, nullptr, rt::nested_bytes::opaque).ok() &&
         again == bytes);
      again.clear();
      NESTED_CHECK(!rt::encode_auto(schema, "Action", value, again).ok() && again.empty());

      // Rejects, and the opaque option.
      const Bytes garbage = pssz_of(RawAction{1, 2, {1, 2, 3}});
      NESTED_CHECK(!rt::validate_pssz(schema, "Action", garbage).ok());
      NESTED_CHECK(
         rt::validate_pssz(schema, "Action", garbage, nullptr, nullptr, rt::nested_bytes::opaque).ok());
      NESTED_CHECK(rt::validate_pssz(schema, "DeferredAction", garbage, nullptr,
                                     nullptr, rt::nested_bytes::opaque)
                      .ok());
      const Bytes newer =
         pssz_of(RawAction{1, 2, octets(pssz_of(TransferV2{"bob", 5, std::string{"rent"}, 7u}))});
      NESTED_CHECK(rt::validate_pssz(schema, "Action", newer, &unknown).ok() && unknown);
      NESTED_CHECK(!rt::decode_exact<0>(schema, "Action", newer, value).ok());
      // Unknown inner fields are dropped by the re-encode: not canonical.
      NESTED_CHECK(!rt::validate_canonical_for_width<0>(schema, "Action", newer).ok());
      NESTED_CHECK(rt::validate_canonical_for_width<0>(schema, "Action", bytes).ok());

      // Explicit-width profile around an automatic-profile inner value.
      const Bytes narrow = psio::encode(psio::pssz_<2>{}, action);
      NESTED_CHECK(rt::decode_exact<2>(schema, "Action", narrow, value).ok());
      again.clear();
      NESTED_CHECK(rt::encode<2>(schema, "Action", value, again).ok() && again == narrow);
      const Bytes narrow_newer = psio::encode(
         psio::pssz_<2>{},
         RawAction{1, 2, octets(pssz_of(TransferV2{"bob", 5, std::string{"rent"}, 7u}))});
      NESTED_CHECK(!rt::decode_exact<2>(schema, "Action", narrow_newer, value).ok());
      unknown = false;
      NESTED_CHECK(rt::decode_compatible<2>(schema, "Action", narrow_newer, value, unknown).ok() &&
                   unknown);

      // Text through the schema is the typed text, both ways.
      for (const bool ijson : {false, true})
      {
         std::string typed, dynamic;
         Bytes       from_text;
         if (ijson)
         {
            NESTED_CHECK(psio::encode_json_checked(psio::ijson{}, action, typed).ok());
            NESTED_CHECK(rt::pssz_to_json(schema, "Action", psio::ijson{}, bytes, dynamic).ok());
            NESTED_CHECK(
               rt::json_to_pssz(schema, "Action", psio::ijson{}, typed, from_text).ok());
         }
         else
         {
            NESTED_CHECK(psio::encode_json_checked(psio::json{}, action, typed).ok());
            NESTED_CHECK(rt::pssz_to_json(schema, "Action", psio::json{}, bytes, dynamic).ok());
            NESTED_CHECK(rt::json_to_pssz(schema, "Action", psio::json{}, typed, from_text).ok());
         }
         NESTED_CHECK(typed == dynamic);
         NESTED_CHECK(from_text == bytes);
      }
      std::string text;
      NESTED_CHECK(!rt::pssz_to_json(schema, "Action", psio::ijson{}, garbage, text).ok() &&
                   text.empty());

      // Every position.
      const Shapes shapes       = sample_shapes();
      const Bytes  shapes_bytes = pssz_of(shapes);
      std::string  shapes_text, shapes_typed;
      NESTED_CHECK(
         rt::pssz_to_json(schema, "Shapes", psio::ijson{}, shapes_bytes, shapes_text).ok());
      NESTED_CHECK(psio::encode_json_checked(psio::ijson{}, shapes, shapes_typed).ok());
      NESTED_CHECK(shapes_text == shapes_typed);
      again.clear();
      NESTED_CHECK(rt::json_to_pssz(schema, "Shapes", psio::ijson{}, shapes_text, again).ok() &&
                   again == shapes_bytes);

      // lengthBound on a nested member: the bytes of the inner encoding.
      Capped capped;
      capped.note  = psio::nested<std::string>::from("12345678");
      capped.maybe = psio::nested<std::string>::from("1234");
      const Bytes capped_bytes = pssz_of(capped);
      NESTED_CHECK(rt::validate_pssz(schema, "Capped", capped_bytes).ok());
      NESTED_CHECK(rt::decode_auto(schema, "Capped", capped_bytes, value, unknown).ok());
      again.clear();
      NESTED_CHECK(rt::encode_auto(schema, "Capped", value, again).ok() && again == capped_bytes);
      capped.note = psio::nested<std::string>::from("123456789");
      NESTED_CHECK(!rt::validate_pssz(
                       schema, "Capped",
                       psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{},
                                                                              capped))
                       .ok());
      again.clear();
      NESTED_CHECK(!rt::json_to_pssz(schema, "Capped", psio::ijson{},
                                     std::string_view{R"({"note":"123456789","maybe":null})"},
                                     again)
                       .ok() &&
                   again.empty());
      NESTED_CHECK(rt::json_to_pssz(schema, "Capped", psio::ijson{},
                                    std::string_view{R"({"note":"12345678","maybe":"1234"})"},
                                    again)
                      .ok() &&
                   again == capped_bytes);
      again.clear();
      NESTED_CHECK(!rt::json_to_pssz(schema, "Capped", psio::ijson{},
                                     std::string_view{R"({"note":"1","maybe":"12345"})"}, again)
                       .ok());
   }

   // ── lengthBound on a nested member counts encoded bytes ─────────────────
   //
   // Typed and schema-driven codecs must agree on every entry point where
   // the inner value's own length (none for a record, the element count for
   // a list) differs from the byte count of its encoding.
   template <typename T>
   void bound_agreement(const psio::schema_types::Schema& schema, const std::string& name,
                        const T& value, bool admitted)
   {
      namespace rt = psio::pssz_ir;
      const Bytes wire = psio::detail::pssz_impl::encode_ignoring_length_bounds(psio::pssz{}, value);
      Bytes       typed;
      // Typed: encode and validate.
      NESTED_CHECK(psio::encode_pssz_checked(psio::pssz{}, value, typed).ok() == admitted);
      NESTED_CHECK(valid<T>(wire) == admitted);
      if (admitted)
         NESTED_CHECK(typed == wire);
      // Schema-driven: validate, decode, encode of the decoded value.
      NESTED_CHECK(rt::validate_pssz(schema, name, wire).ok() == admitted);
      rt::Value decoded;
      bool      unknown = false;
      NESTED_CHECK(rt::decode_auto(schema, name, wire, decoded, unknown).ok() == admitted);
      // The dynamic value of the same data, built without the bound in the
      // way: the relaxed twin of the schema decodes the same bytes.
      rt::Value   free_value;
      const auto  twin = name + "Free";
      NESTED_CHECK(rt::decode_auto(schema, twin, wire, free_value, unknown).ok());
      Bytes again;
      NESTED_CHECK(rt::encode_auto(schema, name, free_value, again).ok() == admitted);
      if (admitted)
         NESTED_CHECK(again == wire);
      // JSON in: the text of the unbounded twin through the bounded schema.
      std::string text;
      NESTED_CHECK(rt::pssz_to_json(schema, twin, psio::ijson{}, wire, text).ok());
      again.clear();
      NESTED_CHECK(rt::json_to_pssz(schema, name, psio::ijson{}, std::string_view{text}, again)
                      .ok() == admitted);
      if (admitted)
         NESTED_CHECK(again == wire);
      std::string out_text;
      NESTED_CHECK(rt::pssz_to_json(schema, name, psio::ijson{}, wire, out_text).ok() ==
                   admitted);
   }
   // The schema of T under `name`, with a copy under name+"Free" whose
   // bounds admit every value used here.
   template <typename T>
   psio::schema_types::Schema bound_schema(std::string_view name)
   {
      namespace ir = psio::schema_types;
      auto        schema = psio::SchemaBuilder{}.insert<T>(std::string{name}).build();
      const auto  count  = schema.types.size();
      for (std::size_t i = 0; i < count; ++i)
         if (schema.types[i].name == name)
         {
            const auto& object = std::get<ir::Object>(schema.types[i].type->value);
            // The member is written as a named alias of the Nested node (or
            // of an Option of one), not as the node itself.
            NESTED_CHECK(std::holds_alternative<ir::Type>(object.members[0].type->value));
            NESTED_CHECK(
               std::holds_alternative<ir::Nested>(object.members[0].type->resolve(schema)->value));
            NESTED_CHECK(object.members[0].attributes.size() == 1 &&
                         object.members[0].attributes[0].name == "lengthBound");
            // Same offset width as the original (two members of at most
            // 100 bytes still take one-byte offsets; a minimum alone leaves
            // the record unbounded), nothing in the way.
            ir::Object free = object;
            for (auto& member : free.members)
               member.attributes[0].value =
                  std::string{member.attributes[0].value->starts_with("min") ? "min=0" : "max=100"};
            schema.types.push_back(ir::Member{
               std::string{name} + "Free",
               ir::Box<ir::AnyType>{ir::AnyType{std::move(free)}}, {}});
            break;
         }
      return schema;
   }
   inline void nested_bound_counts_bytes()
   {
      const auto point = psio::nested<Point>::from(Point{1, 2});   // 9 bytes, no length
      {
         const auto schema = bound_schema<PointFits>("PointFits");
         bound_agreement(schema, "PointFits", PointFits{point, {}}, true);
         bound_agreement(schema, "PointFits", PointFits{point, point}, true);
      }
      {
         const auto schema = bound_schema<PointOver>("PointOver");
         bound_agreement(schema, "PointOver", PointOver{point, {}}, false);
      }
      const auto three = psio::nested<Words>::from(Words{1, 2, 3});   // 3 elements, 6 bytes
      const auto two   = psio::nested<Words>::from(Words{1, 2});      // 2 elements, 4 bytes
      const auto five  = psio::nested<Words>::from(Words{1, 2, 3, 4, 5});
      {
         const auto schema = bound_schema<WordsMax>("WordsMax");
         bound_agreement(schema, "WordsMax", WordsMax{two, {}}, true);
         bound_agreement(schema, "WordsMax", WordsMax{two, two}, true);
         // 3 <= 5 elements but 6 > 5 bytes.
         bound_agreement(schema, "WordsMax", WordsMax{three, {}}, false);
         bound_agreement(schema, "WordsMax", WordsMax{two, three}, false);
      }
      {
         const auto schema = bound_schema<WordsMin>("WordsMin");
         // 3 < 6 elements but 6 >= 6 bytes.
         bound_agreement(schema, "WordsMin", WordsMin{three, {}}, true);
         bound_agreement(schema, "WordsMin", WordsMin{three, three}, true);
         bound_agreement(schema, "WordsMin", WordsMin{five, five}, true);
         bound_agreement(schema, "WordsMin", WordsMin{two, {}}, false);
         bound_agreement(schema, "WordsMin", WordsMin{three, two}, false);
      }
   }

   inline int run()
   {
      failures() = 0;
      value_type();
      pssz_bytes();
      pssz_validation();
      pssz_depth();
      key_format();
      text_format<psio::json>(
         R"({"sender":1,"method":2,"args":{"to":"bob","amount":5,"memo":"rent"}})");
      text_format<psio::ijson>(
         R"({"sender":1,"method":2,"args":{"to":"bob","amount":5,"memo":"rent"}})");
      schema_driven();
      nested_bound_counts_bytes();
      return failures();
   }
}  // namespace nested_cases

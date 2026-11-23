#pragma once
// psio::validate_strict<T>(psio::pssz{}, bytes) — "valid, canonical, and no
// unknown fields" — on types that need more than a structural walk: binary
// adapters, text adapters, invariants, transparent types, bounded members,
// order annotations, constructed (non-aggregate) records, nested values.
//
// One body for two binaries: pssz_strict_tests.cpp (Catch2, exceptions) and
// pssz_strict_noexceptions.cpp (-fno-exceptions -fno-rtti,
// PSIO_EXCEPTIONS_ENABLED=0). The answers are the same in both: strict
// validation returns a status and needs no exceptions.

#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strict_cases
{
   // The shape of psibase::AccountNumber: a definitionWillNotChange record
   // of one integer with a binary adapter and a text adapter.
   struct Name
   {
      std::uint64_t value = 0;
      friend bool   operator==(const Name&, const Name&) = default;
   };
   PSIO_REFLECT(Name, definitionWillNotChange(), value)
   struct NameBin
   {
      static std::size_t packsize(const Name&) { return 8; }
      static void        encode(const Name& n, std::vector<char>& o)
      {
         o.insert(o.end(), reinterpret_cast<const char*>(&n.value),
                  reinterpret_cast<const char*>(&n.value) + 8);
      }
      template <typename Sink>
      static void encode(const Name& n, Sink& s) { s.write(&n.value, 8); }
      static Name decode(std::span<const char> b)
      {
         Name n;
         std::memcpy(&n.value, b.data(), 8);
         return n;
      }
      static psio::codec_status validate(std::span<const char> b) noexcept
      {
         return b.size() == 8 ? psio::codec_ok() : psio::codec_fail("Name: size", 0, "Name");
      }
      static psio::codec_status validate_strict(std::span<const char> b) noexcept
      {
         return validate(b);
      }
   };
   struct NameText
   {
      static constexpr std::string_view schema_id = "Name";
      static std::size_t packsize(const Name& n) { return std::to_string(n.value).size(); }
      static void        encode(const Name& n, std::string& out) { out += std::to_string(n.value); }
      static Name        decode(std::span<const char> t)
      {
         std::uint64_t v = 0;
         for (char c : t) v = v * 10 + std::uint64_t(c - '0');
         return Name{v};
      }
      static psio::codec_status validate(std::span<const char> t) noexcept
      {
         for (char c : t)
            if (c < '0' || c > '9') return psio::codec_fail("Name: digit", 0, "Name");
         return psio::codec_ok();
      }
      static psio::codec_status validate_strict(std::span<const char> t) noexcept
      {
         return validate(t);
      }
   };
}  // namespace strict_cases
PSIO_ADAPTER(strict_cases::Name, psio::binary_category, strict_cases::NameBin)
PSIO_ADAPTER(strict_cases::Name, psio::text_category, strict_cases::NameText)

namespace strict_cases
{
   // An adapter whose strict check is narrower than its plain one: a value
   // of 4 bytes, where the plain check also takes the legacy 8-byte form.
   struct Code
   {
      std::uint32_t value = 0;
      friend bool   operator==(const Code&, const Code&) = default;
   };
   PSIO_REFLECT(Code, definitionWillNotChange(), value)
   struct CodeBin
   {
      static std::size_t packsize(const Code&) { return 4; }
      template <typename Sink>
      static void encode(const Code& c, Sink& s) { s.write(&c.value, 4); }
      static void encode(const Code& c, std::vector<char>& o)
      {
         o.insert(o.end(), reinterpret_cast<const char*>(&c.value),
                  reinterpret_cast<const char*>(&c.value) + 4);
      }
      static Code decode(std::span<const char> b)
      {
         Code c;
         std::memcpy(&c.value, b.data(), 4);
         return c;
      }
      static psio::codec_status validate(std::span<const char> b) noexcept
      {
         return b.size() == 4 || b.size() == 8 ? psio::codec_ok()
                                               : psio::codec_fail("Code: size", 0, "Code");
      }
      static psio::codec_status validate_strict(std::span<const char> b) noexcept
      {
         return b.size() == 4 ? psio::codec_ok()
                              : psio::codec_fail("Code: legacy form", 0, "Code");
      }
   };
   // An adapter that reads two spellings of one value and whose strict
   // check does not tell them apart: the re-encoding does.
   struct Flag
   {
      bool        on = false;
      friend bool operator==(const Flag&, const Flag&) = default;
   };
   PSIO_REFLECT(Flag, definitionWillNotChange(), on)
   struct FlagBin
   {
      static std::size_t packsize(const Flag&) { return 1; }
      template <typename Sink>
      static void encode(const Flag& f, Sink& s) { s.write(f.on ? 'Y' : 'N'); }
      static void encode(const Flag& f, std::vector<char>& o) { o.push_back(f.on ? 'Y' : 'N'); }
      static Flag decode(std::span<const char> b) { return Flag{b[0] == 'Y' || b[0] == 'y'}; }
      static psio::codec_status validate(std::span<const char> b) noexcept
      {
         return b.size() == 1 && (b[0] == 'Y' || b[0] == 'y' || b[0] == 'N')
                   ? psio::codec_ok()
                   : psio::codec_fail("Flag: letter", 0, "Flag");
      }
      static psio::codec_status validate_strict(std::span<const char> b) noexcept
      {
         return validate(b);
      }
   };
}  // namespace strict_cases
PSIO_ADAPTER(strict_cases::Code, psio::binary_category, strict_cases::CodeBin)
PSIO_ADAPTER(strict_cases::Flag, psio::binary_category, strict_cases::FlagBin)

namespace strict_cases
{
   // A transparent type with an invariant (psibase::Memo's C++ form).
   struct Memo
   {
      std::string contents;
      friend bool operator==(const Memo&, const Memo&) = default;
      static psio::codec_status wellFormed(const Memo& m) noexcept
      {
         return m.contents.size() <= 8 ? psio::codec_ok()
                                       : psio::codec_fail("Memo: too long", 0, "Memo");
      }
      PSIO_REFLECT(Memo, transparent(), invariant(wellFormed), contents)
   };
   // A record with an invariant across members.
   struct Range
   {
      std::uint32_t begin = 0;
      std::uint32_t end   = 0;
      friend bool   operator==(const Range&, const Range&) = default;
      static psio::codec_status ordered(const Range& r) noexcept
      {
         return r.begin <= r.end ? psio::codec_ok()
                                 : psio::codec_fail("Range: reversed", 0, "Range");
      }
      PSIO_REFLECT(Range, begin, end, invariant(ordered))
   };
   // Not an aggregate.
   class Counter
   {
      std::uint32_t n_ = 0;

    public:
      Counter() : n_(0) {}
      explicit Counter(std::uint32_t n) : n_(n) {}
      friend bool operator==(const Counter&, const Counter&) = default;
      PSIO_REFLECT(Counter, n_)
   };

   // psibase::Action in miniature, and what a later revision appends.
   struct Action
   {
      Name              sender;
      Name              service;
      std::vector<char> raw;
      friend bool       operator==(const Action&, const Action&) = default;
   };
   PSIO_REFLECT(Action, sender, service, raw)
   struct ActionV2
   {
      Name                       sender;
      Name                       service;
      std::vector<char>          raw;
      std::optional<std::string> note;
   };
   PSIO_REFLECT(ActionV2, sender, service, raw, note)

   struct Everything
   {
      Name                              owner;
      Code                              code;
      Flag                              flag;
      Memo                              memo;
      Range                             range;
      Counter                           counter;
      std::vector<Name>                 names;
      std::optional<Memo>               maybe;
      std::vector<std::string>          tags;
      std::vector<std::uint32_t>        ids;
      psio::utf8_string<16>             label;
      psio::nested<Action>              action;
      std::tuple<Name, std::string>     pair;
      std::variant<std::uint8_t, Name>  choice;
      friend bool operator==(const Everything&, const Everything&) = default;
      PSIO_REFLECT(Everything, owner, code, flag, memo, range, counter, names, maybe,
                   attr(tags, max<4> | element(utf8 | max<8>)),
                   attr(ids, unique),
                   label, action, pair, choice)
   };

   struct Holder
   {
      std::uint8_t tag = 0;
      Code         code;
      PSIO_REFLECT(Holder, definitionWillNotChange(), tag, code)
   };
   struct Tags
   {
      std::vector<std::string> tags;
      PSIO_REFLECT(Tags, attr(tags, max<2> | element(utf8 | max<4>)))
   };
   struct AnyTags
   {
      std::vector<std::string> tags;
      PSIO_REFLECT(AnyTags, tags)
   };
   struct Ids
   {
      std::vector<std::uint32_t> ids;
      PSIO_REFLECT(Ids, attr(ids, unique))
   };
   struct AnyIds
   {
      std::vector<std::uint32_t> ids;
      PSIO_REFLECT(AnyIds, ids)
   };
   struct Envelope
   {
      psio::nested<Action> action;
      PSIO_REFLECT(Envelope, action)
   };
   struct EnvelopeV2
   {
      psio::nested<ActionV2> action;
      PSIO_REFLECT(EnvelopeV2, action)
   };

   inline Everything sample()
   {
      Everything e;
      e.owner   = Name{7};
      e.code    = Code{9};
      e.flag    = Flag{true};
      e.memo    = Memo{"hi"};
      e.range   = Range{1, 5};
      e.counter = Counter{3};
      e.names   = {Name{1}, Name{2}};
      e.maybe   = Memo{"m"};
      e.tags    = {"a", "bb"};
      e.ids     = {1, 2, 9};
      e.label   = psio::utf8_string<16>{"label"};
      e.action  = psio::nested<Action>{Action{Name{1}, Name{2}, {1, 2, 3}}};
      e.pair    = {Name{4}, "p"};
      e.choice  = Name{11};
      return e;
   }

   inline bool is(const psio::codec_status& status, std::string_view reason)
   {
      return !status.ok() && status.error().what == reason;
   }
   template <typename T>
   psio::codec_status strict(const std::vector<char>& bytes)
   {
      return psio::validate_strict<T>(psio::pssz{}, std::span<const char>{bytes});
   }
   template <typename T>
   psio::codec_status plain(const std::vector<char>& bytes)
   {
      return psio::validate_pssz<T>(std::span<const char>{bytes});
   }

   // Returns 0, or the line of the first expectation that failed.
   inline int run()
   {
#define STRICT_EXPECT(cond)                                              \
   do                                                                    \
   {                                                                     \
      if (!(cond))                                                       \
      {                                                                  \
         std::printf("FAILED line %d: %s\n", __LINE__, #cond);           \
         return __LINE__;                                                \
      }                                                                  \
   } while (0)

      // The record of the gap report: an adapter member and bytes.
      {
         const Action a{Name{1}, Name{2}, {1, 2}};
         const auto   bytes = psio::to_pssz(a);
         STRICT_EXPECT(strict<Action>(bytes).ok());
         STRICT_EXPECT(strict<Name>(psio::to_pssz(Name{5})).ok());
         STRICT_EXPECT(strict<std::vector<Name>>(psio::to_pssz(std::vector<Name>{{1}, {2}})).ok());
         STRICT_EXPECT(strict<std::optional<Name>>(psio::to_pssz(std::optional<Name>{Name{1}})).ok());
         // Unknown members: the compatible reader accepts and reports
         // them, strict refuses.
         const auto wider = psio::to_pssz(ActionV2{Name{1}, Name{2}, {1, 2}, "n"});
         bool       unknown = false;
         STRICT_EXPECT(psio::validate_pssz_compatible<Action>(psio::pssz{}, wider, unknown).ok());
         STRICT_EXPECT(unknown);
         STRICT_EXPECT(is(strict<Action>(wider), "pssz: unknown fields in exact validation"));
         // The absent appended member is pruned: the same bytes, exact.
         STRICT_EXPECT(psio::to_pssz(ActionV2{Name{1}, Name{2}, {1, 2}, std::nullopt}) == bytes);
         STRICT_EXPECT(strict<ActionV2>(bytes).ok());
         // Malformed bytes: a status, as before.
         auto cut = bytes;
         cut.resize(cut.size() - 1);
         cut[0] = 0x7f;
         STRICT_EXPECT(!strict<Action>(cut).ok());
         STRICT_EXPECT(!strict<Action>(std::vector<char>{}).ok());
      }
      // Every kind at once, valid.
      {
         const Everything e     = sample();
         const auto       bytes = psio::to_pssz(e);
         STRICT_EXPECT(plain<Everything>(bytes).ok());
         const auto st = strict<Everything>(bytes);
         if (!st.ok()) std::printf("strict: %.*s\n", int(st.error().what.size()), st.error().what.data());
         STRICT_EXPECT(st.ok());
         STRICT_EXPECT(psio::from_pssz<Everything>(bytes) == e);
      }
      // An adapter's strict check is what exact mode asks.
      {
         std::vector<char> legacy(8, 0);
         STRICT_EXPECT(plain<Code>(legacy).ok());
         STRICT_EXPECT(is(strict<Code>(legacy), "Code: legacy form"));
         STRICT_EXPECT(strict<Code>(psio::to_pssz(Code{3})).ok());
         STRICT_EXPECT(strict<Holder>(psio::to_pssz(Holder{1, Code{2}})).ok());
      }
      // An adapter that accepts a second spelling: not canonical.
      {
         STRICT_EXPECT(strict<Flag>(std::vector<char>{'Y'}).ok());
         STRICT_EXPECT(plain<Flag>(std::vector<char>{'y'}).ok());
         STRICT_EXPECT(is(strict<Flag>(std::vector<char>{'y'}), "pssz: noncanonical encoding"));
         STRICT_EXPECT(is(strict<Flag>(std::vector<char>{'x'}), "Flag: letter"));
      }
      // Invariants: the invariant's own status, from the strict entry point.
      {
         namespace pi = psio::detail::pssz_impl;
         std::vector<char> bad;
         STRICT_EXPECT(pi::encode_ignoring_invariants(psio::pssz{}, Memo{"far too long"}, bad).ok());
         STRICT_EXPECT(is(plain<Memo>(bad), "Memo: too long"));
         STRICT_EXPECT(is(strict<Memo>(bad), "Memo: too long"));
         STRICT_EXPECT(strict<Memo>(psio::to_pssz(Memo{"ok"})).ok());
         std::vector<char> reversed;
         STRICT_EXPECT(pi::encode_ignoring_invariants(psio::pssz{}, Range{5, 1}, reversed).ok());
         STRICT_EXPECT(is(strict<Range>(reversed), "Range: reversed"));
         STRICT_EXPECT(strict<Range>(psio::to_pssz(Range{1, 5})).ok());
         STRICT_EXPECT(strict<Counter>(psio::to_pssz(Counter{4})).ok());
      }
      // Bounds and order of a member are part of its type.
      {
         STRICT_EXPECT(strict<Tags>(psio::to_pssz(Tags{{"a", "b"}})).ok());
         STRICT_EXPECT(strict<Ids>(psio::to_pssz(AnyIds{{1, 2, 3}})).ok());
         STRICT_EXPECT(!strict<Ids>(psio::to_pssz(AnyIds{{3, 2}})).ok());
         STRICT_EXPECT(!plain<Ids>(psio::to_pssz(AnyIds{{3, 2}})).ok());
      }
      // A nested value is put to the same test as its type.
      {
         const auto same = psio::to_pssz(Envelope{psio::nested<Action>{Action{{1}, {2}, {3}}}});
         STRICT_EXPECT(strict<Envelope>(same).ok());
         const auto wider = psio::to_pssz(
            EnvelopeV2{psio::nested<ActionV2>{ActionV2{{1}, {2}, {3}, "n"}}});
         STRICT_EXPECT(plain<Envelope>(wider).ok());
         STRICT_EXPECT(!strict<Envelope>(wider).ok());
      }
      // The explicit-width profiles answer alike.
      {
         const Action a{Name{1}, Name{2}, {1, 2}};
         STRICT_EXPECT(psio::validate_strict<Action>(psio::pssz_<4>{},
                                                     psio::encode(psio::pssz_<4>{}, a)).ok());
         STRICT_EXPECT(psio::validate_strict<Action>(psio::pssz_<2>{},
                                                     psio::encode(psio::pssz_<2>{}, a)).ok());
      }
#undef STRICT_EXPECT
      return 0;
   }
}  // namespace strict_cases

// Emitter for the shared cross-language PSSZ conformance fixture.
//
//   psio_pssz_conformance_emit <out.json>          write the fixture
//   psio_pssz_conformance_emit --check <ref.json>  regenerate and compare
//
// Outcomes are computed from the C++ implementation. Every hand-made frame and
// every evolution pair additionally carries the outcome expected from the
// format rules; generation aborts when the implementation disagrees, so the
// fixture cannot silently record a bug. Output is deterministic.

#include "pssz_conformance_types.hpp"
#include "pssz_evolution_goldens.hpp"

#include <psio/pssz_ir_json.hpp>
#include <psio/schema_builder.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace
{
   using namespace pssz_conf;
   namespace impl = psio::detail::pssz_impl;
   using Bytes    = std::vector<char>;

   [[noreturn]] void die(const std::string& message)
   {
      std::fprintf(stderr, "pssz conformance emit: %s\n", message.c_str());
      std::exit(2);
   }

   // ── Output rows ───────────────────────────────────────────────────────
   struct Row
   {
      std::string name, cls, type, format, hex, expect, reason, reencoded, value, writer;
   };
   std::vector<Row>                   rows;
   std::map<std::string, std::size_t> row_index;
   std::set<std::string>              registered;
   std::set<std::string>              unregistered;

   template <typename F, typename T>
   constexpr std::size_t wof()
   {
      if constexpr (std::is_same_v<F, psio::pssz>) return psio::auto_pssz_width_v<T>;
      else return F::offset_width;
   }

   template <typename Fn>
   void formats(Fn&& fn)
   {
      fn.template operator()<psio::pssz>();
      fn.template operator()<psio::pssz8>();
      fn.template operator()<psio::pssz16>();
      fn.template operator()<psio::pssz32>();
   }

   template <typename F, typename T>
   bool try_encode(const T& value, Bytes& out)
   {
      try
      {
         out = psio::encode(F{}, value);
         return true;
      }
      catch (const psio::codec_exception&)
      {
         return false;
      }
   }
   template <typename F, typename T>
   Bytes must_encode(const T& value, const std::string& what)
   {
      Bytes out;
      if (!try_encode<F>(value, out))
         die("cannot encode " + what + " as " + type_id<T>() + " in " +
             std::string(format_name<F>()));
      return out;
   }

   // ── What the implementation does with a frame ─────────────────────────
   struct Seen
   {
      std::string expect;  // canonical | canonical-nonstrict | unknown | reject
      std::string reason;
      Bytes       reencoded;
   };

   template <typename F, typename R>
   Seen observe(const Bytes& bytes, const std::string& where)
   {
      bool unknown = false;
      auto status  = psio::validate_pssz_compatible<R>(F{}, bytes, unknown);
      const bool plain  = psio::validate<R>(F{}, bytes).ok();
      const bool strict = psio::validate_strict<R>(F{}, bytes).ok();
      bool       throws = false;
      try
      {
         psio::validate_or_throw<R>(F{}, bytes);
      }
      catch (const psio::codec_exception&)
      {
         throws = true;
      }
      if (plain != status.ok() || throws == status.ok())
         die(where + ": validate / validate_or_throw / validate_pssz_compatible disagree");
      if (!status.ok())
      {
         if (strict) die(where + ": validate_strict accepts a frame validate rejects");
         R    out{};
         bool reported = false;
         if (psio::decode_pssz_checked(F{}, bytes, out, reported).ok())
            die(where + ": decode_pssz_checked accepts a frame validate rejects");
         const std::string reason(status.error().what);
         if (decode_also_rejects<F>(reason))
         {
            bool decoded = true;
            try
            {
               (void)psio::decode<R>(F{}, bytes);
            }
            catch (const psio::codec_exception&)
            {
               decoded = false;
            }
            if (decoded) die(where + ": decode accepts a frame rejected with: " + reason);
         }
         return {"reject", reason, {}};
      }
      R    out{};
      bool reported = !unknown;
      if (!psio::decode_pssz_checked(F{}, bytes, out, reported).ok() || reported != unknown)
         die(where + ": decode_pssz_checked disagrees with validate_pssz_compatible");
      Bytes again, checked;
      try
      {
         again   = psio::encode(F{}, psio::decode<R>(F{}, bytes));
         checked = psio::encode(F{}, out);
      }
      catch (const psio::codec_exception& e)
      {
         die(where + ": a validated frame does not decode and re-encode: " + e.what());
      }
      if (again != checked) die(where + ": decode and decode_pssz_checked disagree");
      if (unknown)
      {
         if (strict) die(where + ": validate_strict accepts unknown fields");
         return {"unknown", "", again};
      }
      if (again != bytes)
         die(where + ": a valid frame without unknown fields does not re-encode to itself");
      return {strict ? "canonical" : "canonical-nonstrict", "", {}};
   }

   // ── What the rules say it should do ───────────────────────────────────
   struct Want
   {
      std::string          expect;   // "" = record whatever the implementation does
      std::string          reason;   // reject: exact message, "" = any
      std::optional<Bytes> reencoded;
   };
   Want canonical() { return {"canonical", "", {}}; }
   Want nonstrict() { return {"canonical-nonstrict", "", {}}; }
   Want unknown(Bytes reencoded) { return {"unknown", "", std::move(reencoded)}; }
   Want reject(std::string reason = "") { return {"reject", std::move(reason), {}}; }
   Want record_only() { return {"", "", {}}; }

   template <typename F, typename R>
   void add(const std::string& cls, const std::string& descr, const Bytes& bytes,
            const Want& want, const std::string& value = "", const std::string& writer = "")
   {
      const std::string type = type_id<R>();
      const std::string name =
         cls + "/" + descr + "/as/" + type + "/" + std::string(format_name<F>());
      if (!registered.count(type)) unregistered.insert(type);
      const auto seen = observe<F, R>(bytes, name);
      if (!want.expect.empty())
      {
         if (seen.expect != want.expect)
            die(name + ": expected " + want.expect + ", implementation says " + seen.expect +
                (seen.reason.empty() ? "" : " (" + seen.reason + ")") + " for " + to_hex(bytes));
         if (!want.reason.empty() && seen.reason != want.reason)
            die(name + ": expected rejection '" + want.reason + "', implementation says '" +
                seen.reason + "' for " + to_hex(bytes));
         if (want.reencoded && seen.reencoded != *want.reencoded)
            die(name + ": expected re-encoding " + to_hex(*want.reencoded) + ", got " +
                to_hex(seen.reencoded));
      }
      else if (seen.expect == "canonical-nonstrict")
         die(name + ": unexpected strict-only rejection");
      Row row{name,
              cls,
              type,
              std::string(format_name<F>()),
              to_hex(bytes),
              seen.expect == "canonical-nonstrict" ? "canonical" : seen.expect,
              seen.reason,
              to_hex(seen.reencoded),
              value,
              writer};
      if (auto it = row_index.find(name); it != row_index.end())
      {
         // The same value can be reached through more than one lineage.
         if (rows[it->second].hex != row.hex) die(name + ": duplicate name, different bytes");
         return;
      }
      row_index[name] = rows.size();
      rows.push_back(std::move(row));
   }

   // ── Independent byte builder for hand-made frames ─────────────────────
   struct Fr
   {
      std::size_t W;
      Bytes       b{};
      Fr& ow(std::size_t width, std::size_t value)
      {
         for (std::size_t i = 0; i < width; ++i)
            b.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
         return *this;
      }
      Fr& o(std::size_t value) { return ow(W, value); }
      Fr& u32(std::uint32_t value) { return ow(4, value); }
      Fr& byte(unsigned value)
      {
         b.push_back(static_cast<char>(value));
         return *this;
      }
      Fr& raw(std::string_view text)
      {
         b.insert(b.end(), text.begin(), text.end());
         return *this;
      }
      Fr& cat(const Bytes& more)
      {
         b.insert(b.end(), more.begin(), more.end());
         return *this;
      }
      operator Bytes() const { return b; }
   };
   Bytes hx(std::string_view hex) { return from_hex(hex); }
   Bytes text(std::string_view s) { return {s.begin(), s.end()}; }
   Bytes join(std::initializer_list<Bytes> parts)
   {
      Bytes out;
      for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
      return out;
   }
   // A list or array of variable-size elements: W-byte offset table, payloads.
   Bytes table_of(std::size_t W, const std::vector<Bytes>& elements)
   {
      Fr          out{W};
      std::size_t at = elements.size() * W;
      for (const auto& e : elements)
      {
         out.o(at);
         at += e.size();
      }
      for (const auto& e : elements) out.cat(e);
      return out;
   }
   // An extensible record whose fields are all variable: header, slots, payloads.
   Bytes slots_of(std::size_t W, const std::vector<Bytes>& fields)
   {
      Fr          out{W};
      std::size_t at = fields.size() * W;
      out.o(at);
      for (const auto& f : fields)
      {
         out.o(at);
         at += f.size();
      }
      for (const auto& f : fields) out.cat(f);
      return out;
   }
   // {a:u32, s:raw_string} at width W (R0, C1a, C2a), and with one / two
   // stored optional slots.
   Bytes rec_as(std::size_t W, std::uint32_t a, std::string_view s)
   {
      return Fr{W}.o(4 + W).u32(a).o(4 + W).raw(s);
   }

   // ── Corpus and pinned literals ────────────────────────────────────────
   template <typename T>
   void corpus(const std::string& cls, const std::string& vname, const T& value)
   {
      formats([&]<typename F>() {
         Bytes bytes;
         if (!try_encode<F>(value, bytes))
         {
            // An explicit narrow width may be unable to hold a value.
            if (std::is_same_v<F, psio::pssz>)
               die(cls + "/" + vname + ": the automatic profile cannot encode the value");
            return;
         }
         add<F, T>(cls, vname, bytes, canonical(), vname, type_id<T>());
      });
   }
   // A value whose canonical bytes are written out by hand.
   template <typename F, typename T>
   void literal(const std::string& cls, const std::string& vname, const T& value,
                const Bytes& bytes)
   {
      const auto encoded = must_encode<F>(value, vname);
      if (encoded != bytes)
         die(cls + "/" + vname + " as " + type_id<T>() + " in " +
             std::string(format_name<F>()) + ": expected bytes " + to_hex(bytes) +
             ", encoder produced " + to_hex(encoded));
      add<F, T>(cls, vname, bytes, canonical(), vname, type_id<T>());
   }
   template <typename T>
   void literal_all(const std::string& cls, const std::string& vname, const T& value,
                    const Bytes& bytes)
   {
      formats([&]<typename F>() { literal<F, T>(cls, vname, value, bytes); });
   }
   template <typename R>
   void frame_all(const std::string& cls, const std::string& descr, const Bytes& bytes,
                  const Want& want)
   {
      formats([&]<typename F>() { add<F, R>(cls, descr, bytes, want); });
   }

   // ── Evolution: the rules, stated independently of the codec ───────────
   //
   // convert<To>(from) is how a reader of type To sees a value written as
   // From: known fields are kept, fields the reader lacks are dropped, fields
   // the writer lacks are None. has_unknown says whether any record in the
   // value wrote more slots than its reader knows.
   template <typename T>
   std::size_t emitted_fields(const T& value)
   {
      using R = impl::record_traits<T>;
      if (psio::is_dwnc_v<T>) return R::member_count;
      return [&]<std::size_t... I>(std::index_sequence<I...>) {
         std::size_t count = 0;
         (([&] {
            if constexpr (impl::is_std_optional_v<typename R::template member_type<I>>)
            {
               if (impl::record_member<I>(value).has_value()) count = I + 1;
            }
            else
               count = I + 1;
         }()),
          ...);
         return count;
      }(std::make_index_sequence<R::member_count>{});
   }

   template <typename To, typename From>
   To convert(const From& from)
   {
      if constexpr (std::is_same_v<To, From>) return from;
      else if constexpr (impl::is_std_vector_v<To>)
      {
         To out;
         for (const auto& e : from) out.push_back(convert<typename To::value_type>(e));
         return out;
      }
      else if constexpr (impl::is_std_array_v<To>)
      {
         To out{};
         for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = convert<typename To::value_type>(from[i]);
         return out;
      }
      else if constexpr (impl::is_std_optional_v<To>)
      {
         if (!from) return std::nullopt;
         return To{convert<typename To::value_type>(*from)};
      }
      else if constexpr (impl::is_std_variant_v<To>)
      {
         return [&]<std::size_t... I>(std::index_sequence<I...>) {
            To out;
            (void)((from.index() == I
                       ? (out = To{std::in_place_index<I>,
                                   convert<std::variant_alternative_t<I, To>>(std::get<I>(from))},
                          true)
                       : false) ||
                   ...);
            return out;
         }(std::make_index_sequence<std::variant_size_v<To>>{});
      }
      else
      {
         using RT = impl::record_traits<To>;
         using RF = impl::record_traits<From>;
         To out{};
         [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((impl::record_member<I>(out) =
                 convert<typename RT::template member_type<I>>(impl::record_member<I>(from))),
             ...);
         }(std::make_index_sequence<std::min(RT::member_count, RF::member_count)>{});
         return out;
      }
   }

   template <typename To, typename From>
   bool has_unknown(const From& from)
   {
      if constexpr (std::is_same_v<To, From>) return false;
      else if constexpr (impl::is_std_vector_v<To> || impl::is_std_array_v<To>)
      {
         for (const auto& e : from)
            if (has_unknown<typename To::value_type>(e)) return true;
         return false;
      }
      else if constexpr (impl::is_std_optional_v<To>)
         return from && has_unknown<typename To::value_type>(*from);
      else if constexpr (impl::is_std_variant_v<To>)
      {
         return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return ((from.index() == I &&
                     has_unknown<std::variant_alternative_t<I, To>>(std::get<I>(from))) ||
                    ...);
         }(std::make_index_sequence<std::variant_size_v<To>>{});
      }
      else
      {
         using RT = impl::record_traits<To>;
         using RF = impl::record_traits<From>;
         if (emitted_fields(from) > RT::member_count) return true;
         return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return (has_unknown<typename RT::template member_type<I>>(
                       impl::record_member<I>(from)) ||
                    ...);
         }(std::make_index_sequence<std::min(RT::member_count, RF::member_count)>{});
      }
   }

   // The position of the evolving record.
   struct AtTop
   {
      static constexpr const char* tag = "top";
      template <typename T> using type = T;
      template <typename T> static T wrap(const T& v) { return v; }
   };
   struct InVector
   {
      static constexpr const char* tag = "vector";
      template <typename T> using type = Vec<T>;
      template <typename T> static Vec<T> wrap(const T& v) { return {v, v}; }
   };
   struct InOptional
   {
      static constexpr const char* tag = "optional";
      template <typename T> using type = Opt<T>;
      template <typename T> static Opt<T> wrap(const T& v) { return v; }
   };
   struct InVariant
   {
      static constexpr const char* tag = "variant";
      template <typename T> using type = VarU<T>;
      template <typename T> static VarU<T> wrap(const T& v)
      {
         return VarU<T>{std::in_place_index<1>, v};
      }
   };
   struct InTuple
   {
      static constexpr const char* tag = "tuple";
      template <typename T> using type = TupS<T>;
      template <typename T> static TupS<T> wrap(const T& v) { return {v, "tail"}; }
   };
   struct InArray
   {
      static constexpr const char* tag = "array";
      template <typename T> using type = Arr2<T>;
      template <typename T> static Arr2<T> wrap(const T& v) { return {v, v}; }
   };
   struct InParent
   {
      static constexpr const char* tag = "parent";
      template <typename T> using type = Parent<T>;
      template <typename T> static Parent<T> wrap(const T& v) { return {v, "tail"}; }
   };

   template <typename Ctx, typename F, typename Wr, typename Rd>
   void evolve_pair(const std::string& lineage, const std::string& vname, const Wr& value)
   {
      using WT = typename Ctx::template type<Wr>;
      using RT = typename Ctx::template type<Rd>;
      constexpr bool same    = std::is_same_v<Wr, Rd>;
      constexpr bool forward = psio::pssz_evolves_from<WT, RT>;   // old data, newer reader
      constexpr bool back    = psio::pssz_evolves_from<RT, WT>;   // new data, older reader
      static_assert(same || forward || back, "not a pssz_evolves_from-legal lineage");
      const WT written = Ctx::wrap(value);
      Bytes    bytes;
      if (!try_encode<F>(written, bytes))
      {
         if (std::is_same_v<F, psio::pssz>) die(lineage + "/" + vname + ": cannot encode");
         return;
      }
      const RT    seen    = convert<RT>(written);
      const Bytes rebuilt = must_encode<F>(seen, vname + " as its reader sees it");
      const bool  unk     = has_unknown<RT>(written);
      // Pruning identity: with nothing unknown, the reader's own encoding of
      // what it saw is byte-identical to what the writer produced.
      if (!unk && rebuilt != bytes)
         die(lineage + "/" + vname + ": " + type_id<WT>() + " -> " + type_id<RT>() +
             " should be canonical, but the encodings differ");
      const std::string cls =
         same ? "corpus" : forward ? "evolution-forward" : "evolution-backward";
      const std::string descr =
         same ? vname + "/" + Ctx::tag : lineage + "/" + vname + "/" + Ctx::tag;
      add<F, RT>(cls, descr, bytes, unk ? unknown(rebuilt) : canonical(), vname, type_id<WT>());
   }

   template <typename Ctx, typename Wr, typename... Ls>
   void evolve(const std::string& lineage, const std::string& vname, const Wr& value,
               types<Ls...>)
   {
      formats([&]<typename F>() {
         (evolve_pair<Ctx, F, Wr, Ls>(lineage, vname, value), ...);
      });
   }
   template <typename Wr, typename L, typename... Ctxs>
   void evolve_in(const std::string& lineage, const std::string& vname, const Wr& value, L list,
                  types<Ctxs...>)
   {
      (evolve<Ctxs>(lineage, vname, value, list), ...);
   }

   using every_position =
      types<AtTop, InVector, InOptional, InVariant, InTuple, InArray, InParent>;
   using three_positions = types<AtTop, InVector, InOptional>;
   using top_only        = types<AtTop>;

   Bits12 bits(std::string_view pattern)
   {
      Bits12 out;
      for (char c : pattern) out.push_back(c == '1');
      return out;
   }
   psio::bitvector<12> bitvec(std::string_view pattern)
   {
      psio::bitvector<12> out;
      for (std::size_t i = 0; i < pattern.size(); ++i) out.set(i, pattern[i] == '1');
      return out;
   }

   // ══ Sections ══════════════════════════════════════════════════════════

   void emit_evolution()
   {
      const auto none = std::nullopt;
      using OO        = std::optional<std::optional<u32>>;
      // Lineage A (the evolution corpus of pssz_evolution_fixture.hpp): a fixed
      // scalar, a string and a vector appended, at automatic width 4.
      {
         using L = types<R0, R1, R2, R3>;
         auto all = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-fixed-string-vector", vname, value, L{}, every_position{});
         };
         all("r0", R0{7, "hi"});
         all("r1_none", R1{7, "hi", none});
         all("r1_some", R1{7, "hi", 9});
         all("r2_none_none", R2{7, "hi", none, none});
         all("r2_some_none", R2{7, "hi", 9, none});
         all("r2_none_some", R2{7, "hi", none, Raw("yo")});
         all("r2_none_empty", R2{7, "hi", none, Raw()});
         all("r3_none", R3{7, "hi", none, none, none});
         all("r3_all", R3{7, "hi", 9, Raw("yo"), std::vector<u16>{1, 2}});
         all("r3_mid", R3{7, "hi", none, Raw("yo"), none});
         all("r3_empty_vec", R3{7, "hi", none, none, std::vector<u16>{}});
         // The fixture's own nested values (two different elements, etc.).
         const R2 full{1, "a", 9, Raw("b")};
         evolve<AtTop>("w4-fixed-string-vector", "vec_new",
                       Vec<R2>{full, R2{2, "c", none, none}},
                       types<Vec<R0>, Vec<R1>, Vec<R2>, Vec<R3>>{});
         evolve<AtTop>("w4-fixed-string-vector", "opt_new", Opt<R2>{full},
                       types<Opt<R0>, Opt<R1>, Opt<R2>, Opt<R3>>{});
         evolve<AtTop>("w4-fixed-string-vector", "var_new", VarU<R2>{full},
                       types<VarU<R0>, VarU<R1>, VarU<R2>, VarU<R3>>{});
         evolve<AtTop>("w4-fixed-string-vector", "parent_new", Parent2{full, "tail"},
                       types<Parent0, Parent1, Parent2, Parent3>{});
      }
      // Lineage B: a nested record, then optional<optional<T>>.
      {
         using L = types<R0, N1, N2>;
         auto all = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-record-nested-optional", vname, value, L{}, three_positions{});
         };
         all("r0", R0{7, "hi"});
         all("n1_none", N1{7, "hi", none});
         all("n1_some", N1{7, "hi", Inner{3, "t"}});
         all("n2_none", N2{7, "hi", none, none});
         all("n2_in", N2{7, "hi", Inner{3, "t"}, none});
         all("n2_oo_none", N2{7, "hi", none, OO{std::in_place, none}});
         all("n2_both", N2{7, "hi", Inner{3, "t"}, OO{std::in_place, 5}});
         auto p = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-nested-optional", vname, value, types<R0, P1>{}, three_positions{});
         };
         p("r0", R0{7, "hi"});
         p("p1_none", P1{7, "hi", none});
         p("p1_some_none", P1{7, "hi", OO{std::in_place, none}});
         p("p1_some_some", P1{7, "hi", OO{std::in_place, 5}});
      }
      // Lineage C: an enum, a variant, a tuple.
      {
         using L = types<R0, E1, E2, E3>;
         auto all = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-enum-variant-tuple", vname, value, L{}, three_positions{});
         };
         all("r0", R0{7, "hi"});
         all("e1_none", E1{7, "hi", none});
         all("e1_some", E1{7, "hi", Color::Green});
         all("e1_red", E1{7, "hi", Color::Red});
         all("e2_none", E2{7, "hi", none, none});
         all("e2_var_u32", E2{7, "hi", none, VarUS{std::in_place_index<0>, 5}});
         all("e2_var_string", E2{7, "hi", Color::Blue, VarUS{std::in_place_index<1>, "v"}});
         all("e3_none", E3{7, "hi", none, none, none});
         all("e3_tuple", E3{7, "hi", none, none, TupBS{1, "t"}});
         all("e3_all", E3{7, "hi", Color::Green, VarUS{std::in_place_index<1>, ""}, TupBS{0, ""}});
      }
      // Capped lineages: an explicit maxDynamicData pins one- and two-byte
      // offsets in the automatic profile while fields are appended.
      {
         auto capped = [&]<typename A, typename B, typename C>(const std::string& lineage,
                                                               const std::string& p) {
            auto all = [&](const std::string& vname, const auto& value) {
               evolve_in(lineage, p + vname, value, types<A, B, C>{}, three_positions{});
            };
            all("a", A{7, "hi"});
            all("b_none", B{7, "hi", none});
            all("b_some", B{7, "hi", 9});
            all("c_none_none", C{7, "hi", none, none});
            all("c_some_none", C{7, "hi", 9, none});
            all("c_none_some", C{7, "hi", none, Raw("yo")});
            all("c_none_empty", C{7, "hi", none, Raw()});
            all("c_all", C{7, "hi", 9, Raw("yo")});
         };
         capped.template operator()<C1a, C1b, C1c>("w1-capped", "c1");
         capped.template operator()<C2a, C2b, C2c>("w2-capped", "c2");
         auto inferred = [&](const std::string& vname, const auto& value) {
            evolve_in("w1-inferred", vname, value, types<I0, I1>{}, three_positions{});
         };
         inferred("i0", I0{7, 3});
         inferred("i1_none", I1{7, 3, none});
         inferred("i1_some", I1{7, 3, 9});
         auto grow = [&](const std::string& vname, const auto& value) {
            evolve_in("w2-capped-grow", vname, value, types<WidthGrowV1, WidthGrowV2>{},
                      top_only{});
         };
         grow("grow_v1", WidthGrowV1{7});
         grow("grow_v2_none", WidthGrowV2{7, none});
         grow("grow_v2_some", WidthGrowV2{7, 9});
         auto grow_parent = [&](const std::string& vname, const auto& value) {
            evolve_in("w2-capped-grow-parent", vname, value,
                      types<WidthGrowParentV1, WidthGrowParentV2>{}, top_only{});
         };
         grow_parent("grow_parent_v1", WidthGrowParentV1{{7}});
         grow_parent("grow_parent_v2_some", WidthGrowParentV2{{7, 9}});
      }
      // The optional fixtures (pssz_optional_fixtures.hpp).
      {
         auto be = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-optional-fixture", vname, value, types<Base, Extended>{}, top_only{});
         };
         be("base", Base{"a"});
         be("extended_none", Extended{"a", none});
         be("extended_some", Extended{"a", Raw("b")});
         be("extended_empty", Extended{"a", Raw()});
         auto bt = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-optional-fixture-twice", vname, value, types<Base, Twice>{},
                      top_only{});
         };
         bt("base", Base{"a"});
         bt("twice_none_some", Twice{"a", none, Raw("b")});
         bt("twice_some_none", Twice{"a", 9, none});
         bt("twice_none_none", Twice{"a", none, none});
         auto odd = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-optional-fixture-odd", vname, value, types<OddBase, OddExtended>{},
                      top_only{});
         };
         odd("odd_base", OddBase{true, "a"});
         odd("odd_extended", OddExtended{true, "a", Raw("b")});
         auto outer = [&](const std::string& vname, const auto& value) {
            evolve_in("w4-optional-fixture-outer", vname, value, types<OldOuter, NewOuter>{},
                      top_only{});
         };
         outer("old_outer", OldOuter{{"a"}, {{"c"}}});
         outer("new_outer", NewOuter{{"a", Raw("b")}, {{"c", Raw("d")}}});
         outer("new_outer_pruned", NewOuter{{"a", none}, {{"c", none}, {"e", Raw("f")}}});
      }
   }

   // The evolution corpus must still be the checked-in golden bytes.
   void check_goldens()
   {
      static constexpr std::pair<std::string_view, std::string_view> writers[]{
         {"r0", "R0"}, {"r1_none", "R1"}, {"r1_some", "R1"}, {"r2_none_none", "R2"},
         {"r2_some_none", "R2"}, {"r2_none_some", "R2"}, {"r2_none_empty", "R2"},
         {"r3_none", "R3"}, {"r3_all", "R3"}, {"r3_mid", "R3"}, {"r3_empty_vec", "R3"},
         {"n1_none", "N1"}, {"n1_some", "N1"}, {"p1_none", "P1"}, {"p1_some_none", "P1"},
         {"p1_some_some", "P1"}, {"vec_new", "list<R2>"}, {"opt_new", "optional<R2>"},
         {"var_new", "variant<u32,R2>"}, {"parent_new", "Parent2"}};
      static constexpr std::string_view columns[]{"pssz8", "pssz16", "pssz32"};
      if (std::size(writers) != evo::goldens.size()) die("golden corpus size changed");
      for (const auto& golden : evo::goldens)
      {
         std::string_view writer;
         for (const auto& [name, type] : writers)
            if (name == golden.name) writer = type;
         if (writer.empty()) die("golden " + std::string(golden.name) + " has no fixture value");
         for (std::size_t column = 0; column < 4; ++column)
         {
            const std::string name = "corpus/" + std::string(golden.name) + "/top/as/" +
                                     std::string(writer) + "/" +
                                     std::string(column < 3 ? columns[column] : "auto");
            auto it = row_index.find(name);
            if (it == row_index.end()) die("missing corpus vector " + name);
            // The automatic profile selects width 4 for the whole corpus.
            if (rows[it->second].hex != golden.hex[column < 3 ? column : 2])
               die(name + " differs from pssz_evolution_goldens.hpp");
         }
      }
   }

   void emit_pruning_and_malformed()
   {
      // Present-but-empty values are values: they are kept, not pruned.
      corpus("pruning", "some_empty_string", R2{7, "hi", std::nullopt, Raw()});
      corpus("pruning", "some_empty_vector",
             R3{7, "hi", std::nullopt, std::nullopt, std::vector<u16>{}});
      corpus("pruning", "some_none",
             P1{7, "hi", std::optional<std::optional<u32>>{std::in_place, std::nullopt}});
      corpus("pruning", "tuple_some_empty_string",
             std::tuple<u8, std::optional<u32>, std::optional<Raw>>{1, std::nullopt, Raw()});

      const std::string unpruned = "pssz: trailing absent optional field was not pruned";
      const std::string payload  = "pssz: invalid record payload offset";
      const std::string missing  = "pssz: required record field missing";
      const std::string cuts     = "pssz: header cuts a known field";
      const std::string hdr      = "pssz: record header truncated";
      const std::string region   = "pssz: record fixed region truncated";
      const std::string partial  = "pssz: partial unknown optional slot";

      // Lineage A selects width 4 in the automatic profile; explicit tags use
      // their own width throughout.
      formats([&]<typename F>() {
         const std::size_t W  = wof<F, R0>();
         const std::size_t K0 = 4 + W, K1 = 4 + 2 * W, K2 = 4 + 3 * W;
         const Bytes r0       = rec_as(W, 7, "hi");
         const Bytes r1_some  = Fr{W}.o(K1).u32(7).o(K1).o(K1 + 2).raw("hi").u32(9);

         // ── Stored trailing None ────────────────────────────────────────
         // R1 with x stored as None: optional<u32> has no selector, so None
         // is the empty span.
         const Bytes fixed_none = Fr{W}.o(K1).u32(7).o(K1).o(K1 + 2).raw("hi");
         add<F, R1>("pruning", "stored_none_no_selector", fixed_none, reject(unpruned));
         add<F, R2>("pruning", "stored_none_no_selector", fixed_none, reject(unpruned));
         add<F, R3>("pruning", "stored_none_no_selector", fixed_none, reject(unpruned));
         // An older reader does not know the writer's last slot.
         add<F, R0>("pruning", "stored_none_no_selector", fixed_none, unknown(r0));
         // R2 with x present and y stored as None: optional<string> None is 00.
         const Bytes selector_none =
            Fr{W}.o(K2).u32(7).o(K2).o(K2 + 2).o(K2 + 6).raw("hi").u32(9).byte(0);
         add<F, R2>("pruning", "stored_none_selector", selector_none, reject(unpruned));
         add<F, R3>("pruning", "stored_none_selector", selector_none, reject(unpruned));
         add<F, R1>("pruning", "stored_none_selector", selector_none, unknown(r1_some));
         add<F, R0>("pruning", "stored_none_selector", selector_none, unknown(r0));
         // Both suffix fields stored as None.
         const Bytes all_none = Fr{W}.o(K2).u32(7).o(K2).o(K2 + 2).o(K2 + 2).raw("hi").byte(0);
         add<F, R2>("pruning", "stored_none_both", all_none, reject(unpruned));
         add<F, R3>("pruning", "stored_none_both", all_none, reject(unpruned));
         add<F, R1>("pruning", "stored_none_both", all_none, unknown(r0));
         add<F, R0>("pruning", "stored_none_both", all_none, unknown(r0));
         // The canonical forms of the same values.
         add<F, R1>("pruning", "pruned_none", r0, canonical());
         add<F, R2>("pruning", "pruned_none", r0, canonical());
         add<F, R2>("pruning", "pruned_trailing_none", r1_some, canonical());

         // ── The same unpruned record wherever it is nested ──────────────
         auto nested = [&]<typename Old, typename Known, typename Newer>(
                          const std::string& where, const Bytes& bad, const Bytes& good) {
            add<F, Known>("pruning", "nested_stored_none/" + where, bad, reject(unpruned));
            add<F, Newer>("pruning", "nested_stored_none/" + where, bad, reject(unpruned));
            add<F, Old>("pruning", "nested_stored_none/" + where, bad, unknown(good));
         };
         {
            const std::size_t V = wof<F, Vec<R1>>();
            nested.template operator()<Vec<R0>, Vec<R1>, Vec<R2>>(
               "vector", table_of(V, {r0, fixed_none}), table_of(V, {r0, r0}));
            nested.template operator()<Opt<R0>, Opt<R1>, Opt<R2>>("optional", fixed_none, r0);
            nested.template operator()<VarU<R0>, VarU<R1>, VarU<R2>>(
               "variant", join({hx("01"), fixed_none}), join({hx("01"), r0}));
            const std::size_t T = wof<F, TupS<R1>>();
            nested.template operator()<TupS<R0>, TupS<R1>, TupS<R2>>(
               "tuple", slots_of(T, {fixed_none, text("tail")}), slots_of(T, {r0, text("tail")}));
            const std::size_t A = wof<F, Arr2<R1>>();
            nested.template operator()<Arr2<R0>, Arr2<R1>, Arr2<R2>>(
               "array", table_of(A, {fixed_none, r0}), table_of(A, {r0, r0}));
            const std::size_t P = wof<F, Parent1>();
            nested.template operator()<Parent0, Parent1, Parent2>(
               "parent", slots_of(P, {fixed_none, text("tail")}), slots_of(P, {r0, text("tail")}));
         }

         // ── Malformed record frames ─────────────────────────────────────
         // Header smaller than the non-optional prefix: `s` has no slot.
         add<F, R0>("malformed", "record/header_too_small", Fr{W}.o(4).u32(7), reject(missing));
         add<F, R1>("malformed", "record/header_too_small", Fr{W}.o(4).u32(7), reject(missing));
         add<F, R0>("malformed", "record/header_zero", Fr{W}.o(0), reject(missing));
         // Not on a field boundary: cuts `a`.
         add<F, R0>("malformed", "record/header_cuts_field", Fr{W}.o(2).byte(7).byte(0),
                    reject(cuts));
         add<F, R3>("malformed", "record/header_cuts_field", Fr{W}.o(2).byte(7).byte(0),
                    reject(cuts));
         // Empty input and a truncated header.
         add<F, R0>("malformed", "record/empty_input", Bytes{}, reject(hdr));
         add<F, R3>("malformed", "record/empty_input", Bytes{}, reject(hdr));
         add<F, R0>("malformed", "record/one_byte", hx("05"), reject(W > 1 ? hdr : region));
         if (W > 1)
         {
            // The suffix is not whole slots: cuts an optional slot the reader
            // knows, or leaves a partial unknown slot for an older reader.
            const Bytes part = Fr{W}.o(K0 + 1).u32(7).o(K0 + 1).byte(0).raw("hi");
            add<F, R0>("malformed", "record/partial_unknown_slot", part, reject(partial));
            add<F, R1>("malformed", "record/partial_unknown_slot", part, reject(cuts));
            add<F, R2>("malformed", "record/partial_unknown_slot", part, reject(cuts));
         }
         // Header larger than the frame.
         const Bytes big = Fr{W}.o(200).u32(7).o(200).raw("hi");
         add<F, R0>("malformed", "record/header_larger_than_frame", big, reject(region));
         add<F, R1>("malformed", "record/header_larger_than_frame", big, reject(region));
         add<F, R0>("malformed", "record/unknown_slot_missing",
                    Fr{W}.o(K0 + W).u32(7).o(K0 + W), reject(region));
         // The first payload must begin exactly at the end of the fixed region.
         add<F, R0>("malformed", "record/first_payload_after_fixed_region",
                    Fr{W}.o(K0).u32(7).o(K0 + 1).raw("xhi"), reject(payload));
         add<F, R0>("malformed", "record/first_payload_inside_fixed_region",
                    Fr{W}.o(K0).u32(7).o(K0 - 1).raw("hi"), reject(payload));
         // Offsets past the frame.
         add<F, R0>("malformed", "record/offset_past_frame",
                    Fr{W}.o(K0).u32(7).o(K0 + 9).raw("hi"), reject(payload));
         add<F, R0>("malformed", "record/unknown_offset_past_frame",
                    Fr{W}.o(K1).u32(7).o(K1).o(K1 + 3).raw("hi"), reject(payload));
         add<F, R1>("malformed", "record/offset_past_frame",
                    Fr{W}.o(K1).u32(7).o(K1).o(K1 + 3).raw("hi"), reject(payload));
         // Offsets running backwards: between known fields, and in the
         // unknown suffix.
         const Bytes first_wrong = Fr{W}.o(K1).u32(7).o(K1 + 1).o(K1).raw("hi");
         add<F, R0>("malformed", "record/offsets_backwards_first", first_wrong, reject(payload));
         add<F, R1>("malformed", "record/offsets_backwards_first", first_wrong, reject(payload));
         const Bytes backwards =
            Fr{W}.o(K2).u32(7).o(K2).o(K2 + 2).o(K2 + 1).raw("hi").u32(9);
         add<F, R0>("malformed", "record/offsets_backwards", backwards, reject(payload));
         add<F, R1>("malformed", "record/offsets_backwards", backwards, reject(payload));
         add<F, R2>("malformed", "record/offsets_backwards", backwards, reject(payload));
         // A selector-carrying optional stored as an empty span in a middle
         // slot: the selector byte is missing.
         const Bytes no_selector =
            Fr{W}.o(4 + 4 * W).u32(7).o(4 + 4 * W).o(4 + 4 * W + 2).o(4 + 4 * W + 2).o(4 + 4 * W + 2)
               .raw("hi").byte(1);
         add<F, R3>("malformed", "record/middle_optional_missing_selector", no_selector,
                    reject("pssz: optional<var> missing selector"));

         // ── An appended non-optional field is not a compatible revision ──
         add<F, Bad1>("malformed", "record/appended_required_field_absent", r0, reject(missing));
         if (W < 4)
            add<F, Bad1>("malformed", "record/appended_required_field_vs_optional", r1_some,
                         reject(cuts));
         else
         {
            // At four-byte offsets an offset slot and a u32 are the same size.
            // The wire carries no schema, so the reader cannot tell: it reads
            // the slot as `b` and the rest as `s`. This is why "append only
            // optionals" is a schema-upgrade rule, not a wire check.
            const Bad1 misread{7, std::string("hi\x09\0\0\0", 6), 14};
            if (must_encode<F>(misread, "misread") != r1_some) die("Bad1 misread expectation");
            add<F, Bad1>("malformed", "record/appended_required_field_vs_optional", r1_some,
                         canonical());
         }
         const Bad1 bad{7, "hi", 9};
         add<F, Bad1>("corpus", "bad1/top", must_encode<F>(bad, "bad1"), canonical(), "bad1",
                      "Bad1");
      });

      // Capped lineages: the same pruning rule at automatic widths 1 and 2.
      formats([&]<typename F>() {
         auto capped = [&]<typename A, typename B, typename C>(const std::string& tag) {
            const std::size_t W  = wof<F, A>();
            const std::size_t K1 = 4 + 2 * W;
            const Bytes       a  = rec_as(W, 7, "hi");
            const Bytes stored   = Fr{W}.o(K1).u32(7).o(K1).o(K1 + 2).raw("hi");
            add<F, B>("pruning", tag + "/stored_none_no_selector", stored, reject(unpruned));
            add<F, C>("pruning", tag + "/stored_none_no_selector", stored, reject(unpruned));
            add<F, A>("pruning", tag + "/stored_none_no_selector", stored, unknown(a));
            const std::size_t V = wof<F, Vec<B>>();
            add<F, Vec<B>>("pruning", tag + "/nested_stored_none/vector",
                           table_of(V, {a, stored}), reject(unpruned));
            add<F, Vec<A>>("pruning", tag + "/nested_stored_none/vector",
                           table_of(V, {a, stored}), unknown(table_of(V, {a, a})));
            add<F, Opt<B>>("pruning", tag + "/nested_stored_none/optional", stored,
                           reject(unpruned));
         };
         capped.template operator()<C1a, C1b, C1c>("w1-capped");
         capped.template operator()<C2a, C2b, C2c>("w2-capped");
      });

      // Tuples are positional records and prune the same way.
      formats([&]<typename F>() {
         using T             = std::tuple<u8, std::optional<u32>, std::optional<Raw>>;
         const std::size_t W = wof<F, T>();
         literal<F, T>("tuple", "prune_all", T{1, std::nullopt, std::nullopt},
                       Fr{W}.o(1).byte(1));
         literal<F, T>("tuple", "prune_last", T{1, 9, std::nullopt},
                       Fr{W}.o(1 + W).byte(1).o(1 + W).u32(9));
         literal<F, T>("tuple", "keep_middle_none", T{1, std::nullopt, Raw("yo")},
                       Fr{W}.o(1 + 2 * W).byte(1).o(1 + 2 * W).o(1 + 2 * W).raw("\x01yo"));
         add<F, T>("pruning", "tuple/stored_none_no_selector",
                   Fr{W}.o(1 + W).byte(1).o(1 + W), reject(unpruned));
         add<F, T>("pruning", "tuple/stored_none_selector",
                   Fr{W}.o(1 + 2 * W).byte(1).o(1 + 2 * W).o(1 + 2 * W + 4).u32(9).byte(0),
                   reject(unpruned));
         add<F, std::tuple<u32>>("pruning", "tuple/unknown_slot",
                                 Fr{W}.o(4 + W).u32(9).o(4 + W).raw("x"),
                                 unknown(Fr{wof<F, std::tuple<u32>>()}.o(4).u32(9)));
      });
   }

   void emit_containers_malformed()
   {
      formats([&]<typename F>() {
         // ── Vector offset tables ────────────────────────────────────────
         {
            const std::size_t W = wof<F, Vec<Raw>>();
            const char*       c = "malformed";
            literal<F, Vec<Raw>>("corpus", "list_of_strings", Vec<Raw>{"ab", "", "c"},
                                 Fr{W}.o(3 * W).o(3 * W + 2).o(3 * W + 2).raw("abc"));
            add<F, Vec<Raw>>(c, "list/first_offset_zero", Fr{W}.o(0),
                             reject("pssz: invalid first list offset"));
            if (W > 1)
            {
               add<F, Vec<Raw>>(c, "list/shorter_than_one_slot", hx("01"),
                                reject("pssz: variable list missing offset table"));
               add<F, Vec<Raw>>(c, "list/first_offset_not_slot_multiple",
                                Fr{W}.o(W + 1).byte(0), reject("pssz: invalid first list offset"));
            }
            add<F, Vec<Raw>>(c, "list/first_offset_past_frame", Fr{W}.o(2 * W),
                             reject("pssz: invalid first list offset"));
            add<F, Vec<Raw>>(c, "list/offsets_backwards",
                             Fr{W}.o(2 * W).o(2 * W - 1).raw("ab"),
                             reject("pssz: list offset out of range"));
            add<F, Vec<Raw>>(c, "list/offset_past_frame", Fr{W}.o(2 * W).o(2 * W + 5).raw("ab"),
                             reject("pssz: list offset out of range"));
         }
         // ── Array offset tables ─────────────────────────────────────────
         {
            const std::size_t W = wof<F, Arr2<Raw>>();
            const char*       c = "malformed";
            literal<F, Arr2<Raw>>("corpus", "array_of_strings", Arr2<Raw>{"ab", "c"},
                                  Fr{W}.o(2 * W).o(2 * W + 2).raw("abc"));
            add<F, Arr2<Raw>>(c, "array/empty_input", Bytes{},
                              reject("pssz: array offset table truncated"));
            Bytes cut(2 * W - 1, 0);
            cut[0] = static_cast<char>(2 * W);
            add<F, Arr2<Raw>>(c, "array/table_truncated", cut,
                              reject("pssz: array offset table truncated"));
            add<F, Arr2<Raw>>(c, "array/first_offset_wrong",
                              Fr{W}.o(2 * W + 1).o(2 * W + 1).raw("a"),
                              reject("pssz: invalid array first offset"));
            add<F, Arr2<Raw>>(c, "array/three_slots", Fr{W}.o(3 * W).o(3 * W).o(3 * W),
                              reject("pssz: invalid array first offset"));
            add<F, Arr2<Raw>>(c, "array/offset_past_frame",
                              Fr{W}.o(2 * W).o(2 * W + 9).raw("ab"),
                              reject("pssz: array offset out of range"));
            add<F, Arr2<Raw>>(c, "array/offsets_backwards", Fr{W}.o(2 * W).o(W).raw("ab"),
                              reject("pssz: array offset out of range"));
         }
      });

      const std::string span = "pssz: fixed primitive span mismatch";
      // ── Fixed spans and trailing bytes ─────────────────────────────────
      frame_all<u32>("malformed", "fixed/u32_short", hx("010203"), reject(span));
      frame_all<u32>("malformed", "fixed/u32_trailing_byte", hx("0102030405"), reject(span));
      frame_all<u32>("malformed", "fixed/u32_empty", Bytes{}, reject(span));
      frame_all<u8>("malformed", "fixed/u8_trailing_byte", hx("0100"), reject(span));
      frame_all<u64>("malformed", "fixed/u64_short", hx("01020304050607"), reject(span));
      frame_all<double>("malformed", "fixed/f64_trailing_byte", hx("000000000000f03f00"),
                        reject(span));
      frame_all<psio::uint256>("malformed", "fixed/u256_short", Bytes(31, 1), reject(span));
      frame_all<psio::uint128>("malformed", "fixed/u128_trailing_byte", Bytes(17, 1),
                               reject(span));
      frame_all<std::array<u8, 4>>("malformed", "fixed/array_short", hx("010203"), reject(span));
      frame_all<std::array<u8, 4>>("malformed", "fixed/array_trailing_byte", hx("0102030405"),
                                   reject(span));
      frame_all<Pair>("malformed", "fixed/dwnc_record_trailing_byte", hx("010203"),
                      reject("pssz: fixed record has trailing payload"));
      frame_all<Pair>("malformed", "fixed/dwnc_record_short", hx("01"),
                      reject("pssz: record fixed region truncated"));
      frame_all<Vec<u32>>("malformed", "fixed/vector_span_not_multiple", hx("0102030405"),
                          reject("pssz: vector span not a multiple of element size"));
      frame_all<Vec<Pair>>("malformed", "fixed/vector_of_records_span_not_multiple", hx("010203"),
                           reject("pssz: vector span not a multiple of element size"));
      frame_all<Opt<u32>>("malformed", "fixed/optional_short", hx("010203"),
                          reject("pssz: optional<fixed> span mismatch"));
      frame_all<Opt<u32>>("malformed", "fixed/optional_with_selector", hx("0109000000"),
                          reject("pssz: optional<fixed> span mismatch"));
      // Extensible record whose fields are all fixed: nothing may follow.
      formats([&]<typename F>() {
         const std::size_t W = wof<F, I0>();
         literal<F, I0>("corpus", "i0_literal", I0{7, 3}, Fr{W}.o(5).u32(7).byte(3));
         add<F, I0>("malformed", "fixed/record_trailing_byte", Fr{W}.o(5).u32(7).byte(3).byte(0),
                    reject("pssz: fixed record has trailing payload"));
      });

      // ── bool ───────────────────────────────────────────────────────────
      literal_all<bool>("corpus", "bool_false", false, hx("00"));
      literal_all<bool>("corpus", "bool_true", true, hx("01"));
      frame_all<bool>("malformed", "bool/two", hx("02"), reject("pssz: bool is not 0/1"));
      frame_all<bool>("malformed", "bool/ff", hx("ff"), reject("pssz: bool is not 0/1"));
      frame_all<bool>("malformed", "bool/trailing_byte", hx("0000"), reject(span));
      // (std::vector<bool> cannot be encoded by the C++ codec, so bool
      // elements are exercised through an array.)
      literal_all<Arr2<bool>>("corpus", "bools", Arr2<bool>{false, true}, hx("0001"));
      frame_all<Arr2<bool>>("malformed", "bool/in_array", hx("0002"),
                            reject("pssz: bool is not 0/1"));
      formats([&]<typename F>() {
         const std::size_t W = wof<F, OddBase>();
         add<F, OddBase>("malformed", "bool/in_record", Fr{W}.o(1 + W).byte(2).o(1 + W).raw("a"),
                         reject("pssz: bool is not 0/1"));
      });

      // ── Optional selectors ─────────────────────────────────────────────
      frame_all<Opt<Raw>>("malformed", "optional/selector_two", hx("02"),
                          reject("pssz: optional selector not 0/1"));
      frame_all<Opt<Raw>>("malformed", "optional/selector_two_with_payload", hx("026869"),
                          reject("pssz: optional selector not 0/1"));
      frame_all<Opt<Raw>>("malformed", "optional/bytes_after_none", hx("0078"),
                          reject("pssz: None has trailing bytes"));
      frame_all<Opt<Raw>>("malformed", "optional/missing_selector", Bytes{},
                          reject("pssz: optional<var> missing selector"));
      frame_all<Opt<Unit>>("malformed", "optional/unit_missing_selector", Bytes{},
                           reject("pssz: optional<var> missing selector"));
      frame_all<Opt<Unit>>("malformed", "optional/unit_selector_two", hx("02"),
                           reject("pssz: optional selector not 0/1"));
      frame_all<Opt<Unit>>("malformed", "optional/unit_some_with_payload", hx("0100"),
                           reject("pssz: zero-size fixed payload must be empty"));
      frame_all<Opt<OptU32>>("malformed", "optional/nested_bytes_after_none", hx("0009000000"),
                             reject("pssz: None has trailing bytes"));
      // A selector byte where the payload has a positive minimum size.
      frame_all<Opt<Inner>>("malformed", "optional/record_with_none_selector", hx("00"), reject());
      frame_all<Opt<Color>>("malformed", "optional/enum_with_selector", hx("0101"),
                            reject("pssz: optional<fixed> span mismatch"));
      frame_all<Opt<Pair>>("malformed", "optional/dwnc_record_with_selector", hx("010102"),
                           reject("pssz: optional<fixed> span mismatch"));

      // ── Variants ───────────────────────────────────────────────────────
      frame_all<VarUS>("malformed", "variant/index_out_of_range", hx("0200000000"),
                       reject("pssz: variant selector out of range"));
      frame_all<VarUS>("malformed", "variant/index_ff", hx("ff"),
                       reject("pssz: variant selector out of range"));
      frame_all<VarUS>("malformed", "variant/empty", Bytes{},
                       reject("pssz: variant span out of bounds"));
      frame_all<VarUS>("malformed", "variant/fixed_alternative_short", hx("00010203"),
                       reject(span));
      frame_all<VarUS>("malformed", "variant/fixed_alternative_trailing_byte", hx("000102030405"),
                       reject(span));
      frame_all<Var5>("malformed", "variant/index_out_of_range", hx("05"),
                      reject("pssz: variant selector out of range"));
      frame_all<Var5>("malformed", "variant/unit_alternative_with_payload", hx("0400"),
                      reject("pssz: zero-size fixed payload must be empty"));

      // ── Bitlists and bitvectors ────────────────────────────────────────
      frame_all<Bits12>("malformed", "bitlist/empty", Bytes{},
                        reject("pssz: bitlist span out of bounds"));
      frame_all<Bits12>("malformed", "bitlist/no_delimiter", hx("00"),
                        reject("pssz: bitlist missing delimiter"));
      frame_all<Bits12>("malformed", "bitlist/trailing_zero_byte", hx("0100"),
                        reject("pssz: bitlist missing delimiter"));
      frame_all<Bits12>("malformed", "bitlist/over_bound", hx("ff3f"),
                        reject("pssz: bitlist bit_count exceeds bound"));
      frame_all<Bits12>("malformed", "bitlist/far_over_bound", hx("ffffff01"),
                        reject("pssz: bitlist bit_count exceeds bound"));
      frame_all<psio::bitvector<12>>("malformed", "bitvector/unused_bits_set", hx("ff1f"),
                                     reject("pssz: nonzero unused bitvector bits"));
      frame_all<psio::bitvector<12>>("malformed", "bitvector/short", hx("ff"), reject(span));
      frame_all<psio::bitvector<12>>("malformed", "bitvector/trailing_byte", hx("ff0f00"),
                                     reject(span));

      // ── Zero-size elements and payloads ────────────────────────────────
      const std::string zero = "pssz: zero-size fixed payload must be empty";
      frame_all<Vec<Unit>>("malformed", "zero/nonempty_vector_of_units", hx("00"),
                           reject("pssz: nonempty zero-size vector is unsupported"));
      frame_all<Vec<Unit>>("malformed", "zero/nonempty_vector_of_units_01", hx("01"),
                           reject("pssz: nonempty zero-size vector is unsupported"));
      frame_all<Unit>("malformed", "zero/unit_with_byte", hx("00"), reject(zero));
      frame_all<Arr2<Unit>>("malformed", "zero/array_of_units_with_byte", hx("00"), reject(zero));
      frame_all<std::array<u32, 0>>("malformed", "zero/empty_array_with_byte", hx("00"),
                                    reject(zero));
      frame_all<std::tuple<>>("malformed", "zero/empty_tuple_without_header", Bytes{},
                              reject("pssz: record header truncated"));
      formats([&]<typename F>() {
         const std::size_t W = wof<F, Empty>();
         add<F, Empty>("malformed", "zero/empty_record_without_header", Bytes{},
                       reject("pssz: record header truncated"));
         add<F, Empty>("malformed", "zero/empty_record_trailing_byte", Fr{W}.o(0).byte(0),
                       reject("pssz: fixed record has trailing payload"));
         // An unknown suffix slot on a zero-field record is a compatible frame.
         add<F, Empty>("zero-fixed", "empty_record_unknown_slot", Fr{W}.o(W).o(W).raw("x"),
                       unknown(Fr{W}.o(0)));
         const std::size_t D = wof<F, DwncVar>();
         // DWNC records have no header and are never pruned or extended.
         add<F, DwncVar>("malformed", "zero/dwnc_short", hx("07"),
                         reject("pssz: record fixed region truncated"));
         add<F, DwncVar>("malformed", "zero/dwnc_offset_wrong",
                         Fr{D}.byte(7).o(D + 2).raw("hi"),
                         reject("pssz: invalid record payload offset"));
         add<F, DwncVar>("malformed", "zero/dwnc_with_header",
                         Fr{D}.o(1 + D).byte(7).o(1 + D).raw("hi"), reject());
      });

      // ── Enum spans ─────────────────────────────────────────────────────
      frame_all<Color>("malformed", "enum/u8_empty", Bytes{}, reject(span));
      frame_all<Color>("malformed", "enum/u8_trailing_byte", hx("0100"), reject(span));
      frame_all<E16>("malformed", "enum/u16_short", hx("01"), reject(span));
      frame_all<E32>("malformed", "enum/i32_short", hx("010203"), reject(span));
      frame_all<E32>("malformed", "enum/i32_trailing_byte", hx("0102030405"), reject(span));
      frame_all<E64>("malformed", "enum/u64_short", hx("01020304050607"), reject(span));
      frame_all<Vec<E16>>("malformed", "enum/vector_span_not_multiple", hx("010203"),
                          reject("pssz: vector span not a multiple of element size"));
      frame_all<Arr2<E32>>("malformed", "enum/array_short", hx("01020304050607"), reject(span));
   }

   void emit_optional_presence()
   {
      const char* c = "optional-presence";
      // min(V) > 0: no selector. None is zero bytes, Some is the bare V.
      literal_all<Opt<u32>>(c, "u32_none", std::nullopt, {});
      literal_all<Opt<u32>>(c, "u32_some", 9, hx("09000000"));
      literal_all<Opt<u32>>(c, "u32_some_zero", 0, hx("00000000"));
      literal_all<Opt<Color>>(c, "enum_none", std::nullopt, {});
      literal_all<Opt<Color>>(c, "enum_some", Color::Green, hx("01"));
      literal_all<Opt<Color>>(c, "enum_some_zero", Color::Red, hx("00"));
      literal_all<Opt<Pair>>(c, "dwnc_pair_none", std::nullopt, {});
      literal_all<Opt<Pair>>(c, "dwnc_pair_some", Pair{1, 2}, hx("0102"));
      literal_all<Opt<VarBS>>(c, "variant_none", std::nullopt, {});
      literal_all<Opt<VarBS>>(c, "variant_some", VarBS{std::in_place_index<0>, 5}, hx("0005"));
      literal_all<Opt<VarBS>>(c, "variant_some_empty_string", VarBS{std::in_place_index<1>, ""},
                              hx("01"));
      literal_all<Opt<Bits12>>(c, "bitlist_none", std::nullopt, {});
      literal_all<Opt<Bits12>>(c, "bitlist_some_empty", Bits12{}, hx("01"));
      literal_all<Opt<Bits12>>(c, "bitlist_some", bits("101"), hx("0d"));
      // min(V) == 0: one selector byte.
      literal_all<Opt<Raw>>(c, "string_none", std::nullopt, hx("00"));
      literal_all<Opt<Raw>>(c, "string_some_empty", Raw(), hx("01"));
      literal_all<Opt<Raw>>(c, "string_some", Raw("hi"), hx("016869"));
      literal_all<Opt<Vec<u16>>>(c, "vector_none", std::nullopt, hx("00"));
      literal_all<Opt<Vec<u16>>>(c, "vector_some_empty", Vec<u16>{}, hx("01"));
      literal_all<Opt<Vec<u16>>>(c, "vector_some", Vec<u16>{1, 2}, hx("0101000200"));
      literal_all<Opt<Unit>>(c, "unit_none", std::nullopt, hx("00"));
      literal_all<Opt<Unit>>(c, "unit_some", Unit{}, hx("01"));
      literal_all<Opt<std::array<u32, 0>>>(c, "empty_array_none", std::nullopt, hx("00"));
      literal_all<Opt<std::array<u32, 0>>>(c, "empty_array_some", std::array<u32, 0>{},
                                           hx("01"));
      // Nested optionals. optional<u32> can be empty, so the outer one has a
      // selector; optional<string> always has its own selector byte, so the
      // outer one has none.
      using OU = Opt<OptU32>;
      literal_all<OU>(c, "optional_u32_none", std::nullopt, hx("00"));
      literal_all<OU>(c, "optional_u32_some_none", OU{std::in_place, std::nullopt}, hx("01"));
      literal_all<OU>(c, "optional_u32_some_some", OU{std::in_place, 9}, hx("0109000000"));
      using OS = Opt<Opt<Raw>>;
      literal_all<OS>(c, "optional_string_none", std::nullopt, {});
      literal_all<OS>(c, "optional_string_some_none", OS{std::in_place, std::nullopt}, hx("00"));
      literal_all<OS>(c, "optional_string_some_empty", OS{std::in_place, Raw()}, hx("01"));
      literal_all<OS>(c, "optional_string_some_some", OS{std::in_place, Raw("h")}, hx("0168"));
      // Width-dependent payloads.
      formats([&]<typename F>() {
         const std::size_t WE = wof<F, Empty>();
         literal<F, Opt<Empty>>(c, "empty_record_none", std::nullopt, {});
         literal<F, Opt<Empty>>(c, "empty_record_some", Empty{}, Fr{WE}.o(0));
         const std::size_t WI = wof<F, Inner>();
         literal<F, Opt<Inner>>(c, "record_none", std::nullopt, {});
         literal<F, Opt<Inner>>(c, "record_some", Inner{3, "t"},
                                Fr{WI}.o(1 + WI).byte(3).o(1 + WI).raw("t"));
         // A non-empty array of variable elements always has its offset
         // table, so it cannot be empty: no selector.
         const std::size_t WA = wof<F, Arr2<Raw>>();
         literal<F, Opt<Arr2<Raw>>>(c, "array_of_strings_none", std::nullopt, {});
         literal<F, Opt<Arr2<Raw>>>(c, "array_of_strings_some_empty", Arr2<Raw>{"", ""},
                                    Fr{WA}.o(2 * WA).o(2 * WA));
         literal<F, Opt<Arr2<Raw>>>(c, "array_of_strings_some", Arr2<Raw>{"a", "b"},
                                    Fr{WA}.o(2 * WA).o(2 * WA + 1).raw("ab"));
      });

      // Every presence class as a middle field (never pruned) and as a
      // trailing field (pruned when absent).
      auto fill = [](auto& p, int field, bool empty_form) {
         switch (field)
         {
            case 0: p.o_u32 = empty_form ? 0u : 9u; break;
            case 1: p.o_str = empty_form ? Raw() : Raw("hi"); break;
            case 2: p.o_vec = empty_form ? Vec<u16>{} : Vec<u16>{1, 2}; break;
            case 3: p.o_unit = Unit{}; break;
            case 4: p.o_empty = Empty{}; break;
            case 5: p.o_pair = empty_form ? Pair{0, 0} : Pair{1, 2}; break;
            case 6: p.o_rec = empty_form ? Inner{0, ""} : Inner{3, "t"}; break;
            case 7:
               p.o_var = empty_form ? VarBS{std::in_place_index<1>, ""}
                                    : VarBS{std::in_place_index<0>, 5};
               break;
            case 8: p.o_bits = empty_form ? Bits12{} : bits("101"); break;
            case 9: p.o_arr = empty_form ? Arr2<Raw>{"", ""} : Arr2<Raw>{"a", "b"}; break;
            case 10: p.o_arr0 = std::array<u32, 0>{}; break;
            case 11:
               p.o_oo = empty_form ? Opt<OptU32>{std::in_place, std::nullopt}
                                   : Opt<OptU32>{std::in_place, 5};
               break;
            case 12:
               p.o_oos = empty_form ? Opt<Opt<Raw>>{std::in_place, std::nullopt}
                                    : Opt<Opt<Raw>>{std::in_place, Raw("s")};
               break;
            case 13: p.o_enum = empty_form ? Color::Red : Color::Green; break;
         }
      };
      static constexpr const char* fields[]{"u32", "string", "vector", "unit", "empty_record",
                                           "dwnc_pair", "record", "variant", "bitlist",
                                           "array_of_strings", "empty_array", "optional_u32",
                                           "optional_string", "enum"};
      auto records = [&]<typename P>(const std::string& where) {
         auto make = [] {
            P p{};
            p.head = 1;
            if constexpr (requires { p.tail; }) p.tail = 2;
            return p;
         };
         corpus(c, where + "/all_none", make());
         P all = make(), all_empty = make();
         for (int i = 0; i < 14; ++i)
         {
            fill(all, i, false);
            fill(all_empty, i, true);
            P one = make(), one_empty = make();
            fill(one, i, false);
            fill(one_empty, i, true);
            corpus(c, where + "/only_" + fields[i], one);
            corpus(c, where + "/only_" + fields[i] + "_empty_form", one_empty);
         }
         corpus(c, where + "/all_some", all);
         corpus(c, where + "/all_some_empty_form", all_empty);
      };
      records.template operator()<PresMid>("middle");
      records.template operator()<PresTail>("trailing");
   }

   // Mix<P, C> built by hand: `own` is the parent's width, `cw` the child's,
   // `vw` / `aw` the widths of the vector and array offset tables.
   Bytes mix_frame(std::size_t own, std::size_t cw, std::size_t vw, std::size_t aw,
                   std::string_view first = "a")
   {
      const Bytes c1 = rec_as(cw, 1, first);
      const Bytes c2 = rec_as(cw, 2, "bc");
      return slots_of(own, {c1, table_of(vw, {c2}), table_of(aw, {c1, c2}), c1,
                            join({hx("01"), c2})});
   }

   void emit_width()
   {
      const char* c = "width";
      // ── Static facts the bytes below rely on ───────────────────────────
      static_assert(psio::auto_pssz_width_v<C1a> == 1 && psio::auto_pssz_width_v<C2a> == 2 &&
                    psio::auto_pssz_width_v<R0> == 4);
      static_assert(psio::auto_pssz_width_v<WidthCheckpoint> == 1);
      static_assert(psio::auto_pssz_width_v<WidthLargeBudget> == 4);      // cap > needed
      static_assert(psio::auto_pssz_width_v<WidthNarrowEnvelope> == 1);   // cap < inferred
      static_assert(psio::auto_pssz_width_v<WidthStringPair> == 4 &&
                    psio::auto_pssz_width_v<WidthCappedPair> == 1);
      static_assert(psio::max_encoded_size<S247>() == 255 && psio::auto_pssz_width_v<S247> == 1);
      static_assert(psio::max_encoded_size<S248>() == 256 && psio::auto_pssz_width_v<S248> == 2);
      static_assert(psio::max_encoded_size<S65527>() == 65535 &&
                    psio::auto_pssz_width_v<S65527> == 2);
      static_assert(psio::max_encoded_size<S65528>() == 65536 &&
                    psio::auto_pssz_width_v<S65528> == 4);
      static_assert(psio::max_encoded_size<WidthThreshold251>() == 255 &&
                    psio::auto_pssz_width_v<WidthThreshold251> == 1);
      static_assert(psio::max_encoded_size<WidthThreshold252>() == 256 &&
                    psio::auto_pssz_width_v<WidthThreshold252> == 2);
      // A vector's offset table belongs to the vector type, which has no
      // bound of its own: four bytes, even as a bounded member of a narrow
      // parent. An array's table follows the array's inferred bound.
      static_assert(psio::auto_pssz_width_v<NarrowBatch> == 1 &&
                    psio::auto_pssz_width_v<Vec<Tiny>> == 4 && psio::auto_pssz_width_v<Tiny> == 1);
      static_assert(psio::auto_pssz_width_v<Vec<C1a>> == 4 && psio::auto_pssz_width_v<Vec<C2a>> == 4);
      static_assert(psio::auto_pssz_width_v<Arr2<C1a>> == 2 &&
                    psio::auto_pssz_width_v<Arr2<C2a>> == 2 && psio::auto_pssz_width_v<Arr2<R0>> == 4);
      static_assert(psio::auto_pssz_width_v<Mix1x1> == 1 && psio::auto_pssz_width_v<Mix1x4> == 1 &&
                    psio::auto_pssz_width_v<Mix2x1> == 2 && psio::auto_pssz_width_v<Mix2x4> == 2 &&
                    psio::auto_pssz_width_v<Mix4x1> == 4 && psio::auto_pssz_width_v<Mix4x4> == 4);

      // A bounded UTF-8 string member bounds its record: no annotation needed.
      static_assert(psio::max_encoded_size<T16>() == 28 && psio::auto_pssz_width_v<T16> == 1);
      static_assert(psio::max_encoded_size<T300>() == 312 && psio::auto_pssz_width_v<T300> == 2);

      using A = psio::pssz;
      // ── Pinned automatic-profile bytes ─────────────────────────────────
      literal<A, T16>(c, "bounded_string_member_width_1", {7, psio::utf8_string<16>{"hi"}},
                      hx("0507000000056869"));
      literal<A, T300>(c, "bounded_string_member_width_2", {7, psio::utf8_string<300>{"hi"}},
                       hx("06000700000006006869"));
      corpus(c, "t16_full", T16{7, psio::utf8_string<16>{"0123456789abcdef"}});
      corpus(c, "t16_empty", T16{0, {}});
      corpus(c, "t300_full", T300{7, psio::utf8_string<300>{std::string(300, 's')}});
      corpus(c, "t300_empty", T300{0, {}});
      formats([&]<typename F>() {
         const std::size_t W = wof<F, T16>();
         add<F, T16>(c, "bounded_string_member_over_bound",
                     rec_as(W, 7, "0123456789abcdefg"),
                     reject("pssz: utf8_string exceeds its bound"));
         add<F, T16>(c, "bounded_string_member_not_utf8", rec_as(W, 7, "\xff"),
                     reject("utf8_spec: invalid leading byte"));
      });

      literal<A, WidthCheckpoint>(c, "checkpoint", {7, "main"}, hx("0507000000056d61696e"));
      literal<A, S247>(c, "threshold_255", {"a"}, hx("010161"));
      literal<A, S248>(c, "threshold_256", {"a"}, hx("0200020061"));
      literal<A, S65527>(c, "threshold_65535", {"a"}, hx("0200020061"));
      literal<A, S65528>(c, "threshold_65536", {"a"}, hx("040000000400000061"));
      literal<A, WidthLargeBudget>(c, "cap_larger_than_needed", {5}, hx("0100000005"));
      literal<A, WidthNarrowEnvelope>(c, "cap_smaller_than_inferred", {{5}, 9},
                                      hx("0505090000000100000005"));
      literal<A, WidthReserveParent>(c, "reserve_parent", {{5}}, hx("04000000040000000100000005"));
      literal<A, WidthStringPair>(c, "uncapped_pair", {"a", "b"},
                                  hx("0800000008000000090000006162"));
      literal<A, WidthCappedPair>(c, "capped_pair", {"a", "b"}, hx("0202036162"));
      literal<A, WidthGrowV1>(c, "grow_v1", {7}, hx("040007000000"));
      literal<A, WidthGrowParentV2>(c, "grow_parent_v2", {{7, 9}},
                                    hx("02000200" "0600070000000600" "09000000"));
      // NarrowBatch: one-byte record, four-byte vector table, one-byte elements.
      literal<A, NarrowBatch>(c, "bounded_vector_in_narrow_parent", {{Tiny{1}, Tiny{2}}},
                              hx("0101" "080000000a000000" "0101" "0102"));
      literal<A, Vec<Tiny>>(c, "vector_of_narrow", {Tiny{1}, Tiny{2}},
                            hx("080000000a000000" "0101" "0102"));

      // Explicit tags: the same types, one inherited width.
      formats([&]<typename F>() {
         if constexpr (!std::is_same_v<F, psio::pssz>)
         {
            const std::size_t W = F::offset_width;
            literal<F, WidthCheckpoint>(c, "checkpoint", {7, "main"},
                                        Fr{W}.o(4 + W).u32(7).o(4 + W).raw("main"));
            literal<F, S248>(c, "threshold_256", {"a"}, Fr{W}.o(W).o(W).raw("a"));
            literal<F, S65528>(c, "threshold_65536", {"a"}, Fr{W}.o(W).o(W).raw("a"));
            literal<F, WidthLargeBudget>(c, "cap_larger_than_needed", {5}, Fr{W}.o(1).byte(5));
            literal<F, WidthNarrowEnvelope>(
               c, "cap_smaller_than_inferred", {{5}, 9},
               Fr{W}.o(W + 4).o(W + 4).u32(9).cat(Fr{W}.o(1).byte(5)));
            literal<F, NarrowBatch>(
               c, "bounded_vector_in_narrow_parent", {{Tiny{1}, Tiny{2}}},
               slots_of(W, {table_of(W, {Fr{W}.o(1).byte(1), Fr{W}.o(1).byte(2)})}));
         }
      });

      // ── Corpus values of the width fixture ─────────────────────────────
      corpus(c, "relay_sample",
             WidthRelayBatch{{7, "main"},
                             {{8, "one"}, {9, "two"}},
                             WidthCheckpoint{10, "last"},
                             {{{11, "left"}, {12, "right"}}},
                             WidthCheckpoint{13, "proof"},
                             "relay"});
      corpus(c, "relay_empty",
             WidthRelayBatch{{0, ""}, {}, std::nullopt, {{{0, ""}, {0, ""}}}, u32{42}, ""});
      corpus(c, "checkpoint_empty", WidthCheckpoint{0, ""});
      corpus(c, "checkpoint_full_label", WidthCheckpoint{0xffffffffu, "0123456789abcdef"});
      corpus(c, "checkpoint_vector", Vec<WidthCheckpoint>{{8, "one"}, {9, "two"}});
      corpus(c, "checkpoint_array", Arr2<WidthCheckpoint>{{{11, "left"}, {12, "right"}}});
      corpus(c, "checkpoint_some", Opt<WidthCheckpoint>{WidthCheckpoint{10, "last"}});
      corpus(c, "checkpoint_none", Opt<WidthCheckpoint>{});
      corpus(c, "checkpoint_variant_record",
             std::variant<WidthCheckpoint, u32>{WidthCheckpoint{13, "proof"}});
      corpus(c, "checkpoint_variant_number", std::variant<WidthCheckpoint, u32>{u32{42}});
      corpus(c, "narrow_envelope", WidthNarrowEnvelope{{17}, 42});
      corpus(c, "large_budget", WidthLargeBudget{17});
      corpus(c, "reserve_parent_17", WidthReserveParent{{17}});
      corpus(c, "threshold_251", WidthThreshold251{});
      corpus(c, "threshold_252", WidthThreshold252{});
      {
         WidthThreshold251 a{};
         WidthThreshold252 b{};
         for (std::size_t i = 0; i < a.value.size(); ++i) a.value[i] = static_cast<u8>(i);
         for (std::size_t i = 0; i < b.value.size(); ++i) b.value[i] = static_cast<u8>(255 - i);
         corpus(c, "threshold_251_pattern", a);
         corpus(c, "threshold_252_pattern", b);
      }
      corpus(c, "bounded_batch_empty", WidthBoundedBatch{});
      corpus(c, "bounded_batch_full",
             WidthBoundedBatch{{{1, "a"}, {2, "bb"}, {3, "ccc"}, {4, "dddd"}}});
      corpus(c, "string_pair", WidthStringPair{"first", "second"});
      corpus(c, "capped_pair_words", WidthCappedPair{"first", "second"});
      corpus(c, "capped_pair_at_cap", WidthCappedPair{std::string(126, 'a'), std::string(126, 'b')});
      corpus(c, "s247_max", S247{std::string(247, 's')});
      corpus(c, "s248_max", S248{std::string(248, 's')});
      corpus(c, "s65527_small", S65527{"small"});
      corpus(c, "s65528_small", S65528{"small"});
      corpus(c, "tiny", Tiny{9});
      corpus(c, "narrow_batch_empty", NarrowBatch{});
      corpus(c, "narrow_batch_full", NarrowBatch{{Tiny{1}, Tiny{2}}});

      // ── Every parent width over every child width ──────────────────────
      auto mix = [&]<typename M, typename C>(std::size_t own, std::size_t cw) {
         static_assert(psio::auto_pssz_width_v<M> == 0 || true);
         const std::size_t aw = cw == 4 ? 4 : 2;  // array<C,2> inferred bound
         const C           c1{1, "a"}, c2{2, "bc"};
         const M value{c1, {c2}, {c1, c2}, c1, std::variant<u32, C>{std::in_place_index<1>, c2}};
         if (psio::auto_pssz_width_v<M> != own || psio::auto_pssz_width_v<C> != cw ||
             psio::auto_pssz_width_v<Arr2<C>> != aw || psio::auto_pssz_width_v<Vec<C>> != 4)
            die("Mix width expectations are stale");
         literal<A, M>(c, "mix_sample", value, mix_frame(own, cw, 4, aw));
         formats([&]<typename F>() {
            if constexpr (!std::is_same_v<F, psio::pssz>)
            {
               const std::size_t W = F::offset_width;
               literal<F, M>(c, "mix_sample", value, mix_frame(W, W, W, W));
            }
         });
         corpus(c, "mix_minimal",
                M{C{0, ""}, {}, {C{0, ""}, C{0, ""}}, std::nullopt, std::variant<u32, C>{u32{0}}});
         corpus(c, "mix_three_kids",
                M{c2, {c1, c2, c1}, {c2, c2}, std::nullopt, std::variant<u32, C>{u32{9}}});
      };
      mix.template operator()<Mix1x1, C1a>(1, 1);
      mix.template operator()<Mix1x2, C2a>(1, 2);
      mix.template operator()<Mix1x4, R0>(1, 4);
      mix.template operator()<Mix2x1, C1a>(2, 1);
      mix.template operator()<Mix2x2, C2a>(2, 2);
      mix.template operator()<Mix2x4, R0>(2, 4);
      mix.template operator()<Mix4x1, C1a>(4, 1);
      mix.template operator()<Mix4x2, C2a>(4, 2);
      mix.template operator()<Mix4x4, R0>(4, 4);

      // ── Cap overflow ───────────────────────────────────────────────────
      // The cap bounds the whole encoding of the capped type.
      const std::string cap = "pssz: encoded size exceeds maxDynamicData cap";
      formats([&]<typename F>() {
         constexpr bool    is_auto = std::is_same_v<F, psio::pssz>;
         const std::size_t W       = wof<F, C1a>();
         const std::size_t fits    = 200 - 4 - 2 * W;  // s length for exactly 200 bytes
         const Bytes at   = rec_as(W, 7, std::string(fits, 'a'));
         const Bytes over = rec_as(W, 7, std::string(fits + 1, 'a'));
         add<F, C1a>(c, "cap/at_cap", at, canonical());
         add<F, C1a>(c, "cap/over_cap", over, reject(cap));
         add<F, C1b>(c, "cap/over_cap", over, reject(cap));
         const std::size_t WC = wof<F, WidthCheckpoint>();
         // Within the 64-byte cap, but the label exceeds its member
         // length_bound of 16: the member bound rejects it.
         add<F, WidthCheckpoint>(c, "cap/checkpoint_at_cap",
                                 rec_as(WC, 7, std::string(64 - 4 - 2 * WC, 'a')),
                                 reject("pssz: member length_bound violated"));
         add<F, WidthCheckpoint>(c, "cap/checkpoint_over_cap",
                                 rec_as(WC, 7, std::string(65 - 4 - 2 * WC, 'a')), reject(cap));

         // Nested. The automatic profile checks every capped type where it
         // occurs. Explicit-width tags check only the top-level type's cap
         // (they keep one inherited width and never look up a nested type's
         // budget): those vectors are recorded as observed in their own
         // class, without an expectation.
         const std::string nc = is_auto ? c : "cap-overflow-explicit-nested";
         const Want        nw = is_auto ? reject(cap) : record_only();
         add<F, Vec<C1a>>(nc, "cap/over_cap_in_vector",
                          table_of(wof<F, Vec<C1a>>(), {rec_as(W, 1, "a"), over}), nw);
         add<F, Opt<C1a>>(nc, "cap/over_cap_in_optional", over, nw);
         add<F, VarU<C1a>>(nc, "cap/over_cap_in_variant", join({hx("01"), over}), nw);
         add<F, TupS<C1a>>(nc, "cap/over_cap_in_tuple",
                           slots_of(wof<F, TupS<C1a>>(), {over, text("tail")}), nw);
         const std::size_t M = wof<F, Mix4x1>();
         add<F, Mix4x1>(nc, "cap/over_cap_child",
                        mix_frame(M, W, wof<F, Vec<C1a>>(), wof<F, Arr2<C1a>>(),
                                  std::string(fits + 1, 'a')),
                        nw);
         add<F, Vec<C1a>>(c, "cap/at_cap_in_vector",
                          table_of(wof<F, Vec<C1a>>(), {rec_as(W, 1, "a"), at}), canonical());
      });
      // The encoder refuses the same values.
      {
         Bytes out;
         if (try_encode<psio::pssz>(C1a{7, std::string(195, 'a')}, out))
            die("the automatic encoder accepted a value over its cap");
         if (try_encode<psio::pssz>(Vec<C1a>{C1a{7, std::string(195, 'a')}}, out))
            die("the automatic encoder accepted a nested value over its cap");
         if (try_encode<psio::pssz8>(C1a{7, std::string(195, 'a')}, out))
            die("the pssz8 encoder accepted a value over its cap");
      }
   }

   void emit_length_bound_overflow()
   {
      // A member length_bound narrows the inferred width and is enforced by
      // validate and validate_strict (and so by the checked decode and view
      // entry points) and by the encoder, which refuses an out-of-bound
      // value. These frames are what a writer that skips the bound check
      // would produce; every reader rejects them.
      const char* c = "length-bound-overflow";
      static_assert(psio::max_encoded_size<Bounded>() == 20 && psio::auto_pssz_width_v<Bounded> == 1);
      corpus("corpus", "bounded_within", Bounded{"1234", {1, 2}});
      corpus("corpus", "bounded_empty", Bounded{"", {}});
      auto observed = [&]<typename T>(const std::string& vname, const T& value) {
         formats([&]<typename F>() {
            Bytes bytes;
            try
            {
               bytes = psio::detail::pssz_impl::encode_ignoring_length_bounds(F{}, value);
            }
            catch (const psio::codec_exception&)
            {
               return;
            }
            Bytes refused;
            if (try_encode<F>(value, refused))
               die("the encoder accepted a value outside a member length_bound");
            if (psio::encode_pssz_checked(F{}, value, refused).ok() || !refused.empty())
               die("encode_pssz_checked accepted a value outside a member length_bound");
            add<F, T>(c, vname, bytes, reject("pssz: member length_bound violated"), vname,
                      type_id<T>());
         });
      };
      observed("string_one_over", Bounded{"12345", {}});
      observed("vector_one_over", Bounded{"", {1, 2, 3}});
      observed("both_over", Bounded{"123456789", {1, 2, 3, 4, 5}});
      observed("checkpoint_label_17", WidthCheckpoint{7, "0123456789abcdefg"});
      observed("narrow_batch_three", NarrowBatch{{Tiny{1}, Tiny{2}, Tiny{3}}});
      observed("bounded_batch_five",
               WidthBoundedBatch{{{1, "a"}, {2, "b"}, {3, "c"}, {4, "d"}, {5, "e"}}});
      observed("s247_with_248_bytes", S247{std::string(248, 's')});
   }

   void emit_tuples_variants_enums()
   {
      // ── Tuples: positional extensible records ──────────────────────────
      formats([&]<typename F>() {
         const char* c = "tuple";
         {
            const std::size_t W = wof<F, std::tuple<>>();
            literal<F, std::tuple<>>(c, "empty", {}, Fr{W}.o(0));
         }
         {
            const std::size_t W = wof<F, std::tuple<u32>>();
            literal<F, std::tuple<u32>>(c, "one_fixed", {9}, Fr{W}.o(4).u32(9));
         }
         {
            const std::size_t W = wof<F, std::tuple<Raw>>();
            literal<F, std::tuple<Raw>>(c, "one_string", {"hi"}, Fr{W}.o(W).o(W).raw("hi"));
            literal<F, std::tuple<Raw>>(c, "one_empty_string", {""}, Fr{W}.o(W).o(W));
         }
         {
            const std::size_t W = wof<F, TupBS>();
            literal<F, TupBS>(c, "fixed_and_string", {1, "t"},
                              Fr{W}.o(1 + W).byte(1).o(1 + W).raw("t"));
         }
         {
            using T             = std::tuple<u32, Raw, std::optional<u16>>;
            const std::size_t W = wof<F, T>();
            literal<F, T>(c, "three_optional_none", {7, "hi", std::nullopt},
                          Fr{W}.o(4 + W).u32(7).o(4 + W).raw("hi"));
            literal<F, T>(c, "three_optional_some", {7, "hi", 9},
                          Fr{W}.o(4 + 2 * W).u32(7).o(4 + 2 * W).o(4 + 2 * W + 2).raw("hi").ow(2, 9));
         }
         {
            const std::size_t W = wof<F, TupRec>();
            const std::size_t T = wof<F, TupBS>();
            literal<F, TupRec>(c, "in_record", {{1, "t"}, 5},
                               Fr{W}.o(W + 1).o(W + 1).byte(5).cat(
                                  Fr{T}.o(1 + T).byte(1).o(1 + T).raw("t")));
         }
         {
            const std::size_t V = wof<F, Vec<TupBS>>();
            const std::size_t T = wof<F, TupBS>();
            literal<F, Vec<TupBS>>(c, "in_vector", {{1, "t"}, {2, ""}},
                                   table_of(V, {Fr{T}.o(1 + T).byte(1).o(1 + T).raw("t"),
                                                Fr{T}.o(1 + T).byte(2).o(1 + T)}));
         }
         {
            using Alt           = std::tuple<u8, std::optional<u16>>;
            using V             = std::variant<Alt, u32>;
            const std::size_t T = wof<F, Alt>();
            literal<F, V>(c, "in_variant_pruned", V{Alt{1, std::nullopt}},
                          join({hx("00"), Fr{T}.o(1).byte(1)}));
            literal<F, V>(c, "in_variant", V{Alt{1, 9}},
                          join({hx("00"), Fr{T}.o(1 + T).byte(1).o(1 + T).ow(2, 9)}));
            literal<F, V>(c, "in_variant_other", V{u32{9}}, hx("0109000000"));
            add<F, V>("pruning", "tuple/stored_none_in_variant",
                      join({hx("00"), Fr{T}.o(1 + T).byte(1).o(1 + T)}),
                      reject("pssz: trailing absent optional field was not pruned"));
         }
      });

      // ── Variants: one index byte, then the alternative ─────────────────
      {
         const char* c = "variant";
         literal_all<VarUS>(c, "u32", VarUS{std::in_place_index<0>, 5}, hx("0005000000"));
         literal_all<VarUS>(c, "string", VarUS{std::in_place_index<1>, "hi"}, hx("016869"));
         literal_all<VarUS>(c, "empty_string", VarUS{std::in_place_index<1>, ""}, hx("01"));
         literal_all<Var5>(c, "index0_scalar", Var5{std::in_place_index<0>, 5}, hx("0005000000"));
         literal_all<Var5>(c, "index1_string", Var5{std::in_place_index<1>, "hi"}, hx("016869"));
         literal_all<Var5>(c, "index4_unit", Var5{std::in_place_index<4>}, hx("04"));
         formats([&]<typename F>() {
            const std::size_t I = wof<F, Inner>();
            const std::size_t T = wof<F, TupBS>();
            literal<F, Var5>(c, "index2_record", Var5{std::in_place_index<2>, Inner{3, "t"}},
                             join({hx("02"), Fr{I}.o(1 + I).byte(3).o(1 + I).raw("t")}));
            literal<F, Var5>(c, "index3_tuple", Var5{std::in_place_index<3>, TupBS{1, "t"}},
                             join({hx("03"), Fr{T}.o(1 + T).byte(1).o(1 + T).raw("t")}));
         });
         literal_all<VarBS>(c, "u8", VarBS{std::in_place_index<0>, 5}, hx("0005"));
         literal_all<VarBS>(c, "u8_or_string", VarBS{std::in_place_index<1>, "hi"}, hx("016869"));
         formats([&]<typename F>() {
            const std::size_t I = wof<F, Inner>();
            literal<F, Inner>("corpus", "inner", Inner{3, "t"},
                              Fr{I}.o(1 + I).byte(3).o(1 + I).raw("t"));
         });
         literal_all<std::variant<Color, E64>>(c, "enum_u8", std::variant<Color, E64>{Color::Blue},
                                               hx("0002"));
         literal_all<std::variant<Color, E64>>(c, "enum_u64", std::variant<Color, E64>{E64::Big},
                                               hx("010807060504030201"));
      }

      // ── Enums: the underlying integer ──────────────────────────────────
      {
         const char* c = "enum";
         literal_all<Color>(c, "u8_red", Color::Red, hx("00"));
         literal_all<Color>(c, "u8_blue", Color::Blue, hx("02"));
         literal_all<E16>(c, "u16_lo", E16::Lo, hx("0100"));
         literal_all<E16>(c, "u16_hi", E16::Hi, hx("0201"));
         literal_all<E32>(c, "i32_neg", E32::Neg, hx("feffffff"));
         literal_all<E32>(c, "i32_zero", E32::Zero, hx("00000000"));
         literal_all<E32>(c, "i32_pos", E32::Pos, hx("04030201"));
         literal_all<E64>(c, "u64_zero", E64::Zero, hx("0000000000000000"));
         literal_all<E64>(c, "u64_big", E64::Big, hx("0807060504030201"));
         literal_all<Vec<Color>>(c, "vector_u8", {Color::Red, Color::Green, Color::Blue},
                                 hx("000102"));
         literal_all<Vec<Color>>(c, "vector_empty", {}, {});
         literal_all<Vec<E16>>(c, "vector_u16", {E16::Lo, E16::Hi}, hx("01000201"));
         literal_all<Arr2<E32>>(c, "array_i32", {E32::Neg, E32::Pos}, hx("feffffff04030201"));
         literal_all<Opt<Color>>(c, "optional_none", std::nullopt, {});
         literal_all<Opt<Color>>(c, "optional_some", Color::Blue, hx("02"));
         literal_all<Opt<E32>>(c, "optional_i32_some", E32::Neg, hx("feffffff"));
         corpus(c, "record",
                EnumRec{Color::Blue, E16::Hi, E32::Neg, E64::Big, {Color::Green, Color::Red},
                        E32::Pos});
         corpus(c, "record_pruned",
                EnumRec{Color::Red, E16::Lo, E32::Zero, E64::Zero, {}, std::nullopt});
         formats([&]<typename F>() {
            const std::size_t W = wof<F, EnumRec>();
            literal<F, EnumRec>(
               c, "record_literal",
               EnumRec{Color::Blue, E16::Hi, E32::Neg, E64::Big, {Color::Green}, std::nullopt},
               Fr{W}.o(15 + W).byte(2).ow(2, 258).cat(hx("feffffff0807060504030201")).o(15 + W)
                  .byte(1));
         });
         // A value that names no enumerator. C++ carries the underlying
         // integer through unchanged in every entry point.
         const char* u = "enum-unknown-value";
         frame_all<Color>(u, "u8_0x55", hx("55"), canonical());
         frame_all<Color>(u, "u8_0xff", hx("ff"), canonical());
         frame_all<E16>(u, "u16_zero", hx("0000"), canonical());
         frame_all<E32>(u, "i32_minus_one", hx("ffffffff"), canonical());
         frame_all<E64>(u, "u64_max", hx("ffffffffffffffff"), canonical());
         frame_all<Vec<Color>>(u, "in_vector", hx("0055ff"), canonical());
         frame_all<Opt<Color>>(u, "in_optional", hx("55"), canonical());
         frame_all<std::variant<Color, E64>>(u, "in_variant", hx("0055"), canonical());
         formats([&]<typename F>() {
            const std::size_t W = wof<F, E1>();
            add<F, E1>(u, "in_record",
                       Fr{W}.o(4 + 2 * W).u32(7).o(4 + 2 * W).o(4 + 2 * W + 2).raw("hi").byte(0x55),
                       canonical());
         });
      }
   }

   // Boxed values: the bytes are the inner type's; the offset width of the
   // container is what the box changes (see the types).
   void emit_boxes()
   {
      const char* c    = "box";
      auto        leaf = [](u16 n, std::string_view t) {
         return BLeaf{n, psio::utf8_string<4>{std::string(t)}};
      };
      auto boxed = [&](u16 n, std::string_view t) { return Boxed<BLeaf>{leaf(n, t)}; };
      static_assert(psio::auto_pssz_width_v<BDirect> == 1);
      static_assert(psio::auto_pssz_width_v<BoxRec> == 4);
      static_assert(psio::auto_pssz_width_v<BoxCapRec> == 1);
      static_assert(psio::auto_pssz_width_v<BoxOptRec> == 4);
      static_assert(psio::auto_pssz_width_v<BoxVecRec> == 4);
      static_assert(psio::auto_pssz_width_v<BoxVecCapRec> == 2);
      corpus<BLeaf>(c, "leaf", leaf(7, "abcd"));
      corpus<BDirect>(c, "direct", BDirect{1, leaf(2, "ab")});
      corpus<BoxRec>(c, "record", BoxRec{1, boxed(2, "ab")});
      corpus<BoxRec>(c, "record_empty_text", BoxRec{0, boxed(0, "")});
      corpus<BoxCapRec>(c, "record_capped", BoxCapRec{1, boxed(2, "ab")});
      corpus<BoxOptRec>(c, "optional_some", BoxOptRec{3, boxed(4, "xyz"), 9});
      corpus<BoxOptRec>(c, "optional_none", BoxOptRec{3, std::nullopt, 9});
      {
         BoxVecRec v{5, {}};
         corpus<BoxVecRec>(c, "vector_empty", v);
         v.leaves.push_back(boxed(1, "a"));
         v.leaves.push_back(boxed(2, ""));
         v.leaves.push_back(boxed(3, "abcd"));
         corpus<BoxVecRec>(c, "vector_three", v);
         BoxVecCapRec w{5, {}};
         w.leaves.push_back(boxed(1, "a"));
         w.leaves.push_back(boxed(2, "bc"));
         corpus<BoxVecCapRec>(c, "vector_capped", w);
      }
      corpus<BoxFixedRec>(c, "fixed", BoxFixedRec{1, Boxed<u32>{0x01020304u},
                                                  Boxed<std::array<u8, 2>>{{{5, 6}}}});
      corpus<BoxOuter>(c, "outer",
                       BoxOuter{1, BoxCapRec{2, boxed(3, "c")}, BoxRec{4, boxed(5, "open")},
                                BDirect{6, leaf(7, "d")}});
      corpus<BoxOuter>(c, "outer_pruned",
                       BoxOuter{1, BoxCapRec{2, boxed(3, "")}, BoxRec{4, boxed(5, "")},
                                std::nullopt});
      corpus<Boxed<BLeaf>>(c, "top_level", boxed(9, "top"));
      corpus<Vec<Boxed<BLeaf>>>(c, "in_vector", {boxed(1, "a"), boxed(2, "bb")});
      corpus<Vec<Boxed<BLeaf>>>(c, "in_vector_empty", {});
      corpus<Opt<Boxed<BLeaf>>>(c, "in_optional", boxed(1, "a"));
      corpus<Opt<Boxed<BLeaf>>>(c, "in_optional_none", std::nullopt);
      corpus<Vec<BoxRec>>(c, "records_in_vector", {BoxRec{1, boxed(2, "ab")}, BoxRec{3, boxed(4, "")}});
      corpus<Opt<BoxCapRec>>(c, "capped_in_optional", BoxCapRec{1, boxed(2, "ab")});
      corpus<std::variant<Boxed<BLeaf>, u8>>(c, "in_variant",
                                             std::variant<Boxed<BLeaf>, u8>{boxed(1, "v")});
      corpus<std::tuple<Boxed<BLeaf>, u8>>(c, "in_tuple",
                                           std::tuple<Boxed<BLeaf>, u8>{boxed(1, "t"), 2});
      // The boxed text is still a bounded UTF-8 string, and a box does not
      // make a frame of the wrong shape acceptable.
      formats([&]<typename F>() {
         const std::size_t W = wof<F, BoxRec>();
         const std::size_t L = wof<F, BLeaf>();
         auto frame = [&](const Bytes& text_bytes) {
            Fr leaf_frame{L};
            leaf_frame.o(2 + L).byte(7).byte(0).o(2 + L).cat(text_bytes);
            Fr rec{W};
            rec.o(4 + W).u32(1).o(4 + W).cat(leaf_frame);
            return Bytes(rec);
         };
         add<F, BoxRec>(c, "valid_frame", frame(text("abcd")), canonical());
         add<F, BoxRec>(c, "text_over_bound", frame(text("abcde")),
                        reject("pssz: utf8_string exceeds its bound"));
         add<F, BoxRec>(c, "text_not_utf8", frame(hx("ff")), reject());
      });
   }

   void emit_strings()
   {
      const char*       c       = "string";
      const std::string bound   = "pssz: utf8_string exceeds its bound";
      const std::string over    = "utf8_spec: overlong encoding";
      const std::string leading = "utf8_spec: invalid leading byte";
      struct Sample
      {
         const char* name;
         std::string bytes;
      };
      const Sample valid[]{{"ascii", "hi"}, {"multibyte", "h\xc3\xa9"}, {"empty", ""},
                           {"four_byte_scalar", "\xf0\x9f\x98\x80"}};
      const Sample invalid[]{{"overlong_c080", "\xc0\x80"}, {"lone_ff", "\xff"}};
      auto reason = [&](const Sample& s) { return s.bytes == "\xff" ? leading : over; };

      formats([&]<typename F>() {
         const std::size_t WR = wof<F, StrRec>();
         auto record = [&](std::string_view r, std::string_view t, std::string_view b) {
            return slots_of(WR, {text(r), text(t), text(b)});
         };
         auto list = [&]<typename L>(std::string_view s) {
            return table_of(wof<F, L>(), {text("ok"), text(s)});
         };
         auto some = [](std::string_view s) { return join({hx("01"), text(s)}); };

         for (const auto& s : valid)
         {
            const std::string n = std::string("valid_") + s.name;
            // Standalone: the bytes are the whole encoding of every string type.
            add<F, Raw>(c, n, text(s.bytes), canonical());
            add<F, Text>(c, n, text(s.bytes), canonical());
            add<F, Text4>(c, n, text(s.bytes), canonical());
            // Record field, vector element, optional payload.
            add<F, StrRec>(c, n + "/in_record", record(s.bytes, s.bytes, s.bytes), canonical());
            add<F, Vec<Raw>>(c, n + "/in_vector", list.template operator()<Vec<Raw>>(s.bytes),
                             canonical());
            add<F, Vec<Text>>(c, n + "/in_vector", list.template operator()<Vec<Text>>(s.bytes),
                              canonical());
            add<F, Vec<Text4>>(c, n + "/in_vector", list.template operator()<Vec<Text4>>(s.bytes),
                               canonical());
            add<F, Opt<Raw>>(c, n + "/in_optional", some(s.bytes), canonical());
            add<F, Opt<Text>>(c, n + "/in_optional", some(s.bytes), canonical());
            add<F, Opt<Text4>>(c, n + "/in_optional", some(s.bytes), canonical());
         }
         for (const auto& s : invalid)
         {
            const std::string n = std::string("invalid_") + s.name;
            const char*       r = "string-raw-non-utf8";
            // A raw string carries any bytes. Every entry point accepts them
            // except C++ validate_strict, which checks every plain string.
            add<F, Raw>(r, n, text(s.bytes), nonstrict());
            add<F, Vec<Raw>>(r, n + "/in_vector", list.template operator()<Vec<Raw>>(s.bytes),
                             nonstrict());
            add<F, Opt<Raw>>(r, n + "/in_optional", some(s.bytes), nonstrict());
            add<F, StrRec>(r, n + "/in_record", record(s.bytes, "ok", "ok"), nonstrict());
            add<F, R0>(r, n + "/in_r0", rec_as(wof<F, R0>(), 7, s.bytes), nonstrict());
            // The UTF-8 types reject them everywhere.
            add<F, Text>(c, n, text(s.bytes), reject(reason(s)));
            add<F, Text4>(c, n, text(s.bytes), reject(reason(s)));
            add<F, StrRec>(c, n + "/in_record_string", record("ok", s.bytes, "ok"),
                           reject(reason(s)));
            add<F, StrRec>(c, n + "/in_record_bounded_string", record("ok", "ok", s.bytes),
                           reject(reason(s)));
            add<F, Vec<Text>>(c, n + "/in_vector", list.template operator()<Vec<Text>>(s.bytes),
                              reject(reason(s)));
            add<F, Vec<Text4>>(c, n + "/in_vector", list.template operator()<Vec<Text4>>(s.bytes),
                               reject(reason(s)));
            add<F, Opt<Text>>(c, n + "/in_optional", some(s.bytes), reject(reason(s)));
            add<F, Opt<Text4>>(c, n + "/in_optional", some(s.bytes), reject(reason(s)));
         }
         // More ways not to be UTF-8.
         add<F, Text>(c, "invalid_truncated_sequence", hx("68c3"),
                      reject("utf8_spec: truncated sequence"));
         add<F, Text>(c, "invalid_bad_continuation", hx("c328"),
                      reject("utf8_spec: bad continuation"));
         add<F, Text>(c, "invalid_surrogate", hx("eda080"),
                      reject("utf8_spec: codepoint out of range"));
         add<F, Text>(c, "invalid_above_10ffff", hx("f4908080"),
                      reject("utf8_spec: codepoint out of range"));
         add<F, Raw>("string-raw-non-utf8", "invalid_surrogate", hx("eda080"), nonstrict());
         add<F, Text>(c, "valid_nul", hx("00"), canonical());
         add<F, Text>(c, "valid_max_scalar", hx("f48fbfbf"), canonical());

         // string<=4: the bound counts bytes. Exactly 4 is accepted, 5 is not.
         add<F, Text4>(c, "bounded_exactly_n", text("abcd"), canonical());
         add<F, Text4>(c, "bounded_exactly_n_multibyte", text("\xc3\xa9\xc3\xa9"), canonical());
         add<F, Text4>(c, "bounded_n_plus_one", text("abcde"), reject(bound));
         add<F, Text4>(c, "bounded_n_plus_one_multibyte", text("a\xc3\xa9\xc3\xa9"),
                       reject(bound));
         add<F, Text>(c, "unbounded_five_bytes", text("abcde"), canonical());
         add<F, Raw>(c, "raw_five_bytes", text("abcde"), canonical());
         add<F, StrRec>(c, "bounded_exactly_n/in_record", record("abcde", "abcde", "abcd"),
                        canonical());
         add<F, StrRec>(c, "bounded_n_plus_one/in_record", record("abcde", "abcde", "abcde"),
                        reject(bound));
         add<F, Vec<Text4>>(c, "bounded_exactly_n/in_vector",
                            list.template operator()<Vec<Text4>>("abcd"), canonical());
         add<F, Vec<Text4>>(c, "bounded_n_plus_one/in_vector",
                            list.template operator()<Vec<Text4>>("abcde"), reject(bound));
         add<F, Opt<Text4>>(c, "bounded_exactly_n/in_optional", some("abcd"), canonical());
         add<F, Opt<Text4>>(c, "bounded_n_plus_one/in_optional", some("abcde"), reject(bound));
         add<F, Opt<Text>>(c, "optional_none", hx("00"), canonical());
         add<F, Opt<Text>>(c, "optional_some_empty", hx("01"), canonical());
         add<F, Opt<Text4>>(c, "optional_none", hx("00"), canonical());
      });
      // The encoder refuses a non-UTF-8 utf8_string (its constructor does not).
      {
         Bytes out;
         if (try_encode<psio::pssz>(Text{std::string("\xc0\x80")}, out) ||
             try_encode<psio::pssz8>(Text4{std::string("\xff")}, out) ||
             try_encode<psio::pssz>(StrRec{"ok", Text{std::string("\xff")}, Text4{"ok"}}, out))
            die("the encoder accepted a non-UTF-8 utf8_string");
      }
      corpus("corpus", "strings", StrRec{"raw", Text{"h\xc3\xa9"}, Text4{"abcd"}});
      corpus("corpus", "strings_empty", StrRec{"", Text{}, Text4{}});
      corpus("corpus", "string_list", Vec<Text>{Text{"a"}, Text{}, Text{"\xc3\xa9"}});
   }

   void emit_bytes_and_scalars()
   {
      const char* c = "bytes";
      literal_all<Vec<u8>>(c, "byte_list", {1, 2, 3}, hx("010203"));
      literal_all<Vec<u8>>(c, "byte_list_empty", {}, {});
      literal_all<std::array<u8, 4>>(c, "byte_array", {1, 2, 3, 4}, hx("01020304"));
      literal_all<psio::bitvector<12>>(c, "bitvector_zero", {}, hx("0000"));
      literal_all<psio::bitvector<12>>(c, "bitvector_all", bitvec("111111111111"), hx("ff0f"));
      literal_all<psio::bitvector<12>>(c, "bitvector_pattern", bitvec("100000000001"), hx("0108"));
      // bitlist: bits LSB-first, then a delimiter bit one past the last bit.
      literal_all<Bits12>(c, "bitlist_empty", {}, hx("01"));
      literal_all<Bits12>(c, "bitlist_one_true", bits("1"), hx("03"));
      literal_all<Bits12>(c, "bitlist_one_false", bits("0"), hx("02"));
      literal_all<Bits12>(c, "bitlist_seven", bits("1010101"), hx("d5"));
      literal_all<Bits12>(c, "bitlist_eight", bits("11111111"), hx("ff01"));
      literal_all<Bits12>(c, "bitlist_full", bits("111111111111"), hx("ff1f"));
      literal_all<Bits12>(c, "bitlist_full_zero", bits("000000000000"), hx("0010"));
      {
         psio::uint256 v{};
         for (std::size_t i = 0; i < 32; ++i)
            reinterpret_cast<unsigned char*>(v.limb)[i] = static_cast<unsigned char>(i + 1);
         literal_all<psio::uint256>(
            c, "u256_pattern", v,
            hx("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"));
         literal_all<psio::uint256>(c, "u256_zero", psio::uint256{}, Bytes(32, 0));
         const psio::uint128 x = (psio::uint128{0x0f0e0d0c0b0a0908ull} << 64) | 0x0706050403020100ull;
         literal_all<psio::uint128>(c, "u128_pattern", x, hx("000102030405060708090a0b0c0d0e0f"));
         literal_all<psio::uint128>(c, "u128_max", ~psio::uint128{0}, Bytes(16, char(0xff)));
         literal_all<psio::int128>(c, "i128_minus_one", psio::int128{-1}, Bytes(16, char(0xff)));
         literal_all<psio::int128>(c, "i128_minus_two", psio::int128{-2},
                                   hx("feffffffffffffffffffffffffffffff"));
         literal_all<psio::int128>(c, "i128_one", psio::int128{1},
                                   hx("01000000000000000000000000000000"));
         corpus(c, "record", BytesRec{{1, 2, 3}, {1, 2, 3, 4}, bitvec("100000000001"),
                                      bits("101"), v, x, psio::int128{-2}});
         corpus(c, "record_empty", BytesRec{{}, {}, {}, {}, {}, 0, 0});
      }

      const char* s = "corpus";
      literal_all<u8>(s, "u8_max", 0xff, hx("ff"));
      literal_all<u16>(s, "u16_pattern", 0x0102, hx("0201"));
      literal_all<u32>(s, "u32_pattern", 0x01020304, hx("04030201"));
      literal_all<u64>(s, "u64_pattern", 0x0102030405060708ull, hx("0807060504030201"));
      literal_all<std::int8_t>(s, "i8_min", -128, hx("80"));
      literal_all<std::int16_t>(s, "i16_minus_two", -2, hx("feff"));
      literal_all<std::int32_t>(s, "i32_min", std::numeric_limits<std::int32_t>::min(),
                                hx("00000080"));
      literal_all<std::int64_t>(s, "i64_minus_one", -1, hx("ffffffffffffffff"));
      literal_all<float>(s, "f32_one", 1.0f, hx("0000803f"));
      literal_all<float>(s, "f32_negative_zero", -0.0f, hx("00000080"));
      literal_all<double>(s, "f64_one", 1.0, hx("000000000000f03f"));
      literal_all<double>(s, "f64_minus_two_and_a_half", -2.5, hx("00000000000004c0"));
      // NaN payloads are carried bit-for-bit.
      frame_all<float>(s, "f32_nan_payload", hx("010000ff"), canonical());
      frame_all<double>(s, "f64_nan_payload", hx("010000000000f87f"), canonical());
      corpus(s, "scalars",
             Scalars{true, 0xff, 0x0102, 0x01020304, 0x0102030405060708ull, -128, -2,
                     std::numeric_limits<std::int32_t>::min(), -1, 1.0f, -2.5});
      corpus(s, "scalars_zero", Scalars{});
      literal_all<Vec<u32>>(s, "u32_list", {1, 2}, hx("0100000002000000"));
   }

   void emit_zero_fixed()
   {
      const char* c = "zero-fixed";
      // Zero-field DWNC record: zero bytes. It cannot be counted in a vector.
      literal_all<Unit>(c, "unit", {}, {});
      literal_all<Vec<Unit>>(c, "vector_of_units_empty", {}, {});
      literal_all<Arr2<Unit>>(c, "array_of_units", {}, {});
      literal_all<Opt<Unit>>(c, "optional_unit_none", std::nullopt, hx("00"));
      literal_all<Opt<Unit>>(c, "optional_unit_some", Unit{}, hx("01"));
      {
         Bytes out;
         if (try_encode<psio::pssz>(Vec<Unit>(1), out) || try_encode<psio::pssz8>(Vec<Unit>(1), out))
            die("the encoder accepted a nonempty vector of zero-size elements");
      }
      // DWNC all-fixed record: its fields inline, no header.
      literal_all<Pair>(c, "dwnc_pair", {1, 2}, hx("0102"));
      literal_all<Vec<Pair>>(c, "vector_of_dwnc_pairs", {{1, 2}, {3, 4}}, hx("01020304"));
      literal_all<Vec<Pair>>(c, "vector_of_dwnc_pairs_empty", {}, {});
      literal_all<Arr2<Pair>>(c, "array_of_dwnc_pairs", {{{1, 2}, {3, 4}}}, hx("01020304"));
      literal_all<Opt<Pair>>(c, "optional_dwnc_pair_none", std::nullopt, {});
      literal_all<Opt<Pair>>(c, "optional_dwnc_pair_some", Pair{1, 2}, hx("0102"));
      literal_all<std::array<u32, 0>>(c, "empty_array", {}, {});
      formats([&]<typename F>() {
         // Zero-field extensible record: header only.
         const std::size_t E = wof<F, Empty>();
         literal<F, Empty>(c, "empty_record", {}, Fr{E}.o(0));
         literal<F, Vec<Empty>>(c, "vector_of_empty_records", {Empty{}, Empty{}},
                                table_of(wof<F, Vec<Empty>>(), {Fr{E}.o(0), Fr{E}.o(0)}));
         literal<F, Vec<Empty>>(c, "vector_of_empty_records_empty", {}, {});
         literal<F, Arr2<Empty>>(c, "array_of_empty_records", {},
                                 table_of(wof<F, Arr2<Empty>>(), {Fr{E}.o(0), Fr{E}.o(0)}));
         literal<F, Opt<Empty>>(c, "optional_empty_record_none", std::nullopt, {});
         literal<F, Opt<Empty>>(c, "optional_empty_record_some", Empty{}, Fr{E}.o(0));
         // DWNC record with a variable field: no header, but it has slots.
         const std::size_t D   = wof<F, DwncVar>();
         const Bytes       one = Fr{D}.byte(7).o(1 + D).raw("hi");
         const Bytes       two = Fr{D}.byte(8).o(1 + D);
         literal<F, DwncVar>(c, "dwnc_with_variable_field", {7, "hi"}, one);
         literal<F, DwncVar>(c, "dwnc_with_empty_variable_field", {8, ""}, two);
         literal<F, Vec<DwncVar>>(c, "vector_of_dwnc_variable", {{7, "hi"}, {8, ""}},
                                  table_of(wof<F, Vec<DwncVar>>(), {one, two}));
         literal<F, Arr2<DwncVar>>(c, "array_of_dwnc_variable", {{{7, "hi"}, {8, ""}}},
                                   table_of(wof<F, Arr2<DwncVar>>(), {one, two}));
         literal<F, Opt<DwncVar>>(c, "optional_dwnc_variable_none", std::nullopt, {});
         literal<F, Opt<DwncVar>>(c, "optional_dwnc_variable_some", DwncVar{7, "hi"}, one);
      });
   }

   // ── std::pair, maps and sets ───────────────────────────────────────────
   //
   // A map / set is the list of its entries / elements in strictly
   // ascending order of the key encoding of the key. `map-set`: values, read
   // as the map / set and as the list they are. `map-set-order`: lists that
   // are not strictly ascending (valid as lists) are rejected as maps / sets.
   // `value` and the list `sorted` of its entries: the same bytes, read as
   // the map / set and as the list.
   template <typename M, typename L>
   void same(const std::string& vname, const M& value, const L& sorted)
   {
      const char* c = "map-set";
      formats([&]<typename F>() {
         Bytes bytes;
         if (!try_encode<F>(value, bytes))
         {
            if (std::is_same_v<F, psio::pssz>)
               die(std::string(c) + "/" + vname + ": the automatic profile cannot encode it");
            return;
         }
         if (bytes != must_encode<F>(sorted, vname))
            die(std::string(c) + "/" + vname + " as " + type_id<M>() +
                ": not the bytes of the sorted list");
         add<F, M>(c, vname, bytes, canonical(), vname, type_id<M>());
         add<F, L>(c, vname, bytes, canonical(), vname, type_id<M>());
      });
   }
   // `list` is a valid list and not a valid M.
   template <typename M, typename L>
   void unordered(const std::string& vname, const L& list, const std::string& reason)
   {
      const char* o = "map-set-order";
      formats([&]<typename F>() {
         Bytes bytes;
         if (!try_encode<F>(list, bytes))
            return;
         add<F, M>(o, vname, bytes, reject(reason), vname, type_id<L>());
         add<F, L>(o, vname, bytes, canonical(), vname, type_id<L>());
      });
   }

   void emit_assoc()
   {
      const char*       c          = "map-set";
      const char*       o          = "map-set-order";
      const std::string map_reason = "pssz: map entries are not in strictly ascending key order";
      const std::string set_reason = "pssz: set elements are not in strictly ascending order";

      const Raw nul("\0", 1);

      // A pair is the tuple.
      for (const PairT& p : {PairT{0, ""}, PairT{513, "pair"}})
         formats([&]<typename F>() {
            if (must_encode<F>(p, "pair") != must_encode<F>(std::tuple<u16, Raw>{p.first, p.second},
                                                            "pair"))
               die("a pair is not the 2-tuple");
         });
      corpus(c, "pair_zero", PairT{0, ""});
      corpus(c, "pair_some", PairT{513, "pair"});
      literal<psio::pssz, PairT>(c, "pair_literal", PairT{7, "ab"},
                                 hx("06000000" "0700" "06000000" "6162"));

      // map<raw_string, u32>
      {
         using M = MapRawU32;
         using L = Vec<std::tuple<Raw, u32>>;
         same<M, L>("map_str_empty", {}, {});
         same<M, L>("map_str_one", {{"a", 1}}, {{"a", 1}});
         same<M, L>("map_str_many", {{"pear", 3}, {"apple", 1}, {"", 9}, {"apple pie", 2}},
                    {{"", 9}, {"apple", 1}, {"apple pie", 2}, {"pear", 3}});
         same<M, L>("map_str_nul_keys", {{"a", 2}, {"a" + nul, 1}, {nul, 0}, {"\x01", 4}},
                    {{nul, 0}, {"\x01", 4}, {"a", 2}, {"a" + nul, 1}});
         literal<psio::pssz, M>(c, "map_str_literal", {{"b", 2}, {"a", 1}},
                                hx("08000000" "15000000"
                                   "08000000" "08000000" "01000000" "61"
                                   "08000000" "08000000" "02000000" "62"));
         unordered<M, L>("map_str_swapped", {{"b", 2}, {"a", 1}}, map_reason);
         unordered<M, L>("map_str_duplicate", {{"a", 1}, {"a", 1}}, map_reason);
         unordered<M, L>("map_str_duplicate_key_other_value", {{"a", 1}, {"a", 2}}, map_reason);
         unordered<M, L>("map_str_last_out_of_place", {{"a", 1}, {"c", 3}, {"b", 2}}, map_reason);
         unordered<M, L>("map_str_prefix_after", {{"ab", 1}, {"a", 2}}, map_reason);
      }
      // map<u32, raw_string>: numeric order, not the order of the wire bytes.
      {
         using M = MapU32Raw;
         using L = Vec<std::tuple<u32, Raw>>;
         same<M, L>("map_u32_empty", {}, {});
         same<M, L>("map_u32_many", {{70000, "big"}, {1, "one"}, {256, "x"}, {255, "ff"}},
                    {{1, "one"}, {255, "ff"}, {256, "x"}, {70000, "big"}});
         same<M, L>("map_u32_extremes", {{0xffffffffu, ""}, {0, "zero"}},
                    {{0, "zero"}, {0xffffffffu, ""}});
         unordered<M, L>("map_u32_little_endian_order", {{256, "x"}, {255, "ff"}}, map_reason);
         unordered<M, L>("map_u32_duplicate", {{7, "a"}, {7, "b"}}, map_reason);
      }
      // A tuple key; a list value.
      {
         using M = MapTup;
         using L = Vec<std::tuple<TupBS, Vec<u16>>>;
         same<M, L>("map_tuple_many", {{{2, "b"}, {1}}, {{1, "z"}, {}}, {{1, "a"}, {513, 2}}},
                    {{{1, "a"}, {513, 2}}, {{1, "z"}, {}}, {{2, "b"}, {1}}});
         unordered<M, L>("map_tuple_second_element", {{{1, "z"}, {}}, {{1, "a"}, {}}}, map_reason);
         unordered<M, L>("map_tuple_first_element", {{{2, "a"}, {}}, {{1, "a"}, {}}}, map_reason);
      }
      // Record values.
      {
         using M = MapInner;
         using L = Vec<std::tuple<u16, Inner>>;
         same<M, L>("map_record_values", {{513, Inner{2, "two"}}, {2, Inner{1, ""}}},
                    {{2, Inner{1, ""}}, {513, Inner{2, "two"}}});
         unordered<M, L>("map_record_values_unsorted", {{513, Inner{2, "two"}}, {2, Inner{1, ""}}},
                         map_reason);
      }
      // Nested maps and sets: each level is checked.
      {
         using M = MapNest;
         using L = ListNest;
         same<M, L>("map_nested_empty", {}, {});
         same<M, L>("map_nested", {{"k", {{7, {"z", "y"}}, {1, {}}}}, {"", {{255, {""}}}}, {"e", {}}},
                    {{"", {{255, {""}}}}, {"e", {}}, {"k", {{1, {}}, {7, {"y", "z"}}}}});
         unordered<M, L>("map_nested_outer", {{"k", {}}, {"e", {}}}, map_reason);
         unordered<M, L>("map_nested_inner_map", {{"k", {{7, {}}, {1, {}}}}}, map_reason);
         unordered<M, L>("map_nested_inner_set", {{"k", {{1, {"z", "y"}}}}}, set_reason);
         unordered<M, L>("map_nested_inner_set_duplicate", {{"k", {{1, {"y", "y"}}}}}, set_reason);
      }
      // Sets of fixed-size elements (no offset table) and of variable ones.
      {
         using L = Vec<u16>;
         same<SetU16, L>("set_u16_empty", {}, {});
         same<SetU16, L>("set_u16_one", {7}, {7});
         same<SetU16, L>("set_u16_many", {513, 2, 65535, 0, 256}, {0, 2, 256, 513, 65535});
         literal_all<SetU16>(c, "set_u16_literal", {513, 2}, hx("0200" "0102"));
         unordered<SetU16, L>("set_u16_swapped", {513, 2}, set_reason);
         unordered<SetU16, L>("set_u16_duplicate", {2, 2}, set_reason);
         unordered<SetU16, L>("set_u16_duplicate_at_end", {1, 2, 3, 3}, set_reason);
      }
      {
         using L = Vec<Raw>;
         same<SetRaw, L>("set_str_empty", {}, {});
         same<SetRaw, L>("set_str_many", {"b", "", "a", "ab", "a" + nul}, {"", "a", "a" + nul, "ab", "b"});
         unordered<SetRaw, L>("set_str_swapped", {"b", "a"}, set_reason);
         unordered<SetRaw, L>("set_str_duplicate", {"a", "a"}, set_reason);
         unordered<SetRaw, L>("set_str_duplicate_empty", {"", ""}, set_reason);
      }
      {
         using L = Vec<std::int32_t>;
         const auto lo = std::numeric_limits<std::int32_t>::min();
         const auto hi = std::numeric_limits<std::int32_t>::max();
         same<SetI32, L>("set_i32_signed", {1, -1, 0, lo, hi}, {lo, -1, 0, 1, hi});
         unordered<SetI32, L>("set_i32_negative_after_positive", {1, -1}, set_reason);
         unordered<SetI32, L>("set_i32_duplicate", {-5, -5}, set_reason);
      }
      {
         using L = Vec<TupBS>;
         same<SetTup, L>("set_tuple_many", {{2, "a"}, {1, "b"}, {1, "a"}, {1, ""}},
                         {{1, ""}, {1, "a"}, {1, "b"}, {2, "a"}});
         unordered<SetTup, L>("set_tuple_swapped", {{1, "b"}, {1, "a"}}, set_reason);
         unordered<SetTup, L>("set_tuple_duplicate", {{1, "a"}, {1, "a"}}, set_reason);
      }
      // Floats: key-spec order. Negative NaN first, -0 and +0 one key,
      // positive NaN last.
      {
         using L          = Vec<double>;
         const double inf = std::numeric_limits<double>::infinity();
         const auto   f64 = [](std::uint64_t bits) { return std::bit_cast<double>(bits); };
         const double nan = f64(0x7ff8000000000000ull), neg_nan = f64(0xfff8000000000000ull);
         same<SetF64, L>("set_f64_finite", {1.5, -2.0, 0.0}, {-2.0, 0.0, 1.5});
         same<SetF64, L>("set_f64_negative_zero", {-0.0}, {-0.0});
         same<SetF64, L>("set_f64_nan_order", {nan, 1.0, -inf, neg_nan, inf, 0.0},
                         {neg_nan, -inf, 0.0, 1.0, inf, nan});
         same<SetF64, L>("set_f64_nan_payloads", {f64(0x7ff8000000000001ull), nan,
                                                  f64(0x7ff0000000000001ull)},
                         {f64(0x7ff0000000000001ull), nan, f64(0x7ff8000000000001ull)});
         unordered<SetF64, L>("set_f64_swapped", {1.0, 0.0}, set_reason);
         unordered<SetF64, L>("set_f64_both_zeros", {0.0, -0.0}, set_reason);
         unordered<SetF64, L>("set_f64_both_zeros_reversed", {-0.0, 0.0}, set_reason);
         unordered<SetF64, L>("set_f64_nan_before_number", {nan, 1.0}, set_reason);
         unordered<SetF64, L>("set_f64_number_before_negative_nan", {1.0, neg_nan}, set_reason);
         unordered<SetF64, L>("set_f64_same_nan_twice", {nan, nan}, set_reason);
      }
      // A fixed-size record as the element: ordered by its key (x, then y).
      {
         using L = Vec<Pair>;
         same<SetPair, L>("set_record_many", {Pair{2, 0}, Pair{1, 5}, Pair{1, 4}},
                          L{Pair{1, 4}, Pair{1, 5}, Pair{2, 0}});
         unordered<SetPair, L>("set_record_swapped", L{Pair{1, 5}, Pair{1, 4}}, set_reason);
         unordered<SetPair, L>("set_record_duplicate", L{Pair{1, 4}, Pair{1, 4}}, set_reason);
      }
      // An optional set carries a selector, as an optional list does.
      corpus(c, "optional_set_none", Opt<SetU16>{});
      corpus(c, "optional_set_empty", Opt<SetU16>{SetU16{}});
      corpus(c, "optional_set_some", Opt<SetU16>{SetU16{513, 2}});
      formats([&]<typename F>() {
         add<F, Opt<SetU16>>(o, "optional_set_swapped", join({hx("01"), hx("0102" "0200")}),
                             reject(set_reason));
      });
      // A map as a variant alternative.
      corpus(c, "variant_map", VarMap{MapRawU32{{"b", 2}, {"a", 1}}});
      corpus(c, "variant_other", VarMap{u32{7}});
      formats([&]<typename F>() {
         Bytes list;
         if (!try_encode<F>(Vec<std::tuple<Raw, u32>>{{"b", 2}, {"a", 1}}, list))
            return;
         add<F, VarMap>(o, "variant_map_swapped", join({hx("00"), list}), reject(map_reason));
      });

      // As record members: every member is checked where it occurs.
      {
         const AssocLists full{{{"a", 1}, {"b", 2}},
                               {2, 513},
                               {7, "head"},
                               Vec<std::tuple<u32, Raw>>{{1, "one"}, {256, "x"}},
                               {{2, 2}, {9, 1}},
                               {{-1, 1}, {}, {5}}};
         same<AssocRec, AssocLists>("record_empty", AssocRec{}, AssocLists{});
         same<AssocRec, AssocLists>(
            "record_full",
            AssocRec{{{"b", 2}, {"a", 1}},
                     {513, 2},
                     {7, "head"},
                     MapU32Raw{{256, "x"}, {1, "one"}},
                     {{9, 1}, {2, 2}},
                     {{1, -1}, {}, {5}}},
            full);
         auto broken = [&](const std::string& vname, const std::string& reason, auto&& change) {
            AssocLists bad = full;
            change(bad);
            unordered<AssocRec, AssocLists>(vname, bad, reason);
         };
         broken("record_map_member", map_reason, [](AssocLists& b) { std::swap(b.counts[0], b.counts[1]); });
         broken("record_set_member", set_reason, [](AssocLists& b) { std::swap(b.ids[0], b.ids[1]); });
         broken("record_optional_map_member", map_reason,
                [](AssocLists& b) { std::swap((*b.names)[0], (*b.names)[1]); });
         broken("record_bounded_map_member", map_reason,
                [](AssocLists& b) { std::swap(b.limited[0], b.limited[1]); });
         broken("record_set_in_list_member", set_reason,
                [](AssocLists& b) { std::swap(b.groups[0][0], b.groups[0][1]); });
         broken("record_duplicate_in_set_member", set_reason, [](AssocLists& b) { b.ids = {2, 2}; });

         // A member length_bound counts entries, as for a list.
         AssocRec over;
         over.limited = {{1, 1}, {2, 2}, {3, 3}};
         formats([&]<typename F>() {
            Bytes bytes, refused;
            bytes = psio::detail::pssz_impl::encode_ignoring_length_bounds(F{}, over);
            if (try_encode<F>(over, refused) ||
                psio::encode_pssz_checked(F{}, over, refused).ok() || !refused.empty())
               die("the encoder accepted a map outside its member length_bound");
            add<F, AssocRec>("length-bound-overflow", "map_three_entries", bytes,
                             reject("pssz: member length_bound violated"), "map_three_entries",
                             type_id<AssocRec>());
         });
      }
   }

   // ── Depth ─────────────────────────────────────────────────────────────
   DepthNode chain(std::size_t links)
   {
      DepthNode out{7, std::nullopt};
      for (std::size_t i = 0; i < links; ++i)
         out = DepthNode{1, psio::schema_types::Box<DepthNode>{std::move(out)}};
      return out;
   }
   // The same chain built by hand, so a too-deep frame can exist at all.
   Bytes chain_bytes(std::size_t W, std::size_t links)
   {
      Bytes bytes = Fr{W}.o(1).byte(7);
      for (std::size_t i = 0; i < links; ++i)
         bytes = Fr{W}.o(1 + W).byte(1).o(1 + W).cat(bytes);
      return bytes;
   }

   void emit_depth()
   {
      // Node depth: the root is 0; each present record field, sequence
      // element, Some payload and variant payload is one deeper; a Box adds
      // none. Nodes through depth kMaxValidationDepth (64) are accepted.
      // A chain of n links puts its innermost `value` at depth 2n + 1.
      static_assert(psio::kMaxValidationDepth == 64);
      const char*       c    = "depth";
      const std::string deep = "pssz: max depth exceeded";
      formats([&]<typename F>() {
         const std::size_t W = wof<F, DepthNode>();
         literal<F, DepthNode>(c, "depth_1", chain(0), chain_bytes(W, 0));
         literal<F, DepthNode>(c, "depth_3", chain(1), chain_bytes(W, 1));
         literal<F, DepthNode>(c, "depth_63", chain(31), chain_bytes(W, 31));
         // Wrapped in an optional the same chain is one deeper: exactly 64.
         literal<F, Opt<DepthNode>>(c, "depth_64", Opt<DepthNode>{chain(31)}, chain_bytes(W, 31));
         add<F, DepthNode>(c, "depth_65", chain_bytes(W, 32), reject(deep));
         add<F, Opt<DepthNode>>(c, "depth_66", chain_bytes(W, 32), reject(deep));
         add<F, DepthNode>(c, "depth_67", chain_bytes(W, 33), reject(deep));
         Bytes out;
         if (try_encode<F>(chain(32), out)) die("the encoder accepted a value of depth 65");
         if (try_encode<F>(Opt<DepthNode>{chain(32)}, out))
            die("the encoder accepted a value of depth 66");
      });
   }

   // ── JSON ──────────────────────────────────────────────────────────────
   std::string jq(std::string_view s)
   {
      std::string out = "\"";
      for (char c : s)
      {
         if (c == '"' || c == '\\') out.push_back('\\');
         if (static_cast<unsigned char>(c) < 0x20) die("control character in a JSON string");
         out.push_back(c);
      }
      return out + "\"";
   }
   // The Schema IR of every fixture type, each under its fixture id, so a
   // consumer holding only this file can run the schema-driven codec.
   const psio::Schema& fixture_schema()
   {
      static const psio::Schema schema = [] {
         psio::SchemaBuilder builder;
         for_each_type([&]<typename T>(std::type_identity<T>) { builder.insert<T>(type_id<T>()); });
         return std::move(builder).build();
      }();
      return schema;
   }
   // Whether the IR describes T well enough for the schema-driven codec to
   // agree with the typed one. See pssz_conf::ir_expressible.
   template <typename T>
   bool ir_flag()
   {
      const bool expressible = ir_expressible<T>();
      std::size_t width = 0;
      const auto  status =
         psio::pssz_ir::auto_width(fixture_schema(), psio::schema_types::Type{type_id<T>()}, width);
      const bool agrees = status.ok() && width == psio::auto_pssz_width_v<T>;
      if (expressible && !agrees)
         die("the Schema IR of " + type_id<T>() + " derives width " + std::to_string(width) +
             (status.ok() ? "" : " (" + std::string(status.error().what) + ")") + ", the type has " +
             std::to_string(psio::auto_pssz_width_v<T>));
      return expressible;
   }
   template <typename T>
   std::string type_json()
   {
      std::ostringstream out;
      out << "{\"id\": " << jq(type_id<T>()) << ", \"ir\": " << (ir_flag<T>() ? "true" : "false")
          << ", \"shape\": " << jq(type_shape<T>())
          << ", \"auto_width\": " << psio::auto_pssz_width_v<T> << ", \"max_encoded_size\": ";
      if (constexpr auto m = psio::max_encoded_size<T>(); m) out << *m;
      else out << "null";
      out << ", \"max_dynamic_data\": ";
      if (constexpr auto m = psio::max_dynamic_data_v<T>; m) out << *m;
      else out << "null";
      out << ", \"fixed\": " << (impl::is_fixed_v<T> ? "true" : "false") << ", \"fixed_size\": "
          << (impl::is_fixed_v<T> ? impl::fixed_size_of<T>() : std::size_t{0}) << "}";
      return out.str();
   }

   std::string render()
   {
      std::string out = "{\n\"version\": 1,\n\"schema_ir\": " +
                        jq(to_hex(psio::encode(psio::pssz{}, fixture_schema()))) +
                        ",\n\"types\": [\n";
      bool        first = true;
      for_each_type([&]<typename T>(std::type_identity<T>) {
         out += (first ? "" : ",\n") + type_json<T>();
         first = false;
      });
      out += "\n],\n\"vectors\": [\n";
      first = true;
      for (const auto& r : rows)
      {
         out += first ? "" : ",\n";
         first = false;
         out += "{\"name\": " + jq(r.name) + ", \"class\": " + jq(r.cls) +
                ", \"type\": " + jq(r.type) + ", \"format\": " + jq(r.format) +
                ", \"hex\": " + jq(r.hex) + ", \"expect\": " + jq(r.expect) +
                ", \"reason\": " + jq(r.reason) + ", \"reencoded\": " + jq(r.reencoded) +
                ", \"value\": " + jq(r.value) + ", \"writer\": " + jq(r.writer) + "}";
      }
      out += "\n]\n}\n";
      return out;
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   const bool stats = argc == 2 && std::string_view(argv[1]) == "--stats";
   if (!check && !stats && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json> | --stats\n", argv[0]);
      return 2;
   }
   for_each_type([&]<typename T>(std::type_identity<T>) {
      if (!registered.insert(type_id<T>()).second) die("duplicate type id " + type_id<T>());
   });

   emit_evolution();
   check_goldens();
   emit_pruning_and_malformed();
   emit_containers_malformed();
   emit_optional_presence();
   emit_width();
   emit_length_bound_overflow();
   emit_tuples_variants_enums();
   emit_boxes();
   emit_strings();
   emit_bytes_and_scalars();
   emit_zero_fixed();
   emit_depth();
   emit_assoc();

   if (!unregistered.empty())
   {
      std::string list;
      for (const auto& id : unregistered) list += " " + id;
      die("types used by vectors but missing from the registry:" + list);
   }
   // Every registered type must be exercised by at least one vector.
   std::set<std::string> used;
   for (const auto& r : rows) used.insert(r.type);
   std::string unused;
   for (const auto& id : registered)
      if (!used.count(id)) unused += " " + id;
   if (!unused.empty()) die("registered types without vectors:" + unused);

   if (stats)
   {
      std::map<std::string, std::size_t> counts;
      for (const auto& r : rows) ++counts[r.cls];
      for (const auto& [cls, n] : counts) std::printf("| `%s` | %zu |\n", cls.c_str(), n);
      std::printf("types %zu, vectors %zu\n", registered.size(), rows.size());
      return 0;
   }
   const std::string json = render();
   if (check)
   {
      std::ifstream      in(argv[2], std::ios::binary);
      std::ostringstream have;
      have << in.rdbuf();
      if (!in || have.str() != json)
      {
         std::fprintf(stderr,
                      "pssz conformance emit: %s is not what the implementation generates.\n"
                      "Regenerate it only for an intended wire-format change.\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << json;
   if (!out) die(std::string("cannot write ") + argv[1]);
   std::printf("wrote %zu types, %zu vectors to %s\n", registered.size(), rows.size(), argv[1]);
   return 0;
}

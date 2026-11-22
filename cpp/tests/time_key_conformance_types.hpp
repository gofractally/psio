#pragma once
// Types and row model of conformance/time-key-vectors.json, the fixture for
// time values and the psio::key format shared by ctest, cargo test and the
// JS tests (conformance/time-key-vectors.md describes the file).
//
// Shared by the emitter (time_key_conformance_emit.cpp) and the consumer
// (time_key_conformance_tests.cpp).

#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/nested.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>
#include <psio/time.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

// A fixed-layout record of time values (global scope: the attribute form of
// PSIO_REFLECT specializes psio::annotate).
struct TkStamps
{
   std::chrono::sys_seconds                              created;
   std::chrono::sys_time<std::chrono::microseconds>      updated;
   std::chrono::milliseconds                             ttl{};
   std::chrono::sys_time<std::chrono::duration<std::uint32_t>> legacy;
   friend bool operator==(const TkStamps&, const TkStamps&)  = default;
   friend auto operator<=>(const TkStamps&, const TkStamps&) = default;
};
PSIO_REFLECT(TkStamps, created, updated, ttl, legacy, definitionWillNotChange())

namespace tk_conf
{
   using namespace std::chrono;

   using sys_millis = sys_time<milliseconds>;
   using sys_micros = sys_time<microseconds>;
   using sys_nanos  = sys_time<nanoseconds>;
   using seconds32  = duration<std::uint32_t>;
   using sys_sec32  = sys_time<seconds32>;
   using minutes32  = duration<std::int32_t, std::ratio<60>>;
   // Monotonic time points (a clock other than system_clock): ticks since the
   // clock's own epoch, with tick text, never a date.
   using mono_seconds = time_point<steady_clock, seconds>;
   using mono_micros  = time_point<steady_clock, microseconds>;
   using mono_ms32    = time_point<steady_clock, duration<std::uint32_t, std::milli>>;
   using Stamps     = ::TkStamps;

   // An extensible record mixing time values with variable members.
   struct Event
   {
      std::string              name;
      sys_millis               at;
      std::optional<sys_nanos> precise;
      std::vector<sys_seconds> history;
      nanoseconds              elapsed{};
      minutes32                timeout{};
      friend bool operator==(const Event&, const Event&)  = default;
      friend auto operator<=>(const Event&, const Event&) = default;
   };
   PSIO_REFLECT(Event, name, at, precise, history, elapsed, timeout)

   // Monotonic time points as record members, in a list and in an optional.
   struct Timer
   {
      mono_micros                expires;
      std::optional<mono_seconds> started;
      std::vector<mono_ms32>     laps;
      friend bool operator==(const Timer&, const Timer&)  = default;
      friend auto operator<=>(const Timer&, const Timer&) = default;
   };
   PSIO_REFLECT(Timer, expires, started, laps)

   // A record used as a composite key: its key is that of
   // tuple<u32, string, optional<i64>>.
   struct Account
   {
      std::uint32_t               shard = 0;
      std::string                 owner;
      std::optional<std::int64_t> seq;
      friend bool operator==(const Account&, const Account&)  = default;
      friend auto operator<=>(const Account&, const Account&) = default;
   };
   PSIO_REFLECT(Account, shard, owner, seq)

   using Bytes = std::vector<std::uint8_t>;
   template <typename... Ts>
   using Tup = std::tuple<Ts...>;

   using KEmpty   = Tup<>;
   using KShard   = Tup<std::uint32_t, std::string>;
   using KAccount = Tup<std::uint32_t, std::string, std::optional<std::int64_t>>;
   using KStrU8   = Tup<std::string, std::uint8_t>;
   using KStrStr  = Tup<std::string, std::string>;
   using KBytes   = Tup<Bytes, std::int8_t>;
   using KList    = Tup<std::vector<std::int16_t>, std::int8_t>;
   using KOptStr  = Tup<std::optional<std::string>, std::string>;
   using KStrList = Tup<std::vector<std::string>, std::string>;
   using KInts    = Tup<bool, std::int8_t, std::int16_t, std::int32_t, std::int64_t, std::uint8_t,
                        std::uint16_t, std::uint32_t, std::uint64_t>;
   using KFloats  = Tup<float, double>;
   using KTime    = Tup<sys_micros, std::string, seconds>;
   using KMono    = Tup<mono_micros, std::string>;
   using KArray   = Tup<std::array<std::uint16_t, 2>, std::string>;
   using KVariant = Tup<std::variant<std::uint8_t, std::string>, std::uint8_t>;
   using KNested  = Tup<Tup<std::string, std::uint8_t>, std::string>;
   // A nested value in a key (psio/nested.hpp): the key of the byte list
   // holding the PSSZ encoding of the inner value.
   using KPacked  = Tup<std::uint32_t, psio::nested<Account>>;

   // ── Fixture ids ────────────────────────────────────────────────────────
   template <typename T>
   struct type_name;
#define TK_TYPE(T, ID)                                 \
   template <>                                         \
   struct type_name<T>                                 \
   {                                                   \
      static constexpr const char* value = ID;         \
   };
   TK_TYPE(sys_seconds, "time_point<seconds>")
   TK_TYPE(sys_millis, "time_point<millis>")
   TK_TYPE(sys_micros, "time_point<micros>")
   TK_TYPE(sys_nanos, "time_point<nanos>")
   TK_TYPE(sys_sec32, "time_point<seconds,u32>")
   TK_TYPE(seconds, "duration<seconds>")
   TK_TYPE(milliseconds, "duration<millis>")
   TK_TYPE(microseconds, "duration<micros>")
   TK_TYPE(nanoseconds, "duration<nanos>")
   TK_TYPE(minutes32, "duration<60/1,i32>")
   TK_TYPE(mono_seconds, "monotonic<seconds>")
   TK_TYPE(mono_micros, "monotonic<micros>")
   TK_TYPE(mono_ms32, "monotonic<millis,u32>")
   TK_TYPE(Timer, "Timer")
   TK_TYPE(KMono, "tuple<monotonic<micros>,string>")
   TK_TYPE(Stamps, "Stamps")
   TK_TYPE(Event, "Event")
   TK_TYPE(Account, "Account")
   TK_TYPE(KEmpty, "tuple<>")
   TK_TYPE(KShard, "tuple<u32,string>")
   TK_TYPE(KAccount, "tuple<u32,string,optional<i64>>")
   TK_TYPE(KStrU8, "tuple<string,u8>")
   TK_TYPE(KStrStr, "tuple<string,string>")
   TK_TYPE(KBytes, "tuple<bytes,i8>")
   TK_TYPE(KList, "tuple<list<i16>,i8>")
   TK_TYPE(KOptStr, "tuple<optional<string>,string>")
   TK_TYPE(KStrList, "tuple<list<string>,string>")
   TK_TYPE(KInts, "tuple<bool,i8,i16,i32,i64,u8,u16,u32,u64>")
   TK_TYPE(KFloats, "tuple<f32,f64>")
   TK_TYPE(KTime, "tuple<time_point<micros>,string,duration<seconds>>")
   TK_TYPE(KArray, "tuple<array<u16,2>,string>")
   TK_TYPE(KVariant, "tuple<variant<u8,string>,u8>")
   TK_TYPE(KNested, "tuple<tuple<string,u8>,string>")
   TK_TYPE(KPacked, "tuple<u32,nested<Account>>")
#undef TK_TYPE

   template <typename T>
   std::string type_id()
   {
      return type_name<T>::value;
   }

   template <typename F>
   void for_each_type(F&& f)
   {
      auto each = [&]<typename... Ts>(std::type_identity<std::tuple<Ts...>>) {
         (f(std::type_identity<Ts>{}), ...);
      };
      each(std::type_identity<
           std::tuple<sys_seconds, sys_millis, sys_micros, sys_nanos, sys_sec32, seconds,
                      milliseconds, microseconds, nanoseconds, minutes32, Stamps, Event, Account,
                      KEmpty, KShard, KAccount, KStrU8, KStrStr, KBytes, KList, KOptStr, KStrList,
                      KInts, KFloats, KTime, KArray, KVariant, KNested,
                      // Appended (the ids above keep their schema positions).
                      mono_seconds, mono_micros, mono_ms32, Timer, KMono, KPacked>>{});
   }

   // The Schema IR of every fixture type, each under its fixture id. (Also
   // recorded, as text, by conformance/schema-json-vectors.json.)
   inline const psio::Schema& fixture_schema()
   {
      static const psio::Schema schema = [] {
         psio::SchemaBuilder builder;
         for_each_type([&]<typename T>(std::type_identity<T>) { builder.insert<T>(type_id<T>()); });
         return std::move(builder).build();
      }();
      return schema;
   }

   // Composite key types: the ones with key-prefix rows.
   template <typename T>
   inline constexpr bool is_composite_v = psio::detail::key_impl::Composite<T>;

   // ── Rows ───────────────────────────────────────────────────────────────
   //
   //   value        pssz, key; json + ijson unless the value has no text
   //                (a time point outside years 0000..9999)
   //   text-accept  text (a JSON document) reads, under json and ijson, as
   //                the value whose encoding is `pssz`
   //   text-reject  text is rejected under json and ijson
   //   key-reject   key bytes are rejected
   //   key-order    values (pssz hex) in strictly ascending order; their
   //                keys must be strictly ascending bytewise
   //   key-prefix   prefix = key of the first `count` elements of the value
   //                `pssz`; bound = its exclusive upper bound (null: none)
   //   bound        bound = exclusive upper bound of the raw bytes `prefix`
   struct Row
   {
      std::string                             name, kind, type;
      std::optional<std::string>              pssz, key, json, ijson, text, prefix, bound;
      std::optional<std::uint32_t>            count;
      std::optional<std::vector<std::string>> values;
   };
   PSIO_REFLECT(Row, name, kind, type, pssz, key, json, ijson, text, prefix, bound, count, values)

   struct TypeRow
   {
      std::string id;
      bool        composite = false;
   };
   PSIO_REFLECT(TypeRow, id, composite)

   struct Fixture
   {
      std::uint32_t        version = 0;
      std::string          schema_ir;
      std::vector<TypeRow> types;
      std::vector<Row>     vectors;
   };
   PSIO_REFLECT(Fixture, version, schema_ir, types, vectors)

   // ── Helpers ────────────────────────────────────────────────────────────
   inline std::string to_hex(std::span<const char> bytes)
   {
      std::string out;
      for (unsigned char c : bytes)
      {
         out += "0123456789abcdef"[c >> 4];
         out += "0123456789abcdef"[c & 15];
      }
      return out;
   }
   inline std::optional<std::vector<char>> from_hex(std::string_view text)
   {
      auto nib = [](char c) {
         return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      };
      if (text.size() % 2)
         return std::nullopt;
      std::vector<char> out;
      for (std::size_t i = 0; i < text.size(); i += 2)
      {
         const int hi = nib(text[i]), lo = nib(text[i + 1]);
         if (hi < 0 || lo < 0)
            return std::nullopt;
         out.push_back(static_cast<char>(hi << 4 | lo));
      }
      return out;
   }
   inline bool bytes_less(std::span<const char> a, std::span<const char> b)
   {
      const std::size_t n = std::min(a.size(), b.size());
      const int         c = n ? std::memcmp(a.data(), b.data(), n) : 0;
      return c != 0 ? c < 0 : a.size() < b.size();
   }

   // The key of the first `count` elements of composite value `v`, through
   // the typed API: key_prefix<T>(e0, ..., e{count-1}).
   template <typename T>
   std::vector<char> prefix_of(const T& v, std::size_t count)
   {
      using E = psio::detail::key_impl::key_elements<T>;
      auto element = [&]<std::size_t I>(std::integral_constant<std::size_t, I>) -> decltype(auto) {
         if constexpr (psio::detail::key_impl::is_std_tuple<T>::value)
            return std::get<I>(v);
         else
            return v.*(psio::reflect<T>::template member_pointer<I>);
      };
      std::vector<char> out;
      [&]<std::size_t... Ks>(std::index_sequence<Ks...>)
      {
         auto at = [&]<std::size_t K>(std::integral_constant<std::size_t, K>) {
            if (K != count)
               return;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               out = psio::key_prefix<T>(element(std::integral_constant<std::size_t, Is>{})...);
            }(std::make_index_sequence<K>{});
         };
         (at(std::integral_constant<std::size_t, Ks>{}), ...);
      }(std::make_index_sequence<E::count + 1>{});
      return out;
   }
}  // namespace tk_conf

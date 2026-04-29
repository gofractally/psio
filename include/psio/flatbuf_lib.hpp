#pragma once
//
// psio/flatbuf_lib.hpp — `flatbuf_lib` format tag: Google flatbuffers
// adapter. Requires the flatbuffers C++ headers on the include path; the
// tests build target links flatbuffers::flatbuffers when the package is
// found.
//
// Wire is the canonical flatbuffers table layout — interoperable with
// flatc-generated code that uses PSIO1_REFLECT/PSIO_REFLECT field order
// matching the .fbs schema. MVP scope: primitives, std::string,
// std::vector<arithmetic | string | table>, std::optional,
// reflected-record root/nested tables.

#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/adapter.hpp>
#include <psio/reflect.hpp>

#include <flatbuffers/flatbuffers.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio {

   struct flatbuf_lib;

   namespace detail::flatbuf_lib_impl {

      template <typename T>
      concept Record = ::psio::Reflected<T>;

      template <typename T>
      struct is_optional : std::false_type {};
      template <typename T>
      struct is_optional<std::optional<T>> : std::true_type {};

      template <typename T>
      struct is_vector : std::false_type {};
      template <typename T, typename A>
      struct is_vector<std::vector<T, A>> : std::true_type {};

      template <typename T, typename = void>
      struct is_table : std::false_type {};
      template <typename T>
      struct is_table<T, std::void_t<decltype(::psio::reflect<T>::member_count)>>
         : std::bool_constant<!std::is_arithmetic_v<T> &&
                              !std::is_same_v<T, std::string>> {};

      // Phase 1: pre-create child offsets (strings, vectors, nested tables).
      template <typename V>
      std::uint32_t pre_create(flatbuffers::FlatBufferBuilder& fbb,
                               const V&                         val)
      {
         if constexpr (std::is_arithmetic_v<V>)
            return 0;
         else if constexpr (std::is_same_v<V, std::string>)
            return fbb.CreateString(val).o;
         else if constexpr (is_optional<V>::value)
         {
            if (val.has_value())
               return pre_create<typename V::value_type>(fbb, *val);
            return 0;
         }
         else if constexpr (is_vector<V>::value)
         {
            using E = typename V::value_type;
            if (val.empty())
               return 0;
            if constexpr (std::is_same_v<E, std::string>)
               return fbb.CreateVectorOfStrings(val).o;
            else if constexpr (std::is_arithmetic_v<E>)
               return fbb.CreateVector(val.data(), val.size()).o;
            else if constexpr (is_table<E>::value)
            {
               std::vector<flatbuffers::Offset<flatbuffers::Table>> offs;
               offs.reserve(val.size());
               for (const auto& item : val)
               {
                  auto root_fn = [&](flatbuffers::FlatBufferBuilder& b) {
                     return pre_create<E>(b, item);
                  };
                  offs.push_back(flatbuffers::Offset<flatbuffers::Table>(
                     root_fn(fbb)));
               }
               return fbb.CreateVector(offs).o;
            }
            else
            {
               static_assert(sizeof(E) == 0,
                             "psio::flatbuf_lib: unsupported vector element");
            }
         }
         else if constexpr (is_table<V>::value)
         {
            using R = ::psio::reflect<V>;
            constexpr std::size_t N = R::member_count;
            std::uint32_t         offsets[N > 0 ? N : 1]{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((offsets[Is] = pre_create<typename R::template member_type<Is>>(
                    fbb, val.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});

            auto start = fbb.StartTable();
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (([&]
                 {
                    constexpr std::size_t i = N - 1 - Is;
                    using F = typename R::template member_type<i>;
                    const F& fref = val.*(R::template member_pointer<i>);
                    auto     vt   = static_cast<flatbuffers::voffset_t>(
                       4 + 2 * i);
                    if constexpr (std::is_same_v<F, bool>)
                       fbb.AddElement<std::uint8_t>(
                          vt, static_cast<std::uint8_t>(fref), 0);
                    else if constexpr (std::is_arithmetic_v<F>)
                       fbb.AddElement<F>(vt, fref, F{});
                    else if constexpr (is_optional<F>::value)
                    {
                       using Inner = typename F::value_type;
                       if (fref.has_value())
                       {
                          if constexpr (std::is_same_v<Inner, bool>)
                             fbb.AddElement<std::uint8_t>(
                                vt, static_cast<std::uint8_t>(*fref), 0);
                          else if constexpr (std::is_arithmetic_v<Inner>)
                             fbb.AddElement<Inner>(vt, *fref, Inner{});
                          else if (offsets[i])
                             fbb.AddOffset(vt, flatbuffers::Offset<void>(
                                                  offsets[i]));
                       }
                    }
                    else
                    {
                       if (offsets[i])
                          fbb.AddOffset(vt,
                                        flatbuffers::Offset<void>(offsets[i]));
                    }
                 }()),
                ...);
            }(std::make_index_sequence<N>{});
            return fbb.EndTable(start);
         }
         else
         {
            static_assert(sizeof(V) == 0,
                          "psio::flatbuf_lib: unsupported type");
         }
      }

      // ── Decode ─────────────────────────────────────────────────────────

      template <typename T>
      void unpack_table(const flatbuffers::Table& table, T& out);

      template <typename V>
      void read_field(const flatbuffers::Table& table, int idx, V& out)
      {
         auto vt = static_cast<flatbuffers::voffset_t>(4 + 2 * idx);
         if constexpr (std::is_same_v<V, bool>)
            out = table.GetField<std::uint8_t>(vt, 0) != 0;
         else if constexpr (std::is_arithmetic_v<V>)
            out = table.GetField<V>(vt, V{});
         else if constexpr (std::is_same_v<V, std::string>)
         {
            auto s = table.GetPointer<const flatbuffers::String*>(vt);
            if (s)
               out.assign(s->c_str(), s->size());
         }
         else if constexpr (is_optional<V>::value)
         {
            using Inner = typename V::value_type;
            if (table.GetOptionalFieldOffset(vt))
            {
               if constexpr (std::is_same_v<Inner, bool>)
                  out = table.GetField<std::uint8_t>(vt, 0) != 0;
               else if constexpr (std::is_arithmetic_v<Inner>)
                  out = table.GetField<Inner>(vt, Inner{});
               else if constexpr (std::is_same_v<Inner, std::string>)
               {
                  auto s = table.GetPointer<const flatbuffers::String*>(vt);
                  if (s)
                     out = std::string(s->c_str(), s->size());
               }
            }
         }
         else if constexpr (is_vector<V>::value)
         {
            using E = typename V::value_type;
            if constexpr (std::is_same_v<E, std::string>)
            {
               auto vec = table.GetPointer<const flatbuffers::Vector<
                  flatbuffers::Offset<flatbuffers::String>>*>(vt);
               if (vec)
               {
                  out.resize(vec->size());
                  for (flatbuffers::uoffset_t j = 0; j < vec->size(); ++j)
                     out[j].assign(vec->Get(j)->c_str(), vec->Get(j)->size());
               }
            }
            else if constexpr (std::is_arithmetic_v<E>)
            {
               auto vec = table.GetPointer<const flatbuffers::Vector<E>*>(vt);
               if (vec)
               {
                  out.resize(vec->size());
                  for (flatbuffers::uoffset_t j = 0; j < vec->size(); ++j)
                     out[j] = vec->Get(j);
               }
            }
            else if constexpr (is_table<E>::value)
            {
               auto vec = table.GetPointer<const flatbuffers::Vector<
                  flatbuffers::Offset<flatbuffers::Table>>*>(vt);
               if (vec)
               {
                  out.resize(vec->size());
                  for (flatbuffers::uoffset_t j = 0; j < vec->size(); ++j)
                     unpack_table(*vec->Get(j), out[j]);
               }
            }
         }
         else if constexpr (is_table<V>::value)
         {
            auto nested = table.GetPointer<const flatbuffers::Table*>(vt);
            if (nested)
               unpack_table(*nested, out);
         }
      }

      template <typename T>
      void unpack_table(const flatbuffers::Table& table, T& out)
      {
         using R = ::psio::reflect<T>;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (read_field(table, static_cast<int>(Is),
                        out.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<R::member_count>{});
      }

   }  // namespace detail::flatbuf_lib_impl

   struct flatbuf_lib : format_tag_base<flatbuf_lib>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
         requires detail::flatbuf_lib_impl::is_table<T>::value
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          flatbuf_lib, const T& v)
      {
         flatbuffers::FlatBufferBuilder fbb(1024);
         auto root = detail::flatbuf_lib_impl::pre_create<T>(fbb, v);
         fbb.Finish(flatbuffers::Offset<flatbuffers::Table>(root));
         std::vector<char> out(fbb.GetSize());
         std::memcpy(out.data(), fbb.GetBufferPointer(), fbb.GetSize());
         return out;
      }

      template <typename T>
         requires detail::flatbuf_lib_impl::is_table<T>::value
      friend T tag_invoke(decltype(::psio::decode<T>), flatbuf_lib, T*,
                          std::span<const char> bytes)
      {
         const auto* buf =
            reinterpret_cast<const std::uint8_t*>(bytes.data());
         auto* table = flatbuffers::GetRoot<flatbuffers::Table>(buf);
         T     out{};
         detail::flatbuf_lib_impl::unpack_table(*table, out);
         return out;
      }

      template <typename T>
         requires detail::flatbuf_lib_impl::is_table<T>::value
      friend std::size_t tag_invoke(decltype(::psio::size_of), flatbuf_lib,
                                    const T& v)
      {
         flatbuffers::FlatBufferBuilder fbb(1024);
         auto root = detail::flatbuf_lib_impl::pre_create<T>(fbb, v);
         fbb.Finish(flatbuffers::Offset<flatbuffers::Table>(root));
         return fbb.GetSize();
      }

      template <typename T>
         requires detail::flatbuf_lib_impl::is_table<T>::value
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     flatbuf_lib, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.size() < 4)
            return codec_fail("flatbuf_lib: buffer too small", 0,
                              "flatbuf_lib");
         return codec_ok();
      }

      template <typename T>
         requires detail::flatbuf_lib_impl::is_table<T>::value
      friend codec_status
      tag_invoke(decltype(::psio::validate_strict<T>), flatbuf_lib, T*,
                 std::span<const char> bytes) noexcept
      {
         if (bytes.size() < 4)
            return codec_fail("flatbuf_lib: buffer too small", 0,
                              "flatbuf_lib");
         return codec_ok();
      }

      template <typename T>
         requires detail::flatbuf_lib_impl::is_table<T>::value
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio::make_boxed<T>), flatbuf_lib, T*,
                 std::span<const char> bytes) noexcept
      {
         auto v = std::make_unique<T>();
         const auto* buf =
            reinterpret_cast<const std::uint8_t*>(bytes.data());
         auto* table = flatbuffers::GetRoot<flatbuffers::Table>(buf);
         detail::flatbuf_lib_impl::unpack_table(*table, *v);
         return v;
      }
   };

}  // namespace psio

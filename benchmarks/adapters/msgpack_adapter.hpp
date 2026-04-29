#pragma once
//
// adapters/msgpack_adapter.hpp — drive msgpack-cxx (the canonical
// MessagePack C++ library) from psio's reflection so the benchmark
// can compare psio::msgpack against the upstream library on the same
// shape values, byte-identical wire output.
//
// Approach: specialise msgpack-cxx's adaptor::pack and adaptor::convert
// for any type satisfying psio::Reflected.  This means the bench
// shape headers don't need msgpack-cxx-specific MSGPACK_DEFINE
// macros — the same Point / NameRecord / FlatRecord etc. structs we
// already PSIO_REFLECT'd are sufficient.
//
// The wire form is fixarray (positional), matching psio::msgpack's
// default record encoding (psio_record_form<T>::as_map = false).
// Both libraries produce identical bytes for the same struct.

#include <msgpack.hpp>

#include <psio/reflect.hpp>

#include <utility>

namespace msgpack {
   MSGPACK_API_VERSION_NAMESPACE(MSGPACK_DEFAULT_API_NS)
   {
      namespace adaptor
      {

         template <typename T>
            requires ::psio::Reflected<T>
         struct pack<T>
         {
            template <typename Stream>
            packer<Stream>& operator()(packer<Stream>& o,
                                        const T& v) const
            {
               using R          = ::psio::reflect<T>;
               constexpr auto N = R::member_count;
               o.pack_array(static_cast<std::uint32_t>(N));
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  ((o.pack(v.*(R::template member_pointer<Is>))), ...);
               }(std::make_index_sequence<N>{});
               return o;
            }
         };

         template <typename T>
            requires ::psio::Reflected<T>
         struct convert<T>
         {
            msgpack::object const& operator()(msgpack::object const& o,
                                                T&                     v) const
            {
               using R          = ::psio::reflect<T>;
               constexpr auto N = R::member_count;
               if (o.type != msgpack::type::ARRAY)
                  throw msgpack::type_error();
               if (o.via.array.size != N)
                  throw msgpack::type_error();
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  ((o.via.array.ptr[Is].convert(
                       v.*(R::template member_pointer<Is>))),
                   ...);
               }(std::make_index_sequence<N>{});
               return o;
            }
         };

      }  // namespace adaptor
   }     // MSGPACK_API_VERSION_NAMESPACE
}  // namespace msgpack

namespace mp_bench {

   //  Encode a value via msgpack-cxx into a fresh sbuffer; return as
   //  std::vector<char> for parity with the rest of the bench's
   //  buffer types.
   template <typename T>
   inline std::vector<char> encode(const T& v)
   {
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, v);
      return std::vector<char>(sbuf.data(), sbuf.data() + sbuf.size());
   }

   template <typename T>
   inline void encode_into(const T& v, msgpack::sbuffer& sbuf)
   {
      sbuf.clear();
      msgpack::pack(sbuf, v);
   }

   //  Decode bytes back into a T via msgpack-cxx.  Throws on type
   //  mismatch (the same way psio::decode does for invalid inputs).
   template <typename T>
   inline T decode(const char* data, std::size_t size)
   {
      auto handle = msgpack::unpack(data, size);
      T    out{};
      handle.get().convert(out);
      return out;
   }

   //  size_of equivalent — pack into a throwaway sbuffer, return
   //  the byte count.  msgpack-cxx doesn't expose a separate sizing
   //  primitive (no two-pass encoder), so this is the cheapest path.
   template <typename T>
   inline std::size_t size_of(const T& v)
   {
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, v);
      return sbuf.size();
   }

}  // namespace mp_bench

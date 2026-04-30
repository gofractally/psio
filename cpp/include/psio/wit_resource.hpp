#pragma once
//
// psio/wit_resource.hpp — WIT Component Model resource vocabulary.
//
// Direct port of psio/wit_resource.hpp.  This header is the **shared
// vocabulary** between psio's reflection / WIT-generator side and
// psizam's runtime side.  It contains:
//
//   1. wit_resource             empty marker base — opt-in for types
//                                that should be emitted as WIT
//                                `resource` blocks (opaque, methods
//                                only) instead of `record` blocks.
//   2. is_wit_resource_v<T>     trait checking std::is_base_of<wit_resource, T>.
//                                Read by wit_gen to choose resource-vs-record
//                                emission.
//   3. own<T>                   RAII handle.  Wraps a u32 (handle table
//                                index).  Move-only, release()-able,
//                                drops via wit_resource_drop<T>(handle)
//                                customization point.
//   4. borrow<T>                Non-owning handle.  Bare u32 wrapper.
//                                Valid only for the duration of the
//                                call that received it.
//   5. wit_resource_drop<T>(h)  Customization point — guest binding
//                                specializes to call the WASM
//                                [resource-drop] import; host runtime
//                                specializes to call handle_table::destroy.
//                                Default is a no-op (leak) so unspecialized
//                                resources are valid C++ but obviously
//                                wrong if exercised at runtime.
//
// On the wire (canonical ABI), both own<T> and borrow<T> are a single
// u32 — index into the per-component handle table for type T.
// own transfers ownership: caller's table entry is consumed, callee
// receives a new entry.  borrow is a temporary reference good only
// for the call.
//
// Per-type wasm_type_traits specializations for own<T> / borrow<T>
// (mapping them to a u32 wasm_type) live in psizam — they're a
// runtime concern, not a serialization concern, so they stay there.
// This header has no psizam dependency.

#include <cstdint>
#include <type_traits>

namespace psio {

   // ── wit_resource — opt-in marker base ───────────────────────────────
   //
   // Inherit from this to declare a type as a WIT resource:
   //
   //    struct cursor : psio::wit_resource {
   //        bool seek(std::vector<std::uint8_t> key);
   //        bool next();
   //    };
   //    PSIO_REFLECT(cursor, method(seek, key), method(next))
   //
   // Resources are opaque across the WIT boundary — only their reflected
   // methods are visible.  Data members on resource types are ignored
   // by the WIT generator.
   struct wit_resource
   {
   };

   template <typename T>
   constexpr bool is_wit_resource_v = std::is_base_of_v<wit_resource, T>;

   // ── borrow<T> forward decl ─────────────────────────────────────────
   template <typename T>
   struct borrow;

   // ── wit_resource_drop — drop customization point ───────────────────
   //
   // The binding layer specialises this for each concrete resource type:
   //
   //   template <> inline void
   //   psio::wit_resource_drop<my_socket>(std::uint32_t h) {
   //      __wasi_sockets_drop_socket(h);  // WASM import on guest
   //   }
   //
   // Unspecialised types fall through to the leak-default below — this
   // keeps own<T> a valid C++ value type even before the binding code
   // is generated, but means runtime use without a specialisation will
   // silently leak handles.
   template <typename T>
   inline void wit_resource_drop(std::uint32_t /*handle*/)
   {
      /* default: leak */
   }

   // ── own<T> — owning u32 handle ─────────────────────────────────────
   //
   // Move-only RAII wrapper.  Calls wit_resource_drop<T>(handle) on
   // destruction unless release() has been used to surrender ownership
   // (typically when the handle crosses the ABI boundary).
   //
   // Example:
   //    own<cursor> open_cursor(std::uint32_t root);  // factory
   //    void close(own<cursor> c);                     // consumer
   template <typename T>
   struct own
   {
      static_assert(is_wit_resource_v<T>,
                    "own<T> requires T to inherit from psio::wit_resource");

      static constexpr std::uint32_t null_handle =
         static_cast<std::uint32_t>(-1);

      std::uint32_t handle;

      explicit own(std::uint32_t h) : handle(h) {}

      ~own()
      {
         if (handle != null_handle)
            wit_resource_drop<T>(handle);
      }

      own(const own&)            = delete;
      own& operator=(const own&) = delete;

      own(own&& o) noexcept : handle(o.handle) { o.handle = null_handle; }
      own& operator=(own&& o) noexcept
      {
         if (this != &o)
         {
            if (handle != null_handle)
               wit_resource_drop<T>(handle);
            handle   = o.handle;
            o.handle = null_handle;
         }
         return *this;
      }

      // Surrender ownership.  Used when handing the handle off across
      // the ABI — caller's destructor will not drop after this.
      [[nodiscard]] std::uint32_t release() noexcept
      {
         std::uint32_t h = handle;
         handle          = null_handle;
         return h;
      }

      operator borrow<T>() const { return borrow<T>{handle}; }
   };

   // ── borrow<T> — non-owning u32 handle ──────────────────────────────
   //
   // Valid only for the call that received it.  Multiple borrows of the
   // same resource may coexist; none take part in lifetime management.
   // In WIT resource methods, the implicit `self` parameter is a
   // borrow<self>.
   template <typename T>
   struct borrow
   {
      static_assert(is_wit_resource_v<T>,
                    "borrow<T> requires T to inherit from psio::wit_resource");

      std::uint32_t handle;

      explicit borrow(std::uint32_t h) : handle(h) {}
   };

   namespace detail
   {
      template <typename T>
      struct is_own_ct : std::false_type
      {
      };
      template <typename T>
      struct is_own_ct<own<T>> : std::true_type
      {
      };

      template <typename T>
      struct is_borrow_ct : std::false_type
      {
      };
      template <typename T>
      struct is_borrow_ct<borrow<T>> : std::true_type
      {
      };
   }  // namespace detail

}  // namespace psio

// ── psizam wasm_type_traits — runtime ABI lowering ─────────────────
//
// own<T> and borrow<T> both lower to a u32 (handle table index) at
// the WASM boundary.  The traits live in psizam to avoid a psizam
// dependency in psio — the forward declaration of the primary
// template is exactly what's needed here.
namespace psizam
{
   template <typename T, typename>
   struct wasm_type_traits;

   template <typename T>
   struct wasm_type_traits<::psio::own<T>, void>
   {
      static constexpr bool is_wasm_type = true;
      using wasm_type                    = std::uint32_t;
      // Transfer ownership: release() prevents the destructor from
      // dropping after the receiver takes it.
      static std::uint32_t      unwrap(::psio::own<T> o) noexcept
      {
         return o.release();
      }
      static ::psio::own<T>    wrap(std::uint32_t v) noexcept
      {
         return ::psio::own<T>{v};
      }
   };

   template <typename T>
   struct wasm_type_traits<::psio::borrow<T>, void>
   {
      static constexpr bool is_wasm_type = true;
      using wasm_type                    = std::uint32_t;
      static constexpr std::uint32_t      unwrap(::psio::borrow<T> b)
      {
         return b.handle;
      }
      static constexpr ::psio::borrow<T> wrap(std::uint32_t v)
      {
         return ::psio::borrow<T>{v};
      }
   };
}  // namespace psizam

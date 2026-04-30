#pragma once
//
// psio/error.hpp — error model (design doc § 5.4).
//
// `psio::validate` is the only operation that can report an error.
// Every other operation (encode, decode, size, make_boxed, view
// accessors) is `noexcept` and assumes its input has been validated.
//
// Return type: `[[nodiscard]] psio::codec_status` — a single-pointer
// move-only handle. Null pointer = OK; non-null = points to a heap-
// allocated `codec_error` payload. The OK path (the overwhelmingly
// common case) is one register-sized return; the error path costs a
// `make_unique<codec_error>`.
//
// Why a pointer instead of an inline 40-byte struct: every validate
// call had to zero-init two string_views + a uint32 + a bool and
// return ~48 bytes via 2-3 register stores, even on success. On
// fixed-shape validate paths (where the entire body collapses to
// "check span >= N") the return-value initialization was the same
// order of magnitude as the validate work itself. The pointer
// representation makes "no error" the natural default — ABIs return
// `nullptr` in a single register.
//
// Trade-off: codec_status is now move-only. The error payload is on
// the heap, which means codec_fail can throw bad_alloc. That's
// considered acceptable: the slow path is already the slow path, and
// running out of memory while reporting an error is unrecoverable
// either way.
//
// `[[nodiscard]]` at the class level + -Werror=unused-result keeps
// the "did you check the status?" invariant.

#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#if defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
#include <stdexcept>
#include <string>
#endif

namespace psio {

   // Structured error payload. No allocation inside the payload itself —
   // string_views must point to static storage (string literals).
   struct codec_error
   {
      std::string_view what{};          // static message, e.g. "offset out of range"
      std::uint32_t    byte_offset = 0; // where in the buffer the problem was
      std::string_view format_name{};   // "ssz", "frac", etc.

      constexpr codec_error() = default;
      constexpr codec_error(std::string_view w, std::uint32_t off,
                            std::string_view fmt) noexcept
         : what(w), byte_offset(off), format_name(fmt) {}
   };

#if defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
   // Exception type thrown by .or_throw(). Derives from runtime_error
   // so generic catch(std::exception&) works.
   class codec_exception : public std::runtime_error
   {
      codec_error err_;

    public:
      explicit codec_exception(codec_error e)
         : std::runtime_error(std::string{e.what}),
           err_(e) {}

      const codec_error& error() const noexcept { return err_; }
   };
#endif

   // ── codec_status ──────────────────────────────────────────────────────
   //
   // Move-only. Holds either nullptr (OK) or a unique_ptr to a heap
   // `codec_error`. The fast path is a single null pointer return.

   class [[nodiscard]] codec_status
   {
      std::unique_ptr<codec_error> err_;

    public:
      // Default → OK (null pointer, no allocation).
      codec_status() noexcept = default;

      // Error path. Allocates the payload. Use `codec_fail()` factory
      // for readability; this constructor is the implementation hook.
      explicit codec_status(codec_error e)
         : err_(std::make_unique<codec_error>(e)) {}

      codec_status(codec_status&&) noexcept            = default;
      codec_status& operator=(codec_status&&) noexcept = default;
      codec_status(const codec_status&)                = delete;
      codec_status& operator=(const codec_status&)     = delete;

      bool ok() const noexcept { return err_ == nullptr; }
      explicit operator bool() const noexcept { return ok(); }

      // Valid only when !ok(). UB to call otherwise — callers gate on
      // ok() / operator bool first.
      const codec_error& error() const noexcept { return *err_; }

#if defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
      // Opt-in exception path. Users who WANT exceptions call .or_throw()
      // explicitly; nothing in the library throws without being asked.
      void or_throw() const
      {
         if (err_) throw codec_exception{*err_};
      }
#endif
   };

   // Factory for the error path — reads cleaner than
   // `codec_status{codec_error{...}}`.
   inline codec_status codec_fail(std::string_view what,
                                  std::uint32_t    byte_offset,
                                  std::string_view format_name)
   {
      return codec_status{codec_error{what, byte_offset, format_name}};
   }

   // Factory for success. Returns a default-constructed (null) status —
   // one register-sized return.
   inline codec_status codec_ok() noexcept { return codec_status{}; }

}  // namespace psio

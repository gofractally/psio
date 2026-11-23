#pragma once
//
// psio/error.hpp — error model (design doc § 5.4).
//
// Checked validation/read APIs report codec_status. Fast decode, size and
// view accessors assume prevalidated input. Explicit fail-fast helpers throw
// codec_exception in native exception-enabled builds and abort in guests
// compiled without exceptions.
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
// `[[nodiscard]]` at the class level keeps the "did you check the
// status?" invariant: a dropped status is a -Wunused-result warning in any
// consumer, and an error (-Werror=unused-result) in psio's own targets.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

#if defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
#include <stdexcept>
#include <string>
#endif

namespace psio {

   namespace detail {
      // The text of a message built at run time: a reference count followed
      // by the characters, in one block. See codec_fail_owned.
      struct owned_text
      {
         std::atomic<std::size_t> refs{1};

         char*       chars() noexcept { return reinterpret_cast<char*>(this + 1); }
         static owned_text* make(std::string_view text)
         {
            void* block = ::operator new(sizeof(owned_text) + text.size() + 1);
            auto* out   = ::new (block) owned_text{};
            for (std::size_t i = 0; i < text.size(); ++i)
               out->chars()[i] = text[i];
            out->chars()[text.size()] = '\0';
            return out;
         }
         void retain() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
         void release() noexcept
         {
            if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
               this->~owned_text();
               ::operator delete(static_cast<void*>(this));
            }
         }
      };
   }  // namespace detail

   // Structured error payload.
   //
   // `what` and `format_name` are views. For the common case — a fixed
   // message — they point to static storage (string literals), the payload
   // allocates nothing, and a codec_error is a literal type: it can be a
   // constexpr variable. A message computed at run time ("Value 9 outside
   // allowed Precision range") is made with codec_fail_owned(): the text
   // is copied once into a reference-counted block that the codec_error
   // shares, `what` views it, and every copy of the codec_error — into
   // another status, into a codec_exception, into a writer's state — keeps
   // it alive. A moved-from codec_error has an empty `what`.
   struct codec_error
   {
      std::string_view what{};          // the message, e.g. "offset out of range"
      std::uint32_t    byte_offset = 0; // where in the buffer the problem was
      std::string_view format_name{};   // "ssz", "frac", a type name: static storage

      constexpr codec_error() = default;
      constexpr codec_error(std::string_view w, std::uint32_t off,
                            std::string_view fmt) noexcept
         : what(w), byte_offset(off), format_name(fmt) {}

      constexpr codec_error(const codec_error& other) noexcept
         : what(other.what), byte_offset(other.byte_offset), format_name(other.format_name),
           owned_(other.owned_)
      {
         if (owned_)
            owned_->retain();
      }
      constexpr codec_error(codec_error&& other) noexcept
         : what(other.what), byte_offset(other.byte_offset), format_name(other.format_name),
           owned_(other.owned_)
      {
         if (other.owned_)
         {
            other.owned_ = nullptr;
            other.what   = {};
         }
      }
      constexpr codec_error& operator=(const codec_error& other) noexcept
      {
         if (this != &other)
         {
            if (other.owned_)
               other.owned_->retain();
            if (owned_)
               owned_->release();
            what        = other.what;
            byte_offset = other.byte_offset;
            format_name = other.format_name;
            owned_      = other.owned_;
         }
         return *this;
      }
      constexpr codec_error& operator=(codec_error&& other) noexcept
      {
         if (this != &other)
         {
            if (owned_)
               owned_->release();
            what        = other.what;
            byte_offset = other.byte_offset;
            format_name = other.format_name;
            owned_      = other.owned_;
            if (other.owned_)
            {
               other.owned_ = nullptr;
               other.what   = {};
            }
         }
         return *this;
      }
      constexpr ~codec_error()
      {
         if (owned_)
            owned_->release();
      }

      // Whether `what` is text this error keeps alive (codec_fail_owned)
      // rather than static storage.
      constexpr bool owns_text() const noexcept { return owned_ != nullptr; }

      // `text`, copied; `format_name` is static storage as always.
      static codec_error with_owned_text(std::string_view text, std::uint32_t off,
                                         std::string_view fmt)
      {
         codec_error out;
         out.owned_      = detail::owned_text::make(text);
         out.what        = std::string_view{out.owned_->chars(), text.size()};
         out.byte_offset = off;
         out.format_name = fmt;
         return out;
      }

    private:
      detail::owned_text* owned_ = nullptr;
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
           err_(std::move(e)) {}

      const codec_error& error() const noexcept { return err_; }
   };
#endif

   // ── The abort handler ─────────────────────────────────────────────────
   //
   // Where exceptions are enabled a fail-fast operation — the checked
   // decode `from_F<T>(bytes)` on bytes its validator rejects, `to_F` of a
   // value with no encoding — throws psio::codec_exception, which carries
   // the error. Without exceptions (PSIO_EXCEPTIONS_ENABLED=0,
   // -fno-exceptions: a WebAssembly guest) it ends in std::abort(), which
   // says nothing. An embedder receives the error first, in one of two ways:
   //
   // 1. At run time. `psio::set_codec_abort_handler(fn)` installs a
   //    process-wide `void fn(const psio::codec_error&) noexcept`, called
   //    with the error just before std::abort(). The handler may itself not
   //    return (log and trap, longjmp, ...). The slot is one pointer,
   //    constant-initialized to null: there is no static initialization to
   //    order, and installing a handler from a static initializer is fine
   //    (it is in place for every failure after that point). Install it
   //    once, before other threads decode; it is read without
   //    synchronization.
   //
   // 2. At build time. Compile with
   //        -DPSIO_CODEC_ABORT_HANDLER=my_handler
   //    and define, once in the program,
   //        extern "C" void my_handler(const char* what, std::size_t what_size,
   //                                   std::uint32_t byte_offset,
   //                                   const char* format, std::size_t format_size);
   //    psio then calls it directly: no pointer, no registration, nothing
   //    to run at start-up. EVERY translation unit of the program that
   //    includes psio must be compiled with the same definition of the
   //    macro (codec_abort is an inline function; a mixed build violates
   //    the one-definition rule).
   //
   // A build that uses neither pays one null test on the failure path and
   // nothing anywhere else; the pointer is discarded by the linker when no
   // fail-fast operation is linked in. Neither form needs RTTI or
   // exceptions. With exceptions enabled the handler is not called: the
   // exception is the report.
   //
   // The text is `error.what` (not NUL-terminated), valid during the call.
   using codec_abort_handler = void (*)(const codec_error&) noexcept;

   namespace detail
   {
      inline constinit codec_abort_handler codec_abort_hook = nullptr;
   }

   // Installs `handler` (nullptr: none) and returns the previous one.
   inline codec_abort_handler set_codec_abort_handler(codec_abort_handler handler) noexcept
   {
      const codec_abort_handler previous = detail::codec_abort_hook;
      detail::codec_abort_hook           = handler;
      return previous;
   }
}  // namespace psio

#if defined(PSIO_CODEC_ABORT_HANDLER)
extern "C" void PSIO_CODEC_ABORT_HANDLER(const char* what, std::size_t what_size,
                                         std::uint32_t byte_offset, const char* format,
                                         std::size_t format_size);
#endif

namespace psio
{
   // What a fail-fast path does when it cannot throw: report, then abort.
   [[noreturn]] inline void codec_abort_without_exceptions(const codec_error& error) noexcept
   {
#if defined(PSIO_CODEC_ABORT_HANDLER)
      PSIO_CODEC_ABORT_HANDLER(error.what.data(), error.what.size(), error.byte_offset,
                               error.format_name.data(), error.format_name.size());
#else
      if (const codec_abort_handler handler = detail::codec_abort_hook)
         handler(error);
#endif
      std::abort();
   }

   [[noreturn]] inline void codec_abort(codec_error error)
   {
#if defined(__cpp_exceptions) && defined(PSIO_EXCEPTIONS_ENABLED) && PSIO_EXCEPTIONS_ENABLED
      throw codec_exception{error};
#else
      codec_abort_without_exceptions(error);
#endif
   }

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
         : err_(std::make_unique<codec_error>(std::move(e))) {}

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

   // The error path with a message built at run time: `what` is copied (one
   // allocation, on the error path only) and lives as long as any copy of
   // the error does. For a check whose message names the offending value;
   // a fixed message should stay a literal passed to codec_fail. Works the
   // same with exceptions disabled. `format_name` is still static storage.
   inline codec_status codec_fail_owned(std::string_view what,
                                        std::uint32_t    byte_offset,
                                        std::string_view format_name)
   {
      return codec_status{codec_error::with_owned_text(what, byte_offset, format_name)};
   }

   // Factory for success. Returns a default-constructed (null) status —
   // one register-sized return.
   inline codec_status codec_ok() noexcept { return codec_status{}; }

}  // namespace psio

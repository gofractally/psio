// An invariant whose message is computed from the value (codec_fail_owned,
// error.hpp), through every status-returning entry point: the message
// arrives unchanged and outlives the status that first carried it. Shared
// by a Catch test and by an executable built with exceptions and RTTI
// disabled: nothing here throws or aborts.
//
// run() returns the number of checks that did not hold (each is printed).

#pragma once

#include <psio/invariant.hpp>
#include <psio/json.hpp>
#include <psio/pssz.hpp>
#include <psio/pssz_ir_json.hpp>
#include <psio/reflect.hpp>
#include <psio/schema_builder.hpp>

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace owned_error_cases
{
   // psibase's Precision: 0..8, and the message names the value.
   struct Precision
   {
      std::uint8_t value = 0;
      static psio::codec_status check(const Precision& p) noexcept
      {
         if (p.value <= 8)
            return psio::codec_ok();
         char  text[48];
         const int n = std::snprintf(text, sizeof(text),
                                     "Value %u outside allowed Precision range",
                                     static_cast<unsigned>(p.value));
         return psio::codec_fail_owned(std::string_view{text, static_cast<std::size_t>(n)}, 0,
                                       "Precision");
      }
      PSIO_REFLECT(Precision, definitionWillNotChange(), invariant(check), value)
   };
   struct Token
   {
      std::uint32_t          id = 0;
      Precision              precision;
      std::vector<Precision> history;
      PSIO_REFLECT(Token, id, precision, history)
   };

   inline int& failures()
   {
      static int count = 0;
      return count;
   }
#define OWNED_CHECK(...)                                                                  \
   do                                                                                     \
   {                                                                                      \
      if (!(__VA_ARGS__))                                                                 \
      {                                                                                   \
         ++::owned_error_cases::failures();                                               \
         std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);   \
      }                                                                                   \
   } while (false)

   inline bool says(const psio::codec_status& status, std::string_view what)
   {
      return !status.ok() && status.error().what == what &&
             status.error().format_name == "Precision";
   }

   inline int run()
   {
      failures() = 0;
      constexpr std::string_view nine = "Value 9 outside allowed Precision range";
      constexpr std::string_view big  = "Value 200 outside allowed Precision range";

      // The factory: the text is copied, and every copy of the error owns it.
      {
         psio::codec_error kept;
         {
            std::string text = "built at run time";
            auto        status = psio::codec_fail_owned(text, 7, "test");
            text.assign(text.size(), 'x');   // the source is gone
            OWNED_CHECK(!status.ok() && status.error().what == "built at run time");
            OWNED_CHECK(status.error().byte_offset == 7 && status.error().owns_text());
            kept = status.error();
            psio::codec_status moved = std::move(status);
            OWNED_CHECK(moved.error().what == "built at run time");
         }
         // ...also after the status is destroyed.
         OWNED_CHECK(kept.what == "built at run time");
         psio::codec_status again{kept};
         OWNED_CHECK(again.error().what == "built at run time");
         // A static message owns nothing.
         OWNED_CHECK(!psio::codec_fail("static", 0, "test").error().owns_text());
         OWNED_CHECK(psio::codec_fail_owned("", 0, "test").error().what.empty());
         // A fixed message is still a constant.
         constexpr psio::codec_error fixed{"fixed", 3, "test"};
         static_assert(fixed.what == "fixed" && fixed.byte_offset == 3 && !fixed.owns_text());
         constexpr psio::codec_error fixed_copy = fixed;
         static_assert(fixed_copy.what == "fixed");
         // Copy and move assignment keep, share and drop the text.
         psio::codec_error a = psio::codec_fail_owned("first", 0, "test").error();
         psio::codec_error b = psio::codec_fail_owned("second", 0, "test").error();
         a = b;
         OWNED_CHECK(a.what == "second" && b.what == "second");
         b = fixed;
         OWNED_CHECK(a.what == "second" && b.what == "fixed" && !b.owns_text());
         psio::codec_error c = std::move(a);
         OWNED_CHECK(c.what == "second" && a.what.empty() && !a.owns_text());
         a = std::move(c);
         OWNED_CHECK(a.what == "second" && a.owns_text());
      }

      OWNED_CHECK(psio::check_invariant(Precision{8}).ok());
      OWNED_CHECK(says(psio::check_invariant(Precision{9}), nine));
      OWNED_CHECK(says(psio::check_invariant(Precision{200}), big));

      // pssz: the writers and the checking readers.
      const Token good{1, {4}, {{0}, {8}}};
      Token       bad_member = good;
      bad_member.precision.value = 9;
      Token bad_element = good;
      bad_element.history[1].value = 200;
      std::vector<char> bytes;
      OWNED_CHECK(psio::encode_pssz_checked(psio::pssz{}, good, bytes).ok());
      std::vector<char> refused;
      OWNED_CHECK(says(psio::encode_pssz_checked(psio::pssz{}, bad_member, refused), nine));
      OWNED_CHECK(says(psio::encode_pssz_checked(psio::pssz{}, bad_element, refused), big));
      OWNED_CHECK(refused.empty());
      // The frame of the bad value: the good one with the byte changed.
      auto frame_of = [&](const Token& value) {
         std::vector<char> frame = bytes;
         // precision is the fifth byte of the fixed region (after the
         // header and the u32 id); history's last byte is the last byte.
         for (std::size_t i = 0; i < frame.size(); ++i)
            if (static_cast<unsigned char>(frame[i]) == 4 && i > 0 && value.precision.value != 4)
            {
               frame[i] = static_cast<char>(value.precision.value);
               break;
            }
         frame.back() = static_cast<char>(value.history.back().value);
         return frame;
      };
      const auto member_frame  = frame_of(bad_member);
      const auto element_frame = frame_of(bad_element);
      OWNED_CHECK(psio::validate<Token>(psio::pssz{}, std::span<const char>{bytes}).ok());
      OWNED_CHECK(says(psio::validate<Token>(psio::pssz{}, std::span<const char>{member_frame}),
                       nine));
      OWNED_CHECK(says(psio::validate<Token>(psio::pssz{}, std::span<const char>{element_frame}),
                       big));
      Token out;
      bool  unknown = false;
      OWNED_CHECK(says(psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{member_frame},
                                                 out, unknown),
                       nine));

      // Text: reader and writer.
      std::string text;
      OWNED_CHECK(psio::encode_json_checked(psio::json{}, good, text).ok());
      std::string none;
      OWNED_CHECK(says(psio::encode_json_checked(psio::json{}, bad_member, none), nine));
      OWNED_CHECK(none.empty());
      const std::string_view bad_text = R"({"id":1,"precision":{"value":9},"history":[]})";
      OWNED_CHECK(says(psio::decode_json_checked(psio::json{}, std::span<const char>{bad_text}, out),
                       nine));
      OWNED_CHECK(says(psio::validate<Token>(psio::ijson{}, std::span<const char>{bad_text}), nine));

      // Schema-driven, with the type's own handler.
      const auto schema = psio::SchemaBuilder{}.insert<Token>("Token").build();
      psio::pssz_ir::CustomHandlers handlers;
      handlers.add<Precision>();
      OWNED_CHECK(psio::pssz_ir::validate_pssz(schema, "Token", bytes, nullptr, &handlers).ok());
      OWNED_CHECK(says(psio::pssz_ir::validate_pssz(schema, "Token", member_frame, nullptr,
                                                    &handlers),
                       nine));
      OWNED_CHECK(says(psio::pssz_ir::validate_pssz(schema, "Token", element_frame, nullptr,
                                                    &handlers),
                       big));
      std::string ir_text;
      OWNED_CHECK(says(psio::pssz_ir::pssz_to_json(schema, "Token", psio::ijson{}, member_frame,
                                                   ir_text, nullptr, &handlers),
                       nine));
      std::vector<char> ir_bytes;
      OWNED_CHECK(says(psio::pssz_ir::json_to_pssz(schema, "Token", psio::ijson{}, bad_text,
                                                   ir_bytes, nullptr,
                                                   psio::json_parser::automatic,
                                                   &handlers),
                       nine));
      return failures();
   }
}  // namespace owned_error_cases

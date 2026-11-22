#pragma once

#include <psio/error.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace psio::detail
{
   struct pssz_field_layout
   {
      std::size_t fixed_size;
      bool        variable;
      bool        optional;
   };

   struct pssz_field_span
   {
      std::size_t begin = 0;
      std::size_t end = 0;
      bool        present = false;
   };

   // A compatible extension preserves the initial layout and appends only
   // optional fields. Consequently every unknown fixed-region slot is an
   // offset, and can delimit the last variable field the reader knows.
   // Layout metadata is compiled into the reader; no writer schema is needed.
   //
   // Two spellings of one parser. `pssz_record_spans_inline` is always
   // inlined into its caller: the typed codec passes a constexpr layout, so
   // the loops over `fields` unroll, the layout checks fold away and the
   // positions of the fixed fields become constants — one straight-line
   // frame parser per record type instead of an interpreter over a layout
   // table (decode of a nested record with a list ran 1.6x slower through
   // the shared out-of-line copy). `pssz_record_spans` is that out-of-line
   // copy, for callers whose layout is only known at run time (pssz_ir.hpp).
   template <std::size_t W>
   [[gnu::always_inline]] inline codec_status pssz_record_spans_inline(std::span<const char> bytes,
                                  std::size_t begin,
                                  std::size_t end,
                                  bool dwnc,
                                  std::span<const pssz_field_layout> fields,
                                  std::span<pssz_field_span> spans,
                                  bool& has_unknown) noexcept
   {
      static_assert(W == 1 || W == 2 || W == 4);
      has_unknown = false;
      if (begin > end || end > bytes.size() || fields.size() != spans.size())
         return codec_fail("pssz: invalid record span", 0, "pssz");
      for (auto& span : spans)
         span = {};
      std::size_t known_size = 0;
      for (const auto& field : fields)
      {
         if ((field.variable && field.fixed_size != W) ||
             (field.optional && !field.variable) ||
             field.fixed_size > std::numeric_limits<std::size_t>::max() - known_size)
            return codec_fail("pssz: invalid reader field layout", 0, "pssz");
         known_size += field.fixed_size;
      }
      auto read_offset = [&](std::size_t position) noexcept {
         std::uint32_t value = 0;
         for (std::size_t i = 0; i < W; ++i)
            value |= std::uint32_t(static_cast<unsigned char>(bytes[position + i])) << (i * 8);
         return std::size_t(value);
      };
      std::size_t fixed_start = begin;
      std::size_t fixed_size = known_size;
      if (!dwnc)
      {
         if (end - begin < W)
            return codec_fail("pssz: record header truncated", 0, "pssz");
         fixed_size = read_offset(begin);
         fixed_start += W;
      }
      if (fixed_size > end - fixed_start)
         return codec_fail("pssz: record fixed region truncated", 0, "pssz");
      if (fixed_size > known_size && (fixed_size - known_size) % W != 0)
         return codec_fail("pssz: partial unknown optional slot", 0, "pssz");
      has_unknown = fixed_size > known_size;

      const std::size_t payload_size = end - fixed_start;
      const std::size_t no_field = fields.size();
      std::size_t previous_field = no_field;
      std::size_t previous_offset = fixed_size;
      bool seen_variable = false;
      auto accept_offset = [&](std::size_t offset, std::size_t index) -> codec_status {
         if (offset > payload_size || offset < previous_offset ||
             (!seen_variable && offset != fixed_size))
            return codec_fail("pssz: invalid record payload offset", 0, "pssz");
         if (previous_field != no_field)
            spans[previous_field].end = fixed_start + offset;
         if (index != no_field)
            spans[index].begin = fixed_start + offset;
         previous_field = index;
         previous_offset = offset;
         seen_variable = true;
         return codec_ok();
      };

      std::size_t cursor = 0;
      bool missing = false;
      for (std::size_t i = 0; i < fields.size(); ++i)
      {
         const auto& field = fields[i];
         if (missing || field.fixed_size > fixed_size - cursor)
         {
            if (cursor != fixed_size)
               return codec_fail("pssz: header cuts a known field", 0, "pssz");
            if (!field.optional || dwnc)
               return codec_fail("pssz: required record field missing", 0, "pssz");
            missing = true;
            continue;
         }
         spans[i].present = true;
         if (field.variable)
         {
            auto status = accept_offset(read_offset(fixed_start + cursor), i);
            if (!status.ok())
               return status;
         }
         else
         {
            spans[i].begin = fixed_start + cursor;
            spans[i].end = spans[i].begin + field.fixed_size;
         }
         cursor += field.fixed_size;
      }
      if (has_unknown)
      {
         for (std::size_t offset = known_size; offset < fixed_size; offset += W)
         {
            auto status = accept_offset(read_offset(fixed_start + offset), no_field);
            if (!status.ok())
               return status;
         }
      }
      if (previous_field != no_field)
         spans[previous_field].end = end;
      if (!seen_variable && payload_size != fixed_size)
         return codec_fail("pssz: fixed record has trailing payload", 0, "pssz");
      return codec_ok();
   }

   template <std::size_t W>
   codec_status pssz_record_spans(std::span<const char> bytes,
                                  std::size_t begin,
                                  std::size_t end,
                                  bool dwnc,
                                  std::span<const pssz_field_layout> fields,
                                  std::span<pssz_field_span> spans,
                                  bool& has_unknown) noexcept
   {
      return pssz_record_spans_inline<W>(bytes, begin, end, dwnc, fields, spans, has_unknown);
   }
}

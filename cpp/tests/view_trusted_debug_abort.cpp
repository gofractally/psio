// A trusted view over bytes that are not a frame of its type fails where a
// member cannot be placed (view.hpp "Safe and trusted views";
// gofractally/psio#15): psio::codec_abort, which nothing here catches.
//
//   view_trusted_debug_abort <case>
//
// Each case hands corrupted bytes to a trusted view — breaking the caller's
// guarantee — and reads through it. The process must die with the message
// of psio::detail::view_malformed; reaching the end is the failure (exit
// status 0, "NOT CAUGHT"). `valid` is the control: the same reads on the
// valid bytes return normally. (The file keeps the name it had when these
// compares were made only under PSIO_DEBUG_VIEWS.)

#include <psio/pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/wit.hpp>
#include <psio/wit_view.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
   struct Item
   {
      std::string   product;
      std::uint32_t qty = 0;
   };
   PSIO_REFLECT(Item, product, qty)
   struct Order
   {
      std::uint64_t              id = 0;
      std::string                name;
      std::vector<Item>          items;
      std::vector<std::uint32_t> numbers;
      std::string                note;
   };
   PSIO_REFLECT(Order, id, name, items, numbers, note)

   Order sample()
   {
      return Order{7, "Alice", {{"a", 1}, {"bb", 2}, {"ccc", 3}}, {10, 20, 30}, "note"};
   }

   template <typename V>
   std::uint64_t walk(const V& v)
   {
      std::uint64_t sum = v.id() + v.name().size() + v.note().size();
      for (auto item : v.items())
         sum += item.qty() + item.product().size();
      for (std::uint32_t n : v.numbers())
         sum += n;
      return sum;
   }

   void put32(std::vector<char>& bytes, std::size_t at, std::uint32_t value)
   {
      std::memcpy(bytes.data() + at, &value, 4);
   }
   std::uint32_t get32(const std::vector<char>& bytes, std::size_t at)
   {
      std::uint32_t value = 0;
      std::memcpy(&value, bytes.data() + at, 4);
      return value;
   }
}  // namespace

int main(int argc, char** argv)
{
   const std::string_view which = argc > 1 ? argv[1] : "";
   const Order            order = sample();
   std::uint64_t          result = 0;

   if (which == "valid")
   {
      const auto p = psio::to_pssz(order);
      const auto s = psio::to_ssz(order);
      const auto w = psio::to_wit(order);
      result = walk(psio::pssz_trusted_view<Order>{psio::trusted{p}}) +
               walk(psio::ssz_trusted_view<Order>{psio::trusted{s}}) +
               walk(psio::wit_trusted_view<Order>{psio::trusted{w}});
      std::printf("valid bytes read normally: %llu\n", static_cast<unsigned long long>(result));
      return result == 3 * walk(psio::pssz_view<Order>{std::span<const char>{p}}) ? 0 : 1;
   }
   else if (which == "pssz-record-header")
   {
      // The record's header announces a fixed region larger than the record.
      auto bytes = psio::to_pssz(order);
      std::vector<char> bad(bytes.begin(), bytes.end());
      bad[0] = char(0xff);
      bad[1] = char(0xff);
      result = walk(psio::pssz_trusted_view<Order>{psio::trusted{bad}});
   }
   else if (which == "pssz-member-offset")
   {
      // Every byte of the encoding in turn, last first, set to 0xff: the
      // first frame validation refuses has a broken offset in the lists or
      // their elements.
      auto bytes = psio::to_pssz(order);
      for (std::size_t i = bytes.size(); i-- > 0;)
      {
         std::vector<char> bad(bytes.begin(), bytes.end());
         bad[i] = char(0xff);
         if (psio::validate_pssz<Order>(bad).ok())
            continue;
         result += walk(psio::pssz_trusted_view<Order>{psio::trusted{bad}});
      }
   }
   else if (which == "pssz-truncated")
   {
      auto bytes = psio::to_pssz(order);
      std::vector<char> bad(bytes.begin(), bytes.begin() + 6);
      result = walk(psio::pssz_trusted_view<Order>{psio::trusted{bad}});
   }
   else if (which == "pssz-index")
   {
      // operator[] past the end fails (at(i) is the form that returns a
      // view of nothing).
      const auto bytes = psio::to_pssz(order);
      const psio::pssz_trusted_view<Order> v{psio::trusted{bytes}};
      result = v.numbers()[3];
   }
   else if (which == "pssz-scalar")
   {
      const std::vector<char> two(2, char(1));
      result = psio::pssz_trusted_view<std::uint32_t>{psio::trusted{two}}.get();
   }
   else if (which == "ssz-offset")
   {
      // The offset of `name` (the first variable member, after the u64 id)
      // moved past the end of the bytes.
      auto bytes = psio::to_ssz(order);
      std::vector<char> bad(bytes.begin(), bytes.end());
      put32(bad, 8, static_cast<std::uint32_t>(bad.size() + 100));
      result = walk(psio::ssz_trusted_view<Order>{psio::trusted{bad}});
   }
   else if (which == "ssz-truncated")
   {
      auto bytes = psio::to_ssz(order);
      std::vector<char> bad(bytes.begin(), bytes.begin() + 10);
      result = walk(psio::ssz_trusted_view<Order>{psio::trusted{bad}});
   }
   else if (which == "wit-pointer")
   {
      // The (ptr, len) of `name` (after the u64 id): the length runs past
      // the buffer.
      auto bytes = psio::to_wit(order);
      std::vector<char> bad(bytes.begin(), bytes.end());
      put32(bad, 12, get32(bad, 12) + 100000);
      result = walk(psio::wit_trusted_view<Order>{psio::trusted{bad}});
   }
   else
   {
      std::fprintf(stderr, "unknown case '%s'\n", argv[1] ? argv[1] : "");
      return 2;
   }
   std::printf("NOT CAUGHT: %s read %llu\n", argv[1], static_cast<unsigned long long>(result));
   return 0;
}

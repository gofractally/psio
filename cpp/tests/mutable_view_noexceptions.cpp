// psio::mutable_view with exceptions and RTTI disabled (how a WebAssembly
// guest builds it): validated construction, navigation and every refused
// write report through return values.
#include <psio/mutable_view.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct Position { std::int32_t x = 0; std::int32_t y = 0; };
PSIO_REFLECT(Position, x, y, definitionWillNotChange())
struct Account
{
   std::uint64_t                balance = 0;
   std::string                  owner;
   bool                         frozen = false;
   std::vector<std::uint32_t>   history;
   std::array<Position, 2>      corners{};
   psio::bitvector<5>           flags;
   std::optional<std::uint16_t> limit;
};
PSIO_REFLECT(Account, balance, owner, frozen, history, corners, flags, limit)

template <typename F>
int run()
{
   Account account{10, "alice", false, {1, 2, 3}, {Position{1, 2}, Position{3, 4}}, {}, 7};
   std::vector<char> bytes;
   if (!psio::encode_pssz_checked(F{}, account, bytes).ok()) return 1;

   std::optional<psio::mutable_view<Account, F>> view;
   bool unknown = false;
   if (!psio::validated_pssz_mutable_view<Account>(F{}, std::span<char>{bytes}, view, unknown).ok())
      return 2;
   if (!view->template get<0>().set(11)) return 3;
   if (!view->template get<2>().set(true)) return 4;
   if (!view->template get<3>()[1].set(20)) return 5;
   if (!view->template get<4>()[1].template get<0>().set(-3)) return 6;
   if (!view->template get<4>()[0].set(Position{5, 6})) return 7;
   if (!(*view->template get<6>()).set(8)) return 8;
   // Refused writes leave the frame alone.
   if (view->template get<3>().at(3).set(1)) return 9;
   psio::bitvector<5> stray;
   stray.data()[0] = 0x20;
   if (view->template get<5>().set(stray)) return 10;

   account.balance = 11;
   account.frozen = true;
   account.history[1] = 20;
   account.corners = {Position{5, 6}, Position{-3, 4}};
   account.limit = 8;
   std::vector<char> expected;
   if (!psio::encode_pssz_checked(F{}, account, expected).ok()) return 11;
   if (bytes != expected) return 12;
   // Byte equality with a fresh encoding is the canonical check here: strict
   // validation of this type is not available in a no-exceptions build.
   if (!psio::validate<Account>(F{}, bytes).ok()) return 13;

   // An absent optional has no payload to overwrite.
   account.limit.reset();
   bytes.clear();
   if (!psio::encode_pssz_checked(F{}, account, bytes).ok()) return 15;
   psio::mutable_view<Account, F> pruned{bytes};
   if (pruned.template get<6>().has_value() || (*pruned.template get<6>()).set(1)) return 16;
   if (!psio::validate<Account>(F{}, bytes).ok()) return 17;
   return 0;
}

int main()
{
   if (int status = run<psio::pssz8>()) return status;
   if (int status = run<psio::pssz16>()) return 20 + status;
   if (int status = run<psio::pssz32>()) return 40 + status;
   if (int status = run<psio::pssz>()) return 60 + status;
   return 0;
}

// Stated labels of a variant are distinct: two alternatives with one label
// could not be told apart in text, so it must not compile.
#include <psio/json.hpp>
#include <psio/variant_label.hpp>

#include <array>
#include <string_view>
#include <variant>

using Twice = std::variant<int, bool>;
template <>
struct psio::variant_labels<Twice>
{
   static constexpr std::array<std::string_view, 2> names{"value", "value"};
};

int main()
{
   return static_cast<int>(psio::to_ijson(Twice{1}).size());
}

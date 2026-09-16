#include <psio/frac.hpp>
#include <psio/pjson.hpp>
#include <psio/reflect.hpp>

struct Message {
   std::uint32_t id;
   std::string text;
};
PSIO_REFLECT(Message, id, text)

int main() {
   const Message input{42, "standalone psio"};
   auto bytes = psio::encode(psio::frac32{}, input);
   if (!psio::validate<Message>(psio::frac32{}, std::span<const char>{bytes}).ok())
      return 1;
   auto decoded = psio::decode<Message>(psio::frac32{}, std::span<const char>{bytes});
   if (decoded.id != input.id || decoded.text != input.text)
      return 2;
   auto five = psio::pjson::encode(psio::pjson_value{std::int64_t{5}});
   return five == std::vector<std::uint8_t>{0x25} ? 0 : 3;
}

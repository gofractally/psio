// Line-oriented pjson validator used by tools/check-pjson-validation.py.
// One hex frame per line in; per line out:
//   invalid                        not a well-formed frame (psio::pjson_lazy)
//   valid <c> <hex>|noform         well-formed; <c> is 1 when it is the
//                                  canonical frame of its value (psio::pjson);
//                                  then the canonical frame, or `noform` when
//                                  the value has none (text that is not UTF-8)
#include <psio/pjson.hpp>
#include <iostream>
#include <string>
int main() {
  std::string hex;
  while (std::getline(std::cin, hex)) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i=0; i+1<hex.size(); i+=2) bytes.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i,2),nullptr,16)));
    if (!psio::pjson_lazy::validate(bytes)) { std::cout << "invalid\n"; continue; }
    (void)psio::pjson_lazy::try_decode(bytes);
    std::cout << "valid " << psio::pjson::validate(bytes) << ' ';
    try {
      constexpr char digits[] = "0123456789abcdef";
      for (auto b : psio::pjson::canonicalize(bytes)) std::cout << digits[b >> 4] << digits[b & 15];
    } catch (const std::exception&) { std::cout << "noform"; }
    std::cout << '\n';
  }
}

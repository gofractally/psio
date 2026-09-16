#include <psio/pjson.hpp>
#include <iostream>
#include <string>
int main() {
  std::string hex;
  while (std::cin >> hex) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i=0; i<hex.size(); i+=2) bytes.push_back(std::stoi(hex.substr(i,2),nullptr,16));
    bool valid = psio::pjson::validate(bytes);
    if (valid) (void)psio::pjson::try_decode(bytes);
    std::cout << valid << '\n';
  }
}

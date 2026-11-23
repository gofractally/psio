// psio::json_value with exceptions and RTTI off: a guest reads any JSON,
// looks into it, edits it and writes it, in text and in PSSZ.
#include <psio/json.hpp>
#include <psio/pssz.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

struct Pass
{
   std::string      name;
   psio::json_value config;
   PSIO_REFLECT(Pass, name, config)
};

int main()
{
   const std::string_view text = R"({"name":"n","config":{"port":8080,"hosts":["a","b"],"ratio":0.50}})";
   Pass                   p;
   if (!psio::decode_json_checked(psio::ijson{}, text, p).ok())
      return 1;
   const psio::json_value* port = p.config.find("port");
   if (!port || !port->as_number() || port->as_number()->as_int64() != 8080)
      return 2;
   std::string out;
   if (!psio::encode_json_checked(psio::ijson{}, p, out).ok() || out != text)
      return 3;
   p.config.set("port", 9090);
   p.config.erase("hosts");
   out.clear();
   if (!psio::encode_json_checked(psio::json{}, p, out).ok() ||
       out != R"({"name":"n","config":{"port":9090,"ratio":0.50}})")
      return 4;
   // Malformed text is a status.
   psio::json_value v;
   if (psio::decode_json_checked(psio::json{}, R"({"a":)", v).ok())
      return 5;
   if (psio::encode_json_checked(psio::json{}, psio::json_value{psio::json_number{"1."}}, out).ok())
      return 6;
   // PSSZ: a UTF-8 string holding the compact text.
   std::vector<char> bytes;
   if (!psio::encode_pssz_checked(psio::pssz{}, p, bytes).ok())
      return 7;
   if (!psio::validate<Pass>(psio::pssz{}, std::span<const char>{bytes}).ok())
      return 8;
   Pass back;
   bool unknown = false;
   if (!psio::decode_pssz_checked(psio::pssz{}, std::span<const char>{bytes}, back, unknown).ok() ||
       !(back.config == p.config))
      return 9;
   std::puts("json_value noexceptions ok");
   return 0;
}

// Time values and psio::key with exceptions and RTTI disabled: text, json,
// ijson and key reading report through codec_status / return values.

#include <psio/json.hpp>
#include <psio/key.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/time.hpp>

#include <chrono>

using namespace std::chrono;

struct GuestStamp
{
   sys_time<microseconds> at;
   milliseconds           ttl{};
   std::string            who;
   friend bool operator==(const GuestStamp&, const GuestStamp&) = default;
};
PSIO_REFLECT(GuestStamp, at, ttl, who)

template <typename Fmt>
int run(int base)
{
   const GuestStamp  value{sys_time<microseconds>{microseconds{1795342530123456}}, milliseconds{-7},
                           "bob"};
   const std::string want =
      R"({"at":"2026-11-22T10:15:30.123456Z","ttl":-7,"who":"bob"})";
   std::string text;
   if (!psio::encode_json_checked(Fmt{}, value, text).ok()) return base + 1;
   if (text != want) return base + 2;
   GuestStamp back;
   if (!psio::decode_json_checked(Fmt{}, std::span<const char>{text}, back).ok() || !(back == value))
      return base + 3;
   // Bad text and values without text: a status, never an abort.
   for (std::string_view bad :
        {R"({"at":"2026-11-22T10:15:60Z","ttl":1,"who":""})",
         R"({"at":"2026-11-22T10:15:30.1234567Z","ttl":1,"who":""})",
         R"({"at":"2026-11-22T10:15:30","ttl":1,"who":""})", R"({"at":17,"ttl":1,"who":""})",
         R"({"at":"2026-11-22T10:15:30Z","ttl":"PT1S","who":""})"})
      if (psio::decode_json_checked(Fmt{}, std::span<const char>{bad}, back).ok() ||
          psio::validate<GuestStamp>(Fmt{}, std::span<const char>{bad}).ok())
         return base + 4;
   GuestStamp  far = value;
   far.at          = sys_time<microseconds>::max();
   std::string out = "keep";
   if (psio::encode_json_checked(Fmt{}, far, out).ok() || out != "keep") return base + 5;
   return 0;
}

int main()
{
   if (int rc = run<psio::json>(0)) return rc;
   if (int rc = run<psio::ijson>(10)) return rc;

   char        buf[psio::max_time_text_size];
   sys_seconds t{};
   if (psio::write_time_text(sys_seconds::max(), buf) != nullptr) return 21;
   if (psio::parse_time_text("2026-11-22T10:15:30Z", t) != nullptr) return 22;
   if (psio::parse_time_text("2026-11-22T10:15:30", t) == nullptr) return 23;

   // key: checked validation of a composite key; binary round trip.
   using K = std::tuple<sys_seconds, std::string, std::optional<std::uint32_t>>;
   const K    k{t, "bob", 7};
   const auto bytes = psio::encode(psio::key{}, k);
   if (!psio::validate<K>(psio::key{}, std::span<const char>{bytes}).ok()) return 24;
   if (!(psio::decode<K>(psio::key{}, std::span<const char>{bytes}) == k)) return 25;
   for (std::size_t cut = 0; cut < bytes.size(); ++cut)
      if (psio::validate<K>(psio::key{}, std::span<const char>{bytes.data(), cut}).ok()) return 26;
   if (psio::key_prefix<K>(t, "bob") !=
       std::vector<char>(bytes.begin(), bytes.begin() + 13))
      return 27;
   const auto wire = psio::encode(psio::pssz{}, std::tuple{t, milliseconds{5}});
   if (!(psio::decode<std::tuple<sys_seconds, milliseconds>>(psio::pssz{},
                                                             std::span<const char>{wire}) ==
         std::tuple{t, milliseconds{5}}))
      return 28;
   return 0;
}

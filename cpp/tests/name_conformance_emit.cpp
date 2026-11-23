// Emits conformance/name-vectors.json: the shared vectors of the two name
// coders (psio/name.hpp name_to_number / number_to_name, and
// psio/compress_name.hpp method_to_number / number_to_method).
//
//   psio_name_conformance_emit <out.json>          write the fixture
//   psio_name_conformance_emit --check <ref.json>  fail unless <ref.json> is
//                                                  what this build generates
//
// { "version": 1,
//   "generator": "...",
//   "names":   [ [text, method_number, method_text, name_number, name_text], ... ],
//   "numbers": [ [number, method_text, name_text], ... ] }
//
// Numbers are decimal strings. `names`: both encoders applied to `text`, and
// each decoder applied to its encoder's result. `numbers`: both decoders
// applied to an arbitrary number (most are no name's: the row pins what the
// decoder writes anyway). The rows are the format of
// rust/psio/tests/fixtures/compress-name-vectors.json, whose corpus this
// one starts with.
#include <psio/compress_name.hpp>
#include <psio/name.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
   [[noreturn]] void die(const std::string& what)
   {
      std::fprintf(stderr, "name conformance emit: %s\n", what.c_str());
      std::exit(1);
   }
   std::string esc(const std::string& s)
   {
      std::string o;
      char        b[8];
      for (unsigned char c : s)
      {
         if (c == '"' || c == '\\')
         {
            o += '\\';
            o += static_cast<char>(c);
         }
         else if (c < 0x20 || c >= 0x7f)
         {
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
         }
         else
            o += static_cast<char>(c);
      }
      return o;
   }
   std::string fold(const std::string& s)
   {
      using M = psio::detail::func_model;
      std::string o;
      for (unsigned char c : s)
         o += M::symbol_to_char[M::char_to_symbol[c]];
      return o;
   }

   std::string render()
   {
      // The corpus of rust/psio/tests/fixtures/compress-name-vectors.gen.cpp ...
      std::vector<std::string> names = {
          "", "a", "b", "c", "e", "z", "abc123", "spiderman", "brucewayne", "anthonystark",
          "natasharomanoff", "abcdefghijklmnopqrstuvwxyz", "A", "Abc", "transfer", "balance",
          "account", "hello", "world", "test", "name", "get", "set", "run", "init", "ping", "pong",
          "alice", "bob", "carol", "prod", "root", "symbol", "tokens", "transact", "setcode",
          "startsession", "loginsys", "credit", "uncredit", "thisisaverylongnamethatwontcompress",
          "?", "what?", "?what", "eaorintsl?", "????", "#hneunophpilcroch", "#niiutpmlecuamehe",
          "#psaoryoiluhlrpyn", "#", "#short", "#toolongtoolongtoolong", "#hneunophpilcro?h",
          "#0123456789abcdef", "a-b", "a_b", "get_balance", "1abc", "-abc", "abc-", "a1", "a-1",
          "zzzzzzzzzzzzzzzzzz", "zzzzzzzzzzzzzzzzzzz", "qqqqqqqqqqqq", "jjjjjjjjjjjjj",
          "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", "e-e-e-e-e-e-e-e-e-", "abcdefghijklmnopqr",
          "0", "9", "x9", "my-service", "http-server", "r-tokens", "with space", "UPPER", "MiXeD",
          "settings", "x", "q", "j", "qj", "jq", "zx", "aaaaaaaaaaaaaaaaaaaaaaaa", "storeSys",
          "exec", "recv", "send",
          // ... and the names whose arithmetic code does not read back as the
          // name. Identifier names: a doubled hyphen, a code past 64 bits.
          "x--b", "a--b", "a--", "te6--2", "gt--", "uoc--ew", "grryhn4d3x0", "grryhn4d3xl",
          "sqhwqzfkinp", "sqhwqzfkino",
          // Method names: a code cut at 64 bits (they take the hash form), in
          // either case, with '_' and digits; hash texts, well-formed or not.
          "getSymbolType", "getsymboltype", "getsymboltyp", "checkFirstAuth", "removeCallback",
          "cpu_min_unit", "get_resources", "frozenAccounts", "regEvIdx", "rowsBySecondary",
          "create_request", "set_paid_occ", "rfx", "esxkx", "qxgbfjzl", "#hmlnslheseoypnnp",
          "#HMLNSLHESEOYPNNP", "#eeeeeeeeeeeeeeee", "#eeeeeeeeeeeeeeae", "#abc",
          "#hmlnslheseoypnnpe", "#hmlnslheseoypnnz", "#hmlnslheseoypn-p"};
      std::mt19937_64   rng(12345);
      const std::string a1 = "abcdefghijklmnopqrstuvwxyz", a2 = "abcdefghijklmnopqrstuvwxyz0123456789-",
                        a3 = "eaiotnrslcu", a4 = "abcdefghijklmnopqrstuvwxyz0123456789-_#?AZ";
      for (int i = 0; i < 1500; i++)
      {
         const std::string& al = (i % 4 == 0) ? a1 : (i % 4 == 1) ? a2 : (i % 4 == 2) ? a3 : a4;
         const int          len = 1 + static_cast<int>(rng() % ((i % 7 == 0) ? 26 : 14));
         std::string        s;
         for (int j = 0; j < len; j++)
            s += al[rng() % al.size()];
         names.push_back(s);
      }

      std::ostringstream out;
      out << "{\n\"version\": 1,\n\"generator\": \"cpp/tests/name_conformance_emit.cpp: C++ "
             "psio::detail::method_to_number / number_to_method (psio/compress_name.hpp) and "
             "psio::name_to_number / number_to_name (psio/name.hpp)\",\n\"names\": [\n";
      std::set<std::string> seen;
      bool                  first = true;
      for (const auto& n : names)
      {
         if (!seen.insert(n).second)
            continue;
         const std::uint64_t m  = psio::detail::method_to_number(n);
         const std::uint64_t a  = psio::name_to_number(n);
         const std::string   mt = psio::detail::number_to_method(m);
         const std::string   at = psio::number_to_name(a);
         // The properties the vectors pin.
         if (a != 0 && at != n)
            die("name_to_number(" + n + ") does not decode to the name");
         if (m != 0 && psio::detail::method_to_number(mt) != m)
            die("method_to_number(" + n + ") is not the number of its own text");
         if (m != 0 && !psio::detail::is_hash_name(m) && mt != fold(n))
            die("method_to_number(" + n + ") does not decode to the canonical name");
         out << (first ? "" : ",\n") << "[\"" << esc(n) << "\",\"" << m << "\",\"" << esc(mt)
             << "\",\"" << a << "\",\"" << esc(at) << "\"]";
         first = false;
      }
      out << "\n],\n\"numbers\": [\n";
      first = true;
      std::vector<std::uint64_t> nums = {1,
                                         2,
                                         255,
                                         256,
                                         196,
                                         32783,
                                         32884,
                                         0xffffffffffffffffull,
                                         0x8000000000000000ull,
                                         0x0100000000000000ull,
                                         0x00ffffffffffffffull,
                                         311625498215ull,
                                         6905860632893337981ull,
                                         13346021867974402139ull,
                                         // what the coders used to give names they
                                         // could not represent
                                         690109440766ull,
                                         0x7ea96086faa7f7c0ull,
                                         0xd55df3070b8758ebull};
      for (int i = 0; i < 600; i++)
      {
         std::uint64_t v = rng();
         if (i % 3 == 0)
            v >>= (rng() % 56);
         if (i % 5 == 0)
            v &= ~(1ull << 56);
         nums.push_back(v);
      }
      for (auto v : nums)
      {
         out << (first ? "" : ",\n") << "[\"" << v << "\",\""
             << esc(psio::detail::number_to_method(v)) << "\",\"" << esc(psio::number_to_name(v))
             << "\"]";
         first = false;
      }
      out << "\n]\n}\n";
      return out.str();
   }
}  // namespace

int main(int argc, char** argv)
{
   const bool check = argc == 3 && std::string_view(argv[1]) == "--check";
   if (!check && argc != 2)
   {
      std::fprintf(stderr, "usage: %s <out.json> | --check <ref.json>\n", argv[0]);
      return 2;
   }
   const std::string text = render();
   if (check)
   {
      std::ifstream     in(argv[2], std::ios::binary);
      std::stringstream buf;
      buf << in.rdbuf();
      if (!in || buf.str() != text)
      {
         std::fprintf(stderr,
                      "name conformance emit: %s is not what the implementation generates; "
                      "regenerate it only for an intended change of a coder\n",
                      argv[2]);
         return 1;
      }
      return 0;
   }
   std::ofstream out(argv[1], std::ios::binary);
   out << text;
   return out ? 0 : 1;
}

#include <psio/compress_name.hpp>
#include <psio/name.hpp>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include <set>
static std::string esc(const std::string& s){ std::string o; char b[8]; for(unsigned char c: s){ if(c=='"'||c=='\\'){o+='\\';o+=c;} else if(c<0x20||c>=0x7f){ snprintf(b,sizeof b,"\\u%04x",c); o+=b;} else o+=c;} return o; }
int main(){
  std::vector<std::string> names = {"", "a","b","c","e","z","abc123","spiderman","brucewayne","anthonystark","natasharomanoff",
    "abcdefghijklmnopqrstuvwxyz","A","Abc","transfer","balance","account","hello","world","test","name","get","set","run","init",
    "ping","pong","alice","bob","carol","prod","root","symbol","tokens","transact","setcode","startsession","loginsys","credit","uncredit",
    "thisisaverylongnamethatwontcompress","?","what?","?what","eaorintsl?","????","#hneunophpilcroch","#niiutpmlecuamehe","#psaoryoiluhlrpyn",
    "#","#short","#toolongtoolongtoolong","#hneunophpilcro?h","#0123456789abcdef","a-b","a_b","get_balance","1abc","-abc","abc-","a1","a-1",
    "zzzzzzzzzzzzzzzzzz","zzzzzzzzzzzzzzzzzzz","qqqqqqqqqqqq","jjjjjjjjjjjjj","eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","e-e-e-e-e-e-e-e-e-",
    "abcdefghijklmnopqr","0","9","x9","my-service","http-server","r-tokens","with space","UPPER","MiXeD","settings","x","q","j","qj","jq","zx",
    "aaaaaaaaaaaaaaaaaaaaaaaa", "storeSys", "exec", "recv", "send"};
  std::mt19937_64 rng(12345);
  const std::string a1="abcdefghijklmnopqrstuvwxyz", a2="abcdefghijklmnopqrstuvwxyz0123456789-", a3="eaiotnrslcu", a4="abcdefghijklmnopqrstuvwxyz0123456789-_#?AZ";
  for(int i=0;i<1500;i++){ const std::string& al = (i%4==0)?a1:(i%4==1)?a2:(i%4==2)?a3:a4; int len=1+rng()%((i%7==0)?26:14); std::string s; for(int j=0;j<len;j++) s+=al[rng()%al.size()]; names.push_back(s);}  
  std::set<std::string> seen; 
  printf("{\n\"generator\": \"C++ psio::detail::method_to_number / number_to_method (psio/compress_name.hpp) and psio::name_to_number / number_to_name (psio/name.hpp)\",\n\"names\": [\n");
  bool first=true;
  for(auto& n: names){ if(!seen.insert(n).second) continue; uint64_t m=psio::detail::method_to_number(n); uint64_t a=psio::name_to_number(n);
    printf("%s[\"%s\",\"%llu\",\"%s\",\"%llu\",\"%s\"]", first?"":",\n", esc(n).c_str(), (unsigned long long)m, esc(psio::detail::number_to_method(m)).c_str(), (unsigned long long)a, esc(psio::number_to_name(a)).c_str()); first=false; }
  printf("\n],\n\"numbers\": [\n"); first=true;
  std::vector<uint64_t> nums={1,2,255,256,196,32783,32884,0xffffffffffffffffull,0x8000000000000000ull,0x0100000000000000ull,0x00ffffffffffffffull,311625498215ull,6905860632893337981ull,13346021867974402139ull};
  for(int i=0;i<600;i++){ uint64_t v=rng(); if(i%3==0) v>>= (rng()%56); if(i%5==0) v &= ~(1ull<<56); nums.push_back(v);} 
  for(auto v: nums){ printf("%s[\"%llu\",\"%s\",\"%s\"]", first?"":",\n", (unsigned long long)v, esc(psio::detail::number_to_method(v)).c_str(), esc(psio::number_to_name(v)).c_str()); first=false; }
  printf("\n]\n}\n");
}

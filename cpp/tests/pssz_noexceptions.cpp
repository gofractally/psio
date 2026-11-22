#include <psio/pssz_view.hpp>
struct Old { std::string name; };PSIO_REFLECT(Old,name)
struct New { std::string name; std::optional<std::string> extra; };PSIO_REFLECT(New,name,extra)
int main() {
 auto bytes=psio::encode(psio::pssz8{},New{"a","b"});Old old;bool unknown=false;
 auto status=psio::decode_pssz_checked(psio::pssz8{},bytes,old,unknown);
 if(!status.ok() || !unknown || old.name!="a") return 1;
 std::optional<psio::view<Old,psio::pssz8,psio::storage::const_borrow>> view;
 status=psio::validated_pssz_view<Old>(psio::pssz8{},bytes,view,unknown);
 if(!status.ok() || !view || view->get<0>().view_()!="a") return 2;
 auto exact=psio::validate_strict<New>(psio::pssz8{},bytes);
 if(!exact.ok()) return 3;
 // Mandatory pruning and header checks report through codec_status in an
 // exception-disabled build, for explicit widths and the automatic profile.
 const auto canonical=psio::encode(psio::pssz8{},New{"a",std::nullopt});
 if(canonical!=psio::encode(psio::pssz8{},Old{"a"})) return 4;
 const std::vector<char> unpruned8{2,2,3,'a',0};           // New{"a",None} with None stored
 if(psio::validate<New>(psio::pssz8{},unpruned8).ok()) return 5;
 if(psio::validate_strict<New>(psio::pssz8{},unpruned8).ok()) return 6;
 New ignored;
 if(psio::decode_pssz_checked(psio::pssz8{},unpruned8,ignored,unknown).ok()) return 7;
 const std::vector<char> unpruned_auto{8,0,0,0,8,0,0,0,9,0,0,0,'a',0};
 if(psio::validate<New>(psio::pssz{},unpruned_auto).ok()) return 8;
 if(psio::validate_strict<New>(psio::pssz{},unpruned_auto).ok()) return 9;
 const auto automatic=psio::encode(psio::pssz{},New{"a","b"});
 if(!psio::decode_pssz_checked(psio::pssz{},automatic,old,unknown).ok() || !unknown || old.name!="a") return 10;
 if(psio::encode(psio::pssz{},New{"a",std::nullopt})!=psio::encode(psio::pssz{},Old{"a"})) return 11;
 const std::vector<char> oversized_header{9,1,'a'};          // header beyond the frame
 if(psio::validate<Old>(psio::pssz8{},oversized_header).ok()) return 12;
 return 0;
}

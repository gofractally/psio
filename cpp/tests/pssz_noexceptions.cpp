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
 return exact.ok()?0:3;
}

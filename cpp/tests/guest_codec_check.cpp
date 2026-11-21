// Standalone native and -fno-exceptions WASI preparation consumer.
#include <psio/frac.hpp>
#include <psio/pssz.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>

struct GuestPlain { std::uint32_t id; std::string label; };
PSIO_REFLECT(GuestPlain, id, label)
struct GuestUtf8 { std::string label; };
PSIO_REFLECT(GuestUtf8, label)
PSIO_FIELD_ATTRS(GuestUtf8, label, psio::utf8_spec{})
struct GuestBound { std::string label; };
PSIO_REFLECT(GuestBound, label)
PSIO_FIELD_ATTRS(GuestBound, label, psio::length_bound{.max=4})
struct GuestUnknownSpec { constexpr bool operator==(const GuestUnknownSpec&) const = default; };
struct GuestUnknown { std::uint32_t id; };
PSIO_REFLECT(GuestUnknown, id)
PSIO_FIELD_ATTRS(GuestUnknown, id, GuestUnknownSpec{})
struct GuestNode { std::uint32_t id; std::vector<GuestNode> children; };
PSIO_REFLECT(GuestNode, id, children)
struct GuestConstructed { std::uint32_t id; GuestConstructed() : id(0) {} };
PSIO_REFLECT(GuestConstructed, id)
struct GuestGuarded { std::uint32_t id; };
PSIO_REFLECT(GuestGuarded, id)
struct GuestGuardAdapter {
    static std::size_t packsize(const GuestGuarded&) { return 4; }
    template<class Sink> static void encode(const GuestGuarded& x, Sink& sink) {
        psio::encode(psio::frac32{},x.id,sink);
    }
    static GuestGuarded decode(std::span<const char> bytes) {
#if defined(__cpp_exceptions)
        return {psio::decode<std::uint32_t>(psio::frac32{},bytes)};
#else
        std::abort(); // A guest strict validator must never reach this.
#endif
    }
    static psio::codec_status validate(std::span<const char> bytes) {
        return psio::validate<std::uint32_t>(psio::frac32{},bytes);
    }
    static psio::codec_status validate_strict(std::span<const char> bytes) { return validate(bytes); }
};
PSIO_ADAPTER(GuestGuarded, psio::binary_category, GuestGuardAdapter)
struct GuestNestedGuard { GuestGuarded value; };
PSIO_REFLECT(GuestNestedGuard, value)
struct GuestCapped { std::string label; };
PSIO_REFLECT(GuestCapped, label)
PSIO_TYPE_ATTRS(GuestCapped, psio::max_dynamic_data_spec{.value=16})

using psio::detail::vstrict::can_validate_without_materialization;
static_assert(can_validate_without_materialization<GuestPlain>());
static_assert(can_validate_without_materialization<GuestNode>());
static_assert(can_validate_without_materialization<std::array<GuestPlain,2>>());
static_assert(can_validate_without_materialization<std::variant<GuestPlain,std::optional<GuestNode>>>());
static_assert(!can_validate_without_materialization<GuestUtf8>());
static_assert(!can_validate_without_materialization<GuestBound>());
static_assert(!can_validate_without_materialization<GuestUnknown>());
static_assert(!can_validate_without_materialization<GuestConstructed>());
static_assert(!can_validate_without_materialization<GuestGuarded>());
static_assert(!can_validate_without_materialization<GuestNestedGuard>());
static_assert(!can_validate_without_materialization<std::vector<GuestUtf8>>());

template<class T, class Fmt> bool unsupported_in_guest(Fmt fmt, const T& value) {
    const auto bytes=psio::encode(fmt,value);
    auto status=psio::validate_strict<T>(fmt,bytes);
#if defined(__cpp_exceptions)
    return status.ok(); // Existing native semantic behavior is retained.
#else
    return !status.ok() && status.error().what.find("unsupported")!=std::string_view::npos;
#endif
}
template<class Fmt> bool check(Fmt fmt, const char* name) {
    const auto fail=[name](int line) { std::fprintf(stderr,"%s failed at line %d\n",name,line);return false; };
    const GuestPlain value{7,"hi"};
    const auto bytes=psio::encode(fmt,value);
    if (!psio::validate<GuestPlain>(fmt,bytes).ok() ||
        !psio::validate_strict<GuestPlain>(fmt,bytes).ok()) return fail(__LINE__);
    const auto decoded=psio::decode<GuestPlain>(fmt,bytes);
    if (decoded.id!=7 || decoded.label!="hi") return fail(__LINE__);
    auto bad=bytes;std::fill(bad.begin()+4,bad.end(),static_cast<char>(0xff));
    if (psio::validate<GuestPlain>(fmt,bad).ok() || psio::validate_strict<GuestPlain>(fmt,bad).ok()) return fail(__LINE__);
    const auto utf8=psio::encode(fmt,GuestUtf8{"hi"});
    auto invalid_utf8=psio::encode(fmt,GuestUtf8{std::string(1,static_cast<char>(0xff))});
    if (!psio::validate<GuestUtf8>(fmt,invalid_utf8).ok()) return fail(__LINE__);
    if (psio::validate_strict<GuestUtf8>(fmt,invalid_utf8).ok()) return fail(__LINE__);
    auto truncated=utf8;truncated.resize(1);
    auto status=psio::validate_strict<GuestUtf8>(fmt,truncated);
    if (status.ok() || status.error().what.find("unsupported")!=std::string_view::npos) return fail(__LINE__);
    if (!unsupported_in_guest(fmt,GuestUtf8{"hi"}) || !unsupported_in_guest(fmt,GuestBound{"hi"}) ||
        !unsupported_in_guest(fmt,GuestUnknown{7}) || !unsupported_in_guest(fmt,GuestGuarded{9}) ||
        !unsupported_in_guest(fmt,GuestConstructed{}) ||
        !unsupported_in_guest(fmt,GuestNestedGuard{{9}})) return fail(__LINE__);
#if !defined(__cpp_exceptions)
    if (!unsupported_in_guest(fmt,GuestBound{std::string(8,'x')})) return fail(__LINE__);
#endif
    std::printf("%s=",name);for(unsigned char c:bytes)std::printf("%02x",c);std::puts("");
    return true;
}
int main(int argc, char** argv) {
#if defined(__cpp_exceptions)
    try {
#endif
    if(argc>1) {
        switch(argv[1][0]) {
            case '1': { psio::bounded<int,1> x;x.push_back(1);x.push_back(2);break; }
            case '2': { psio::utf8_string<1> x(std::string("xx"));(void)x;break; }
            case '3': { psio::bitlist<1> x;x.push_back(true);x.push_back(false);break; }
            case '4': { (void)psio::encode(psio::pssz{},GuestCapped{std::string(32,'x')});break; }
            case '5': { const char bad[]={static_cast<char>(0xff)};
                psio::validate_or_throw<GuestPlain>(psio::pssz{},bad);break; }
            default:return 2;
        }
        return 3;
    }
    if(!check(psio::frac32{},"frac32") || !check(psio::pssz_<1>{},"pssz8") ||
       !check(psio::pssz_<2>{},"pssz16") || !check(psio::pssz_<4>{},"pssz32") ||
       !check(psio::pssz{},"pssz-auto")) return 1;
    std::puts("guest-codec-preparation=PASS");
#if defined(__cpp_exceptions)
    } catch(const std::length_error&) { if(argc>1)return 0;throw;
    } catch(const psio::codec_exception&) { if(argc>1)return 0;throw; }
#endif
}

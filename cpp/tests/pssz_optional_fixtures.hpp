#pragma once
#include <psio/pssz_view.hpp>
#include <psio/schema_ir.hpp>

namespace optional_fixture {
struct Base { std::string name; };
PSIO_REFLECT(Base, name)
struct Extended { std::string name; std::optional<std::string> extra; };
PSIO_REFLECT(Extended, name, extra)
struct Twice { std::string name; std::optional<std::uint32_t> middle; std::optional<std::string> last; };
PSIO_REFLECT(Twice, name, middle, last)
struct OddBase { bool flag; std::string name; };
PSIO_REFLECT(OddBase, flag, name)
struct OddExtended { bool flag; std::string name; std::optional<std::string> extra; };
PSIO_REFLECT(OddExtended, flag, name, extra)
struct NewOuter { Extended inner; std::vector<Extended> values; };
PSIO_REFLECT(NewOuter, inner, values)
struct OldOuter { Base inner; std::vector<Base> values; };
PSIO_REFLECT(OldOuter, inner, values)
struct Unit {};
PSIO_REFLECT(Unit)
struct DepthNode { std::uint8_t value; std::optional<psio::schema_types::Box<DepthNode>> next; };
PSIO_REFLECT(DepthNode, value, next)
inline DepthNode node(std::size_t links) {
 DepthNode out{7, std::nullopt};
 for (std::size_t i=0;i<links;++i) out=DepthNode{1, psio::schema_types::Box<DepthNode>{std::move(out)}};
 return out;
}
// An independent byte builder deliberately creates an invalid-depth packet.
// Production checked encoders must reject the corresponding value.
template<std::size_t W> std::vector<char> invalid_depth_bytes(std::size_t links) {
 auto put=[](std::vector<char>& bytes,std::size_t at,std::size_t value) {
  for(std::size_t i=0;i<W;++i) bytes[at+i]=char((value>>(i*8))&255);
 };
 std::vector<char> bytes(W+1,0);put(bytes,0,1);bytes[W]=7;
 for(std::size_t i=0;i<links;++i) {
  std::vector<char> parent(W+1+W,0);put(parent,0,1+W);parent[W]=1;put(parent,W+1,1+W);
  parent.insert(parent.end(),bytes.begin(),bytes.end());bytes=std::move(parent);
 }
 return bytes;
}
}

PSIO_TYPE_ATTRS(optional_fixture::Unit, psio::definition_will_not_change{})

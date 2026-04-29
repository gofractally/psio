#pragma once
//
// libraries/psio/cpp/include/psio/varint/varint.hpp — umbrella header.
//
// Header-only library of variable-length integer codecs used across the
// codebase. Three algorithms live alongside each other so call sites
// can pick the one appropriate to their wire format:
//
//   varint::leb128   — standard LEB128 (uleb / sleb / zigzag).
//                      Used by:
//                        - psio bin codec (varuint32 prefixes)
//                        - psio avro codec (zig-zag long)
//                        - psizam wasm parser (full uleb/sleb family)
//
//   varint::prefix3  — 3-bit length prefix in the top of byte 0,
//                      payload in the remaining 5 bits + trailing
//                      bytes. Codes 0–6 ⇒ 1–7 bytes, code 7 ⇒ 9 bytes
//                      (escape for full u64). See prefix3.hpp.
//
//   varint::prefix2  — RFC 9000 / QUIC 2-bit length prefix; lengths
//                      1/2/4/8 bytes, payloads 6/14/30/62 bits, big
//                      endian on the wire (canonical). A `_le` sibling
//                      with the same layout but little-endian payloads
//                      is provided for direct head-to-head benchmarking
//                      against the BE form.
//
// Each algorithm exposes a `scalar::` namespace (portable C++; always
// available) and a `fast::` namespace (SIMD / bit-manipulation; falls
// back to scalar where the target lacks the required features).
//
// The top-level `varint::<algo>::` namespace re-exports the fast path,
// so callers that don't care about choosing get the best implementation
// for the build target automatically.
//

#include <psio/varint/result.hpp>

#include <psio/varint/leb128.hpp>
#include <psio/varint/prefix2.hpp>
#include <psio/varint/prefix3.hpp>

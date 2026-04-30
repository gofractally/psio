// psio — phase 0 placeholder translation unit.
//
// CMake requires a library target to have at least one compiled source
// file, and the resulting static archive must contain at least one
// non-inline symbol to link cleanly on macOS. psio is header-only
// through phase 13; real compiled units (schema, from_json, chrono,
// emit_wit) arrive with phase 14's dynamic schema + reflection
// machinery.
//
// When phase 14 adds its first real .cpp, this file can be removed.

namespace psio1::detail {

// Non-inline symbol so the resulting archive has at least one entry
// that makes it a well-formed Mach-O archive on macOS.
const char* build_sentinel() noexcept {
   return "psio phase 0 scaffold";
}

}  // namespace psio1::detail

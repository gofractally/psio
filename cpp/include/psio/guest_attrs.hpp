#pragma once

// psio/guest_attrs.hpp — Clang attribute macros for wiring a
// WebAssembly guest's imports and exports directly onto the static
// member declarations in a shared interface header. Split out from
// psio/structural.hpp so a guest translation unit can include it
// without dragging in reflection's stdlib dependencies (`<map>`,
// `<variant>`, `<vector>`, …).
//
// On the guest the macros apply clang's `import_module` / `import_name`
// / `export_name` attributes so the compiler emits a direct WASM
// import/export binding — no hand-written trampoline functions
// required. On the host both macros expand to nothing.

#ifdef __wasm__
#define PSIO_IMPORT(MOD, NAME) \
   [[clang::import_module(#MOD), clang::import_name(#NAME)]]
#define PSIO_EXPORT(NAME)      [[clang::export_name(#NAME)]]
#else
#define PSIO_IMPORT(MOD, NAME)
#define PSIO_EXPORT(NAME)
#endif

#pragma once
//
// psio/compat/psio_aliases.hpp — migration-aid header that re-exports
// psio symbols under the historical `psio1::` namespace. Intended for
// consumers (psizam, pfs, application code) that currently call
// `psio1::encode(…)` / `psio1::frac::…` / `psio1::reflect<T>` and want to
// validate their code against psio before the swap lands in Phase 18.
//
// **Status:** transitional. This header is NOT part of the final v2
// API — after Phase 18 renames `libraries/psio/` → `libraries/psio1/`
// and rewrites `psio::` → `psio1::` across the tree, this header
// disappears along with the `psio::` namespace itself.
//
// **Scope of aliases:** just enough that a consumer's existing code
// compiles. Format tags, the top-level CPO objects, reflect<T>, schema
// helpers, and the error model are all pulled in. Anything not
// listed here still needs an explicit `::psio::` qualifier in the
// consumer's code — a deliberate rough edge so the consumer knows
// the migration surface isn't complete.
//
// **Enabling:** this header is NOT included by the master psio.hpp.
// Consumers include it explicitly when they want the bridge:
//
//   #include <psio/compat/psio_aliases.hpp>
//
// Once included, `psio1::encode(psio1::frac32{}, value, sink)` calls the
// psio encoder. Mixed usage (some `psio1::`, some `psio::`) also
// works — both refer to the same underlying entities.

#include <psio/psio.hpp>

// Top-level namespace alias — makes `psio1::` resolve to `psio::` for
// all uses. Nested symbols, templates, traits, concepts, and macros
// all flow through automatically.
namespace psio1 = ::psio;

// Macros: C++ namespaces don't catch macros, so re-export the common
// ones under the old spelling so consumers' existing macro callsites
// continue to work. If the old macro name already exists (e.g. from a
// transitional build where both psio v1 and psio are linked), the
// guard skips the redefinition.

#ifndef PSIO1_REFLECT
   #define PSIO1_REFLECT(...)     PSIO_REFLECT(__VA_ARGS__)
#endif

#ifndef PSIO1_ATTRS
   // Symbolic placeholder; PSIO_ATTRS is still deferred (see
   // annotate.hpp). Consumers using PSIO1_ATTRS get a compile error
   // referring to the plan until the macro lands.
   #define PSIO1_ATTRS(...)                                            \
      static_assert(false,                                            \
                    "PSIO1_ATTRS → PSIO_ATTRS not yet implemented — " \
                    "see .issues/psio-v2-design.md § 5.3.5")
#endif


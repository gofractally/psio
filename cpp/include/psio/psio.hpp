#pragma once
//
// psio — v2-architecture serialization library.
//
// This is the master include. Pulls in every public header that psio
// provides. Header contents grow as the 18-phase implementation plan
// proceeds; see .issues/psio-v2-design.md for the full architecture
// spec and /Users/dlarimer/.claude/plans/idempotent-beaming-milner.md
// for the phase-by-phase plan.
//
// During development psio lives alongside psio (v1). Once every phase's
// parity gate passes, psio replaces psio via a rename + namespace
// rewrite — at which point the `psio::` identifier goes away and this
// file is replaced by psio/psio.hpp.
//
// Phase 0 scaffold: header is intentionally empty beyond this note.
// Phase 1 adds reflect.hpp. Phase 2 adds shapes.hpp, annotate.hpp,
// wrappers.hpp. Etc.

#include <psio/annotate.hpp>
#include <psio/bin.hpp>
#include <psio/buffer.hpp>
#include <psio/cpo.hpp>
#include <psio/dynamic_json.hpp>
#include <psio/dynamic_value.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/frac.hpp>
#include <psio/json.hpp>
#include <psio/mutable_view.hpp>
#include <psio/adapter.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/schema.hpp>
#include <psio/shapes.hpp>
#include <psio/ssz.hpp>
#include <psio/storage.hpp>
#include <psio/transcode.hpp>
#include <psio/view.hpp>
#include <psio/wrappers.hpp>

namespace psio {}

//! PSIO — Multi-format serialization library.
//!
//! Supports fracpack, Cap'n Proto, FlatBuffers, WIT Canonical ABI, JSON,
//! and more from a single set of derive macros.
//!
//! # Fracpack (default format)
//!
//! ```
//! use psio1::{Pack, Unpack, Result};
//!
//! #[derive(Pack, Unpack, PartialEq, Debug)]
//! #[fracpack(fracpack_mod = "psio")]
//! struct Example {
//!     a_string: String,
//!     a_tuple: (u32, String),
//! }
//!
//! let orig = Example {
//!     a_string: "content".into(),
//!     a_tuple: (1234, "5678".into()),
//! };
//!
//! let packed: Vec<u8> = orig.packed();
//! let unpacked = Example::unpacked(&packed)?;
//! assert_eq!(orig, unpacked);
//! # Ok::<(), psio1::Error>(())
//! ```

// Core fracpack format (re-exported at top level for convenience)
#[path = "fracpack.rs"]
mod fracpack_impl;

pub use fracpack_impl::*;

// Validation policy — type machinery for the format-tagged
// `validate<F, T, P>` CPO described in `docs/psio-overview.md` §2.4.
mod policy;
pub use policy::{ValidationPolicy, DefaultSafe, StrictCanonical,
                  JustDontCrash, DynamicPolicy};

// Format trait + crate-root CPOs (encode<F, T> / decode<F, T> /
// validate<F, T, P>). Per-format modules (`pjson`, `pssz`, …) provide
// a `Format` impl and per-type `Encode<F>` / `Decode<F>` / `Validate<F>`
// blanket impls.
mod format;
pub use format::{Format, Encode, Decode, Validate, encode, decode, validate};

// New modules (populated in subsequent phases)
pub mod xxh64;
pub mod xxh3_64;
pub mod dynamic_schema;

// Multi-format wire formats
pub mod capnp;
pub mod flatbuf;
pub mod pjson;
#[macro_use]
pub mod pjson_derive;
pub mod pjson_view;
pub mod pjson_typed;
pub mod pjson_json;
pub mod pssz;
#[macro_use]
pub mod pssz_derive;
pub mod pssz_view;
pub mod ssz;
#[macro_use]
pub mod ssz_derive;
pub mod ssz_view;
pub mod wit;

// Cross-validation tests against C++ encoder fixtures
#[cfg(test)]
mod cross_validation_tests;

// pjson cross-validation: round-trip self-checks + #[ignore]-marked
// byte-identity comparisons against C++ output (placeholders until
// the sibling pjson-impl-conformance branch lands).
#[cfg(test)]
mod pjson_cross_validation_tests;

// Ethereum Phase-0 BeaconState types (Rust port of beacon_types.hpp)
pub mod beacon_types;

// BeaconState Phase-0 Validator cross-val (Phase J of parity plan)
#[cfg(test)]
mod beacon_state_tests;

// Dynamic dispatch (runtime field access by name)
pub mod dynamic_view;

// Schema tooling
pub mod schema_export;
pub mod schema_import;

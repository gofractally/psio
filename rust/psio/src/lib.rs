//! PSIO — Multi-format serialization library.
//!
//! Supports pSSZ, pjson, Cap'n Proto, FlatBuffers, WIT Canonical ABI, JSON,
//! and more from a single set of derive macros.
//!
//! pSSZ is the primary schema-driven binary format and the successor to
//! fracpack. Fracpack is a legacy prototype retained for benchmark comparisons
//! of different data-layout designs. Its `Pack` / `Unpack` API remains
//! re-exported at the crate root.
//!
//! # Legacy fracpack benchmark API
//!
//! ```
//! use psio::{Pack, Unpack, Result};
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
//! # Ok::<(), psio::Error>(())
//! ```

// Legacy fracpack benchmark API (re-exported at the crate root)
#[path = "fracpack.rs"]
mod fracpack_impl;

pub use fracpack_impl::*;
pub use psio_macros::{PsszPack, PsszUnpack, PsszView, ToPsszSchema};
pub use psio_macros::JsonText;
pub use psio_macros::Key;

// The helper names, one set per format (docs/helpers.md): `to_F`,
// `try_to_F`, `to_F_into`, `from_F`, `from_F_trusted`, `validate_F`,
// `F_size`, and the view / buffer types.
pub use capnp::{capnp_size, from_capnp, from_capnp_trusted, to_capnp, to_capnp_into, validate_capnp};
pub use flatbuf::{
    flatbuf_size, from_flatbuf, from_flatbuf_trusted, to_flatbuf, to_flatbuf_into, validate_flatbuf,
};
pub use wit::{from_wit, from_wit_trusted, to_wit, to_wit_into, validate_wit, wit_size};
pub use json::{
    from_ijson, from_ijson_trusted, from_json, from_json_trusted, ijson_size, json_size, to_ijson,
    to_ijson_into, to_json, to_json_into, try_to_ijson, try_to_json, validate_ijson, validate_json,
    IJson, Json,
};
pub use key::{from_key, from_key_trusted, key_size, to_key, to_key_into, try_to_key, validate_key};
pub use pssz::{
    from_pssz, from_pssz_trusted, pssz_size, to_pssz, to_pssz_into, try_to_pssz, validate_pssz,
    Pssz, PsszBuffer, SharedPssz,
};
pub use pssz_view::{PsszMapEntry, PsszMutMapEntry, PsszMutView, PsszView};
pub use ssz::{
    from_ssz, from_ssz_trusted, ssz_size, to_ssz, to_ssz_into, validate_ssz, SharedSsz, Ssz,
    SszBuffer,
};
pub use ssz_view::SszView;

mod buffer;
pub use buffer::{Buffer, MutViewFormat, Shared, ViewFormat};

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
pub use format::{Format, Encode, Decode, Validate, encode, decode, validate, size_of};

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
pub mod json;
pub mod key;
pub mod time;
pub mod pssz;
#[doc(hidden)]
pub mod pssz_checked;
#[doc(hidden)]
pub mod pssz_bits;
pub mod pssz_schema_ir;
pub mod pssz_dynamic;
pub mod pssz_upgrade;
pub mod pssz_evolves;
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

// Macro dependencies are resolved through this crate for downstream consumers.
#[doc(hidden)]
pub use paste as __paste;

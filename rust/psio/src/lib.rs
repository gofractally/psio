//! PSIO: annotate a type once, use it with every format.
//!
//! One set of derives and one attribute, `#[psio(...)]`, describe a type;
//! each format reads that description. The formats:
//!
//! * [`pssz`]: pSSZ, the primary schema-driven binary format, with views
//!   ([`struct@PsszView`], [`PsszMutView`]), a Schema IR ([`pssz_schema_ir`]) and
//!   schema-driven dynamic access ([`pssz_dynamic`]);
//! * [`json`]: Json and IJson text, and through it serde
//!   ([`serde_bridge`]): `#[derive(psio::Serde)]` gives a type
//!   `Serialize` / `Deserialize` with PSIO's text;
//! * [`key`]: memcmp-sortable keys;
//! * [`interface`](mod@interface): methods and interfaces, `#[psio::interface]` on a trait
//!   (argument records, dispatch, call proxies, the schema's `Interface`);
//! * [`compress_name`]: method and identifier names as 64-bit numbers;
//! * [`time`]: durations and time points;
//! * with a cargo feature each: [`pjson`] (`pjson`), a self-describing
//!   binary JSON; [`ssz`] (`ssz`; the beacon-chain records with `beacon`);
//!   [`capnp`] (`capnp`), [`flatbuf`] (`flatbuf`) and [`wit`] (`wit`), for
//!   interchange with Cap'n Proto, FlatBuffers and the WIT Canonical ABI;
//!   [`dynamic_view`] (`dynamic-view`). `all-formats` is all of them.
//!
//! # Cargo features
//!
//! With no feature a dependent compiles pSSZ, keys, Json / IJson text and
//! the serde bridge, the Schema IR, time, names and interfaces, and nothing
//! of the other formats.
//!
//! | feature | adds |
//! |---------|------|
//! | `ssz`, `beacon` | [`ssz`], `ssz_struct!`, [`struct@SszView`]; [`beacon_types`] |
//! | `pjson` | [`pjson`], `pjson_struct!` |
//! | `capnp`, `flatbuf`, `wit` | the module and the `Capnp*` / `Fb*` / `Wit*` derives |
//! | `dynamic-view` | [`dynamic_view`], [`dynamic_schema`], schema import and export (implies the three above) |
//! | `all-formats` | every format above |
//! | `chrono` | `TryFrom` between [`time`] values and `chrono` |
//! | `async-graphql` | `psio::graphql_scalars`: PSIO values as `async-graphql` scalars |
//! | `experimental-frac` | [`experimental`] |
//!
//! No feature changes what `serde_json` does for the rest of a program
//! ([`serde_bridge`]).
//!
//! # Helpers
//!
//! Every format `F` has the same helper names at the crate root: `to_F`,
//! `try_to_F`, `to_F_into`, `from_F`, `from_F_trusted`, `validate_F` and
//! `F_size`. The tag-generic layer is [`encode`], [`decode`], [`validate`]
//! and [`size_of`] over a [`Format`] tag, and [`Buffer`] / [`Shared`] own
//! validated bytes.
//!
//! ```
//! use psio::{JsonText, Key, PsszPack, PsszUnpack, PsszView};
//!
//! #[derive(Debug, PartialEq, PsszPack, PsszUnpack, PsszView, JsonText, Key)]
//! struct Account {
//!     id: u32,
//!     name: String,
//! }
//!
//! let account = Account { id: 7, name: "alice".into() };
//!
//! // pSSZ: encode, validate, decode, or read a member in place.
//! let bytes = psio::to_pssz(&account);
//! psio::validate_pssz::<Account>(&bytes)?;
//! assert_eq!(psio::from_pssz::<Account>(&bytes)?, account);
//! assert_eq!(psio::PsszView::<Account>::new(&bytes)?.id().get(), 7);
//!
//! // The same type as JSON text and as a sortable key.
//! let text = psio::to_json(&account);
//! assert_eq!(text, r#"{"id":7,"name":"alice"}"#);
//! assert_eq!(psio::from_json::<Account>(&text)?, account);
//! let key = psio::to_key(&account);
//! assert_eq!(psio::from_key::<Account>(&key)?, account);
//! # Ok::<(), Box<dyn std::error::Error>>(())
//! ```
//!
//! # Annotations
//!
//! `#[psio(...)]` describes the type and is read by every derive. A
//! format-named attribute holds only what is specific to that format.
//!
//! | attribute | key | on | meaning |
//! |-----------|-----|----|---------|
//! | `psio` | `crate = "path"` | type | path of the psio crate (default `::psio`) |
//! | `psio` | `transparent` | type | the type is its one member in every format |
//! | `psio` | `invariant = "path"` | type | a `fn(&Self) -> Result<(), E>` with `E: Into<Cow<'static, str>>` (a `&'static str`, or a `String` computed from the value) every reader and writer enforces |
//! | `psio` | `definition_will_not_change` | type | the record never gains members |
//! | `psio` | `variant` | type | a field-less enum is a variant of unit alternatives, not its integer |
//! | `psio` | `custom`, `custom = "id"` | type | the type has a text form of its own; the id defaults to the schema name |
//! | `psio` | `name = "..."` | type, variant, field | the type's name in a schema; an enumerator's, alternative's or member's name in a schema and in text |
//! | `psio` | `rename_all = "..."` | type | a case convention (`"camelCase"`, `"snake_case"`, ...) for the member names of a struct or the variant names of an enum |
//! | `psio` | `skip` | field | the field is not a member of the type in any format; a reader fills it with its default |
//! | `psio` | `max = N`, `element(...)`, `keys(...)`, `values(...)`, `at(I, ...)` | field | bounds of the member and of the positions below it ([`pssz::bounds`]): the member is the bounded type in every format |
//! | `psio` | `other` | variant | the catch-all of a field-less `#[repr(int)]` enum |
//! | `pssz` | `max_dynamic_data = N` | type | pSSZ cap on the record's variable part |
//! | `json` | `json_number`, `json_string`, `ijson_number`, `ijson_string` | type, field | integer form in Json / IJson text |
//!
//! The derives also read `#[serde(...)]`: `transparent`, `rename`,
//! `rename_all` and `skip` mean what the `psio` keys above mean, an attribute
//! PSIO's text cannot honour is a compile error on the derives that describe
//! text, and the rest is ignored. [`serde_attrs`] has the rule for every
//! serde attribute.
//!
//! ```
//! use psio::{JsonText, PsszPack, PsszUnpack};
//!
//! #[derive(PsszPack, PsszUnpack, JsonText)]
//! #[psio(definition_will_not_change)]
//! #[pssz(max_dynamic_data = 64)]
//! struct Balance {
//!     #[json(json_string)]
//!     amount: u64,
//!     memo: String,
//! }
//! ```
//!
//! Each key has one home. A key under another attribute is an error that
//! names the right one:
//!
//! ```compile_fail
//! use psio::{PsszPack, PsszUnpack};
//!
//! #[derive(PsszPack, PsszUnpack)]
//! #[pssz(definition_will_not_change)] // write #[psio(definition_will_not_change)]
//! struct Point { x: u32, y: u32 }
//! ```
//!
//! ```compile_fail
//! use psio::JsonText;
//!
//! #[derive(JsonText)]
//! #[psio(json_string)] // write #[json(json_string)]
//! struct Amount { value: u64 }
//! ```
//!
//! An unknown key is an error:
//!
//! ```compile_fail
//! use psio::{PsszPack, PsszUnpack};
//!
//! #[derive(PsszPack, PsszUnpack)]
//! #[psio(frozen)]
//! struct Point { x: u32, y: u32 }
//! ```
//!
//! So is a key on something it does not apply to:
//!
//! ```compile_fail
//! use psio::{PsszPack, PsszUnpack};
//!
//! #[derive(PsszPack, PsszUnpack)]
//! struct Point {
//!     #[psio(transparent)]
//!     x: u32,
//!     y: u32,
//! }
//! ```
//!
//! The same types with the keys in place compile:
//!
//! ```
//! use psio::{JsonText, PsszPack, PsszUnpack};
//!
//! #[derive(PsszPack, PsszUnpack)]
//! #[psio(definition_will_not_change)]
//! struct Point { x: u32, y: u32 }
//!
//! #[derive(JsonText)]
//! #[json(json_string)]
//! struct Amount { value: u64 }
//! ```
//!
//! # Experimental formats
//!
//! [`experimental`] holds formats kept to measure design trade-offs. They are
//! not supported wire formats, have no helper names, and are compiled only
//! with their cargo feature.

pub mod experimental;
pub mod serde_attrs;
pub mod serde_bridge;

// The derives. A derive and the trait it implements share a name; the trait
// lives in its format's module.
#[cfg(feature = "capnp")]
pub use psio_macros::{CapnpPack, CapnpUnpack, CapnpView};
#[cfg(feature = "flatbuf")]
pub use psio_macros::{FbPack, FbUnpack, FbView};
#[cfg(feature = "dynamic-view")]
pub use psio_macros::ToDynamicSchema;
#[cfg(feature = "wit")]
pub use psio_macros::{WitPack, WitUnpack, WitView};
pub use psio_macros::{PsszPack, PsszUnpack, PsszView, ToPsszSchema};
pub use psio_macros::JsonText;
pub use psio_macros::Serde;
pub use psio_macros::Key;
// The attribute `#[psio::interface]`; the module of the same name has the
// traits it implements.
pub use psio_macros::interface;

// The helper names, one set per format (docs/helpers.md): `to_F`,
// `try_to_F`, `to_F_into`, `from_F`, `from_F_trusted`, `validate_F`,
// `F_size`, and the view / buffer types.
#[cfg(feature = "capnp")]
pub use capnp::{capnp_size, from_capnp, from_capnp_trusted, to_capnp, to_capnp_into, validate_capnp};
#[cfg(feature = "flatbuf")]
pub use flatbuf::{
    flatbuf_size, from_flatbuf, from_flatbuf_trusted, to_flatbuf, to_flatbuf_into, validate_flatbuf,
};
#[cfg(feature = "wit")]
pub use wit::{from_wit, from_wit_trusted, to_wit, to_wit_into, validate_wit, wit_size};
pub use json::{
    from_ijson, from_ijson_trusted, from_json, from_json_trusted, ijson_size, json_size, to_ijson,
    to_ijson_into, to_json, to_json_into, try_to_ijson, try_to_json, validate_ijson, validate_json,
    IJson, Json, JsonNumber, JsonValue,
};
pub use key::{from_key, from_key_trusted, key_size, to_key, to_key_into, try_to_key, validate_key};
pub use pssz::{
    from_pssz, from_pssz_trusted, pssz_size, to_pssz, to_pssz_into, try_to_pssz, validate_pssz,
    Pssz, PsszBuffer, SharedPssz,
};
pub use pssz_view::{PsszMapEntry, PsszMutMapEntry, PsszMutView, PsszView};
#[cfg(feature = "ssz")]
pub use ssz::{
    from_ssz, from_ssz_trusted, ssz_size, to_ssz, to_ssz_into, validate_ssz, SharedSsz, Ssz,
    SszBuffer,
};
#[cfg(feature = "ssz")]
pub use ssz_view::SszView;

// Every derive resolves the psio crate through #[psio(crate = "...")].
#[cfg(all(test, feature = "capnp", feature = "flatbuf", feature = "wit", feature = "dynamic-view"))]
mod annotation_tests;

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

#[cfg(feature = "dynamic-view")]
pub mod xxh64;
#[cfg(feature = "pjson")]
pub mod xxh3_64;
#[cfg(feature = "dynamic-view")]
pub mod dynamic_schema;

// The formats a dependent asks for by cargo feature.
#[cfg(feature = "capnp")]
pub mod capnp;
#[cfg(feature = "flatbuf")]
pub mod flatbuf;
#[cfg(feature = "pjson")]
pub mod pjson;
#[cfg(feature = "pjson")]
#[macro_use]
pub mod pjson_derive;
#[cfg(feature = "pjson")]
pub mod pjson_view;
#[cfg(feature = "pjson")]
pub mod pjson_typed;
#[cfg(feature = "pjson")]
pub mod pjson_json;
// Always compiled: pSSZ, keys, Json / IJson text, the Schema IR, time,
// names, interfaces.
mod value_types;
pub mod json;
#[cfg(feature = "async-graphql")]
pub mod graphql_scalars;
pub mod key;
pub mod compress_name;
pub mod interface;
pub mod time;
pub mod pssz;
#[doc(hidden)]
pub mod pssz_checked;
#[doc(hidden)]
pub mod pssz_bits;
pub mod pssz_bounds;
pub mod pssz_schema_ir;
pub mod pssz_dynamic;
pub mod pssz_upgrade;
pub mod pssz_evolves;
pub mod pssz_view;
#[cfg(feature = "ssz")]
pub mod ssz;
#[cfg(feature = "ssz")]
#[macro_use]
pub mod ssz_derive;
#[cfg(feature = "ssz")]
pub mod ssz_view;
#[cfg(feature = "wit")]
pub mod wit;

// pjson cross-validation: round-trip self-checks + #[ignore]-marked
// byte-identity comparisons against C++ output (placeholders until
// the sibling pjson-impl-conformance branch lands).
#[cfg(all(test, feature = "pjson"))]
mod pjson_cross_validation_tests;

// Ethereum Phase-0 BeaconState types (Rust port of beacon_types.hpp)
#[cfg(feature = "beacon")]
pub mod beacon_types;

// BeaconState Phase-0 Validator cross-val (Phase J of parity plan)
#[cfg(all(test, feature = "beacon"))]
mod beacon_state_tests;

// Dynamic dispatch (runtime field access by name)
#[cfg(feature = "dynamic-view")]
pub mod dynamic_view;

// Schema tooling
#[cfg(feature = "dynamic-view")]
pub mod schema_export;
#[cfg(feature = "dynamic-view")]
pub mod schema_import;

// Macro dependencies are resolved through this crate for downstream consumers.
#[doc(hidden)]
pub use paste as __paste;
#[doc(hidden)]
pub use serde as __serde;

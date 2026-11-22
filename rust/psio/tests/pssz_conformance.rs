//! Cross-language PSSZ conformance: consumes conformance/pssz-vectors.json,
//! the fixture generated from the C++ implementation
//! (cpp/tests/pssz_conformance_emit.cpp) and also checked by ctest
//! (cpp/tests/pssz_conformance_tests.cpp).
//!
//! For every vector Rust must reach the fixture's outcome on every reader
//! entry point: `canonical` vectors decode and re-encode to identical bytes,
//! `unknown` vectors are accepted with unknown fields reported and re-encode
//! to the fixture's projection, `reject` vectors are rejected everywhere.
//! The records are declared with the proc-macro derives.
#![allow(dead_code)]

mod harness {
    include!("conformance/harness.rs");
}
// The transparent wrappers, custom types and invariants of
// conformance/custom-types.md.
#[path = "custom/text.rs"]
#[allow(dead_code)]
mod text;
#[path = "custom/types.rs"]
#[allow(dead_code)]
mod custom_types;

mod records {
    macro_rules! rec {
        (#[cap($cap:literal)] $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
            #[pssz(max_dynamic_data = $cap)]
            pub struct $name { $(pub $f: $t),* }
            conf_record!("record", $cap; $name { $($f: $t),* });
        };
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
            pub struct $name { $(pub $f: $t),* }
            conf_record!("record", ; $name { $($f: $t),* });
        };
    }
    macro_rules! dwnc {
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView, psio::Key)]
            #[psio(definition_will_not_change)]
            pub struct $name { $(pub $f: $t),* }
            conf_record!("dwnc", ; $name { $($f: $t),* });
        };
    }
    include!("conformance/body.rs");
}

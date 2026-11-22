//! Cross-language PSSZ conformance: consumes conformance/pssz-vectors.json,
//! the fixture generated from the C++ implementation
//! (cpp/tests/pssz_conformance_emit.cpp) and also checked by ctest
//! (cpp/tests/pssz_conformance_tests.cpp).
//!
//! For every vector Rust must reach the fixture's outcome on every reader
//! entry point: `canonical` vectors decode and re-encode to identical bytes,
//! `unknown` vectors are accepted with unknown fields reported and re-encode
//! to the fixture's projection, `reject` vectors are rejected everywhere.
//! The whole fixture runs twice: against records declared with the
//! pssz_struct! macros and against the proc-macro derives.
#![allow(dead_code)]

mod harness {
    include!("conformance/harness.rs");
}
// The transparent wrappers, custom types and invariants of
// conformance/custom-types.md: one declaration (the derives), used by both
// record flavours.
#[path = "custom/text.rs"]
mod text;
#[path = "custom/types.rs"]
mod custom_types;

mod macro_rules_records {
    macro_rules! rec {
        ($(#[cap($cap:literal)])? $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq)]
            pub struct $name { $(pub $f: $t),* }
            rec!(@codec $(#[cap($cap)])? $name { $($f: $t),* });
            conf_record!("record", $($cap)?; $name { $($f: $t),* });
        };
        (@codec #[cap($cap:literal)] $name:ident { $($f:ident : $t:ty),* }) => {
            psio::pssz_struct!(#[pssz(max_dynamic_data = $cap)] $name { $($f: $t),* });
        };
        (@codec $name:ident { $($f:ident : $t:ty),* }) => {
            psio::pssz_struct!($name { $($f: $t),* });
        };
    }
    macro_rules! dwnc {
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, Copy, PartialEq, psio::Key)]
            pub struct $name { $(pub $f: $t),* }
            psio::pssz_struct_dwnc!($name { $($f: $t),* });
            conf_record!("dwnc", ; $name { $($f: $t),* });
        };
    }
    include!("conformance/body.rs");
}

mod derived_records {
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
            #[pssz(definition_will_not_change)]
            pub struct $name { $(pub $f: $t),* }
            conf_record!("dwnc", ; $name { $($f: $t),* });
        };
    }
    include!("conformance/body.rs");
}

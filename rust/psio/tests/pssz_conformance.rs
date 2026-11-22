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

// Method argument tuples under a cap (docs/pssz-spec.md 16), declared as
// C++ declares them: `method(transfer, attr(to, utf8 | max<16>), amount,
// attr(memo, utf8 | max<80>), maxDynamicData(256))`. The fixture names the
// argument type by the tuple's shape and the cap.
mod method_args {
    use super::harness::*;

    #[psio::interface(name = "token", derive(Debug, Clone, PartialEq))]
    pub trait Token {
        #[pssz(max_dynamic_data = 256)]
        fn transfer(&mut self, #[psio(max = 16)] to: String, amount: u64, #[psio(max = 80)] memo: String);
        #[pssz(max_dynamic_data = 256)]
        fn transfer_fee(
            &mut self,
            #[psio(max = 16)] to: String,
            amount: u64,
            #[psio(max = 80)] memo: String,
            fee: Option<u32>,
        );
    }
    impl Conf for token::args::transfer {
        fn id() -> String { "tuple<string<=16,u64,string<=80>@cap=256".into() }
    }
    impl Conf for token::args::transfer_fee {
        fn id() -> String { "tuple<string<=16,u64,string<=80,optional<u32>>@cap=256".into() }
    }
    pub fn register(registry: &mut Registry) {
        super::harness::register::<token::args::transfer>(registry);
        super::harness::register::<token::args::transfer_fee>(registry);
    }
}

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

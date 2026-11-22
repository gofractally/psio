//! Rust/C++ PSSZ parity for the owner's rules, run once against records
//! declared with the `pssz_struct!` macros and once against the proc-macro
//! derives. Expected bytes come from the C++ golden tables in `cpp/tests`
//! (read at test time, not copied) wherever C++ pins them.
//!
//! Rules covered (see docs/pssz-rust-profile.md):
//!   1 record evolution through the writer's header, both directions
//!   2 mandatory pruning: encoder prunes, every reader rejects a stored None
//!   3 optional presence (spec 5.5)
//!   4 exact spans / no trailing data
//!   5 type-derived offset width, explicit cap first, per-type ownership
//!   6 String (UTF-8) vs RawString (bytes)
//!   7 C-like enums as integers, data enums as variants
#![allow(dead_code)]

pub mod support {
    use std::collections::HashMap;

    pub fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }
    fn source(name: &str) -> String {
        let path = format!("{}/../../cpp/tests/{name}", env!("CARGO_MANIFEST_DIR"));
        std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
    }
    fn strings(line: &str) -> Vec<String> {
        line.split('"').skip(1).step_by(2).map(str::to_owned).collect()
    }
    /// cpp/tests/pssz_evolution_goldens.hpp: name → [pssz8, pssz16, pssz32].
    pub fn evolution_goldens() -> HashMap<String, [Vec<u8>; 3]> {
        let text = source("pssz_evolution_goldens.hpp");
        let body = &text[text.find("goldens{{").expect("golden table")..];
        let all: Vec<String> = strings(body);
        assert_eq!(all.len() % 4, 0, "name + three columns per entry");
        let map: HashMap<_, _> = all
            .chunks(4)
            .map(|c| (c[0].clone(), [hex(&c[1]), hex(&c[2]), hex(&c[3])]))
            .collect();
        assert_eq!(map.len(), 20);
        map
    }
    /// cpp/tests/pssz_type_width_explicit_goldens.hpp: "name-W" → bytes.
    pub fn width_goldens() -> HashMap<String, Vec<u8>> {
        let text = source("pssz_type_width_explicit_goldens.hpp");
        let body = &text[text.find("width_explicit_goldens{{").expect("golden table")..];
        let all = strings(body);
        let map: HashMap<_, _> = all.chunks(2).map(|c| (c[0].clone(), hex(&c[1]))).collect();
        assert_eq!(map.len(), 24);
        map
    }
    /// Little-endian frame builder for hand-made frames (C++ test `Frame`).
    pub struct Frame { pub w: usize, pub bytes: Vec<u8> }
    impl Frame {
        pub fn new(w: usize) -> Self { Self { w, bytes: vec![] } }
        pub fn o(mut self, v: usize) -> Self {
            self.bytes.extend_from_slice(&(v as u64).to_le_bytes()[..self.w]);
            self
        }
        pub fn u32(mut self, v: u32) -> Self { self.bytes.extend_from_slice(&v.to_le_bytes()); self }
        pub fn raw(mut self, v: &str) -> Self { self.bytes.extend_from_slice(v.as_bytes()); self }
        pub fn byte(mut self, v: u8) -> Self { self.bytes.push(v); self }
    }
}

mod macro_rules_records {
    macro_rules! rec {
        (#[cap($cap:literal)] $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq)]
            pub struct $name { $(pub $f: $t),* }
            psio::pssz_struct!(#[pssz(max_dynamic_data = $cap)] $name { $($f: $t),* });
        };
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq)]
            pub struct $name { $(pub $f: $t),* }
            psio::pssz_struct!($name { $($f: $t),* });
        };
    }
    macro_rules! dwnc {
        (#[cap($cap:literal)] $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, Copy, PartialEq)]
            pub struct $name { $(pub $f: $t),* }
            psio::pssz_struct_dwnc!(#[pssz(max_dynamic_data = $cap)] $name { $($f: $t),* });
        };
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq)]
            pub struct $name { $(pub $f: $t),* }
            psio::pssz_struct_dwnc!($name { $($f: $t),* });
        };
    }
    include!("parity/body.rs");
}

mod derived_records {
    macro_rules! rec {
        (#[cap($cap:literal)] $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
            #[pssz(max_dynamic_data = $cap)]
            pub struct $name { $(pub $f: $t),* }
        };
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
            pub struct $name { $(pub $f: $t),* }
        };
    }
    macro_rules! dwnc {
        (#[cap($cap:literal)] $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, Copy, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
            #[pssz(definition_will_not_change, max_dynamic_data = $cap)]
            pub struct $name { $(pub $f: $t),* }
        };
        ($name:ident { $($f:ident : $t:ty),* $(,)? }) => {
            #[derive(Debug, Clone, PartialEq, psio::PsszPack, psio::PsszUnpack, psio::PsszView)]
            #[pssz(definition_will_not_change)]
            pub struct $name { $(pub $f: $t),* }
        };
    }
    include!("parity/body.rs");
}

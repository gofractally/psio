// The Rust mirror of cpp/tests/custom_conformance_types.hpp: the transparent
// wrappers, custom text forms and invariants of conformance/custom-types.md,
// declared the way a Rust program declares them. Shared by
// pssz_conformance.rs, json_conformance.rs and time_key_conformance.rs.
//
//   #[psio(transparent)]            the type is its one member on the wire,
//                                   in a key and in text
//   #[psio(custom)] + TextForm      a text form of its own (one JSON string)
//   #[psio(invariant = "path")]     a predicate every reader and writer
//                                   enforces, in every format

use super::text::*;
use psio::json::TextForm;
use psio::pssz::RawString;

macro_rules! derives {
    ($(#[$attr:meta])* $vis:vis struct $($rest:tt)*) => {
        #[derive(
            Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, psio::PsszPack, psio::PsszUnpack,
            psio::JsonText, psio::ToPsszSchema, psio::Key,
        )]
        $(#[$attr])*
        $vis struct $($rest)*
    };
}
// A type with a text form of its own: JsonText comes from `TextForm`.
macro_rules! derives_text {
    ($(#[$attr:meta])* $vis:vis $kind:ident $($rest:tt)*) => {
        #[derive(
            Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, psio::PsszPack, psio::PsszUnpack,
            psio::ToPsszSchema, psio::Key,
        )]
        $(#[$attr])*
        $vis $kind $($rest)*
    };
}

derives! {
    #[psio(transparent)]
    pub struct Id { pub value: u64 }
}
derives! {
    #[psio(transparent)]
    pub struct Tag { pub text: RawString }
}

// A transparent type that bounds its member (docs/pssz-spec.md 5.10): the
// wrapper is the bounded UTF-8 string. C++ `PSIO_REFLECT(Memo,
// transparent(), attr(text, utf8 | max<80>))`; `WrapMemo` is the same type
// spelled with the wrapper type (C++ `utf8_string<80>`).
derives! {
    #[psio(transparent)]
    pub struct Memo { #[psio(max = 80)] pub text: String }
}
derives! {
    #[psio(transparent)]
    pub struct WrapMemo { pub text: psio::pssz::BoundedString<80> }
}
derives! {
    pub struct MemoRec { pub id: u32, pub memo: Memo, pub note: Option<Memo>, pub memos: Vec<Memo> }
}

// Bytes as base64.
derives_text! {
    #[psio(transparent, custom)]
    pub struct Blob { pub data: Vec<u8> }
}
impl TextForm for Blob {
    fn to_text(&self, out: &mut String) {
        out.push_str(&base64_encode(&self.data));
    }
    fn from_text(text: &str) -> Option<Self> {
        base64_decode(text).map(|data| Blob { data })
    }
}
psio::json_text_form!(Blob);

derives! {
    #[psio(definition_will_not_change)]
    pub struct V4 { pub address: [u8; 4], pub port: u16 }
}
derives! {
    #[psio(definition_will_not_change)]
    pub struct V6 { pub address: [u8; 16], pub port: u16 }
}
// C++ registers the text form for the std::variant itself; a Rust enum is a
// named type and carries it directly.
derives_text! {
    #[psio(custom)]
    pub enum Endpoint { V4(V4), V6(V6) }
}
impl TextForm for Endpoint {
    fn to_text(&self, out: &mut String) {
        out.push_str(&endpoint_to_text(&match self {
            Endpoint::V4(v) => EndpointParts::V4(v.address, v.port),
            Endpoint::V6(v) => EndpointParts::V6(v.address, v.port),
        }));
    }
    fn from_text(text: &str) -> Option<Self> {
        Some(match endpoint_from_text(text)? {
            EndpointParts::V4(address, port) => Endpoint::V4(V4 { address, port }),
            EndpointParts::V6(address, port) => Endpoint::V6(V6 { address, port }),
        })
    }
}
psio::json_text_form!(Endpoint);

// C++ registers the text form for the std::tuple itself; Rust wraps the
// tuple (a foreign type) transparently.
derives_text! {
    #[psio(transparent, custom)]
    pub struct Ratio(pub (i16, u16));
}
impl TextForm for Ratio {
    fn to_text(&self, out: &mut String) {
        out.push_str(&ratio_to_text(self.0 .0, self.0 .1));
    }
    fn from_text(text: &str) -> Option<Self> {
        ratio_from_text(text).map(Ratio)
    }
}
psio::json_text_form!(Ratio);

#[derive(
    Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, psio::PsszPack, psio::PsszUnpack,
    psio::JsonText, psio::ToPsszSchema, psio::Key,
)]
pub enum RefTarget {
    #[psio(name = "uint64")]
    Number(u64),
    #[psio(name = "string")]
    Name(RawString),
}
// A text form and an invariant: `"@"` is well-formed text of a value the
// invariant refuses.
derives_text! {
    #[psio(transparent, custom, invariant = "ref_check")]
    pub struct Ref { pub target: RefTarget }
}
pub fn ref_check(value: &Ref) -> Result<(), &'static str> {
    match &value.target {
        RefTarget::Name(name) if name.0.is_empty() => Err(REF),
        _ => Ok(()),
    }
}
impl TextForm for Ref {
    fn to_text(&self, out: &mut String) {
        let parts = match &self.target {
            RefTarget::Number(n) => RefParts::Number(*n),
            RefTarget::Name(name) => RefParts::Name(name.0.clone()),
        };
        // A name that is not UTF-8 has no text; the empty text is no value
        // of the type (it does not read back).
        out.push_str(&ref_to_text(&parts).unwrap_or_default());
    }
    fn from_text(text: &str) -> Option<Self> {
        Some(Ref {
            target: match ref_from_text(text)? {
                RefParts::Number(n) => RefTarget::Number(n),
                RefParts::Name(name) => RefTarget::Name(RawString(name)),
            },
        })
    }
}
psio::json_text_form!(Ref, invariant = ref_check);

derives! {
    #[psio(definition_will_not_change, invariant = "name_check")]
    pub struct Name { pub value: u64 }
}
pub fn name_check(name: &Name) -> Result<(), &'static str> {
    if name.value >> 60 == 0 { Ok(()) } else { Err(NAME) }
}

derives! {
    #[psio(transparent, invariant = "der_key_check")]
    pub struct DerKey { pub data: Vec<u8> }
}
pub fn der_key_check(key: &DerKey) -> Result<(), &'static str> {
    if key.data.first() == Some(&0x30) { Ok(()) } else { Err(DER_KEY) }
}

derives! {
    #[psio(invariant = "range_check")]
    pub struct Range { pub lo: u32, pub hi: u32 }
}
pub fn range_check(range: &Range) -> Result<(), &'static str> {
    if range.lo <= range.hi { Ok(()) } else { Err(RANGE) }
}

// A C++ enum holds every value of its underlying type; the invariant says
// which are values of the type. The mirror has the catch-all, and the same
// invariant refuses what it catches.
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, psio::PsszPack, psio::PsszUnpack,
    psio::JsonText, psio::ToPsszSchema, psio::Key,
)]
#[repr(u8)]
#[psio(invariant = "level_check")]
pub enum Level {
    #[psio(name = "low")]
    Low = 0,
    #[psio(name = "mid")]
    Mid = 1,
    #[psio(name = "high")]
    High = 2,
    #[psio(name = "top")]
    Top = 3,
    #[psio(other)]
    Unknown(u8),
}
pub fn level_check(level: &Level) -> Result<(), &'static str> {
    if matches!(level, Level::Unknown(_)) { Err(LEVEL) } else { Ok(()) }
}

#[derive(
    Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, psio::PsszPack, psio::PsszUnpack,
    psio::JsonText, psio::ToPsszSchema, psio::Key,
)]
pub enum NameOrRange {
    Name(Name),
    Range(Range),
}

derives! {
    pub struct Guarded {
        pub owner: Name,
        pub delegate: Option<Name>,
        pub members: Vec<Name>,
        pub key: DerKey,
        pub window: Range,
        pub levels: Vec<Level>,
        pub pair: [Level; 2],
        pub either: NameOrRange,
        pub r#ref: Ref,
        pub id: Id,
        pub blob: Option<Blob>,
        pub endpoint: Endpoint,
        pub ratios: Vec<Ratio>,
    }
}

/// The handlers of the custom types, from the types themselves: a text form
/// from `TextForm`, an invariant from the typed validation.
pub fn typed_custom_handlers(handlers: &mut psio::pssz::dynamic::CustomHandlers) {
    handlers.add_type::<Blob>().unwrap();
    handlers.add_type::<Endpoint>().unwrap();
    handlers.add_type::<Ratio>().unwrap();
    handlers.add_type::<Ref>().unwrap();
    handlers.add_invariant_type::<Name>().unwrap();
    handlers.add_invariant_type::<DerKey>().unwrap();
    handlers.add_invariant_type::<Range>().unwrap();
    handlers.add_invariant_type::<Level>().unwrap();
}

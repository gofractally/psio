//! Derive macros for the [psio crate](https://docs.rs/psio).
//!
//! * pSSZ: `PsszPack`, `PsszUnpack`, `PsszView`, `ToPsszSchema`
//! * Json / IJson text: `JsonText`
//! * serde impls with PSIO's text: `Serde`
//! * memcmp-sortable keys: `Key`
//! * Cap'n Proto: `CapnpPack`, `CapnpUnpack`, `CapnpView`
//! * FlatBuffers: `FbPack`, `FbUnpack`, `FbView`
//! * Protocol Buffers: `Protobuf`
//! * WIT Canonical ABI: `WitPack`, `WitUnpack`, `WitView`
//! * dynamic views: `ToDynamicSchema`
//! * methods and interfaces: the attribute `#[psio::interface]` on a trait
//! * with the feature `experimental-frac`, the derives of
//!   `psio::experimental::frac`: `Pack`, `Unpack`, `FracView`, `FracMutView`,
//!   `ToSchema`
//!
//! Every derive reads `#[psio(...)]`, the annotations that describe the type
//! for all formats (`crate`, `transparent`, `invariant`,
//! `definition_will_not_change`, `variant`, `custom`, `name`, `rename_all`,
//! `skip`, `other`). A format-named attribute holds only that format's keys:
//! `#[pssz(...)]` (`max_dynamic_data`) and `#[json(...)]` (the integer text
//! forms). The table is in `attrs.rs`; a key under the wrong attribute or an
//! unknown key is a compile error.
//!
//! Every derive also reads `#[serde(...)]`: an attribute with a PSIO meaning
//! (`transparent`, `rename`, `rename_all`, `skip`) is honoured, one that
//! would make PSIO's text differ from what it says is an error on the
//! derives that describe text, and the rest is ignored (`attrs.rs`).

use proc_macro::TokenStream;

mod attrs;
mod c_enum;
#[cfg(feature = "capnp")]
mod capnp_macro;
#[cfg(feature = "dynamic-view")]
mod dynamic_schema_macro;
#[cfg(feature = "flatbuf")]
mod fb_macro;
#[cfg(feature = "protobuf")]
mod pb_macro;
#[cfg(feature = "experimental-frac")]
mod frac_macro;
#[cfg(feature = "experimental-frac")]
mod frac_schema_macro;
mod interface_macro;
mod json_text_macro;
mod key_macro;
mod pssz_macro;
mod view_macro;
mod serde_macro;
#[cfg(feature = "wit")]
mod wit_macro;

#[proc_macro_derive(PsszPack, attributes(psio, pssz, serde))]
pub fn derive_pssz_pack(input: TokenStream) -> TokenStream { pssz_macro::derive(input, false) }

#[proc_macro_derive(PsszUnpack, attributes(psio, pssz, serde))]
pub fn derive_pssz_unpack(input: TokenStream) -> TokenStream { pssz_macro::derive(input, true) }

/// Also reads `#[json(...)]` (see `JsonText`): integer forms become the member
/// attributes `jsonInt` / `ijsonInt`. A variant's `#[psio(name = "...")]` is
/// its enumerator name, or its label when it is an alternative (the name of
/// the alternative in the schema and the key of its tagged text form).
/// A member's `#[psio(field = N)]`, `#[psio(deprecated)]` and
/// `#[psio(default = value)]` become its attributes `field`, `deprecated` and
/// `default` (a scalar's text; nothing for zero), in that order after the
/// others: what the numbered formats read of a member.
#[proc_macro_derive(ToPsszSchema, attributes(psio, pssz, json, serde))]
pub fn derive_to_pssz_schema(input: TokenStream) -> TokenStream { pssz_macro::derive_schema(input) }

#[proc_macro_derive(PsszView, attributes(psio, pssz, serde))]
pub fn derive_pssz_view(input: TokenStream) -> TokenStream { pssz_macro::derive_view(input) }

// ── psio::experimental::frac derive macros ──────────────────────────────

#[cfg(feature = "experimental-frac")]
#[proc_macro_derive(Pack, attributes(psio, serde))]
pub fn derive_pack(input: TokenStream) -> TokenStream {
    frac_macro::frac_macro_impl(input, true, false)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_derive(Unpack, attributes(psio, serde))]
pub fn derive_unpack(input: TokenStream) -> TokenStream {
    frac_macro::frac_macro_impl(input, false, true)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_error::proc_macro_error]
#[proc_macro_derive(FracView, attributes(psio, serde))]
pub fn derive_frac_view(input: TokenStream) -> TokenStream {
    frac_macro::fracview_macro_impl(input)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_error::proc_macro_error]
#[proc_macro_derive(FracMutView, attributes(psio, serde))]
pub fn derive_frac_mut_view(input: TokenStream) -> TokenStream {
    frac_macro::fracmutview_macro_impl(input)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_derive(ToSchema, attributes(psio, serde))]
pub fn to_schema(input: TokenStream) -> TokenStream {
    frac_schema_macro::schema_derive_macro(input)
}

/// The Json / IJson text formats (`psio::json::{Json, IJson}`); see
/// `json_text_macro.rs` for the supported shapes and `#[json(...)]` attributes.
#[proc_macro_derive(JsonText, attributes(psio, json, serde))]
pub fn derive_json_text(input: TokenStream) -> TokenStream {
    json_text_macro::derive(input)
}

/// `serde::Serialize` and `serde::Deserialize` for a type that is `JsonText`
/// (`psio::serde_bridge`): the serde impls read and write the type's IJson
/// text. Named `Serde` so it can be imported next to serde's own derives.
#[proc_macro_derive(Serde, attributes(psio, json, serde))]
pub fn derive_serde(input: TokenStream) -> TokenStream {
    serde_macro::derive(input)
}

/// The memcmp-sortable `key` format (`psio::key`): a struct is the
/// concatenation of its fields' keys (a composite key), an enum of
/// single-field variants is the variant index byte and the payload.
///
///
/// Reads `#[psio(transparent)]` and `#[psio(invariant = "path")]`.
#[proc_macro_derive(Key, attributes(psio, serde))]
pub fn derive_key(input: TokenStream) -> TokenStream {
    key_macro::derive(input)
}

// ── Cap'n Proto derive macros ───────────────────────────────────────────

#[cfg(feature = "capnp")]
#[proc_macro_derive(CapnpPack, attributes(psio, serde))]
pub fn derive_capnp_pack(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_pack_macro_impl(input)
}

#[cfg(feature = "capnp")]
#[proc_macro_derive(CapnpUnpack, attributes(psio, serde))]
pub fn derive_capnp_unpack(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_unpack_macro_impl(input)
}

#[cfg(feature = "capnp")]
#[proc_macro_derive(CapnpView, attributes(psio, serde))]
pub fn derive_capnp_view(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_view_macro_impl(input)
}

// ── FlatBuffers derive macros ───────────────────────────────────────────

#[cfg(feature = "flatbuf")]
#[proc_macro_derive(FbPack, attributes(psio, serde))]
pub fn derive_fb_pack(input: TokenStream) -> TokenStream {
    fb_macro::fb_pack_macro_impl(input)
}

#[cfg(feature = "flatbuf")]
#[proc_macro_derive(FbUnpack, attributes(psio, serde))]
pub fn derive_fb_unpack(input: TokenStream) -> TokenStream {
    fb_macro::fb_unpack_macro_impl(input)
}

#[cfg(feature = "flatbuf")]
#[proc_macro_derive(FbView, attributes(psio, serde))]
pub fn derive_fb_view(input: TokenStream) -> TokenStream {
    fb_macro::fb_view_macro_impl(input)
}

/// The view of a type that has one shape in every format (`psio::view`):
/// for a record `Order`, the struct `OrderView` that `View<'_, Order, F>`
/// names; for an enum with payloads, an enum to match on.
/// `#[view(pssz, flatbuf)]` names the formats (`pssz` when none is named);
/// the type derives each named format's own view too (`PsszView`,
/// `FbView`). Also `#[view(name = "Name")]`, `#[view(no_validator)]`,
/// `#[view(no_debug)]`: see `docs/rust-views.md`.
#[proc_macro_derive(View, attributes(view, psio, pssz, json, serde))]
pub fn derive_view(input: TokenStream) -> TokenStream {
    view_macro::view_macro_impl(input)
}

// ── Protocol Buffers derive macro ───────────────────────────────────────

/// A struct is a message, a field-less `#[repr(int)]` enum a protobuf enum,
/// an enum whose variants hold one value each a oneof (psio `protobuf`).
#[cfg(feature = "protobuf")]
#[proc_macro_derive(Protobuf, attributes(psio, serde))]
pub fn derive_protobuf(input: TokenStream) -> TokenStream {
    pb_macro::protobuf_macro_impl(input)
}

// ── WIT Canonical ABI derive macros ─────────────────────────────────────

#[cfg(feature = "wit")]
#[proc_macro_derive(WitPack, attributes(psio, serde))]
pub fn derive_wit_pack(input: TokenStream) -> TokenStream {
    wit_macro::wit_pack_macro_impl(input)
}

#[cfg(feature = "wit")]
#[proc_macro_derive(WitUnpack, attributes(psio, serde))]
pub fn derive_wit_unpack(input: TokenStream) -> TokenStream {
    wit_macro::wit_unpack_macro_impl(input)
}

#[cfg(feature = "wit")]
#[proc_macro_derive(WitView, attributes(psio, serde))]
pub fn derive_wit_view(input: TokenStream) -> TokenStream {
    wit_macro::wit_view_macro_impl(input)
}

// ── Dynamic schema derive macro ────────────────────────────────────────

#[cfg(feature = "dynamic-view")]
#[proc_macro_derive(ToDynamicSchema, attributes(psio, serde))]
pub fn derive_to_dynamic_schema(input: TokenStream) -> TokenStream {
    dynamic_schema_macro::dynamic_schema_derive_impl(input)
}

// ── Methods and interfaces ─────────────────────────────────────────────

/// A trait's functions as an interface: names, parameter names and types,
/// the argument record of each method with its pSSZ, dispatch, a call proxy
/// and the Schema IR `Interface` (C++ `PSIO_REFLECT(T, method(...))` +
/// `PSIO_INTERFACE(T)`). See `psio::interface`.
#[proc_macro_attribute]
pub fn interface(args: TokenStream, item: TokenStream) -> TokenStream {
    interface_macro::expand(args.into(), item.into())
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

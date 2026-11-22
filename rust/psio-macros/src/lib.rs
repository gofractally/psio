//! Derive macros for the [psio crate](https://docs.rs/psio).
//!
//! * pSSZ: `PsszPack`, `PsszUnpack`, `PsszView`, `ToPsszSchema`
//! * Json / IJson text: `JsonText`
//! * memcmp-sortable keys: `Key`
//! * Cap'n Proto: `CapnpPack`, `CapnpUnpack`, `CapnpView`
//! * FlatBuffers: `FbPack`, `FbUnpack`, `FbView`
//! * WIT Canonical ABI: `WitPack`, `WitUnpack`, `WitView`
//! * dynamic views: `ToDynamicSchema`
//! * with the feature `experimental-frac`, the derives of
//!   `psio::experimental::frac`: `Pack`, `Unpack`, `FracView`, `FracMutView`,
//!   `ToSchema`
//!
//! Every derive reads `#[psio(...)]`, the annotations that describe the type
//! for all formats (`crate`, `transparent`, `invariant`,
//! `definition_will_not_change`, `variant`, `custom`, `name`, `other`). A
//! format-named attribute holds only that format's keys: `#[pssz(...)]`
//! (`max_dynamic_data`), `#[json(...)]` (the integer text forms) and
//! `#[frac(...)]` (`skip`). The table is in `attrs.rs`; a key under the wrong
//! attribute or an unknown key is a compile error.

use proc_macro::TokenStream;

mod attrs;
mod c_enum;
mod capnp_macro;
mod dynamic_schema_macro;
mod fb_macro;
#[cfg(feature = "experimental-frac")]
mod frac_macro;
#[cfg(feature = "experimental-frac")]
mod frac_schema_macro;
mod json_text_macro;
mod key_macro;
mod pssz_macro;
mod wit_macro;

#[proc_macro_derive(PsszPack, attributes(psio, pssz))]
pub fn derive_pssz_pack(input: TokenStream) -> TokenStream { pssz_macro::derive(input, false) }

#[proc_macro_derive(PsszUnpack, attributes(psio, pssz))]
pub fn derive_pssz_unpack(input: TokenStream) -> TokenStream { pssz_macro::derive(input, true) }

/// Also reads `#[json(...)]` (see `JsonText`): integer forms become the member
/// attributes `jsonInt` / `ijsonInt`. A variant's `#[psio(name = "...")]` is
/// its enumerator name, or its `ijsonName` when it is an alternative.
#[proc_macro_derive(ToPsszSchema, attributes(psio, pssz, json))]
pub fn derive_to_pssz_schema(input: TokenStream) -> TokenStream { pssz_macro::derive_schema(input) }

#[proc_macro_derive(PsszView, attributes(psio, pssz))]
pub fn derive_pssz_view(input: TokenStream) -> TokenStream { pssz_macro::derive_view(input) }

// ── psio::experimental::frac derive macros ──────────────────────────────

#[cfg(feature = "experimental-frac")]
#[proc_macro_derive(Pack, attributes(psio, frac))]
pub fn derive_pack(input: TokenStream) -> TokenStream {
    frac_macro::frac_macro_impl(input, true, false)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_derive(Unpack, attributes(psio, frac))]
pub fn derive_unpack(input: TokenStream) -> TokenStream {
    frac_macro::frac_macro_impl(input, false, true)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_error::proc_macro_error]
#[proc_macro_derive(FracView, attributes(psio, frac))]
pub fn derive_frac_view(input: TokenStream) -> TokenStream {
    frac_macro::fracview_macro_impl(input)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_error::proc_macro_error]
#[proc_macro_derive(FracMutView, attributes(psio, frac))]
pub fn derive_frac_mut_view(input: TokenStream) -> TokenStream {
    frac_macro::fracmutview_macro_impl(input)
}

#[cfg(feature = "experimental-frac")]
#[proc_macro_derive(ToSchema, attributes(psio, frac))]
pub fn to_schema(input: TokenStream) -> TokenStream {
    frac_schema_macro::schema_derive_macro(input)
}

/// The Json / IJson text formats (`psio::json::{Json, IJson}`); see
/// `json_text_macro.rs` for the supported shapes and `#[json(...)]` attributes.
#[proc_macro_derive(JsonText, attributes(psio, json))]
pub fn derive_json_text(input: TokenStream) -> TokenStream {
    json_text_macro::derive(input)
}

/// The memcmp-sortable `key` format (`psio::key`): a struct is the
/// concatenation of its fields' keys (a composite key), an enum of
/// single-field variants is the variant index byte and the payload.
///
///
/// Reads `#[psio(transparent)]` and `#[psio(invariant = "path")]`.
#[proc_macro_derive(Key, attributes(psio))]
pub fn derive_key(input: TokenStream) -> TokenStream {
    key_macro::derive(input)
}

// ── Cap'n Proto derive macros ───────────────────────────────────────────

#[proc_macro_derive(CapnpPack, attributes(psio))]
pub fn derive_capnp_pack(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_pack_macro_impl(input)
}

#[proc_macro_derive(CapnpUnpack, attributes(psio))]
pub fn derive_capnp_unpack(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_unpack_macro_impl(input)
}

#[proc_macro_derive(CapnpView, attributes(psio))]
pub fn derive_capnp_view(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_view_macro_impl(input)
}

// ── FlatBuffers derive macros ───────────────────────────────────────────

#[proc_macro_derive(FbPack, attributes(psio))]
pub fn derive_fb_pack(input: TokenStream) -> TokenStream {
    fb_macro::fb_pack_macro_impl(input)
}

#[proc_macro_derive(FbUnpack, attributes(psio))]
pub fn derive_fb_unpack(input: TokenStream) -> TokenStream {
    fb_macro::fb_unpack_macro_impl(input)
}

#[proc_macro_derive(FbView, attributes(psio))]
pub fn derive_fb_view(input: TokenStream) -> TokenStream {
    fb_macro::fb_view_macro_impl(input)
}

// ── WIT Canonical ABI derive macros ─────────────────────────────────────

#[proc_macro_derive(WitPack, attributes(psio))]
pub fn derive_wit_pack(input: TokenStream) -> TokenStream {
    wit_macro::wit_pack_macro_impl(input)
}

#[proc_macro_derive(WitUnpack, attributes(psio))]
pub fn derive_wit_unpack(input: TokenStream) -> TokenStream {
    wit_macro::wit_unpack_macro_impl(input)
}

#[proc_macro_derive(WitView, attributes(psio))]
pub fn derive_wit_view(input: TokenStream) -> TokenStream {
    wit_macro::wit_view_macro_impl(input)
}

// ── Dynamic schema derive macro ────────────────────────────────────────

#[proc_macro_derive(ToDynamicSchema, attributes(psio))]
pub fn derive_to_dynamic_schema(input: TokenStream) -> TokenStream {
    dynamic_schema_macro::dynamic_schema_derive_impl(input)
}

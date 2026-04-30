//! Derive macros for the [psio crate](https://docs.rs/psio).
//!
//! Provides `Pack`, `Unpack`, `FracView`, `FracMutView`, `ToSchema` derive macros
//! for fracpack, plus `CapnpPack`, `CapnpUnpack`, `CapnpView`, `FbPack`, `FbUnpack`,
//! `FbView`, `WitPack`, `WitUnpack`, `WitView` for multi-format serialization.

use fracpack_macro::{fracmutview_macro_impl, fracpack_macro_impl, fracview_macro_impl};
use json_macro::{from_canonical_json_macro_impl, to_canonical_json_macro_impl};
use proc_macro::TokenStream;
use proc_macro_error::proc_macro_error;
use schema_macro::schema_derive_macro;

mod dynamic_schema_macro;
mod fracpack_macro;
mod json_macro;
mod schema_macro;
mod capnp_macro;
mod fb_macro;
mod wit_macro;

// TODO: remove
#[proc_macro_derive(Fracpack, attributes(fracpack))]
pub fn derive_fracpack(input: TokenStream) -> TokenStream {
    fracpack_macro_impl(input, true, true)
}

#[proc_macro_derive(Pack, attributes(fracpack))]
pub fn derive_pack(input: TokenStream) -> TokenStream {
    fracpack_macro_impl(input, true, false)
}

#[proc_macro_derive(Unpack, attributes(fracpack))]
pub fn derive_unpack(input: TokenStream) -> TokenStream {
    fracpack_macro_impl(input, false, true)
}

#[proc_macro_error]
#[proc_macro_derive(FracView, attributes(fracpack))]
pub fn derive_frac_view(input: TokenStream) -> TokenStream {
    fracview_macro_impl(input)
}

#[proc_macro_error]
#[proc_macro_derive(FracMutView, attributes(fracpack))]
pub fn derive_frac_mut_view(input: TokenStream) -> TokenStream {
    fracmutview_macro_impl(input)
}

#[proc_macro_derive(ToSchema, attributes(schema, fracpack))]
pub fn to_schema(input: TokenStream) -> TokenStream {
    schema_derive_macro(input)
}

#[proc_macro_derive(ToCanonicalJson, attributes(fracpack))]
pub fn derive_to_canonical_json(input: TokenStream) -> TokenStream {
    to_canonical_json_macro_impl(input)
}

#[proc_macro_derive(FromCanonicalJson, attributes(fracpack))]
pub fn derive_from_canonical_json(input: TokenStream) -> TokenStream {
    from_canonical_json_macro_impl(input)
}

// ── Cap'n Proto derive macros ───────────────────────────────────────────

#[proc_macro_derive(CapnpPack, attributes(fracpack))]
pub fn derive_capnp_pack(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_pack_macro_impl(input)
}

#[proc_macro_derive(CapnpUnpack, attributes(fracpack))]
pub fn derive_capnp_unpack(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_unpack_macro_impl(input)
}

#[proc_macro_derive(CapnpView, attributes(fracpack))]
pub fn derive_capnp_view(input: TokenStream) -> TokenStream {
    capnp_macro::capnp_view_macro_impl(input)
}

// ── FlatBuffers derive macros ───────────────────────────────────────────

#[proc_macro_derive(FbPack, attributes(fracpack))]
pub fn derive_fb_pack(input: TokenStream) -> TokenStream {
    fb_macro::fb_pack_macro_impl(input)
}

#[proc_macro_derive(FbUnpack, attributes(fracpack))]
pub fn derive_fb_unpack(input: TokenStream) -> TokenStream {
    fb_macro::fb_unpack_macro_impl(input)
}

#[proc_macro_derive(FbView, attributes(fracpack))]
pub fn derive_fb_view(input: TokenStream) -> TokenStream {
    fb_macro::fb_view_macro_impl(input)
}

// ── WIT Canonical ABI derive macros ─────────────────────────────────────

#[proc_macro_derive(WitPack, attributes(fracpack))]
pub fn derive_wit_pack(input: TokenStream) -> TokenStream {
    wit_macro::wit_pack_macro_impl(input)
}

#[proc_macro_derive(WitUnpack, attributes(fracpack))]
pub fn derive_wit_unpack(input: TokenStream) -> TokenStream {
    wit_macro::wit_unpack_macro_impl(input)
}

#[proc_macro_derive(WitView, attributes(fracpack))]
pub fn derive_wit_view(input: TokenStream) -> TokenStream {
    wit_macro::wit_view_macro_impl(input)
}

// ── Dynamic schema derive macro ────────────────────────────────────────

#[proc_macro_derive(ToDynamicSchema)]
pub fn derive_to_dynamic_schema(input: TokenStream) -> TokenStream {
    dynamic_schema_macro::dynamic_schema_derive_impl(input)
}

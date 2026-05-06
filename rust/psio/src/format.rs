//! Format-tagged CPO machinery — the crate-root `encode<F, T>`,
//! `decode<F, T>`, and `validate<F, T, P>` functions described in
//! `docs/psio-overview.md` §2.4.
//!
//! A `Format` type is a type-level tag for a particular wire format
//! (`Pjson`, `Pssz`, `Fracpack`, …). The actual encode / decode /
//! validate logic lives in three per-type traits parameterized by
//! `F: Format`:
//!
//!   * `Encode<F>`   — `T` knows how to encode as format `F`.
//!   * `Decode<F>`   — `T` knows how to decode from format-`F` bytes.
//!   * `Validate<F>` — format-`F` bytes can be validated as a `T`.
//!
//! Per-format modules (`psio::pjson`, `psio::pssz`, …) provide the
//! format tag type and `impl` the three per-type traits for whatever
//! types they support. Callers reach the CPO through three free
//! functions at the crate root:
//!
//! ```ignore
//! let bytes = psio::encode::<psio::pjson::format::Pjson, MyType>(&value)?;
//! let v: MyType = psio::decode::<psio::pjson::format::Pjson, _>(&bytes)?;
//! psio::validate::<psio::pjson::format::Pjson, MyType, _>(&bytes, &StrictCanonical)?;
//! ```
//!
//! With type inference and `use` statements the call sites flatten:
//!
//! ```ignore
//! use psio::{validate, StrictCanonical};
//! use psio::pjson::format::Pjson;
//!
//! validate::<Pjson, MyType, _>(&bytes, &StrictCanonical)?;
//! ```

use crate::ValidationPolicy;

/// Marker trait for wire formats. A `Format` type tags a particular
/// serialization. Implementations are zero-sized types (`Pjson`,
/// `Pssz`, …); no instances are constructed — the type is used as
/// a generic parameter.
pub trait Format: 'static + Sized {
    /// Format-specific error type returned by all CPO operations.
    type Error;
}

/// `Self` is encodable as format `F`.
///
/// Implemented by per-type code (or macro-derived) for every type a
/// format supports. The crate-root `encode<F, T>` function dispatches
/// here.
pub trait Encode<F: Format> {
    /// Append the format-`F` encoding of `self` to `out`. Returns the
    /// number of bytes written.
    fn encode_into(&self, out: &mut Vec<u8>) -> Result<usize, F::Error>;
}

/// `Self` is decodable from a format-`F` byte buffer.
pub trait Decode<F: Format>: Sized {
    /// Decode a format-`F` buffer into `Self`. The buffer must be
    /// exactly the encoded value's bytes — no trailing data, no
    /// preceding data.
    fn decode(bytes: &[u8]) -> Result<Self, F::Error>;
}

/// Format-`F` bytes can be validated against the structural rules
/// for a value of type `Self`. No materialization — the validator
/// walks the buffer and asserts structural invariants per the policy.
pub trait Validate<F: Format>: Sized {
    /// Validate `bytes` against the format's rules under `policy`.
    /// Returns `Ok(())` if every requested invariant holds.
    fn validate<P: ValidationPolicy>(bytes: &[u8], policy: &P)
        -> Result<(), F::Error>;
}

// ── Crate-root CPOs ─────────────────────────────────────────────────────────

/// Encode `value` as format `F`. Allocates a fresh `Vec<u8>` exactly
/// sized for the encoded form.
///
/// Equivalent to `T::encode_into(value, &mut out)` followed by
/// returning `out`.
pub fn encode<F: Format, T: Encode<F> + ?Sized>(value: &T)
    -> Result<Vec<u8>, F::Error>
{
    let mut out = Vec::new();
    value.encode_into(&mut out)?;
    Ok(out)
}

/// Decode a format-`F` byte buffer into a value of type `T`.
pub fn decode<F: Format, T: Decode<F>>(bytes: &[u8]) -> Result<T, F::Error> {
    T::decode(bytes)
}

/// Validate `bytes` as a format-`F` encoding of type `T` under
/// `policy`. The CPO works with both call shapes from
/// `docs/psio-overview.md` §2.4:
///
///   * **Compile-time policy** — pass a ZST preset
///     (`&StrictCanonical`, `&DefaultSafe`, `&JustDontCrash`); the
///     trait method calls inline to constants and the optimizer
///     eliminates dead branches.
///
///   * **Runtime policy** — pass a `&DynamicPolicy { ... }` value
///     constructed at run time from a CLI flag, config, etc.;
///     branches are retained.
pub fn validate<F: Format, T: Validate<F>, P: ValidationPolicy>(
    bytes: &[u8], policy: &P,
) -> Result<(), F::Error> {
    T::validate::<P>(bytes, policy)
}

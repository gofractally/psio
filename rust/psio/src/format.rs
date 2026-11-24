//! Format-tagged CPO machinery — the crate-root `encode<F, T>`,
//! `decode<F, T>`, and `validate<F, T, P>` functions described in
//! `docs/psio-overview.md` §2.4.
//!
//! A `Format` type is a type-level tag for a particular wire format
//! (`Pjson`, `Pssz`, `Json`, …). The actual encode / decode /
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

    /// Size of the encoding. Formats that can compute it without encoding
    /// override this.
    fn encoded_size(&self) -> Result<usize, F::Error> {
        let mut out = Vec::new();
        self.encode_into(&mut out)
    }
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

/// Size in bytes of the format-`F` encoding of `value` (C++
/// `psio::size_of(fmt, v)`).
pub fn size_of<F: Format, T: Encode<F> + ?Sized>(value: &T) -> Result<usize, F::Error> {
    value.encoded_size()
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

/// The helper names of a format whose codec is a pair of traits with a
/// whole-message pack and unpack (capnp, wit): docs/helpers.md. (flatbuf
/// writes its own: its verifier is not its decoder.)
#[cfg(any(feature = "capnp", feature = "wit"))]
macro_rules! message_format_helpers {
    (
        tag $tag:ident, $cpp:literal, error $err:ty,
        pack $pack:path => $pack_fn:ident, unpack $unpack:path => $unpack_fn:ident,
        names $to:ident $to_into:ident $from:ident $from_trusted:ident $validate:ident $size:ident
              $buffer:ident $shared:ident
    ) => {
        #[doc = concat!("The format tag (C++ `psio::", $cpp, "`).")]
        pub struct $tag;
        impl $crate::Format for $tag {
            type Error = $err;
        }
        impl<T: $pack> $crate::Encode<$tag> for T {
            fn encode_into(&self, out: &mut Vec<u8>) -> Result<usize, $err> {
                let bytes = $pack_fn(self);
                out.extend_from_slice(&bytes);
                Ok(bytes.len())
            }
        }
        impl<T: $unpack> $crate::Decode<$tag> for T {
            fn decode(bytes: &[u8]) -> Result<Self, $err> {
                $unpack_fn::<T>(bytes)
            }
        }
        impl<T: $unpack> $crate::Validate<$tag> for T {
            fn validate<P: $crate::ValidationPolicy>(bytes: &[u8], _policy: &P) -> Result<(), $err> {
                $unpack_fn::<T>(bytes).map(drop)
            }
        }

        #[doc = concat!("Encodes `value` into a fresh buffer (C++ `psio::to_", $cpp, "(v)`).")]
        pub fn $to<T: $pack>(value: &T) -> Vec<u8> {
            $pack_fn(value)
        }
        #[doc = concat!("Replaces the content of `out` with the encoding of `value` (C++ `psio::to_", $cpp, "(v, sink)`; a message is self-contained and cannot be appended to another).")]
        pub fn $to_into<T: $pack>(value: &T, out: &mut Vec<u8>) {
            *out = $pack_fn(value);
        }
        #[doc = concat!("The checked decode (C++ `psio::from_", $cpp, "<T>(bytes)`).")]
        pub fn $from<T: $unpack>(bytes: &[u8]) -> Result<T, $err> {
            $unpack_fn::<T>(bytes)
        }
        #[doc = concat!("Decodes bytes the caller vouches for (C++ `psio::from_", $cpp, "<T>(psio::trusted{bytes})`). A safe function: nothing is skipped, and bytes that are not a valid `T` cause a panic.")]
        #[track_caller]
        pub fn $from_trusted<T: $unpack>(bytes: &[u8]) -> T {
            match $unpack_fn::<T>(bytes) {
                Ok(value) => value,
                Err(e) => panic!(concat!(stringify!($from_trusted), ": the bytes are not valid: {:?}"), e),
            }
        }
        #[doc = concat!("Whether `bytes` is one `T` (C++ `psio::validate_", $cpp, "<T>(bytes)`). Never panics. Accepts what the checked decode accepts.")]
        pub fn $validate<T: $unpack>(bytes: &[u8]) -> Result<(), $err> {
            $unpack_fn::<T>(bytes).map(drop)
        }
        #[doc = concat!("Size of the encoding of `value` (C++ `psio::", $cpp, "_size(v)`).")]
        pub fn $size<T: $pack>(value: &T) -> usize {
            <T as $crate::Encode<$tag>>::encoded_size(value).unwrap_or(0)
        }
        #[doc = concat!("One `T` in this format, owned (C++ `psio::", $cpp, "_buffer<T>`).")]
        pub type $buffer<T> = $crate::Buffer<T, $tag>;
        #[doc = concat!("One `T` in this format, immutable and shared (C++ `psio::shared_", $cpp, "<T>`).")]
        pub type $shared<T> = $crate::Shared<T, $tag>;
    };
}
#[cfg(any(feature = "capnp", feature = "wit"))]
pub(crate) use message_format_helpers;

//! The `key` format: memcmp-sortable byte strings (docs/key-spec.md). The
//! Rust counterpart of C++ `psio::key` (cpp/include/psio/key.hpp); both
//! produce and accept the same bytes (conformance/time-key-vectors.json).
//!
//! Every encoding is order-preserving (bytewise order of two keys is the
//! order of the two values) and prefix-free (no key is a proper prefix of
//! another key of the same type). Tuples and records are their elements
//! concatenated, so the key of `(A, B)` is a byte-prefix of the key of
//! `(A, B, C)` and tuple keys order lexicographically.
//!
//! ```
//! use psio::key::{from_key, key_prefix, key_prefix_upper_bound, to_key};
//!
//! type K = (u32, String, Option<i64>);
//! let k: K = (7, "bob".into(), Some(-1));
//! let bytes = to_key(&k);
//! assert_eq!(from_key::<K>(&bytes).unwrap(), k);
//!
//! // Scan every key of shard 7 owned by "bob": [prefix, bound).
//! let prefix = key_prefix::<K, _>(&(7, "bob"));
//! let bound = key_prefix_upper_bound(&prefix).unwrap();
//! assert!(bytes.starts_with(&prefix) && prefix <= bytes && bytes < bound);
//! ```
//!
//! A struct becomes a composite key equal to the tuple of its fields with
//! `#[derive(psio::Key)]`; an enum of single-field variants is the variant
//! index byte followed by the payload (C++ `std::variant`).
//!
//! Where byte order is not `Ord` / `PartialOrd` (docs/key-spec.md §4): a NaN
//! with the sign bit clear sorts above +inf and one with it set below -inf;
//! `-0.0` and `+0.0` share one key, which decodes to `+0.0`.
//!
//! Maps and sets (`BTreeMap`, `HashMap`, `BTreeSet`, `HashSet`,
//! `pssz::BoundedMap` / `BoundedSet`) are the key of the list of their
//! entries in ascending key order; that order — the bytes of this format —
//! is the one every PSIO format gives their entries (docs/pssz-spec.md 5.9).
//! [`ByKey`] makes any key type an `Ord + Hash` container key with exactly
//! that order (`BTreeSet<ByKey<f64>>`).
//!
//! Type invariants (docs/pssz-spec.md §18, `#[psio(invariant = "path")]`)
//! are part of both directions: [`try_to_key`] refuses a value that violates
//! one ([`to_key`] panics on it) and [`from_key`] / [`validate_key`] reject
//! its key with the invariant's message.

use crate::pssz::{Bitvector, BoundedList, BoundedRawString, BoundedString, RawString, Uint256};

pub use psio_macros::Key;

/// Why bytes are not a key of the requested type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct KeyError(pub &'static str);

impl std::fmt::Display for KeyError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}
impl std::error::Error for KeyError {}

pub type KeyResult<T> = Result<T, KeyError>;

/// Encoding half. The bytes must sort like the value and be prefix-free.
pub trait KeyPack {
    /// The encoded size when it is the same for every value of the type.
    const FIXED_SIZE: Option<usize> = None;

    /// One key byte per value (`u8`, `i8`): a list, set or map of them
    /// uses the string framing.
    const KEY_OCTET: bool = false;

    /// Exact size of [`KeyPack::key_pack`]'s output.
    fn key_size(&self) -> usize;
    /// Appends the key bytes. Runs no invariant: the bytes are defined for
    /// every value (they order the entries of maps and sets). Writers go
    /// through [`try_to_key`] / [`to_key`], which check first.
    fn key_pack(&self, out: &mut Vec<u8>);
    /// The invariants of the value and of the values it holds, innermost
    /// first (`#[psio(invariant = "path")]`). Containers forward to their
    /// elements; a type without one accepts every value.
    fn key_check(&self) -> KeyResult<()> {
        Ok(())
    }

    /// A list of `Self`: `01` + element per element, then `00`. Octets
    /// (`u8`, `i8`) override this pair with the string framing.
    fn key_list_size(items: &[Self]) -> usize
    where
        Self: Sized,
    {
        items.iter().map(|item| 1 + item.key_size()).sum::<usize>() + 1
    }
    fn key_pack_list(items: &[Self], out: &mut Vec<u8>)
    where
        Self: Sized,
    {
        for item in items {
            out.push(1);
            item.key_pack(out);
        }
        out.push(0);
    }
}

/// Decoding half. Accepts exactly the canonical encodings; never panics.
pub trait KeyUnpack: Sized {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self>;

    /// A list of `Self` of at most `limit` elements.
    fn key_unpack_list(r: &mut KeyReader<'_>, limit: usize) -> KeyResult<Vec<Self>> {
        let mut out = Vec::new();
        loop {
            match r.byte()? {
                0 => return Ok(out),
                1 => {}
                _ => return Err(KeyError("key: invalid list element marker")),
            }
            if out.len() >= limit {
                return Err(KeyError("key: list exceeds its bound"));
            }
            out.push(Self::key_unpack(r)?);
        }
    }
}

/// Cursor over key bytes for [`KeyUnpack`].
pub struct KeyReader<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> KeyReader<'a> {
    pub fn new(src: &'a [u8]) -> Self {
        Self { src, pos: 0 }
    }
    /// Bytes not yet consumed.
    pub fn remaining(&self) -> &'a [u8] {
        &self.src[self.pos..]
    }
    pub fn take(&mut self, n: usize) -> KeyResult<&'a [u8]> {
        let rest = self.remaining();
        if rest.len() < n {
            return Err(KeyError("key: truncated"));
        }
        self.pos += n;
        Ok(&rest[..n])
    }
    pub fn array<const N: usize>(&mut self) -> KeyResult<[u8; N]> {
        let mut out = [0u8; N];
        out.copy_from_slice(self.take(N)?);
        Ok(out)
    }
    pub fn byte(&mut self) -> KeyResult<u8> {
        Ok(self.take(1)?[0])
    }
    /// Escaped bytes up to and including the `00 00` terminator.
    pub fn escaped(&mut self) -> KeyResult<Vec<u8>> {
        let mut out = Vec::new();
        loop {
            let rest = self.remaining();
            let Some(zero) = rest.iter().position(|&b| b == 0) else {
                return Err(KeyError("key: unterminated string"));
            };
            match rest.get(zero + 1) {
                None => return Err(KeyError("key: unterminated string")),
                Some(0) => {
                    out.extend_from_slice(&rest[..zero]);
                    self.pos += zero + 2;
                    return Ok(out);
                }
                Some(1) => {
                    out.extend_from_slice(&rest[..=zero]);
                    self.pos += zero + 2;
                }
                Some(_) => return Err(KeyError("key: invalid escape")),
            }
        }
    }
}

fn escaped_size(bytes: &[u8]) -> usize {
    bytes.len() + bytes.iter().filter(|&&b| b == 0).count() + 2
}

fn pack_escaped(bytes: &[u8], out: &mut Vec<u8>) {
    let mut rest = bytes;
    while let Some(zero) = rest.iter().position(|&b| b == 0) {
        out.extend_from_slice(&rest[..=zero]);
        out.push(1);
        rest = &rest[zero + 1..];
    }
    out.extend_from_slice(rest);
    out.extend_from_slice(&[0, 0]);
}

// ── Entry points ────────────────────────────────────────────────────────────

/// The key of `value`. Panics when the value violates a type invariant
/// (it has no key): use [`try_to_key`] for values that were not checked.
pub fn to_key<T: KeyPack + ?Sized>(value: &T) -> Vec<u8> {
    try_to_key(value).expect("key: the value violates a type invariant; use try_to_key")
}

/// The key of `value`, or the invariant it (or a value it holds) violates.
pub fn try_to_key<T: KeyPack + ?Sized>(value: &T) -> KeyResult<Vec<u8>> {
    value.key_check()?;
    Ok(key_bytes(value))
}

/// The key bytes of `value` with no invariant check: the bytes maps and
/// sets are ordered by, defined for every value.
pub fn key_bytes<T: KeyPack + ?Sized>(value: &T) -> Vec<u8> {
    let mut out = Vec::with_capacity(value.key_size());
    value.key_pack(&mut out);
    out
}

/// Appends the key of `value` to `out` (C++ `psio::to_key(v, sink)`), or
/// reports the invariant it violates, leaving `out` as it was.
pub fn to_key_into<T: KeyPack + ?Sized>(value: &T, out: &mut Vec<u8>) -> KeyResult<()> {
    value.key_check()?;
    out.reserve(value.key_size());
    value.key_pack(out);
    Ok(())
}

/// Size of the key of `value`.
pub fn key_size<T: KeyPack + ?Sized>(value: &T) -> usize {
    value.key_size()
}

/// Decodes a full key. `bytes` must be exactly one canonical `T`.
pub fn from_key<T: KeyUnpack>(bytes: &[u8]) -> KeyResult<T> {
    let mut r = KeyReader::new(bytes);
    let value = T::key_unpack(&mut r)?;
    if !r.remaining().is_empty() {
        return Err(KeyError("key: trailing bytes"));
    }
    Ok(value)
}

/// Decodes a key the caller vouches for (C++
/// `psio::from_key<T>(psio::trusted{bytes})`). A safe function: the reader
/// checks as it reads, nothing is skipped, and bytes that are not a
/// canonical key of `T` cause a panic.
#[track_caller]
pub fn from_key_trusted<T: KeyUnpack>(bytes: &[u8]) -> T {
    match from_key::<T>(bytes) {
        Ok(value) => value,
        Err(e) => panic!("from_key_trusted: the bytes are not valid: {e}"),
    }
}

/// Checks that `bytes` is exactly one canonical key of `T`.
pub fn validate_key<T: KeyUnpack>(bytes: &[u8]) -> KeyResult<()> {
    from_key::<T>(bytes).map(drop)
}

/// The exclusive upper bound of a prefix scan: the smallest byte string
/// greater than every byte string that starts with `prefix` (`prefix`
/// without its trailing `FF` bytes, last byte incremented). The keys with
/// the prefix are exactly `prefix <= k < bound`. `None`: no bound — the
/// prefix is empty or all `FF`, so the scan runs to the end of the key space.
pub fn key_prefix_upper_bound(prefix: &[u8]) -> Option<Vec<u8>> {
    let keep = prefix.iter().rposition(|&b| b != 0xff)? + 1;
    let mut out = prefix[..keep].to_vec();
    out[keep - 1] += 1;
    Some(out)
}

// ── The generic layer and the buffer names ──────────────────────────────────

/// The key format tag (C++ `psio::key`).
pub struct KeyFormat;
impl crate::Format for KeyFormat {
    type Error = KeyError;
}
impl<T: KeyPack + ?Sized> crate::Encode<KeyFormat> for T {
    fn encode_into(&self, out: &mut Vec<u8>) -> KeyResult<usize> {
        let start = out.len();
        to_key_into(self, out)?;
        Ok(out.len() - start)
    }
    fn encoded_size(&self) -> KeyResult<usize> {
        Ok(self.key_size())
    }
}
impl<T: KeyUnpack> crate::Decode<KeyFormat> for T {
    fn decode(bytes: &[u8]) -> KeyResult<Self> {
        from_key::<T>(bytes)
    }
}
impl<T: KeyUnpack> crate::Validate<KeyFormat> for T {
    fn validate<P: crate::ValidationPolicy>(bytes: &[u8], _policy: &P) -> KeyResult<()> {
        validate_key::<T>(bytes)
    }
}

/// The key of one `T`, owned (C++ `psio::key_buffer<T>`).
pub type KeyBuffer<T> = crate::Buffer<T, KeyFormat>;
/// The key of one `T`, immutable and shared (C++ `psio::shared_key<T>`).
pub type SharedKey<T> = crate::Shared<T, KeyFormat>;

// ── Composite keys ──────────────────────────────────────────────────────────

/// A key made of ordered elements: a tuple, or a `#[derive(Key)]` struct
/// (whose `Elements` is the tuple of its field types).
pub trait CompositeKey {
    type Elements;
}

/// A value that can stand for key element type `E` in a prefix: `E`, `&E`,
/// and `str` / `[T]` borrows for `String` / `Vec<T>` elements. It is encoded
/// as `E`.
pub trait KeyElementAs<E> {
    fn key_element_size(&self) -> usize;
    fn key_element_pack(&self, out: &mut Vec<u8>);
    /// The element's invariants: each element of a prefix is checked as the
    /// value it is.
    fn key_element_check(&self) -> KeyResult<()> {
        Ok(())
    }
}
impl<E: KeyPack> KeyElementAs<E> for E {
    fn key_element_size(&self) -> usize {
        self.key_size()
    }
    fn key_element_pack(&self, out: &mut Vec<u8>) {
        self.key_pack(out)
    }
    fn key_element_check(&self) -> KeyResult<()> {
        self.key_check()
    }
}
impl<E: KeyPack> KeyElementAs<E> for &E {
    fn key_element_size(&self) -> usize {
        (**self).key_size()
    }
    fn key_element_pack(&self, out: &mut Vec<u8>) {
        (**self).key_pack(out)
    }
    fn key_element_check(&self) -> KeyResult<()> {
        (**self).key_check()
    }
}
impl KeyElementAs<String> for &str {
    fn key_element_size(&self) -> usize {
        escaped_size(self.as_bytes())
    }
    fn key_element_pack(&self, out: &mut Vec<u8>) {
        pack_escaped(self.as_bytes(), out)
    }
}
impl<T: KeyPack> KeyElementAs<Vec<T>> for &[T] {
    fn key_element_size(&self) -> usize {
        T::key_list_size(self)
    }
    fn key_element_pack(&self, out: &mut Vec<u8>) {
        T::key_pack_list(self, out)
    }
    fn key_element_check(&self) -> KeyResult<()> {
        self.iter().try_for_each(T::key_check)
    }
}

/// A tuple of the first K elements of the element tuple `Elements`.
pub trait KeyPrefixOf<Elements> {
    fn key_prefix_size(&self) -> usize;
    fn key_prefix_pack(&self, out: &mut Vec<u8>);
    fn key_prefix_check(&self) -> KeyResult<()>;
}

/// The key of the first K elements of `K` — the prefix every full key whose
/// leading elements equal `prefix` starts with. `prefix` is a tuple of 0 up
/// to all of `K`'s elements; each is checked at compile time against, and
/// encoded as, the key's element type at its position.
///
/// Panics when an element violates its type's invariant: use
/// [`try_key_prefix`] for elements that were not checked. The composite as a
/// whole is not checked (a prefix is not a value of the composite type).
pub fn key_prefix<K: CompositeKey, P: KeyPrefixOf<K::Elements>>(prefix: &P) -> Vec<u8> {
    try_key_prefix::<K, P>(prefix)
        .expect("key: a prefix element violates a type invariant; use try_key_prefix")
}

/// [`key_prefix`], or the invariant an element violates.
pub fn try_key_prefix<K: CompositeKey, P: KeyPrefixOf<K::Elements>>(prefix: &P) -> KeyResult<Vec<u8>> {
    prefix.key_prefix_check()?;
    let mut out = Vec::with_capacity(prefix.key_prefix_size());
    prefix.key_prefix_pack(&mut out);
    Ok(out)
}

// ── Scalars ─────────────────────────────────────────────────────────────────

impl KeyPack for bool {
    const FIXED_SIZE: Option<usize> = Some(1);
    fn key_size(&self) -> usize {
        1
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        out.push(*self as u8);
    }
}
impl KeyUnpack for bool {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        match r.byte()? {
            0 => Ok(false),
            1 => Ok(true),
            _ => Err(KeyError("key: invalid bool")),
        }
    }
}

macro_rules! key_integer {
    ($($ty:ty, $flip:expr;)*) => {$(
        impl KeyPack for $ty {
            const FIXED_SIZE: Option<usize> = Some(std::mem::size_of::<$ty>());
            fn key_size(&self) -> usize { std::mem::size_of::<$ty>() }
            fn key_pack(&self, out: &mut Vec<u8>) {
                let mut bytes = self.to_be_bytes();
                bytes[0] ^= $flip;
                out.extend_from_slice(&bytes);
            }
        }
        impl KeyUnpack for $ty {
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                let mut bytes = r.array::<{ std::mem::size_of::<$ty>() }>()?;
                bytes[0] ^= $flip;
                Ok(<$ty>::from_be_bytes(bytes))
            }
        }
    )*};
}
key_integer! {
    u16, 0; u32, 0; u64, 0; u128, 0;
    i16, 0x80; i32, 0x80; i64, 0x80; i128, 0x80;
}

// Octets: one key byte each, and the string framing inside a list.
macro_rules! key_octet {
    ($($ty:ty, $flip:expr;)*) => {$(
        impl KeyPack for $ty {
            const FIXED_SIZE: Option<usize> = Some(1);
            const KEY_OCTET: bool = true;
            fn key_size(&self) -> usize { 1 }
            fn key_pack(&self, out: &mut Vec<u8>) { out.push(*self as u8 ^ $flip); }
            fn key_list_size(items: &[Self]) -> usize {
                items.len() + items.iter().filter(|&&b| b as u8 ^ $flip == 0).count() + 2
            }
            fn key_pack_list(items: &[Self], out: &mut Vec<u8>) {
                for &item in items {
                    let b = item as u8 ^ $flip;
                    out.push(b);
                    if b == 0 {
                        out.push(1);
                    }
                }
                out.extend_from_slice(&[0, 0]);
            }
        }
        impl KeyUnpack for $ty {
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                Ok((r.byte()? ^ $flip) as $ty)
            }
            fn key_unpack_list(r: &mut KeyReader<'_>, limit: usize) -> KeyResult<Vec<Self>> {
                let bytes = r.escaped()?;
                if bytes.len() > limit {
                    return Err(KeyError("key: list exceeds its bound"));
                }
                Ok(bytes.into_iter().map(|b| (b ^ $flip) as $ty).collect())
            }
        }
    )*};
}
key_octet! { u8, 0; i8, 0x80; }

macro_rules! key_float {
    ($($ty:ty, $bits:ty;)*) => {$(
        impl KeyPack for $ty {
            const FIXED_SIZE: Option<usize> = Some(std::mem::size_of::<$ty>());
            fn key_size(&self) -> usize { std::mem::size_of::<$ty>() }
            fn key_pack(&self, out: &mut Vec<u8>) {
                const SIGN: $bits = 1 << (<$bits>::BITS - 1);
                let mut bits = self.to_bits();
                if bits == SIGN {
                    bits = 0; // -0 is +0
                }
                bits = if bits & SIGN != 0 { !bits } else { bits ^ SIGN };
                out.extend_from_slice(&bits.to_be_bytes());
            }
        }
        impl KeyUnpack for $ty {
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                const SIGN: $bits = 1 << (<$bits>::BITS - 1);
                let mut bits = <$bits>::from_be_bytes(r.array()?);
                bits = if bits & SIGN != 0 { bits ^ SIGN } else { !bits };
                if bits == SIGN {
                    return Err(KeyError("key: negative zero is not canonical"));
                }
                Ok(<$ty>::from_bits(bits))
            }
        }
    )*};
}
key_float! { f32, u32; f64, u64; }

impl KeyPack for Uint256 {
    const FIXED_SIZE: Option<usize> = Some(32);
    fn key_size(&self) -> usize {
        32
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        for limb in self.limb.iter().rev() {
            out.extend_from_slice(&limb.to_be_bytes());
        }
    }
}
impl KeyUnpack for Uint256 {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        let mut limb = [0u64; 4];
        for l in limb.iter_mut().rev() {
            *l = u64::from_be_bytes(r.array()?);
        }
        Ok(Uint256 { limb })
    }
}

// ── Strings ─────────────────────────────────────────────────────────────────

impl KeyPack for str {
    fn key_size(&self) -> usize {
        escaped_size(self.as_bytes())
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        pack_escaped(self.as_bytes(), out)
    }
}
impl KeyPack for String {
    fn key_size(&self) -> usize {
        escaped_size(self.as_bytes())
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        pack_escaped(self.as_bytes(), out)
    }
}
impl KeyUnpack for String {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        String::from_utf8(r.escaped()?).map_err(|_| KeyError("key: string is not valid UTF-8"))
    }
}
impl<const N: usize> KeyPack for BoundedString<N> {
    fn key_size(&self) -> usize {
        self.0.key_size()
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        self.0.key_pack(out)
    }
}
impl<const N: usize> KeyUnpack for BoundedString<N> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        let text = String::key_unpack(r)?;
        if text.len() > N {
            return Err(KeyError("key: string exceeds its bound"));
        }
        Ok(BoundedString(text))
    }
}
impl KeyPack for RawString {
    fn key_size(&self) -> usize {
        escaped_size(&self.0)
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        pack_escaped(&self.0, out)
    }
}
impl KeyUnpack for RawString {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        Ok(RawString(r.escaped()?))
    }
}
impl<const N: usize> KeyPack for BoundedRawString<N> {
    fn key_size(&self) -> usize {
        escaped_size(&self.0)
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        pack_escaped(&self.0, out)
    }
}
impl<const N: usize> KeyUnpack for BoundedRawString<N> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        let bytes = r.escaped()?;
        if bytes.len() > N {
            return Err(KeyError("key: string exceeds its bound"));
        }
        Ok(BoundedRawString(bytes))
    }
}

// ── Containers ──────────────────────────────────────────────────────────────

impl<T: KeyPack + ?Sized> KeyPack for &T {
    const FIXED_SIZE: Option<usize> = T::FIXED_SIZE;
    const KEY_OCTET: bool = T::KEY_OCTET;
    fn key_size(&self) -> usize {
        (**self).key_size()
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        (**self).key_pack(out)
    }
    fn key_check(&self) -> KeyResult<()> {
        (**self).key_check()
    }
}
impl<T: KeyPack + ?Sized> KeyPack for Box<T> {
    const FIXED_SIZE: Option<usize> = T::FIXED_SIZE;
    fn key_size(&self) -> usize {
        (**self).key_size()
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        (**self).key_pack(out)
    }
    fn key_check(&self) -> KeyResult<()> {
        (**self).key_check()
    }
}
impl<T: KeyUnpack> KeyUnpack for Box<T> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        T::key_unpack(r).map(Box::new)
    }
}

impl<T: KeyPack> KeyPack for [T] {
    fn key_size(&self) -> usize {
        T::key_list_size(self)
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        T::key_pack_list(self, out)
    }
    fn key_check(&self) -> KeyResult<()> {
        self.iter().try_for_each(T::key_check)
    }
}
impl<T: KeyPack> KeyPack for Vec<T> {
    fn key_size(&self) -> usize {
        T::key_list_size(self)
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        T::key_pack_list(self, out)
    }
    fn key_check(&self) -> KeyResult<()> {
        self.iter().try_for_each(T::key_check)
    }
}
impl<T: KeyUnpack> KeyUnpack for Vec<T> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        T::key_unpack_list(r, usize::MAX)
    }
}
impl<T: KeyPack, const N: usize> KeyPack for BoundedList<T, N> {
    fn key_size(&self) -> usize {
        T::key_list_size(&self.0)
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        T::key_pack_list(&self.0, out)
    }
    fn key_check(&self) -> KeyResult<()> {
        self.0.iter().try_for_each(T::key_check)
    }
}
impl<T: KeyUnpack, const N: usize> KeyUnpack for BoundedList<T, N> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        T::key_unpack_list(r, N).map(BoundedList)
    }
}

impl<T: KeyPack> KeyPack for Option<T> {
    fn key_size(&self) -> usize {
        1 + self.as_ref().map_or(0, T::key_size)
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        match self {
            None => out.push(0),
            Some(value) => {
                out.push(1);
                value.key_pack(out);
            }
        }
    }
    fn key_check(&self) -> KeyResult<()> {
        self.as_ref().map_or(Ok(()), T::key_check)
    }
}
impl<T: KeyUnpack> KeyUnpack for Option<T> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        match r.byte()? {
            0 => Ok(None),
            1 => T::key_unpack(r).map(Some),
            _ => Err(KeyError("key: invalid optional marker")),
        }
    }
}

const fn array_fixed(element: Option<usize>, n: usize) -> Option<usize> {
    match element {
        Some(size) => Some(size * n),
        None => None,
    }
}

// A fixed array is its N elements with no framing.
impl<T: KeyPack, const N: usize> KeyPack for [T; N] {
    const FIXED_SIZE: Option<usize> = array_fixed(T::FIXED_SIZE, N);
    fn key_size(&self) -> usize {
        self.iter().map(T::key_size).sum()
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        for item in self {
            item.key_pack(out);
        }
    }
    fn key_check(&self) -> KeyResult<()> {
        self.iter().try_for_each(T::key_check)
    }
}
impl<T: KeyUnpack, const N: usize> KeyUnpack for [T; N] {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        let mut items = Vec::with_capacity(N);
        for _ in 0..N {
            items.push(T::key_unpack(r)?);
        }
        items.try_into().map_err(|_| KeyError("key: array length"))
    }
}

// A bitvector is its (N + 7) / 8 bytes: equality order only.
impl<const N: usize> KeyPack for Bitvector<N> {
    const FIXED_SIZE: Option<usize> = Some((N + 7) / 8);
    fn key_size(&self) -> usize {
        (N + 7) / 8
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        // `bytes` is public: normalize a wrong length instead of panicking.
        let at = out.len();
        out.extend(self.bytes.iter().copied().chain(std::iter::repeat(0)).take((N + 7) / 8));
        if N % 8 != 0 {
            let last = at + (N + 7) / 8 - 1;
            out[last] &= (1u8 << (N % 8)) - 1;
        }
    }
}
impl<const N: usize> KeyUnpack for Bitvector<N> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        let bytes = r.take((N + 7) / 8)?;
        if N % 8 != 0 && bytes[bytes.len() - 1] >> (N % 8) != 0 {
            return Err(KeyError("key: bitvector padding bits are set"));
        }
        Ok(Bitvector { bytes: bytes.to_vec() })
    }
}

// ── Tuples ──────────────────────────────────────────────────────────────────

const fn sum_fixed(parts: &[Option<usize>]) -> Option<usize> {
    let mut total = 0;
    let mut i = 0;
    while i < parts.len() {
        match parts[i] {
            Some(size) => total += size,
            None => return None,
        }
        i += 1;
    }
    Some(total)
}

/// `FIXED_SIZE` of a record with these field sizes (used by `derive(Key)`).
#[doc(hidden)]
pub const fn record_fixed_size(parts: &[Option<usize>]) -> Option<usize> {
    sum_fixed(parts)
}

macro_rules! key_tuple {
    ($(($ty:ident, $p:ident, $index:tt)),*) => {
        impl<$($ty: KeyPack),*> KeyPack for ($($ty,)*) {
            const FIXED_SIZE: Option<usize> = sum_fixed(&[$($ty::FIXED_SIZE),*]);
            fn key_size(&self) -> usize { 0 $(+ self.$index.key_size())* }
            #[allow(unused_variables)]
            fn key_pack(&self, out: &mut Vec<u8>) { $(self.$index.key_pack(out);)* }
            fn key_check(&self) -> KeyResult<()> { $(self.$index.key_check()?;)* Ok(()) }
        }
        impl<$($ty: KeyUnpack),*> KeyUnpack for ($($ty,)*) {
            #[allow(unused_variables, clippy::unused_unit)]
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                Ok(($($ty::key_unpack(r)?,)*))
            }
        }
        impl<$($ty),*> CompositeKey for ($($ty,)*) {
            type Elements = Self;
        }
        key_tuple!(@prefixes [$($ty),*] [] $(($ty, $p, $index))*);
    };
    // One KeyPrefixOf impl per prefix length, 0 ..= arity.
    (@prefixes [$($all:ident),*] [$(($ty:ident, $p:ident, $index:tt))*]) => {
        key_tuple!(@prefix [$($all),*] $(($ty, $p, $index))*);
    };
    (@prefixes [$($all:ident),*] [$($done:tt)*] $next:tt $($rest:tt)*) => {
        key_tuple!(@prefix [$($all),*] $($done)*);
        key_tuple!(@prefixes [$($all),*] [$($done)* $next] $($rest)*);
    };
    (@prefix [$($all:ident),*] $(($ty:ident, $p:ident, $index:tt))*) => {
        impl<$($all,)* $($p: KeyElementAs<$ty>),*> KeyPrefixOf<($($all,)*)> for ($($p,)*) {
            fn key_prefix_size(&self) -> usize { 0 $(+ self.$index.key_element_size())* }
            #[allow(unused_variables)]
            fn key_prefix_pack(&self, out: &mut Vec<u8>) { $(self.$index.key_element_pack(out);)* }
            fn key_prefix_check(&self) -> KeyResult<()> { $(self.$index.key_element_check()?;)* Ok(()) }
        }
    };
}
key_tuple!();
key_tuple!((A, PA, 0));
key_tuple!((A, PA, 0), (B, PB, 1));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5), (H, PH, 6));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5), (H, PH, 6), (I, PI, 7));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5), (H, PH, 6), (I, PI, 7), (J, PJ, 8));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5), (H, PH, 6), (I, PI, 7), (J, PJ, 8), (K, PK, 9));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5), (H, PH, 6), (I, PI, 7), (J, PJ, 8), (K, PK, 9), (L, PL, 10));
key_tuple!((A, PA, 0), (B, PB, 1), (C, PC, 2), (D, PD, 3), (E, PE, 4), (G, PG, 5), (H, PH, 6), (I, PI, 7), (J, PJ, 8), (K, PK, 9), (L, PL, 10), (M, PM, 11));

// ── Keys as container keys ──────────────────────────────────────────────────

/// `T` ordered, compared and hashed by its key bytes: the order PSIO gives
/// the entries of a map and the elements of a set (docs/pssz-spec.md 5.9),
/// whatever `T`'s own `Ord` is and whether or not it has one. In every
/// format it is `T` (same bytes, key, text and schema).
///
/// ```
/// use psio::key::ByKey;
/// use std::collections::BTreeSet;
/// // f64 has no `Ord`; its key has (NaNs included, -0.0 == +0.0).
/// let set: BTreeSet<ByKey<f64>> = [2.5, -1.0, f64::NAN].map(ByKey).into();
/// assert_eq!(set.iter().next().unwrap().0, -1.0);
/// ```
#[derive(Debug, Clone, Copy, Default)]
pub struct ByKey<T>(pub T);

impl<T: KeyPack> PartialEq for ByKey<T> {
    fn eq(&self, other: &Self) -> bool {
        key_bytes(&self.0) == key_bytes(&other.0)
    }
}
impl<T: KeyPack> Eq for ByKey<T> {}
impl<T: KeyPack> PartialOrd for ByKey<T> {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl<T: KeyPack> Ord for ByKey<T> {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        key_bytes(&self.0).cmp(&key_bytes(&other.0))
    }
}
impl<T: KeyPack> std::hash::Hash for ByKey<T> {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        key_bytes(&self.0).hash(state)
    }
}
impl<T> From<T> for ByKey<T> {
    fn from(value: T) -> Self {
        Self(value)
    }
}
impl<T: KeyPack> KeyPack for ByKey<T> {
    const FIXED_SIZE: Option<usize> = T::FIXED_SIZE;
    const KEY_OCTET: bool = T::KEY_OCTET;
    fn key_size(&self) -> usize {
        self.0.key_size()
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        self.0.key_pack(out)
    }
    fn key_check(&self) -> KeyResult<()> {
        self.0.key_check()
    }
}
impl<T: KeyUnpack> KeyUnpack for ByKey<T> {
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        T::key_unpack(r).map(ByKey)
    }
}

// ── Maps and sets ───────────────────────────────────────────────────────────
//
// The key of the list of the entries in ascending key order: `01` + key(k) +
// key(v) per entry, then `00`; for a set `01` + key(element) per element,
// then `00`, and the string framing for a set of octets.

pub(crate) const MAP_ORDER: KeyError =
    KeyError("key: map entries are not in strictly ascending key order");
pub(crate) const SET_ORDER: KeyError =
    KeyError("key: set elements are not in strictly ascending order");

/// The key bytes of each item, ascending.
pub(crate) fn sorted_keys<'a, K: KeyPack + 'a>(keys: impl Iterator<Item = &'a K>) -> Vec<Vec<u8>> {
    let mut out: Vec<Vec<u8>> = keys.map(|key| key_bytes(key)).collect();
    out.sort();
    out
}
pub(crate) fn pack_set<'a, K: KeyPack + 'a>(keys: impl Iterator<Item = &'a K>, out: &mut Vec<u8>) {
    let sorted = sorted_keys(keys);
    if K::KEY_OCTET {
        let octets: Vec<u8> = sorted.iter().map(|key| key[0]).collect();
        pack_escaped(&octets, out);
    } else {
        for key in sorted {
            out.push(1);
            out.extend_from_slice(&key);
        }
        out.push(0);
    }
}
pub(crate) fn pack_map<'a, K: KeyPack + 'a, V: KeyPack + 'a>(
    entries: impl Iterator<Item = (&'a K, &'a V)>,
    out: &mut Vec<u8>,
) {
    let mut sorted: Vec<(Vec<u8>, &V)> = entries.map(|(k, v)| (key_bytes(k), v)).collect();
    sorted.sort_by(|a, b| a.0.cmp(&b.0));
    for (key, value) in sorted {
        out.push(1);
        out.extend_from_slice(&key);
        value.key_pack(out);
    }
    out.push(0);
}
pub(crate) fn unpack_set<K: KeyPack + KeyUnpack>(r: &mut KeyReader<'_>, limit: usize) -> KeyResult<Vec<K>> {
    let items = K::key_unpack_list(r, limit)?;
    let keys: Vec<Vec<u8>> = items.iter().map(|key| key_bytes(key)).collect();
    if keys.windows(2).any(|pair| pair[0] >= pair[1]) {
        return Err(SET_ORDER);
    }
    Ok(items)
}
pub(crate) fn unpack_map<K: KeyPack + KeyUnpack, V: KeyUnpack>(
    r: &mut KeyReader<'_>,
    limit: usize,
) -> KeyResult<Vec<(K, V)>> {
    let items = <(K, V)>::key_unpack_list(r, limit)?;
    let keys: Vec<Vec<u8>> = items.iter().map(|(key, _)| key_bytes(key)).collect();
    if keys.windows(2).any(|pair| pair[0] >= pair[1]) {
        return Err(MAP_ORDER);
    }
    Ok(items)
}

macro_rules! key_set {
    ([$($kb:tt)*], [$($cg:tt)*], $ty:ty, $limit:expr, |$s:ident| $inner:expr, |$items:ident| $build:expr) => {
        impl<K: KeyPack + $($kb)*, $($cg)*> KeyPack for $ty {
            fn key_size(&self) -> usize {
                let mut out = Vec::new();
                self.key_pack(&mut out);
                out.len()
            }
            fn key_pack(&self, out: &mut Vec<u8>) {
                let $s = self;
                pack_set($inner.iter(), out)
            }
            fn key_check(&self) -> KeyResult<()> {
                let $s = self;
                $inner.iter().try_for_each(K::key_check)
            }
        }
        impl<K: KeyPack + KeyUnpack + $($kb)*, $($cg)*> KeyUnpack for $ty {
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                let $items = unpack_set::<K>(r, $limit)?;
                let count = $items.len();
                let out: Self = $build;
                let $s = &out;
                if $inner.len() != count {
                    return Err(KeyError("key: set container merged distinct elements"));
                }
                Ok(out)
            }
        }
    };
}
macro_rules! key_map {
    ([$($kb:tt)*], [$($cg:tt)*], $ty:ty, $limit:expr, |$s:ident| $inner:expr, |$items:ident| $build:expr) => {
        impl<K: KeyPack + $($kb)*, V: KeyPack, $($cg)*> KeyPack for $ty {
            fn key_size(&self) -> usize {
                let mut out = Vec::new();
                self.key_pack(&mut out);
                out.len()
            }
            fn key_pack(&self, out: &mut Vec<u8>) {
                let $s = self;
                pack_map($inner.iter(), out)
            }
            fn key_check(&self) -> KeyResult<()> {
                let $s = self;
                $inner.iter().try_for_each(|(k, v)| {
                    k.key_check()?;
                    v.key_check()
                })
            }
        }
        impl<K: KeyPack + KeyUnpack + $($kb)*, V: KeyUnpack, $($cg)*> KeyUnpack for $ty {
            fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
                let $items = unpack_map::<K, V>(r, $limit)?;
                let count = $items.len();
                let out: Self = $build;
                let $s = &out;
                if $inner.len() != count {
                    return Err(KeyError("key: map container merged distinct keys"));
                }
                Ok(out)
            }
        }
    };
}
use crate::pssz::{BoundedMap, BoundedSet};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::hash::Hash;
key_set!([Ord], [], BTreeSet<K>, usize::MAX, |s| s, |items| items.into_iter().collect());
key_set!([Eq + Hash], [], HashSet<K>, usize::MAX, |s| s, |items| items.into_iter().collect());
key_set!([Ord], [const N: usize], BoundedSet<K, N>, N, |s| s.0,
         |items| BoundedSet(items.into_iter().collect()));
key_map!([Ord], [], BTreeMap<K, V>, usize::MAX, |s| s, |items| items.into_iter().collect());
key_map!([Eq + Hash], [], HashMap<K, V>, usize::MAX, |s| s, |items| items.into_iter().collect());
key_map!([Ord], [const N: usize], BoundedMap<K, V, N>, N, |s| s.0,
         |items| BoundedMap(items.into_iter().collect()));

// A nested value (`pssz::Nested`) is the key of the byte list holding its
// inner encoding: the octet framing, ordered by those bytes. Reading
// validates the bytes as the inner type unless the value is deferred.
impl<T, V, const N: usize> KeyPack for crate::pssz::Nested<T, V, N> {
    fn key_size(&self) -> usize {
        escaped_size(self.bytes())
    }
    fn key_pack(&self, out: &mut Vec<u8>) {
        pack_escaped(self.bytes(), out)
    }
}
impl<T, V, const N: usize> KeyUnpack for crate::pssz::Nested<T, V, N>
where
    T: crate::pssz::PsszPack<crate::pssz::Pssz> + crate::pssz::PsszValidate<crate::pssz::Pssz>,
    V: crate::pssz::NestedValidation,
{
    fn key_unpack(r: &mut KeyReader<'_>) -> KeyResult<Self> {
        Self::adopt(r.escaped()?).map_err(|error| KeyError(error.0))
    }
}

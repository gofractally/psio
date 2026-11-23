//! `Nested<T>`: a value that is the pSSZ encoding of a `T`, carried as a byte
//! list (docs/pssz-spec.md 5.8; C++ `psio::nested<T>`; Schema IR
//! `Nested{type}`).
//!
//! ```
//! use psio::pssz::{from_pssz, to_pssz, Nested};
//!
//! #[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
//! struct Transfer { to: String, amount: u64 }
//! #[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
//! struct Action { method: u32, args: Nested<Transfer> }
//! // The same bytes, read by a router that does not interpret `args`.
//! #[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
//! struct RawAction { method: u32, args: Vec<u8> }
//!
//! let args = Transfer { to: "bob".into(), amount: 5 };
//! let action = Action { method: 9, args: Nested::new(&args).unwrap() };
//! let frame = to_pssz(&action);
//! let raw = from_pssz::<RawAction>(&frame).unwrap();
//! assert_eq!(raw.args, action.args.bytes());
//! assert_eq!(from_pssz::<Action>(&frame).unwrap().args.decode().unwrap(), args);
//! ```
//!
//! * **Wire.** The bytes of `Vec<u8>` holding `T`'s encoding under the
//!   automatic profile (`Pssz`), whatever profile the enclosing value uses.
//! * **Validation descends.** Every reader of the enclosing value validates
//!   the inner bytes as a `T` with its own rule (compatible or exact),
//!   invariants included; the inner root is one node level below the nested
//!   member. `Nested<T, Deferred>` is not looked into by an enclosing reader:
//!   the holder calls [`Nested::validate`] before reading through it.
//! * **Encoding does not re-validate.** The bytes were produced by the
//!   encoder ([`Nested::new`]) or checked when adopted ([`Nested::checked`]);
//!   [`Nested::unchecked`] vouches for them.
//! * **Order, equality, hash and key** are those of the bytes: nested values
//!   order by their inner encoding as stored, not by the inner values.
//! * **Text** (`JsonText`) is the inner value's own text; reading stores the
//!   canonical encoding of the value read.
//! * `Nested<T, Checked, N>` bounds the byte count of the inner encoding
//!   (C++ `length_bound{.max = N}` on a nested member).
use super::*;
use std::marker::PhantomData;

/// The validation policy of a [`Nested`] value: [`Checked`] or [`Deferred`].
pub trait NestedValidation: 'static {
    /// Enclosing readers do not look inside the bytes.
    const DEFERRED: bool;
}
/// Every reader of the enclosing value validates the inner bytes (default).
pub struct Checked;
/// Enclosing readers treat the bytes as opaque; the holder validates.
pub struct Deferred;
impl NestedValidation for Checked {
    const DEFERRED: bool = false;
}
impl NestedValidation for Deferred {
    const DEFERRED: bool = true;
}

/// See the [module documentation](self).
pub struct Nested<T, V = Checked, const N: usize = { usize::MAX }> {
    bytes: Vec<u8>,
    _phantom: PhantomData<fn() -> (T, V)>,
}

const OVERFLOW: PsszError = PsszError::new("pssz: member length_bound violated");

impl<T, V, const N: usize> Nested<T, V, N> {
    /// Adopts bytes the caller vouches for. Bytes that are not a `T` (or
    /// exceed the bound) produce a frame every checking reader rejects.
    pub fn unchecked(bytes: Vec<u8>) -> Self {
        Self { bytes, _phantom: PhantomData }
    }
    /// The inner encoding.
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn into_bytes(self) -> Vec<u8> {
        self.bytes
    }
    pub fn len(&self) -> usize {
        self.bytes.len()
    }
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
}
impl<T: PsszPack<Pssz>, V, const N: usize> Nested<T, V, N> {
    /// The encoding of `value` (which runs its invariants).
    pub fn new(value: &T) -> PsszResult<Self> {
        let bytes = try_encode::<Pssz, T>(value)?;
        if bytes.len() > N {
            return Err(OVERFLOW);
        }
        Ok(Self::unchecked(bytes))
    }
}
impl<T: PsszPack<Pssz> + PsszValidate<Pssz>, V: NestedValidation, const N: usize> Nested<T, V, N> {
    /// Adopts `bytes` after validating them as a `T` (the compatible rule:
    /// unknown appended optional fields are accepted and kept).
    pub fn checked(bytes: Vec<u8>) -> PsszResult<Self> {
        let out = Self::unchecked(bytes);
        out.validate()?;
        Ok(out)
    }
    /// Validates the bytes as a `T`; the result tells whether unknown
    /// appended optional fields are present.
    pub fn validate(&self) -> PsszResult<bool> {
        if self.bytes.len() > N {
            return Err(OVERFLOW);
        }
        validate_compatible::<Pssz, T>(&self.bytes)
    }
    /// Validates the bytes as the one canonical encoding of a `T`.
    pub fn validate_exact(&self) -> PsszResult<()> {
        if self.bytes.len() > N {
            return Err(OVERFLOW);
        }
        validate_strict::<Pssz, T>(&self.bytes)
    }
    /// What a reader of the enclosing value does with the bytes: the rule
    /// of the reader in progress (compatible or exact), or nothing for a
    /// deferred value. The inner root is one node level below this one.
    /// `depth`: the depth of the inner root.
    fn descend(bytes: &[u8], depth: usize) -> PsszResult<()> {
        if bytes.len() > N {
            return Err(OVERFLOW);
        }
        if V::DEFERRED {
            if !bytes.is_empty() {
                checked::decode_node(depth)?;
            }
            return Ok(());
        }
        <T as PsszValidate<Pssz>>::pssz_validate_at_depth(bytes, depth)
    }
    /// Adopts bytes read from an enclosing value (PSSZ or key).
    pub(crate) fn adopt(bytes: Vec<u8>) -> PsszResult<Self> {
        Self::descend(&bytes, checked::decode_depth())?;
        Ok(Self::unchecked(bytes))
    }
}
impl<T: PsszPack<Pssz> + PsszUnpack<Pssz>, V, const N: usize> Nested<T, V, N> {
    /// The inner value: validates, then decodes (unknown appended optional
    /// fields are skipped).
    pub fn decode(&self) -> PsszResult<T> {
        from_pssz::<T>(&self.bytes)
    }
    /// [`Nested::decode`], reporting whether unknown fields were present.
    pub fn decode_compatible(&self) -> PsszResult<Decoded<T>> {
        decode_checked::<Pssz, T>(&self.bytes)
    }
}

impl<T, V, const N: usize> Clone for Nested<T, V, N> {
    fn clone(&self) -> Self {
        Self::unchecked(self.bytes.clone())
    }
}
impl<T, V, const N: usize> std::fmt::Debug for Nested<T, V, N> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("Nested").field(&self.bytes).finish()
    }
}
impl<T, V, const N: usize> PartialEq for Nested<T, V, N> {
    fn eq(&self, other: &Self) -> bool {
        self.bytes == other.bytes
    }
}
impl<T, V, const N: usize> Eq for Nested<T, V, N> {}
impl<T, V, const N: usize> PartialOrd for Nested<T, V, N> {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}
impl<T, V, const N: usize> Ord for Nested<T, V, N> {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.bytes.cmp(&other.bytes)
    }
}
impl<T, V, const N: usize> std::hash::Hash for Nested<T, V, N> {
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.bytes.hash(state)
    }
}
/// The encoding of `T::default()`.
impl<T: Default + PsszPack<Pssz>, V, const N: usize> Default for Nested<T, V, N> {
    fn default() -> Self {
        Self::unchecked(encode::<Pssz, T>(&T::default()))
    }
}

const fn nested_bound(n: usize) -> Option<usize> {
    if n == usize::MAX { None } else { Some(n) }
}

impl<F: PsszFormat, T: PsszPack<Pssz>, V: NestedValidation, const N: usize> PsszPack<F>
    for Nested<T, V, N>
{
    const MIN_ENCODED_SIZE: usize = 0;
    const MAX_ENCODED_SIZE: Option<usize> = nested_bound(N);
    const IS_FIXED_SIZE: bool = false;
    const FIXED_SIZE: usize = 0;
    const INFERRED_BOUND: Option<usize> = nested_bound(N);
    // The inner value is always in the automatic profile, whatever `F` is.
    const SHAPE: crate::pssz::Shape = crate::pssz::Shape::node(
        crate::pssz::ShapeKind::Nested,
        N,
        <Self as PsszPack<F>>::OFFSET_WIDTH,
        false,
        &[<T as PsszPack<Pssz>>::SHAPE],
    );
    fn pssz_size(&self) -> usize {
        self.bytes.len()
    }
    fn pssz_pack(&self, out: &mut Vec<u8>) {
        <Self as PsszPack<F>>::pssz_pack_checked(self, out)
            .expect("pssz: nested value exceeds its bound or the depth limit; use try_to_pssz");
    }
    fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> PsszResult<()> {
        checked::pack_checked::<F, Self>(self, out)
    }
    fn pssz_pack_at_depth(&self, out: &mut Vec<u8>, depth: usize) -> PsszResult<()> {
        checked::encode_node(depth)?;
        // The member must leave one level for the inner root; the levels
        // inside the bytes are the readers' to check.
        if !(V::DEFERRED && self.bytes.is_empty()) {
            checked::encode_node(depth + 1)?;
        }
        if self.bytes.len() > N {
            return Err(OVERFLOW);
        }
        checked::append(out, &self.bytes)
    }
    fn pssz_size_at_depth(&self, depth: usize) -> PsszResult<usize> {
        checked::encode_node(depth)?;
        if !(V::DEFERRED && self.bytes.is_empty()) {
            checked::encode_node(depth + 1)?;
        }
        if self.bytes.len() > N {
            return Err(OVERFLOW);
        }
        Ok(self.bytes.len())
    }
    fn pssz_size_hint(&self) -> usize {
        self.bytes.len()
    }
}
impl<F: PsszFormat, T, V, const N: usize> PsszUnpack<F> for Nested<T, V, N>
where
    T: PsszPack<Pssz> + PsszValidate<Pssz>,
    V: NestedValidation,
{
    fn pssz_unpack(bytes: &[u8]) -> PsszResult<Self> {
        <Self as PsszUnpack<F>>::pssz_unpack_at_depth(bytes, checked::decode_depth())
    }
    fn pssz_unpack_at_depth(bytes: &[u8], depth: usize) -> PsszResult<Self> {
        checked::decode_node(depth)?;
        Self::descend(bytes, depth + 1)?;
        let mut out = Vec::new();
        out.try_reserve(bytes.len()).map_err(|_| PsszError::new("pssz: nested allocation overflow"))?;
        out.extend_from_slice(bytes);
        Ok(Self::unchecked(out))
    }
}
impl<F: PsszFormat, T, V, const N: usize> PsszValidate<F> for Nested<T, V, N>
where
    T: PsszPack<Pssz> + PsszValidate<Pssz>,
    V: NestedValidation,
{
    fn pssz_validate(bytes: &[u8]) -> PsszResult<()> {
        <Self as PsszValidate<F>>::pssz_validate_at_depth(bytes, checked::decode_depth())
    }
    fn pssz_validate_at_depth(bytes: &[u8], depth: usize) -> PsszResult<()> {
        checked::decode_node(depth)?;
        Self::descend(bytes, depth + 1)
    }
}

impl<T, V, const N: usize> crate::pssz_schema_ir::ToPsszSchema for Nested<T, V, N>
where
    T: crate::pssz_schema_ir::ToPsszSchema,
    V: NestedValidation,
{
    fn member_attributes() -> Vec<crate::pssz_schema_ir::Attribute> {
        // A nested member's bound is the member attribute `lengthBound`.
        match nested_bound(N) {
            Some(max) => vec![crate::pssz_schema_ir::Attribute {
                name: "lengthBound".into(),
                value: Some(format!("max={max}")),
            }],
            None => vec![],
        }
    }
    fn build_schema(
        builder: &mut crate::pssz_schema_ir::SchemaBuilder,
    ) -> PsszResult<crate::pssz_schema_ir::AnyType> {
        use crate::pssz_schema_ir as ir;
        Ok(ir::AnyType::new(ir::AnyValue::Nested(ir::Nested { r#type: Box::new(T::schema(builder)?) })))
    }
}

// Text: the inner value's own text. A member's integer form reaches the
// inner value, as it reaches through an optional.
impl<T, V, const N: usize> crate::json::JsonText for Nested<T, V, N>
where
    T: crate::json::JsonText + PsszPack<Pssz> + PsszUnpack<Pssz>,
{
    fn write_json(&self, w: &mut crate::json::Writer) -> Result<(), crate::json::JsonTextError> {
        if self.bytes.len() > N {
            return Err(w.err("json: member length_bound violated"));
        }
        match from_pssz::<T>(&self.bytes) {
            Ok(value) => value.write_json(w),
            Err(_) => Err(w.err("json: nested bytes are not a valid encoding of their type")),
        }
    }
    fn read_json(r: &mut crate::json::Reader<'_>) -> Result<Self, crate::json::JsonTextError> {
        r.ws();
        let at = r.position();
        let value = T::read_json(r)?;
        match try_encode::<Pssz, T>(&value) {
            Ok(bytes) if bytes.len() <= N => Ok(Self::unchecked(bytes)),
            Ok(_) => Err(r.err_at(at, "json: member length_bound violated")),
            Err(_) => Err(r.err_at(at, "json: nested value has no encoding")),
        }
    }
}

//! Typed byte buffers: the counterpart of C++ `psio::buffer<T, Fmt, Store>`
//! (docs/helpers.md, "Views and buffers").
//!
//! * [`Buffer<T, F>`] owns a `Vec<u8>` holding one `T` encoded in format `F`
//!   (C++ `F_buffer<T>`; the alias for pSSZ is `PsszBuffer<T>`).
//! * [`Shared<T, F>`] holds the same bytes immutably behind an `Arc<[u8]>`
//!   (C++ `shared_F<T>`; `SharedPssz<T>`). A clone is another owner of the
//!   same bytes: a pointer copy and an atomic increment. It is the type for
//!   an encoded block or transaction that several parts of a program hold.
//!
//! `new` validates the bytes; `trusted` takes the caller's word. Both are
//! safe: the views and decoders never read outside the bytes whatever they
//! hold.
//!
//! A read view comes from either ([`Buffer::as_view`], [`Shared::as_view`]).
//! A mutable view comes only from `&mut Buffer` ([`Buffer::as_mut_view`]):
//! a `Shared`, a `&Buffer` and a read view have no path to one.

use crate::{Decode, DefaultSafe, Encode, Format, Validate};
use std::marker::PhantomData;
use std::sync::Arc;

/// A format with a read-only view type for `T`.
pub trait ViewFormat<T: ?Sized>: Format {
    type View<'a>;
    /// The view of bytes that are a valid `T`; does not validate.
    fn view(bytes: &[u8]) -> Self::View<'_>;
}

/// A format with a mutable view type for `T` (pSSZ only: docs/pssz-spec.md
/// §17).
pub trait MutViewFormat<T: ?Sized>: ViewFormat<T> {
    type MutView<'a>;
    fn view_mut(bytes: &mut [u8]) -> Self::MutView<'_>;
}

/// One `T` encoded in format `F`, owned.
pub struct Buffer<T: ?Sized, F: Format> {
    bytes: Vec<u8>,
    _phantom: PhantomData<(fn() -> *const T, fn() -> F)>,
}

/// One `T` encoded in format `F`, immutable and reference-counted.
pub struct Shared<T: ?Sized, F: Format> {
    bytes: Arc<[u8]>,
    _phantom: PhantomData<(fn() -> *const T, fn() -> F)>,
}

impl<T: ?Sized, F: Format> Buffer<T, F> {
    /// Takes `bytes` after validating them as one `T`.
    pub fn new(bytes: Vec<u8>) -> Result<Self, F::Error>
    where
        T: Validate<F>,
    {
        T::validate(&bytes, &DefaultSafe)?;
        Ok(Self::trusted(bytes))
    }
    /// Takes bytes the caller vouches for (validated earlier, or produced
    /// by `to_F`); does not validate.
    pub fn trusted(bytes: Vec<u8>) -> Self {
        Buffer { bytes, _phantom: PhantomData }
    }
    /// The encoding of `value`.
    pub fn encode(value: &T) -> Result<Self, F::Error>
    where
        T: Encode<F>,
    {
        let mut bytes = Vec::new();
        value.encode_into(&mut bytes)?;
        Ok(Self::trusted(bytes))
    }
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn len(&self) -> usize {
        self.bytes.len()
    }
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
    pub fn into_bytes(self) -> Vec<u8> {
        self.bytes
    }
    /// Decodes the value (the checked decode).
    pub fn decode(&self) -> Result<T, F::Error>
    where
        T: Decode<F>,
    {
        T::decode(&self.bytes)
    }
    /// The read-only view of the bytes.
    pub fn as_view(&self) -> <F as ViewFormat<T>>::View<'_>
    where
        F: ViewFormat<T>,
    {
        F::view(&self.bytes)
    }
    /// The mutable view of the bytes. Needs `&mut self`.
    pub fn as_mut_view(&mut self) -> <F as MutViewFormat<T>>::MutView<'_>
    where
        F: MutViewFormat<T>,
    {
        F::view_mut(&mut self.bytes)
    }
    /// The same bytes, shared and immutable from here on.
    pub fn into_shared(self) -> Shared<T, F> {
        Shared { bytes: self.bytes.into(), _phantom: PhantomData }
    }
}

impl<T: ?Sized, F: Format> Shared<T, F> {
    /// Takes `bytes` after validating them as one `T`. A `Vec<u8>`,
    /// `Box<[u8]>`, `&[u8]` or `Arc<[u8]>` converts.
    pub fn new(bytes: impl Into<Arc<[u8]>>) -> Result<Self, F::Error>
    where
        T: Validate<F>,
    {
        let bytes = bytes.into();
        T::validate(&bytes, &DefaultSafe)?;
        Ok(Shared { bytes, _phantom: PhantomData })
    }
    /// Takes bytes the caller vouches for; does not validate.
    pub fn trusted(bytes: impl Into<Arc<[u8]>>) -> Self {
        Shared { bytes: bytes.into(), _phantom: PhantomData }
    }
    /// The encoding of `value`.
    pub fn encode(value: &T) -> Result<Self, F::Error>
    where
        T: Encode<F>,
    {
        Buffer::<T, F>::encode(value).map(Buffer::into_shared)
    }
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
    pub fn len(&self) -> usize {
        self.bytes.len()
    }
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
    /// The shared allocation itself.
    pub fn into_arc(self) -> Arc<[u8]> {
        self.bytes
    }
    /// Whether two handles share one allocation.
    pub fn ptr_eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.bytes, &other.bytes)
    }
    /// Decodes the value (the checked decode).
    pub fn decode(&self) -> Result<T, F::Error>
    where
        T: Decode<F>,
    {
        T::decode(&self.bytes)
    }
    /// The read-only view of the bytes.
    pub fn as_view(&self) -> <F as ViewFormat<T>>::View<'_>
    where
        F: ViewFormat<T>,
    {
        F::view(&self.bytes)
    }
    /// An owning copy of the bytes.
    pub fn to_buffer(&self) -> Buffer<T, F> {
        Buffer::trusted(self.bytes.to_vec())
    }
}

impl<T: ?Sized, F: Format> Clone for Buffer<T, F> {
    fn clone(&self) -> Self {
        Self::trusted(self.bytes.clone())
    }
}
impl<T: ?Sized, F: Format> Clone for Shared<T, F> {
    /// Another owner of the same bytes; nothing is copied.
    fn clone(&self) -> Self {
        Shared { bytes: Arc::clone(&self.bytes), _phantom: PhantomData }
    }
}
impl<T: ?Sized, F: Format> From<Buffer<T, F>> for Shared<T, F> {
    fn from(buffer: Buffer<T, F>) -> Self {
        buffer.into_shared()
    }
}

macro_rules! by_bytes {
    ($ty:ident, $name:literal) => {
        impl<T: ?Sized, F: Format> PartialEq for $ty<T, F> {
            fn eq(&self, other: &Self) -> bool {
                self.bytes() == other.bytes()
            }
        }
        impl<T: ?Sized, F: Format> Eq for $ty<T, F> {}
        impl<T: ?Sized, F: Format> std::hash::Hash for $ty<T, F> {
            fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
                self.bytes().hash(state)
            }
        }
        impl<T: ?Sized, F: Format> std::fmt::Debug for $ty<T, F> {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.debug_struct($name).field("bytes", &self.bytes()).finish()
            }
        }
        impl<T: ?Sized, F: Format> AsRef<[u8]> for $ty<T, F> {
            fn as_ref(&self) -> &[u8] {
                self.bytes()
            }
        }
    };
}
by_bytes!(Buffer, "Buffer");
by_bytes!(Shared, "Shared");

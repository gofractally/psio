//! Zero-copy SSZ views. Rust counterpart of `ssz_view.hpp`.
//!
//! Every view is a `(const char* data, u32 span)` pair over the
//! underlying bytes — no allocation, no materialization. Primitives
//! return by value through `.get()`; containers expose `.size()` and
//! `.get(i)` for sub-views.
//!
//! For reflected structs, use the `ssz_struct_view!` macro to generate
//! typed field accessors (mirrors C++ `view.field<I>()`).

use crate::ssz::{Bitlist, Bitvector, BoundedList, BoundedString};

/// A zero-copy view over a sub-range of an SSZ-encoded buffer.
#[derive(Debug, Clone, Copy)]
pub struct SszView<'a, T: ?Sized> {
    pub data: &'a [u8],
    _phantom: std::marker::PhantomData<*const T>,
}

impl<'a, T: ?Sized> SszView<'a, T> {
    pub fn new(data: &'a [u8]) -> Self {
        SszView { data, _phantom: std::marker::PhantomData }
    }
    pub fn raw(&self) -> &'a [u8] { self.data }
}

// ── Primitive accessors ─────────────────────────────────────────────────────

macro_rules! impl_view_primitive {
    ($($ty:ty),* $(,)?) => {$(
        impl<'a> SszView<'a, $ty> {
            pub fn get(&self) -> $ty {
                let mut buf = [0u8; std::mem::size_of::<$ty>()];
                buf.copy_from_slice(&self.data[..std::mem::size_of::<$ty>()]);
                <$ty>::from_le_bytes(buf)
            }
        }
    )*};
}
impl_view_primitive!(u8, u16, u32, u64, u128, i8, i16, i32, i64, i128, f32, f64);

impl<'a> SszView<'a, bool> {
    pub fn get(&self) -> bool { self.data[0] != 0 }
}

// ── String view ─────────────────────────────────────────────────────────────

impl<'a> SszView<'a, String> {
    pub fn view(&self) -> &'a str {
        std::str::from_utf8(self.data).expect("ssz view: invalid UTF-8")
    }
    pub fn len(&self) -> usize { self.data.len() }
}

// ── [T; N] view ─────────────────────────────────────────────────────────────

impl<'a, T: crate::ssz::SszPack + 'static, const N: usize> SszView<'a, [T; N]> {
    pub const fn size() -> usize { N }
    pub fn get(&self, i: usize) -> SszView<'a, T> {
        if T::IS_FIXED_SIZE {
            let elem = T::FIXED_SIZE;
            SszView::new(&self.data[i * elem .. (i + 1) * elem])
        } else {
            // Variable-element arrays use an N-entry offset table just
            // like vectors but with known size.
            let off_i = u32::from_le_bytes([
                self.data[i * 4], self.data[i * 4 + 1],
                self.data[i * 4 + 2], self.data[i * 4 + 3],
            ]) as usize;
            let off_next = if i + 1 < N {
                u32::from_le_bytes([
                    self.data[(i + 1) * 4], self.data[(i + 1) * 4 + 1],
                    self.data[(i + 1) * 4 + 2], self.data[(i + 1) * 4 + 3],
                ]) as usize
            } else {
                self.data.len()
            };
            SszView::new(&self.data[off_i .. off_next])
        }
    }
}

// ── Box<[T; N]> view — transparent forward to [T; N] ───────────────────────
// Box is a decode-side storage detail (heap-allocated fixed array); on the
// wire and on the view side it's indistinguishable from [T; N]. Keeps
// `view.randao_mixes().get(i)` working without unwrapping the Box.
impl<'a, T: crate::ssz::SszPack + 'static, const N: usize> SszView<'a, Box<[T; N]>> {
    pub const fn size() -> usize { N }
    pub fn get(&self, i: usize) -> SszView<'a, T> {
        if T::IS_FIXED_SIZE {
            let elem = T::FIXED_SIZE;
            SszView::new(&self.data[i * elem .. (i + 1) * elem])
        } else {
            let off_i = u32::from_le_bytes([
                self.data[i * 4], self.data[i * 4 + 1],
                self.data[i * 4 + 2], self.data[i * 4 + 3],
            ]) as usize;
            let off_next = if i + 1 < N {
                u32::from_le_bytes([
                    self.data[(i + 1) * 4], self.data[(i + 1) * 4 + 1],
                    self.data[(i + 1) * 4 + 2], self.data[(i + 1) * 4 + 3],
                ]) as usize
            } else {
                self.data.len()
            };
            SszView::new(&self.data[off_i .. off_next])
        }
    }
}

// ── Vec<T> view ─────────────────────────────────────────────────────────────

impl<'a, T: crate::ssz::SszPack + 'static> SszView<'a, Vec<T>> {
    pub fn len(&self) -> usize {
        if self.data.is_empty() { return 0; }
        if T::IS_FIXED_SIZE {
            self.data.len() / T::FIXED_SIZE
        } else {
            let first = u32::from_le_bytes([self.data[0], self.data[1], self.data[2], self.data[3]]);
            (first / 4) as usize
        }
    }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> SszView<'a, T> {
        if T::IS_FIXED_SIZE {
            let elem = T::FIXED_SIZE;
            SszView::new(&self.data[i * elem .. (i + 1) * elem])
        } else {
            let n = self.len();
            let off_i = u32::from_le_bytes([
                self.data[i * 4], self.data[i * 4 + 1],
                self.data[i * 4 + 2], self.data[i * 4 + 3],
            ]) as usize;
            let off_next = if i + 1 < n {
                u32::from_le_bytes([
                    self.data[(i + 1) * 4], self.data[(i + 1) * 4 + 1],
                    self.data[(i + 1) * 4 + 2], self.data[(i + 1) * 4 + 3],
                ]) as usize
            } else {
                self.data.len()
            };
            SszView::new(&self.data[off_i .. off_next])
        }
    }
}

// ── Option<T> view ──────────────────────────────────────────────────────────

impl<'a, T: crate::ssz::SszPack + 'static> SszView<'a, Option<T>> {
    pub fn has_value(&self) -> bool {
        !self.data.is_empty() && self.data[0] == 1
    }
    pub fn unwrap(&self) -> SszView<'a, T> {
        SszView::new(&self.data[1..])
    }
}

// ── BoundedList<T, N> view — wire-identical to Vec<T> ──────────────────────
impl<'a, T: crate::ssz::SszPack + 'static, const N: usize> SszView<'a, BoundedList<T, N>> {
    pub fn len(&self) -> usize {
        if self.data.is_empty() { return 0; }
        if T::IS_FIXED_SIZE {
            self.data.len() / T::FIXED_SIZE
        } else {
            let first = u32::from_le_bytes([self.data[0], self.data[1], self.data[2], self.data[3]]);
            (first / 4) as usize
        }
    }
    pub fn is_empty(&self) -> bool { self.len() == 0 }
    pub fn get(&self, i: usize) -> SszView<'a, T> {
        if T::IS_FIXED_SIZE {
            let elem = T::FIXED_SIZE;
            SszView::new(&self.data[i * elem .. (i + 1) * elem])
        } else {
            let n = self.len();
            let off_i = u32::from_le_bytes([
                self.data[i * 4], self.data[i * 4 + 1],
                self.data[i * 4 + 2], self.data[i * 4 + 3],
            ]) as usize;
            let off_next = if i + 1 < n {
                u32::from_le_bytes([
                    self.data[(i + 1) * 4], self.data[(i + 1) * 4 + 1],
                    self.data[(i + 1) * 4 + 2], self.data[(i + 1) * 4 + 3],
                ]) as usize
            } else {
                self.data.len()
            };
            SszView::new(&self.data[off_i .. off_next])
        }
    }
}

// BoundedString<N>: wire-identical to String (raw UTF-8).
impl<'a, const N: usize> SszView<'a, BoundedString<N>> {
    pub fn view(&self) -> &'a str {
        std::str::from_utf8(self.data).expect("ssz view: invalid UTF-8")
    }
    pub fn len(&self) -> usize { self.data.len() }
    pub fn is_empty(&self) -> bool { self.data.is_empty() }
}

// Bitlist<N>: just expose raw bytes; decoding the logical length requires
// scanning for the delimiter bit — done lazily via `Bitlist::ssz_unpack`.
impl<'a, const N: usize> SszView<'a, Bitlist<N>> {
    pub fn raw_bytes(&self) -> &'a [u8] { self.data }
}

// ── Bitvector<N> view ───────────────────────────────────────────────────────

impl<'a, const N: usize> SszView<'a, Bitvector<N>> {
    pub fn test(&self, i: usize) -> bool {
        (self.data[i >> 3] >> (i & 7)) & 1 != 0
    }
    pub const fn size(&self) -> usize { N }
}

// ── Top-level entry point ───────────────────────────────────────────────────

pub fn ssz_view_of<T: ?Sized>(buf: &[u8]) -> SszView<'_, T> {
    SszView::new(buf)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ssz::to_ssz;

    #[test]
    fn primitive_view() {
        let b = to_ssz(&0xDEADBEEFu32);
        let v: SszView<u32> = ssz_view_of(&b);
        assert_eq!(v.get(), 0xDEADBEEF);
    }

    #[test]
    fn string_view_no_alloc() {
        let b = to_ssz(&"hello".to_string());
        let v: SszView<String> = ssz_view_of(&b);
        assert_eq!(v.view(), "hello");
        assert_eq!(v.len(), 5);
    }

    #[test]
    fn vec_fixed_view() {
        let v = vec![10u32, 20, 30];
        let b = to_ssz(&v);
        let view: SszView<Vec<u32>> = ssz_view_of(&b);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0).get(), 10);
        assert_eq!(view.get(2).get(), 30);
    }

    #[test]
    fn vec_variable_view() {
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        let b = to_ssz(&v);
        let view: SszView<Vec<String>> = ssz_view_of(&b);
        assert_eq!(view.len(), 3);
        assert_eq!(view.get(0).view(), "a");
        assert_eq!(view.get(1).view(), "bc");
        assert_eq!(view.get(2).view(), "def");
    }

    #[test]
    fn option_view() {
        let some = Some(42u32);
        let b = to_ssz(&some);
        let view: SszView<Option<u32>> = ssz_view_of(&b);
        assert!(view.has_value());
        assert_eq!(view.unwrap().get(), 42);

        let none: Option<u32> = None;
        let b = to_ssz(&none);
        let view: SszView<Option<u32>> = ssz_view_of(&b);
        assert!(!view.has_value());
    }
}

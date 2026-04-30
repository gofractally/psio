//! WIT Canonical ABI layout computation.
//!
//! Provides alignment, size, and field offset computation matching
//! the WebAssembly Component Model Canonical ABI wire format.
//!
//! Key rules:
//! - Scalars: size = natural size, alignment = natural alignment
//! - bool: size 1, alignment 1 (NOT bit-packed)
//! - Strings/Lists: size 8, alignment 4 (two u32: ptr + len)
//! - Option<T>: discriminant (1 byte) + padding to payload alignment + payload
//! - Structs: fields laid out sequentially with alignment padding; trailing pad
//!   to struct alignment
//! - Tuples: same as structs

/// Trait for types that have a WIT Canonical ABI layout.
pub trait WitLayout {
    /// Alignment in bytes (always a power of two).
    fn wit_alignment() -> u32;

    /// Size in bytes, including trailing padding to alignment.
    fn wit_size() -> u32;
}

/// Location of a field within a WIT-encoded struct.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WitFieldLoc {
    /// Byte offset from the start of the containing record.
    pub offset: u32,
    /// Size of this field in the wire format.
    pub size: u32,
    /// Alignment of this field.
    pub alignment: u32,
}

// ── Scalar implementations ──────────────────────────────────────────────────

impl WitLayout for bool {
    fn wit_alignment() -> u32 { 1 }
    fn wit_size() -> u32 { 1 }
}

impl WitLayout for u8 {
    fn wit_alignment() -> u32 { 1 }
    fn wit_size() -> u32 { 1 }
}

impl WitLayout for i8 {
    fn wit_alignment() -> u32 { 1 }
    fn wit_size() -> u32 { 1 }
}

impl WitLayout for u16 {
    fn wit_alignment() -> u32 { 2 }
    fn wit_size() -> u32 { 2 }
}

impl WitLayout for i16 {
    fn wit_alignment() -> u32 { 2 }
    fn wit_size() -> u32 { 2 }
}

impl WitLayout for u32 {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 4 }
}

impl WitLayout for i32 {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 4 }
}

impl WitLayout for f32 {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 4 }
}

impl WitLayout for u64 {
    fn wit_alignment() -> u32 { 8 }
    fn wit_size() -> u32 { 8 }
}

impl WitLayout for i64 {
    fn wit_alignment() -> u32 { 8 }
    fn wit_size() -> u32 { 8 }
}

impl WitLayout for f64 {
    fn wit_alignment() -> u32 { 8 }
    fn wit_size() -> u32 { 8 }
}

// ── char: stored as u32 (Unicode scalar value) ─────────────────────────────

impl WitLayout for char {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 4 }
}

// ── String: stored as (ptr: u32, len: u32) ──────────────────────────────────

impl WitLayout for String {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 8 }
}

// Also support &str for view purposes (same wire layout)
impl WitLayout for str {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 8 }
}

// ── Vec<T>: stored as (ptr: u32, len: u32) ──────────────────────────────────

impl<T: WitLayout> WitLayout for Vec<T> {
    fn wit_alignment() -> u32 { 4 }
    fn wit_size() -> u32 { 8 }
}

// ── Option<T>: discriminant (1 byte) + aligned payload ──────────────────────

impl<T: WitLayout> WitLayout for Option<T> {
    fn wit_alignment() -> u32 {
        let ea = T::wit_alignment();
        if ea > 1 { ea } else { 1 }
    }

    fn wit_size() -> u32 {
        let ea = T::wit_alignment();
        let es = T::wit_size();
        let disc_padded = align_up(1, ea);
        let total = disc_padded + es;
        let a = if ea > 1 { ea } else { 1 };
        align_up(total, a)
    }
}

// ── Result<T, E>: discriminant + max(T, E) payload ─────────────────────────

impl<T: WitLayout, E: WitLayout> WitLayout for Result<T, E> {
    fn wit_alignment() -> u32 {
        // Variant alignment = max(disc_alignment=1, max(T::align, E::align))
        let max_case_align = std::cmp::max(T::wit_alignment(), E::wit_alignment());
        std::cmp::max(1, max_case_align)
    }

    fn wit_size() -> u32 {
        let max_case_align = std::cmp::max(T::wit_alignment(), E::wit_alignment());
        let max_case_size = std::cmp::max(T::wit_size(), E::wit_size());
        let disc_padded = align_up(1, max_case_align);
        let total = disc_padded + max_case_size;
        let a = std::cmp::max(1, max_case_align);
        align_up(total, a)
    }
}

// ── Tuples ──────────────────────────────────────────────────────────────────

impl WitLayout for () {
    fn wit_alignment() -> u32 { 1 }
    fn wit_size() -> u32 { 0 }
}

macro_rules! impl_wit_layout_tuple {
    ($($T:ident),+) => {
        impl<$($T: WitLayout),+> WitLayout for ($($T,)+) {
            fn wit_alignment() -> u32 {
                let mut max_align: u32 = 1;
                $(
                    let a = $T::wit_alignment();
                    if a > max_align { max_align = a; }
                )+
                max_align
            }

            fn wit_size() -> u32 {
                let mut offset: u32 = 0;
                let mut max_align: u32 = 1;
                $(
                    let fa = $T::wit_alignment();
                    let fs = $T::wit_size();
                    if fa > max_align { max_align = fa; }
                    offset = align_up(offset, fa) + fs;
                )+
                align_up(offset, max_align)
            }
        }
    };
}

impl_wit_layout_tuple!(A);
impl_wit_layout_tuple!(A, B);
impl_wit_layout_tuple!(A, B, C);
impl_wit_layout_tuple!(A, B, C, D);
impl_wit_layout_tuple!(A, B, C, D, E);
impl_wit_layout_tuple!(A, B, C, D, E, F);
impl_wit_layout_tuple!(A, B, C, D, E, F, G);
impl_wit_layout_tuple!(A, B, C, D, E, F, G, H);

// ── WitFlags: bitfield of named flags ──────────────────────────────────────

/// WIT flags type: a fixed-width bitfield.
///
/// Canonical ABI encoding:
/// - 0 flags: size 0, align 1
/// - 1-8 flags: stored as u8
/// - 9-16 flags: stored as u16
/// - 17-32 flags: stored as u32
/// - 33+ flags: stored as multiple u32 chunks
///
/// `N` is the number of flag fields.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WitFlags<const N: usize> {
    bits: Vec<u32>,
}

impl<const N: usize> WitFlags<N> {
    /// Number of u32 chunks needed to store N flags.
    const fn num_i32s() -> usize {
        if N == 0 { 0 } else { (N + 31) / 32 }
    }

    /// Create a new flags value with all flags cleared.
    pub fn new() -> Self {
        Self { bits: vec![0u32; Self::num_i32s()] }
    }

    /// Create from raw u32 chunks (little-endian, flag 0 in bit 0 of chunk 0).
    pub fn from_bits(bits: Vec<u32>) -> Self {
        let mut b = bits;
        let needed = Self::num_i32s();
        b.resize(needed, 0);
        Self { bits: b }
    }

    /// Get the raw u32 chunks.
    pub fn as_bits(&self) -> &[u32] {
        &self.bits
    }

    /// Test whether flag `i` is set.
    pub fn get(&self, i: usize) -> bool {
        assert!(i < N, "flag index {} out of range (max {})", i, N);
        let chunk = i / 32;
        let bit = i % 32;
        (self.bits[chunk] >> bit) & 1 != 0
    }

    /// Set flag `i` to `val`.
    pub fn set(&mut self, i: usize, val: bool) {
        assert!(i < N, "flag index {} out of range (max {})", i, N);
        let chunk = i / 32;
        let bit = i % 32;
        if val {
            self.bits[chunk] |= 1 << bit;
        } else {
            self.bits[chunk] &= !(1 << bit);
        }
    }

    /// Number of flags.
    pub const fn count(&self) -> usize { N }
}

impl<const N: usize> Default for WitFlags<N> {
    fn default() -> Self { Self::new() }
}

impl<const N: usize> WitLayout for WitFlags<N> {
    fn wit_alignment() -> u32 {
        match N {
            0 => 1,
            1..=8 => 1,
            9..=16 => 2,
            _ => 4,
        }
    }

    fn wit_size() -> u32 {
        match N {
            0 => 0,
            1..=8 => 1,
            9..=16 => 2,
            _ => (Self::num_i32s() as u32) * 4,
        }
    }
}

// ── Helper ──────────────────────────────────────────────────────────────────

/// Align `offset` up to the next multiple of `alignment`.
/// `alignment` must be a power of two.
#[inline]
pub fn align_up(offset: u32, alignment: u32) -> u32 {
    (offset + alignment - 1) & !(alignment - 1)
}

// ── Variant helpers ────────────────────────────────────────────────────────

/// Compute discriminant size in bytes for a variant with `case_count` alternatives.
///
/// Per the WIT Canonical ABI spec:
/// - <= 256 cases: u8 (1 byte)
/// - <= 65536 cases: u16 (2 bytes)
/// - otherwise: u32 (4 bytes)
pub const fn discriminant_size(case_count: usize) -> u32 {
    if case_count <= 256 {
        1
    } else if case_count <= 65536 {
        2
    } else {
        4
    }
}

/// Compute variant layout given case count and per-case (size, align) pairs.
///
/// Returns `(total_size, alignment)` for the variant.
pub fn variant_layout(case_count: usize, cases: &[(u32, u32)]) -> (u32, u32) {
    let disc_sz = discriminant_size(case_count);
    let disc_al = disc_sz;
    let mut max_payload_align: u32 = 1;
    let mut max_payload_size: u32 = 0;
    for &(size, align) in cases {
        max_payload_align = max_payload_align.max(align);
        max_payload_size = max_payload_size.max(size);
    }
    let variant_align = disc_al.max(max_payload_align);
    let payload_offset = align_up(disc_sz, max_payload_align);
    let total = align_up(payload_offset + max_payload_size, variant_align);
    (total, variant_align)
}

/// Compute payload offset for a variant (byte offset from the start of the
/// variant record to where the payload data begins).
pub fn variant_payload_offset(case_count: usize, max_payload_align: u32) -> u32 {
    align_up(discriminant_size(case_count), max_payload_align)
}

/// Compute field locations for a struct-like sequence of types.
///
/// Given a list of (alignment, size) pairs representing fields in order,
/// returns the corresponding `WitFieldLoc` for each, and the total
/// struct size (with trailing padding).
pub fn compute_struct_layout(fields: &[(u32, u32)]) -> (Vec<WitFieldLoc>, u32, u32) {
    let mut offset: u32 = 0;
    let mut max_align: u32 = 1;
    let mut locs = Vec::with_capacity(fields.len());

    for &(alignment, size) in fields {
        if alignment > max_align {
            max_align = alignment;
        }
        offset = align_up(offset, alignment);
        locs.push(WitFieldLoc { offset, size, alignment });
        offset += size;
    }

    let total_size = align_up(offset, max_align);
    (locs, total_size, max_align)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scalar_layouts() {
        assert_eq!(bool::wit_size(), 1);
        assert_eq!(bool::wit_alignment(), 1);

        assert_eq!(u8::wit_size(), 1);
        assert_eq!(u8::wit_alignment(), 1);

        assert_eq!(u16::wit_size(), 2);
        assert_eq!(u16::wit_alignment(), 2);

        assert_eq!(u32::wit_size(), 4);
        assert_eq!(u32::wit_alignment(), 4);

        assert_eq!(u64::wit_size(), 8);
        assert_eq!(u64::wit_alignment(), 8);

        assert_eq!(f32::wit_size(), 4);
        assert_eq!(f32::wit_alignment(), 4);

        assert_eq!(f64::wit_size(), 8);
        assert_eq!(f64::wit_alignment(), 8);
    }

    #[test]
    fn char_layout() {
        assert_eq!(char::wit_size(), 4);
        assert_eq!(char::wit_alignment(), 4);
    }

    #[test]
    fn string_layout() {
        assert_eq!(String::wit_size(), 8);
        assert_eq!(String::wit_alignment(), 4);
    }

    #[test]
    fn vec_layout() {
        assert_eq!(Vec::<u32>::wit_size(), 8);
        assert_eq!(Vec::<u32>::wit_alignment(), 4);
    }

    #[test]
    fn result_layout() {
        // Result<u32, u8>: disc(1) + 3 pad + max(4,1) = 8, align 4
        assert_eq!(Result::<u32, u8>::wit_size(), 8);
        assert_eq!(Result::<u32, u8>::wit_alignment(), 4);

        // Result<u8, u8>: disc(1) + max(1,1) = 2, align 1
        assert_eq!(Result::<u8, u8>::wit_size(), 2);
        assert_eq!(Result::<u8, u8>::wit_alignment(), 1);

        // Result<u64, u32>: disc(1) + 7 pad + max(8,4)=8 = 16, align 8
        assert_eq!(Result::<u64, u32>::wit_size(), 16);
        assert_eq!(Result::<u64, u32>::wit_alignment(), 8);

        // Result<(), String>: disc(1) + 3 pad + max(0,8)=8 = 12, align 4
        assert_eq!(Result::<(), String>::wit_size(), 12);
        assert_eq!(Result::<(), String>::wit_alignment(), 4);
    }

    #[test]
    fn flags_layout() {
        // 0 flags: size 0, align 1
        assert_eq!(WitFlags::<0>::wit_size(), 0);
        assert_eq!(WitFlags::<0>::wit_alignment(), 1);

        // 1 flag: u8
        assert_eq!(WitFlags::<1>::wit_size(), 1);
        assert_eq!(WitFlags::<1>::wit_alignment(), 1);

        // 8 flags: u8
        assert_eq!(WitFlags::<8>::wit_size(), 1);
        assert_eq!(WitFlags::<8>::wit_alignment(), 1);

        // 9 flags: u16
        assert_eq!(WitFlags::<9>::wit_size(), 2);
        assert_eq!(WitFlags::<9>::wit_alignment(), 2);

        // 16 flags: u16
        assert_eq!(WitFlags::<16>::wit_size(), 2);
        assert_eq!(WitFlags::<16>::wit_alignment(), 2);

        // 17 flags: u32
        assert_eq!(WitFlags::<17>::wit_size(), 4);
        assert_eq!(WitFlags::<17>::wit_alignment(), 4);

        // 33 flags: 2 x u32 = 8
        assert_eq!(WitFlags::<33>::wit_size(), 8);
        assert_eq!(WitFlags::<33>::wit_alignment(), 4);
    }

    #[test]
    fn option_layout() {
        // Option<u8>: disc(1) + payload(1) = 2, alignment 1
        assert_eq!(Option::<u8>::wit_size(), 2);
        assert_eq!(Option::<u8>::wit_alignment(), 1);

        // Option<u32>: disc(1) + 3 pad + payload(4) = 8, alignment 4
        assert_eq!(Option::<u32>::wit_size(), 8);
        assert_eq!(Option::<u32>::wit_alignment(), 4);

        // Option<u64>: disc(1) + 7 pad + payload(8) = 16, alignment 8
        assert_eq!(Option::<u64>::wit_size(), 16);
        assert_eq!(Option::<u64>::wit_alignment(), 8);

        // Option<String>: disc(1) + 3 pad + payload(8) = 12, alignment 4
        assert_eq!(Option::<String>::wit_size(), 12);
        assert_eq!(Option::<String>::wit_alignment(), 4);
    }

    #[test]
    fn tuple_layouts() {
        // (u32, u64): u32 at 0, u64 at 8 (aligned), total 16
        assert_eq!(<(u32, u64)>::wit_alignment(), 8);
        assert_eq!(<(u32, u64)>::wit_size(), 16);

        // (u8, u8): 2 bytes, alignment 1
        assert_eq!(<(u8, u8)>::wit_alignment(), 1);
        assert_eq!(<(u8, u8)>::wit_size(), 2);

        // (u8, u32): u8 at 0, pad 3, u32 at 4, total 8
        assert_eq!(<(u8, u32)>::wit_alignment(), 4);
        assert_eq!(<(u8, u32)>::wit_size(), 8);
    }

    #[test]
    fn struct_layout_computation() {
        // Simulate: struct { a: u8, b: u32, c: u16 }
        let fields = vec![
            (u8::wit_alignment(), u8::wit_size()),    // (1, 1)
            (u32::wit_alignment(), u32::wit_size()),   // (4, 4)
            (u16::wit_alignment(), u16::wit_size()),   // (2, 2)
        ];
        let (locs, total, max_align) = compute_struct_layout(&fields);

        assert_eq!(max_align, 4);
        assert_eq!(locs[0].offset, 0); // u8 at 0
        assert_eq!(locs[1].offset, 4); // u32 at 4 (padded from 1)
        assert_eq!(locs[2].offset, 8); // u16 at 8
        assert_eq!(total, 12);         // 10 rounded up to alignment 4
    }
}

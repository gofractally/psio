//! WIT Canonical ABI serialization (packing).
//!
//! Converts Rust values into the Canonical ABI wire format:
//! - Scalars are written at their natural alignment within a fixed-layout record
//! - Strings/lists are stored as `(offset: u32, len: u32)` inline, with the
//!   actual data appended to a trailing section
//! - Option<T> uses a 1-byte discriminant + aligned payload
//!
//! Two-pass approach (matching the C++ implementation):
//! 1. Compute total buffer size (`wit_packed_size`)
//! 2. Allocate once, write into fixed buffer

use super::layout::{align_up, WitLayout};

/// Trait for types that can be serialized to WIT Canonical ABI format.
pub trait WitPack: WitLayout {
    /// Serialize this value to WIT Canonical ABI bytes.
    fn wit_pack(&self) -> Vec<u8> {
        let total = self.wit_packed_size();
        let mut buf = vec![0u8; total as usize];
        let mut packer = WitPacker::new(&mut buf);
        let root = packer.alloc(Self::wit_alignment(), Self::wit_size());
        self.wit_store(&mut packer, root);
        buf
    }

    /// Compute total packed buffer size (root record + all variable-length data).
    fn wit_packed_size(&self) -> u32 {
        let mut bump = Self::wit_size();
        self.wit_accumulate_size(&mut bump);
        bump
    }

    /// Accumulate variable-length data sizes into `bump`.
    /// Scalars do nothing; strings/vecs/etc. add their out-of-line data.
    fn wit_accumulate_size(&self, bump: &mut u32);

    /// Store this value into the buffer at the given offset.
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32);
}

/// Bump-allocating writer into a pre-sized buffer.
pub struct WitPacker<'a> {
    buf: &'a mut [u8],
    bump: u32,
}

impl<'a> WitPacker<'a> {
    pub fn new(buf: &'a mut [u8]) -> Self {
        Self { buf, bump: 0 }
    }

    /// Allocate `size` bytes at `alignment` from the bump pointer.
    /// Returns the offset where data should be written.
    pub fn alloc(&mut self, alignment: u32, size: u32) -> u32 {
        self.bump = align_up(self.bump, alignment);
        let ptr = self.bump;
        self.bump += size;
        ptr
    }

    pub fn store_u8(&mut self, off: u32, v: u8) {
        self.buf[off as usize] = v;
    }

    pub fn store_u16(&mut self, off: u32, v: u16) {
        let bytes = v.to_le_bytes();
        self.buf[off as usize..off as usize + 2].copy_from_slice(&bytes);
    }

    pub fn store_u32(&mut self, off: u32, v: u32) {
        let bytes = v.to_le_bytes();
        self.buf[off as usize..off as usize + 4].copy_from_slice(&bytes);
    }

    pub fn store_u64(&mut self, off: u32, v: u64) {
        let bytes = v.to_le_bytes();
        self.buf[off as usize..off as usize + 8].copy_from_slice(&bytes);
    }

    pub fn store_f32(&mut self, off: u32, v: f32) {
        let bytes = v.to_le_bytes();
        self.buf[off as usize..off as usize + 4].copy_from_slice(&bytes);
    }

    pub fn store_f64(&mut self, off: u32, v: f64) {
        let bytes = v.to_le_bytes();
        self.buf[off as usize..off as usize + 8].copy_from_slice(&bytes);
    }

    pub fn store_bytes(&mut self, off: u32, data: &[u8]) {
        if !data.is_empty() {
            self.buf[off as usize..off as usize + data.len()].copy_from_slice(data);
        }
    }
}

// ── Scalar implementations ──────────────────────────────────────────────────

impl WitPack for bool {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u8(dest, if *self { 1 } else { 0 });
    }
}

impl WitPack for u8 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u8(dest, *self);
    }
}

impl WitPack for i8 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u8(dest, *self as u8);
    }
}

impl WitPack for u16 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u16(dest, *self);
    }
}

impl WitPack for i16 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u16(dest, *self as u16);
    }
}

impl WitPack for u32 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u32(dest, *self);
    }
}

impl WitPack for i32 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u32(dest, *self as u32);
    }
}

impl WitPack for u64 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u64(dest, *self);
    }
}

impl WitPack for i64 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u64(dest, *self as u64);
    }
}

impl WitPack for f32 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_f32(dest, *self);
    }
}

impl WitPack for f64 {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_f64(dest, *self);
    }
}

// ── char ────────────────────────────────────────────────────────────────────

impl WitPack for char {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        packer.store_u32(dest, *self as u32);
    }
}

// ── String ──────────────────────────────────────────────────────────────────

impl WitPack for String {
    fn wit_accumulate_size(&self, bump: &mut u32) {
        // String data is appended with alignment 1
        *bump += self.len() as u32;
    }

    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        let len = self.len() as u32;
        let ptr = packer.alloc(1, len);
        packer.store_bytes(ptr, self.as_bytes());
        packer.store_u32(dest, ptr);
        packer.store_u32(dest + 4, len);
    }
}

// ── Vec<T> ──────────────────────────────────────────────────────────────────

impl<T: WitPack> WitPack for Vec<T> {
    fn wit_accumulate_size(&self, bump: &mut u32) {
        let ea = T::wit_alignment();
        let es = T::wit_size();
        let count = self.len() as u32;
        *bump = align_up(*bump, ea);
        *bump += count * es;
        // Recurse for nested variable-length data
        for elem in self.iter() {
            elem.wit_accumulate_size(bump);
        }
    }

    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        let ea = T::wit_alignment();
        let es = T::wit_size();
        let count = self.len() as u32;
        let arr = packer.alloc(ea, count * es);
        for (i, elem) in self.iter().enumerate() {
            elem.wit_store(packer, arr + i as u32 * es);
        }
        packer.store_u32(dest, arr);
        packer.store_u32(dest + 4, count);
    }
}

// ── Option<T> ───────────────────────────────────────────────────────────────

impl<T: WitPack> WitPack for Option<T> {
    fn wit_accumulate_size(&self, bump: &mut u32) {
        if let Some(val) = self {
            val.wit_accumulate_size(bump);
        }
    }

    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        let ea = T::wit_alignment();
        let payload_offset = align_up(1, ea);
        match self {
            Some(val) => {
                packer.store_u8(dest, 1);
                val.wit_store(packer, dest + payload_offset);
            }
            None => {
                packer.store_u8(dest, 0);
            }
        }
    }
}

// ── Result<T, E> ───────────────────────────────────────────────────────────

impl<T: WitPack, E: WitPack> WitPack for Result<T, E> {
    fn wit_accumulate_size(&self, bump: &mut u32) {
        match self {
            Ok(val) => val.wit_accumulate_size(bump),
            Err(val) => val.wit_accumulate_size(bump),
        }
    }

    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        let max_case_align = std::cmp::max(T::wit_alignment(), E::wit_alignment());
        let payload_offset = align_up(1, max_case_align);
        match self {
            Ok(val) => {
                packer.store_u8(dest, 0); // ok = case 0
                val.wit_store(packer, dest + payload_offset);
            }
            Err(val) => {
                packer.store_u8(dest, 1); // err = case 1
                val.wit_store(packer, dest + payload_offset);
            }
        }
    }
}

// ── WitFlags ───────────────────────────────────────────────────────────────

impl<const N: usize> WitPack for super::layout::WitFlags<N> {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}

    fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
        let bits = self.as_bits();
        match N {
            0 => {}
            1..=8 => {
                packer.store_u8(dest, bits.first().copied().unwrap_or(0) as u8);
            }
            9..=16 => {
                packer.store_u16(dest, bits.first().copied().unwrap_or(0) as u16);
            }
            _ => {
                for (i, chunk) in bits.iter().enumerate() {
                    packer.store_u32(dest + i as u32 * 4, *chunk);
                }
            }
        }
    }
}

// ── Tuples ──────────────────────────────────────────────────────────────────

impl WitPack for () {
    fn wit_accumulate_size(&self, _bump: &mut u32) {}
    fn wit_store(&self, _packer: &mut WitPacker<'_>, _dest: u32) {}
}

macro_rules! impl_wit_pack_tuple {
    ($(($T:ident, $idx:tt)),+) => {
        impl<$($T: WitPack),+> WitPack for ($($T,)+) {
            fn wit_accumulate_size(&self, bump: &mut u32) {
                $(self.$idx.wit_accumulate_size(bump);)+
            }

            fn wit_store(&self, packer: &mut WitPacker<'_>, dest: u32) {
                // Compute field offsets
                let fields: &[(u32, u32)] = &[$(($T::wit_alignment(), $T::wit_size())),+];
                let (locs, _total, _align) = super::layout::compute_struct_layout(fields);
                let mut _i = 0;
                $(
                    self.$idx.wit_store(packer, dest + locs[_i].offset);
                    _i += 1;
                )+
            }
        }
    };
}

impl_wit_pack_tuple!((A, 0));
impl_wit_pack_tuple!((A, 0), (B, 1));
impl_wit_pack_tuple!((A, 0), (B, 1), (C, 2));
impl_wit_pack_tuple!((A, 0), (B, 1), (C, 2), (D, 3));
impl_wit_pack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4));
impl_wit_pack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (F, 5));
impl_wit_pack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (F, 5), (G, 6));
impl_wit_pack_tuple!((A, 0), (B, 1), (C, 2), (D, 3), (E, 4), (F, 5), (G, 6), (H, 7));

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pack_bool() {
        assert_eq!(true.wit_pack(), vec![1]);
        assert_eq!(false.wit_pack(), vec![0]);
    }

    #[test]
    fn pack_u32() {
        let val: u32 = 0x12345678;
        let buf = val.wit_pack();
        assert_eq!(buf, vec![0x78, 0x56, 0x34, 0x12]);
    }

    #[test]
    fn pack_u64() {
        let val: u64 = 0x0102030405060708;
        let buf = val.wit_pack();
        assert_eq!(buf, 0x0102030405060708u64.to_le_bytes().to_vec());
    }

    #[test]
    fn pack_f32() {
        let val: f32 = 3.14;
        let buf = val.wit_pack();
        assert_eq!(buf, 3.14f32.to_le_bytes().to_vec());
    }

    #[test]
    fn pack_string() {
        let val = String::from("hello");
        let buf = val.wit_pack();
        // Root record: 8 bytes (ptr=8, len=5)
        // Then string data "hello" at offset 8
        assert_eq!(buf.len(), 13);
        // ptr field (offset 0): should point to 8
        assert_eq!(u32::from_le_bytes(buf[0..4].try_into().unwrap()), 8);
        // len field (offset 4): should be 5
        assert_eq!(u32::from_le_bytes(buf[4..8].try_into().unwrap()), 5);
        // string data at offset 8
        assert_eq!(&buf[8..13], b"hello");
    }

    #[test]
    fn pack_vec_u32() {
        let val: Vec<u32> = vec![1, 2, 3];
        let buf = val.wit_pack();
        // Root: 8 bytes (ptr, len)
        // Array: 3 * 4 = 12 bytes at offset 8 (alignment 4)
        assert_eq!(buf.len(), 20);
        // ptr = 8
        assert_eq!(u32::from_le_bytes(buf[0..4].try_into().unwrap()), 8);
        // len = 3
        assert_eq!(u32::from_le_bytes(buf[4..8].try_into().unwrap()), 3);
        // elements
        assert_eq!(u32::from_le_bytes(buf[8..12].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(buf[12..16].try_into().unwrap()), 2);
        assert_eq!(u32::from_le_bytes(buf[16..20].try_into().unwrap()), 3);
    }

    #[test]
    fn pack_option_none() {
        let val: Option<u32> = None;
        let buf = val.wit_pack();
        // disc(1) + pad(3) + payload(4) = 8
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 0); // discriminant = 0
    }

    #[test]
    fn pack_option_some() {
        let val: Option<u32> = Some(42);
        let buf = val.wit_pack();
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 1); // discriminant = 1
        // payload at offset 4
        assert_eq!(u32::from_le_bytes(buf[4..8].try_into().unwrap()), 42);
    }

    #[test]
    fn pack_tuple() {
        let val: (u8, u32) = (0xFF, 42);
        let buf = val.wit_pack();
        // u8 at 0, pad 3, u32 at 4, total 8
        assert_eq!(buf.len(), 8);
        assert_eq!(buf[0], 0xFF);
        assert_eq!(u32::from_le_bytes(buf[4..8].try_into().unwrap()), 42);
    }
}

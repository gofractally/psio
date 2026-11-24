//! Reading FlatBuffers bytes without knowing their type, and without
//! trusting them.
//!
//! [`FbTable`] and [`FbVector`] are cursors into a buffer. Every offset,
//! vtable entry and length is compared with the buffer before it is used;
//! what does not fit reads as absent — a default, an empty string, an empty
//! vector. Nothing here panics or reads outside the bytes it was given,
//! whatever they are. The typed views ([`FbView`](struct@super::FbView)) and the
//! decoder are written on top of these.

/// `N` bytes at `pos`, when the buffer has them.
#[inline]
pub(crate) fn bytes_at<const N: usize>(buf: &[u8], pos: usize) -> Option<[u8; N]> {
    let end = pos.checked_add(N)?;
    buf.get(pos..end)?.try_into().ok()
}

#[inline]
pub(crate) fn u16_at(buf: &[u8], pos: usize) -> Option<u16> {
    bytes_at(buf, pos).map(u16::from_le_bytes)
}

#[inline]
pub(crate) fn u32_at(buf: &[u8], pos: usize) -> Option<u32> {
    bytes_at(buf, pos).map(u32::from_le_bytes)
}

/// The position the offset stored at `cell` points to: `None` when the cell
/// or four bytes at its target are outside the buffer, or the offset is null.
#[inline]
pub fn follow(buf: &[u8], cell: usize) -> Option<usize> {
    let offset = u32_at(buf, cell)? as usize;
    if offset == 0 {
        return None;
    }
    let target = cell.checked_add(offset)?;
    (target.checked_add(4)? <= buf.len()).then_some(target)
}

/// The bytes of the string or byte vector at `pos` (its length prefix).
#[inline]
pub(crate) fn string_bytes(buf: &[u8], pos: usize) -> Option<&[u8]> {
    let len = u32_at(buf, pos)? as usize;
    let start = pos.checked_add(4)?;
    buf.get(start..start.checked_add(len)?)
}

/// A position no buffer has: what an absent table or struct is viewed at,
/// so that every read of it is a read of a default.
pub(crate) const NOWHERE: usize = usize::MAX;

/// A table somewhere in a buffer, or no table (every field then reads as
/// absent). Fields are found by vtable slot.
#[derive(Clone, Copy)]
pub struct FbTable<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> FbTable<'a> {
    /// The root table of `buf`.
    #[inline]
    pub fn root(buf: &'a [u8]) -> Self {
        FbTable { buf, pos: follow(buf, 0).unwrap_or(NOWHERE) }
    }

    /// The table at `pos`.
    #[inline]
    pub fn at(buf: &'a [u8], pos: usize) -> Self {
        FbTable { buf, pos }
    }

    /// The table the offset at `cell` points to.
    #[inline]
    pub fn follow(buf: &'a [u8], cell: usize) -> Self {
        FbTable { buf, pos: follow(buf, cell).unwrap_or(NOWHERE) }
    }

    /// A table with no fields.
    #[inline]
    pub fn absent(buf: &'a [u8]) -> Self {
        FbTable { buf, pos: NOWHERE }
    }

    /// The whole buffer.
    #[inline]
    pub fn buffer(&self) -> &'a [u8] {
        self.buf
    }

    /// The position of the table in the buffer.
    #[inline]
    pub fn position(&self) -> usize {
        self.pos
    }

    /// Where the field of vtable slot `slot` is stored, when the table has
    /// it and `size` bytes there are inside the buffer.
    #[inline]
    pub fn cell(&self, slot: u16, size: usize) -> Option<usize> {
        let buf = self.buf;
        let soffset = i32::from_le_bytes(bytes_at(buf, self.pos)?);
        let vtable = usize::try_from(self.pos as i64 - soffset as i64).ok()?;
        let vsize = u16_at(buf, vtable)? as usize;
        let entry = 4 + 2 * slot as usize;
        if entry + 2 > vsize {
            return None;
        }
        let offset = u16_at(buf, vtable.checked_add(entry)?)? as usize;
        if offset == 0 {
            return None;
        }
        let cell = self.pos.checked_add(offset)?;
        (cell.checked_add(size)? <= buf.len()).then_some(cell)
    }

    /// Whether the table stores the field of `slot`.
    pub fn has_field(&self, slot: u16) -> bool {
        self.cell(slot, 0).is_some()
    }

    /// A string field: `None` when absent, out of bounds or not UTF-8.
    pub fn read_str(&self, slot: u16) -> Option<&'a str> {
        let target = follow(self.buf, self.cell(slot, 4)?)?;
        std::str::from_utf8(string_bytes(self.buf, target)?).ok()
    }

    /// A string field; empty when absent.
    pub fn read_str_or_empty(&self, slot: u16) -> &'a str {
        self.read_str(slot).unwrap_or("")
    }

    /// A nested table field.
    pub fn read_table(&self, slot: u16) -> Option<FbTable<'a>> {
        let target = follow(self.buf, self.cell(slot, 4)?)?;
        Some(FbTable { buf: self.buf, pos: target })
    }

    /// A vector field. The element size is not known here: each read of an
    /// element checks its own bounds.
    pub fn read_vec(&self, slot: u16) -> Option<FbVector<'a>> {
        let target = follow(self.buf, self.cell(slot, 4)?)?;
        let count = u32_at(self.buf, target)?;
        Some(FbVector { buf: self.buf, data_pos: target + 4, count })
    }

    /// A bool field; `def` when absent.
    pub fn read_bool(&self, slot: u16, def: bool) -> bool {
        match self.cell(slot, 1) {
            Some(cell) => self.buf[cell] != 0,
            None => def,
        }
    }
}

macro_rules! table_scalar_readers {
    ($($name:ident: $ty:ty),* $(,)?) => {
        impl<'a> FbTable<'a> {$(
            /// A scalar field; `def` when absent.
            pub fn $name(&self, slot: u16, def: $ty) -> $ty {
                self.cell(slot, std::mem::size_of::<$ty>())
                    .and_then(|cell| bytes_at(self.buf, cell))
                    .map_or(def, <$ty>::from_le_bytes)
            }
        )*}
    };
}
table_scalar_readers!(
    read_u8: u8, read_i8: i8, read_u16: u16, read_i16: i16, read_u32: u32, read_i32: i32,
    read_u64: u64, read_i64: i64, read_f32: f32, read_f64: f64,
);

impl std::fmt::Debug for FbTable<'_> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FbTable").field("pos", &self.pos).finish()
    }
}

/// A vector somewhere in a buffer, of elements whose type the caller knows.
/// An element that is not inside the buffer reads as zero / absent.
#[derive(Clone, Copy, Debug)]
pub struct FbVector<'a> {
    buf: &'a [u8],
    data_pos: usize,
    count: u32,
}

impl<'a> FbVector<'a> {
    /// Number of elements the vector says it has.
    pub fn len(&self) -> u32 {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    fn element(&self, i: u32, size: usize) -> Option<usize> {
        if i >= self.count {
            return None;
        }
        self.data_pos.checked_add((i as usize).checked_mul(size)?)
    }

    pub fn get_bool(&self, i: u32) -> bool {
        self.get_u8(i) != 0
    }

    /// A string element: `None` when out of bounds or not UTF-8.
    pub fn get_str(&self, i: u32) -> Option<&'a str> {
        let target = follow(self.buf, self.element(i, 4)?)?;
        std::str::from_utf8(string_bytes(self.buf, target)?).ok()
    }

    /// A table element.
    pub fn get_table(&self, i: u32) -> Option<FbTable<'a>> {
        let target = follow(self.buf, self.element(i, 4)?)?;
        Some(FbTable { buf: self.buf, pos: target })
    }
}

macro_rules! vector_scalar_readers {
    ($($name:ident: $ty:ty),* $(,)?) => {
        impl<'a> FbVector<'a> {$(
            /// A scalar element; zero when it is not inside the buffer.
            pub fn $name(&self, i: u32) -> $ty {
                self.element(i, std::mem::size_of::<$ty>())
                    .and_then(|pos| bytes_at(self.buf, pos))
                    .map_or(<$ty>::default(), <$ty>::from_le_bytes)
            }
        )*}
    };
}
vector_scalar_readers!(
    get_u8: u8, get_i8: i8, get_u16: u16, get_i16: i16, get_u32: u32, get_i32: i32,
    get_u64: u64, get_i64: i64, get_f32: f32, get_f64: f64,
);

#[cfg(test)]
mod tests {
    use super::*;
    use crate::flatbuf::builder::FbBuilder;

    #[test]
    fn scalar_string_and_absent_fields() {
        let mut b = FbBuilder::default();
        let s = b.create_string("hello");
        b.start_table();
        b.add_scalar_u8(4, 7, 0);
        b.add_scalar_u16(6, 1000, 0);
        b.add_scalar_u32(8, 42, 0);
        b.add_offset_field(10, s);
        let root = b.end_table();
        b.finish(root);

        let t = FbTable::root(b.data());
        assert_eq!(t.read_u8(0, 0), 7);
        assert_eq!(t.read_u16(1, 0), 1000);
        assert_eq!(t.read_u32(2, 0), 42);
        assert_eq!(t.read_str(3), Some("hello"));
        assert_eq!(t.read_u32(4, 777), 777);
        assert_eq!(t.read_str(4), None);
        assert!(t.has_field(3) && !t.has_field(4));
    }

    #[test]
    fn any_bytes_read_as_absent() {
        for len in 0..40usize {
            for fill in [0u8, 1, 4, 0x7f, 0xff] {
                let buf = vec![fill; len];
                let t = FbTable::root(&buf);
                for slot in 0..8 {
                    let _ = t.read_u64(slot, 0);
                    let _ = t.read_str(slot);
                    if let Some(v) = t.read_vec(slot) {
                        let _ = (v.get_u64(0), v.get_str(v.len().saturating_sub(1)), v.get_table(0));
                    }
                    if let Some(sub) = t.read_table(slot) {
                        let _ = sub.read_bool(0, false);
                    }
                }
            }
        }
    }
}

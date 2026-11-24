//! The FlatBuffers verifier: the checks of C++ `psio::validate_flatbuf<T>`
//! (`psio/flatbuf.hpp`), in the same order and with the same messages, so
//! that both languages give the same answer for the same bytes.
//!
//! The verifier is typed — it follows everything a `T` can reach — and safe
//! on arbitrary bytes: every read is bounds-checked, nesting is limited
//! ([`FbLimits::max_depth`]) and so are the number of tables visited
//! ([`FbLimits::max_tables`]) and the bytes of strings and vectors reached
//! ([`FbLimits::max_expansion`]). Offsets are unsigned and point forward, so
//! a buffer cannot contain a cycle; what a hostile buffer can do is reach
//! one sub-object along very many paths, which the budgets bound: the work
//! of the verifier and the size of what the buffer decodes to are both
//! limited by the size of the buffer.

/// What a buffer is refused for, and where.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FbError {
    message: &'static str,
    offset: u32,
}

impl FbError {
    pub(crate) const fn new(message: &'static str, offset: u64) -> Self {
        FbError { message, offset: if offset > u32::MAX as u64 { u32::MAX } else { offset as u32 } }
    }
    /// The reason: the text C++ `codec_status::error().what` has for the
    /// same buffer.
    pub fn message(&self) -> &'static str {
        self.message
    }
    /// The position in the buffer the check failed at.
    pub fn offset(&self) -> u32 {
        self.offset
    }
}

impl std::fmt::Display for FbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} (at byte {})", self.message, self.offset)
    }
}

impl std::error::Error for FbError {}

/// The limits the verifier applies; the defaults are the official
/// verifier's (C++ `psio::flatbuf::limits`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FbLimits {
    /// Tables nested inside tables.
    pub max_depth: u32,
    /// Tables visited in total (a table reachable twice counts twice).
    pub max_tables: u32,
    /// The bytes of the strings and of the vectors of scalars / structs
    /// reached (each with its 4-byte length), one reachable twice counted
    /// twice, as a multiple of the buffer's size (on top of
    /// [`PAYLOAD_ALLOWANCE`], which every buffer has): what a decode
    /// copies. The bytes read to compare string keys of a set / map (the
    /// shorter key of each neighbouring pair) are charged to the same
    /// budget, so the verifier's work is bounded by the buffer too. A
    /// buffer in which nothing is shared needs 3; the default leaves room
    /// for the strings an official builder shares (`CreateSharedString`).
    pub max_expansion: u32,
}

impl Default for FbLimits {
    fn default() -> Self {
        FbLimits { max_depth: 64, max_tables: 1_000_000, max_expansion: 64 }
    }
}

/// The payload budget of every buffer, before [`FbLimits::max_expansion`]
/// times its size: one mebibyte.
pub const PAYLOAD_ALLOWANCE: u64 = 1 << 20;

/// A file identifier and / or a size prefix (C++ `psio::flatbuf::options`):
/// what [`to_flatbuf_with`](super::to_flatbuf_with) writes and
/// [`validate_flatbuf_with`](super::validate_flatbuf_with) requires.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct FbOptions {
    /// The four characters after the root offset.
    pub file_identifier: Option<[u8; 4]>,
    /// The buffer starts with its size (a stream of buffers).
    pub size_prefixed: bool,
}

/// The largest buffer the format allows (offsets are 32 bits, and a signed
/// offset must reach every vtable).
pub const MAX_BUFFER_SIZE: usize = 0x7fff_ffff;

/// The state of one verification. Public for the code the derives write.
pub struct FbChecker<'a> {
    buf: &'a [u8],
    size: u64,
    limits: FbLimits,
    depth: u32,
    tables: u32,
    /// What is left of the payload budget.
    payload: u64,
    error: Option<FbError>,
    utf8_scanned: u64,
    utf8_seen: Option<Utf8Seen>,
}

/// The byte ranges already known to be UTF-8. A buffer may point at one
/// string many times, or at strings that overlap; with this every byte is
/// scanned once however often it is reached.
#[derive(Default)]
struct Utf8Seen {
    // start → end, disjoint.
    ranges: std::collections::BTreeMap<usize, usize>,
}

impl Utf8Seen {
    fn check(&mut self, buf: &[u8], start: usize, end: usize) -> bool {
        if start >= end {
            return true;
        }
        // In UTF-8 a position is a character boundary exactly when the byte
        // there is not a continuation byte.
        let boundary = |p: usize| buf.get(p).map_or(true, |b| b & 0xC0 != 0x80);
        let (mut pos, mut low, mut high) = (start, start, end);
        if let Some((&s, &e)) = self.ranges.range(..=start).next_back() {
            if start < e {
                if !boundary(start) {
                    return false;
                }
                if end <= e {
                    return end == e || boundary(end);
                }
                (pos, low) = (e, s);
            }
        }
        while pos < end {
            let next = self.ranges.range(pos..).next().map(|(&s, &e)| (s, e));
            let gap_end = match next {
                Some((s, _)) if s < end => s,
                _ => end,
            };
            if std::str::from_utf8(&buf[pos..gap_end]).is_err() {
                return false;
            }
            pos = gap_end;
            if let Some((s, e)) = next {
                if s < end {
                    if end < e && !boundary(end) {
                        return false;
                    }
                    pos = e;
                    high = high.max(e);
                }
            }
        }
        let absorbed: Vec<usize> = self.ranges.range(low..high).map(|(&s, _)| s).collect();
        for s in absorbed {
            self.ranges.remove(&s);
        }
        self.ranges.insert(low, high);
        true
    }
}

/// A table header that has passed its checks.
#[derive(Clone, Copy)]
pub struct FbCheckedTable {
    pos: u64,
    vtable: u64,
    vsize: u32,
    object: u32,
}

impl FbCheckedTable {
    /// The position of the table.
    pub fn position(&self) -> u64 {
        self.pos
    }
}

impl<'a> FbChecker<'a> {
    pub(crate) fn new(buf: &'a [u8], limits: FbLimits) -> Self {
        FbChecker {
            buf,
            size: buf.len() as u64,
            limits,
            depth: 0,
            tables: 0,
            payload: PAYLOAD_ALLOWANCE + buf.len() as u64 * limits.max_expansion as u64,
            error: None,
            utf8_scanned: 0,
            utf8_seen: None,
        }
    }

    pub(crate) fn into_error(self) -> FbError {
        self.error.unwrap_or(FbError::new("flatbuf: invalid buffer", 0))
    }

    /// The buffer under verification.
    #[inline]
    pub fn buffer(&self) -> &'a [u8] {
        self.buf
    }

    /// Records the first failure. Returns `false`, for `return c.fail(..)`.
    #[cold]
    pub fn fail(&mut self, message: &'static str, pos: u64) -> bool {
        if self.error.is_none() {
            self.error = Some(FbError::new(message, pos));
        }
        false
    }

    /// Takes `n` bytes from the payload budget.
    #[inline]
    fn charge(&mut self, n: u64, message: &'static str, pos: u64) -> bool {
        if n > self.payload {
            return self.fail(message, pos);
        }
        self.payload -= n;
        true
    }

    /// Charges the bytes read to compare two neighbouring string keys of
    /// the set / map at `pos` (the length of the shorter key). Keys that
    /// share their bytes — one long string in many sets — cannot make the
    /// verifier read more than the budget.
    #[inline]
    pub fn charge_key_bytes(&mut self, n: u64, pos: u64) -> bool {
        n == 0 || self.charge(n, "flatbuf: too many key bytes compared", pos)
    }

    /// Whether `n` bytes at `pos` are inside the buffer.
    #[inline]
    pub fn has(&self, pos: u64, n: u64) -> bool {
        pos <= self.size && n <= self.size - pos
    }

    // Reads of bytes a `has` has covered; zero if a caller forgot, never a
    // panic.
    #[inline]
    fn u8(&self, pos: u64) -> u8 {
        self.buf.get(pos as usize).copied().unwrap_or(0)
    }
    #[inline]
    fn u16(&self, pos: u64) -> u16 {
        super::raw::u16_at(self.buf, pos as usize).unwrap_or(0)
    }
    #[inline]
    fn u32(&self, pos: u64) -> u32 {
        super::raw::u32_at(self.buf, pos as usize).unwrap_or(0)
    }

    /// The byte at `pos` (zero outside the buffer).
    #[inline]
    pub fn byte(&self, pos: u64) -> u8 {
        self.u8(pos)
    }

    /// The position the offset at `cell` (4 readable bytes) points to, or 0
    /// after recording an error.
    pub fn check_offset(&mut self, cell: u64) -> u64 {
        if cell & 3 != 0 {
            self.fail("flatbuf: misaligned offset", cell);
            return 0;
        }
        let o = self.u32(cell) as u64;
        if o == 0 {
            self.fail("flatbuf: null offset", cell);
            return 0;
        }
        if o > MAX_BUFFER_SIZE as u64 || !self.has(cell + o, 4) {
            self.fail("flatbuf: offset out of bounds", cell);
            return 0;
        }
        cell + o
    }

    /// The target of the offset stored in the field or element at `pos`, or
    /// 0 after recording an error.
    pub fn check_offset_cell(&mut self, pos: u64) -> u64 {
        if !self.has(pos, 4) {
            self.fail("flatbuf: field out of bounds", pos);
            return 0;
        }
        self.check_offset(pos)
    }

    /// A scalar or struct of `size` bytes aligned to `align` at `pos`.
    pub fn check_inline(&mut self, pos: u64, size: u64, align: u64) -> bool {
        if pos & (align - 1) != 0 {
            return self.fail("flatbuf: misaligned field", pos);
        }
        if !self.has(pos, size) {
            return self.fail("flatbuf: field out of bounds", pos);
        }
        true
    }

    /// The string at `pos` (4 readable bytes): bounds and the terminator.
    pub fn check_string(&mut self, pos: u64) -> bool {
        if pos & 3 != 0 {
            return self.fail("flatbuf: misaligned string", pos);
        }
        let len = self.u32(pos) as u64;
        if !self.has(pos + 4, len + 1) {
            return self.fail("flatbuf: string out of bounds", pos);
        }
        if self.u8(pos + 4 + len) != 0 {
            return self.fail("flatbuf: string is not NUL-terminated", pos);
        }
        self.charge(4 + len, "flatbuf: too many bytes referenced", pos)
    }

    /// The verified string at `pos` is UTF-8: what a Rust `String` requires
    /// (C++ `std::string` does not, and the C++ verifier does not check it).
    /// Linear in the buffer however often a string is pointed at.
    pub fn check_utf8(&mut self, pos: u64) -> bool {
        let start = pos as usize + 4;
        let end = start + self.u32(pos) as usize;
        let Some(bytes) = self.buf.get(start..end) else {
            return self.fail("flatbuf: string out of bounds", pos);
        };
        let buf = self.buf;
        let valid = match self.utf8_seen.as_mut() {
            Some(seen) => seen.check(buf, start, end),
            None => {
                // Strings that do not share bytes total at most the buffer;
                // once more than that has been scanned, remember the ranges.
                self.utf8_scanned += bytes.len() as u64;
                if self.utf8_scanned > self.size {
                    self.utf8_seen = Some(Utf8Seen::default());
                }
                std::str::from_utf8(bytes).is_ok()
            }
        };
        valid || self.fail("flatbuf: string is not valid UTF-8", pos)
    }

    /// The element count of the vector at `pos` (4 readable bytes); `None`
    /// after recording an error. `leaf`: the elements are scalars or
    /// structs, the vector's own payload (the tables and strings that
    /// offsets lead to are budgeted where they are reached).
    pub fn check_vector(&mut self, pos: u64, elem_size: u64, elem_align: u64, leaf: bool) -> Option<u64> {
        if pos & 3 != 0 {
            self.fail("flatbuf: misaligned vector", pos);
            return None;
        }
        let count = self.u32(pos) as u64;
        if !self.has(pos + 4, count * elem_size) {
            self.fail("flatbuf: vector out of bounds", pos);
            return None;
        }
        if count != 0 && (pos + 4) & (elem_align - 1) != 0 {
            self.fail("flatbuf: misaligned vector elements", pos);
            return None;
        }
        if leaf && !self.charge(4 + count * elem_size, "flatbuf: too many bytes referenced", pos) {
            return None;
        }
        Some(count)
    }

    /// The header of the table at `pos`: the limits, its position and
    /// alignment, its vtable, its size. On success the table is entered;
    /// [`end_table`](Self::end_table) leaves it.
    pub fn begin_table(&mut self, pos: u64) -> Option<FbCheckedTable> {
        if self.depth >= self.limits.max_depth {
            self.fail("flatbuf: tables nested too deeply", pos);
            return None;
        }
        if self.tables >= self.limits.max_tables {
            self.fail("flatbuf: too many tables", pos);
            return None;
        }
        self.tables += 1;
        if pos & 3 != 0 {
            self.fail("flatbuf: misaligned table", pos);
            return None;
        }
        if !self.has(pos, 4) {
            self.fail("flatbuf: table out of bounds", pos);
            return None;
        }
        let vpos = pos as i64 - (self.u32(pos) as i32) as i64;
        if vpos < 0 || vpos & 1 != 0 || !self.has(vpos as u64, 4) {
            self.fail("flatbuf: vtable out of bounds", pos);
            return None;
        }
        let vtable = vpos as u64;
        let vsize = self.u16(vtable) as u32;
        let object = self.u16(vtable + 2) as u32;
        if vsize < 4 || vsize & 1 != 0 || !self.has(vtable, vsize as u64) {
            self.fail("flatbuf: bad vtable size", vtable);
            return None;
        }
        if object < 4 || !self.has(pos, object as u64) {
            self.fail("flatbuf: bad table size", vtable);
            return None;
        }
        self.depth += 1;
        Some(FbCheckedTable { pos, vtable, vsize, object })
    }

    /// Leaves a table entered by [`begin_table`](Self::begin_table).
    #[inline]
    pub fn end_table(&mut self) {
        self.depth -= 1;
    }

    /// The position of the field of vtable slot `slot` occupying `size`
    /// bytes: `Ok(0)` when absent, `Err` after recording an error.
    pub fn checked_field(&mut self, t: &FbCheckedTable, slot: u16, size: u32) -> Result<u64, ()> {
        let vt = 4 + 2 * slot as u32;
        if vt + 2 > t.vsize {
            return Ok(0);
        }
        let off = self.u16(t.vtable + vt as u64) as u32;
        if off == 0 {
            return Ok(0);
        }
        if off < 4 || off + size > t.object {
            self.fail("flatbuf: field outside its table", t.vtable + vt as u64);
            return Err(());
        }
        Ok(t.pos + off as u64)
    }
}

/// Checks a whole buffer whose root is checked by `root`.
pub(crate) fn check_buffer(
    bytes: &[u8],
    opts: &FbOptions,
    limits: FbLimits,
    root: impl FnOnce(&mut FbChecker<'_>, u64) -> bool,
) -> Result<(), FbError> {
    let mut c = FbChecker::new(bytes, limits);
    if bytes.len() > MAX_BUFFER_SIZE {
        return Err(FbError::new("flatbuf: buffer larger than 2 GiB", 0));
    }
    let start: u64 = if opts.size_prefixed { 4 } else { 0 };
    if !c.has(0, start + 4) {
        return Err(FbError::new("flatbuf: buffer too small", 0));
    }
    if opts.size_prefixed && c.u32(0) as u64 != bytes.len() as u64 - 4 {
        return Err(FbError::new("flatbuf: size prefix does not match the buffer", 0));
    }
    if let Some(id) = opts.file_identifier {
        if !c.has(start, 8) || bytes[start as usize + 4..start as usize + 8] != id {
            return Err(FbError::new("flatbuf: wrong file identifier", start + 4));
        }
    }
    let at = c.check_offset(start);
    if at == 0 || !root(&mut c, at) {
        return Err(c.into_error());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The range tracker gives the answer of a full scan, whatever was
    /// checked before and however the ranges overlap.
    #[test]
    fn remembered_utf8_ranges_answer_as_a_full_scan_does() {
        let mut state = 0x2545f4914f6cdd1du64;
        let mut next = move |n: usize| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            (state % n as u64) as usize
        };
        // Text with one-, two-, three- and four-byte characters, and a few
        // bytes that are never valid.
        let pieces: [&[u8]; 8] = [b"a", b"\0", "é".as_bytes(), "€".as_bytes(), "😀".as_bytes(), b"\xff", b"\x80", b"zz"];
        let (mut checked, mut valid) = (0, 0);
        for _ in 0..300 {
            let mut buf = Vec::new();
            while buf.len() < 40 + next(80) {
                // Mostly text; an invalid byte now and then.
                let piece = if next(12) == 0 { 5 + next(2) } else { [0, 1, 2, 3, 4, 7][next(6)] };
                buf.extend_from_slice(pieces[piece]);
            }
            let mut seen = Utf8Seen::default();
            for _ in 0..200 {
                let start = next(buf.len());
                let end = start + next(buf.len() - start + 1);
                let expected = std::str::from_utf8(&buf[start..end]).is_ok();
                assert_eq!(seen.check(&buf, start, end), expected, "{buf:?} {start}..{end} after {:?}", seen.ranges);
                checked += 1;
                valid += expected as usize;
                // The ranges stay disjoint and are all valid text.
                let mut last = 0;
                for (&s, &e) in &seen.ranges {
                    assert!(s >= last && s < e && std::str::from_utf8(&buf[s..e]).is_ok());
                    last = e;
                }
            }
        }
        assert!(valid > 1000 && checked - valid > 1000, "{valid} of {checked}");
    }
}

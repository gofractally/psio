//! Low-level Cap'n Proto wire format helpers.
//!
//! Reads little-endian values, resolves struct/list/text pointers,
//! and performs bounds checking. These are the building blocks for
//! both `CapnpView` and `capnp_unpack`.

/// Parsed representation of a Cap'n Proto struct pointer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CapnpPtr {
    /// Byte offset of the data section start within the segment.
    pub data_offset: usize,
    /// Number of 64-bit words in the data section.
    pub data_words: u16,
    /// Number of pointer slots.
    pub ptr_count: u16,
}

impl CapnpPtr {
    /// A null pointer (no data).
    pub const NULL: CapnpPtr = CapnpPtr {
        data_offset: 0,
        data_words: 0,
        ptr_count: 0,
    };

    pub fn is_null(&self) -> bool {
        self.data_words == 0 && self.ptr_count == 0
    }

    /// Byte offset of the pointer section start.
    pub fn ptr_section_offset(&self) -> usize {
        self.data_offset + self.data_words as usize * 8
    }

    /// Byte offset of pointer slot `idx`.
    pub fn ptr_slot_offset(&self, idx: u32) -> usize {
        self.ptr_section_offset() + idx as usize * 8
    }
}

/// Information about a resolved list pointer.
#[derive(Debug, Clone, Copy)]
pub struct ListInfo {
    /// Byte offset of the list data within the segment.
    pub data_offset: usize,
    /// Number of elements.
    pub count: u32,
    /// Bytes per element (for non-composite lists).
    pub elem_stride: u32,
    /// For composite lists: per-element data words.
    pub elem_data_words: u16,
    /// For composite lists: per-element pointer count.
    pub elem_ptr_count: u16,
}

/// Read a little-endian u64 from a byte slice.
#[inline(always)]
pub fn read_u64(data: &[u8], offset: usize) -> u64 {
    let bytes: [u8; 8] = data[offset..offset + 8].try_into().unwrap();
    u64::from_le_bytes(bytes)
}

/// Read a little-endian u32 from a byte slice.
#[inline(always)]
pub fn read_u32(data: &[u8], offset: usize) -> u32 {
    let bytes: [u8; 4] = data[offset..offset + 4].try_into().unwrap();
    u32::from_le_bytes(bytes)
}

/// Read a little-endian u16 from a byte slice.
#[inline(always)]
pub fn read_u16(data: &[u8], offset: usize) -> u16 {
    let bytes: [u8; 2] = data[offset..offset + 2].try_into().unwrap();
    u16::from_le_bytes(bytes)
}

/// Extract signed 30-bit offset from a struct or list pointer word.
/// Pointer word layout: bits[0:1]=tag, bits[2:31]=offset (signed 30-bit).
#[inline(always)]
pub fn ptr_offset(word: u64) -> i32 {
    ((word as u32 & !3u32) as i32) >> 2
}

/// Resolve a struct pointer at `ptr_byte_offset` within `segment`.
/// Returns `None` if the pointer is null (all zeros).
pub fn resolve_struct_ptr(segment: &[u8], ptr_byte_offset: usize) -> Option<CapnpPtr> {
    if ptr_byte_offset + 8 > segment.len() {
        return None;
    }
    let word = read_u64(segment, ptr_byte_offset);
    if word == 0 {
        return None;
    }
    let off = ptr_offset(word);
    let dw = ((word >> 32) & 0xFFFF) as u16;
    let pc = ((word >> 48) & 0xFFFF) as u16;
    let data_offset = (ptr_byte_offset as i64 + 8 + off as i64 * 8) as usize;
    Some(CapnpPtr {
        data_offset,
        data_words: dw,
        ptr_count: pc,
    })
}

/// Read a Text (string) from a pointer slot at `ptr_byte_offset` within `segment`.
/// Returns an empty string if the pointer is null.
pub fn read_text<'a>(segment: &'a [u8], ptr_byte_offset: usize) -> &'a str {
    if ptr_byte_offset + 8 > segment.len() {
        return "";
    }
    let word = read_u64(segment, ptr_byte_offset);
    if word == 0 {
        return "";
    }
    let off = ptr_offset(word);
    let count = (word >> 35) as u32;
    let data_offset = (ptr_byte_offset as i64 + 8 + off as i64 * 8) as usize;
    if count == 0 {
        return "";
    }
    let text_len = count as usize - 1; // exclude NUL
    if data_offset + text_len > segment.len() {
        return "";
    }
    std::str::from_utf8(&segment[data_offset..data_offset + text_len]).unwrap_or("")
}

/// Resolve a list pointer at `ptr_byte_offset` within `segment`.
pub fn resolve_list_ptr(segment: &[u8], ptr_byte_offset: usize) -> Option<ListInfo> {
    if ptr_byte_offset + 8 > segment.len() {
        return None;
    }
    let word = read_u64(segment, ptr_byte_offset);
    if word == 0 {
        return None;
    }
    let off = ptr_offset(word);
    let elem_sz_tag = ((word >> 32) & 7) as u8;
    let count_or_words = (word >> 35) as u32;
    let list_start = (ptr_byte_offset as i64 + 8 + off as i64 * 8) as usize;

    if elem_sz_tag == 7 {
        // Composite list: first word is a tag describing per-element layout
        if list_start + 8 > segment.len() {
            return None;
        }
        let tag_word = read_u64(segment, list_start);
        let count = (tag_word as u32) >> 2;
        let dw = ((tag_word >> 32) & 0xFFFF) as u16;
        let pc = ((tag_word >> 48) & 0xFFFF) as u16;
        Some(ListInfo {
            data_offset: list_start + 8,
            count,
            elem_stride: (dw as u32 + pc as u32) * 8,
            elem_data_words: dw,
            elem_ptr_count: pc,
        })
    } else {
        const ELEM_SIZES: [u32; 8] = [0, 0, 1, 2, 4, 8, 8, 0];
        Some(ListInfo {
            data_offset: list_start,
            count: count_or_words,
            elem_stride: ELEM_SIZES[elem_sz_tag as usize],
            elem_data_words: 0,
            elem_ptr_count: 0,
        })
    }
}

/// Parse the segment table and return (segment_start_offset, segment_word_count).
/// Only single-segment messages are supported.
pub fn parse_segment_table(msg: &[u8]) -> Result<(usize, u32), &'static str> {
    if msg.len() < 8 {
        return Err("message too short for segment table");
    }
    let seg_count_m1 = read_u32(msg, 0);
    if seg_count_m1 != 0 {
        return Err("only single-segment messages supported");
    }
    let seg_words = read_u32(msg, 4);
    let table_bytes: usize = 8; // 4 bytes seg_count + 4 bytes seg_size, padded to 8
    if msg.len() < table_bytes + seg_words as usize * 8 {
        return Err("message truncated");
    }
    Ok((table_bytes, seg_words))
}

/// Resolve the root struct pointer from a flat-array message.
pub fn resolve_root(msg: &[u8]) -> Result<(&[u8], CapnpPtr), &'static str> {
    let (seg_start, _seg_words) = parse_segment_table(msg)?;
    let segment = &msg[seg_start..];
    match resolve_struct_ptr(segment, 0) {
        Some(ptr) => Ok((segment, ptr)),
        None => Err("null root pointer"),
    }
}

/// Validate a Cap'n Proto flat-array message.
/// Checks that all pointers stay within segment bounds and enforces a
/// traversal limit to detect cycles/amplification.
pub fn validate(msg: &[u8]) -> bool {
    let (seg_start, seg_words) = match parse_segment_table(msg) {
        Ok(v) => v,
        Err(_) => return false,
    };
    if seg_words == 0 {
        return false;
    }
    let segment = &msg[seg_start..seg_start + seg_words as usize * 8];
    let mut words_left: i64 = seg_words as i64 * 8;
    match resolve_struct_ptr(segment, 0) {
        Some(root) => validate_struct(segment, root, &mut words_left, 0),
        None => false,
    }
}

fn validate_struct(
    segment: &[u8],
    p: CapnpPtr,
    words_left: &mut i64,
    depth: i32,
) -> bool {
    if depth > 64 {
        return false;
    }
    let struct_words = p.data_words as u32 + p.ptr_count as u32;
    *words_left -= struct_words as i64;
    if *words_left < 0 {
        return false;
    }

    let data_end = p.data_offset + p.data_words as usize * 8;
    if p.data_offset > segment.len() || data_end > segment.len() {
        return false;
    }

    let ps = p.ptr_section_offset();
    let ps_end = ps + p.ptr_count as usize * 8;
    if ps > segment.len() || ps_end > segment.len() {
        return false;
    }

    for i in 0..p.ptr_count as u32 {
        let slot = ps + i as usize * 8;
        let word = read_u64(segment, slot);
        if word == 0 {
            continue;
        }

        let tag = word & 3;
        if tag == 0 {
            // Struct pointer
            if let Some(child) = resolve_struct_ptr(segment, slot) {
                if !validate_struct(segment, child, words_left, depth + 1) {
                    return false;
                }
            }
        } else if tag == 1 {
            // List pointer
            if let Some(info) = resolve_list_ptr(segment, slot) {
                let total_bytes = info.count as usize * info.elem_stride as usize;
                let list_end = info.data_offset + total_bytes;
                if info.data_offset > segment.len() || list_end > segment.len() {
                    return false;
                }
                let list_words = ((total_bytes + 7) / 8) as i64;
                *words_left -= list_words;
                if *words_left < 0 {
                    return false;
                }

                if info.elem_data_words > 0 || info.elem_ptr_count > 0 {
                    for j in 0..info.count {
                        let elem = CapnpPtr {
                            data_offset: info.data_offset + j as usize * info.elem_stride as usize,
                            data_words: info.elem_data_words,
                            ptr_count: info.elem_ptr_count,
                        };
                        if !validate_struct(segment, elem, words_left, depth + 1) {
                            return false;
                        }
                    }
                }
            }
        } else {
            return false; // far pointers not supported
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_ptr_offset() {
        // Positive offset
        let word: u64 = 4u64; // offset = 1 (bits 2..31 = 1, shifted left 2)
        assert_eq!(ptr_offset(word), 1);

        // Zero offset
        assert_eq!(ptr_offset(0u64), 0);
    }

    #[test]
    fn test_resolve_struct_ptr_null() {
        let segment = [0u8; 16];
        assert!(resolve_struct_ptr(&segment, 0).is_none());
    }
}

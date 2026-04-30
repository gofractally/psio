//! Cap'n Proto struct layout computation.
//!
//! Ports the `capnp_layout<T>` bit-packing algorithm from C++.
//! Each struct has:
//! - `data_words`: number of 64-bit words in the data section (inline scalars)
//! - `ptr_count`: number of pointer slots (strings, lists, sub-structs)
//!
//! Each field gets a [`FieldLoc`] describing where it lives in the struct.

/// Describes a Cap'n Proto field type for layout purposes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldKind {
    /// Boolean — packed as a single bit in the data section.
    Bool,
    /// Scalar of N bytes (1, 2, 4, or 8) — packed in the data section.
    Scalar(u32),
    /// Pointer type (string, list, sub-struct) — goes in pointer section.
    Pointer,
    /// Void (used for monostate variant alternatives) — occupies no space.
    Void,
}

/// Location of a single field (or discriminant, or variant alternative)
/// within a Cap'n Proto struct.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FieldLoc {
    /// True if this field lives in the pointer section.
    pub is_ptr: bool,
    /// For data fields: byte offset within the data section.
    /// For pointer fields: index in the pointer section.
    pub offset: u32,
    /// For booleans: bit index within the byte (0-7).
    pub bit_index: u8,
}

impl Default for FieldLoc {
    fn default() -> Self {
        FieldLoc {
            is_ptr: false,
            offset: 0,
            bit_index: 0,
        }
    }
}

/// Description of a variant (union) field: discriminant location + per-alternative locations.
#[derive(Debug, Clone)]
pub struct VariantLayout {
    /// Location of the u16 discriminant in the data section.
    pub disc_loc: FieldLoc,
    /// Location of each alternative's value.
    pub alt_locs: Vec<FieldLoc>,
}

/// Description of a single struct member for layout computation.
#[derive(Debug, Clone)]
pub enum MemberDesc {
    /// A simple (non-variant) field.
    Simple(FieldKind),
    /// A variant (union) field with the given alternative kinds.
    Variant(Vec<FieldKind>),
}

/// Computed layout for a Cap'n Proto struct.
#[derive(Debug, Clone)]
pub struct CapnpLayout {
    /// Number of 64-bit words in the data section.
    pub data_words: u16,
    /// Number of pointer slots.
    pub ptr_count: u16,
    /// For simple fields: the field location. For variant fields: the discriminant location.
    pub fields: Vec<FieldLoc>,
    /// Variant layouts (indexed by field index). None for non-variant fields.
    pub variants: Vec<Option<VariantLayout>>,
}

impl CapnpLayout {
    /// Compute the layout from a list of member descriptors.
    ///
    /// This implements the same algorithm as the C++ `capnp_layout<T>::compute()`:
    /// 1. For each field (in declaration order), allocate ordinals:
    ///    - Simple fields get one ordinal
    ///    - Variant fields get N ordinals (one per alternative)
    /// 2. After all ordinals, allocate discriminants for variant fields (u16 each)
    ///
    /// Allocation uses a bit-level occupancy bitmap. Scalars are naturally aligned
    /// (e.g., u32 at 32-bit boundaries), and booleans are packed individually.
    pub fn compute(members: &[MemberDesc]) -> Self {
        let mut data_words: u16 = 0;
        let mut ptr_count: u16 = 0;
        // Bit-level occupancy for data section (max 2048 bits = 32 words)
        let mut occupied = [false; 2048];

        let mut fields: Vec<FieldLoc> = Vec::with_capacity(members.len());
        let mut variants: Vec<Option<VariantLayout>> = Vec::with_capacity(members.len());

        // Allocate bits in the data section with given alignment.
        // Returns the starting bit index.
        let alloc_bits =
            |bit_count: u32,
             bit_align: u32,
             occupied: &mut [bool; 2048],
             data_words: &mut u16| -> u32 {
                let mut bit: u32 = 0;
                loop {
                    let end_bit = bit + bit_count;
                    let words_needed = ((end_bit + 63) / 64) as u16;
                    if words_needed > *data_words {
                        *data_words = words_needed;
                    }

                    let mut ok = true;
                    for b in bit..end_bit {
                        if (b as usize) < occupied.len() && occupied[b as usize] {
                            ok = false;
                            break;
                        }
                    }
                    if ok {
                        for b in bit..end_bit {
                            if (b as usize) < occupied.len() {
                                occupied[b as usize] = true;
                            }
                        }
                        return bit;
                    }
                    bit += bit_align;
                }
            };

        // Allocate a slot for a single type kind.
        let alloc_type_slot =
            |kind: &FieldKind,
             occupied: &mut [bool; 2048],
             data_words: &mut u16,
             ptr_count: &mut u16| -> FieldLoc {
                match kind {
                    FieldKind::Void => FieldLoc::default(),
                    FieldKind::Pointer => {
                        let idx = *ptr_count;
                        *ptr_count += 1;
                        FieldLoc {
                            is_ptr: true,
                            offset: idx as u32,
                            bit_index: 0,
                        }
                    }
                    FieldKind::Bool => {
                        let bit = alloc_bits(1, 1, occupied, data_words);
                        FieldLoc {
                            is_ptr: false,
                            offset: bit / 8,
                            bit_index: (bit % 8) as u8,
                        }
                    }
                    FieldKind::Scalar(sz) => {
                        let bit = alloc_bits(sz * 8, sz * 8, occupied, data_words);
                        FieldLoc {
                            is_ptr: false,
                            offset: bit / 8,
                            bit_index: 0,
                        }
                    }
                }
            };

        // Phase 1: Allocate ordinals for all fields.
        for member in members {
            match member {
                MemberDesc::Simple(kind) => {
                    let loc =
                        alloc_type_slot(kind, &mut occupied, &mut data_words, &mut ptr_count);
                    fields.push(loc);
                    variants.push(None);
                }
                MemberDesc::Variant(alts) => {
                    // Allocate one ordinal per alternative
                    let mut alt_locs = Vec::with_capacity(alts.len());
                    for alt_kind in alts {
                        let loc = alloc_type_slot(
                            alt_kind,
                            &mut occupied,
                            &mut data_words,
                            &mut ptr_count,
                        );
                        alt_locs.push(loc);
                    }
                    // Placeholder for discriminant — filled in phase 2
                    fields.push(FieldLoc::default());
                    variants.push(Some(VariantLayout {
                        disc_loc: FieldLoc::default(),
                        alt_locs,
                    }));
                }
            }
        }

        // Phase 2: Allocate discriminants for variant fields (u16, after all ordinals).
        for (i, variant) in variants.iter_mut().enumerate() {
            if let Some(ref mut vl) = variant {
                let bit = alloc_bits(16, 16, &mut occupied, &mut data_words);
                let disc_loc = FieldLoc {
                    is_ptr: false,
                    offset: bit / 8,
                    bit_index: 0,
                };
                vl.disc_loc = disc_loc;
                fields[i] = disc_loc;
            }
        }

        CapnpLayout {
            data_words,
            ptr_count,
            fields,
            variants,
        }
    }
}

/// Helper: return the `FieldKind` for a Rust primitive type.
///
/// This is used when manually building `MemberDesc` lists.
pub fn kind_of_bool() -> FieldKind {
    FieldKind::Bool
}
pub fn kind_of_u8() -> FieldKind {
    FieldKind::Scalar(1)
}
pub fn kind_of_i8() -> FieldKind {
    FieldKind::Scalar(1)
}
pub fn kind_of_u16() -> FieldKind {
    FieldKind::Scalar(2)
}
pub fn kind_of_i16() -> FieldKind {
    FieldKind::Scalar(2)
}
pub fn kind_of_u32() -> FieldKind {
    FieldKind::Scalar(4)
}
pub fn kind_of_i32() -> FieldKind {
    FieldKind::Scalar(4)
}
pub fn kind_of_f32() -> FieldKind {
    FieldKind::Scalar(4)
}
pub fn kind_of_u64() -> FieldKind {
    FieldKind::Scalar(8)
}
pub fn kind_of_i64() -> FieldKind {
    FieldKind::Scalar(8)
}
pub fn kind_of_f64() -> FieldKind {
    FieldKind::Scalar(8)
}
pub fn kind_of_string() -> FieldKind {
    FieldKind::Pointer
}
pub fn kind_of_list() -> FieldKind {
    FieldKind::Pointer
}
pub fn kind_of_struct() -> FieldKind {
    FieldKind::Pointer
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_simple_scalars() {
        // Struct with: u32, u64, bool
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Scalar(4)), // u32
            MemberDesc::Simple(FieldKind::Scalar(8)), // u64
            MemberDesc::Simple(FieldKind::Bool),      // bool
        ]);

        // u32 goes at byte 0 (bit 0), u64 at byte 8 (bit 64, second word),
        // bool at bit 32 (in the gap after u32 in first word).
        assert_eq!(layout.fields[0].offset, 0); // u32 at byte 0
        assert_eq!(layout.fields[0].is_ptr, false);
        assert_eq!(layout.fields[1].offset, 8); // u64 at byte 8
        assert_eq!(layout.fields[1].is_ptr, false);
        assert_eq!(layout.fields[2].offset, 4); // bool at bit 32 => byte 4
        assert_eq!(layout.fields[2].bit_index, 0);
        assert_eq!(layout.data_words, 2);
        assert_eq!(layout.ptr_count, 0);
    }

    #[test]
    fn test_pointer_fields() {
        // Struct with: u32, String, Vec<u8>
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Scalar(4)), // u32
            MemberDesc::Simple(FieldKind::Pointer),   // String
            MemberDesc::Simple(FieldKind::Pointer),   // Vec<u8>
        ]);

        assert_eq!(layout.fields[0].offset, 0);
        assert_eq!(layout.fields[0].is_ptr, false);
        assert_eq!(layout.fields[1].offset, 0); // first pointer
        assert_eq!(layout.fields[1].is_ptr, true);
        assert_eq!(layout.fields[2].offset, 1); // second pointer
        assert_eq!(layout.fields[2].is_ptr, true);
        assert_eq!(layout.data_words, 1);
        assert_eq!(layout.ptr_count, 2);
    }

    #[test]
    fn test_bool_packing() {
        // 9 booleans should pack into 2 bytes in 1 word
        let members: Vec<MemberDesc> = (0..9).map(|_| MemberDesc::Simple(FieldKind::Bool)).collect();
        let layout = CapnpLayout::compute(&members);
        assert_eq!(layout.data_words, 1);

        // Bools should be at bits 0..8
        for i in 0..9 {
            assert_eq!(layout.fields[i].offset, i as u32 / 8);
            assert_eq!(layout.fields[i].bit_index, (i % 8) as u8);
        }
    }

    #[test]
    fn test_variant_layout() {
        // Struct with: variant(u32, String, Void)
        let layout = CapnpLayout::compute(&[MemberDesc::Variant(vec![
            FieldKind::Scalar(4), // u32 alternative
            FieldKind::Pointer,   // String alternative
            FieldKind::Void,      // Void alternative
        ])]);

        let vl = layout.variants[0].as_ref().unwrap();
        // u32 alt at byte 0
        assert_eq!(vl.alt_locs[0].is_ptr, false);
        assert_eq!(vl.alt_locs[0].offset, 0);
        // String alt as pointer 0
        assert_eq!(vl.alt_locs[1].is_ptr, true);
        assert_eq!(vl.alt_locs[1].offset, 0);
        // Void alt has default loc
        assert_eq!(vl.alt_locs[2], FieldLoc::default());

        // Discriminant is u16, allocated after the ordinals
        // u32 took bits 0-31, so discriminant at bits 32-47 => byte 4
        assert_eq!(vl.disc_loc.offset, 4);
        assert_eq!(vl.disc_loc.is_ptr, false);

        assert_eq!(layout.data_words, 1);
        assert_eq!(layout.ptr_count, 1);
    }

    #[test]
    fn test_field_packing_order() {
        // Verify that smaller fields fill gaps left by larger fields.
        // Fields: u64, u16, u8, u8
        // u64 occupies word 0 entirely (bits 0-63).
        // u16 starts at bit 64 (byte 8 in word 1).
        // u8 at bit 80 (byte 10).
        // u8 at bit 88 (byte 11).
        let layout = CapnpLayout::compute(&[
            MemberDesc::Simple(FieldKind::Scalar(8)),
            MemberDesc::Simple(FieldKind::Scalar(2)),
            MemberDesc::Simple(FieldKind::Scalar(1)),
            MemberDesc::Simple(FieldKind::Scalar(1)),
        ]);

        assert_eq!(layout.fields[0].offset, 0); // u64 at byte 0
        assert_eq!(layout.fields[1].offset, 8); // u16 at byte 8
        assert_eq!(layout.fields[2].offset, 10); // u8 at byte 10
        assert_eq!(layout.fields[3].offset, 11); // u8 at byte 11
        assert_eq!(layout.data_words, 2);
    }
}

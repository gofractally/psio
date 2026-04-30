//! Vtable layout computation for FlatBuffers wire format.
//!
//! FlatBuffers tables have a vtable that maps field indices to byte offsets
//! within the table data. Each regular field consumes one vtable slot (2 bytes).
//! Variant/union fields consume two slots: a u8 discriminant + an offset to the
//! value table.
//!
//! The vtable header is:
//! - `vtable_size: u16` — total vtable size in bytes
//! - `table_size: u16`  — size of the table data in bytes
//! - `field_offsets: [u16; N]` — one per vtable slot
//!
//! A field offset of 0 means "absent" (use default value).

/// Describes the location of a single field within a FlatBuffer vtable.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FbFieldLoc {
    /// The vtable slot index for this field.
    /// For variant fields, this is the discriminant slot; the value is at slot+1.
    pub slot: u16,
    /// Number of vtable slots this field consumes (1 for normal, 2 for variant/union).
    pub width: u16,
}

impl FbFieldLoc {
    /// The byte offset into the vtable for this field's primary slot.
    /// Vtable layout: [vt_size:u16, tbl_size:u16, slots...]
    /// So slot 0 starts at byte 4.
    #[inline]
    pub fn vt_offset(&self) -> u16 {
        4 + 2 * self.slot
    }

    /// For variant/union fields, the vtable byte offset for the value slot.
    #[inline]
    pub fn vt_value_offset(&self) -> u16 {
        debug_assert_eq!(self.width, 2, "vt_value_offset only valid for variant fields");
        4 + 2 * (self.slot + 1)
    }
}

/// Layout information for a FlatBuffer table type.
///
/// Computed from struct field metadata at runtime. When derive macros are added,
/// this will be computed at compile time via const evaluation.
#[derive(Debug, Clone)]
pub struct FbLayout {
    /// One entry per struct field, in declaration order.
    pub fields: Vec<FbFieldLoc>,
    /// Total number of vtable slots consumed by all fields.
    pub total_slots: u16,
}

impl FbLayout {
    /// Compute the layout for a table with the given field kinds.
    ///
    /// `is_variant` should be a slice of booleans, one per field, indicating
    /// whether that field is a variant/union type (which consumes 2 vtable slots).
    pub fn new(is_variant: &[bool]) -> Self {
        let mut fields = Vec::with_capacity(is_variant.len());
        let mut slot: u16 = 0;
        for &v in is_variant {
            let width = if v { 2 } else { 1 };
            fields.push(FbFieldLoc { slot, width });
            slot += width;
        }
        FbLayout {
            fields,
            total_slots: slot,
        }
    }

    /// Compute layout for a table where no fields are variants.
    pub fn simple(n: usize) -> Self {
        Self::new(&vec![false; n])
    }

    /// The byte size of the vtable for this layout.
    /// vtable = [vt_size:u16, tbl_size:u16, slot_offsets:u16 * total_slots]
    pub fn vtable_size(&self) -> u16 {
        4 + 2 * self.total_slots
    }

    /// Get the field location for field at `index`.
    pub fn field(&self, index: usize) -> &FbFieldLoc {
        &self.fields[index]
    }

    /// Number of fields.
    pub fn field_count(&self) -> usize {
        self.fields.len()
    }
}

/// Alignment of a scalar type in FlatBuffer wire format.
#[inline]
pub fn scalar_align(size: usize) -> usize {
    size
}

/// FlatBuffer field alignment category.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum FbFieldAlign {
    Align1 = 1,
    Align2 = 2,
    Align4 = 4,
    Align8 = 8,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn simple_layout() {
        let layout = FbLayout::simple(3);
        assert_eq!(layout.total_slots, 3);
        assert_eq!(layout.fields[0].slot, 0);
        assert_eq!(layout.fields[1].slot, 1);
        assert_eq!(layout.fields[2].slot, 2);
        assert_eq!(layout.fields[0].vt_offset(), 4);
        assert_eq!(layout.fields[1].vt_offset(), 6);
        assert_eq!(layout.fields[2].vt_offset(), 8);
        assert_eq!(layout.vtable_size(), 10); // 4 + 2*3
    }

    #[test]
    fn layout_with_variant() {
        // Fields: normal, variant, normal
        let layout = FbLayout::new(&[false, true, false]);
        assert_eq!(layout.total_slots, 4); // 1 + 2 + 1
        assert_eq!(layout.fields[0].slot, 0);
        assert_eq!(layout.fields[1].slot, 1);
        assert_eq!(layout.fields[1].width, 2);
        assert_eq!(layout.fields[2].slot, 3);
        assert_eq!(layout.fields[1].vt_offset(), 6);     // discriminant
        assert_eq!(layout.fields[1].vt_value_offset(), 8); // value offset
        assert_eq!(layout.fields[2].vt_offset(), 10);
    }
}

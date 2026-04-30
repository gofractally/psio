//! Declarative macro generating `PsszPack` / `PsszUnpack` for user structs.
//! Counterpart of C++'s `PSIO1_REFLECT(Struct, fields...)` on the pSSZ side.
//!
//! Unlike SSZ, pSSZ emits an extensibility header (`F::HEADER_BYTES` bytes
//! holding the fixed-region size) before the fixed region. A DWNC variant
//! skips the header and memcpy's the struct; that variant is a future
//! derive attribute (`pssz_struct_dwnc!`).

/// DWNC variant for pSSZ: all-fixed struct, skips the extensibility
/// header entirely. Mirrors C++'s `definitionWillNotChange()` on pSSZ.
#[macro_export]
macro_rules! pssz_struct_dwnc {
    ($Ty:ident { $($field:ident : $FTy:ty),+ $(,)? }) => {
        $crate::pssz_struct!(@view_dwnc $Ty { $($field : $FTy),+ });

        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszPack<F> for $Ty {
            const MIN_ENCODED_SIZE: usize = { 0 $( + <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE )+ };
            const MAX_ENCODED_SIZE: ::core::option::Option<usize> = Some({ 0 $( + <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE )+ });
            const IS_FIXED_SIZE: bool  = true;
            const FIXED_SIZE:    usize = { 0 $( + <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE )+ };
            fn pssz_size(&self) -> usize {
                <Self as $crate::pssz::PsszPack<F>>::FIXED_SIZE
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                // Whole-struct memcpy fast path (see ssz_struct_dwnc!
                // for full commentary). Gated on size_of == FIXED_SIZE
                // which holds iff the user applied #[repr(C, packed)].
                #[cfg(target_endian = "little")]
                if ::core::mem::size_of::<Self>() ==
                    <Self as $crate::pssz::PsszPack<F>>::FIXED_SIZE
                {
                    let bytes = unsafe {
                        ::core::slice::from_raw_parts(
                            self as *const Self as *const u8,
                            ::core::mem::size_of::<Self>())
                    };
                    out.extend_from_slice(bytes);
                    return;
                }
                // See ssz_struct_dwnc! for the rationale behind the
                // by-value copy — lets this compile under `#[repr(C,
                // packed)]`. DWNC fields are Copy by construction.
                $(
                    {
                        let __f: $FTy = self.$field;
                        <$FTy as $crate::pssz::PsszPack<F>>::pssz_pack(&__f, out);
                    }
                )+
            }
        }
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszUnpack<F> for $Ty {
            fn pssz_unpack(bytes: &[u8]) -> $crate::pssz::PsszResult<Self> {
                if bytes.len() < <Self as $crate::pssz::PsszPack<F>>::FIXED_SIZE {
                    return Err($crate::pssz::PsszError("pssz dwnc: underrun"));
                }
                // Whole-struct memcpy decode fast path.
                #[cfg(target_endian = "little")]
                if ::core::mem::size_of::<Self>() ==
                    <Self as $crate::pssz::PsszPack<F>>::FIXED_SIZE
                {
                    let fs = <Self as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                    let mut out: ::core::mem::MaybeUninit<Self> =
                        ::core::mem::MaybeUninit::uninit();
                    unsafe {
                        ::core::ptr::copy_nonoverlapping(
                            bytes.as_ptr(),
                            out.as_mut_ptr() as *mut u8,
                            fs);
                        return Ok(out.assume_init());
                    }
                }
                let mut pos: usize = 0;
                $(
                    let $field = {
                        let fs = <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                        let out = <$FTy as $crate::pssz::PsszUnpack<F>>::pssz_unpack(
                            &bytes[pos .. pos + fs])?;
                        pos += fs;
                        out
                    };
                )+
                let _ = pos;
                Ok($Ty { $($field),+ })
            }
        }
    };
}

/// Variant: extensible pSSZ struct (writes header + fixed region + tail).
#[macro_export]
macro_rules! pssz_struct {
    ($Ty:ident { $($field:ident : $FTy:ty),+ $(,)? }) => {
        $crate::pssz_struct!(@view $Ty { $($field : $FTy),+ });

        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszPack<F> for $Ty {
            const MIN_ENCODED_SIZE: usize = F::HEADER_BYTES;  // header ≥ 1
            const MAX_ENCODED_SIZE: ::core::option::Option<usize> = None;
            const IS_FIXED_SIZE: bool  = false;
            const FIXED_SIZE:    usize = 0;
            fn pssz_size(&self) -> usize {
                let mut total: usize = F::HEADER_BYTES;
                $(
                    total += if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE
                    } else { F::OFFSET_BYTES };
                )+
                $(
                    if !<$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        total += <$FTy as $crate::pssz::PsszPack<F>>::pssz_size(&self.$field);
                    }
                )+
                total
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                // Extensibility header (size of fixed region).
                let mut fixed_region_bytes: usize = 0;
                $(
                    fixed_region_bytes += if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE
                    } else { F::OFFSET_BYTES };
                )+
                // Emit header as F::HEADER_BYTES little-endian.
                let hdr = fixed_region_bytes as u32;
                match F::HEADER_BYTES {
                    1 => out.push(hdr as u8),
                    2 => out.extend_from_slice(&(hdr as u16).to_le_bytes()),
                    4 => out.extend_from_slice(&hdr.to_le_bytes()),
                    _ => unreachable!(),
                }
                let fixed_start = out.len();

                // Fixed region with offset placeholders.
                let mut slots: Vec<usize> = Vec::new();
                $(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        <$FTy as $crate::pssz::PsszPack<F>>::pssz_pack(&self.$field, out);
                    } else {
                        slots.push(out.len());
                        out.resize(out.len() + F::OFFSET_BYTES, 0);
                    }
                )+

                // Emit variable payloads + backpatch offsets.
                let mut var_idx: usize = 0;
                $(
                    if !<$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        let rel = (out.len() - fixed_start) as u32;
                        let slot_pos = slots[var_idx];
                        match F::OFFSET_BYTES {
                            1 => out[slot_pos] = rel as u8,
                            2 => out[slot_pos .. slot_pos + 2].copy_from_slice(&(rel as u16).to_le_bytes()),
                            4 => out[slot_pos .. slot_pos + 4].copy_from_slice(&rel.to_le_bytes()),
                            _ => unreachable!(),
                        }
                        <$FTy as $crate::pssz::PsszPack<F>>::pssz_pack(&self.$field, out);
                        var_idx += 1;
                    }
                )+
                let _ = var_idx;
            }
        }

        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszUnpack<F> for $Ty {
            fn pssz_unpack(bytes: &[u8]) -> $crate::pssz::PsszResult<Self> {
                if bytes.len() < F::HEADER_BYTES {
                    return Err($crate::pssz::PsszError("pssz struct: header underrun"));
                }
                let hdr_bytes_used = F::HEADER_BYTES;
                // Read header (we don't use it for trusted input; future:
                // extensibility-aware decoding can consult it).
                let _hdr: u32 = match F::HEADER_BYTES {
                    1 => bytes[0] as u32,
                    2 => u16::from_le_bytes([bytes[0], bytes[1]]) as u32,
                    4 => u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]),
                    _ => unreachable!(),
                };

                let data = &bytes[hdr_bytes_used..];
                let mut field_pos: usize = 0;
                let mut var_offsets: Vec<u32> = Vec::new();
                $(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        field_pos += <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                    } else {
                        if field_pos + F::OFFSET_BYTES > data.len() {
                            return Err($crate::pssz::PsszError("pssz struct: offset underrun"));
                        }
                        let off: u32 = match F::OFFSET_BYTES {
                            1 => data[field_pos] as u32,
                            2 => u16::from_le_bytes([data[field_pos], data[field_pos + 1]]) as u32,
                            4 => u32::from_le_bytes([
                                data[field_pos], data[field_pos + 1],
                                data[field_pos + 2], data[field_pos + 3],
                            ]),
                            _ => unreachable!(),
                        };
                        var_offsets.push(off);
                        field_pos += F::OFFSET_BYTES;
                    }
                    let _: &str = stringify!($field);
                )+

                let mut field_pos: usize = 0;
                let mut var_i: usize = 0;
                $(
                    let $field: $FTy = if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        let fs = <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                        let out = <$FTy as $crate::pssz::PsszUnpack<F>>::pssz_unpack(&data[field_pos .. field_pos + fs])?;
                        field_pos += fs;
                        out
                    } else {
                        let beg = var_offsets[var_i] as usize;
                        let stop = if var_i + 1 < var_offsets.len() {
                            var_offsets[var_i + 1] as usize
                        } else { data.len() };
                        var_i += 1;
                        field_pos += F::OFFSET_BYTES;
                        <$FTy as $crate::pssz::PsszUnpack<F>>::pssz_unpack(&data[beg .. stop])?
                    };
                )+
                let _ = field_pos;
                let _ = var_i;
                Ok($Ty { $($field),+ })
            }
        }
    };

    // DWNC view accessors via a generated trait (same orphan-rule
    // workaround as ssz_struct!). Users `use ${Ty}PsszAccessors;`.
    (@view_dwnc $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        paste::paste! {
            pub trait [<$Ty PsszAccessors>]<'a, F: $crate::pssz::PsszFormat> {
                $( fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F>; )+
            }
            impl<'a, F: $crate::pssz::PsszFormat> [<$Ty PsszAccessors>]<'a, F>
                for $crate::pssz_view::PsszView<'a, $Ty, F>
            {
                $(
                    fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F> {
                        let (beg, end) = [<__pssz_dwnc_span_ $Ty>]::<F>(
                            self.data, stringify!($field));
                        $crate::pssz_view::PsszView::new(&self.data[beg .. end])
                    }
                )+
            }

            fn [<__pssz_dwnc_span_ $Ty>]<F: $crate::pssz::PsszFormat>(
                data: &[u8], target: &str) -> (usize, usize)
            {
                let _ = data;
                let mut pos: usize = 0;
                $(
                    {
                        if stringify!($field) == target {
                            let fs = <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                            return (pos, pos + fs);
                        }
                        pos += <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                    }
                )+
                panic!("pssz dwnc view: field {} not found in {}",
                        target, stringify!($Ty));
            }
        }
    };

    // Extensible-struct view accessors via generated trait.
    (@view $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        paste::paste! {
            pub trait [<$Ty PsszAccessors>]<'a, F: $crate::pssz::PsszFormat> {
                $( fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F>; )+
            }
            impl<'a, F: $crate::pssz::PsszFormat> [<$Ty PsszAccessors>]<'a, F>
                for $crate::pssz_view::PsszView<'a, $Ty, F>
            {
                $(
                    fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F> {
                        let (beg, end) = [<__pssz_ext_span_ $Ty>]::<F>(
                            self.data, stringify!($field));
                        $crate::pssz_view::PsszView::new(&self.data[beg .. end])
                    }
                )+
            }

            fn [<__pssz_ext_span_ $Ty>]<F: $crate::pssz::PsszFormat>(
                data: &[u8], target: &str) -> (usize, usize)
            {
            let hdr = F::HEADER_BYTES;
            let body = &data[hdr..];
            let mut pos: usize = 0;
            let mut target_off: usize = 0;
            let mut found_var = false;
            $(
                {
                    let this = stringify!($field);
                    let is_fixed = <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE;
                    if this == target {
                        if is_fixed {
                            let fs = <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                            return (hdr + pos, hdr + pos + fs);
                        } else {
                            let ob = F::OFFSET_BYTES;
                            target_off = match ob {
                                1 => body[pos] as usize,
                                2 => u16::from_le_bytes([body[pos], body[pos+1]]) as usize,
                                4 => u32::from_le_bytes([
                                    body[pos], body[pos+1], body[pos+2], body[pos+3]]) as usize,
                                _ => unreachable!(),
                            };
                            found_var = true;
                        }
                    } else if found_var && !is_fixed {
                        let ob = F::OFFSET_BYTES;
                        let next_off = match ob {
                            1 => body[pos] as usize,
                            2 => u16::from_le_bytes([body[pos], body[pos+1]]) as usize,
                            4 => u32::from_le_bytes([
                                body[pos], body[pos+1], body[pos+2], body[pos+3]]) as usize,
                            _ => unreachable!(),
                        };
                        return (hdr + target_off, hdr + next_off);
                    }
                    if is_fixed {
                        pos += <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE;
                    } else {
                        pos += F::OFFSET_BYTES;
                    }
                }
            )+
                if found_var { return (hdr + target_off, data.len()); }
                panic!("pssz view: field {} not found in {}",
                        target, stringify!($Ty));
            }
        }
    };
}

#[cfg(test)]
mod tests {
    use crate::pssz::{Pssz32, PsszPack, PsszUnpack, from_pssz, to_pssz};

    #[derive(Debug, PartialEq, Eq)]
    struct Named { name: String, value: u32 }
    pssz_struct!(Named { name: String, value: u32 });

    #[test]
    fn named_round_trip() {
        let n = Named { name: "alice".into(), value: 77 };
        let b = to_pssz::<Pssz32, _>(&n);
        assert_eq!(from_pssz::<Pssz32, Named>(&b).unwrap(), n);
    }

    #[test]
    fn cpp_pssz32_named_cross() {
        // C++: Named{"alice", 77} → "08000000080000004d000000616c696365"
        // Header: 08000000 (fixed region is 8 bytes)
        // Fixed: 08000000 (offset to name=8) + 4d000000 (value=77)
        // Tail: "alice"
        let expected = hex("08000000080000004d000000616c696365");
        let n = Named { name: "alice".into(), value: 77 };
        assert_eq!(to_pssz::<Pssz32, _>(&n), expected);
        assert_eq!(from_pssz::<Pssz32, Named>(&expected).unwrap(), n);
    }

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i+2], 16).unwrap()).collect()
    }

    #[test]
    fn pssz_view_named_accessors() {
        use super::tests::NamedPsszAccessors;
        let n = Named { name: "alice".into(), value: 77 };
        let b = to_pssz::<Pssz32, _>(&n);
        let v: crate::pssz_view::PsszView<Named, Pssz32> =
            crate::pssz_view::pssz_view_of(&b);
        assert_eq!(v.name().view(), "alice");
        assert_eq!(v.value().get(), 77);
    }
}

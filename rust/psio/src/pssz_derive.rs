//! Declarative macro generating `PsszPack` / `PsszUnpack` for user structs.
//! Counterpart of C++'s `PSIO1_REFLECT(Struct, fields...)` on the pSSZ side.
//!
//! Unlike SSZ, pSSZ emits an extensibility header (`F::HEADER_BYTES` bytes
//! holding the fixed-region size) before the fixed region. A DWNC variant
//! skips the header. Both variants use typed field codecs, not Rust memory
//! layout assumptions; safe trait implementations do not establish POD validity.

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
                let spans = $crate::pssz::input::record_spans::<F, _>(bytes, [$(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        Some(<$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE)
                    } else { None },
                )+], true)?;
                let mut fields = spans.into_iter();
                $(
                    let $field = <$FTy as $crate::pssz::PsszUnpack<F>>::pssz_unpack(
                        &bytes[fields.next().expect("generated field span")])?;
                )+
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
                    total = total.checked_add(if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE
                    } else { F::OFFSET_BYTES }).expect("pssz: record size overflow");
                )+
                $(
                    if !<$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        total = total.checked_add(<$FTy as $crate::pssz::PsszPack<F>>::pssz_size(&self.$field)).expect("pssz: record size overflow");
                    }
                )+
                total
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                // Extensibility header (size of fixed region).
                let mut fixed_region_bytes: usize = 0;
                $(
                    fixed_region_bytes = fixed_region_bytes.checked_add(if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE
                    } else { F::OFFSET_BYTES }).expect("pssz: fixed region overflow");
                )+
                // Share checked explicit-width storage with sequence offsets.
                let header_start = out.len();
                out.resize(header_start.checked_add(F::HEADER_BYTES).expect("pssz: header size overflow"), 0);
                $crate::pssz::write_offset::<F>(&mut out[header_start..], fixed_region_bytes);
                let fixed_start = out.len();

                // Fixed region with offset placeholders.
                let mut slots: Vec<usize> = Vec::new();
                $(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        <$FTy as $crate::pssz::PsszPack<F>>::pssz_pack(&self.$field, out);
                    } else {
                        slots.push(out.len());
                        out.resize(out.len().checked_add(F::OFFSET_BYTES).expect("pssz: record slot overflow"), 0);
                    }
                )+

                // Emit variable payloads + backpatch offsets.
                let mut var_idx: usize = 0;
                $(
                    if !<$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        let rel = out.len() - fixed_start;
                        let slot_pos = slots[var_idx];
                        $crate::pssz::write_offset::<F>(&mut out[slot_pos..slot_pos + F::OFFSET_BYTES], rel);
                        <$FTy as $crate::pssz::PsszPack<F>>::pssz_pack(&self.$field, out);
                        var_idx += 1;
                    }
                )+
                let _ = var_idx;
            }
        }

        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszUnpack<F> for $Ty {
            fn pssz_unpack(bytes: &[u8]) -> $crate::pssz::PsszResult<Self> {
                let spans = $crate::pssz::input::record_spans::<F, _>(bytes, [$(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        Some(<$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE)
                    } else { None },
                )+], false)?;
                let mut fields = spans.into_iter();
                $(
                    let $field = <$FTy as $crate::pssz::PsszUnpack<F>>::pssz_unpack(
                        &bytes[fields.next().expect("generated field span")])?;
                )+
                Ok($Ty { $($field),+ })
            }
        }
    };

    // DWNC view accessors via a generated trait (same orphan-rule
    // workaround as ssz_struct!). Users `use ${Ty}PsszAccessors;`.
    (@view_dwnc $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        $crate::__paste::paste! {
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
                let spans = $crate::pssz::input::record_spans::<F, _>(data, [$(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        Some(<$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE)
                    } else { None },
                )+], true).expect("pssz dwnc view: malformed record");
                let mut spans = spans.into_iter();
                $(
                    {
                        let span = spans.next().expect("generated field span");
                        if stringify!($field) == target { return (span.start, span.end); }
                    }
                )+
                panic!("pssz dwnc view: field {} not found in {}",
                        target, stringify!($Ty));
            }
        }
    };

    // Extensible-struct view accessors via generated trait.
    (@view $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        $crate::__paste::paste! {
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
                let spans = $crate::pssz::input::record_spans::<F, _>(data, [$(
                    if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                        Some(<$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE)
                    } else { None },
                )+], false).expect("pssz view: malformed record");
                let mut spans = spans.into_iter();
                $(
                    {
                        let span = spans.next().expect("generated field span");
                        if stringify!($field) == target { return (span.start, span.end); }
                    }
                )+
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

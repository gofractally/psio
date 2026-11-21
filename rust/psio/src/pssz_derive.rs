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

        $crate::pssz_struct!(@impl $Ty { $($field : $FTy),+ }; true);
    };
}

/// Variant: extensible pSSZ struct (writes header + fixed region + tail).
#[macro_export]
macro_rules! pssz_struct {
    ($Ty:ident { $($field:ident : $FTy:ty),+ $(,)? }) => {
        $crate::pssz_struct!(@view $Ty { $($field : $FTy),+ });

        $crate::pssz_struct!(@impl $Ty { $($field : $FTy),+ }; false);
    };

    (@field $value:expr, $FTy:ty; true) => {{
        let value: $FTy = $value;
        $crate::pssz_checked::field::<F, $FTy>(&value)
    }};
    (@field $value:expr, $FTy:ty; false) => {
        $crate::pssz_checked::field::<F, $FTy>(&$value)
    };

    (@impl $Ty:ident { $($field:ident : $FTy:ty),+ }; $dwnc:tt) => {
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszPack<F> for $Ty {
            const MIN_ENCODED_SIZE: usize = if $dwnc { 0 } else { F::HEADER_BYTES };
            const MAX_ENCODED_SIZE: Option<usize> = None;
            const IS_FIXED_SIZE: bool = $dwnc $(&& <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE)+;
            const FIXED_SIZE: usize = if <Self as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE { 0 $(+ <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE)+ } else { 0 };
            fn pssz_size(&self) -> usize {
                $crate::pssz::try_to_pssz::<F, _>(self).expect("pssz: record size overflow").len()
            }
            fn pssz_pack(&self, out: &mut Vec<u8>) {
                <Self as $crate::pssz::PsszPack<F>>::pssz_pack_checked(self, out).expect("pssz: record encode failed; use try_to_pssz");
            }
            fn pssz_pack_checked(&self, out: &mut Vec<u8>) -> $crate::pssz::PsszResult<()> {
                $crate::pssz_checked::encode_record::<F>(&[
                    $($crate::pssz_struct!(@field self.$field, $FTy; $dwnc)?),+
                ], $dwnc, out)
            }
        }
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszUnpack<F> for $Ty {
            fn pssz_unpack(bytes: &[u8]) -> $crate::pssz::PsszResult<Self> {
                let spans = $crate::pssz_checked::record_spans::<F>(&[
                    $($crate::pssz_checked::Layout::of::<F, $FTy>()),+
                ], bytes, $dwnc)?;
                let mut spans = spans.into_iter();
                Ok(Self {
                    $($field: <$FTy as $crate::pssz::PsszUnpack<F>>::pssz_unpack(&bytes[spans.next().expect("field count matches")])?),+
                })
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

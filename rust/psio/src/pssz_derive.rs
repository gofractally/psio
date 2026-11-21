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

    (@omit $value:expr, $FTy:ty; true) => { false };
    (@omit $value:expr, $FTy:ty; false) => {
        <$FTy as $crate::pssz::PsszPack<F>>::IS_OPTIONAL && <$FTy as $crate::pssz::PsszPack<F>>::pssz_is_none(&$value)
    };
    (@field $value:expr, $FTy:ty, $kept:expr; true) => {{
        let value: $FTy = $value;
        $crate::pssz_checked::field_if_kept::<F, $FTy>(&value, $kept)
    }};
    (@field $value:expr, $FTy:ty, $kept:expr; false) => {
        $crate::pssz_checked::field_if_kept::<F, $FTy>(&$value, $kept)
    };

    (@impl $Ty:ident { $($field:ident : $FTy:ty),+ }; $dwnc:tt) => {
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszPack<F> for $Ty {
            const MIN_ENCODED_SIZE: usize = if $dwnc { 0 $(+ if <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE { <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE } else { F::OFFSET_BYTES })+ } else { F::HEADER_BYTES };
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
                let _depth = $crate::pssz_checked::EncodeDepth::enter()?;
                let mut kept = $crate::pssz_checked::record_kept_fields(&[
                    $($crate::pssz_struct!(@omit self.$field, $FTy; $dwnc)),+
                ], $dwnc);
                let result = $crate::pssz_checked::encode_record::<F>(&[
                    $({ let include = kept > 0; kept = kept.saturating_sub(1);
                        $crate::pssz_struct!(@field self.$field, $FTy, include; $dwnc)? }),+
                ], $dwnc, out);
                let _ = kept;
                result
            }
        }
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszUnpack<F> for $Ty {
            fn pssz_unpack(bytes: &[u8]) -> $crate::pssz::PsszResult<Self> {
                let _depth = $crate::pssz_checked::DecodeDepth::enter()?;
                let spans = $crate::pssz_checked::record_spans::<F>(&[
                    $($crate::pssz_checked::Layout::of::<F, $FTy>()),+
                ], bytes, $dwnc)?;
                let mut spans = spans.into_iter();
                Ok(Self {
                    $($field: $crate::pssz_checked::decode_field::<F, $FTy>(bytes, spans.next().expect("field count matches"))?),+
                })
            }
        }
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszValidate<F> for $Ty {
            fn pssz_validate(bytes: &[u8]) -> $crate::pssz::PsszResult<()> {
                let _depth = $crate::pssz_checked::DecodeDepth::enter()?;
                let spans = $crate::pssz_checked::record_spans::<F>(&[
                    $($crate::pssz_checked::Layout::of::<F, $FTy>()),+
                ], bytes, $dwnc)?;
                let mut spans = spans.into_iter();
                $(if let Some(span) = spans.next().expect("field count matches") {
                    <$FTy as $crate::pssz::PsszValidate<F>>::pssz_validate(&bytes[span])?;
                })+
                Ok(())
            }
        }
    };

    (@view_dwnc $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        $crate::pssz_struct!(@view_common $Ty { $($field : $FTy),+ }; true);
    };
    (@view $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        $crate::pssz_struct!(@view_common $Ty { $($field : $FTy),+ }; false);
    };
    (@view_common $Ty:ident { $($field:ident : $FTy:ty),+ }; $dwnc:tt) => {
        $crate::__paste::paste! {
            pub trait [<$Ty PsszAccessors>]<'a, F: $crate::pssz::PsszFormat> {
                $(fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F>;)+
            }
            impl<'a, F: $crate::pssz::PsszFormat> [<$Ty PsszAccessors>]<'a, F>
                for $crate::pssz_view::PsszView<'a, $Ty, F>
            {
                $(fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F> {
                    $crate::pssz_view::PsszView::new([<__pssz_field_ $Ty>]::<F, $FTy>(self.data, stringify!($field)))
                })+
            }
            #[inline(always)]
            #[allow(non_snake_case)]
            fn [<__pssz_field_ $Ty>]<'a, F: $crate::pssz::PsszFormat, T: $crate::pssz::PsszPack<F>>(
                data: &'a [u8], target: &str,
            ) -> &'a [u8] {
                let index = [$(stringify!($field)),+].iter().position(|name| *name == target).expect("known view field");
                let location = $crate::pssz_checked::record_location::<F>(&[
                    $($crate::pssz_checked::Layout::of::<F, $FTy>()),+
                ], index);
                $crate::pssz_checked::field_bytes::<F, T>(data, location, $dwnc).expect("pssz view: use a validated buffer")
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

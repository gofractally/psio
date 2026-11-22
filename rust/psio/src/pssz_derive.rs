//! Declarative macros generating the PSSZ codec for user structs: the
//! hand-written counterpart of `#[derive(PsszPack, PsszUnpack, PsszView)]`
//! and of C++ `PSIO_REFLECT(Struct, fields...)` on the pSSZ side. Both paths
//! share the helpers in `pssz_checked`, so they cannot drift apart.
//!
//! ```ignore
//! pssz_struct!(Account { id: u32, name: String, memo: Option<String> });
//! pssz_struct!(#[pssz(max_dynamic_data = 200)] Small { tag: u8, label: RawString });
//! pssz_struct_dwnc!(Point { x: u32, y: u32 });
//! ```
//!
//! `pssz_struct!` is an extensible record: a header holding the size of the
//! fixed region the writer emitted, then the fixed region, then variable
//! payloads. Trailing `None` fields are always pruned; readers honor the
//! writer's header (older data → missing trailing optionals are None, newer
//! data → unknown trailing slots are skipped) and reject a stored trailing
//! None. Fields appended after publication must be `Option<T>`.
//!
//! `pssz_struct_dwnc!` is a fixed-layout ("definition will not change")
//! record: no header, never pruned, cannot evolve.
//!
//! `#[pssz(max_dynamic_data = N)]` is the type's explicit cap (C++
//! `maxDynamicData(N)`): under the automatic `Pssz` profile it selects the
//! record's offset width and bounds its encoded size. Without it the width
//! follows the bound inferred from the fields.
//!
//! All field access goes through typed field codecs, never Rust memory
//! layout. A `PsszValidate` impl is generated whenever every field type has
//! one; fields with only Pack/Unpack codecs remain usable.

/// Fixed-layout (DWNC) pSSZ record.
#[macro_export]
macro_rules! pssz_struct_dwnc {
    (#[pssz(max_dynamic_data = $cap:expr)] $Ty:ident { $($field:ident : $FTy:ty),* $(,)? }) => {
        $crate::pssz_struct!(@all $Ty { $($field : $FTy),* }; true; ::core::option::Option::Some($cap));
    };
    ($Ty:ident { $($field:ident : $FTy:ty),* $(,)? }) => {
        $crate::pssz_struct!(@all $Ty { $($field : $FTy),* }; true; ::core::option::Option::None);
    };
}

/// Extensible pSSZ record.
#[macro_export]
macro_rules! pssz_struct {
    (#[pssz(max_dynamic_data = $cap:expr)] $Ty:ident { $($field:ident : $FTy:ty),* $(,)? }) => {
        $crate::pssz_struct!(@all $Ty { $($field : $FTy),* }; false; ::core::option::Option::Some($cap));
    };
    ($Ty:ident { $($field:ident : $FTy:ty),* $(,)? }) => {
        $crate::pssz_struct!(@all $Ty { $($field : $FTy),* }; false; ::core::option::Option::None);
    };

    (@all $Ty:ident { $($field:ident : $FTy:ty),* }; $dwnc:tt; $cap:expr) => {
        $crate::pssz_struct!(@view $Ty { $($field : $FTy),* }; $dwnc);
        $crate::pssz_struct!(@impl $Ty { $($field : $FTy),* }; $dwnc; $cap);
    };

    // DWNC fields are copied by value so the macro also works on
    // `#[repr(packed)]` structs; a DWNC record never prunes.
    (@omit $value:expr, $FTy:ty; true) => { false };
    (@omit $value:expr, $FTy:ty; false) => {
        <$FTy as $crate::pssz::PsszPack<F>>::IS_OPTIONAL
            && <$FTy as $crate::pssz::PsszPack<F>>::pssz_is_none(&$value)
    };
    (@field $value:expr, $FTy:ty, $kept:expr; true) => {{
        let value: $FTy = $value;
        $crate::pssz_checked::field_if_kept::<F, $FTy>(&value, $kept)
    }};
    (@field $value:expr, $FTy:ty, $kept:expr; false) => {
        $crate::pssz_checked::field_if_kept::<F, $FTy>(&$value, $kept)
    };

    (@impl $Ty:ident { $($field:ident : $FTy:ty),* }; $dwnc:tt; $cap:expr) => {
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszPack<F> for $Ty {
            // An extensible record always has its header. A DWNC record is
            // as small as its fixed region (zero for a zero-size record).
            const MIN_ENCODED_SIZE: usize = if $dwnc {
                $crate::pssz_checked::fixed_region(1, &[$($crate::pssz_checked::Layout::of::<F, $FTy>()),*])
            } else { 1 };
            const IS_FIXED_SIZE: bool = $dwnc $(&& <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE)*;
            const FIXED_SIZE: usize = if <Self as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE {
                0 $(+ <$FTy as $crate::pssz::PsszPack<F>>::FIXED_SIZE)*
            } else { 0 };
            const MAX_DYNAMIC_DATA: ::core::option::Option<usize> = $cap;
            const AUTO_BOUND: ::core::option::Option<usize> = match <Self as $crate::pssz::PsszPack<F>>::MAX_DYNAMIC_DATA {
                ::core::option::Option::Some(cap) => ::core::option::Option::Some(cap),
                ::core::option::Option::None => $crate::pssz_checked::record_bound(&[
                    $((<$FTy as $crate::pssz::PsszPack<F>>::AUTO_BOUND, <$FTy as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE)),*
                ], $dwnc),
            };
            const MAX_ENCODED_SIZE: ::core::option::Option<usize> = <Self as $crate::pssz::PsszPack<F>>::AUTO_BOUND;
            const SHAPE: $crate::pssz::Shape = $crate::pssz::Shape::record(
                false, $dwnc,
                <Self as $crate::pssz::PsszPack<F>>::OFFSET_WIDTH,
                <Self as $crate::pssz::PsszPack<F>>::IS_FIXED_SIZE,
                &[$(<$FTy as $crate::pssz::PsszPack<F>>::SHAPE),*]);
            fn pssz_size(&self) -> usize {
                let mut out = ::std::vec::Vec::new();
                <Self as $crate::pssz::PsszPack<F>>::pssz_pack_checked(self, &mut out)
                    .expect("pssz: record encode failed; use try_to_pssz");
                out.len()
            }
            fn pssz_pack(&self, out: &mut ::std::vec::Vec<u8>) {
                <Self as $crate::pssz::PsszPack<F>>::pssz_pack_checked(self, out)
                    .expect("pssz: record encode failed; use try_to_pssz");
            }
            #[allow(unused_mut, unused_variables, unused_assignments)]
            fn pssz_pack_checked(&self, out: &mut ::std::vec::Vec<u8>) -> $crate::pssz::PsszResult<()> {
                let _depth = $crate::pssz_checked::EncodeDepth::enter()?;
                let omitted: &[bool] = &[$($crate::pssz_struct!(@omit self.$field, $FTy; $dwnc)),*];
                let keep = $crate::pssz_checked::record_kept_fields(omitted, $dwnc);
                let mut index = 0usize;
                $crate::pssz_checked::finish_record::<F>(
                    <Self as $crate::pssz::PsszPack<F>>::OFFSET_WIDTH,
                    <Self as $crate::pssz::PsszPack<F>>::MAX_DYNAMIC_DATA,
                    &[$({
                        let kept = index < keep;
                        index += 1;
                        $crate::pssz_struct!(@field self.$field, $FTy, kept; $dwnc)?
                    }),*], $dwnc, out)
            }
        }
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszUnpack<F> for $Ty {
            #[allow(unused_mut, unused_variables)]
            fn pssz_unpack(bytes: &[u8]) -> $crate::pssz::PsszResult<Self> {
                let _depth = $crate::pssz_checked::DecodeDepth::enter()?;
                $crate::pssz_checked::enter_record::<F>(<Self as $crate::pssz::PsszPack<F>>::MAX_DYNAMIC_DATA, bytes)?;
                let spans = $crate::pssz_checked::record_spans_n(
                    <Self as $crate::pssz::PsszPack<F>>::OFFSET_WIDTH,
                    [$($crate::pssz_checked::Layout::of::<F, $FTy>()),*], bytes, $dwnc)?;
                let mut spans = spans.into_iter();
                ::core::result::Result::Ok($Ty {
                    $($field: $crate::pssz_checked::decode_field::<F, $FTy>(bytes, spans.next().flatten())?),*
                })
            }
        }
        impl<F: $crate::pssz::PsszFormat> $crate::pssz::PsszValidate<F> for $Ty
        where $($FTy: $crate::pssz::PsszValidate<F>),*
        {
            #[allow(unused_mut, unused_variables)]
            fn pssz_validate(bytes: &[u8]) -> $crate::pssz::PsszResult<()> {
                let _depth = $crate::pssz_checked::DecodeDepth::enter()?;
                $crate::pssz_checked::enter_record::<F>(<Self as $crate::pssz::PsszPack<F>>::MAX_DYNAMIC_DATA, bytes)?;
                let spans = $crate::pssz_checked::record_spans_n(
                    <Self as $crate::pssz::PsszPack<F>>::OFFSET_WIDTH,
                    [$($crate::pssz_checked::Layout::of::<F, $FTy>()),*], bytes, $dwnc)?;
                let mut spans = spans.into_iter();
                $(if let ::core::option::Option::Some(span) = spans.next().flatten() {
                    <$FTy as $crate::pssz::PsszValidate<F>>::pssz_validate(&bytes[span])?;
                })*
                ::core::result::Result::Ok(())
            }
        }
    };

    // Named view accessors via a generated trait (orphan-rule workaround).
    // Users `use ${Ty}PsszAccessors;`. Field access is O(1) and honors the
    // writer's header: a pruned trailing optional reads as None, and unknown
    // appended slots delimit the last known variable field.
    (@view $Ty:ident { $($field:ident : $FTy:ty),* }; $dwnc:tt) => {
        $crate::__paste::paste! {
            pub trait [<$Ty PsszAccessors>]<'a, F: $crate::pssz::PsszFormat> {
                $(
                    fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F>;
                    /// Fallible form for unvalidated buffers.
                    fn [<try_ $field>](&self) -> $crate::pssz::PsszResult<$crate::pssz_view::PsszView<'a, $FTy, F>>;
                )*
            }
            impl<'a, F: $crate::pssz::PsszFormat> [<$Ty PsszAccessors>]<'a, F>
                for $crate::pssz_view::PsszView<'a, $Ty, F>
            {
                $(
                    fn $field(&self) -> $crate::pssz_view::PsszView<'a, $FTy, F> {
                        self.[<try_ $field>]().expect("pssz view: malformed record; validate first")
                    }
                    fn [<try_ $field>](&self) -> $crate::pssz::PsszResult<$crate::pssz_view::PsszView<'a, $FTy, F>> {
                        ::core::result::Result::Ok($crate::pssz_view::PsszView::trusted(
                            [<__pssz_field_ $Ty>]::<F, $FTy>(self.bytes(), stringify!($field))?))
                    }
                )*
            }
            #[allow(non_snake_case, dead_code)]
            fn [<__pssz_field_ $Ty>]<'a, F: $crate::pssz::PsszFormat, T: $crate::pssz::PsszPack<F>>(
                data: &'a [u8], target: &str,
            ) -> $crate::pssz::PsszResult<&'a [u8]> {
                let names: &[&str] = &[$(stringify!($field)),*];
                let index = names.iter().position(|name| *name == target)
                    .ok_or($crate::pssz::PsszError("pssz view: unknown field"))?;
                let location = $crate::pssz_checked::record_location(
                    <$Ty as $crate::pssz::PsszPack<F>>::OFFSET_WIDTH,
                    &[$($crate::pssz_checked::Layout::of::<F, $FTy>()),*], index);
                $crate::pssz_checked::field_bytes::<F, T>(data, location, $dwnc)
            }
        }
    };
}

#[cfg(test)]
mod tests {
    use crate::pssz::{Pssz32, PsszPack, PsszUnpack, decode, encode};

    #[derive(Debug, PartialEq, Eq)]
    struct Named { name: String, value: u32 }
    pssz_struct!(Named { name: String, value: u32 });

    #[test]
    fn named_round_trip() {
        let n = Named { name: "alice".into(), value: 77 };
        let b = encode::<Pssz32, _>(&n);
        assert_eq!(decode::<Pssz32, Named>(&b).unwrap(), n);
    }

    #[test]
    fn cpp_pssz32_named_cross() {
        // C++: Named{"alice", 77} → "08000000080000004d000000616c696365"
        // Header: 08000000 (fixed region is 8 bytes)
        // Fixed: 08000000 (offset to name=8) + 4d000000 (value=77)
        // Tail: "alice"
        let expected = hex("08000000080000004d000000616c696365");
        let n = Named { name: "alice".into(), value: 77 };
        assert_eq!(encode::<Pssz32, _>(&n), expected);
        assert_eq!(decode::<Pssz32, Named>(&expected).unwrap(), n);
    }

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i+2], 16).unwrap()).collect()
    }

    #[test]
    fn pssz_view_named_accessors() {
        use super::tests::NamedPsszAccessors;
        let n = Named { name: "alice".into(), value: 77 };
        let b = encode::<Pssz32, _>(&n);
        let v: crate::pssz_view::PsszView<Named, Pssz32> =
            crate::pssz_view::PsszView::trusted(&b);
        assert_eq!(v.name().view().unwrap(), "alice");
        assert_eq!(v.value().get(), 77);
    }
}

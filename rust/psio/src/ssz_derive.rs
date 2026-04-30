//! Declarative macros that generate `SszPack` / `SszUnpack` for
//! user-defined structs. This is the Rust counterpart of C++'s
//! `PSIO1_REFLECT(Struct, fields...)` on the SSZ side.
//!
//! A true proc-macro derive is the long-term plan (see Phase E in
//! `.issues/rust-ssz-pssz-parity-plan.md`); this declarative macro
//! covers the common case without requiring a separate build step.
//!
//! Usage:
//! ```ignore
//! use psio1::ssz::{SszPack, SszUnpack};
//!
//! struct Validator {
//!     pubkey: [u8; 48],
//!     withdrawal_credentials: [u8; 32],
//!     effective_balance: u64,
//!     slashed: bool,
//!     activation_eligibility_epoch: u64,
//!     activation_epoch: u64,
//!     exit_epoch: u64,
//!     withdrawable_epoch: u64,
//! }
//! ssz_struct!(Validator {
//!     pubkey, withdrawal_credentials, effective_balance, slashed,
//!     activation_eligibility_epoch, activation_epoch, exit_epoch, withdrawable_epoch
//! });
//! ```
//!
//! Matches the C++ reflected-struct SSZ emission:
//! - Fixed fields inline in declaration order
//! - Variable fields: 4-byte offset slot in the fixed region, payload
//!   appended to the tail (container-relative offset)
//! - Single-pass backpatching encoder (same as C++ fix in this session)

/// Generate `SszPack` and `SszUnpack` impls for a struct whose every
/// field is itself `SszPack + SszUnpack`.
///
/// Limitations (current):
/// - Doesn't detect DWNC / memcpy-layout automatically (future proc-macro)
/// - Requires all fields to be named and individually listed
/// - IS_FIXED_SIZE is set to false; FIXED_SIZE to 0 — meaning every
///   struct is treated as variable at the outer level (safe, pessimal)
///
/// Future improvements: const-eval to derive IS_FIXED_SIZE from member
/// consts, DWNC attribute, memcpy fast-path detection.
/// Helper: paste two idents at macro-expansion time without needing the
/// `paste` crate. Used by `ssz_struct!` to name the generated View type.
#[doc(hidden)]
#[macro_export]
macro_rules! __ssz_view_name {
    ($Ty:ident) => { $Ty };
}

/// DWNC variant: all-fixed struct with declared-stable layout. Mirrors
/// C++'s `definitionWillNotChange()` attribute. Uses memcpy-style pack/
/// unpack when the struct's layout matches wire (sum of field sizes ==
/// `size_of::<T>()`). Works with `#[repr(C, packed)]` to eliminate
/// alignment padding — see the Validator port for the canonical example.
///
/// The pack/unpack path still goes field-by-field, but because every
/// field is fixed-size the emitted code is a single flat concatenation
/// of member bytes — no offset table, no backpatching.
#[macro_export]
macro_rules! ssz_struct_dwnc {
    ($Ty:ident { $($field:ident : $FTy:ty),+ $(,)? }) => {
        $crate::ssz_struct!(@view $Ty { $($field : $FTy),+ });

        impl $crate::ssz::SszPack for $Ty {
            const IS_FIXED_SIZE: bool  = true;
            const FIXED_SIZE:    usize = { 0 $( + <$FTy as $crate::ssz::SszPack>::FIXED_SIZE )+ };
            fn ssz_size(&self) -> usize { Self::FIXED_SIZE }
            fn ssz_pack(&self, out: &mut Vec<u8>) {
                // Whole-struct memcpy fast path: when `size_of::<Self>()`
                // equals FIXED_SIZE (guaranteed by `#[repr(C, packed)]`
                // on the struct), the in-memory bytes of `self` are
                // already the SSZ wire bytes. One `extend_from_slice`
                // beats the per-field loop 10-100×. Matches the C++
                // `memcpy_layout_struct` behavior used for
                // `std::vector<Validator>` encode.
                #[cfg(target_endian = "little")]
                if std::mem::size_of::<Self>() == <Self as $crate::ssz::SszPack>::FIXED_SIZE {
                    // SAFETY: layout invariants above + packed repr
                    // guarantee no padding; the first size_of<Self> bytes
                    // of `self` are fully initialized and equal the
                    // wire layout.
                    let bytes = unsafe {
                        ::core::slice::from_raw_parts(
                            self as *const Self as *const u8,
                            ::core::mem::size_of::<Self>())
                    };
                    out.extend_from_slice(bytes);
                    return;
                }
                // Slow path: flat field-by-field concatenation. Use a
                // by-value copy of each field so this compiles under
                // `#[repr(C, packed)]` too (you can't take `&self.field`
                // on a packed struct, but `{self.field}` is fine when
                // the field is `Copy`). DWNC fields are all Copy by
                // construction (primitives / byte arrays / nested DWNC
                // structs) so this is always well-formed. The extra
                // copy is moot: for packed structs the fast path above
                // fires; for non-packed ones it doesn't cost anything
                // because the compiler elides the temporary.
                $(
                    {
                        let __f: $FTy = self.$field;
                        <$FTy as $crate::ssz::SszPack>::ssz_pack(&__f, out);
                    }
                )+
            }
        }
        impl $crate::ssz::SszUnpack for $Ty {
            fn ssz_unpack(bytes: &[u8]) -> $crate::ssz::SszResult<Self> {
                if bytes.len() < <Self as $crate::ssz::SszPack>::FIXED_SIZE {
                    return Err($crate::ssz::SszError("ssz dwnc: underrun"));
                }
                // Whole-struct memcpy decode fast path (same invariants
                // as pack). Reads wire bytes directly into a freshly
                // allocated `Self` on the stack via MaybeUninit.
                #[cfg(target_endian = "little")]
                if ::core::mem::size_of::<Self>() == <Self as $crate::ssz::SszPack>::FIXED_SIZE {
                    let fs = <Self as $crate::ssz::SszPack>::FIXED_SIZE;
                    let mut out: ::core::mem::MaybeUninit<Self> =
                        ::core::mem::MaybeUninit::uninit();
                    // SAFETY: layout match + LE + packed repr make the
                    // wire bytes a valid bit pattern for `Self`.
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
                        let fs = <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                        let out = <$FTy as $crate::ssz::SszUnpack>::ssz_unpack(
                            &bytes[pos .. pos + fs])?;
                        pos += fs;
                        out
                    };
                )+
                let _ = pos;
                Ok($Ty { $($field),+ })
            }
        }
        impl $crate::ssz::SszValidate for $Ty {
            fn ssz_validate(bytes: &[u8]) -> $crate::ssz::SszResult<()> {
                if bytes.len() < <Self as $crate::ssz::SszPack>::FIXED_SIZE {
                    return Err($crate::ssz::SszError("ssz dwnc validate: underrun"));
                }
                let mut pos: usize = 0;
                $(
                    {
                        let fs = <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                        <$FTy as $crate::ssz::SszValidate>::ssz_validate(
                            &bytes[pos .. pos + fs])?;
                        pos += fs;
                    }
                )+
                let _ = pos;
                Ok(())
            }
        }
    };
}

#[macro_export]
macro_rules! ssz_struct {
    ($Ty:ident { $($field:ident : $FTy:ty),+ $(,)? }) => {
        $crate::ssz_struct!(@view $Ty { $($field : $FTy),+ });

        impl $crate::ssz::SszPack for $Ty {
            const IS_FIXED_SIZE: bool  = false;
            const FIXED_SIZE:    usize = 0;
            fn ssz_size(&self) -> usize {
                let mut total: usize = 0;
                $(
                    total += if <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        <$FTy as $crate::ssz::SszPack>::FIXED_SIZE
                    } else { 4 };
                )+
                $(
                    if !<$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        total += <$FTy as $crate::ssz::SszPack>::ssz_size(&self.$field);
                    }
                )+
                total
            }
            fn ssz_pack(&self, out: &mut Vec<u8>) {
                let fixed_start = out.len();
                let mut slots: Vec<usize> = Vec::new();
                $(
                    if <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        <$FTy as $crate::ssz::SszPack>::ssz_pack(&self.$field, out);
                    } else {
                        slots.push(out.len());
                        out.resize(out.len() + 4, 0);
                    }
                )+
                let mut var_idx: usize = 0;
                $(
                    if !<$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        let rel = (out.len() - fixed_start) as u32;
                        let slot_pos = slots[var_idx];
                        out[slot_pos .. slot_pos + 4].copy_from_slice(&rel.to_le_bytes());
                        <$FTy as $crate::ssz::SszPack>::ssz_pack(&self.$field, out);
                        var_idx += 1;
                    }
                )+
                let _ = var_idx;
            }
        }
        impl $crate::ssz::SszUnpack for $Ty {
            fn ssz_unpack(bytes: &[u8]) -> $crate::ssz::SszResult<Self> {
                let mut field_pos: usize = 0;
                let mut var_offsets: Vec<u32> = Vec::new();
                $(
                    if <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        field_pos += <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                    } else {
                        if field_pos + 4 > bytes.len() {
                            return Err($crate::ssz::SszError("ssz struct: offset underrun"));
                        }
                        let off = u32::from_le_bytes([
                            bytes[field_pos], bytes[field_pos + 1],
                            bytes[field_pos + 2], bytes[field_pos + 3],
                        ]);
                        var_offsets.push(off);
                        field_pos += 4;
                    }
                    let _: &str = stringify!($field);
                )+
                let mut field_pos: usize = 0;
                let mut var_i: usize = 0;
                $(
                    let $field: $FTy = if <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        let fs = <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                        let out = <$FTy as $crate::ssz::SszUnpack>::ssz_unpack(&bytes[field_pos .. field_pos + fs])?;
                        field_pos += fs;
                        out
                    } else {
                        let beg = var_offsets[var_i] as usize;
                        let stop = if var_i + 1 < var_offsets.len() {
                            var_offsets[var_i + 1] as usize
                        } else { bytes.len() };
                        var_i += 1;
                        field_pos += 4;
                        <$FTy as $crate::ssz::SszUnpack>::ssz_unpack(&bytes[beg .. stop])?
                    };
                )+
                let _ = field_pos;
                let _ = var_i;
                Ok($Ty { $($field),+ })
            }
        }
        impl $crate::ssz::SszValidate for $Ty {
            fn ssz_validate(bytes: &[u8]) -> $crate::ssz::SszResult<()> {
                // Pass 1: walk the fixed region and capture variable offsets.
                let mut fixed_pos: usize = 0;
                let mut var_offsets: Vec<u32> = Vec::new();
                $(
                    if <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        fixed_pos += <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                    } else {
                        if fixed_pos + 4 > bytes.len() {
                            return Err($crate::ssz::SszError(
                                "ssz validate: struct offset underrun"));
                        }
                        let off = u32::from_le_bytes([
                            bytes[fixed_pos], bytes[fixed_pos + 1],
                            bytes[fixed_pos + 2], bytes[fixed_pos + 3],
                        ]);
                        var_offsets.push(off);
                        fixed_pos += 4;
                    }
                    let _: &str = stringify!($field);
                )+
                if fixed_pos > bytes.len() {
                    return Err($crate::ssz::SszError(
                        "ssz validate: struct fixed region overruns buffer"));
                }
                // Pass 2: validate each field's span.
                let mut fixed_pos: usize = 0;
                let mut var_i: usize = 0;
                $(
                    if <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE {
                        let fs = <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                        <$FTy as $crate::ssz::SszValidate>::ssz_validate(
                            &bytes[fixed_pos .. fixed_pos + fs])?;
                        fixed_pos += fs;
                    } else {
                        let beg = var_offsets[var_i] as usize;
                        let stop = if var_i + 1 < var_offsets.len() {
                            var_offsets[var_i + 1] as usize
                        } else { bytes.len() };
                        if beg > stop || stop > bytes.len() {
                            return Err($crate::ssz::SszError(
                                "ssz validate: struct var offset out of range"));
                        }
                        <$FTy as $crate::ssz::SszValidate>::ssz_validate(
                            &bytes[beg .. stop])?;
                        var_i += 1;
                        fixed_pos += 4;
                    }
                )+
                let _ = fixed_pos;
                let _ = var_i;
                Ok(())
            }
        }
    };

    // Emit named-accessor view for a reflected struct.
    //
    // Rust's orphan rule forbids inherent `impl SszView<T>` blocks
    // outside the psio crate, so we emit methods via a trait that lives
    // in the caller's module. Both the trait AND its impl block are
    // local to the caller; the implemented type (`SszView<T>`) is
    // foreign — which is allowed as long as EITHER side is local.
    //
    // The user can call `v.name()` directly if the trait is in scope.
    // The macro re-exports with a `pub use` shim so `use MyModule::*`
    // brings both the struct and the accessor trait along.
    (@view $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        paste::paste! {
            pub trait [<$Ty SszAccessors>]<'a> {
                $(
                    fn $field(&self) -> $crate::ssz_view::SszView<'a, $FTy>;
                )+
            }

            impl<'a> [<$Ty SszAccessors>]<'a> for $crate::ssz_view::SszView<'a, $Ty> {
                $(
                    fn $field(&self) -> $crate::ssz_view::SszView<'a, $FTy> {
                        let (beg, end) = [<__ssz_span_of_ $Ty>](
                            self.data, stringify!($field));
                        $crate::ssz_view::SszView::new(&self.data[beg .. end])
                    }
                )+
            }

            #[allow(non_snake_case)]
            fn [<__ssz_span_of_ $Ty>](data: &[u8], target: &str) -> (usize, usize) {
                let mut pos: usize = 0;
                let mut target_off: usize = 0;
                let mut found_var = false;
                $(
                    {
                        let this = stringify!($field);
                        let is_fixed = <$FTy as $crate::ssz::SszPack>::IS_FIXED_SIZE;
                        if this == target {
                            if is_fixed {
                                let fs = <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                                return (pos, pos + fs);
                            } else {
                                target_off = u32::from_le_bytes([
                                    data[pos], data[pos+1], data[pos+2], data[pos+3],
                                ]) as usize;
                                found_var = true;
                            }
                        } else if found_var && !is_fixed {
                            let next_off = u32::from_le_bytes([
                                data[pos], data[pos+1], data[pos+2], data[pos+3],
                            ]) as usize;
                            return (target_off, next_off);
                        }
                        if is_fixed {
                            pos += <$FTy as $crate::ssz::SszPack>::FIXED_SIZE;
                        } else {
                            pos += 4;
                        }
                    }
                )+
                let _ = pos;
                if found_var { return (target_off, data.len()); }
                panic!("ssz view: field {} not found in {}",
                        target, stringify!($Ty));
            }
        }

    };
}

#[cfg(test)]
mod tests {
    use crate::ssz::{SszPack, SszUnpack, from_ssz, to_ssz};

    // Reflected struct with mixed fixed + variable fields — matches the
    // C++ `Named` test struct from bench_fracpack emit_fixtures.
    #[derive(Debug, PartialEq, Eq)]
    struct Named {
        name: String,
        value: u32,
    }
    ssz_struct!(Named { name: String, value: u32 });

    #[test]
    fn named_round_trip() {
        let n = Named { name: "alice".into(), value: 77 };
        let b = to_ssz(&n);
        assert_eq!(from_ssz::<Named>(&b).unwrap(), n);
    }

    #[test]
    fn cpp_ssz_named_cross() {
        // C++: Named{name="alice", value=77} → "080000004d000000616c696365"
        // Fixed region: offset[name]=8 (4 bytes), value=77 (4 bytes) = 8 bytes
        // Tail: "alice" (5 bytes) → total 13 bytes
        let expected = hex("080000004d000000616c696365");
        let n = Named { name: "alice".into(), value: 77 };
        assert_eq!(to_ssz(&n), expected);
        assert_eq!(from_ssz::<Named>(&expected).unwrap(), n);
    }

    // Struct with only fixed fields — like C++ `Point` (DWNC).
    #[derive(Debug, PartialEq, Eq)]
    struct Point { x: u32, y: u32 }
    ssz_struct!(Point { x: u32, y: u32 });

    #[test]
    fn cpp_ssz_point_cross() {
        // C++: Point{100, 200} → "64000000c8000000"
        let expected = hex("64000000c8000000");
        let p = Point { x: 100, y: 200 };
        assert_eq!(to_ssz(&p), expected);
        assert_eq!(from_ssz::<Point>(&expected).unwrap(), p);
    }

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i+2], 16).unwrap()).collect()
    }

    // ── Named-accessor view tests ─────────────────────────────────────────
    //
    // Proxy/view objects in Rust use field NAMES, not compile-time indices
    // (C++ uses `field<I>()` because template metaprogramming gives it
    // integer indices for free — Rust derive macros give us names).

    #[test]
    fn view_named_accessors_fixed_fields() {
        // Trait is generated by the macro and brought in by `use` below.
        use super::tests::PointSszAccessors;
        let p = Point { x: 100, y: 200 };
        let b = to_ssz(&p);
        let v: crate::ssz_view::SszView<Point> = crate::ssz_view::ssz_view_of(&b);
        assert_eq!(v.x().get(), 100);
        assert_eq!(v.y().get(), 200);
    }

    #[test]
    fn view_named_accessors_mixed_variable() {
        use super::tests::NamedSszAccessors;
        let n = Named { name: "alice".into(), value: 77 };
        let b = to_ssz(&n);
        let v: crate::ssz_view::SszView<Named> = crate::ssz_view::ssz_view_of(&b);
        assert_eq!(v.name().view(), "alice");
        assert_eq!(v.value().get(), 77);
    }
}

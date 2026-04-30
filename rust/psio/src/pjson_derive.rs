//! Declarative macro generating `Pjson` impls + canonical-typed view
//! accessors for user structs.
//!
//! Counterpart of C++'s `PSIO_REFLECT(Struct, fields...)` on the pjson
//! side. The generated code:
//!
//! - Implements `Pjson` for the struct with field encounter order
//!   matching the macro invocation order.
//! - Emits a `<Struct>PjsonAccessors` trait providing
//!   `pjson_view::View`-style sub-views per field, accessed by the
//!   canonical hash-template fast path.
//! - Stores the canonical hash template (one byte per field) as a
//!   static array, matching the layout the canonical encoder writes.
//!
//! Mirrors the patterns from `pssz_derive.rs` — declarative macro,
//! no proc-macro dependency, paste! for trait/method name interpolation.

#[macro_export]
macro_rules! pjson_struct {
    ($Ty:ident { $($field:ident : $FTy:ty),+ $(,)? }) => {
        $crate::pjson_struct!(@impl $Ty { $($field : $FTy),+ });
        $crate::pjson_struct!(@view $Ty { $($field : $FTy),+ });
    };

    (@impl $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        impl $crate::pjson::Pjson for $Ty {
            fn pjson_size(&self) -> usize {
                use $crate::pjson::{self, Pjson};
                let mut vd: usize = 0;
                $(
                    {
                        let key: &[u8] = stringify!($field).as_bytes();
                        let kx = if key.len() >= 0xFF {
                            pjson::varuint_byte_count((key.len() - 0xFF) as u64)
                        } else { 0 };
                        vd += kx + key.len()
                            + <$FTy as Pjson>::pjson_size(&self.$field);
                    }
                )+
                let n: usize = 0 $( + { let _ = stringify!($field); 1 } )+;
                let slot_w_code = if vd <= 0xFF { 0 }
                                  else if vd <= 0xFFFF { 1 }
                                  else if vd <= 0xFFFFFF { 2 }
                                  else { 3 };
                let slot_w = (slot_w_code as usize) + 1;
                1 + 1 + vd + n + (slot_w + 1) * n + 2
            }

            fn pjson_encode_at(&self, dst: &mut [u8], pos_in: usize) -> usize {
                use $crate::pjson::{self, key_hash8, tag, Pjson};
                let mut pos = pos_in;
                dst[pos] = tag::OBJECT << 4;
                pos += 1;

                // Compute value_data first to pick slot_w.
                let mut vd: usize = 0;
                $(
                    {
                        let key: &[u8] = stringify!($field).as_bytes();
                        let kx = if key.len() >= 0xFF {
                            pjson::varuint_byte_count((key.len() - 0xFF) as u64)
                        } else { 0 };
                        vd += kx + key.len()
                            + <$FTy as Pjson>::pjson_size(&self.$field);
                    }
                )+
                let n: usize = 0 $( + { let _ = stringify!($field); 1 } )+;

                let slot_w_code: u8 = if vd <= 0xFF { 0 }
                                       else if vd <= 0xFFFF { 1 }
                                       else if vd <= 0xFFFFFF { 2 }
                                       else { 3 };
                let slot_w = (slot_w_code as usize) + 1;
                dst[pos] = slot_w_code;
                pos += 1;
                let value_data_start = pos;
                let hash_table_pos = value_data_start + vd;
                let slot_table_pos = hash_table_pos + n;
                let count_pos = slot_table_pos + (slot_w + 1) * n;
                let entry_stride = slot_w + 1;

                let mut field_idx: usize = 0;
                $(
                    {
                        let key: &[u8] = stringify!($field).as_bytes();
                        let off = (pos - value_data_start) as u32;
                        let ks_byte: u8 = if key.len() < 0xFF {
                            key.len() as u8
                        } else {
                            0xFFu8
                        };
                        let slot_pos = slot_table_pos + field_idx * entry_stride;
                        // write_width inlined.
                        let off_bytes = off.to_le_bytes();
                        dst[slot_pos..slot_pos + slot_w].copy_from_slice(&off_bytes[..slot_w]);
                        dst[slot_pos + slot_w] = ks_byte;
                        dst[hash_table_pos + field_idx] = key_hash8(key);
                        if key.len() >= 0xFF {
                            // Long-key escape — varuint for the excess.
                            let excess = (key.len() - 0xFF) as u64;
                            let nb = pjson::varuint_byte_count(excess);
                            // Inline write_varuint.
                            let prefix = ((nb - 1) as u8) << 6;
                            dst[pos] = prefix | ((excess & 0x3F) as u8);
                            let mut hi = excess >> 6;
                            for i in 1..nb {
                                dst[pos + i] = (hi & 0xFF) as u8;
                                hi >>= 8;
                            }
                            pos += nb;
                        }
                        if !key.is_empty() {
                            dst[pos..pos + key.len()].copy_from_slice(key);
                        }
                        pos += key.len();
                        let written = <$FTy as Pjson>::pjson_encode_at(
                            &self.$field, dst, pos);
                        pos += written;
                        field_idx += 1;
                    }
                )+
                let _ = field_idx;
                debug_assert_eq!(pos, hash_table_pos);
                dst[count_pos] = (n & 0xFF) as u8;
                dst[count_pos + 1] = ((n >> 8) & 0xFF) as u8;
                count_pos + 2 - pos_in
            }

            fn pjson_decode(bytes: &[u8]) -> $crate::pjson::PjsonResult<Self> {
                use $crate::pjson::{Pjson, Value, PjsonError};
                let parsed = $crate::pjson::decode(bytes)?;
                match parsed {
                    Value::Object(entries) => {
                        // Order-tolerant decode: look up each field by
                        // key. The encoder produces canonical order, so
                        // a same-engine round-trip will hit the
                        // sequential path; cross-encoder buffers may
                        // have a different order and still decode
                        // correctly. (The C++ reference decoder is
                        // order-tolerant for the same reason.)
                        let mut by_key: std::collections::HashMap<&[u8], &Value<'_>> =
                            std::collections::HashMap::with_capacity(entries.len());
                        for (k, v) in entries.iter() {
                            by_key.insert(*k, v);
                        }
                        $(
                            let key: &[u8] = stringify!($field).as_bytes();
                            let $field: $FTy = {
                                let v = by_key.get(key)
                                    .ok_or(PjsonError(concat!(
                                        "pjson decode: missing field ", stringify!($field))))?;
                                // Re-encode the field's value to a temp
                                // buffer and decode through Pjson::pjson_decode.
                                // This is correctness-first; a fast
                                // path that walks the original buffer
                                // is the typed-view's job.
                                let mut tmp = vec![];
                                $crate::pjson::encode(v, &mut tmp);
                                <$FTy as Pjson>::pjson_decode(&tmp)?
                            };
                        )+
                        Ok($Ty { $($field),+ })
                    }
                    _ => Err(PjsonError("pjson decode: expected object")),
                }
            }
        }
    };

    // Generate <Ty>PjsonAccessors trait + canonical-template view.
    (@view $Ty:ident { $($field:ident : $FTy:ty),+ }) => {
        paste::paste! {
            #[allow(non_camel_case_types, dead_code)]
            pub trait [<$Ty PjsonAccessors>]<'a> {
                $( fn $field(&self) -> $crate::pjson::PjsonResult<$FTy>; )+
            }
            impl<'a> [<$Ty PjsonAccessors>]<'a>
                for $crate::pjson_view::View<'a>
            {
                $(
                    fn $field(&self) -> $crate::pjson::PjsonResult<$FTy> {
                        let key: &[u8] = stringify!($field).as_bytes();
                        let v = self.find(key).ok_or(
                            $crate::pjson::PjsonError(concat!(
                                "pjson view: missing field ",
                                stringify!($field))))?;
                        // Decode the field's sub-view via Pjson trait.
                        <$FTy as $crate::pjson::Pjson>::pjson_decode(v.raw())
                    }
                )+
            }
        }
    };
}

#[cfg(test)]
mod tests {
    use crate::pjson::{from_pjson, to_pjson};

    // Cross-cutting tests live here; the macro tests proper live in
    // each user file. Use a small struct to verify the macro output.
    #[derive(Debug, PartialEq)]
    pub struct Point {
        pub x: i32,
        pub y: i32,
    }
    pjson_struct!(Point { x: i32, y: i32 });

    #[test]
    fn point_round_trip() {
        let p = Point { x: -42, y: 77 };
        let b = to_pjson(&p);
        let q: Point = from_pjson(&b).unwrap();
        assert_eq!(p, q);
    }

    #[derive(Debug, PartialEq)]
    pub struct NameRecord {
        pub account: u64,
        pub limit: u64,
    }
    pjson_struct!(NameRecord {
        account: u64,
        limit: u64,
    });

    #[test]
    fn name_record_round_trip() {
        let n = NameRecord {
            account: 0x0123_4567_89AB_CDEF,
            limit: 1_000_000,
        };
        let b = to_pjson(&n);
        let m: NameRecord = from_pjson(&b).unwrap();
        assert_eq!(n, m);
    }

    #[derive(Debug, PartialEq)]
    pub struct FlatRecord {
        pub id: u32,
        pub label: String,
        pub values: Vec<u16>,
    }
    pjson_struct!(FlatRecord {
        id: u32,
        label: String,
        values: Vec<u16>,
    });

    #[test]
    fn flat_record_round_trip() {
        let r = FlatRecord {
            id: 42,
            label: "hello".to_string(),
            values: vec![1, 2, 3, 100, 1000],
        };
        let b = to_pjson(&r);
        let s: FlatRecord = from_pjson(&b).unwrap();
        assert_eq!(r, s);
    }

    #[test]
    fn point_view_accessors() {
        use super::tests::PointPjsonAccessors;
        let p = Point { x: -42, y: 77 };
        let b = to_pjson(&p);
        let v = crate::pjson_view::View::new(&b);
        assert_eq!(v.x().unwrap(), -42);
        assert_eq!(v.y().unwrap(), 77);
    }
}

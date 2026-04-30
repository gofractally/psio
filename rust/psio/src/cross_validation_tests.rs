//! Cross-format validation: verify the Rust encoders produce byte-identical
//! output to the C++ encoders on canonical shapes. Hex fixtures were emitted
//! by `/tmp/xval/emit_fixtures.cpp` on 2026-04-23 (see
//! `.issues/pssz-overnight-completion-report.md` and the cross-format
//! audit report in `.issues/format-parity-audit.md`).
//!
//! For formats that don't have a Rust implementation yet, the fixture is
//! recorded as documentation — no `#[test]` runs it.

#[cfg(test)]
mod tests {
    use crate::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    // ── Fracpack (C++ frac_format_32) ────────────────────────────────────────
    // C++ and Rust must produce byte-identical fracpack output for these.

    #[test]
    fn fracpack_u32_cross() {
        // C++: psio1::to_frac(std::uint32_t{0xDEADBEEF}) → "efbeadde"
        let expected = hex("efbeadde");
        assert_eq!(0xDEADBEEFu32.packed(), expected);
        assert_eq!(u32::unpacked(&expected).unwrap(), 0xDEADBEEF);
    }

    #[test]
    fn fracpack_string_cross() {
        // C++: psio1::to_frac(std::string("hello")) → "0500000068656c6c6f"
        // (4-byte LE length prefix + raw bytes).
        let expected = hex("0500000068656c6c6f");
        let s = "hello".to_string();
        assert_eq!(s.packed(), expected);
        assert_eq!(String::unpacked(&expected).unwrap(), "hello");
    }

    #[test]
    fn fracpack_vec_u32_cross() {
        // C++: psio1::to_frac(std::vector<uint32_t>{1,2,3})
        //       → "0c000000010000000200000003000000"
        let expected = hex("0c000000010000000200000003000000");
        let v = vec![1u32, 2, 3];
        assert_eq!(v.packed(), expected);
        assert_eq!(<Vec<u32>>::unpacked(&expected).unwrap(), v);
    }

    #[test]
    fn fracpack_vec_string_cross() {
        // C++: psio1::to_frac(std::vector<std::string>{"a","bc","def"}) →
        //   0c000000 — length in bytes of fixed region (3 × 4-byte offsets)
        //   0c000000 0d000000 0f000000 — pointer-relative offsets to each
        //                                 string's (length+data) pair
        //   01000000 61 — "a" prefixed with 4-byte length
        //   02000000 6263 — "bc"
        //   03000000 646566 — "def"
        let expected =
            hex("0c0000000c0000000d0000000f000000010000006102000000626303000000646566");
        let v: Vec<String> = vec!["a".into(), "bc".into(), "def".into()];
        assert_eq!(v.packed(), expected);
        assert_eq!(<Vec<String>>::unpacked(&expected).unwrap(), v);
    }

    #[test]
    fn fracpack_option_u32_some_cross() {
        // C++: psio1::to_frac(std::optional<uint32_t>{42}) → "040000002a000000"
        // Pointer-relative offset 4 + u32 value 42.
        let expected = hex("040000002a000000");
        let v: Option<u32> = Some(42);
        assert_eq!(v.packed(), expected);
        assert_eq!(<Option<u32>>::unpacked(&expected).unwrap(), Some(42));
    }

    #[test]
    fn fracpack_option_u32_none_cross() {
        // C++: psio1::to_frac(std::optional<uint32_t>{}) → "01000000"
        // Sentinel value 1 (< sizeof(offset)) indicates None.
        let expected = hex("01000000");
        let v: Option<u32> = None;
        assert_eq!(v.packed(), expected);
        assert_eq!(<Option<u32>>::unpacked(&expected).unwrap(), None);
    }

    // ── Documentation-only: formats without Rust impls ───────────────────────
    //
    // The following fixtures are recorded for reference. They prove the C++
    // encoder output for each format on the shared shapes; they will become
    // live Rust cross-validation tests when the corresponding Rust impls
    // are added.
    //
    // str_hello.bincode    = "050000000000000068656c6c6f"  (u64 length)
    // str_hello.borsh      = "0500000068656c6c6f"           (u32 length)
    // str_hello.ssz        = "68656c6c6f"                    (no prefix)
    // vec_u32_123.bincode  = "0300000000000000010000000200000003000000"
    // vec_u32_123.borsh    = "03000000010000000200000003000000"
    // vec_u32_123.ssz      = "010000000200000003000000"
    // opt_u32_42.bincode   = "012a000000" (selector + payload)
    // opt_u32_42.borsh     = "012a000000"
    // opt_u32_42.ssz       = "012a000000" (Union[null, T])
    // opt_u32_42.pssz32    = "2a000000"   (no selector — fixed inner)
}

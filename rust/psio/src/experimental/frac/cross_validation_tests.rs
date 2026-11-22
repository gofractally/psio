//! The Rust frac encoder produces the bytes the C++ `frac32` encoder does on
//! canonical shapes. The expected bytes are C++ output.

#[cfg(test)]
mod tests {
    use crate::experimental::frac::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len())
            .step_by(2)
            .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn frac_u32_matches_cpp() {
        // C++: psio1::to_frac(std::uint32_t{0xDEADBEEF}) → "efbeadde"
        let expected = hex("efbeadde");
        assert_eq!(0xDEADBEEFu32.packed(), expected);
        assert_eq!(u32::unpacked(&expected).unwrap(), 0xDEADBEEF);
    }

    #[test]
    fn frac_string_matches_cpp() {
        // C++: psio1::to_frac(std::string("hello")) → "0500000068656c6c6f"
        // (4-byte LE length prefix + raw bytes).
        let expected = hex("0500000068656c6c6f");
        let s = "hello".to_string();
        assert_eq!(s.packed(), expected);
        assert_eq!(String::unpacked(&expected).unwrap(), "hello");
    }

    #[test]
    fn frac_vec_u32_matches_cpp() {
        // C++: psio1::to_frac(std::vector<uint32_t>{1,2,3})
        //       → "0c000000010000000200000003000000"
        let expected = hex("0c000000010000000200000003000000");
        let v = vec![1u32, 2, 3];
        assert_eq!(v.packed(), expected);
        assert_eq!(<Vec<u32>>::unpacked(&expected).unwrap(), v);
    }

    #[test]
    fn frac_vec_string_matches_cpp() {
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
    fn frac_option_u32_some_matches_cpp() {
        // C++: psio1::to_frac(std::optional<uint32_t>{42}) → "040000002a000000"
        // Pointer-relative offset 4 + u32 value 42.
        let expected = hex("040000002a000000");
        let v: Option<u32> = Some(42);
        assert_eq!(v.packed(), expected);
        assert_eq!(<Option<u32>>::unpacked(&expected).unwrap(), Some(42));
    }

    #[test]
    fn frac_option_u32_none_matches_cpp() {
        // C++: psio1::to_frac(std::optional<uint32_t>{}) → "01000000"
        // Sentinel value 1 (< sizeof(offset)) indicates None.
        let expected = hex("01000000");
        let v: Option<u32> = None;
        assert_eq!(v.packed(), expected);
        assert_eq!(<Option<u32>>::unpacked(&expected).unwrap(), None);
    }
}

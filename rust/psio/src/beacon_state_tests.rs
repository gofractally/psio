//! Phase J — BeaconState smoke test. Port the Ethereum Phase-0 Validator
//! struct to Rust and verify cross-validation against a C++-emitted
//! hex fixture. If this passes, it means the Rust SSZ port is ready
//! for real Ethereum consensus-layer workloads.

#[cfg(test)]
mod tests {
    use crate::ssz::{from_ssz, to_ssz};

    // Standalone DWNC Validator kept in the test module for the cross-val
    // fixture below; `crate::beacon_types::Validator` is the production
    // copy used by BeaconState and the bench.
    #[derive(Debug, PartialEq, Eq)]
    struct Validator {
        pubkey:                       [u8; 48],
        withdrawal_credentials:       [u8; 32],
        effective_balance:            u64,
        slashed:                      bool,
        activation_eligibility_epoch: u64,
        activation_epoch:             u64,
        exit_epoch:                   u64,
        withdrawable_epoch:           u64,
    }
    ssz_struct_dwnc!(Validator {
        pubkey:                       [u8; 48],
        withdrawal_credentials:       [u8; 32],
        effective_balance:            u64,
        slashed:                      bool,
        activation_eligibility_epoch: u64,
        activation_epoch:             u64,
        exit_epoch:                   u64,
        withdrawable_epoch:           u64,
    });

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i+2], 16).unwrap()).collect()
    }

    fn sample_validator() -> Validator {
        let mut v = Validator {
            pubkey: [0u8; 48],
            withdrawal_credentials: [0u8; 32],
            effective_balance: 32_000_000_000,
            slashed: true,
            activation_eligibility_epoch: 10,
            activation_epoch: 20,
            exit_epoch: u64::MAX,
            withdrawable_epoch: u64::MAX,
        };
        for i in 0..48 { v.pubkey[i] = (i + 1) as u8; }
        for i in 0..32 { v.withdrawal_credentials[i] = (100 + i) as u8; }
        v
    }

    #[test]
    fn cpp_ssz_validator_byte_identical() {
        // Fixture emitted by C++ psio1::convert_to_ssz(eth::Validator) on
        // 2026-04-23. Must be byte-identical to our Rust port's output.
        let expected = hex(concat!(
            "0102030405060708090a0b0c0d0e0f10",
            "1112131415161718191a1b1c1d1e1f20",
            "2122232425262728292a2b2c2d2e2f30",
            "6465666768696a6b6c6d6e6f70717273",
            "7475767778797a7b7c7d7e7f80818283",
            "0040597307000000",
            "01",
            "0a00000000000000",
            "1400000000000000",
            "ffffffffffffffff",
            "ffffffffffffffff",
        ));
        let v = sample_validator();
        assert_eq!(to_ssz(&v), expected);
        let back = from_ssz::<Validator>(&expected).unwrap();
        assert_eq!(back, v);
        assert_eq!(expected.len(), 121, "Phase 0 Validator wire size");
    }

    #[test]
    fn validator_round_trip() {
        let v = sample_validator();
        let b = to_ssz(&v);
        assert_eq!(from_ssz::<Validator>(&b).unwrap(), v);
    }

    #[test]
    fn validator_view_named_fields() {
        // Zero-copy access by field name via the generated trait — the
        // Rust-idiomatic replacement for C++'s `view.field<I>()`.
        use super::tests::ValidatorSszAccessors;
        let v = sample_validator();
        let b = to_ssz(&v);
        let view: crate::ssz_view::SszView<Validator> = crate::ssz_view::ssz_view_of(&b);
        assert_eq!(view.effective_balance().get(), 32_000_000_000);
        assert_eq!(view.slashed().get(), true);
        assert_eq!(view.activation_epoch().get(), 20);
        assert_eq!(view.exit_epoch().get(), u64::MAX);
        // Array-typed field works too — pubkey[5] should be 6.
        assert_eq!(view.pubkey().get(5).get(), 6);
        assert_eq!(view.withdrawal_credentials().get(0).get(), 100);
    }

    // ── Full Phase 0 BeaconState decode / round-trip on real mainnet data ────
    //
    // Exercises every BeaconState field — primitives, inline fixed arrays
    // (boxed to avoid stack overflow), bounded lists of fixed + variable
    // elements, bitvector, nested reflected structs. Byte-identical
    // round-trip proves wire parity with Lighthouse, Prysm, Teku, and
    // every other spec-conforming consensus client.
    //
    // File: /tmp/beacon_data/mainnet-genesis.ssz (≈5.15 MiB, 21 063
    // validators). Fetched by the C++ bench setup:
    //   curl -sL -o /tmp/beacon_data/mainnet-genesis.ssz \
    //     https://github.com/eth-clients/mainnet/raw/main/metadata/genesis.ssz
    // Skipped if the file is not present — CI builds without internet
    // still get the unit-test Validator coverage above.

    #[test]
    fn mainnet_genesis_round_trip() {
        let path = "/tmp/beacon_data/mainnet-genesis.ssz";
        let Ok(raw) = std::fs::read(path) else {
            eprintln!("skipped: {path} not present", path = path);
            return;
        };

        use crate::beacon_types::{
            BeaconState, BeaconStateSszAccessors, ValidatorSszAccessors,
        };
        use crate::ssz_view::{ssz_view_of, SszView};

        // Heap-allocate — by-value BeaconState is ~2.56 MiB on the stack
        // and would overflow immediately. Mirrors the C++ side's
        // `std::make_unique<eth::BeaconState>()`.
        let state = Box::new(from_ssz::<BeaconState>(&raw).expect("decode"));

        // Sanity: mainnet genesis slot is 0, validator count matches the
        // well-known deposit contract snapshot.
        assert_eq!(state.slot, 0, "genesis slot must be 0");
        assert_eq!(state.validators.len(), 21_063,
            "mainnet genesis has 21063 validators");
        assert_eq!(state.balances.len(), 21_063);

        // Encode back and compare byte-for-byte with the input buffer.
        let re = to_ssz(&*state);
        assert_eq!(re.len(), raw.len(),
            "re-encode size must equal input ({} vs {})",
            re.len(), raw.len());
        assert_eq!(re, raw,
            "mainnet genesis BeaconState re-encode must be byte-identical");

        // Zero-copy view: reach into the bytes without decoding.
        let v: SszView<BeaconState> = ssz_view_of(&raw);
        assert_eq!(v.slot().get(), 0);
        // First validator's pubkey, byte 0 — matches decoded.
        let first_pk_byte0 = v.validators().get(0).pubkey().get(0).get();
        assert_eq!(first_pk_byte0, state.validators.inner()[0].pubkey[0]);
    }

    #[test]
    fn mainnet_genesis_validate() {
        let path = "/tmp/beacon_data/mainnet-genesis.ssz";
        let Ok(raw) = std::fs::read(path) else { return; };
        use crate::beacon_types::BeaconState;
        use crate::ssz::ssz_validate;
        ssz_validate::<BeaconState>(&raw).expect("structural validation must pass");
    }

    // ── pSSZ (psiSSZ) round-trip on the same BeaconState ────────────────────
    //
    // pSSZ is not wire-compatible with SSZ — it adds an extensibility
    // header and narrower offset widths — so we round-trip through
    // itself: decode SSZ from mainnet genesis, encode as pSSZ, decode
    // pSSZ back, then encode as pSSZ again. Second encoding must equal
    // first.
    #[test]
    fn mainnet_genesis_pssz_round_trip() {
        let path = "/tmp/beacon_data/mainnet-genesis.ssz";
        let Ok(raw) = std::fs::read(path) else { return; };

        use crate::beacon_types::BeaconState;
        use crate::pssz::{from_pssz, to_pssz, Pssz32};
        use crate::ssz::from_ssz;

        // Decode SSZ → produce native BeaconState.
        let state = Box::new(from_ssz::<BeaconState>(&raw).expect("ssz decode"));

        // Encode as pSSZ (Pssz32 — same 4-byte offsets as SSZ so sizing
        // is comparable; Pssz8/16 would be smaller but need per-subfield
        // max-size bounds to decide).
        let pssz_bytes = to_pssz::<Pssz32, _>(&*state);

        // Decode pSSZ back.
        let decoded = Box::new(
            from_pssz::<Pssz32, BeaconState>(&pssz_bytes).expect("pssz decode"));

        // Assert the decoded state equals the original.
        assert_eq!(*state, *decoded,
            "pSSZ round-trip must reproduce the original BeaconState");

        // And encoding again produces identical pSSZ bytes.
        let pssz2 = to_pssz::<Pssz32, _>(&*decoded);
        assert_eq!(pssz_bytes, pssz2,
            "pSSZ encode must be deterministic");

        eprintln!("pSSZ BeaconState: {} bytes ({:+} vs SSZ)",
            pssz_bytes.len(),
            pssz_bytes.len() as isize - raw.len() as isize);
    }
}

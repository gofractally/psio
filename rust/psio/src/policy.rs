//! Validation policy — type machinery for the format-tagged
//! `validate<F, T, P>(bytes)` and `validate<F, T>(bytes, policy)`
//! CPOs described in `docs/psio-overview.md` §2.4.
//!
//! The same trait works for both call shapes:
//!
//!   * **Compile-time policy** — caller passes a zero-sized preset
//!     type as the `P` parameter:
//!
//!     ```ignore
//!     psio::validate::<Pjson, MyType, _>(bytes, &StrictCanonical)
//!     ```
//!
//!     The `&StrictCanonical` reference points at a ZST; the
//!     trait method calls inline to constants and the optimizer
//!     eliminates the dead branches.
//!
//!   * **Runtime policy** — caller passes a `DynamicPolicy` value
//!     whose fields hold the bool flags decided at runtime:
//!
//!     ```ignore
//!     let p = DynamicPolicy { verify_canonical: cfg.strict, .. };
//!     psio::validate::<Pjson, MyType, _>(bytes, &p)
//!     ```
//!
//! The flag taxonomy follows `docs/spec-evolution-notes.md` E-002.
//! `bounds_safety` is implicit and not exposed — disabling it would
//! let `decode` produce out-of-bounds reads, which no legitimate
//! caller needs.

/// Validation extent for the format-tagged `validate` CPO. Each method
/// returns `true` if the corresponding invariant must be checked,
/// `false` if it may be skipped. Defaults are `false` so a minimal
/// `impl ValidationPolicy for MyType {}` produces "bounds-only" behavior.
pub trait ValidationPolicy {
    /// Reject reserved tag codes / reserved low-nibble bits.
    fn reject_reserved(&self) -> bool { false }

    /// Verify slot-table monotonicity and offset-in-range for §5
    /// containers (pjson; analogues in other formats).
    fn verify_slot_invariants(&self) -> bool { false }

    /// Verify per-key prefilter hash bytes (formats that carry an
    /// inline hash — e.g. pjson §5.2's hash-byte array).
    fn verify_hash_bytes(&self) -> bool { false }

    /// Verify numeric_string / similar structural type-tag rules
    /// (e.g., pjson §4.8: inner code must be one of {2..7}).
    fn verify_numeric_string(&self) -> bool { false }

    /// Verify the wire is strictly canonical: smallest-form integers,
    /// smallest bit-exact ieee_float width, canonical NaN bit pattern,
    /// no negative-zero negint, smallest slot widths, decimal mantissa
    /// trailing-zero trim. Required for content-addressable / signed
    /// payloads under the byte-equality contract (pjson §15.7;
    /// analogues in other formats).
    fn verify_canonical(&self) -> bool { false }

    /// Verify `is_sorted` array hints actually hold (E-001 in pjson).
    /// Cheap for typed arrays; expensive for generic.
    fn verify_sorted_hints(&self) -> bool { false }
}

/// Bounds-only — every flag false. Use for hot-path repeat reads of
/// already-trusted buffers (e.g., immediately after a strict
/// pre-flight on the same data).
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct JustDontCrash;
impl ValidationPolicy for JustDontCrash {}

/// Production default — every malformedness that would silently
/// produce wrong answers is rejected. Skips canonical-encoding and
/// sorted-hint checks (those are extra work that only matters for
/// content-addressing).
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct DefaultSafe;
impl ValidationPolicy for DefaultSafe {
    #[inline] fn reject_reserved(&self) -> bool { true }
    #[inline] fn verify_slot_invariants(&self) -> bool { true }
    #[inline] fn verify_hash_bytes(&self) -> bool { true }
    #[inline] fn verify_numeric_string(&self) -> bool { true }
}

/// Strict canonical — content-addressable / signed-payload pre-flight.
/// Two equal logical values MUST yield byte-equal wires.
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct StrictCanonical;
impl ValidationPolicy for StrictCanonical {
    #[inline] fn reject_reserved(&self) -> bool { true }
    #[inline] fn verify_slot_invariants(&self) -> bool { true }
    #[inline] fn verify_hash_bytes(&self) -> bool { true }
    #[inline] fn verify_numeric_string(&self) -> bool { true }
    #[inline] fn verify_canonical(&self) -> bool { true }
    #[inline] fn verify_sorted_hints(&self) -> bool { true }
}

/// Runtime policy — for callers parameterizing validation extent at
/// run time (CLI flags, config). Compile-time use should prefer the
/// preset ZST types above for zero-cost dispatch.
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub struct DynamicPolicy {
    pub reject_reserved: bool,
    pub verify_slot_invariants: bool,
    pub verify_hash_bytes: bool,
    pub verify_numeric_string: bool,
    pub verify_canonical: bool,
    pub verify_sorted_hints: bool,
}

impl ValidationPolicy for DynamicPolicy {
    fn reject_reserved(&self)         -> bool { self.reject_reserved }
    fn verify_slot_invariants(&self)  -> bool { self.verify_slot_invariants }
    fn verify_hash_bytes(&self)       -> bool { self.verify_hash_bytes }
    fn verify_numeric_string(&self)   -> bool { self.verify_numeric_string }
    fn verify_canonical(&self)        -> bool { self.verify_canonical }
    fn verify_sorted_hints(&self)     -> bool { self.verify_sorted_hints }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn just_dont_crash_is_all_false() {
        let p = JustDontCrash;
        assert!(!p.reject_reserved());
        assert!(!p.verify_slot_invariants());
        assert!(!p.verify_hash_bytes());
        assert!(!p.verify_numeric_string());
        assert!(!p.verify_canonical());
        assert!(!p.verify_sorted_hints());
    }

    #[test]
    fn default_safe_rejects_silent_wrong_answers_only() {
        let p = DefaultSafe;
        assert!(p.reject_reserved());
        assert!(p.verify_slot_invariants());
        assert!(p.verify_hash_bytes());
        assert!(p.verify_numeric_string());
        // Canonical / sorted-hint extras are off.
        assert!(!p.verify_canonical());
        assert!(!p.verify_sorted_hints());
    }

    #[test]
    fn strict_canonical_is_all_true() {
        let p = StrictCanonical;
        assert!(p.reject_reserved());
        assert!(p.verify_slot_invariants());
        assert!(p.verify_hash_bytes());
        assert!(p.verify_numeric_string());
        assert!(p.verify_canonical());
        assert!(p.verify_sorted_hints());
    }

    #[test]
    fn dynamic_policy_reflects_fields() {
        let p = DynamicPolicy {
            reject_reserved: true,
            verify_canonical: true,
            ..DynamicPolicy::default()
        };
        assert!(p.reject_reserved());
        assert!(p.verify_canonical());
        assert!(!p.verify_slot_invariants());      // unset
    }

    /// The preset ZSTs are zero-sized; passing `&StrictCanonical` to
    /// a `<P: ValidationPolicy>` generic doesn't cost a runtime byte
    /// (compiles to monomorphized code with constant-folded branches).
    #[test]
    fn preset_types_are_zero_sized() {
        assert_eq!(core::mem::size_of::<JustDontCrash>(),    0);
        assert_eq!(core::mem::size_of::<DefaultSafe>(),      0);
        assert_eq!(core::mem::size_of::<StrictCanonical>(),  0);
    }

    /// A generic function that consumes `&P` calls the trait methods
    /// on it. With ZST presets the calls inline to constants; with
    /// `DynamicPolicy` they read fields. This test exercises both
    /// paths through the same generic body, demonstrating the API
    /// supports both call shapes from the design doc.
    fn count_set_flags<P: ValidationPolicy>(p: &P) -> usize {
        [p.reject_reserved(), p.verify_slot_invariants(),
         p.verify_hash_bytes(), p.verify_numeric_string(),
         p.verify_canonical(), p.verify_sorted_hints()]
            .iter().filter(|b| **b).count()
    }

    #[test]
    fn generic_consumes_both_compile_time_and_runtime_policies() {
        // Compile-time path: ZST preset.
        assert_eq!(count_set_flags(&JustDontCrash),    0);
        assert_eq!(count_set_flags(&DefaultSafe),      4);
        assert_eq!(count_set_flags(&StrictCanonical),  6);

        // Runtime path: struct with bool fields.
        let p = DynamicPolicy {
            reject_reserved: true, verify_canonical: true,
            ..DynamicPolicy::default()
        };
        assert_eq!(count_set_flags(&p), 2);
    }
}

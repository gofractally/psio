# Exception-disabled codec preparation

The focused consumer `cpp/tests/guest_codec_check.cpp` uses the experimental frac32
(`psio::experimental`) and PSSZ codecs, status CPOs, and annotation metadata. It introduces no format,
wire framing, or replacement runtime validation walker. Valid encodings remain
unchanged.

Native exception-enabled wrapper overflows still throw `std::length_error`;
existing PSSZ unchecked and throwing-CPO failures still throw `codec_exception`.
When exceptions are disabled these same unchecked failures abort. They are not
the checked input path: `validate` reports malformed input using `codec_status`.
Callers must check that status before decoding.

Guest `validate_strict` first runs the format's structural status validator.
It does not materialize a value to discover unsupported semantic checks: a
wrapper or custom adapter can reject construction by aborting, which cannot be
caught in a guest. A conservative metadata-only capability check permits strict
structural success for known scalars, strings, arrays, vectors, options,
variants and aggregate reflected graphs requiring no semantic materialization.
A seen-type pack terminates recursive type graphs. The only annotation types
recognized as needing no semantic work are DWNC and field numbers.

Runtime specs, length bounds, custom binary adapters, unknown annotations or
wrappers, and nonaggregate reflected construction return an explicit
`guest strict semantic validation unsupported` status. This includes currently
unsupported constraints rather than accepting them silently. The existing native
semantic materialization and catch behavior is unchanged. This is a preparation
gate, not a fully implemented guest strict-semantic validator.

The consumer checks native and guest positive paths, malformed offsets and
truncation, invalid UTF-8 semantics, explicit unsupported results, recursive
metadata, and an adapter whose guest decode deliberately aborts if reached.
Its argument selectors 1–5 intentionally exercise unchecked wrapper, size-cap,
and throwing-validator failure paths. They are expected exceptions natively and
aborts in a guest; ordinary status validation must return normally.

Since the evolution merge the PSSZ status validator, which is what a guest
uses, resolves every record from the writer's fixed-region header (a header
larger than its frame is rejected), accepts pruned and unknown optional suffix
fields, and rejects a stored trailing None. Guest `validate_strict` additionally
rejects unknown fields and checks every plain string for UTF-8 through the same
status walker; plain `validate` passes strings through as raw bytes. It still
does not materialize, so the native canonical re-encode comparison is not run
in a guest.

The status validator also enforces a member's declared `length_bound` on
strings and lists, so a guest rejects an out-of-bound member in `validate`.
Guest `validate_strict` still reports the explicit unsupported status for a
type that carries length bounds, as described above.

Independent activation gates remain open: schema and language parity (Rust has
no evolution or pruning) and all other format headers. Passing the guest preparation consumer does not qualify the entire
SDK.

## PSSZ strict validation in a guest

`psio::validate_strict<T>(psio::pssz{}, bytes)` (and the explicit-width tags)
no longer has an "unsupported" answer. It runs the same steps in a build
without exceptions as natively, and returns the same statuses:

1. the structural validation in exact mode — unknown optional suffix fields
   are refused, an adapter's payload is put to its `validate_strict`, a
   nested value is checked as its type by this same function;
2. the value is decoded from the accepted bytes, and its semantic specs run;
3. the value is sized and encoded again through the checked sizing pass, and
   the bytes must be the input ("pssz: noncanonical encoding" otherwise).

Step 2 is not a new requirement on user code: the plain validator already
materializes members to check invariants and order annotations, and every
checking reader calls an adapter's `decode` only on a payload its `validate`
accepted (`psio/adapter.hpp`). An adapter or constructor that aborts on bytes
its own `validate` accepted is a defect of that type in every checked entry
point, not only in this one.

The metadata-only capability check
(`detail::vstrict::can_validate_without_materialization`) and the
`guest strict semantic validation unsupported` status remain for the
experimental frac32 prototype and for `psio::ssz`. Everything above that
describes them is about those two formats.
`cpp/tests/pssz_strict_noexceptions.cpp` runs the PSSZ cases with
`-fno-exceptions -fno-rtti`.

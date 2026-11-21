# Exception-disabled codec preparation

The focused consumer `cpp/tests/guest_codec_check.cpp` uses the existing frac32
and PSSZ codecs, status CPOs, and annotation metadata. It introduces no format,
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

Independent activation gates remain open: appended optional fields, schema and
language parity, complete malformed/canonical validation, typed length-bound
enforcement, zero-size containers/options, and all other format headers. In
particular, the existing PSSZ record validator treats the writer header as
informational and accepts a header larger than its frame; the failed audit
probe is retained outside this change. Passing the guest preparation consumer
does not qualify that behavior or the entire SDK.

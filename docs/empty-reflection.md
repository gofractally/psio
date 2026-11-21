# Zero-field reflected records

`PSIO_REFLECT(T)` exposes the metadata template names needed by generic empty
pack expansions. It still reports zero members: no field index is valid.
Borrowing record views can hold an empty record's bytes without probing a
nonexistent field zero. Nonempty record layout and width selection are unchanged.

The empty action-data consumer exercises extensible and
`definition_will_not_change` records using the existing `pssz8`, `pssz16`,
`pssz32`, and automatic `pssz` formats. It checks encode, validate, decode,
byte-preserving re-encode, borrowed view identity, and empty reflection visits.
A separate compile-failure probe verifies that `get<0>()` remains invalid.

Focused qualification is reproducible with `cpp/tests/qualify_empty_reflection.py`.
The retained results are indexed by `logs/empty-reflection-summary.json`.
Modern glibc requires the explicit test-only `--catch-no-posix-signals` option
for the bundled Catch2 v2 runner's obsolete `SIGSTKSZ` constant expression.
This disables its crash-reporting handler, without disabling assertions or
process-failure detection. The library's signal policy is unchanged.

The WASI consumer was compiled to an object with the existing SDK and
`PSIO_EXCEPTIONS_ENABLED=1`. This does not qualify guest linking, guest execution,
the existing incomplete exception-disabled profile, or unrelated codecs.

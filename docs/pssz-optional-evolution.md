# Optional suffix evolution

PSSZ payloads contain no schema identifiers or writer schema. A reader uses its own compiled layout. Compatible record revisions preserve every initial field's type, order, annotations and layout, then append **only optional fields**. All revisions in a compatible lineage retain the same offset width. A schema upgrade checker enforces this contract during development; no schema registry is needed to read values.

An extensible record already contains a width-sized fixed-region size header. Every appended optional occupies one width-sized offset slot when present in that region. An older reader can therefore read unknown suffix slots as offsets without knowing the appended payload types. These offsets also delimit its last known variable payload. No payload lengths, end-offset slots or per-value schema IDs are added.

For example, at width 1, `{name: String}` with `name="a"` is `01 01 61`. Appending `extra: Option<String>` with `Some("b")` produces `02 02 03 61 01 62`. The old reader uses the unknown offset `03` to stop `name` after `61`; it reports that unknown fields were present. `extra=None` produces exactly the original three bytes, because trailing absent optional fields are pruned.

The reader merges offsets for present known variable fields and unknown optional suffix slots. The first payload begins at the writer's fixed-region size; offsets are monotone, within the enclosing value, and may be equal for empty optional payloads. The unknown suffix length is a multiple of the offset width relative to the known fixed region. The entire fixed region need not be aligned: an initial boolean followed by a variable field remains valid. A header that cuts through a known slot or omits a required field is invalid. Missing trailing optionals synthesize `None` without traversing a child node.

An optional payload with a positive minimum encoded size needs no selector: empty means `None`, nonempty means `Some`. A payload that can itself encode empty needs a selector byte to distinguish `Some(empty)` from `None`. This rule uses minimum encoded size, independently of fixed-size classification. An extensible empty record has a positive header size; a zero-sized `definition_will_not_change` unit does not. Only trailing `None` fields are pruned. DWNC layouts are unchanged and cannot acquire suffix fields.

Checked compatible reads validate the whole known frame, accept and report unknown suffixes, and propagate that report through nested containers. They cannot validate the semantics of unknown payload types. Exact-schema reads reject unknown fields. Canonical validation additionally rejects an encoding that differs from canonical re-encoding, including an explicitly stored trailing `None`. Exact-schema validation and canonical validation are distinct modes; an exact reader may accept a well-formed older unpruned representation.

The supported checked walkers start at depth zero and allow nodes through depth 64. Present record fields, sequence elements, `Some` payloads and variant payloads add one child node. A transparent `Box` adds none. A pruned missing optional adds no child node. Strings are one UTF-8 node; nonempty byte vectors have byte element children. A variable-count list of zero-sized fixed elements cannot represent a nonzero count and is rejected; fixed-count arrays retain their declared element count.

The optional suffix contract does not choose a final psibase width profile. Compact width selection and compatible-lineage pinning must be qualified together before the wire cutover; automatically changing width after appending a field is incompatible.

## Status on the trunk (`codex/psibase-core-evolution`)

The evolution line was merged onto the trunk's per-type automatic widths. The
following supersedes the paragraphs above where they differ.

**Width.** Evolution and pruning are implemented in the shared `Auto` recursion
of `pssz.hpp`, so the automatic entry point `psio::pssz` (each record owns its
header and slots at its own width; children use theirs) and the explicit
`pssz_<1|2|4>` regression tags behave identically. A lineage keeps its width
through its declared `maxDynamicData` budget, which also caps how far optional
extension may grow a value.

**Pruning is mandatory.** A value has exactly one encoding. The encoder always
drops trailing absent optional fields and writes the reduced fixed-region
header. `validate`, `validate_or_throw`, `validate_strict`,
`validate_pssz_compatible`, `decode`, the in-place vector element decoder and
the schema-IR runtime reads all reject a record whose last written slot is an
absent optional, natively and in exception-disabled builds (status paths return
an error; fail-fast paths throw or abort). Only optional fields can be absent,
so "trailing default" means "trailing None": a required field is never
omitted, and a present-but-empty value (`Some("")`, `Some([])`, `Some(None)`)
is a value and is kept. Two limits follow from the wire carrying no schema:

- An older reader cannot interpret the writer's last slot when it is one the
  reader does not know, so it cannot judge pruning there. It accepts the frame
  and reports unknown fields.
- DWNC records have no header, are never pruned and never extended; every
  field is always present, so the rule does not apply to them.

**Optional presence.** Resolved in favour of `docs/pssz-spec.md` 5.5 and the
rule at the top of this document: a selector only when the payload's minimum
encoded size is zero. The typed codec, the schema-IR runtime codec, the shared
fixtures and Rust agree. The trunk's explicit-profile goldens that pinned a
selector for `optional<record>` were regenerated.

**Strings.** A plain string is `Vector(u8)` carrying raw bytes: encode, decode,
default validation and views do not check UTF-8. Strict validation checks every
string, the `utf8_spec` annotation is checked by strict validation, and the
`utf8_string<N>` wrapper carries the constraint in its type (enforced by
encode, decode and both validation modes). `utf8_string<>` is the unbounded
UTF-8 string. The user type encodes the restriction, and pairs across
languages as: C++ `std::string` = Rust `RawString` (raw bytes), C++
`utf8_string<>` = Rust `String` (UTF-8), `utf8_string<N>` = UTF-8 of at most N
bytes. A bounded `utf8_string<N>` contributes N bytes to the inferred width
bound wherever it appears (its bound is enforced), like a `length_bound` member
annotation on a `std::string` (which narrows the width but is not itself
enforced by any codec path). `utf8_string<>` is unbounded.

**Trailing data.** A buffer holds exactly one value. Fixed-size values must
occupy exactly their span in decode as well as validate.

**Compile-time check.** `psio::pssz_evolves_from<Old, New>` is true iff New is
Old plus optional fields at an unchanged automatic width and DWNC status.

**Rust.** `rust/psio` implements the same rules through one frame parser
(`pssz_checked::record_spans_into`, a port of `pssz_record_spans` and
`record_has_unpruned_tail`) shared by the derives,
tuples, variant payloads, views and the schema-driven codec: readers honor the
writer's header, the encoder always prunes, and every reader rejects a stored
trailing None. `pssz::pssz_evolves_from::<Old, New>()` is the `const fn`
counterpart of the compile-time check. Rust `String` pairs with the UTF-8
string types and `RawString` with `std::string`; see
`docs/pssz-rust-profile.md`.

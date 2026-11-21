# Zero-sized payloads in the existing C++ PSSZ codec

The reference is immutable-width checkpoint
`7dff9e327e23cfc4307f5cdce2af7cc96f357e1c`, retained in its clean original
checkout. A zero-field DWNC record has a legitimate zero-byte payload. The old
optional shortcut used no selector for every fixed payload, so both None and
Some(Unit) encoded as empty bytes and Some decoded as None.

The optional repair uses the existing one-byte presence selector when the inner
payload is variable **or fixed with size zero**. None is `00`, Some(Unit) is
`01`, and its Unit payload remains empty. Nonzero fixed payloads retain their
existing length-based optional representation. No framing, record identifiers,
new width/profile, or length prefix is introduced. Sizing, encoding, decoding,
both validators, and borrowed views use one shared selector predicate.

Zero-size fixed payloads must have empty spans. This also bounds the payload of
Some(Unit) and zero-size fixed-count arrays; malformed extra bytes are rejected.
The new zero-size optional decoder rejects missing/invalid selectors and extra
bytes directly. The behavior of unrelated variable-payload optional decoders is
outside this focused change.

The first checkpoint qualifies optional presence on macOS Clang22: 64 existing
and focused native test cases, 1,173 assertions, plus the empty-action consumer;
25 focused/type-width cases and 1,084 assertions under ASan/UBSan with no
suppressions. The existing 24 explicit-profile golden encodings remain unchanged.
Raw commands, source/binary hashes, and output are in
`logs/zero-optional-20261121T214303Z-mac-clang22/results.json`. Linux qualification
is pending the higher-priority integrated storage build.

The retained baseline reproducer also demonstrates independent sequence problems:
nonempty vector<Unit> loses its count, decode/validation divide by zero on
nonempty bytes (UBSan reports retained), and array<Unit,3> accepted trailing junk.
The separate sequence repair now rejects nonempty fixed-zero vectors during
sizing/encoding and rejects nonempty byte spans in decode and both validators
before any divide/modulo. Empty vectors remain valid. An encoding size failure
leaves an append destination unchanged. Fixed-count arrays retain their
compile-time count without a wire count; zero-count arrays also reject extra
bytes even when their element type is variable.

The existing variable classification of array<String,0> remains unchanged. A
vector of that type already carries an offset table, which preserves its count
even though each payload is empty, and remains supported. There is no type
reclassification or context-dependent width change. Borrowed C++ views
require validated input; the existing noexcept zero-size vector-count guard
returns zero without dividing, and does not itself certify arbitrary input.

The final sequence source passes 76 native cases / 2,697 assertions plus the
empty-action consumer, and 37 ASan/UBSan cases / 2,608 assertions on macOS. Exact
sources and commands are recorded in
`logs/zero-sequence-20261121T214747Z-mac-clang22-qualified/results.json`. The same
sanitized baseline reproducer now passes all five modes. Earlier setup/prototype
attempts are retained separately, including a test fixture that accidentally
requested decoding a const-qualified target type; the corrected test does not
expand the decoder API. Linux remains deferred behind the integrated storage
qualification.

Typed length-bound validation and canonical validation of nonzero fixed payloads
(including strict boolean canonicality and unused bitvector bits) remain
independent activation gates. Ordinary boolean reads remain permissive under the
documented specification; normalization of memcpy-decoded boolean fields is a
separate safety repair.
These focused checks are not global strict PSSZ or SDK cutover qualification.

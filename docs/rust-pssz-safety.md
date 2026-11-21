# Existing Rust PSSZ input and memory safety

This focused repair starts at published
`f6b10ec7d7bba4022cd1c9394f88ad1440b4b2b7`. It changes the existing Rust
explicit-width PSSZ implementation, not the wire format, automatic width API,
derive features, schema IR, or other format implementations. Original user
checkouts remain untouched.

## Why the old memory copies were unsound

`PsszPack` and `PsszUnpack` are safe public traits. An implementation's
`IS_FIXED_SIZE` and `FIXED_SIZE` describe its wire representation. Matching
`FIXED_SIZE` to `size_of::<T>()` does not prove that T has initialized padding,
accepts arbitrary bit patterns, or uses its memory bytes as its wire format.
Nevertheless, the old array/vector/boxed-array and DWNC record paths used that
match to copy bytes into arbitrary T or expose its raw memory as initialized
bytes.

The consequences include invalid Rust booleans from `Vec<bool>`/`[bool; N]`,
forged owning pointers when a custom fixed codec contains a Box, reads of
uninitialized padding while packing custom types, and bypassed custom validity
checks. These undefined-behavior paths are established by the public trait
contracts and the exact reference source; they are not executed on the unsafe
baseline. The reference boxed-array allocator also permits a zero-sized layout,
and its manual partial-initialization cleanup does not run if a child decoder
panics.

Four memory-safe baseline probes demonstrate independent observable failures:
a custom one-byte wire codec is bypassed, a boolean view accepts 2, record
truncation panics through a Result decoder, and zero-sized vector metadata
divides by zero. Both native qualification drivers compile these probes against
the exact clean reference and retain the expected four failures.

## Corrected contracts

The affected PSSZ modules contain no unsafe blocks or raw memory copies. Packing
calls each element/field's existing codec. Decoding initializes values only
through their existing typed decoders. A Vec owns initialized elements throughout
failure and unwinding; converting its boxed slice to a boxed array avoids raw
allocation and large inline array materialization. A type with Drop and an owning
Box exercises successful decode, codec errors, and child panics for Vec, array,
and boxed-array consumers.

Shared layout helpers check supported widths, checked arithmetic, array/vector
counts, table boundaries, monotone offsets, complete fixed regions, and exact
child spans. Record helpers return stack arrays of field ranges, so generated
borrowed access still needs no heap allocation. Bounded lists check their count
before allocating decoded elements. Fixed Option payloads are bounded to the
child's complete size before primitive decoding or validation.

The existing primitive scalar prefix policy remains: a standalone fixed primitive
may accept trailing bytes. This repair bounds internal container/record child
windows rather than changing that policy globally. The record decoder accepts
the current same-schema fixed-region header and rejects unknown or malformed
header layouts explicitly. Appended-field evolution remains a separate gate.

An empty zero-size-element Vec decodes as empty; a nonempty byte span is rejected.
Encoding a nonempty Vec of zero-size elements now fails explicitly through the
existing infallible API's panic contract, instead of losing the count. Fixed-count
zero-size arrays, including zero-length boxed arrays, remain representable with
their compile-time count. Rust's existing MIN/presence mechanism for
`Option<Zero>` is preserved without adding a prefix or changing C++ behavior.

Stored offsets and record headers use checked explicit-width conversions.
Unsupported profiles and oversized offsets cannot silently truncate. The
infallible encoding API reports unrepresentable output by panic; this repair
does not introduce a second checked encoder or change valid profile bytes.

## Borrowed views and validation

Existing `pssz_view_of`, `PsszView::new`, and the public byte span retain their
signatures. They are unchecked borrowed views: malformed access or an invalid
index can panic, but must not fabricate an invalid Rust value or use unchecked
memory. Boolean access accepts only 0/1. String access checks UTF-8, optional
access checks presence/shape, and sequence/record access uses the shared bounded
layout helpers. Mutating a view's public byte span after validation cannot create
an unsafe invariant.

The additive `PsszView::try_new` and `try_pssz_view_of` helpers validate through
the existing `PsszValidate` trait before borrowing. They are preferred for
untrusted input where that trait is available. Arrays, boxed arrays, bounded
lists/strings, and bit types reuse the existing codec layout for validation.
This is the existing validator contract, not a new promise of full canonical
strictness: scalar trailing-prefix acceptance, for example, remains documented.

Existing record macros continue to require only their existing Pack/Unpack field
traits. A regression record contains a custom owning field with no PsszValidate
implementation to preserve that compatibility. Automatic record validator
generation is deferred; a future canonical macro enhancement must avoid imposing
an unconditional new trait requirement on existing custom fields. No private SDK
codec or duplicate validator is introduced here.

## Qualification and limits

`rust/psio/tests/qualify_pssz_safety.py --help` describes the native driver. It
records exact source/toolchain/lockfile hashes, commands, raw output, host load,
disk headroom, and binary hashes. It compiles the four safe failure probes and
39 valid explicit W1/W2/W4 byte fixtures against the exact clean f6 source,
compares candidate bytes, runs the full existing Rust library/conformance suite
plus focused safety regressions, and repeats the focused regressions in release
mode so checks cannot depend on debug overflow traps.

The focused suite includes invalid bool containers, custom field codec dispatch,
owning-T Drop/error/panic cleanup, truncated records, invalid UTF-8, malformed
offset tables, overflowing type metadata, unsupported formats, zero-sized
elements, nested record views, public view-span replacement, and 4,800 small
malformed sample buffers across W1/W2/W4. It is not an exhaustive security proof
or an input-size/allocation policy for a network endpoint. The two already ignored
library tests remain ignored; full external fixture corpus runs remain separate.

There is no Miri qualification in this checkpoint. The library's other codecs,
including the analogous generic raw copies in SSZ, are outside this focused
PSSZ repair. Complete strict semantic/malformed parity, generated record
validation, empty/tuple expansion, append evolution, Rust/C++/browser automatic
width parity, and the consumer SDK cutover remain independent activation gates.

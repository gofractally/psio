//! What a type declares about its values, checked on a value (C++
//! `psio/value_contract.hpp`).
//!
//! A bound declared on a member is part of the type, whatever format the
//! value arrives in. pSSZ, keys and JSON read the declarations where they
//! read the bytes. A format whose validator follows another specification —
//! FlatBuffers, Cap'n Proto, protobuf — checks the structure of its bytes
//! first and then holds the value they decode to against the declaration:
//! a member's `#[psio(max = N)]` (and what is stated below it: `element`,
//! `keys`, `values`, `at`), `sorted` / `unique` / `unique_keys`, and the
//! type's `#[psio(invariant = "path")]`.
//!
//! The order of the walk is the one of C++, so that both languages report
//! the same violation for the same bytes: the members of a record in
//! declaration order; for each member what it holds first (elements in
//! order, the entries of a map in key order with the key before the mapped
//! value, the alternative an enum holds), then the member's own
//! declarations; the invariant of a type after everything the value holds.
//!
//! This module is what the code of the `Fb*`, `Capnp*` and `Protobuf`
//! derives calls.

use crate::pssz::bounds::Bounds;
use std::borrow::Cow;

/// The types a search for declarations has already entered: a type that
/// holds itself (`struct Tree { children: Vec<Tree> }`) is inspected once.
#[derive(Default)]
pub struct Seen(Vec<&'static str>);

impl Seen {
    pub fn new() -> Self {
        Seen(Vec::new())
    }
    /// Whether `T` is entered for the first time.
    pub fn enter<T: ?Sized>(&mut self) -> bool {
        let name = core::any::type_name::<T>();
        if self.0.contains(&name) {
            return false;
        }
        self.0.push(name);
        true
    }
}

/// The declarations of a member (`S`: the marker type of its annotation) on
/// its value. `length` is the text of a length violation in the format that
/// reports it (`"flatbuf: member length_bound violated"`); the texts of the
/// order constraints are the same in every format.
pub fn member<T: Bounds<S>, S>(value: &T, length: &'static str) -> Result<(), &'static str> {
    match value.check() {
        Ok(()) => Ok(()),
        Err(error) => Err(match error.0 {
            Cow::Borrowed(message) if !message.starts_with("pssz:") => message,
            _ => length,
        }),
    }
}

/// The message of a failed invariant. These formats report a constant
/// text: a message the invariant computed from the value (a `String`) is
/// reported as `fallback`.
pub fn invariant<E: Into<Cow<'static, str>>>(why: E, fallback: &'static str) -> &'static str {
    match why.into() {
        Cow::Borrowed(message) => message,
        Cow::Owned(_) => fallback,
    }
}

/// The answer of a search for declarations, kept by the type it was made
/// for (the derives hold one in a `static` for a type without parameters).
pub struct Declares(core::sync::atomic::AtomicU8);

impl Declares {
    #[allow(clippy::new_without_default)]
    pub const fn new() -> Self {
        Declares(core::sync::atomic::AtomicU8::new(0))
    }
    /// The kept answer, or `search` run once and kept. `search` starts from
    /// the type itself with nothing seen, so the answer does not depend on
    /// where the question came from.
    pub fn get(&self, search: impl FnOnce() -> bool) -> bool {
        use core::sync::atomic::Ordering::Relaxed;
        match self.0.load(Relaxed) {
            1 => false,
            2 => true,
            _ => {
                let answer = search();
                self.0.store(if answer { 2 } else { 1 }, Relaxed);
                answer
            }
        }
    }
}

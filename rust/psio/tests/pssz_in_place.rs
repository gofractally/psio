//! Packing in place: `NestedRef` / `PackedRef` write the inner value straight
//! into the buffer of the enclosing one. The bytes are those of the two-step
//! form (`Nested::new`, `to_pssz` into a `Vec<u8>` member), and three layers
//! around a 1 MiB payload take one allocation of the result's size.

use psio::pssz::{from_pssz, to_pssz, try_to_pssz, validate_pssz, Nested, NestedRef, PackedRef};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::cell::Cell;
use std::sync::Mutex;

struct Counting;
static LIVE: AtomicUsize = AtomicUsize::new(0);
static PEAK: AtomicUsize = AtomicUsize::new(0);
static COUNT: AtomicUsize = AtomicUsize::new(0);
thread_local! {
    // Only the measuring thread is counted: the test harness allocates on
    // its own threads while a test runs.
    static MEASURING: Cell<bool> = const { Cell::new(false) };
}
fn measuring() -> bool {
    MEASURING.try_with(|m| m.get()).unwrap_or(false)
}
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if measuring() {
            let live = LIVE.fetch_add(layout.size(), Relaxed) + layout.size();
            PEAK.fetch_max(live, Relaxed);
            COUNT.fetch_add(1, Relaxed);
        }
        System.alloc(layout)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if measuring() {
            // (Saturating: a block made before the window may be freed in it.)
            let _ = LIVE.fetch_update(Relaxed, Relaxed, |v| Some(v.saturating_sub(layout.size())));
        }
        System.dealloc(ptr, layout)
    }
    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if measuring() {
            // Counted as the new block beside the old one: what a moving
            // reallocation holds at its peak.
            let live = LIVE.fetch_add(new_size, Relaxed) + new_size;
            PEAK.fetch_max(live, Relaxed);
            COUNT.fetch_add(1, Relaxed);
            let _ = LIVE.fetch_update(Relaxed, Relaxed, |v| Some(v.saturating_sub(layout.size())));
        }
        System.realloc(ptr, layout, new_size)
    }
}
#[global_allocator]
static ALLOC: Counting = Counting;
// One measurement at a time (the counters are shared).
static MEASURE: Mutex<()> = Mutex::new(());

/// (peak bytes, allocations) of `f` on this thread. The result is kept
/// alive until the window closes, so the peak includes it.
fn measure<R>(f: impl FnOnce() -> R) -> (R, usize, usize) {
    LIVE.store(0, Relaxed);
    PEAK.store(0, Relaxed);
    COUNT.store(0, Relaxed);
    MEASURING.with(|m| m.set(true));
    let result = f();
    MEASURING.with(|m| m.set(false));
    (result, PEAK.load(Relaxed), COUNT.load(Relaxed))
}

#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct Inner {
    data: Vec<u8>,
}
#[derive(psio::PsszPack, psio::PsszUnpack)]
struct Middle {
    tag: u32,
    inner: Nested<Inner>,
}
#[derive(psio::PsszPack, psio::PsszUnpack)]
struct Outer {
    tag: u32,
    middle: Nested<Middle>,
}
#[derive(psio::PsszPack)]
struct MiddleRef<'a> {
    tag: u32,
    inner: NestedRef<'a, Inner>,
}
#[derive(psio::PsszPack)]
struct OuterRef<'a> {
    tag: u32,
    middle: NestedRef<'a, MiddleRef<'a>>,
}

// A call inside a call: an action whose arguments are bytes.
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct Action {
    sender: u64,
    service: u64,
    method: u64,
    raw_data: Vec<u8>,
}
#[derive(psio::PsszPack)]
struct ActionRef<'a, A: psio::pssz::PsszPack<psio::pssz::Pssz>> {
    sender: u64,
    service: u64,
    method: u64,
    raw_data: PackedRef<'a, A>,
}
#[derive(Debug, PartialEq, psio::PsszPack, psio::PsszUnpack)]
struct PostArgs {
    mode: u8,
    action: Action,
    timeout: u32,
}
#[derive(psio::PsszPack)]
struct PostArgsRef<'a, A: psio::pssz::PsszPack<psio::pssz::Pssz>> {
    mode: u8,
    action: ActionRef<'a, A>,
    timeout: u32,
}

#[test]
fn nested_in_place_has_the_bytes_of_the_two_step_encoding() {
    let _guard = MEASURE.lock().unwrap_or_else(|e| e.into_inner());
    let inner = Inner { data: vec![1, 2, 3] };
    let two_step = to_pssz(&Outer {
        tag: 1,
        middle: Nested::new(&Middle { tag: 2, inner: Nested::new(&inner).unwrap() }).unwrap(),
    });
    let middle = MiddleRef { tag: 2, inner: NestedRef::new(&inner) };
    let direct = to_pssz(&OuterRef { tag: 1, middle: NestedRef::new(&middle) });
    assert_eq!(direct, two_step);
    assert!(validate_pssz::<Outer>(&direct).is_ok());
    let back = from_pssz::<Outer>(&direct).unwrap();
    assert_eq!(back.middle.decode().unwrap().inner.decode().unwrap(), inner);
    assert_eq!(psio::pssz::pssz_size(&OuterRef { tag: 1, middle: NestedRef::new(&middle) }), two_step.len());
}

#[test]
fn three_layers_of_one_mebibyte_are_one_allocation() {
    let _guard = MEASURE.lock().unwrap_or_else(|e| e.into_inner());
    const SIZE: usize = 1 << 20;
    let inner = Inner { data: vec![b'x'; SIZE] };
    let (two_step, two_step_peak, two_step_count) = measure(|| {
        to_pssz(&Outer {
            tag: 1,
            middle: Nested::new(&Middle { tag: 2, inner: Nested::new(&inner).unwrap() }).unwrap(),
        })
    });
    let (direct, direct_peak, direct_count) = measure(|| {
        let middle = MiddleRef { tag: 2, inner: NestedRef::new(&inner) };
        to_pssz(&OuterRef { tag: 1, middle: NestedRef::new(&middle) })
    });
    println!(
        "two-step: peak {two_step_peak} bytes in {two_step_count} allocations; \
         in place: peak {direct_peak} bytes in {direct_count}"
    );
    assert!(direct == two_step);
    assert_eq!(direct_count, 1);
    assert_eq!(direct_peak, direct.len());
    assert!(two_step_count >= 3);
    assert!(two_step_peak >= 3 * SIZE);
}

#[test]
fn a_call_inside_a_call_is_one_allocation() {
    let _guard = MEASURE.lock().unwrap_or_else(|e| e.into_inner());
    const SIZE: usize = 1 << 20;
    let payload = Inner { data: vec![b't'; SIZE] };
    let (layered, layered_peak, layered_count) = measure(|| {
        let exec = Action { sender: 1, service: 2, method: 3, raw_data: to_pssz(&payload) };
        let args = to_pssz(&PostArgs { mode: 2, action: exec, timeout: 1000 });
        to_pssz(&Action { sender: 4, service: 5, method: 6, raw_data: args })
    });
    let (direct, direct_peak, direct_count) = measure(|| {
        let args = PostArgsRef {
            mode: 2,
            action: ActionRef { sender: 1, service: 2, method: 3, raw_data: PackedRef::new(&payload) },
            timeout: 1000,
        };
        to_pssz(&ActionRef { sender: 4, service: 5, method: 6, raw_data: PackedRef::new(&args) })
    });
    println!(
        "layered: peak {layered_peak} bytes in {layered_count} allocations; \
         in place: peak {direct_peak} bytes in {direct_count}"
    );
    assert!(direct == layered);
    assert_eq!(direct_count, 1);
    assert_eq!(direct_peak, direct.len());
    // (The innermost buffer is dropped before the outermost is made.)
    assert!(layered_peak >= 2 * SIZE);
    let outer = from_pssz::<Action>(&direct).unwrap();
    let post = from_pssz::<PostArgs>(&outer.raw_data).unwrap();
    assert_eq!(post.timeout, 1000);
    assert!(from_pssz::<Inner>(&post.action.raw_data).unwrap() == payload);
}

#[derive(psio::PsszPack, psio::PsszUnpack)]
struct Small {
    #[psio(max = 4)]
    text: String,
}
#[derive(psio::PsszPack)]
struct Envelope<'a> {
    id: u32,
    body: NestedRef<'a, Small>,
}
#[derive(psio::PsszPack)]
struct Bounded<'a> {
    id: u32,
    body: NestedRef<'a, Inner, 8>,
}
#[derive(psio::PsszPack, psio::PsszUnpack)]
struct BoundedOwned {
    id: u32,
    body: Nested<Inner, psio::pssz::Checked, 8>,
}

#[test]
fn the_inner_value_is_checked_where_it_is_encoded() {
    let _guard = MEASURE.lock().unwrap_or_else(|e| e.into_inner());
    let ok = Small { text: "abcd".into() };
    let long = Small { text: "abcde".into() };
    assert!(try_to_pssz(&Envelope { id: 1, body: NestedRef::new(&ok) }).is_ok());
    assert!(try_to_pssz(&Envelope { id: 1, body: NestedRef::new(&long) }).is_err());
    assert!(try_to_pssz(&PackedRef::new(&long)).is_err());
    // Nothing is left in the output of a failed append.
    let mut out = vec![b'x'];
    assert!(psio::pssz::try_to_pssz_into(&Envelope { id: 1, body: NestedRef::new(&long) }, &mut out).is_err());
    assert_eq!(out, vec![b'x']);
    // The member's own bound counts the bytes written in place.
    let small = Inner { data: vec![] };
    let big = Inner { data: vec![0; 9] };
    assert_eq!(
        try_to_pssz(&Bounded { id: 1, body: NestedRef::new(&small) }).unwrap(),
        try_to_pssz(&BoundedOwned { id: 1, body: Nested::new(&small).unwrap() }).unwrap()
    );
    assert!(try_to_pssz(&Bounded { id: 1, body: NestedRef::new(&big) }).is_err());
}

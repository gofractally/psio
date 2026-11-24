//! psio::flatbuf on bytes that are not a valid buffer: the cases of
//! `cpp/tests/flatbuf_fuzz_tests.cpp`.
//!
//! What is pinned, for every truncation, every single-byte corruption and a
//! few hundred thousand random corruptions of encoded values, and for
//! buffers of random bytes:
//!
//! * the verifier returns (it does not panic, loop or run away);
//! * a view can be walked in full whatever the bytes are — every accessor,
//!   every element, every key lookup — and every string and byte vector it
//!   hands out lies inside the buffer;
//! * when the bytes validate, the value read through the accessors is the
//!   value the decoder gives, and it re-encodes to a buffer that validates
//!   and decodes to itself.

mod flatbuf_shapes;

use flatbuf_shapes::{natural, raw, Walk};
use psio::flatbuf::{FbLimits, FbOptions, FbPack, FbUnpack, FbView};

#[derive(Default)]
struct Tally {
    tried: usize,
    valid: usize,
}

/// One buffer as a `T`. `rebuild` reads a `T` through the view's accessors.
fn probe<T: FbPack + FbUnpack>(bytes: &[u8], t: &mut Tally, rebuild: impl Fn(FbView<'_, T>, &mut Walk) -> T) {
    // Exactly `bytes`, in an allocation of exactly that size.
    let exact: Box<[u8]> = bytes.into();
    let b = &exact[..];
    t.tried += 1;
    let status = T::fb_validate(b);
    let mut walk = Walk::new(b);
    let seen = rebuild(FbView::trusted(b), &mut walk);
    assert!(walk.inside, "a view handed out bytes outside its buffer");
    if status.is_err() {
        assert!(FbView::<T>::new(b).is_err() && psio::from_flatbuf::<T>(b).is_err());
        return;
    }
    t.valid += 1;
    let again = psio::to_flatbuf(&psio::from_flatbuf_trusted::<T>(b));
    T::fb_validate(&again).expect("the value of a valid buffer re-encodes to a valid buffer");
    // Compared as encodings: a corrupted float may be a NaN.
    assert_eq!(psio::to_flatbuf(&psio::from_flatbuf::<T>(&again).unwrap()), again);
    assert_eq!(psio::to_flatbuf(&FbView::<T>::new(b).unwrap().unpack()), again);
    if walk.budget > 0 {
        assert_eq!(psio::to_flatbuf(&seen), again, "the accessors read what the decoder reads");
    }
}

fn systematic<T: FbPack + FbUnpack>(value: &T, t: &mut Tally, rebuild: impl Fn(FbView<'_, T>, &mut Walk) -> T + Copy) {
    let bytes = psio::to_flatbuf(value);
    // Truncations.
    for n in 0..=bytes.len() {
        probe(&bytes[..n], t, rebuild);
    }
    // Every byte, several replacements.
    let mut b = bytes.clone();
    for i in 0..b.len() {
        let was = b[i];
        for v in [
            0,
            0xff,
            was.wrapping_add(1),
            was.wrapping_sub(1),
            was ^ 0x80,
            was.wrapping_add(4),
            was.wrapping_sub(4),
            0x7f,
        ] {
            if v == was {
                continue;
            }
            b[i] = v;
            probe(&b, t, rebuild);
        }
        b[i] = was;
    }
}

/// splitmix64.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
    fn below(&mut self, n: usize) -> usize {
        (self.next() % n as u64) as usize
    }
}

fn random_edits<T: FbPack + FbUnpack>(
    value: &T,
    seed: u64,
    iterations: usize,
    t: &mut Tally,
    rebuild: impl Fn(FbView<'_, T>, &mut Walk) -> T + Copy,
) {
    let bytes = psio::to_flatbuf(value);
    let mut rng = Rng(seed);
    for _ in 0..iterations {
        let mut b = bytes.clone();
        for _ in 0..1 + rng.below(4) {
            if b.is_empty() {
                break;
            }
            let at = rng.below(b.len());
            match rng.below(6) {
                0 => b[at] = rng.next() as u8,
                1 => b[at] = b[at].wrapping_add(4),
                2 => b[at] ^= 1 << rng.below(8),
                3 => {
                    // A 32-bit word: an offset or a length.
                    let w = at & !3;
                    let v = if rng.below(2) == 1 { rng.below(b.len() + 8) as u32 } else { rng.next() as u32 };
                    if w + 4 <= b.len() {
                        b[w..w + 4].copy_from_slice(&v.to_le_bytes());
                    }
                }
                4 => {
                    // A 16-bit word: a vtable entry.
                    let w = at & !1;
                    let v = rng.below(96) as u16;
                    if w + 2 <= b.len() {
                        b[w..w + 2].copy_from_slice(&v.to_le_bytes());
                    }
                }
                _ => b.truncate(at),
            }
        }
        probe(&b, t, rebuild);
    }
}

#[test]
fn truncations_and_single_byte_corruptions() {
    let mut t = Tally::default();
    systematic(&natural::sample_order(), &mut t, natural::order_from_view);
    systematic(&natural::empty::<natural::Order>(), &mut t, natural::order_from_view);
    systematic(&natural::sample_tree(), &mut t, natural::tree_from_view);
    systematic(&natural::sample_scalars(), &mut t, |v, _| natural::scalars_from_view(v));
    systematic(&natural::sample_profile(), &mut t, natural::profile_from_view);
    // The declarations that hold what the C++ types hold.
    systematic(&raw::sample_order(), &mut t, raw::order_from_view);
    systematic(&raw::sample_scalars(), &mut t, |v, _| raw::scalars_from_view(v));
    println!("systematic: {} buffers, {} valid", t.tried, t.valid);
    assert!(t.valid > 0 && t.valid < t.tried);
}

#[test]
fn random_corruptions() {
    let mut t = Tally::default();
    random_edits(&natural::sample_order(), 1, 150_000, &mut t, natural::order_from_view);
    random_edits(&natural::sample_tree(), 2, 60_000, &mut t, natural::tree_from_view);
    random_edits(&natural::sample_profile(), 3, 30_000, &mut t, natural::profile_from_view);
    random_edits(&raw::sample_order(), 4, 60_000, &mut t, raw::order_from_view);
    println!("random edits: {} buffers, {} valid", t.tried, t.valid);
    assert!(t.valid > 0 && t.valid < t.tried);
}

#[test]
fn random_bytes() {
    let mut t = Tally::default();
    let mut rng = Rng(4);
    for iter in 0..60_000 {
        let mut b = vec![0u8; rng.below(96)];
        for c in b.iter_mut() {
            *c = if rng.below(3) != 0 { rng.below(24) as u8 } else { rng.next() as u8 };
        }
        // Half the time, a plausible root offset.
        if b.len() >= 8 && iter % 2 == 1 {
            let root = (rng.below(b.len() / 4) * 4) as u32;
            b[..4].copy_from_slice(&root.to_le_bytes());
        }
        probe(&b, &mut t, natural::order_from_view);
        probe(&b, &mut t, natural::tree_from_view);
        probe(&b, &mut t, raw::order_from_view);
    }
    println!("random bytes: {} buffers, {} valid", t.tried, t.valid);
}

fn put16(b: &mut [u8], at: usize, v: u16) {
    b[at..at + 2].copy_from_slice(&v.to_le_bytes());
}
fn put32(b: &mut [u8], at: usize, v: u32) {
    b[at..at + 4].copy_from_slice(&v.to_le_bytes());
}

/// A Tree whose `children` vector holds `fan` offsets to ONE child, whose
/// own `children` is that vector's twin, and so on: `fan^levels` tables if
/// every path were walked. Offsets only point forward, so the sharing is
/// built by hand from the leaves back.
fn shared_tree(fan: u32, levels: u32) -> (Vec<u8>, usize, usize) {
    let (vtable, first) = (4usize, 16usize);
    let stride = 8 + 4 + fan as usize * 4;
    let mut buf = vec![0u8; first + stride * levels as usize + 12];
    put32(&mut buf, 0, first as u32);
    put16(&mut buf, vtable, 10);
    put16(&mut buf, vtable + 2, 8);
    put16(&mut buf, vtable + 8, 4); // children at +4
    for l in 0..=levels as usize {
        let tbl = first + stride * l;
        put32(&mut buf, tbl, (tbl - vtable) as u32);
        if l == levels as usize {
            // The leaf: a table whose children field points at an empty vector.
            put32(&mut buf, tbl + 4, 4);
            break;
        }
        let vec = tbl + 8;
        put32(&mut buf, tbl + 4, (vec - (tbl + 4)) as u32);
        put32(&mut buf, vec, fan);
        let next = first + stride * (l + 1);
        for i in 0..fan as usize {
            let cell = vec + 4 + i * 4;
            put32(&mut buf, cell, (next - cell) as u32);
        }
    }
    (buf, first, stride)
}

#[test]
fn a_buffer_that_shares_sub_objects_cannot_make_the_verifier_run_away() {
    let (buf, first, stride) = shared_tree(40, 12);
    let e = psio::validate_flatbuf::<natural::Tree>(&buf).unwrap_err();
    assert_eq!(e.message(), "flatbuf: too many tables");
    // A smaller budget stops sooner; the view of the unverified bytes is lazy.
    let limits = FbLimits { max_depth: 64, max_tables: 100 };
    let e = psio::validate_flatbuf_with::<natural::Tree>(&buf, &FbOptions::default(), limits).unwrap_err();
    assert_eq!(e.message(), "flatbuf: too many tables");
    use natural::TreeFbAccessors;
    assert_eq!(FbView::<natural::Tree>::trusted(&buf).children().get(39).unwrap().children().len(), 40);

    // With the sharing cut down to something a budget allows, it is valid —
    // so the construction above is a well-formed buffer, not noise.
    let mut small = buf.clone();
    for l in 0..12 {
        put32(&mut small, first + stride * l + 8, 1);
    }
    let view = FbView::<natural::Tree>::new(&small).unwrap();
    assert_eq!(view.children().get(0).unwrap().children().len(), 1);
}

#[test]
fn nesting_is_limited() {
    let nested = |depth: usize| {
        let mut t = natural::empty::<natural::Tree>();
        for _ in 0..depth {
            t = natural::Tree { value: 0, name: String::new(), children: vec![t] };
        }
        psio::to_flatbuf(&t)
    };
    assert!(psio::validate_flatbuf::<natural::Tree>(&nested(63)).is_ok());
    let deep = nested(64);
    assert_eq!(
        psio::validate_flatbuf::<natural::Tree>(&deep).unwrap_err().message(),
        "flatbuf: tables nested too deeply"
    );
    let limits = FbLimits { max_depth: 65, ..FbLimits::default() };
    assert!(psio::validate_flatbuf_with::<natural::Tree>(&deep, &FbOptions::default(), limits).is_ok());
}

/// `table Names { names: [string]; }`, with `count` offsets to strings that
/// share their bytes: all the same string, or strings that overlap.
#[derive(Debug, PartialEq, psio::FbPack, psio::FbUnpack)]
struct Names {
    names: Vec<String>,
}

#[test]
fn checking_strings_for_utf8_is_linear_however_they_share_bytes() {
    // One 400 KiB string pointed at 100,000 times: 40 GB to scan if every
    // reference were scanned.
    let (count, len) = (100_000usize, 400 * 1024usize);
    let mut buf = vec![0u8; 16];
    put32(&mut buf, 0, 12);
    put16(&mut buf, 4, 6);
    put16(&mut buf, 6, 8);
    put16(&mut buf, 8, 4);
    put32(&mut buf, 12, 8); // the table: soffset to the vtable at 4
    buf.extend_from_slice(&4u32.to_le_bytes()); // names → the vector at 20
    let vector = buf.len();
    buf.extend_from_slice(&(count as u32).to_le_bytes());
    let string = vector + 4 + count * 4;
    for i in 0..count {
        let cell = vector + 4 + i * 4;
        buf.extend_from_slice(&((string - cell) as u32).to_le_bytes());
    }
    buf.extend_from_slice(&(len as u32).to_le_bytes());
    buf.extend(std::iter::repeat(b'a').take(len));
    buf.extend_from_slice(&[0; 4]);
    let start = std::time::Instant::now();
    psio::validate_flatbuf::<Names>(&buf).unwrap();
    // The same with a byte that is not UTF-8 at the end of the string.
    let last = string + 4 + len - 1;
    buf[last] = 0xff;
    assert_eq!(psio::validate_flatbuf::<Names>(&buf).unwrap_err().message(), "flatbuf: string is not valid UTF-8");
    buf[last] = b'a';

    // Overlapping strings: every 8 bytes of the big string start another
    // one that runs to the same end (its length is 4 bytes of the big
    // string's text).
    let overlapping = count.min(len / 8 - 1);
    for i in 0..overlapping {
        let at = string + 8 * (i + 1);
        let rest = (string + 4 + len) - (at + 4);
        put32(&mut buf, at, rest as u32);
        let cell = vector + 4 + i * 4;
        put32(&mut buf, cell, (at - cell) as u32);
    }
    match psio::validate_flatbuf::<Names>(&buf) {
        Ok(()) => {}
        Err(e) => assert_eq!(e.message(), "flatbuf: string is not valid UTF-8"),
    }
    assert!(start.elapsed() < std::time::Duration::from_secs(20), "took {:?}", start.elapsed());
}

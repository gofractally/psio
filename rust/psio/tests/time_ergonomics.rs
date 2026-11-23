//! `psio::time` as a value to compute with (RG3): operators, unit
//! conversions, `Display` / `FromStr`, ordering and hashing, and the
//! optional `chrono` and `async-graphql` integrations.

use psio::time::{Duration, Micros, Millis, MonotonicPoint, Nanos, Seconds, TimeError, TimePoint, Unit};
use std::collections::{BTreeSet, HashSet};

type Sec = TimePoint<Seconds>;
type USec = TimePoint<Micros>;

#[test]
fn points_and_durations_add_and_subtract() {
    let t = Sec::from_ticks(1_795_342_530);
    let hour = Duration::<Seconds>::from_ticks(3600);
    assert_eq!((t + hour).ticks(), 1_795_346_130);
    assert_eq!((t - hour).ticks(), 1_795_338_930);
    assert_eq!((t + hour) - t, hour);
    assert_eq!(t - (t + hour), -hour);
    let mut moving = t;
    moving += hour;
    moving += hour;
    moving -= hour;
    assert_eq!(moving, t + hour);

    assert_eq!(hour + hour, Duration::from_ticks(7200));
    assert_eq!(hour - hour, Duration::zero());
    assert!((hour - hour).is_zero());
    assert_eq!(hour * 24, Duration::from_ticks(86_400));
    assert_eq!(hour / 60, Duration::from_ticks(60));
    assert_eq!(-hour, Duration::from_ticks(-3600));
    let mut total = hour;
    total += hour;
    total -= Duration::from_ticks(1);
    assert_eq!(total.ticks(), 7199);
    assert_eq!([hour, hour, hour].into_iter().sum::<Duration<Seconds>>(), hour * 3);

    // A monotonic point computes the same way.
    let m = MonotonicPoint::<Millis>::from_ticks(10);
    let step = Duration::<Millis>::from_ticks(5);
    assert_eq!((m + step).ticks(), 15);
    assert_eq!((m + step) - m, step);

    // since_epoch / from_since_epoch.
    assert_eq!(t.since_epoch().ticks(), t.ticks());
    assert_eq!(Sec::from_since_epoch(hour), Sec::from_ticks(3600));
    assert_eq!(Sec::epoch() + t.since_epoch(), t);
}

#[test]
fn overflow_is_refused_never_wrapped() {
    let end = TimePoint::<Seconds, u32>::MAX;
    let one = Duration::<Seconds, u32>::from_ticks(1);
    assert_eq!(end.checked_add(one), None);
    assert_eq!(Duration::<Seconds, u32>::zero().checked_sub(one), None);
    assert_eq!(one.checked_neg(), None);
    assert_eq!(Duration::<Seconds, i8>::from_ticks(100).checked_mul(2), None);
    assert_eq!(one.checked_div(0), None);
    assert_eq!(Duration::<Seconds, i64>::MIN.checked_neg(), None);
    assert_eq!(Duration::<Seconds, i64>::MIN.checked_div(-1), None);

    let panics = |f: fn()| std::panic::catch_unwind(f).is_err();
    assert!(panics(|| {
        let _ = TimePoint::<Seconds, u32>::MAX + Duration::<Seconds, u32>::from_ticks(1);
    }));
    assert!(panics(|| {
        let _ = TimePoint::<Seconds, u32>::from_ticks(1) - TimePoint::<Seconds, u32>::from_ticks(2);
    }));
    assert!(panics(|| {
        let _ = Duration::<Seconds, i64>::MAX + Duration::<Seconds, i64>::from_ticks(1);
    }));
    assert!(panics(|| {
        let _ = Duration::<Seconds, i64>::MAX * 2;
    }));
    assert!(panics(|| {
        let _ = Duration::<Seconds, i64>::from_ticks(1) / 0;
    }));
    assert!(panics(|| {
        let _ = Sec::MAX.floor::<Micros>();
    }));
}

#[test]
fn units_convert_exactly_or_by_floor_and_ceil() {
    let t = USec::from_ticks(1_795_342_530_250_000);
    // To a coarser unit: only a whole number of ticks is exact.
    assert_eq!(t.to_unit::<Seconds>(), Err(TimeError("time: not a whole number of ticks")));
    assert_eq!(t.to_unit::<Millis>().unwrap().ticks(), 1_795_342_530_250);
    assert_eq!(t.floor::<Seconds>(), Sec::from_ticks(1_795_342_530));
    assert_eq!(t.ceil::<Seconds>(), Sec::from_ticks(1_795_342_531));
    // A whole second floors and ceils to itself.
    let whole = USec::from_ticks(1_795_342_530_000_000);
    assert_eq!(whole.to_unit::<Seconds>().unwrap(), Sec::from_ticks(1_795_342_530));
    assert_eq!(whole.floor::<Seconds>(), whole.ceil::<Seconds>());
    // To a finer unit: always whole; the tick type may not hold it.
    let s = Sec::from_ticks(1_795_342_530);
    assert_eq!(s.to_unit::<Micros>().unwrap(), whole);
    assert_eq!(s.floor::<Micros>(), whole);
    assert_eq!(s.to_unit::<Nanos>().unwrap().ticks(), 1_795_342_530_000_000_000);
    assert_eq!(Sec::MAX.to_unit::<Micros>(), Err(TimeError("time: outside the range of the tick type")));
    assert_eq!(Sec::MAX.checked_floor::<Micros>(), None);
    assert_eq!(Sec::MAX.checked_ceil::<Micros>(), None);

    // Before the epoch: floor is toward negative infinity, ceil toward
    // positive (C++ std::chrono::floor / ceil).
    let before = USec::from_ticks(-1_500_000);
    assert_eq!(before.floor::<Seconds>().ticks(), -2);
    assert_eq!(before.ceil::<Seconds>().ticks(), -1);
    let d = Duration::<Micros>::from_ticks(-1);
    assert_eq!(d.floor::<Seconds>().ticks(), -1);
    assert_eq!(d.ceil::<Seconds>().ticks(), 0);
    assert_eq!(Duration::<Micros>::from_ticks(2_500_000).floor::<Seconds>().ticks(), 2);
    assert_eq!(Duration::<Micros>::from_ticks(2_500_000).ceil::<Seconds>().ticks(), 3);
    assert_eq!(MonotonicPoint::<Nanos>::from_ticks(1_999).floor::<Micros>().ticks(), 1);

    // Any period.
    struct Minutes;
    impl Unit for Minutes {
        const NUM: u64 = 60;
        const DEN: u64 = 1;
    }
    assert_eq!(Duration::<Minutes>::from_ticks(2).to_unit::<Seconds>().unwrap().ticks(), 120);
    assert_eq!(Duration::<Seconds>::from_ticks(119).floor::<Minutes>().ticks(), 1);
    assert_eq!(Duration::<Seconds>::from_ticks(119).ceil::<Minutes>().ticks(), 2);

    // The tick type.
    assert_eq!(s.to_tick_type::<u32>().unwrap().ticks(), 1_795_342_530u32);
    assert!(Sec::from_ticks(-1).to_tick_type::<u32>().is_err());
    assert!(Sec::from_ticks(1 << 40).to_tick_type::<i32>().is_err());
}

#[test]
fn display_and_from_str_are_the_json_text() {
    let s = Sec::from_ticks(1_795_342_530);
    assert_eq!(s.to_string(), "2026-11-22T10:15:30Z");
    assert_eq!(format!("\"{s}\""), psio::to_json(&s));
    assert_eq!("2026-11-22T10:15:30Z".parse::<Sec>().unwrap(), s);
    // Any RFC 3339 date-time reads; an offset is applied.
    assert_eq!("2026-11-22T12:15:30+02:00".parse::<Sec>().unwrap(), s);
    assert_eq!("2026-11-22t10:15:30.000z".parse::<Sec>().unwrap(), s);
    assert!("2026-11-22T10:15:30.5Z".parse::<Sec>().is_err());
    assert!("2026-11-22 10:15:30Z".parse::<Sec>().is_err());
    assert!("1795342530".parse::<Sec>().is_err());
    assert!("".parse::<Sec>().is_err());

    let u = USec::from_ticks(1_795_342_530_123_456);
    assert_eq!(u.to_string(), "2026-11-22T10:15:30.123456Z");
    assert_eq!(u.to_string().parse::<USec>().unwrap(), u);
    assert_eq!("2026-11-22T10:15:30.5Z".parse::<USec>().unwrap().ticks(), 1_795_342_530_500_000);
    assert_eq!(TimePoint::<Millis>::from_ticks(-1).to_string(), "1969-12-31T23:59:59.999Z");
    assert_eq!(TimePoint::<Nanos>::from_ticks(1).to_string(), "1970-01-01T00:00:00.000000001Z");

    // An instant with no RFC 3339 text is written as its ticks in
    // parentheses, which is not read back.
    assert_eq!(Sec::MAX.to_string(), format!("({})", i64::MAX));
    assert!(Sec::MAX.to_string().parse::<Sec>().is_err());
    assert_eq!(Sec::from_ticks(253_402_300_799).to_string(), "9999-12-31T23:59:59Z");
    assert_eq!(Sec::from_ticks(253_402_300_800).to_string(), "(253402300800)");

    // A duration and a monotonic point are the tick count.
    let d = Duration::<Micros>::from_ticks(-1_500_000);
    assert_eq!(d.to_string(), "-1500000");
    assert_eq!(d.to_string(), psio::to_json(&d));
    assert_eq!("-1500000".parse::<Duration<Micros>>().unwrap(), d);
    assert_eq!("0".parse::<Duration<Micros>>().unwrap(), Duration::zero());
    assert_eq!(MonotonicPoint::<Seconds, u8>::from_ticks(255).to_string(), "255");
    assert_eq!("255".parse::<MonotonicPoint<Seconds, u8>>().unwrap().ticks(), 255);
    for bad in ["", "-", "+1", "01", "-0", "1.0", "1e3", " 1", "256", "abc"] {
        assert!(bad.parse::<MonotonicPoint<Seconds, u8>>().is_err(), "{bad:?}");
    }
    assert!("-1".parse::<Duration<Seconds, u32>>().is_err());
}

#[test]
fn values_order_hash_and_default() {
    let times: BTreeSet<Sec> = [3, 1, 2, -5].into_iter().map(Sec::from_ticks).collect();
    let ordered: Vec<i64> = times.iter().map(|t| t.ticks()).collect();
    assert_eq!(ordered, [-5, 1, 2, 3]);
    assert!(Sec::from_ticks(1) < Sec::from_ticks(2));
    assert_eq!(Sec::from_ticks(1).max(Sec::from_ticks(2)).ticks(), 2);
    let hashed: HashSet<Duration<Micros>> = [1, 1, 2].into_iter().map(Duration::from_ticks).collect();
    assert_eq!(hashed.len(), 2);
    assert_eq!(Sec::default(), Sec::epoch());
    assert_eq!(Duration::<Seconds>::default(), Duration::zero());
    // Ordering is the key's ordering.
    assert!(psio::to_key(&Sec::from_ticks(-5)) < psio::to_key(&Sec::from_ticks(1)));
    // now() is a time point of the unit asked for.
    let now = Sec::now().unwrap();
    assert!(now > Sec::from_ticks(1_700_000_000));
    assert!(USec::now().unwrap().floor::<Seconds>() >= now);
    assert!(TimePoint::<Seconds, u8>::now().is_err());
}

#[cfg(feature = "chrono")]
#[test]
fn chrono_values_convert_exactly() {
    use chrono::{DateTime, TimeDelta, Utc};
    let s = Sec::from_ticks(1_795_342_530);
    let date: DateTime<Utc> = s.try_into().unwrap();
    assert_eq!(date.to_rfc3339(), "2026-11-22T10:15:30+00:00");
    assert_eq!(Sec::try_from(date).unwrap(), s);
    // An offset is the same instant.
    let offset = DateTime::parse_from_rfc3339("2026-11-22T12:15:30+02:00").unwrap();
    assert_eq!(Sec::try_from(offset).unwrap(), s);

    let u = USec::from_ticks(1_795_342_530_123_456);
    let date: DateTime<Utc> = u.try_into().unwrap();
    assert_eq!(date.timestamp_subsec_micros(), 123_456);
    assert_eq!(USec::try_from(date).unwrap(), u);
    // Exact: a fraction the unit cannot hold is refused; floor rounds down.
    assert_eq!(Sec::try_from(date), Err(TimeError("time: not a whole number of ticks")));
    assert_eq!(Sec::from_chrono_floor(date), Some(s));
    let before: DateTime<Utc> = USec::from_ticks(-1_500_000).try_into().unwrap();
    assert_eq!(Sec::from_chrono_floor(before).unwrap().ticks(), -2);
    assert_eq!(TimePoint::<Seconds, u8>::from_chrono_floor(date), None);
    // Out of chrono's range.
    assert!(DateTime::<Utc>::try_from(Sec::MAX).is_err());

    let d = Duration::<Micros>::from_ticks(-1_500_000);
    let delta: TimeDelta = d.try_into().unwrap();
    assert_eq!(delta.num_microseconds(), Some(-1_500_000));
    assert_eq!(Duration::<Micros>::try_from(delta).unwrap(), d);
    assert!(Duration::<Seconds>::try_from(delta).is_err());
    assert_eq!(Duration::<Seconds>::try_from(TimeDelta::seconds(90)).unwrap().ticks(), 90);
}

#[cfg(feature = "async-graphql")]
mod graphql {
    use super::*;
    use async_graphql::{EmptyMutation, EmptySubscription, InputObject, Object, ScalarType, Schema, SimpleObject, Value};
    use psio::pssz::{BoundedString, RawString, Uint256};
    use psio::JsonText;

    #[derive(Clone, Debug, PartialEq, JsonText)]
    #[psio(transparent)]
    pub struct Memo(pub String);
    psio::async_graphql_scalar!(Memo, "Memo");

    #[derive(SimpleObject, Clone)]
    pub struct Row {
        pub at: Sec,
        pub at_micros: USec,
        pub timeout: Duration<Micros>,
        pub uptime: MonotonicPoint<Millis, u32>,
        pub raw: RawString,
        pub tag: BoundedString<8>,
        pub big: Uint256,
        pub memo: Memo,
    }

    #[derive(InputObject)]
    pub struct Window {
        pub from: Sec,
        pub length: Duration<Seconds>,
        pub memo: Memo,
    }

    pub struct Query;
    #[Object]
    impl Query {
        async fn row(&self) -> Row {
            Row {
                at: Sec::from_ticks(1_795_342_530),
                at_micros: USec::from_ticks(1_795_342_530_123_456),
                timeout: Duration::from_ticks(1 << 60),
                uptime: MonotonicPoint::from_ticks(7),
                raw: RawString(b"raw".to_vec()),
                tag: psio::from_json::<BoundedString<8>>("\"tag\"").unwrap(),
                big: psio::from_json::<Uint256>("\"5\"").unwrap(),
                memo: Memo("hi".into()),
            }
        }
        async fn end(&self, window: Window) -> Sec {
            let _ = &window.memo;
            window.from + window.length
        }
    }

    /// Runs a future that never waits.
    fn block_on<F: std::future::Future>(future: F) -> F::Output {
        use std::task::{Context, Poll, RawWaker, RawWakerVTable, Waker};
        fn raw() -> RawWaker {
            fn no(_: *const ()) {}
            fn clone(_: *const ()) -> RawWaker {
                raw()
            }
            static VTABLE: RawWakerVTable = RawWakerVTable::new(clone, no, no, no);
            RawWaker::new(std::ptr::null(), &VTABLE)
        }
        let waker = unsafe { Waker::from_raw(raw()) };
        let mut future = std::pin::pin!(future);
        loop {
            if let Poll::Ready(out) = future.as_mut().poll(&mut Context::from_waker(&waker)) {
                return out;
            }
        }
    }

    #[test]
    fn psio_values_are_scalars_through_their_text() {
        let schema = Schema::new(Query, EmptyMutation, EmptySubscription);
        let sdl = schema.sdl();
        for name in [
            "scalar DateTimeSeconds",
            "scalar DateTimeMicroseconds",
            "scalar Microseconds",
            "scalar Seconds",
            "scalar MonotonicMilliseconds_u32",
            "scalar RawString",
            "scalar BoundedString_8",
            "scalar UInt256",
            "scalar Memo",
        ] {
            assert!(sdl.contains(name), "{name} in:\n{sdl}");
        }

        let reply = block_on(schema.execute("{ row { at atMicros timeout uptime raw tag big memo } }"));
        assert!(reply.errors.is_empty(), "{:?}", reply.errors);
        assert_eq!(
            reply.data.into_json().unwrap().to_string(),
            concat!(
                r#"{"row":{"at":"2026-11-22T10:15:30Z","atMicros":"2026-11-22T10:15:30.123456Z","#,
                r#""big":"5","memo":"hi","raw":"raw","tag":"tag","#,
                r#""timeout":"1152921504606846976","uptime":7}}"#
            )
        );

        // Arguments are read by the IJson reader: any RFC 3339 date-time,
        // a tick count as a number or a string.
        for length in ["60", "\"60\""] {
            let query =
                format!(r#"{{ end(window: {{ from: "2026-11-22T12:15:30+02:00", length: {length}, memo: "m" }}) }}"#);
            let reply = block_on(schema.execute(query));
            assert!(reply.errors.is_empty(), "{:?}", reply.errors);
            assert_eq!(reply.data.into_json().unwrap().to_string(), r#"{"end":"2026-11-22T10:16:30Z"}"#);
        }
        let reply = block_on(schema.execute(r#"{ end(window: { from: "yesterday", length: 60, memo: "m" }) }"#));
        assert_eq!(reply.errors.len(), 1);
        assert!(reply.errors[0].message.contains("time:"), "{}", reply.errors[0].message);
        let reply = block_on(schema.execute(r#"{ end(window: { from: 5, length: 60, memo: "m" }) }"#));
        assert_eq!(reply.errors.len(), 1);
    }

    #[test]
    fn scalar_values_are_the_ijson_form() {
        let s = Sec::from_ticks(1_795_342_530);
        assert_eq!(ScalarType::to_value(&s), Value::from("2026-11-22T10:15:30Z"));
        assert_eq!(<Sec as ScalarType>::parse(Value::from("2026-11-22T10:15:30Z")).unwrap(), s);
        // No text: null.
        assert_eq!(ScalarType::to_value(&Sec::MAX), Value::Null);
        let d = Duration::<Seconds>::from_ticks(60);
        assert_eq!(ScalarType::to_value(&d), Value::from(60));
        assert_eq!(<Duration<Seconds> as ScalarType>::parse(Value::from(60)).unwrap(), d);
        assert!(<Duration<Seconds, u8> as ScalarType>::parse(Value::from(256)).is_err());
        assert!(<Duration<Seconds> as ScalarType>::parse(Value::from(1.5)).is_err());
        assert!(<Duration<Seconds> as ScalarType>::parse(Value::from(true)).is_err());
        assert_eq!(<Memo as ScalarType>::parse(Value::from("x")).unwrap(), Memo("x".into()));
    }
}

// ── Across units (C++ std::chrono: the common type is the finer unit) ───────

#[test]
fn a_coarser_unit_converts_to_a_finer_one() {
    use psio::time::{Millis, Nanos};
    let s = Duration::<Seconds>::from_ticks(-3);
    let ms: Duration<Millis> = s.into();
    let us: Duration<Micros> = s.into();
    let ns: Duration<Nanos> = s.into();
    assert_eq!((ms.ticks(), us.ticks(), ns.ticks()), (-3_000, -3_000_000, -3_000_000_000));
    assert_eq!(Duration::<Micros>::from(ms), us);
    assert_eq!(Duration::<Nanos>::from(us), ns);
    assert_eq!(Duration::<Nanos>::from(ms), ns);
    let t = TimePoint::<Seconds>::from_ticks(1_795_342_530);
    assert_eq!(TimePoint::<Micros>::from(t).ticks(), 1_795_342_530_000_000);
    assert_eq!(TimePoint::<Micros>::from(t), t.to_unit::<Micros>().unwrap());
    let m = MonotonicPoint::<Millis, u32>::from_ticks(7);
    assert_eq!(MonotonicPoint::<Micros, u32>::from(m).ticks(), 7_000);
    // The conversion is of the value: bytes, key and text are each type's own.
    assert_eq!(psio::to_pssz(&TimePoint::<Micros>::from(t)), psio::to_pssz(&t.to_unit::<Micros>().unwrap()));
    assert_eq!(psio::to_json(&us), "-3000000");

    // The boundaries of the tick type.
    let most = i64::MAX / 1_000_000;
    assert_eq!(Duration::<Micros>::from(Duration::<Seconds>::from_ticks(most)).ticks(), most * 1_000_000);
    assert_eq!(Duration::<Micros>::from(Duration::<Seconds>::from_ticks(-most)).ticks(), -most * 1_000_000);
    assert_eq!(Duration::<Seconds>::from_ticks(most + 1).checked_widen::<Micros>(), None);
    assert_eq!(Duration::<Seconds>::from_ticks(i64::MIN / 1_000_000 - 1).checked_widen::<Micros>(), None);
    assert_eq!(
        Duration::<Seconds>::from_ticks(i64::MIN / 1_000_000).checked_widen::<Micros>().map(|d| d.ticks()),
        Some(i64::MIN / 1_000_000 * 1_000_000)
    );
    assert_eq!(Duration::<Seconds, u8>::from_ticks(255).checked_widen::<Millis>(), None);
    assert_eq!(TimePoint::<Seconds, u32>::from_ticks(4_294_967).checked_widen::<Millis>().map(|t| t.ticks()), Some(4_294_967_000));
    assert_eq!(TimePoint::<Seconds, u32>::from_ticks(4_294_968).checked_widen::<Millis>(), None);
    // Narrowing is not a widening: explicit, with to_unit / floor / ceil.
    assert_eq!(Duration::<Micros>::from_ticks(1_500_000).checked_widen::<Seconds>(), None);
    assert_eq!(Duration::<Micros>::from_ticks(2_000_000).checked_widen::<Seconds>().map(|d| d.ticks()), Some(2));
}

#[test]
#[should_panic(expected = "arithmetic overflow of the tick type")]
fn widening_past_the_tick_type_panics() {
    let _: Duration<Micros> = Duration::<Seconds>::from_ticks(i64::MAX / 1_000_000 + 1).into();
}

#[test]
fn arithmetic_on_two_units_is_in_the_finer_one() {
    use psio::time::{Millis, Nanos};
    let t = TimePoint::<Micros>::from_ticks(1_000_000);
    let second = Duration::<Seconds>::from_ticks(1);
    // Under the operators the coarser side converts: `into()` is exact.
    let later: TimePoint<Micros> = t + second.into();
    assert_eq!(later.ticks(), 2_000_000);
    // (`point - x` is also `point - point`, so there the target is named)
    assert_eq!((t - Duration::<Micros>::from(second)).ticks(), 0);
    let mut moving = t;
    moving += second.into();
    moving -= Duration::<Millis>::from_ticks(500).into();
    assert_eq!(moving.ticks(), 1_500_000);
    let mut total = Duration::<Micros>::zero();
    total += second.into();
    total -= Duration::<Millis>::from_ticks(1).into();
    assert_eq!(total.ticks(), 999_000);
    assert!(Duration::<Micros>::from_ticks(1_000_000) == second.into());
    assert!(Duration::<Micros>::from_ticks(1_000_001) > second.into());
    // The same-unit operators still settle the unit of a bare literal.
    assert_eq!(t + Duration::from_ticks(1), TimePoint::from_ticks(1_000_001));
    assert_eq!(second + Duration::from_ticks(1), Duration::from_ticks(2));

    // By name, with either side the finer: the result is in the finer unit.
    let named: TimePoint<Micros> = t.add_mixed(second);
    assert_eq!(named, later);
    assert_eq!(t.sub_mixed(second).ticks(), 0);
    let coarse = TimePoint::<Seconds>::from_ticks(10);
    let finer: TimePoint<Micros> = coarse.add_mixed(Duration::<Micros>::from_ticks(5));
    assert_eq!(finer.ticks(), 10_000_005);
    assert_eq!(coarse.sub_mixed(Duration::<Millis>::from_ticks(1)).ticks(), 9_999);
    let between: Duration<Micros> = finer.duration_since_mixed(coarse);
    assert_eq!(between.ticks(), 5);
    assert_eq!(coarse.duration_since_mixed(finer).ticks(), -5);
    let sum: Duration<Millis> = second.add_mixed(Duration::<Millis>::from_ticks(250));
    assert_eq!(sum.ticks(), 1_250);
    assert_eq!(Duration::<Millis>::from_ticks(250).sub_mixed(second).ticks(), -750);
    assert_eq!(Duration::<Nanos>::from_ticks(1).add_mixed(Duration::<Micros>::from_ticks(1)).ticks(), 1_001);
    let m = MonotonicPoint::<Millis>::from_ticks(5).add_mixed(second);
    assert_eq!(m.ticks(), 1_005);
    assert_eq!(MonotonicPoint::<Seconds>::from_ticks(1).duration_since_mixed(m).ticks(), -5);
    // The same unit on both sides is the same-unit rule.
    assert_eq!(second.add_mixed(second).ticks(), 2);

    // Overflow: None from the checked twins, at the boundary.
    let top = TimePoint::<Micros>::MAX;
    assert_eq!(top.checked_add_mixed(second), None);
    assert_eq!(top.checked_sub_mixed(second).map(|t| t.ticks()), Some(i64::MAX - 1_000_000));
    let edge = TimePoint::<Micros>::from_ticks(i64::MAX - 1_000_000);
    assert_eq!(edge.checked_add_mixed(second), Some(top));
    assert_eq!(TimePoint::<Micros>::MIN.checked_sub_mixed(second), None);
    // (the coarse side does not fit the finer unit at all)
    assert_eq!(TimePoint::<Seconds>::MAX.checked_add_mixed(Duration::<Micros>::zero()), None);
    assert_eq!(top.checked_duration_since_mixed(TimePoint::<Seconds>::from_ticks(-1)), None);
    assert_eq!(
        top.checked_duration_since_mixed(TimePoint::<Seconds>::from_ticks(1)).map(|d| d.ticks()),
        Some(i64::MAX - 1_000_000)
    );
    assert_eq!(Duration::<Micros>::MAX.checked_add_mixed(second), None);
    assert_eq!(Duration::<Seconds>::MAX.checked_sub_mixed(Duration::<Micros>::zero()), None);
    assert_eq!(
        Duration::<Micros>::MIN.checked_add_mixed(second).map(|d| d.ticks()),
        Some(i64::MIN + 1_000_000)
    );
}

#[test]
#[should_panic(expected = "arithmetic overflow of the tick type")]
fn mixed_addition_past_the_tick_type_panics() {
    let _ = TimePoint::<Micros>::MAX.add_mixed(Duration::<Seconds>::from_ticks(1));
}

#[test]
fn values_of_two_units_compare() {
    use psio::time::{Millis, Nanos};
    use std::cmp::Ordering::{Equal, Greater, Less};
    let second = Duration::<Seconds>::from_ticks(1);
    assert_eq!(second.cmp_mixed(&Duration::<Micros>::from_ticks(1_000_000)), Equal);
    assert_eq!(Duration::<Micros>::from_ticks(1_000_000).cmp_mixed(&second), Equal);
    assert_eq!(second.cmp_mixed(&Duration::<Micros>::from_ticks(1_000_001)), Less);
    assert_eq!(Duration::<Millis>::from_ticks(999).cmp_mixed(&second), Less);
    assert_eq!(Duration::<Nanos>::from_ticks(-1).cmp_mixed(&Duration::<Seconds>::zero()), Less);
    assert_eq!(TimePoint::<Seconds>::from_ticks(2).cmp_mixed(&TimePoint::<Micros>::from_ticks(1_999_999)), Greater);
    assert_eq!(TimePoint::<Micros>::from_ticks(2_000_000).cmp_mixed(&TimePoint::<Seconds>::from_ticks(2)), Equal);
    assert_eq!(MonotonicPoint::<Millis>::from_ticks(1).cmp_mixed(&MonotonicPoint::<Micros>::from_ticks(1_000)), Equal);
    // Exact where the coarser value does not fit the finer unit's tick type.
    assert_eq!(Duration::<Seconds>::MAX.cmp_mixed(&Duration::<Nanos>::MAX), Greater);
    assert_eq!(Duration::<Seconds>::MIN.cmp_mixed(&Duration::<Nanos>::MIN), Less);
    assert_eq!(Duration::<Seconds, u64>::MAX.cmp_mixed(&Duration::<Nanos, u64>::MAX), Greater);
    assert_eq!(TimePoint::<Seconds>::MAX.cmp_mixed(&TimePoint::<Micros>::MAX), Greater);
    assert_eq!(Duration::<Seconds, u8>::from_ticks(255).cmp_mixed(&Duration::<Millis, u8>::from_ticks(255)), Greater);
    // Any two units, also ones with no common finer unit declared.
    assert_eq!(second.cmp_mixed(&second), Equal);
}

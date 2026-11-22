//! `async-graphql` scalars for PSIO values (cargo feature `async-graphql`).
//!
//! A PSIO type that has a JSON text is a GraphQL scalar through that text:
//! what a query receives and what an argument accepts is the type's IJson
//! form (docs/json.md), read by PSIO's typed reader. PSIO's own leaf types
//! implement `async_graphql::ScalarType`, `InputType` and `OutputType`
//! here, so they can be members of a `SimpleObject` or `InputObject`
//! without a wrapper:
//!
//! | type | scalar name | value |
//! |------|-------------|-------|
//! | [`TimePoint<U, R>`](crate::time::TimePoint) | `DateTimeSeconds`, `DateTimeMilliseconds`, `DateTimeMicroseconds`, `DateTimeNanoseconds`, `DateTime_<num>_<den>` | RFC 3339 UTC string |
//! | [`Duration<U, R>`](crate::time::Duration) | `Seconds`, `Milliseconds`, `Microseconds`, `Nanoseconds`, `Duration_<num>_<den>` | the tick count |
//! | [`MonotonicPoint<U, R>`](crate::time::MonotonicPoint) | `MonotonicSeconds`, ..., `Monotonic_<num>_<den>` | the tick count |
//! | [`RawString`](crate::pssz::RawString) | `RawString` | string |
//! | [`BoundedString<N>`](crate::pssz::BoundedString), [`BoundedRawString<N>`](crate::pssz::BoundedRawString) | `BoundedString_<N>`, `BoundedRawString_<N>` | string |
//! | [`Uint256`](crate::pssz::Uint256) | `UInt256` | decimal string |
//!
//! A tick integer other than `i64` is part of the name (`Seconds_u32`):
//! `async-graphql` refuses two Rust types under one scalar name. For the
//! same reason a time point's name carries its unit, where C++
//! `psio::graphql` names every time point `DateTime`.
//!
//! A type of your own that is [`JsonText`] becomes a scalar with
//! [`async_graphql_scalar!`](crate::async_graphql_scalar):
//!
//! ```
//! use psio::JsonText;
//!
//! #[derive(Clone, JsonText)]
//! #[psio(transparent)]
//! pub struct Memo(pub String);
//! psio::async_graphql_scalar!(Memo, "Memo");
//!
//! #[derive(async_graphql::SimpleObject)]
//! pub struct Transfer {
//!     pub memo: Memo,
//!     pub at: psio::time::TimePoint<psio::time::Seconds>,
//!     pub timeout: psio::time::Duration<psio::time::Micros>,
//! }
//! ```

use crate::json::JsonText;
use crate::time::{Duration, MonotonicPoint, Tick, TimePoint, Unit};
use async_graphql::{InputValueError, InputValueResult, Scalar, ScalarType, TypeName, Value};
use std::borrow::Cow;

#[doc(hidden)]
pub use async_graphql as __async_graphql;

/// The GraphQL value of a PSIO value: its IJson text as a value. A value
/// with no text (a time point outside years 0000..9999, a violated
/// invariant) is `null`.
pub fn to_value<T: JsonText>(value: &T) -> Value {
    crate::try_to_ijson(value)
        .ok()
        .and_then(|text| serde_json::from_str::<serde_json::Value>(&text).ok())
        .and_then(|json| Value::from_json(json).ok())
        .unwrap_or(Value::Null)
}

/// Reads a PSIO value from a GraphQL value with the type's IJson reader:
/// accepts what [`from_ijson`](crate::from_ijson) accepts for the value's
/// JSON form, and reports its message otherwise.
pub fn parse<T: JsonText + async_graphql::InputType>(value: Value) -> InputValueResult<T> {
    let text = match &value {
        // An integer keeps its digits; serde_json would spell a float.
        Value::Number(number) => number.to_string(),
        _ => value
            .into_json()
            .map(|json| json.to_string())
            .map_err(|error| InputValueError::custom(error.to_string()))?,
    };
    crate::from_ijson::<T>(&text).map_err(|error| InputValueError::custom(error.to_string()))
}

/// Makes a [`JsonText`] type an `async-graphql` scalar named `$name`, whose
/// value is the type's IJson text (see the [module](crate::graphql_scalars)).
#[macro_export]
macro_rules! async_graphql_scalar {
    ($ty:ty, $name:literal) => {
        #[$crate::graphql_scalars::__async_graphql::Scalar(name = $name)]
        impl $crate::graphql_scalars::__async_graphql::ScalarType for $ty {
            fn parse(
                value: $crate::graphql_scalars::__async_graphql::Value,
            ) -> $crate::graphql_scalars::__async_graphql::InputValueResult<Self> {
                $crate::graphql_scalars::parse(value)
            }
            fn to_value(&self) -> $crate::graphql_scalars::__async_graphql::Value {
                $crate::graphql_scalars::to_value(self)
            }
        }
    };
}

fn unit_name<U: Unit>(prefix: &str, ratio_prefix: &str) -> String {
    match (U::NUM, U::DEN) {
        (1, 1) => format!("{prefix}Seconds"),
        (1, 1_000) => format!("{prefix}Milliseconds"),
        (1, 1_000_000) => format!("{prefix}Microseconds"),
        (1, 1_000_000_000) => format!("{prefix}Nanoseconds"),
        (num, den) => format!("{ratio_prefix}_{num}_{den}"),
    }
}

fn tick_suffix<R: Tick>() -> String {
    if R::BITS == 64 && R::SIGNED {
        String::new()
    } else {
        format!("_{}{}", if R::SIGNED { 'i' } else { 'u' }, R::BITS)
    }
}

impl<U: Unit, R: Tick> TypeName for TimePoint<U, R> {
    fn type_name() -> Cow<'static, str> {
        Cow::Owned(unit_name::<U>("DateTime", "DateTime") + &tick_suffix::<R>())
    }
}
impl<U: Unit, R: Tick> TypeName for Duration<U, R> {
    fn type_name() -> Cow<'static, str> {
        Cow::Owned(unit_name::<U>("", "Duration") + &tick_suffix::<R>())
    }
}
impl<U: Unit, R: Tick> TypeName for MonotonicPoint<U, R> {
    fn type_name() -> Cow<'static, str> {
        Cow::Owned(unit_name::<U>("Monotonic", "Monotonic") + &tick_suffix::<R>())
    }
}
impl<const N: usize> TypeName for crate::pssz::BoundedString<N> {
    fn type_name() -> Cow<'static, str> {
        Cow::Owned(format!("BoundedString_{N}"))
    }
}
impl<const N: usize> TypeName for crate::pssz::BoundedRawString<N> {
    fn type_name() -> Cow<'static, str> {
        Cow::Owned(format!("BoundedRawString_{N}"))
    }
}

macro_rules! scalar_as_text {
    ($([$($generics:tt)*] $ty:ty, ($($name:tt)+);)*) => {$(
        #[Scalar($($name)+)]
        impl<$($generics)*> ScalarType for $ty
        where
            Self: JsonText,
        {
            fn parse(value: Value) -> InputValueResult<Self> {
                parse(value)
            }
            fn to_value(&self) -> Value {
                to_value(self)
            }
        }
    )*};
}

scalar_as_text! {
    [U: Unit, R: Tick + Send + Sync] TimePoint<U, R>, (name_type);
    [U: Unit, R: Tick + Send + Sync + JsonText] Duration<U, R>, (name_type);
    [U: Unit, R: Tick + Send + Sync + JsonText] MonotonicPoint<U, R>, (name_type);
    [const N: usize] crate::pssz::BoundedString<N>, (name_type);
    [const N: usize] crate::pssz::BoundedRawString<N>, (name_type);
    [] crate::pssz::RawString, (name = "RawString");
    [] crate::pssz::Uint256, (name = "UInt256");
}

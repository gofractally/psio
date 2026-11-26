// Time values (docs/time.md): the explicit helpers of time.ts and the time
// marker in the schema-driven PSSZ / JSON / I-JSON codecs.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  PsszError, PsszSchema, parseTimeSchemaId, timeFractionDigits, timePointFromDate, timePointFromIso,
  timePointToDate, timePointToIso, timeSchemaId, type PsszTypeInput, type TimeUnit,
  custom, duration, field, generateSchema, i32, i64, list, monotonic, optional, record, reservedTimeId, timePoint,
  u32, u64, u8, type IrAnyType, type ListView, type RecordView, type TypeDef,
} from '../index.js';

const throwsPssz = (run: () => unknown, what?: RegExp) =>
  assert.throws(run, (e: unknown) => e instanceof PsszError && e.kind !== 'internal' && (!what || what.test(e.message)));

const int = (bits: number, isSigned: boolean): PsszTypeInput => ({ Int: { bits, isSigned } });
const time = (id: string, bits = 64, isSigned = true): PsszTypeInput => ({ Custom: { type: int(bits, isSigned), id } });
const schema = new PsszSchema({});
const T = 1795342530n; // 2026-11-22T10:15:30Z

test('time: schema ids print and parse, canonical spellings only', () => {
  const cases: [string, boolean, bigint, bigint][] = [
    ['time-point-seconds', true, 1n, 1n],
    ['time-point-millis', true, 1n, 1000n],
    ['time-point-micros', true, 1n, 1000000n],
    ['time-point-nanos', true, 1n, 1000000000n],
    ['duration-seconds', false, 1n, 1n],
    ['duration-nanos', false, 1n, 1000000000n],
    ['duration-60/1', false, 60n, 1n],
    ['duration-1/60', false, 1n, 60n],
    ['time-point-1/10', true, 1n, 10n],
    ['time-point-86400/1', true, 86400n, 1n],
    ['duration-1001/30000', false, 1001n, 30000n],
  ];
  for (const [id, point, num, den] of cases) {
    assert.deepEqual(parseTimeSchemaId(id), { point, num, den }, id);
    assert.equal(timeSchemaId({ point, num, den }), id);
  }
  // The printer reduces and uses the names.
  assert.equal(timeSchemaId({ point: false, num: 2n, den: 2000n }), 'duration-millis');
  assert.equal(timeSchemaId({ point: true, num: 120n, den: 2n }), 'time-point-60/1');
  for (const bad of [
    'time-point', 'time-point-', 'duration-1/1', 'duration-1/1000', 'duration-1/1000000',
    'duration-1/1000000000', 'duration-2/4', 'duration-01/7', 'duration-1/07', 'duration-1/0', 'duration-0/1',
    'duration-7', 'duration-1/7 ', ' duration-1/7', 'time-seconds', 'Duration-seconds', 'duration-minutes',
    'duration-seconds ', 'duration--1/7', 'duration-1/-7', 'duration-1.5/7', 'duration-1/7/2', 'hex', '',
    'duration-1234567890123456789/7',
  ])
    assert.equal(parseTimeSchemaId(bad), null, bad);
  assert.equal(timeFractionDigits(1n, 1n), 0);
  assert.equal(timeFractionDigits(1n, 10n), 1);
  assert.equal(timeFractionDigits(1n, 1000000000n), 9);
  assert.equal(timeFractionDigits(1n, 10000000000n), -1);
  assert.equal(timeFractionDigits(1n, 60n), -1);
  assert.equal(timeFractionDigits(60n, 1n), -1);

  // A near-miss id is an unknown custom adapter, like any other.
  for (const id of ['duration-1/1000', 'time-point-minutes', 'duration-2/4', 'time-point'])
    throwsPssz(() => schema.type(time(id)), /unsupported custom adapter/);
  // The tick type is an 8..64-bit integer.
  throwsPssz(() => schema.type(time('duration-seconds', 128)));
  throwsPssz(() => schema.type(time('duration-seconds', 1, false)));
  throwsPssz(() => schema.type({ Custom: { type: { List: int(8, false) }, id: 'duration-seconds' } }));
});

test('time: text has exactly the unit precision; the year range is 0000..9999', () => {
  assert.equal(timePointToIso(0n, 'seconds'), '1970-01-01T00:00:00Z');
  assert.equal(timePointToIso(T, 'seconds'), '2026-11-22T10:15:30Z');
  assert.equal(timePointToIso(T * 1000n + 123n, 'millis'), '2026-11-22T10:15:30.123Z');
  assert.equal(timePointToIso(T * 1000000n + 123456n, 'micros'), '2026-11-22T10:15:30.123456Z');
  assert.equal(timePointToIso(T * 1000000000n + 123456789n, 'nanos'), '2026-11-22T10:15:30.123456789Z');
  assert.equal(timePointToIso(0n, 'millis'), '1970-01-01T00:00:00.000Z');
  assert.equal(timePointToIso(1n, 'nanos'), '1970-01-01T00:00:00.000000001Z');
  assert.equal(timePointToIso(15n, { num: 1, den: 10 }), '1970-01-01T00:00:01.5Z');
  assert.equal(timePointToIso(T, { num: 2n, den: 2n }), '2026-11-22T10:15:30Z');
  // Before the epoch: floor, never a negative fraction.
  assert.equal(timePointToIso(-1n, 'seconds'), '1969-12-31T23:59:59Z');
  assert.equal(timePointToIso(-1n, 'millis'), '1969-12-31T23:59:59.999Z');
  assert.equal(timePointToIso(-1000001n, 'micros'), '1969-12-31T23:59:58.999999Z');
  assert.equal(timePointToIso(-2208988800n, 'seconds'), '1900-01-01T00:00:00Z');
  // Leap days and century rules.
  assert.equal(timePointToIso(951782400n, 'seconds'), '2000-02-29T00:00:00Z');
  assert.equal(timePointToIso(1709251199n, 'seconds'), '2024-02-29T23:59:59Z');
  assert.equal(timePointToIso(4107542400n, 'seconds'), '2100-03-01T00:00:00Z');
  // The range, to the last tick.
  assert.equal(timePointToIso(-62167219200n, 'seconds'), '0000-01-01T00:00:00Z');
  assert.equal(timePointToIso(-62162035201n, 'seconds'), '0000-02-29T23:59:59Z');
  assert.equal(timePointToIso(253402300799n, 'seconds'), '9999-12-31T23:59:59Z');
  assert.equal(timePointToIso(253402300799999999n, 'micros'), '9999-12-31T23:59:59.999999Z');
  throwsPssz(() => timePointToIso(-62167219201n, 'seconds'), /0000\.\.9999/);
  throwsPssz(() => timePointToIso(253402300800n, 'seconds'), /0000\.\.9999/);
  throwsPssz(() => timePointToIso(-62167219200000001n, 'micros'));
  throwsPssz(() => timePointToIso((1n << 63n) - 1n, 'micros'));
  assert.equal(timePointToIso((1n << 63n) - 1n, 'nanos'), '2262-04-11T23:47:16.854775807Z');
  assert.equal(timePointToIso(-(1n << 63n), 'nanos'), '1677-09-21T00:12:43.145224192Z');
  // No RFC 3339 form for other units; ticks are bigint.
  throwsPssz(() => timePointToIso(1n, { num: 60, den: 1 }), /1\/10\^k/);
  throwsPssz(() => timePointToIso(1 as unknown as bigint, 'seconds'), /bigint/);
  throwsPssz(() => timePointToIso(1n, 'minutes' as unknown as TimeUnit));
  throwsPssz(() => timePointToIso(1n, { num: 0, den: 1 }));
});

test('time: reading accepts RFC 3339 and nothing else', () => {
  const sec = (s: string) => timePointFromIso(s, 'seconds');
  assert.equal(sec('2026-11-22T10:15:30Z'), T);
  assert.equal(sec('2026-11-22t10:15:30z'), T);
  assert.equal(sec('2026-11-22T12:15:30+02:00'), T);
  assert.equal(sec('2026-11-22T04:45:30-05:30'), T);
  assert.equal(sec('2026-11-22T10:15:30-00:00'), T);
  assert.equal(sec('2026-11-23T00:15:30+14:00'), T);
  assert.equal(sec('2027-01-01T00:30:00+01:00'), 1798759800n);
  assert.equal(sec('2026-11-22T10:15:30.000000000000000000000Z'), T);
  assert.equal(timePointFromIso('2026-11-22T10:15:30.1Z', 'millis'), T * 1000n + 100n);
  assert.equal(timePointFromIso('2026-11-22T10:15:30.123000Z', 'millis'), T * 1000n + 123n);
  assert.equal(timePointFromIso('2026-11-22T10:15:30.1234567890Z', 'nanos'), T * 1000000000n + 123456789n);
  assert.equal(timePointFromIso('1969-12-31T23:59:59.999999Z', 'micros'), -1n);
  assert.equal(sec('0000-01-01T00:01:00+00:01'), -62167219200n);
  for (const bad of [
    '2026-11-22T10:15:30.5Z', '2016-12-31T23:59:60Z', '2026-11-22T24:00:00Z', '2026-11-22T10:60:00Z',
    '2026-02-29T00:00:00Z', '2100-02-29T00:00:00Z', '2026-04-31T00:00:00Z', '2026-00-10T00:00:00Z',
    '2026-13-10T00:00:00Z', '2026-11-00T00:00:00Z', '', '2026-11-22', '2026-11-22T10:15:30',
    '2026-11-22 10:15:30Z', '2026-11-22T10:15Z', '2026-11-22T10:15:30.Z', '2026-11-22T10:15:30,5Z',
    '2026-11-22T10:15:30ZZ', '2026-11-22T10:15:30Z ', ' 2026-11-22T10:15:30Z', '2026-11-22T10:15:30+0200',
    '2026-11-22T10:15:30+02', '2026-11-22T10:15:30+02:0', '2026-11-22T10:15:30+24:00',
    '2026-11-22T10:15:30+02:60', '20261122T101530Z', '2026-11-22T10:15:3Z', '+2026-11-22T10:15:30Z',
    '-026-11-22T10:15:30Z', '02026-11-22T10:15:30Z', '2026-1-22T10:15:30Z', '2026-11-22T1O:15:30Z',
    '1795342530', '0000-01-01T00:00:00+00:01', '9999-12-31T23:59:59-00:01', '２０２６-11-22T10:15:30Z',
    '2026-11-22T10:15:30\u{ff3a}',
  ])
    throwsPssz(() => sec(bad));
  throwsPssz(() => timePointFromIso('2026-11-22T10:15:30.1234Z', 'millis'), /finer/);
  throwsPssz(() => sec(17 as unknown as string));

  // The tick type bounds the result; nothing wraps.
  const u32 = { bits: 32, signed: false } as const;
  assert.equal(timePointFromIso('2106-02-07T06:28:15Z', 'seconds', u32), 4294967295n);
  throwsPssz(() => timePointFromIso('2106-02-07T06:28:16Z', 'seconds', u32), /tick type/);
  throwsPssz(() => timePointFromIso('1969-12-31T23:59:59Z', 'seconds', u32));
  assert.equal(timePointFromIso('1970-01-01T00:02:07Z', 'seconds', { bits: 8, signed: true }), 127n);
  throwsPssz(() => timePointFromIso('1970-01-01T00:02:08Z', 'seconds', { bits: 8, signed: true }));
  assert.equal(timePointFromIso('2262-04-11T23:47:16.854775807Z', 'nanos'), (1n << 63n) - 1n);
  throwsPssz(() => timePointFromIso('2262-04-11T23:47:16.854775808Z', 'nanos'));
  throwsPssz(() => timePointFromIso('9999-12-31T23:59:59Z', 'nanos'));

  // Round trip over the whole range.
  let x = 0x9e3779b97f4a7c15n;
  const next = () => {
    x ^= x << 13n; x &= (1n << 64n) - 1n; x ^= x >> 7n; x ^= x << 17n; x &= (1n << 64n) - 1n;
    return x;
  };
  for (let i = 0; i < 20000; ++i) {
    const secs = -62167219200n + (next() % (253402300800n + 62167219200n));
    const text = timePointToIso(secs, 'seconds');
    assert.equal(sec(text), secs);
    // Agrees with Date wherever Date has an opinion.
    assert.equal(text, new Date(Number(secs) * 1000).toISOString().replace('.000Z', 'Z').replace(/^\+00/, ''));
    const us = secs * 1000000n + (next() % 1000000n);
    assert.equal(timePointFromIso(timePointToIso(us, 'micros'), 'micros'), us);
  }
});

test('time: Date conversions never lose precision silently', () => {
  assert.equal(timePointToDate(T, 'seconds').toISOString(), '2026-11-22T10:15:30.000Z');
  assert.equal(timePointToDate(T * 1000n + 123n, 'millis').getTime(), Number(T) * 1000 + 123);
  assert.equal(timePointToDate(T * 1000000n + 123000n, 'micros').getTime(), Number(T) * 1000 + 123);
  assert.equal(timePointToDate(-1000000n, 'nanos').getTime(), -1);
  assert.equal(timePointToDate(2n, { num: 60, den: 1 }).getTime(), 120000);
  // Sub-millisecond detail has no Date.
  throwsPssz(() => timePointToDate(T * 1000000n + 123456n, 'micros'), /whole number of milliseconds/);
  throwsPssz(() => timePointToDate(1n, 'nanos'));
  throwsPssz(() => timePointToDate(1n, { num: 1, den: 3 }));
  // Date covers ±8.64e15 ms.
  assert.equal(timePointToDate(8640000000000n, 'seconds').getTime(), 8.64e15);
  throwsPssz(() => timePointToDate(8640000000001n, 'seconds'), /range of Date/);
  throwsPssz(() => timePointToDate(-(1n << 63n), 'seconds'));
  throwsPssz(() => timePointToDate(1 as unknown as bigint, 'millis'));

  const d = new Date('2026-11-22T10:15:30.123Z');
  assert.equal(timePointFromDate(d, 'millis'), T * 1000n + 123n);
  assert.equal(timePointFromDate(d, 'micros'), T * 1000000n + 123000n);
  assert.equal(timePointFromDate(d, 'nanos'), T * 1000000000n + 123000000n);
  // A coarser unit cannot hold the milliseconds.
  throwsPssz(() => timePointFromDate(d, 'seconds'), /exactly/);
  assert.equal(timePointFromDate(new Date('2026-11-22T10:15:30Z'), 'seconds'), T);
  assert.equal(timePointFromDate(new Date(-1), 'micros'), -1000n);
  assert.equal(timePointFromDate(new Date(120000), { num: 60, den: 1 }), 2n);
  throwsPssz(() => timePointFromDate(new Date(90000), { num: 60, den: 1 }));
  throwsPssz(() => timePointFromDate(new Date(NaN), 'millis'), /invalid Date/);
  throwsPssz(() => timePointFromDate('2026' as unknown as Date, 'millis'));
  for (const unit of ['millis', 'micros', 'nanos'] as const)
    assert.equal(timePointToDate(timePointFromDate(d, unit), unit).getTime(), d.getTime());
});

test('time: the codecs carry bigint ticks at every width', () => {
  for (const bits of [8, 16, 32, 64])
    for (const isSigned of [true, false]) {
      const max = isSigned ? (1n << BigInt(bits - 1)) - 1n : (1n << BigInt(bits)) - 1n;
      const min = isSigned ? -(1n << BigInt(bits - 1)) : 0n;
      for (const id of ['duration-seconds', 'time-point-seconds', 'duration-60/1', 'time-point-1/60']) {
        const type = schema.type(time(id, bits, isSigned));
        const plain = schema.type(int(bits, isSigned));
        for (const v of [min, 0n, 1n, max]) {
          const bytes = type.encode(v);
          assert.deepEqual(bytes, plain.encode(v));
          assert.equal(bytes.length, bits / 8);
          assert.equal(type.decode(bytes), v); // strict: a bigint, never a number
          assert.deepEqual(type.encodeKey(v), plain.encodeKey(v));
          assert.equal(type.decodeKey(type.encodeKey(v)), v);
        }
        // A safe-integer number is accepted on input, like any integer.
        assert.deepEqual(type.encode(1), type.encode(1n));
        throwsPssz(() => type.encodeChecked(max + 1n));
        throwsPssz(() => type.encodeChecked(min - 1n));
        throwsPssz(() => type.encodeChecked(1.5));
        throwsPssz(() => type.encodeChecked('1'));
        throwsPssz(() => type.encodeChecked(new Date(0)));
      }
    }
});

test('time: json and ijson', () => {
  const Event = new PsszSchema({
    Event: {
      Object: { members: [
        { name: 'at', type: time('time-point-millis') },
        { name: 'ttl', type: time('duration-millis') },
        { name: 'forced', type: time('duration-seconds'), attributes: [{ name: 'ijsonInt', value: 'string' }, { name: 'jsonInt', value: 'string' }] },
        { name: 'small', type: time('duration-60/1', 32) },
        { name: 'maybe', type: { Option: time('time-point-seconds', 32, false) } },
      ] },
    },
  }).type('Event');
  const value = { at: T * 1000n + 123n, ttl: 9007199254740993n, forced: 6n, small: -90n, maybe: 0n };
  const json = '{"at":"2026-11-22T10:15:30.123Z","ttl":9007199254740993,"forced":"6","small":-90,"maybe":"1970-01-01T00:00:00Z"}';
  const ijson = json.replace('9007199254740993', '"9007199254740993"');
  assert.equal(Event.valueToJson(value, 'json'), json);
  assert.equal(Event.valueToJson(value, 'ijson'), ijson);
  assert.deepEqual(Event.jsonToValue(json, 'json'), value);
  assert.deepEqual(Event.jsonToValue(ijson, 'ijson'), value);
  throwsPssz(() => Event.jsonToValue(json, 'ijson')); // the unsafe bare number
  assert.deepEqual(Event.decode(Event.jsonToPssz(ijson)), value);
  assert.equal(Event.valueToJson({ ...value, maybe: null }, 'json'), json.replace('"1970-01-01T00:00:00Z"', 'null'));

  const ms = schema.type(time('time-point-millis'));
  assert.equal(ms.jsonToValue('"2026-11-22T12:15:30.5+02:00"'), T * 1000n + 500n);
  assert.equal(ms.jsonToValue(' "2026-11-22T10:15:30Z" ', 'json'), T * 1000n);
  // The JSON string's value is what is read, however it was escaped.
  assert.equal(ms.jsonToValue('"2026-11-22T10:15:30\\u005a"'), T * 1000n);
  for (const bad of [
    '"2026-11-22T10:15:30.1234Z"', '"2026-11-22T10:15:60Z"', '"2026-11-22T10:15:30"', '1795342530000', 'null',
    '"1795342530000"', '["2026-11-22T10:15:30Z"]', '"2026-11-22T10:15:30Z\\u0000"', '"' + '0'.repeat(5000) + '"',
  ])
    for (const format of ['json', 'ijson'] as const) throwsPssz(() => ms.jsonToValue(bad, format));
  const dur = schema.type(time('duration-millis'));
  for (const bad of ['"PT1S"', '1.5', '1e3', 'null', '"1970-01-01T00:00:01Z"'])
    for (const format of ['json', 'ijson'] as const) throwsPssz(() => dur.jsonToValue(bad, format));
  assert.equal(dur.jsonToValue('-3', 'json'), -3n);
  assert.equal(dur.jsonToValue('"9223372036854775807"', 'ijson'), (1n << 63n) - 1n);
  assert.equal(dur.valueToJson(-(1n << 63n), 'json'), '-9223372036854775808');

  // A time point outside the year range has no text; binary is unaffected.
  const sec = schema.type(time('time-point-seconds'));
  const far = (1n << 63n) - 1n;
  assert.equal(sec.decode(sec.encode(far)), far);
  for (const format of ['json', 'ijson'] as const) {
    throwsPssz(() => sec.valueToJsonChecked(far, format), /0000\.\.9999/);
    throwsPssz(() => sec.psszToJson(sec.encodeChecked(far), format));
    throwsPssz(() => sec.valueToJsonChecked(1.5, format));
    throwsPssz(() => sec.valueToJsonChecked('2026-11-22T10:15:30Z', format));
  }
  // A unit that is not 1/10^k has no text form; binary and key still work.
  const minutes = schema.type(time('time-point-60/1'));
  assert.equal(minutes.decode(minutes.encode(5n)), 5n);
  assert.equal(minutes.decodeKey(minutes.encodeKey(5n)), 5n);
  throwsPssz(() => minutes.valueToJsonChecked(5n), /no RFC 3339 text form/);
  throwsPssz(() => minutes.jsonToValue('"1970-01-01T00:05:00Z"'), /no RFC 3339 text form/);
  throwsPssz(() => minutes.jsonToValue('5'));
  // ... while a duration of any unit is its integer.
  assert.equal(schema.type(time('duration-1/60', 16)).valueToJson(-7n), '-7');
});

test('time: the ids are built-in presentations of the Custom-id mechanism', () => {
  // No handler is needed, also when every custom id must have one ...
  const strict = new PsszSchema({}, { requireHandlers: true });
  const at = strict.type(time('time-point-millis'));
  assert.equal(at.valueToJson(T * 1000n + 123n), '"2026-11-22T10:15:30.123Z"');
  assert.equal(at.jsonToValue('"2026-11-22T10:15:30.123Z"'), T * 1000n + 123n);
  assert.equal(strict.type(time('duration-seconds', 32)).valueToJson(-5n), '-5');
  // ... while an ordinary custom id under the same schema does need one.
  throwsPssz(() => strict.type({ Custom: { type: int(64, false), id: 'AccountNumber' } }).valueToJsonChecked(1n),
    /no handler registered/);
  // ... and none can be registered for them, or for a reserved spelling.
  const handler = { toText: () => 'x', fromText: () => 0n };
  for (const id of ['time-point-millis', 'duration-60/1', 'time-point', 'duration', 'duration-minutes', 'time-point-'])
    throwsPssz(() => new PsszSchema({}, { handlers: { [id]: handler } }), /built-in time presentation/);
  throwsPssz(() => schema.withHandlers({ 'duration-nanos': handler }), /built-in time presentation/);
  // The marker composes with the other wrappers: box, a custom id, an enum-free alias.
  const boxed = schema.type({ Custom: { type: time('time-point-seconds'), id: 'box' } });
  assert.equal(boxed.valueToJson(T), '"2026-11-22T10:15:30Z"');
  assert.equal(boxed.decode(boxed.encode(T)), T);
  assert.equal(Buffer.from(boxed.encodeKey(T)).toString('hex'), Buffer.from(schema.type(time('time-point-seconds')).encodeKey(T)).toString('hex'));
  // The tick integer may be named or boxed in the schema; anything else is malformed.
  const aliased = new PsszSchema({ Ticks: int(64, true), At: { Custom: { type: 'Ticks', id: 'time-point-seconds' } } });
  assert.equal(aliased.type('At').valueToJson(T), '"2026-11-22T10:15:30Z"');
  throwsPssz(() => schema.type({ Custom: { type: { Custom: { type: { List: int(8, false) }, id: 'utf8' } }, id: 'duration-millis' } }),
    /tick integer/);
  throwsPssz(() => schema.type({ Custom: { type: { Float: { exp: 11, mantissa: 53 } }, id: 'duration-millis' } }), /tick integer/);
});

// ── Monotonic time points (a clock other than the system clock) ─────────────
// The facts of cpp/tests/time_tests.cpp "a monotonic time point is its tick
// count in every format" and "schema marks a monotonic time point with its
// own id".

test('time: a monotonic time point is its tick count in every format', () => {
  const Timer = record('Timer', {
    expires: monotonic('micros'), started: optional(monotonic('seconds')), laps: list(monotonic('millis', u32)),
  });
  const TimerInts = record('TimerInts', { expires: i64, started: optional(i64), laps: list(u32) });
  const timer = { expires: 9007199254740993n, started: -5n, laps: [0n, 4294967295n] };
  const ints = { expires: 9007199254740993n, started: -5n, laps: [0, 4294967295] };

  // Binary and key: the tick integer.
  const bytes = Timer.encode(timer);
  assert.deepEqual(bytes, TimerInts.encode(ints));
  assert.equal(Timer.validate(bytes).ok, true);
  assert.deepEqual(Timer.decode(bytes), timer); // strict: bigint ticks, also the u32 laps
  const key = Timer.encodeKey(timer);
  assert.deepEqual(key, TimerInts.encodeKey(ints));
  assert.deepEqual(Timer.validateKey(key), { ok: true });
  assert.deepEqual(Timer.decodeKey(key), timer);
  // Key order is tick order (bytes compared unsigned).
  const micros = monotonic('micros');
  assert.ok(Buffer.compare(micros.encodeKey(-1n), micros.encodeKey(0n)) < 0);
  assert.deepEqual(monotonic('nanos').encode(7n), i64.encode(7n));
  // Views read the same ticks.
  const view = Timer.view(bytes) as RecordView;
  assert.equal(view.get('expires'), 9007199254740993n);
  assert.equal(view.get('started'), -5n);
  assert.deepEqual([...(view.get('laps') as ListView)], [0n, 4294967295n]);

  // Text: the tick count under the integer rules, never a date.
  const json = '{"expires":9007199254740993,"started":-5,"laps":[0,4294967295]}';
  const ijson = '{"expires":"9007199254740993","started":-5,"laps":[0,4294967295]}';
  assert.equal(Timer.valueToJson(timer, 'json'), json);
  assert.equal(Timer.valueToJson(timer, 'ijson'), ijson);
  assert.equal(TimerInts.valueToJson(ints, 'json'), json);
  assert.equal(TimerInts.valueToJson(ints, 'ijson'), ijson);
  assert.equal(Timer.psszToJson(bytes, 'json'), json);
  assert.deepEqual(Timer.jsonToValue(json, 'json'), timer);
  assert.deepEqual(Timer.jsonToValue(ijson, 'ijson'), timer);
  assert.deepEqual(Timer.jsonToPssz(ijson, 'ijson'), bytes);
  const date = '{"expires":"2026-11-22T10:15:30Z","started":null,"laps":[]}';
  for (const format of ['json', 'ijson'] as const) {
    throwsPssz(() => Timer.jsonToValue(date, format));
    throwsPssz(() => Timer.jsonToPssz(date, format));
    throwsPssz(() => Timer.valueToJsonChecked({ ...timer, expires: '2026-11-22T10:15:30Z' as unknown as bigint }, format));
    throwsPssz(() => Timer.valueToJsonChecked({ ...timer, expires: new Date(0) as unknown as bigint }, format));
  }
  // Member integer-form annotations apply, as to a duration.
  const Forced = record('Forced', { at: field(monotonic('seconds'), { jsonInt: 'string', ijsonInt: 'string' }) });
  assert.equal(Forced.valueToJson({ at: 6n }, 'json'), '{"at":"6"}');
  assert.deepEqual(Forced.jsonToValue('{"at":"6"}', 'ijson'), { at: 6n });

  // The same through a schema, at every width; the tick type bounds the value.
  for (const bits of [8, 16, 32, 64])
    for (const isSigned of [true, false]) {
      const max = isSigned ? (1n << BigInt(bits - 1)) - 1n : (1n << BigInt(bits)) - 1n;
      const min = isSigned ? -(1n << BigInt(bits - 1)) : 0n;
      for (const id of ['monotonic-seconds', 'monotonic-nanos', 'monotonic-60/1', 'monotonic-1/60']) {
        const type = schema.type(time(id, bits, isSigned));
        const plain = schema.type(int(bits, isSigned));
        for (const v of [min, 0n, 1n, max]) {
          assert.deepEqual(type.encode(v), plain.encode(v));
          assert.equal(type.decode(type.encode(v)), v); // strict: a bigint, never a number
          assert.deepEqual(type.encodeKey(v), plain.encodeKey(v));
          assert.equal(type.decodeKey(type.encodeKey(v)), v);
          for (const format of ['json', 'ijson'] as const) {
            const text = type.valueToJson(v, format);
            assert.equal(text, plain.valueToJson(bits <= 32 ? Number(v) : v, format));
            assert.equal(type.jsonToValue(text, format), v);
          }
        }
        assert.deepEqual(type.encode(1), type.encode(1n));
        throwsPssz(() => type.encodeChecked(max + 1n));
        throwsPssz(() => type.encodeChecked(min - 1n));
        throwsPssz(() => type.encodeChecked(new Date(0)));
        throwsPssz(() => type.valueToJsonChecked(max + 1n));
        throwsPssz(() => type.jsonToValue((max + 1n).toString(), 'json'));
      }
    }
  const mono = schema.type(time('monotonic-seconds'));
  for (const bad of ['"PT1S"', '1.5', '1e3', 'null', '"1970-01-01T00:00:01Z"', '"2026-11-22T10:15:30Z"'])
    for (const format of ['json', 'ijson'] as const) throwsPssz(() => mono.jsonToValue(bad, format));
  // No year range: every tick count has text (a time point of this value has none).
  assert.equal(mono.valueToJson((1n << 63n) - 1n, 'json'), '9223372036854775807');
  assert.equal(mono.valueToJson(-(1n << 63n), 'ijson'), '"-9223372036854775808"');
  // A unit with no RFC 3339 form still has tick text.
  assert.equal(schema.type(time('monotonic-60/1', 32)).valueToJson(0n), '0');
  assert.equal(monotonic({ num: 60, den: 1 }, i32).valueToJson(0n), '0');
});

test('time: schema marks a monotonic time point with its own id', () => {
  const cases: [TypeDef<bigint>, string, number, boolean, bigint, bigint][] = [
    [monotonic('seconds'), 'monotonic-seconds', 64, true, 1n, 1n],
    [monotonic('micros'), 'monotonic-micros', 64, true, 1n, 1000000n],
    [monotonic('millis', u32), 'monotonic-millis', 32, false, 1n, 1000n],
    [monotonic('nanos'), 'monotonic-nanos', 64, true, 1n, 1000000000n],
    [monotonic({ num: 60, den: 1 }, i32), 'monotonic-60/1', 32, true, 60n, 1n],
    [monotonic({ num: 120n, den: 2n }, i32), 'monotonic-60/1', 32, true, 60n, 1n],
    [monotonic({ num: 1, den: 1000 }, u8), 'monotonic-millis', 8, false, 1n, 1000n],
  ];
  for (const [def, id, bits, isSigned, num, den] of cases) {
    const ir = generateSchema({ T: def }).ir;
    const byName = new Map(ir.types.map((m) => [m.name, m.type]));
    const resolved = (t: IrAnyType): IrAnyType['value'] => {
      let v = t.value;
      for (let hops = 0; v.type === 'Type' && hops < 8; ++hops) v = byName.get(v.value.type)!.value;
      return v;
    };
    const node = resolved(byName.get('T')!);
    assert.ok(node.type === 'Custom', id);
    assert.equal(node.value.id, id);
    assert.deepEqual(resolved(node.value.type), { type: 'Int', value: { bits, isSigned } }, id);
    const kind = parseTimeSchemaId(id);
    assert.deepEqual(kind, { point: false, monotonic: true, num, den }, id);
    assert.equal(timeSchemaId(kind!), id);
    assert.equal(reservedTimeId(id), true);
  }
  assert.equal(timeSchemaId({ point: false, monotonic: true, num: 2n, den: 2000n }), 'monotonic-millis');
  throwsPssz(() => timeSchemaId({ point: true, monotonic: true, num: 1n, den: 1n }));
  for (const bad of ['monotonic', 'monotonic-', 'monotonic-1/1000', 'monotonic-2/4', 'monotonic-minutes', 'Monotonic-seconds',
    'monotonic-seconds ', 'monotonic-01/7', 'monotonic--1/7', 'monotonically'])
    assert.equal(parseTimeSchemaId(bad), null, bad);
  assert.equal(reservedTimeId('monotonic'), true);
  assert.equal(reservedTimeId('monotonic-anything'), true);
  assert.equal(reservedTimeId('monotonically'), false);
  // A time point and a monotonic time point of one unit are different ids.
  assert.notEqual(timeSchemaId({ point: true, num: 1n, den: 1000000n }), timeSchemaId({ point: false, monotonic: true, num: 1n, den: 1000000n }));
  // ... and different typed definitions from one another and from the tick integer.
  const irOf = (def: TypeDef<any, any>) => JSON.stringify(generateSchema({ T: def }).ir);
  const distinct = new Set([monotonic('micros'), timePoint('micros'), duration('micros'), i64, monotonic('millis'), monotonic('micros', u64)].map(irOf));
  assert.equal(distinct.size, 6);

  // A reserved spelling that is not a time id, or a time id over anything
  // but an 8/16/32/64-bit integer, is a malformed schema.
  for (const id of ['monotonic', 'monotonic-', 'monotonic-1/1000', 'monotonic-2/4', 'monotonic-minutes', 'monotonic-anything'])
    throwsPssz(() => schema.type(time(id)), /unsupported custom adapter/);
  throwsPssz(() => schema.type(time('monotonic-seconds', 128)), /tick integer/);
  throwsPssz(() => schema.type(time('monotonic-seconds', 1, false)));
  throwsPssz(() => schema.type({ Custom: { type: { List: int(8, false) }, id: 'monotonic-seconds' } }), /tick integer/);
  throwsPssz(() => schema.type({ Custom: { type: { Float: { exp: 11, mantissa: 53 } }, id: 'monotonic-millis' } }), /tick integer/);
  throwsPssz(() => schema.type({ Custom: { type: { Custom: { type: { List: int(8, false) }, id: 'utf8' } }, id: 'monotonic-millis' } }), /tick integer/);
  // "monotonically" is not reserved: an ordinary custom id, its wire type's text.
  assert.equal(schema.type(time('monotonically')).valueToJson(5n), '5');

  // A built-in presentation: presented under requireHandlers, no handler accepted.
  const strict = new PsszSchema({}, { requireHandlers: true });
  const at = strict.type(time('monotonic-micros'));
  assert.equal(at.valueToJson(-7n), '-7');
  assert.equal(at.jsonToValue('"9007199254740993"', 'ijson'), 9007199254740993n);
  throwsPssz(() => strict.type(time('monotonically')).valueToJsonChecked(1n), /no handler registered/);
  const handler = { toText: () => 'x', fromText: () => 0n };
  for (const id of ['monotonic-micros', 'monotonic-60/1', 'monotonic', 'monotonic-', 'monotonic-minutes'])
    throwsPssz(() => new PsszSchema({}, { handlers: { [id]: handler } }), /built-in time presentation/);
  throwsPssz(() => schema.withHandlers({ 'monotonic-nanos': handler }), /built-in time presentation/);
  throwsPssz(() => custom('monotonic-micros', i64, handler), /built-in id/);
  // The tick integer may be named or boxed, as for the other time ids.
  const aliased = new PsszSchema({ Ticks: int(64, true), At: { Custom: { type: 'Ticks', id: 'monotonic-seconds' } } });
  assert.equal(aliased.type('At').valueToJson(T), String(T));
  const boxed = schema.type({ Custom: { type: time('monotonic-seconds'), id: 'box' } });
  assert.equal(boxed.valueToJson(T), String(T));
  assert.equal(boxed.decode(boxed.encode(T)), T);
});

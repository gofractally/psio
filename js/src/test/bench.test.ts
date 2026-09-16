// bench.test.ts — Comprehensive fracpack benchmarks
//
// Measures pack, unpack, view, readField, validate, JSON conversion,
// in-place mutation, and competitor (JSON.parse/stringify) performance
// across multiple schema tiers.

import { describe, it, after } from 'node:test';
import {
  bool, u8, u16, u32, u64, i16, i32, f32, f64, str, vec, optional,
  variant, struct, fixedStruct,
  fracToJson, jsonToFrac, valueToJson,
  validate,
  MutView,
  type RawFieldResult,
} from '../index.js';

// ========================= Benchmark harness =========================

interface BenchResult {
  opsPerSec: number;
  medianNs: number;
}

function bench(name: string, fn: () => any, minTimeMs = 500): BenchResult {
  // Warmup: 100 iterations
  for (let i = 0; i < 100; i++) {
    (globalThis as any).__benchSink = fn();
  }

  // Calibrate: how many iterations to fill minTimeMs
  const calStart = performance.now();
  let calIters = 0;
  while (performance.now() - calStart < 50) {
    (globalThis as any).__benchSink = fn();
    calIters++;
  }
  const itersPerMs = calIters / 50;
  const targetIters = Math.max(1000, Math.floor(itersPerMs * minTimeMs));

  // Measure
  const start = performance.now();
  for (let i = 0; i < targetIters; i++) {
    (globalThis as any).__benchSink = fn();
  }
  const elapsed = performance.now() - start;

  const opsPerSec = Math.round(targetIters / (elapsed / 1000));
  const medianNs = Math.round((elapsed * 1e6) / targetIters);
  return { opsPerSec, medianNs };
}

// ========================= Result collection =========================

interface ResultRow {
  operation: string;
  schema: string;
  opsPerSec: number;
  medianNs: number;
  bytes: number;
}

const results: ResultRow[] = [];

function record(operation: string, schema: string, r: BenchResult, bytes: number) {
  results.push({ operation, schema, opsPerSec: r.opsPerSec, medianNs: r.medianNs, bytes });
}

function fmtNum(n: number): string {
  return n.toLocaleString('en-US');
}

// ========================= Schema definitions =========================

const Point = fixedStruct({ x: f64, y: f64 });

const Token = struct({ kind: u16, offset: u32, length: u32, text: str });

const UserProfile = struct({
  id: u64, name: str, email: str, bio: optional(str),
  age: u32, score: f64, tags: vec(str), verified: bool,
});

const LineItem = struct({ product: str, qty: u32, unit_price: f64 });

const Order = struct({
  id: u64,
  customer: UserProfile,
  items: vec(LineItem),
  total: f64,
  note: optional(str),
  status: variant({ pending: u32, shipped: str, delivered: u64, cancelled: str }),
});

const SensorReading = struct({
  timestamp: u64, device_id: str,
  temp: f64, humidity: f64, pressure: f64,
  accel_x: f64, accel_y: f64, accel_z: f64,
  gyro_x: f64, gyro_y: f64, gyro_z: f64,
  mag_x: f64, mag_y: f64, mag_z: f64,
  battery: f32, signal_dbm: i16,
  error_code: optional(u32), firmware: str,
});

// ========================= Test data =========================

const pointData = { x: 3.14159, y: 2.71828 };

const tokenData = { kind: 42, offset: 1024, length: 15, text: 'identifier_name' };

const userData = {
  id: 123456789n,
  name: 'Alice Johnson',
  email: 'alice@example.com',
  bio: 'Software engineer who loves Rust and WebAssembly',
  age: 32,
  score: 98.5,
  tags: ['developer', 'rust', 'wasm', 'open-source'],
  verified: true,
};

const lineItems = [
  { product: 'Widget A', qty: 3, unit_price: 19.99 },
  { product: 'Gadget B', qty: 1, unit_price: 49.95 },
  { product: 'Thingamajig C', qty: 10, unit_price: 2.50 },
];

const orderData = {
  id: 987654321n,
  customer: userData,
  items: lineItems,
  total: 134.92,
  note: 'Please ship ASAP',
  status: { type: 'shipped' as const, value: 'UPS-1234567890' },
};

const sensorData = {
  timestamp: 1700000000000n, device_id: 'sensor-A7-north',
  temp: 23.45, humidity: 65.2, pressure: 1013.25,
  accel_x: 0.01, accel_y: -0.02, accel_z: 9.81,
  gyro_x: 0.001, gyro_y: -0.003, gyro_z: 0.002,
  mag_x: 25.1, mag_y: -12.3, mag_z: 42.7,
  battery: 3.72, signal_dbm: -67,
  error_code: null, firmware: 'v2.1.3-rc1',
};

// ========================= Benchmarks =========================

describe('Fracpack Benchmarks', () => {

  // ── Pack ──

  describe('pack', () => {
    it('pack/Point', () => {
      const r = bench('pack/Point', () => Point.pack(pointData));
      const packed = Point.pack(pointData);
      record('pack', 'Point', r, packed.length);
    });

    it('pack/Token', () => {
      const r = bench('pack/Token', () => Token.pack(tokenData));
      const packed = Token.pack(tokenData);
      record('pack', 'Token', r, packed.length);
    });

    it('pack/UserProfile', () => {
      const r = bench('pack/UserProfile', () => UserProfile.pack(userData));
      const packed = UserProfile.pack(userData);
      record('pack', 'UserProfile', r, packed.length);
    });

    it('pack/Order', () => {
      const r = bench('pack/Order', () => Order.pack(orderData));
      const packed = Order.pack(orderData);
      record('pack', 'Order', r, packed.length);
    });

    it('pack/SensorReading', () => {
      const r = bench('pack/SensorReading', () => SensorReading.pack(sensorData));
      const packed = SensorReading.pack(sensorData);
      record('pack', 'SensorReading', r, packed.length);
    });
  });

  // ── Unpack ──

  describe('unpack', () => {
    it('unpack/Point', () => {
      const packed = Point.pack(pointData);
      const r = bench('unpack/Point', () => Point.unpack(packed));
      record('unpack', 'Point', r, packed.length);
    });

    it('unpack/Token', () => {
      const packed = Token.pack(tokenData);
      const r = bench('unpack/Token', () => Token.unpack(packed));
      record('unpack', 'Token', r, packed.length);
    });

    it('unpack/UserProfile', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('unpack/UserProfile', () => UserProfile.unpack(packed));
      record('unpack', 'UserProfile', r, packed.length);
    });

    it('unpack/Order', () => {
      const packed = Order.pack(orderData);
      const r = bench('unpack/Order', () => Order.unpack(packed));
      record('unpack', 'Order', r, packed.length);
    });

    it('unpack/SensorReading', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('unpack/SensorReading', () => SensorReading.unpack(packed));
      record('unpack', 'SensorReading', r, packed.length);
    });
  });

  // ── View (the fracpack advantage) ──

  describe('view', () => {
    it('view-one/UserProfile.name', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('view-one/UserProfile.name', () => {
        const v = UserProfile.view(packed);
        return v.name;
      });
      record('view-one', 'UserProfile.name', r, packed.length);
    });

    it('view-one/UserProfile.verified (last field)', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('view-one/UserProfile.verified', () => {
        const v = UserProfile.view(packed);
        return v.verified;
      });
      record('view-one', 'UserProfile.verified', r, packed.length);
    });

    it('view-one/Order.total', () => {
      const packed = Order.pack(orderData);
      const r = bench('view-one/Order.total', () => {
        const v = Order.view(packed);
        return v.total;
      });
      record('view-one', 'Order.total', r, packed.length);
    });

    it('view-one/SensorReading.firmware (last string)', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('view-one/SensorReading.firmware', () => {
        const v = SensorReading.view(packed);
        return v.firmware;
      });
      record('view-one', 'SensorReading.firmware', r, packed.length);
    });

    it('view-all/UserProfile', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('view-all/UserProfile', () => {
        const v = UserProfile.view(packed);
        return { ...v };
      });
      record('view-all', 'UserProfile', r, packed.length);
    });

    it('view-all/Order', () => {
      const packed = Order.pack(orderData);
      const r = bench('view-all/Order', () => {
        const v = Order.view(packed);
        // Force access of all top-level fields
        return {
          id: v.id, customer: v.customer, items: v.items,
          total: v.total, note: v.note, status: v.status,
        };
      });
      record('view-all', 'Order', r, packed.length);
    });

    it('view-all/Point', () => {
      const packed = Point.pack(pointData);
      const r = bench('view-all/Point', () => {
        const v = Point.view(packed);
        return { x: v.x, y: v.y };
      });
      record('view-all', 'Point', r, packed.length);
    });

    it('view-all/SensorReading', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('view-all/SensorReading', () => {
        const v = SensorReading.view(packed);
        return {
          timestamp: v.timestamp, device_id: v.device_id,
          temp: v.temp, humidity: v.humidity, pressure: v.pressure,
          accel_x: v.accel_x, accel_y: v.accel_y, accel_z: v.accel_z,
          gyro_x: v.gyro_x, gyro_y: v.gyro_y, gyro_z: v.gyro_z,
          mag_x: v.mag_x, mag_y: v.mag_y, mag_z: v.mag_z,
          battery: v.battery, signal_dbm: v.signal_dbm,
          error_code: v.error_code, firmware: v.firmware,
        };
      });
      record('view-all', 'SensorReading', r, packed.length);
    });
  });

  // ── readField ──

  describe('readField', () => {
    it('readField/UserProfile.name', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('readField/UserProfile.name', () => UserProfile.readField(packed, 'name'));
      record('readField', 'UserProfile.name', r, packed.length);
    });

    it('readField/UserProfile.id', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('readField/UserProfile.id', () => UserProfile.readField(packed, 'id'));
      record('readField', 'UserProfile.id', r, packed.length);
    });

    it('readField/UserProfile.verified', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('readField/UserProfile.verified', () => UserProfile.readField(packed, 'verified'));
      record('readField', 'UserProfile.verified', r, packed.length);
    });

    it('readField/SensorReading.firmware', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('readField/SensorReading.firmware', () => SensorReading.readField(packed, 'firmware'));
      record('readField', 'SensorReading.firmware', r, packed.length);
    });

    it('readField/Order.customer.name', () => {
      const packed = Order.pack(orderData);
      const r = bench('readField/Order.customer.name', () => {
        const customer = Order.readField(packed, 'customer') as any;
        return customer.name;
      });
      record('readField', 'Order.customer.name', r, packed.length);
    });
  });

  // ── Validate ──

  describe('validate', () => {
    it('validate/Point', () => {
      const packed = Point.pack(pointData);
      const r = bench('validate/Point', () => validate(Point, packed));
      record('validate', 'Point', r, packed.length);
    });

    it('validate/UserProfile', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('validate/UserProfile', () => validate(UserProfile, packed));
      record('validate', 'UserProfile', r, packed.length);
    });

    it('validate/Order', () => {
      const packed = Order.pack(orderData);
      const r = bench('validate/Order', () => validate(Order, packed));
      record('validate', 'Order', r, packed.length);
    });

    it('validate/SensorReading', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('validate/SensorReading', () => validate(SensorReading, packed));
      record('validate', 'SensorReading', r, packed.length);
    });
  });

  // ── JSON ──

  describe('json', () => {
    it('json-write/UserProfile', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('json-write/UserProfile', () => fracToJson(UserProfile, packed));
      record('json-write', 'UserProfile', r, packed.length);
    });

    it('json-read/UserProfile', () => {
      const packed = UserProfile.pack(userData);
      const jsonStr = JSON.stringify(fracToJson(UserProfile, packed));
      const r = bench('json-read/UserProfile', () => jsonToFrac(UserProfile, JSON.parse(jsonStr)));
      record('json-read', 'UserProfile', r, packed.length);
    });

    it('json-write/Order', () => {
      const packed = Order.pack(orderData);
      const r = bench('json-write/Order', () => fracToJson(Order, packed));
      record('json-write', 'Order', r, packed.length);
    });

    it('json-read/Order', () => {
      const packed = Order.pack(orderData);
      const jsonStr = JSON.stringify(fracToJson(Order, packed));
      const r = bench('json-read/Order', () => jsonToFrac(Order, JSON.parse(jsonStr)));
      record('json-read', 'Order', r, packed.length);
    });

    it('json-write/SensorReading', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('json-write/SensorReading', () => fracToJson(SensorReading, packed));
      record('json-write', 'SensorReading', r, packed.length);
    });

    it('json-read/SensorReading', () => {
      const packed = SensorReading.pack(sensorData);
      const jsonStr = JSON.stringify(fracToJson(SensorReading, packed));
      const r = bench('json-read/SensorReading', () => jsonToFrac(SensorReading, JSON.parse(jsonStr)));
      record('json-read', 'SensorReading', r, packed.length);
    });
  });

  // ── Mutation (MutView) ──

  describe('mutation', () => {
    it('mutate/UserProfile.id (scalar)', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('mutate/UserProfile.id', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('id', 999999n);
        return mv.toBytes();
      });
      record('mutate', 'UserProfile.id', r, packed.length);
    });

    it('mutate/UserProfile.name (same-size string)', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('mutate/UserProfile.name', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('name', 'Bobby Johnson'); // same length as 'Alice Johnson'
        return mv.toBytes();
      });
      record('mutate', 'UserProfile.name', r, packed.length);
    });

    it('mutate/UserProfile.name-grow (short to long)', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('mutate/UserProfile.name-grow', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('name', 'Alexandria Bartholomew-Worthington III');
        return mv.toBytes();
      });
      record('mutate', 'UserProfile.name-grow', r, packed.length);
    });

    it('mutate/UserProfile.name-shrink (long to short)', () => {
      const longNameUser = { ...userData, name: 'Alexandria Bartholomew-Worthington III' };
      const packed = UserProfile.pack(longNameUser);
      const r = bench('mutate/UserProfile.name-shrink', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('name', 'Al');
        return mv.toBytes();
      });
      record('mutate', 'UserProfile.name-shrink', r, packed.length);
    });

    it('mutate/UserProfile.bio (None to Some)', () => {
      const noBioUser = { ...userData, bio: null };
      const packed = UserProfile.pack(noBioUser);
      const r = bench('mutate/UserProfile.bio-toggle', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('bio', 'A brand new bio string for benchmarking purposes');
        return mv.toBytes();
      });
      record('mutate', 'UserProfile.bio-toggle', r, packed.length);
    });

    it('mutate-vs-repack/UserProfile.id', () => {
      const packed = UserProfile.pack(userData);

      // MutView path
      const mutR = bench('mutate-path', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('id', 999999n);
        return mv.toBytes();
      });

      // Unpack-modify-repack path
      const repackR = bench('repack-path', () => {
        const obj = UserProfile.unpack(packed);
        obj.id = 999999n;
        return UserProfile.pack(obj);
      });

      record('mutate-path', 'UserProfile.id', mutR, packed.length);
      record('repack-path', 'UserProfile.id', repackR, packed.length);

      const speedup = (repackR.medianNs / mutR.medianNs).toFixed(2);
      console.log(`    MutView speedup over unpack+repack: ${speedup}x`);
    });

    it('mutate-vs-repack/UserProfile.name', () => {
      const packed = UserProfile.pack(userData);

      const mutR = bench('mutate-path', () => {
        const mv = new MutView(UserProfile, packed);
        mv.set('name', 'Bobby Johnson');
        return mv.toBytes();
      });

      const repackR = bench('repack-path', () => {
        const obj = UserProfile.unpack(packed);
        obj.name = 'Bobby Johnson';
        return UserProfile.pack(obj);
      });

      record('mutate-name', 'UserProfile.name', mutR, packed.length);
      record('repack-name', 'UserProfile.name', repackR, packed.length);

      const speedup = (repackR.medianNs / mutR.medianNs).toFixed(2);
      console.log(`    MutView speedup over unpack+repack: ${speedup}x`);
    });

    it('mutate-fast/UserProfile.id (fast mode)', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('mutate-fast/UserProfile.id', () => {
        const mv = new MutView(UserProfile, packed, { fast: true });
        mv.set('id', 999999n);
        return mv.toBytes();
      });
      record('mutate-fast', 'UserProfile.id', r, packed.length);
    });

    it('mutate-fast/UserProfile.name (fast mode)', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('mutate-fast/UserProfile.name', () => {
        const mv = new MutView(UserProfile, packed, { fast: true });
        mv.set('name', 'Bobby Johnson');
        return mv.toBytes();
      });
      record('mutate-fast', 'UserProfile.name', r, packed.length);
    });

    it('mutate/SensorReading.signal_dbm (wide struct, last fixed)', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('mutate/SensorReading.signal_dbm', () => {
        const mv = new MutView(SensorReading, packed);
        mv.set('signal_dbm', -42);
        return mv.toBytes();
      });
      record('mutate', 'SensorReading.signal_dbm', r, packed.length);
    });
  });

  // ── Competitors: JSON.parse / JSON.stringify ──

  describe('competitor', () => {
    it('competitor/JSON.stringify (UserProfile)', () => {
      // Use plain JS object (no bigint) for fair JSON comparison
      const plainUser = { ...userData, id: Number(userData.id) };
      const r = bench('JSON.stringify', () => JSON.stringify(plainUser));
      const jsonBytes = new TextEncoder().encode(JSON.stringify(plainUser)).length;
      record('JSON.stringify', 'UserProfile', r, jsonBytes);
    });

    it('competitor/JSON.parse (UserProfile)', () => {
      const plainUser = { ...userData, id: Number(userData.id) };
      const jsonStr = JSON.stringify(plainUser);
      const r = bench('JSON.parse', () => JSON.parse(jsonStr));
      const jsonBytes = new TextEncoder().encode(jsonStr).length;
      record('JSON.parse', 'UserProfile', r, jsonBytes);
    });

    it('competitor/JSON.stringify (Order)', () => {
      const plainOrder = {
        ...orderData,
        id: Number(orderData.id),
        customer: { ...orderData.customer, id: Number(orderData.customer.id) },
      };
      const r = bench('JSON.stringify', () => JSON.stringify(plainOrder));
      const jsonBytes = new TextEncoder().encode(JSON.stringify(plainOrder)).length;
      record('JSON.stringify', 'Order', r, jsonBytes);
    });

    it('competitor/JSON.parse (Order)', () => {
      const plainOrder = {
        ...orderData,
        id: Number(orderData.id),
        customer: { ...orderData.customer, id: Number(orderData.customer.id) },
      };
      const jsonStr = JSON.stringify(plainOrder);
      const r = bench('JSON.parse', () => JSON.parse(jsonStr));
      const jsonBytes = new TextEncoder().encode(jsonStr).length;
      record('JSON.parse', 'Order', r, jsonBytes);
    });
  });

  // ── Array scaling ──

  describe('array-scaling', () => {
    const PointVec = vec(Point);

    for (const n of [10, 100, 1000, 10000]) {
      const points = Array.from({ length: n }, (_, i) => ({
        x: Math.sin(i) * 100,
        y: Math.cos(i) * 100,
      }));

      it(`array/pack-points-${n}`, () => {
        const r = bench(`pack-points-${n}`, () => PointVec.pack(points));
        const packed = PointVec.pack(points);
        record('pack-array', `Point[${n}]`, r, packed.length);
      });

      it(`array/unpack-points-${n}`, () => {
        const packed = PointVec.pack(points);
        const r = bench(`unpack-points-${n}`, () => PointVec.unpack(packed));
        record('unpack-array', `Point[${n}]`, r, packed.length);
      });

      it(`array/view-points-${n}`, () => {
        const packed = PointVec.pack(points);
        const r = bench(`view-points-${n}`, () => {
          const v = PointVec.view(packed);
          return v[0];
        });
        record('view-array', `Point[${n}]`, r, packed.length);
      });
    }
  });

  // ── Roundtrip ──

  describe('roundtrip', () => {
    it('roundtrip/UserProfile', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('roundtrip/UserProfile', () => {
        const obj = UserProfile.unpack(packed);
        return UserProfile.pack(obj);
      });
      record('roundtrip', 'UserProfile', r, packed.length);
    });

    it('roundtrip/Order', () => {
      const packed = Order.pack(orderData);
      const r = bench('roundtrip/Order', () => {
        const obj = Order.unpack(packed);
        return Order.pack(obj);
      });
      record('roundtrip', 'Order', r, packed.length);
    });
  });

  // ── View vs Native Object Access ──

  describe('view vs native object access', () => {

    // ── Point (fixed struct, should be nearly equal) ──

    it('Point/native', () => {
      const p = { x: 1.5, y: 2.5 };
      const r = bench('native', () => {
        const _ = p.x + p.y;
        return _;
      });
      record('access-native', 'Point', r, 0);
      console.log(`  Point/native: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('Point/view', () => {
      const packed = Point.pack({ x: 1.5, y: 2.5 });
      const r = bench('view', () => {
        const v = Point.view(packed);
        const _ = v.x + v.y;
        return _;
      });
      record('access-view', 'Point', r, packed.length);
      console.log(`  Point/view: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('Point/unpack+access', () => {
      const packed = Point.pack({ x: 1.5, y: 2.5 });
      const r = bench('unpack', () => {
        const p = Point.unpack(packed);
        const _ = p.x + p.y;
        return _;
      });
      record('access-unpack', 'Point', r, packed.length);
      console.log(`  Point/unpack+access: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('Point/cached-view', () => {
      const packed = Point.pack({ x: 1.5, y: 2.5 });
      const v = Point.view(packed);
      const r = bench('cached-view', () => {
        const _ = v.x + v.y;
        return _;
      });
      record('access-cached-view', 'Point', r, packed.length);
      console.log(`  Point/cached-view: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    // ── UserProfile (8 fields with strings, vec, optional) ──

    it('UserProfile/native', () => {
      const u = {
        id: 12345n, name: 'Alice', email: 'alice@test.com',
        bio: 'A bio', age: 30, score: 95.5,
        tags: ['go', 'perf'], verified: true,
      };
      const r = bench('native', () => {
        let _: any = u.id; _ = u.name; _ = u.email; _ = u.bio;
        _ = u.age; _ = u.score; _ = u.tags; _ = u.verified;
        return _;
      });
      record('access-native', 'UserProfile', r, 0);
      console.log(`  UserProfile/native: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('UserProfile/view-raw (no string decode)', () => {
      const packed = UserProfile.pack(userData);
      const fieldNames = ['id', 'name', 'email', 'bio', 'age', 'score', 'tags', 'verified'];
      const r = bench('view-raw', () => {
        let _: RawFieldResult;
        for (const f of fieldNames) _ = UserProfile.rawField(packed, f);
        return _!;
      });
      record('access-view', 'UserProfile', r, packed.length);
      console.log(`  UserProfile/view-raw: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('UserProfile/unpack+access', () => {
      const packed = UserProfile.pack(userData);
      const r = bench('unpack', () => {
        const u = UserProfile.unpack(packed);
        let _: any = u.id; _ = u.name; _ = u.email; _ = u.bio;
        _ = u.age; _ = u.score; _ = u.tags; _ = u.verified;
        return _;
      });
      record('access-unpack', 'UserProfile', r, packed.length);
      console.log(`  UserProfile/unpack+access: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('UserProfile/cached-view', () => {
      const packed = UserProfile.pack(userData);
      const v = UserProfile.view(packed);
      const r = bench('cached-view', () => {
        let _: any = v.id; _ = v.name; _ = v.email; _ = v.bio;
        _ = v.age; _ = v.score; _ = v.tags; _ = v.verified;
        return _;
      });
      record('access-cached-view', 'UserProfile', r, packed.length);
      console.log(`  UserProfile/cached-view: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    // ── SensorReading (18 fields, wide struct) ──

    it('SensorReading/native', () => {
      const s = { ...sensorData };
      const r = bench('native', () => {
        let _: any = s.timestamp; _ = s.device_id;
        _ = s.temp; _ = s.humidity; _ = s.pressure;
        _ = s.accel_x; _ = s.accel_y; _ = s.accel_z;
        _ = s.gyro_x; _ = s.gyro_y; _ = s.gyro_z;
        _ = s.mag_x; _ = s.mag_y; _ = s.mag_z;
        _ = s.battery; _ = s.signal_dbm;
        _ = s.error_code; _ = s.firmware;
        return _;
      });
      record('access-native', 'SensorReading', r, 0);
      console.log(`  SensorReading/native: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('SensorReading/view-raw (no string decode)', () => {
      const packed = SensorReading.pack(sensorData);
      const fieldNames = [
        'timestamp', 'device_id', 'temp', 'humidity', 'pressure',
        'accel_x', 'accel_y', 'accel_z',
        'gyro_x', 'gyro_y', 'gyro_z',
        'mag_x', 'mag_y', 'mag_z',
        'battery', 'signal_dbm', 'error_code', 'firmware',
      ];
      const r = bench('view-raw', () => {
        let _: RawFieldResult;
        for (const f of fieldNames) _ = SensorReading.rawField(packed, f);
        return _!;
      });
      record('access-view', 'SensorReading', r, packed.length);
      console.log(`  SensorReading/view-raw: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('SensorReading/unpack+access', () => {
      const packed = SensorReading.pack(sensorData);
      const r = bench('unpack', () => {
        const s = SensorReading.unpack(packed);
        let _: any = s.timestamp; _ = s.device_id;
        _ = s.temp; _ = s.humidity; _ = s.pressure;
        _ = s.accel_x; _ = s.accel_y; _ = s.accel_z;
        _ = s.gyro_x; _ = s.gyro_y; _ = s.gyro_z;
        _ = s.mag_x; _ = s.mag_y; _ = s.mag_z;
        _ = s.battery; _ = s.signal_dbm;
        _ = s.error_code; _ = s.firmware;
        return _;
      });
      record('access-unpack', 'SensorReading', r, packed.length);
      console.log(`  SensorReading/unpack+access: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    it('SensorReading/cached-view', () => {
      const packed = SensorReading.pack(sensorData);
      const v = SensorReading.view(packed);
      const r = bench('cached-view', () => {
        let _: any = v.timestamp; _ = v.device_id;
        _ = v.temp; _ = v.humidity; _ = v.pressure;
        _ = v.accel_x; _ = v.accel_y; _ = v.accel_z;
        _ = v.gyro_x; _ = v.gyro_y; _ = v.gyro_z;
        _ = v.mag_x; _ = v.mag_y; _ = v.mag_z;
        _ = v.battery; _ = v.signal_dbm;
        _ = v.error_code; _ = v.firmware;
        return _;
      });
      record('access-cached-view', 'SensorReading', r, packed.length);
      console.log(`  SensorReading/cached-view: ${fmtNum(r.opsPerSec)} ops/sec (${r.medianNs}ns)`);
    });

    // ── Summary comparison ──

    after(() => {
      console.log('\n');
      console.log('='.repeat(72));
      console.log('VIEW vs NATIVE ACCESS COMPARISON');
      console.log('='.repeat(72));

      const hdr = [
        'Schema'.padEnd(18),
        'native_ns'.padStart(10),
        'raw_ns'.padStart(10),
        'cached_ns'.padStart(10),
        'unpack_ns'.padStart(10),
        'raw/nat'.padStart(10),
      ].join('  ');
      console.log(hdr);
      console.log('-'.repeat(72));

      for (const schema of ['Point', 'UserProfile', 'SensorReading']) {
        const nat = results.find(r => r.operation === 'access-native' && r.schema === schema);
        const vw = results.find(r => r.operation === 'access-view' && r.schema === schema);
        const cv = results.find(r => r.operation === 'access-cached-view' && r.schema === schema);
        const up = results.find(r => r.operation === 'access-unpack' && r.schema === schema);
        if (!nat || !vw || !cv || !up) continue;
        const ratio = (vw.medianNs / nat.medianNs).toFixed(2);
        const line = [
          schema.padEnd(18),
          fmtNum(nat.medianNs).padStart(10),
          fmtNum(vw.medianNs).padStart(10),
          fmtNum(cv.medianNs).padStart(10),
          fmtNum(up.medianNs).padStart(10),
          (ratio + 'x').padStart(10),
        ].join('  ');
        console.log(line);
      }
      console.log('='.repeat(72));
      console.log('(raw/nat = ratio of rawField navigation to native access; lower is better, 1.00x = parity)');
      console.log('');
    });
  });

  // ── Print results table ──

  after(() => {
    if (results.length === 0) return;

    console.log('\n');
    console.log('='.repeat(90));
    console.log('FRACPACK BENCHMARK RESULTS');
    console.log('='.repeat(90));

    // Column widths
    const opW = 24;
    const schemaW = 28;
    const opsW = 14;
    const nsW = 12;
    const bytesW = 8;

    const header = [
      'Operation'.padEnd(opW),
      'Schema'.padEnd(schemaW),
      'ops/sec'.padStart(opsW),
      'median_ns'.padStart(nsW),
      'bytes'.padStart(bytesW),
    ].join('  ');

    console.log(header);
    console.log('-'.repeat(90));

    let lastOp = '';
    for (const row of results) {
      if (row.operation !== lastOp && lastOp !== '') {
        console.log('');
      }
      lastOp = row.operation;

      const line = [
        row.operation.padEnd(opW),
        row.schema.padEnd(schemaW),
        fmtNum(row.opsPerSec).padStart(opsW),
        fmtNum(row.medianNs).padStart(nsW),
        fmtNum(row.bytes).padStart(bytesW),
      ].join('  ');
      console.log(line);
    }

    console.log('='.repeat(90));

    // Summary: fracpack vs JSON size comparison
    const fpUser = results.find(r => r.operation === 'pack' && r.schema === 'UserProfile');
    const jsUser = results.find(r => r.operation === 'JSON.stringify' && r.schema === 'UserProfile');
    if (fpUser && jsUser) {
      const sizeRatio = (jsUser.bytes / fpUser.bytes).toFixed(2);
      console.log(`\nSize: fracpack UserProfile = ${fpUser.bytes}B vs JSON = ${jsUser.bytes}B (${sizeRatio}x larger)`);
    }

    const fpOrder = results.find(r => r.operation === 'pack' && r.schema === 'Order');
    const jsOrder = results.find(r => r.operation === 'JSON.stringify' && r.schema === 'Order');
    if (fpOrder && jsOrder) {
      const sizeRatio = (jsOrder.bytes / fpOrder.bytes).toFixed(2);
      console.log(`Size: fracpack Order = ${fpOrder.bytes}B vs JSON = ${jsOrder.bytes}B (${sizeRatio}x larger)`);
    }

    console.log('');
  });
});

// fracpack.ts — Fracpack binary serialization for JavaScript/TypeScript
//
// Binary format compatible with C++ (psio) and Rust (fracpack) implementations.
// Schema-first design: define schemas, TypeScript infers the types.

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

// ========================= Errors =========================

export class FracpackError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'FracpackError';
  }
}

// ========================= Writer =========================

export class Writer {
  buf: Uint8Array;
  view: DataView;
  pos = 0;

  constructor(initialSize = 256) {
    this.buf = new Uint8Array(initialSize);
    this.view = new DataView(this.buf.buffer);
  }

  private grow(needed: number): void {
    const required = this.pos + needed;
    if (required <= this.buf.length) return;
    const newSize = Math.max(this.buf.length * 2, required);
    const newBuf = new Uint8Array(newSize);
    newBuf.set(this.buf);
    this.buf = newBuf;
    this.view = new DataView(newBuf.buffer);
  }

  writeU8(v: number): void  { this.grow(1); this.view.setUint8(this.pos, v); this.pos += 1; }
  writeU16(v: number): void { this.grow(2); this.view.setUint16(this.pos, v, true); this.pos += 2; }
  writeU32(v: number): void { this.grow(4); this.view.setUint32(this.pos, v, true); this.pos += 4; }
  writeI8(v: number): void  { this.grow(1); this.view.setInt8(this.pos, v); this.pos += 1; }
  writeI16(v: number): void { this.grow(2); this.view.setInt16(this.pos, v, true); this.pos += 2; }
  writeI32(v: number): void { this.grow(4); this.view.setInt32(this.pos, v, true); this.pos += 4; }
  writeU64(v: bigint): void { this.grow(8); this.view.setBigUint64(this.pos, v, true); this.pos += 8; }
  writeI64(v: bigint): void { this.grow(8); this.view.setBigInt64(this.pos, v, true); this.pos += 8; }
  writeF32(v: number): void { this.grow(4); this.view.setFloat32(this.pos, v, true); this.pos += 4; }
  writeF64(v: number): void { this.grow(8); this.view.setFloat64(this.pos, v, true); this.pos += 8; }

  writeBytes(data: Uint8Array): void {
    this.grow(data.length);
    this.buf.set(data, this.pos);
    this.pos += data.length;
  }

  patchU16(offset: number, v: number): void { this.view.setUint16(offset, v, true); }
  patchU32(offset: number, v: number): void { this.view.setUint32(offset, v, true); }

  finish(): Uint8Array { return this.buf.slice(0, this.pos); }
}

// ========================= Reader =========================

export class Reader {
  readonly data: Uint8Array;
  readonly view: DataView;
  pos: number;

  constructor(data: Uint8Array) {
    this.data = data;
    this.view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    this.pos = 0;
  }

  private check(n: number, at = this.pos): void {
    if (at + n > this.data.length) throw new FracpackError('Read past end of buffer');
  }

  readU8(): number   { this.check(1); return this.view.getUint8(this.pos++); }
  readU16(): number  { this.check(2); const v = this.view.getUint16(this.pos, true); this.pos += 2; return v; }
  readU32(): number  { this.check(4); const v = this.view.getUint32(this.pos, true); this.pos += 4; return v; }
  readI8(): number   { this.check(1); return this.view.getInt8(this.pos++); }
  readI16(): number  { this.check(2); const v = this.view.getInt16(this.pos, true); this.pos += 2; return v; }
  readI32(): number  { this.check(4); const v = this.view.getInt32(this.pos, true); this.pos += 4; return v; }
  readU64(): bigint  { this.check(8); const v = this.view.getBigUint64(this.pos, true); this.pos += 8; return v; }
  readI64(): bigint  { this.check(8); const v = this.view.getBigInt64(this.pos, true); this.pos += 8; return v; }
  readF32(): number  { this.check(4); const v = this.view.getFloat32(this.pos, true); this.pos += 4; return v; }
  readF64(): number  { this.check(8); const v = this.view.getFloat64(this.pos, true); this.pos += 8; return v; }

  readU32At(at: number): number { this.check(4, at); return this.view.getUint32(at, true); }

  readBytes(n: number): Uint8Array {
    this.check(n);
    const result = this.data.subarray(this.pos, this.pos + n);
    this.pos += n;
    return result;
  }

  advance(n: number): number {
    const old = this.pos;
    const p = old + n;
    if (p > this.data.length) throw new FracpackError('Read past end of buffer');
    this.pos = p;
    return old;
  }

  setPos(newPos: number): void {
    if (newPos > this.data.length || newPos < this.pos)
      throw new FracpackError(`Bad offset: pos=${newPos}, current=${this.pos}, end=${this.data.length}`);
    this.pos = newPos;
  }

  remaining(): number { return this.data.length - this.pos; }

  finish(): void {
    if (this.pos !== this.data.length)
      throw new FracpackError(`Extra data: ${this.data.length - this.pos} bytes remaining`);
  }
}

// ========================= Cursor =========================

interface Cursor { pos: number }

// ========================= FracType =========================

/** Schema node for type reflection — describes the structure of a FracType. */
export type SchemaNode =
  | { kind: 'bool' }
  | { kind: 'int'; bits: number; signed: boolean }
  | { kind: 'float'; exp: number; mantissa: number }
  | { kind: 'string' }
  | { kind: 'bytes' }
  | { kind: 'optional'; inner: FracType<any> }
  | { kind: 'list'; element: FracType<any> }
  | { kind: 'array'; element: FracType<any>; len: number }
  | { kind: 'tuple'; elements: readonly FracType<any>[] }
  | { kind: 'variant'; cases: Record<string, FracType<any>> }
  | { kind: 'struct'; fields: Record<string, FracType<any>> }
  | { kind: 'fixedStruct'; fields: Record<string, FracType<any>> }
  | { kind: 'map'; key: FracType<any>; value: FracType<any> };

/** A fracpack type descriptor. Provides pack, unpack, and zero-copy view. */
export interface FracType<T> {
  readonly fixedSize: number;
  readonly isVariableSize: boolean;
  readonly isOptional: boolean;
  readonly schema: SchemaNode;
  pack(value: T): Uint8Array;
  unpack(data: Uint8Array): T;
  view(data: Uint8Array): T;
}

/** Result of rawField(): byte range within packed data (variable-size) or decoded value (fixed-size). */
export type RawFieldResult = { offset: number; length: number } | { value: any };

/** Struct type with zero-allocation field access and in-place mutation. */
export interface StructType<T> extends FracType<T> {
  /** Read a single field directly from packed data. Zero allocation beyond the decode itself. */
  readField<K extends keyof T & string>(data: Uint8Array, name: K): T[K];
  /** Update a single field in packed data. Returns new buffer (copies for fixed-size, repacks for variable-size). */
  setField<K extends keyof T & string>(data: Uint8Array, name: K, value: T[K]): Uint8Array;
  /** Navigate to a field without decoding variable-size data. Returns {offset, length} for
   *  variable-size fields (strings, bytes, vecs) or {value} for fixed-size fields.
   *  This measures pure buffer navigation cost without string/array decode overhead. */
  rawField(data: Uint8Array, name: string): RawFieldResult;
}

/** Extract the TypeScript type from a FracType. */
export type Infer<T> = T extends FracType<infer U> ? U : never;

// ========================= Internal type =========================

interface FracTypeImpl<T> extends FracType<T> {
  _packTo(value: T, w: Writer): void;
  _unpackFrom(r: Reader): T;
  _viewAt(data: Uint8Array, dv: DataView, pos: number): T;
  _embFixedPack(value: T, w: Writer): void;
  _embFixedRepack(value: T, fixedPos: number, w: Writer): void;
  _embVarPack(value: T, w: Writer): void;
  _embUnpack(r: Reader, c: Cursor): T;
  _embView(data: Uint8Array, dv: DataView, c: Cursor): T;
  _isEmptyContainer(value: T): boolean;
  _isEmptyOptional(value: T): boolean;
  _newEmptyContainer(): T;
}

function impl<T>(type: FracType<T>): FracTypeImpl<T> {
  return type as FracTypeImpl<T>;
}

// Default embedded variable unpack: read offset, follow pointer, unpack
function embVarUnpack<T>(t: FracTypeImpl<T>, r: Reader, c: Cursor): T {
  const origPos = c.pos;
  const offset = r.readU32At(c.pos);
  c.pos += 4;
  if (offset === 0) return t._newEmptyContainer();
  const target = origPos + offset;
  r.setPos(target);
  return t._unpackFrom(r);
}

// Default embedded variable view: read offset, follow pointer, view
function embVarView<T>(t: FracTypeImpl<T>, data: Uint8Array, dv: DataView, c: Cursor): T {
  const origPos = c.pos;
  const offset = dv.getUint32(origPos, true);
  c.pos += 4;
  if (offset === 0) return t._newEmptyContainer();
  return t._viewAt(data, dv, origPos + offset);
}

// ========================= Type factory =========================

function makeType<T>(config: {
  fixedSize: number;
  isVariableSize: boolean;
  isOptional?: boolean;
  schema: SchemaNode;
  packTo: (value: T, w: Writer) => void;
  unpackFrom: (r: Reader) => T;
  viewAt: (data: Uint8Array, dv: DataView, pos: number) => T;
  isEmptyContainer?: (value: T) => boolean;
  newEmptyContainer?: () => T;
  // overrides
  embFixedPack?: (value: T, w: Writer) => void;
  embFixedRepack?: (value: T, fixedPos: number, w: Writer) => void;
  embVarPack?: (value: T, w: Writer) => void;
  embUnpack?: (r: Reader, c: Cursor) => T;
  embView?: (data: Uint8Array, dv: DataView, c: Cursor) => T;
}): FracType<T> {
  const isEmpty = config.isEmptyContainer ?? (() => false);
  const newEmpty = config.newEmptyContainer ?? (() => { throw new FracpackError('Not a container'); });
  const isVar = config.isVariableSize;

  const type: FracTypeImpl<T> = {
    fixedSize: config.fixedSize,
    isVariableSize: isVar,
    isOptional: config.isOptional ?? false,
    schema: config.schema,

    pack(value: T): Uint8Array {
      const w = new Writer();
      config.packTo(value, w);
      return w.finish();
    },
    unpack(data: Uint8Array): T {
      const r = new Reader(data);
      const result = config.unpackFrom(r);
      r.finish();
      return result;
    },
    view(data: Uint8Array): T {
      return config.viewAt(data, new DataView(data.buffer, data.byteOffset, data.byteLength), 0);
    },

    _packTo: config.packTo,
    _unpackFrom: config.unpackFrom,
    _viewAt: config.viewAt,
    _isEmptyContainer: isEmpty,
    _isEmptyOptional: config.isOptional ? ((v: T) => v === null || v === undefined) : (() => false),
    _newEmptyContainer: newEmpty,

    _embFixedPack: config.embFixedPack ?? ((value: T, w: Writer) => {
      if (isVar) w.writeU32(0);
      else config.packTo(value, w);
    }),

    _embFixedRepack: config.embFixedRepack ?? ((value: T, fixedPos: number, w: Writer) => {
      if (isVar && !isEmpty(value)) w.patchU32(fixedPos, w.pos - fixedPos);
    }),

    _embVarPack: config.embVarPack ?? ((value: T, w: Writer) => {
      if (isVar && !isEmpty(value)) config.packTo(value, w);
    }),

    _embUnpack: config.embUnpack ?? ((r: Reader, c: Cursor) => {
      if (isVar) {
        return embVarUnpack(type, r, c);
      } else {
        const saved = r.pos;
        r.pos = c.pos;
        const value = config.unpackFrom(r);
        c.pos = r.pos;
        r.pos = saved;
        return value;
      }
    }),

    _embView: config.embView ?? ((data: Uint8Array, dv: DataView, c: Cursor) => {
      if (isVar) {
        return embVarView(type, data, dv, c);
      } else {
        const value = config.viewAt(data, dv, c.pos);
        c.pos += config.fixedSize;
        return value;
      }
    }),
  };
  return type;
}

// ========================= Scalar types =========================

function scalarType(
  size: number,
  write: (v: number, w: Writer) => void,
  read: (r: Reader) => number,
  viewAt: (dv: DataView, pos: number) => number,
  schema: SchemaNode,
): FracType<number> {
  return makeType<number>({
    fixedSize: size,
    isVariableSize: false,
    schema,
    packTo: (v, w) => write(v, w),
    unpackFrom: (r) => read(r),
    viewAt: (_d, dv, pos) => viewAt(dv, pos),
  });
}

export const bool: FracType<boolean> = makeType<boolean>({
  fixedSize: 1,
  isVariableSize: false,
  schema: { kind: 'bool' },
  packTo: (v, w) => w.writeU8(v ? 1 : 0),
  unpackFrom: (r) => {
    const b = r.readU8();
    if (b > 1) throw new FracpackError(`Invalid bool: ${b}`);
    return b === 1;
  },
  viewAt: (_d, dv, pos) => {
    const b = dv.getUint8(pos);
    if (b > 1) throw new FracpackError(`Invalid bool: ${b}`);
    return b === 1;
  },
});

export const u8  = scalarType(1, (v, w) => w.writeU8(v),  r => r.readU8(),  (dv, p) => dv.getUint8(p),  { kind: 'int', bits: 8,  signed: false });
export const u16 = scalarType(2, (v, w) => w.writeU16(v), r => r.readU16(), (dv, p) => dv.getUint16(p, true), { kind: 'int', bits: 16, signed: false });
export const u32 = scalarType(4, (v, w) => w.writeU32(v), r => r.readU32(), (dv, p) => dv.getUint32(p, true), { kind: 'int', bits: 32, signed: false });
export const i8  = scalarType(1, (v, w) => w.writeI8(v),  r => r.readI8(),  (dv, p) => dv.getInt8(p),  { kind: 'int', bits: 8,  signed: true });
export const i16 = scalarType(2, (v, w) => w.writeI16(v), r => r.readI16(), (dv, p) => dv.getInt16(p, true), { kind: 'int', bits: 16, signed: true });
export const i32 = scalarType(4, (v, w) => w.writeI32(v), r => r.readI32(), (dv, p) => dv.getInt32(p, true), { kind: 'int', bits: 32, signed: true });
export const f32 = scalarType(4, (v, w) => w.writeF32(v), r => r.readF32(), (dv, p) => dv.getFloat32(p, true), { kind: 'float', exp: 8,  mantissa: 23 });
export const f64 = scalarType(8, (v, w) => w.writeF64(v), r => r.readF64(), (dv, p) => dv.getFloat64(p, true), { kind: 'float', exp: 11, mantissa: 52 });

export const u64: FracType<bigint> = makeType<bigint>({
  fixedSize: 8,
  isVariableSize: false,
  schema: { kind: 'int', bits: 64, signed: false },
  packTo: (v, w) => w.writeU64(v),
  unpackFrom: (r) => r.readU64(),
  viewAt: (_d, dv, pos) => dv.getBigUint64(pos, true),
});

export const i64: FracType<bigint> = makeType<bigint>({
  fixedSize: 8,
  isVariableSize: false,
  schema: { kind: 'int', bits: 64, signed: true },
  packTo: (v, w) => w.writeI64(v),
  unpackFrom: (r) => r.readI64(),
  viewAt: (_d, dv, pos) => dv.getBigInt64(pos, true),
});

// ========================= String =========================

/** Decode a string from packed bytes. Fast ASCII path avoids TextDecoder overhead for short strings. */
function decodeString(data: Uint8Array, start: number, len: number): string {
  if (len === 0) return '';
  // Fast path: short ASCII strings bypass TextDecoder's C++ binding overhead.
  // Above ~32 bytes, TextDecoder's native SIMD decode is faster.
  if (len <= 32) {
    let nonAscii = 0;
    for (let i = 0; i < len; i++) nonAscii |= data[start + i];
    if (!(nonAscii & 0x80)) {
      return String.fromCharCode.apply(null, data.subarray(start, start + len) as any);
    }
  }
  return textDecoder.decode(data.subarray(start, start + len));
}

export const str: FracType<string> = makeType<string>({
  fixedSize: 4,
  isVariableSize: true,
  schema: { kind: 'string' },
  isEmptyContainer: (v) => v.length === 0,
  newEmptyContainer: () => '',
  packTo: (v, w) => {
    const encoded = textEncoder.encode(v);
    w.writeU32(encoded.length);
    w.writeBytes(encoded);
  },
  unpackFrom: (r) => {
    const len = r.readU32();
    const start = r.pos;
    r.pos += len;
    return decodeString(r.data, start, len);
  },
  viewAt: (data, _dv, pos) => {
    const len = (data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24)) >>> 0;
    return decodeString(data, pos + 4, len);
  },
});

// ========================= Bytes =========================

export const bytes: FracType<Uint8Array> = makeType<Uint8Array>({
  fixedSize: 4,
  isVariableSize: true,
  schema: { kind: 'bytes' },
  isEmptyContainer: (v) => v.length === 0,
  newEmptyContainer: () => new Uint8Array(0),
  packTo: (v, w) => {
    w.writeU32(v.length);
    w.writeBytes(v);
  },
  unpackFrom: (r) => {
    const len = r.readU32();
    return new Uint8Array(r.readBytes(len)); // copy for safety
  },
  viewAt: (data, dv, pos) => {
    const len = dv.getUint32(pos, true);
    return data.subarray(pos + 4, pos + 4 + len); // zero-copy view
  },
});

// ========================= Optional =========================

export function optional<T>(inner: FracType<T>): FracType<T | null> {
  const t = impl(inner);

  return makeType<T | null>({
    fixedSize: 4,
    isVariableSize: true,
    isOptional: true,
    schema: { kind: 'optional', inner },

    packTo: (v, w) => {
      if (v === null || v === undefined) {
        w.writeU32(1);
        return;
      }
      const fixedPos = w.pos;
      if (t.isOptional || !t.isVariableSize) {
        w.writeU32(1);
      } else {
        t._embFixedPack(v, w);
      }
      const heapPos = w.pos;
      if (t.isOptional || !t.isVariableSize) {
        w.patchU32(fixedPos, heapPos - fixedPos);
        t._packTo(v, w);
      } else {
        t._embFixedRepack(v, fixedPos, w);
        t._embVarPack(v, w);
      }
    },

    unpackFrom: (r) => {
      const c: Cursor = { pos: r.advance(4) };
      return optUnpack(t, r, c);
    },

    viewAt: (data, dv, pos) => {
      // Top-level optional: 4-byte offset at pos
      const offset = dv.getUint32(pos, true);
      if (offset === 1) return null;
      if (offset === 0) return t._newEmptyContainer();
      return t._viewAt(data, dv, pos + offset);
    },

    embFixedPack: (v, w) => {
      if (t.isOptional || !t.isVariableSize) {
        w.writeU32(1);
      } else {
        if (v === null || v === undefined) w.writeU32(1);
        else t._embFixedPack(v, w);
      }
    },

    embFixedRepack: (v, fixedPos, w) => {
      if (v === null || v === undefined) return;
      if (t.isOptional || !t.isVariableSize) {
        w.patchU32(fixedPos, w.pos - fixedPos);
      } else {
        t._embFixedRepack(v, fixedPos, w);
      }
    },

    embVarPack: (v, w) => {
      if (v === null || v === undefined) return;
      if (!t._isEmptyContainer(v)) t._packTo(v, w);
    },

    embUnpack: (r, c) => optUnpack(t, r, c),

    embView: (data, dv, c) => {
      const origPos = c.pos;
      const offset = dv.getUint32(origPos, true);
      if (offset === 1) { c.pos += 4; return null; }
      // Delegate to inner type's variable view
      return embVarView(t, data, dv, c) as T | null;
    },
  });
}

function optUnpack<T>(t: FracTypeImpl<T>, r: Reader, c: Cursor): T | null {
  const offset = r.readU32At(c.pos);
  if (offset === 1) { c.pos += 4; return null; }
  return embVarUnpack(t, r, c);
}

// ========================= Vec =========================

/** Map scalar schema to TypedArray constructor for zero-copy vec views. */
type AnyTypedArrayCtor = { new(buffer: ArrayBuffer, byteOffset: number, length: number): ArrayLike<any>; BYTES_PER_ELEMENT: number };

function getTypedArrayCtor(schema: SchemaNode): AnyTypedArrayCtor | null {
  if (schema.kind === 'int') {
    if (!schema.signed) {
      switch (schema.bits) {
        case 8: return Uint8Array;
        case 16: return Uint16Array;
        case 32: return Uint32Array;
        case 64: return BigUint64Array;
      }
    } else {
      switch (schema.bits) {
        case 8: return Int8Array;
        case 16: return Int16Array;
        case 32: return Int32Array;
        case 64: return BigInt64Array;
      }
    }
  } else if (schema.kind === 'float') {
    return schema.exp === 8 ? Float32Array : Float64Array;
  }
  return null;
}

export function vec<T>(inner: FracType<T>): FracType<T[]> {
  const t = impl(inner);

  // Check if element type supports TypedArray zero-copy view
  const TypedCtor = getTypedArrayCtor(inner.schema);
  const elemAlign = TypedCtor ? TypedCtor.BYTES_PER_ELEMENT : 0;

  return makeType<T[]>({
    fixedSize: 4,
    isVariableSize: true,
    schema: { kind: 'list', element: inner },
    isEmptyContainer: (v) => v.length === 0,
    newEmptyContainer: () => [],

    packTo: (v, w) => {
      const numBytes = v.length * t.fixedSize;
      w.writeU32(numBytes);
      const start = w.pos;
      if (t.isVariableSize) {
        for (let i = 0; i < v.length; i++) w.writeU32(0);
        for (let i = 0; i < v.length; i++) {
          const slot = start + i * t.fixedSize;
          w.patchU32(slot, w.pos - slot);
          t._packTo(v[i], w);
        }
      } else {
        for (const value of v) t._packTo(value, w);
      }
    },

    unpackFrom: (r) => {
      const numBytes = r.readU32();
      if (numBytes % t.fixedSize !== 0) throw new FracpackError('Bad vec size');
      const len = numBytes / t.fixedSize;
      const c: Cursor = { pos: r.advance(numBytes) };
      const result: T[] = new Array(len);
      for (let i = 0; i < len; i++) {
        if (t.isVariableSize) {
          const target = c.pos + r.readU32At(c.pos); c.pos += 4;
          r.setPos(target); result[i] = t._unpackFrom(r);
        } else result[i] = t._embUnpack(r, c);
      }
      return result;
    },

    viewAt: (data, dv, pos) => {
      const numBytes = dv.getUint32(pos, true);
      if (numBytes % t.fixedSize !== 0) throw new FracpackError('Bad vec size');
      const len = numBytes / t.fixedSize;
      if (len === 0) return [];
      const fixedStart = pos + 4;

      // Fast path: TypedArray zero-copy view for scalar vecs
      if (TypedCtor) {
        const absOffset = data.byteOffset + fixedStart;
        if (absOffset % elemAlign === 0) {
          return new TypedCtor(data.buffer as ArrayBuffer, absOffset, len) as unknown as T[];
        }
        // Unaligned: copy bytes to aligned buffer (still faster than per-element decode)
        const alignedBuf = new ArrayBuffer(numBytes);
        new Uint8Array(alignedBuf).set(data.subarray(fixedStart, fixedStart + numBytes));
        return new TypedCtor(alignedBuf, 0, len) as unknown as T[];
      }

      // Lazy Proxy for composite element types
      const elemSize = t.fixedSize;
      return new Proxy(new Array(len), {
        get(tgt, prop, receiver) {
          if (typeof prop === 'string') {
            const idx = (prop as any) >>> 0;
            if (String(idx) === prop && idx < len && tgt[idx] === undefined) {
              const c: Cursor = { pos: fixedStart + idx * elemSize };
              const offset = t.isVariableSize ? dv.getUint32(c.pos, true) : 0;
              tgt[idx] = t.isVariableSize
                ? t._viewAt(data, dv, c.pos + offset) : t._embView(data, dv, c);
            }
          }
          return Reflect.get(tgt, prop, receiver);
        },
      }) as T[];
    },
  });
}

// ========================= Array (fixed length) =========================

export function array<T>(inner: FracType<T>, len: number): FracType<T[]> {
  const t = impl(inner);
  const isVar = t.isVariableSize;

  return makeType<T[]>({
    fixedSize: isVar ? 4 : t.fixedSize * len,
    isVariableSize: isVar,
    schema: { kind: 'array', element: inner, len },

    packTo: (v, w) => {
      if (v.length !== len) throw new FracpackError(`Expected array[${len}], got ${v.length}`);
      const start = w.pos;
      for (let i = 0; i < len; i++) t._embFixedPack(v[i], w);
      for (let i = 0; i < len; i++) {
        t._embFixedRepack(v[i], start + i * t.fixedSize, w);
        t._embVarPack(v[i], w);
      }
    },

    unpackFrom: (r) => {
      const totalFixed = t.fixedSize * len;
      const c: Cursor = { pos: r.advance(totalFixed) };
      const result: T[] = new Array(len);
      for (let i = 0; i < len; i++) result[i] = t._embUnpack(r, c);
      return result;
    },

    viewAt: (data, dv, pos) => {
      const result: T[] = new Array(len);
      const c: Cursor = { pos };
      for (let i = 0; i < len; i++) result[i] = t._embView(data, dv, c);
      return result;
    },
  });
}

// ========================= Tuple =========================

type InferTuple<T extends readonly FracType<any>[]> = {
  [K in keyof T]: T[K] extends FracType<infer U> ? U : never;
};

export function tuple<T extends readonly FracType<any>[]>(
  ...types: T
): FracType<InferTuple<T>> {
  const impls = types.map(impl);

  // Pre-compute trailing optional index (last non-optional + 1)
  let trailingOptIdx = 0;
  for (let i = impls.length - 1; i >= 0; i--) {
    if (!impls[i].isOptional) { trailingOptIdx = i + 1; break; }
  }

  return makeType<InferTuple<T>>({
    fixedSize: 4,
    isVariableSize: true,
    schema: { kind: 'tuple', elements: types },

    packTo: (values, w) => {
      const arr = values as any[];

      // Find last non-empty-optional field
      let trailIdx = 0;
      for (let i = impls.length - 1; i >= 0; i--) {
        if (!impls[i]._isEmptyOptional(arr[i])) { trailIdx = i + 1; break; }
      }

      // Calculate fixed section size
      let fixedSize = 0;
      for (let i = 0; i < trailIdx; i++) fixedSize += impls[i].fixedSize;

      // Write u16 fixed_section_size
      w.writeU16(fixedSize);

      // Write fixed parts
      const positions: number[] = [];
      for (let i = 0; i < trailIdx; i++) {
        positions.push(w.pos);
        impls[i]._embFixedPack(arr[i], w);
      }

      // Repack offsets and write variable parts
      for (let i = 0; i < trailIdx; i++) {
        impls[i]._embFixedRepack(arr[i], positions[i], w);
        impls[i]._embVarPack(arr[i], w);
      }
    },

    unpackFrom: (r) => {
      const fixedSize = r.readU16();
      const c: Cursor = { pos: r.advance(fixedSize) };
      const heapPos = r.pos;

      const result: any[] = new Array(impls.length);
      for (let i = 0; i < impls.length; i++) {
        if (i < trailingOptIdx || c.pos < heapPos) {
          result[i] = impls[i]._embUnpack(r, c);
        } else {
          result[i] = null;
        }
      }

      consumeTrailing(r, c, heapPos);
      return result as InferTuple<T>;
    },

    viewAt: (data, dv, pos) => {
      const fixedSize = dv.getUint16(pos, true);
      const fixedStart = pos + 2;
      const heapPos = fixedStart + fixedSize;

      const result: any[] = new Array(impls.length);
      const c: Cursor = { pos: fixedStart };
      for (let i = 0; i < impls.length; i++) {
        if (i < trailingOptIdx || c.pos < heapPos) {
          result[i] = impls[i]._embView(data, dv, c);
        } else {
          result[i] = null;
        }
      }
      return result as InferTuple<T>;
    },
  });
}

// ========================= Variant =========================

type InferVariant<V extends Record<string, FracType<any>>> = {
  [K in keyof V]: { type: K; value: V[K] extends FracType<infer T> ? T : never };
}[keyof V];

export function variant<V extends Record<string, FracType<any>>>(
  variants: V,
): FracType<InferVariant<V>> {
  const names = Object.keys(variants);
  const impls = names.map(n => impl(variants[n]));

  return makeType<InferVariant<V>>({
    fixedSize: 4,
    isVariableSize: true,
    schema: { kind: 'variant', cases: variants },

    packTo: (v: any, w) => {
      const idx = names.indexOf(v.type);
      if (idx === -1) throw new FracpackError(`Unknown variant: ${String(v.type)}`);
      w.writeU8(idx);
      const sizePos = w.pos;
      w.writeU32(0);
      impls[idx]._packTo(v.value, w);
      w.patchU32(sizePos, w.pos - sizePos - 4);
    },

    unpackFrom: (r) => {
      const idx = r.readU8();
      if (idx >= names.length) throw new FracpackError(`Bad variant index: ${idx}`);
      const size = r.readU32();
      const endPos = r.pos + size;
      const value = impls[idx]._unpackFrom(r);
      r.pos = endPos; // skip any extra data (forward compat)
      return { type: names[idx], value } as InferVariant<V>;
    },

    viewAt: (data, dv, pos) => {
      const idx = dv.getUint8(pos);
      if (idx >= names.length) throw new FracpackError(`Bad variant index: ${idx}`);
      // data starts at pos + 1 (index) + 4 (size) = pos + 5
      const value = impls[idx]._viewAt(data, dv, pos + 5);
      return { type: names[idx], value } as InferVariant<V>;
    },
  });
}

// ========================= Struct (extensible) =========================

// Symbols for per-view metadata stored on Proxy target (avoids per-view closures)
const SD = Symbol(); // data: Uint8Array
const SV = Symbol(); // dv: DataView
const SS = Symbol(); // fixedStart: number
const SH = Symbol(); // heapPos: number

type InferFields<F extends Record<string, FracType<any>>> = {
  [K in keyof F]: F[K] extends FracType<infer T> ? T : never;
};

export function struct<F extends Record<string, FracType<any>>>(
  fields: F,
): StructType<InferFields<F>> {
  const names = Object.keys(fields);
  const impls = names.map(n => impl(fields[n]));

  // Pre-compute trailing optional index for unpack
  let trailingOptIdx = 0;
  for (let i = impls.length - 1; i >= 0; i--) {
    if (!impls[i].isOptional) { trailingOptIdx = i + 1; break; }
  }

  // Pre-compute field offsets within the fixed section (for view)
  const fieldOffsets: number[] = [];
  let off = 0;
  for (const t of impls) { fieldOffsets.push(off); off += t.fixedSize; }
  const totalFixedSize = off;

  // Pre-compute field name → index map for O(1) Proxy lookup
  const fieldLookup = new Map<string, number>();
  for (let i = 0; i < names.length; i++) fieldLookup.set(names[i], i);

  // For optional fields the struct handler resolves the offset itself, so we
  // need the *inner* type's viewAt (not the optional wrapper which would double-
  // resolve). For non-optional fields we use the field impl directly.
  const viewImpls: FracTypeImpl<any>[] = impls.map((fi, i) => {
    if (fi.isOptional) {
      const schema = fields[names[i]].schema;
      if (schema.kind === 'optional') return impl(schema.inner);
    }
    return fi;
  });

  // Shared Proxy handler — created once at schema-definition time
  const viewHandler: ProxyHandler<any> = {
    get(target, prop) {
      if (typeof prop !== 'string') return undefined;
      if (prop in target) return target[prop];
      const idx = fieldLookup.get(prop);
      if (idx === undefined) return undefined;
      const fi = impls[idx];
      const fp = target[SS] + fieldOffsets[idx];
      let val: any;
      if (fp + fi.fixedSize > target[SH]) {
        val = null;
      } else if (fi.isVariableSize) {
        const off = target[SV].getUint32(fp, true);
        if (off === 0) val = fi._newEmptyContainer();
        else if (off === 1 && fi.isOptional) val = null;
        else val = viewImpls[idx]._viewAt(target[SD], target[SV], fp + off);
      } else {
        val = fi._viewAt(target[SD], target[SV], fp);
      }
      target[prop] = val;
      return val;
    },
    set(target, prop, value) {
      target[prop as string] = value;
      return true;
    },
    has(_target, prop) {
      return typeof prop === 'string' && fieldLookup.has(prop);
    },
    ownKeys() { return names; },
    getOwnPropertyDescriptor(target, prop) {
      if (typeof prop !== 'string') return undefined;
      const idx = fieldLookup.get(prop);
      if (idx === undefined) return undefined;
      if (!(prop in target)) this.get!(target, prop, undefined);
      return { value: target[prop], writable: true, enumerable: true, configurable: true };
    },
  };

  const type = makeType<InferFields<F>>({
    fixedSize: 4,
    isVariableSize: true,
    schema: { kind: 'struct', fields },

    packTo: (value: any, w) => {
      // Find trailing empty index
      let trailIdx = 0;
      for (let i = impls.length - 1; i >= 0; i--) {
        if (!impls[i]._isEmptyOptional(value[names[i]])) { trailIdx = i + 1; break; }
      }

      // Fixed section size = sum of FIXED_SIZE for included fields
      let fixedSize = 0;
      for (let i = 0; i < trailIdx; i++) fixedSize += impls[i].fixedSize;

      // Write u16 content size header
      w.writeU16(fixedSize);

      // Phase 1: write fixed parts
      const positions: number[] = [];
      for (let i = 0; i < trailIdx; i++) {
        positions.push(w.pos);
        impls[i]._embFixedPack(value[names[i]], w);
      }

      // Phase 2: repack offsets + write variable data
      for (let i = 0; i < trailIdx; i++) {
        impls[i]._embFixedRepack(value[names[i]], positions[i], w);
        impls[i]._embVarPack(value[names[i]], w);
      }
    },

    unpackFrom: (r) => {
      const fixedSize = r.readU16();
      const c: Cursor = { pos: r.advance(fixedSize) };
      const heapPos = r.pos;

      const result: any = {};
      for (let i = 0; i < impls.length; i++) {
        if (i < trailingOptIdx || c.pos < heapPos) {
          result[names[i]] = impls[i]._embUnpack(r, c);
        } else {
          result[names[i]] = null;
        }
      }

      consumeTrailing(r, c, heapPos);
      return result as InferFields<F>;
    },

    viewAt: (data, dv, pos) => {
      const fixedSize = dv.getUint16(pos, true);
      const target = Object.create(null);
      target[SD] = data;
      target[SV] = dv;
      target[SS] = pos + 2;          // fixedStart
      target[SH] = pos + 2 + fixedSize; // heapPos
      return new Proxy(target, viewHandler) as InferFields<F>;
    },
  });

  // Zero-allocation direct field access
  const stype = type as StructType<InferFields<F>>;
  (stype as any).readField = (data: Uint8Array, name: string): any => {
    const idx = fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`Unknown field: ${name}`);
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fs = dv.getUint16(0, true);
    const fixedStart = 2;
    const heapPos = fixedStart + fs;
    const fi = impls[idx];
    const fp = fixedStart + fieldOffsets[idx];
    if (fp + fi.fixedSize > heapPos) return null;
    if (fi.isVariableSize) {
      const off = dv.getUint32(fp, true);
      if (off === 0) return fi._newEmptyContainer();
      if (off === 1 && fi.isOptional) return null;
      return viewImpls[idx]._viewAt(data, dv, fp + off);
    }
    return fi._viewAt(data, dv, fp);
  };

  // Zero-copy field navigation: returns {offset, length} for variable-size fields,
  // {value} for fixed-size fields. Never decodes strings or allocates arrays.
  (stype as any).rawField = (data: Uint8Array, name: string): RawFieldResult => {
    const idx = fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`Unknown field: ${name}`);
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fs = dv.getUint16(0, true);
    const fixedStart = 2;
    const heapPos = fixedStart + fs;
    const fi = impls[idx];
    const fp = fixedStart + fieldOffsets[idx];
    if (fp + fi.fixedSize > heapPos) return { value: null };
    if (fi.isVariableSize) {
      const off = dv.getUint32(fp, true);
      if (off === 0) return { offset: fp, length: 0 };
      if (off === 1 && fi.isOptional) return { value: null };
      // Follow the pointer to the data; for str/bytes the first 4 bytes are the length prefix
      const dataPos = fp + off;
      const dataLen = dv.getUint32(dataPos, true);
      return { offset: dataPos + 4, length: dataLen };
    }
    return { value: fi._viewAt(data, dv, fp) };
  };

  // In-place field mutation
  (stype as any).setField = (data: Uint8Array, name: string, value: any): Uint8Array => {
    const idx = fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`Unknown field: ${name}`);
    const fi = impls[idx];
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fs = dv.getUint16(0, true);
    const fixedStart = 2;
    const fp = fixedStart + fieldOffsets[idx];

    if (!fi.isVariableSize && fp + fi.fixedSize <= fixedStart + fs) {
      // Fixed-size field within bounds: copy buffer, overwrite bytes
      const result = new Uint8Array(data.length);
      result.set(data);
      const w = new Writer(fi.fixedSize);
      fi._packTo(value, w);
      result.set(w.finish(), fp);
      return result;
    }
    // Variable-size or out-of-bounds: full repack
    const obj = stype.unpack(data);
    (obj as any)[name] = value;
    return stype.pack(obj);
  };

  return stype;
}

// ========================= Fixed struct (definitionWillNotChange) =========================

// Symbol for fixedStruct base position (reuses SD, SV from struct)
const SP = Symbol(); // pos: number

export function fixedStruct<F extends Record<string, FracType<any>>>(
  fields: F,
): StructType<InferFields<F>> {
  const names = Object.keys(fields);
  const impls = names.map(n => impl(fields[n]));

  const hasVarField = impls.some(t => t.isVariableSize);
  const fixedFieldsSize = impls.reduce((s, t) => s + t.fixedSize, 0);

  // Pre-compute field offsets
  const fieldOffsets: number[] = [];
  let off = 0;
  for (const t of impls) { fieldOffsets.push(off); off += t.fixedSize; }

  // Pre-compute field name → index map for O(1) Proxy lookup
  const fieldLookup = new Map<string, number>();
  for (let i = 0; i < names.length; i++) fieldLookup.set(names[i], i);

  // Shared Proxy handler — created once at schema-definition time
  const viewHandler: ProxyHandler<any> = {
    get(target, prop) {
      if (typeof prop !== 'string') return undefined;
      if (prop in target) return target[prop];
      const idx = fieldLookup.get(prop);
      if (idx === undefined) return undefined;
      const fi = impls[idx];
      const fp = target[SP] + fieldOffsets[idx];
      let val: any;
      if (fi.isVariableSize) {
        const off = target[SV].getUint32(fp, true);
        if (off === 0) val = fi._newEmptyContainer();
        else val = fi._viewAt(target[SD], target[SV], fp + off);
      } else {
        val = fi._viewAt(target[SD], target[SV], fp);
      }
      target[prop] = val;
      return val;
    },
    set(target, prop, value) {
      target[prop as string] = value;
      return true;
    },
    has(_target, prop) {
      return typeof prop === 'string' && fieldLookup.has(prop);
    },
    ownKeys() { return names; },
    getOwnPropertyDescriptor(target, prop) {
      if (typeof prop !== 'string') return undefined;
      const idx = fieldLookup.get(prop);
      if (idx === undefined) return undefined;
      if (!(prop in target)) this.get!(target, prop, undefined);
      return { value: target[prop], writable: true, enumerable: true, configurable: true };
    },
  };

  const type = makeType<InferFields<F>>({
    fixedSize: hasVarField ? 4 : fixedFieldsSize,
    isVariableSize: hasVarField,
    schema: { kind: 'fixedStruct', fields },

    packTo: (value: any, w) => {
      const positions: number[] = [];
      for (let i = 0; i < impls.length; i++) {
        positions.push(w.pos);
        impls[i]._embFixedPack(value[names[i]], w);
      }
      for (let i = 0; i < impls.length; i++) {
        impls[i]._embFixedRepack(value[names[i]], positions[i], w);
        impls[i]._embVarPack(value[names[i]], w);
      }
    },

    unpackFrom: (r) => {
      const c: Cursor = { pos: r.advance(fixedFieldsSize) };
      const result: any = {};
      for (let i = 0; i < impls.length; i++) {
        result[names[i]] = impls[i]._embUnpack(r, c);
      }
      return result as InferFields<F>;
    },

    viewAt: (data, dv, pos) => {
      const target = Object.create(null);
      target[SD] = data;
      target[SV] = dv;
      target[SP] = pos;
      return new Proxy(target, viewHandler) as InferFields<F>;
    },
  });

  // Zero-allocation direct field access
  const stype = type as StructType<InferFields<F>>;
  (stype as any).readField = (data: Uint8Array, name: string): any => {
    const idx = fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`Unknown field: ${name}`);
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fi = impls[idx];
    const fp = fieldOffsets[idx];
    if (fi.isVariableSize) {
      const off = dv.getUint32(fp, true);
      if (off === 0) return fi._newEmptyContainer();
      return fi._viewAt(data, dv, fp + off);
    }
    return fi._viewAt(data, dv, fp);
  };

  // Zero-copy field navigation (fixedStruct variant)
  (stype as any).rawField = (data: Uint8Array, name: string): RawFieldResult => {
    const idx = fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`Unknown field: ${name}`);
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const fi = impls[idx];
    const fp = fieldOffsets[idx];
    if (fi.isVariableSize) {
      const off = dv.getUint32(fp, true);
      if (off === 0) return { offset: fp, length: 0 };
      const dataPos = fp + off;
      const dataLen = dv.getUint32(dataPos, true);
      return { offset: dataPos + 4, length: dataLen };
    }
    return { value: fi._viewAt(data, dv, fp) };
  };

  // In-place field mutation
  (stype as any).setField = (data: Uint8Array, name: string, value: any): Uint8Array => {
    const idx = fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`Unknown field: ${name}`);
    const fi = impls[idx];

    if (!fi.isVariableSize) {
      const result = new Uint8Array(data.length);
      result.set(data);
      const w = new Writer(fi.fixedSize);
      fi._packTo(value, w);
      result.set(w.finish(), fieldOffsets[idx]);
      return result;
    }
    const obj = stype.unpack(data);
    (obj as any)[name] = value;
    return stype.pack(obj);
  };

  return stype;
}

// ========================= Map (string-keyed) =========================

export function map<V>(valueType: FracType<V>): FracType<Record<string, V>> {
  const entryType = struct({ key: str, value: valueType });
  const listType = vec(entryType);
  const listImpl = impl(listType);

  return makeType<Record<string, V>>({
    fixedSize: 4,
    isVariableSize: true,
    schema: { kind: 'map', key: str, value: valueType },
    isEmptyContainer: (v) => Object.keys(v).length === 0,
    newEmptyContainer: () => ({} as Record<string, V>),

    packTo: (v, w) => {
      const entries = Object.entries(v).map(([k, val]) => ({ key: k, value: val }));
      listImpl._packTo(entries as any, w);
    },

    unpackFrom: (r) => {
      const entries = listImpl._unpackFrom(r) as any[];
      const result = {} as Record<string, V>;
      for (const e of entries) result[e.key] = e.value;
      return result;
    },

    viewAt: (data, dv, pos) => {
      const entries = listImpl._viewAt(data, dv, pos) as any[];
      const result = {} as Record<string, V>;
      for (let i = 0; i < entries.length; i++) {
        const e = entries[i];
        result[e.key] = e.value;
      }
      return result;
    },
  });
}

// ========================= Trailing optional consumer =========================

function consumeTrailing(r: Reader, c: Cursor, heapPos: number): void {
  while (c.pos < heapPos) {
    const origPos = c.pos;
    const offset = r.readU32At(c.pos);
    c.pos += 4;
    if (offset > 1) {
      const target = origPos + offset;
      if (target > r.data.length || target < r.pos)
        throw new FracpackError('Bad offset in unknown trailing field');
      r.pos = target;
    }
  }
}

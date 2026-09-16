// mutation.ts — In-place mutation system for fracpack packed data.
//
// Provides buffer primitives (spliceBuffer, patchOffset) and a MutView class
// that wraps a mutable buffer and supports reading/writing individual fields
// without a full repack.
//
// Two modes:
//   Canonical (default): splices the buffer in place, shifting bytes and
//     patching self-relative offsets. The result is always valid canonical fracpack.
//   Fast (fast=true): overwrites in place when data fits, appends to end when
//     larger, sets offset markers without reclaiming space. Call compact() to
//     produce canonical output.

import {
  type FracType,
  type StructType,
  type SchemaNode,
  Writer,
  Reader,
  FracpackError,
} from './fracpack.js';

const textEncoder = new TextEncoder();

// ========================= Internal type access =========================

// FracTypeImpl mirrors the internal interface from fracpack.ts
interface FracTypeImpl<T> extends FracType<T> {
  _packTo(value: T, w: Writer): void;
  _unpackFrom(r: Reader): T;
  _viewAt(data: Uint8Array, dv: DataView, pos: number): T;
  _isEmptyContainer(value: T): boolean;
  _isEmptyOptional(value: T): boolean;
  _newEmptyContainer(): T;
}

function asImpl<T>(type: FracType<T>): FracTypeImpl<T> {
  return type as FracTypeImpl<T>;
}

// ========================= Buffer primitives =========================

/**
 * Replace `data[pos..pos+oldLen]` with `newData`, shifting the tail.
 * Returns the delta (positive = grew, negative = shrank, 0 = same size).
 *
 * The input Uint8Array is replaced in the MutView; callers must use the
 * returned value from the MutView's internal buffer reference.
 */
export function spliceBuffer(
  data: Uint8Array,
  pos: number,
  oldLen: number,
  newData: Uint8Array,
): { result: Uint8Array; delta: number } {
  const newLen = newData.length;
  const delta = newLen - oldLen;

  if (delta === 0) {
    // Same size: overwrite in place
    data.set(newData, pos);
    return { result: data, delta: 0 };
  }

  // Different size: build a new buffer
  const out = new Uint8Array(data.length + delta);
  // Copy prefix
  out.set(data.subarray(0, pos), 0);
  // Copy new data
  out.set(newData, pos);
  // Copy tail
  out.set(data.subarray(pos + oldLen), pos + newLen);

  return { result: out, delta };
}

/**
 * Adjust a self-relative u32 offset at `offsetPos` after a splice.
 *
 * Only adjusts if the offset is a valid pointer (>= 4) and the target
 * it points to is at or past `splicePos`.
 *
 * Offset values: 0 = empty container, 1 = None marker, 2-3 = reserved, >= 4 = valid.
 */
export function patchOffset(
  data: Uint8Array,
  offsetPos: number,
  splicePos: number,
  delta: number,
): void {
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const offsetVal = dv.getUint32(offsetPos, true);
  if (offsetVal <= 1) return; // 0 = empty, 1 = None — never patch
  const target = offsetPos + offsetVal;
  if (target >= splicePos) {
    dv.setUint32(offsetPos, offsetVal + delta, true);
  }
}

// ========================= Internal helpers =========================

function readU16(data: Uint8Array, pos: number): number {
  return data[pos] | (data[pos + 1] << 8);
}

function writeU16(data: Uint8Array, pos: number, val: number): void {
  data[pos] = val & 0xFF;
  data[pos + 1] = (val >> 8) & 0xFF;
}

function readU32(data: Uint8Array, pos: number): number {
  return (data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24)) >>> 0;
}

function writeU32(data: Uint8Array, pos: number, val: number): void {
  data[pos] = val & 0xFF;
  data[pos + 1] = (val >> 8) & 0xFF;
  data[pos + 2] = (val >> 16) & 0xFF;
  data[pos + 3] = (val >> 24) & 0xFF;
}

/** Pack a value as standalone bytes using the type's _packTo. */
function packStandalone<T>(ft: FracTypeImpl<T>, value: T): Uint8Array {
  const w = new Writer();
  ft._packTo(value, w);
  return w.finish();
}

/** Get the inner type for an optional wrapping a variable-size inner type. */
function getEffectiveType(ft: FracType<any>): FracTypeImpl<any> {
  const t = asImpl(ft);
  if (t.isOptional && t.schema.kind === 'optional') {
    const inner = asImpl(t.schema.inner);
    if (inner.isVariableSize && !inner.isOptional) {
      return inner;
    }
  }
  return t;
}

/** Get the inner type for packing optional values (unwrap the optional layer). */
function getInnerForPack(ft: FracType<any>): FracTypeImpl<any> {
  const t = asImpl(ft);
  if (t.isOptional && t.schema.kind === 'optional') {
    return asImpl(t.schema.inner);
  }
  return t;
}

// ========================= Packed size measurement =========================

/**
 * Measure the number of heap bytes consumed by a standalone value at `pos`.
 * For variable-size types this is the complete serialized size starting at pos
 * (e.g., length prefix + payload for strings).
 */
function packedSizeAt(ft: FracType<any>, data: Uint8Array, pos: number): number {
  const schema = ft.schema;

  if (schema.kind === 'bool') return 1;

  if (schema.kind === 'int') {
    return schema.bits / 8;
  }

  if (schema.kind === 'float') {
    return schema.exp === 8 ? 4 : 8;
  }

  if (schema.kind === 'string' || schema.kind === 'bytes') {
    const length = readU32(data, pos);
    return 4 + length;
  }

  if (schema.kind === 'list') {
    const dataSize = readU32(data, pos);
    const elemType = schema.element;
    if (!elemType.isVariableSize) {
      return 4 + dataSize;
    }
    // Variable-size elements: walk each element's heap data
    const count = dataSize / elemType.fixedSize;
    let total = 4 + dataSize; // length prefix + fixed region
    const fixedPos = pos + 4;
    for (let i = 0; i < count; i++) {
      const elemOffsetPos = fixedPos + i * elemType.fixedSize;
      const elemOffset = readU32(data, elemOffsetPos);
      if (elemOffset > 1) {
        const elemTarget = elemOffsetPos + elemOffset;
        const elemSize = packedSizeAt(elemType, data, elemTarget);
        const end = elemTarget + elemSize;
        if (end - pos > total) total = end - pos;
      }
    }
    return total;
  }

  if (schema.kind === 'optional') {
    // When measuring on heap, the optional's inner value is stored
    // directly (the offset indirection was in the fixed region).
    return packedSizeAt(schema.inner, data, pos);
  }

  if (schema.kind === 'struct') {
    const fields = schema.fields;
    const fieldNames = Object.keys(fields);
    const fixedSize = readU16(data, pos);
    const fixedStart = pos + 2;
    let end = fixedStart + fixedSize;

    let fieldPos = fixedStart;
    for (const name of fieldNames) {
      const fieldType = fields[name];
      if (fieldPos >= fixedStart + fixedSize) break;
      if (fieldType.isVariableSize) {
        const offVal = readU32(data, fieldPos);
        if (offVal > 1) {
          const target = fieldPos + offVal;
          const innerSize = packedSizeAt(fieldType, data, target);
          const candidate = target + innerSize;
          if (candidate > end) end = candidate;
        }
      }
      fieldPos += fieldType.fixedSize;
    }
    return end - pos;
  }

  if (schema.kind === 'fixedStruct') {
    const fields = schema.fields;
    const fieldNames = Object.keys(fields);
    let fixedSize = 0;
    for (const name of fieldNames) fixedSize += fields[name].fixedSize;
    const fixedStart = pos;
    let end = fixedStart + fixedSize;

    let fieldPos = fixedStart;
    for (const name of fieldNames) {
      const fieldType = fields[name];
      if (fieldType.isVariableSize) {
        const offVal = readU32(data, fieldPos);
        if (offVal > 1) {
          const target = fieldPos + offVal;
          const innerSize = packedSizeAt(fieldType, data, target);
          const candidate = target + innerSize;
          if (candidate > end) end = candidate;
        }
      }
      fieldPos += fieldType.fixedSize;
    }
    return end - pos;
  }

  if (schema.kind === 'variant') {
    // tag(1) + dataSize(4) + data
    const dataSize = readU32(data, pos + 1);
    return 1 + 4 + dataSize;
  }

  if (schema.kind === 'tuple') {
    // u16 fixed_size + fixed region + heap
    const tupleFix = readU16(data, pos);
    const fixedStart = pos + 2;
    let end = fixedStart + tupleFix;

    const elements = schema.elements;
    let elemPos = fixedStart;
    for (const elem of elements) {
      if (elemPos >= fixedStart + tupleFix) break;
      if (elem.isVariableSize) {
        const offVal = readU32(data, elemPos);
        if (offVal > 1) {
          const target = elemPos + offVal;
          const innerSize = packedSizeAt(elem, data, target);
          const candidate = target + innerSize;
          if (candidate > end) end = candidate;
        }
      }
      elemPos += elem.fixedSize;
    }
    return end - pos;
  }

  // Fallback: unpack to measure
  const r = new Reader(data.subarray(pos));
  const t = asImpl(ft);
  t._unpackFrom(r);
  return r.pos;
}

// ========================= Field metadata =========================

interface FieldMeta {
  name: string;
  type: FracType<any>;
  fixedOffset: number; // offset from start of fixed region (after header)
}

function buildFieldMeta(
  schema: SchemaNode & { kind: 'struct' | 'fixedStruct' },
): FieldMeta[] {
  const fields = schema.fields;
  const names = Object.keys(fields);
  const metas: FieldMeta[] = [];
  let offset = 0;
  for (const name of names) {
    const ft = fields[name];
    metas.push({ name, type: ft, fixedOffset: offset });
    offset += ft.fixedSize;
  }
  return metas;
}

// ========================= Sibling patching =========================

function patchSiblings(
  data: Uint8Array,
  base: number,
  headerSize: number,
  modifiedField: FieldMeta,
  allFields: FieldMeta[],
  splicePos: number,
  delta: number,
): void {
  if (delta === 0) return;
  const declaredFixed = headerSize === 2 ? readU16(data, base) : -1;

  for (const f of allFields) {
    if (f === modifiedField) continue;
    if (!f.type.isVariableSize) continue;
    // Skip elided fields
    if (headerSize === 2 && f.fixedOffset >= declaredFixed) continue;
    const siblingOffsetPos = base + headerSize + f.fixedOffset;
    patchOffset(data, siblingOffsetPos, splicePos, delta);
  }
}

// ========================= Elision helpers =========================

function isFieldElided(
  data: Uint8Array,
  base: number,
  headerSize: number,
  field: FieldMeta,
): boolean {
  if (headerSize !== 2) return false;
  const declaredFixed = readU16(data, base);
  return field.fixedOffset >= declaredFixed;
}

/**
 * Expand the extensible struct's fixed region to include `field`.
 * Inserts zero-initialised offset slots for all fields between the current
 * end of the fixed region and `field` (inclusive). Updates the u16 header.
 * Returns the new data and the byte delta.
 */
function expandFixedRegion(
  data: Uint8Array,
  base: number,
  headerSize: number,
  field: FieldMeta,
  allFields: FieldMeta[],
): { data: Uint8Array; delta: number } {
  if (headerSize !== 2) return { data, delta: 0 };

  const declaredFixed = readU16(data, base);
  const neededFixed = field.fixedOffset + field.type.fixedSize;

  if (neededFixed <= declaredFixed) return { data, delta: 0 };

  const insertPos = base + headerSize + declaredFixed;
  const insertSize = neededFixed - declaredFixed;

  // Build the inserted bytes: 4-byte slots initialized to 1 (None) for
  // optional fields, 0 for others
  const insertBytes = new Uint8Array(insertSize);
  for (const f of allFields) {
    if (f.fixedOffset >= declaredFixed && f.fixedOffset < neededFixed) {
      const slotStart = f.fixedOffset - declaredFixed;
      if (f.type.isOptional) {
        writeU32(insertBytes, slotStart, 1);
      }
    }
  }

  const { result: newData, delta } = spliceBuffer(data, insertPos, 0, insertBytes);

  // Update the header
  writeU16(newData, base, neededFixed);

  // Patch existing sibling offsets that point past the insert point
  for (const f of allFields) {
    if (f.fixedOffset >= declaredFixed) continue; // Just inserted
    if (!f.type.isVariableSize) continue;
    const siblingOffsetPos = base + headerSize + f.fixedOffset;
    patchOffset(newData, siblingOffsetPos, insertPos, delta);
  }

  return { data: newData, delta };
}

// ========================= MutView options =========================

export interface MutViewOptions {
  /** Base offset within the buffer. Default: 0. */
  base?: number;
  /** Use fast mode (overwrite/append) instead of canonical splice. Default: false. */
  fast?: boolean;
}

// ========================= MutView class =========================

/**
 * Mutable view over packed fracpack struct data.
 *
 * Reads fields lazily (like View) and supports field assignment that modifies
 * the underlying buffer in place.
 *
 * In canonical mode (default), mutations splice the buffer and patch offsets
 * so the result is always valid canonical fracpack.
 *
 * In fast mode, small mutations overwrite in place and larger ones append to
 * the end. Call compact() to produce canonical output.
 */
export class MutView<T = any> {
  /** @internal */ _data: Uint8Array;
  /** @internal */ _base: number;
  /** @internal */ _fast: boolean;
  /** @internal */ _headerSize: number;
  /** @internal */ _fields: FieldMeta[];
  /** @internal */ _fieldLookup: Map<string, number>;
  /** @internal */ _structType: FracType<T>;

  constructor(
    structType: StructType<T>,
    data: Uint8Array,
    options?: MutViewOptions,
  ) {
    const schema = structType.schema;
    if (schema.kind !== 'struct' && schema.kind !== 'fixedStruct') {
      throw new FracpackError('MutView requires a struct or fixedStruct type');
    }

    this._structType = structType;
    // Copy the data into a mutable buffer
    this._data = new Uint8Array(data);
    this._base = options?.base ?? 0;
    this._fast = options?.fast ?? false;
    this._headerSize = schema.kind === 'struct' ? 2 : 0;
    this._fields = buildFieldMeta(schema);
    this._fieldLookup = new Map();
    for (let i = 0; i < this._fields.length; i++) {
      this._fieldLookup.set(this._fields[i].name, i);
    }
  }

  /**
   * Read a field value from the packed data.
   */
  get<K extends keyof T & string>(name: K): T[K] {
    const idx = this._fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`MutView: unknown field '${name}'`);

    const field = this._fields[idx];
    const ft = field.type;
    const base = this._base;
    const headerSize = this._headerSize;

    // Check for elision
    if (isFieldElided(this._data, base, headerSize, field)) {
      return null as T[K];
    }

    const absPos = base + headerSize + field.fixedOffset;
    const data = this._data;
    const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);

    if (!ft.isVariableSize) {
      return asImpl(ft)._viewAt(data, dv, absPos) as T[K];
    }

    // Variable-size field
    const offset = readU32(data, absPos);
    if (offset === 0) return asImpl(ft)._newEmptyContainer() as T[K];
    if (offset === 1 && ft.isOptional) return null as T[K];

    // For optionals wrapping variable-size inner, use the inner type's viewAt
    const effType = getEffectiveType(ft);
    return effType._viewAt(data, dv, absPos + offset) as T[K];
  }

  /**
   * Set a field value, modifying the underlying buffer.
   */
  set<K extends keyof T & string>(name: K, value: T[K]): void {
    const idx = this._fieldLookup.get(name);
    if (idx === undefined) throw new FracpackError(`MutView: unknown field '${name}'`);

    const field = this._fields[idx];
    const ft = field.type;

    if (!ft.isVariableSize) {
      this._setScalar(field, value);
    } else if (this._fast) {
      this._setVariableFast(field, value);
    } else {
      this._setVariableCanonical(field, value);
    }
  }

  /**
   * Return a copy of the current buffer contents as Uint8Array.
   */
  toBytes(): Uint8Array {
    return new Uint8Array(this._data);
  }

  /**
   * Repack the buffer from scratch (removes dead bytes).
   * Primarily useful after fast-mode mutations.
   */
  compact(): void {
    // Use view instead of unpack to avoid "Extra data" errors in fast mode
    // where dead bytes may exist at the end of the buffer.
    const obj = this._structType.view(this._data);
    const newData = this._structType.pack(obj as T);
    this._data = new Uint8Array(newData);
    this._base = 0;
  }

  // ── Scalar writer ──

  private _setScalar(field: FieldMeta, value: any): void {
    const ft = asImpl(field.type);
    const absPos = this._base + this._headerSize + field.fixedOffset;

    // Check for elision - expand if needed
    if (isFieldElided(this._data, this._base, this._headerSize, field)) {
      const { data } = expandFixedRegion(
        this._data, this._base, this._headerSize, field, this._fields,
      );
      this._data = data;
    }

    const w = new Writer(ft.fixedSize);
    ft._packTo(value, w);
    this._data.set(w.finish(), absPos);
  }

  // ── Variable-size canonical writer ──

  private _setVariableCanonical(field: FieldMeta, value: any): void {
    const ft = field.type;
    const effFt = getEffectiveType(ft);
    const settingNone = value === null && ft.isOptional;

    // Check for trailing optional elision
    if (isFieldElided(this._data, this._base, this._headerSize, field)) {
      if (settingNone) return; // Already elided = None, nothing to do
      // Expand the fixed region
      const { data } = expandFixedRegion(
        this._data, this._base, this._headerSize, field, this._fields,
      );
      this._data = data;
    }

    const offsetPos = this._base + this._headerSize + field.fixedOffset;
    const oldOffset = readU32(this._data, offsetPos);

    // Check if setting to empty container
    const settingEmpty = !settingNone && ft.isVariableSize &&
      asImpl(ft)._isEmptyContainer(value);

    if (settingNone) {
      if (oldOffset <= 1) {
        // Already None or empty - just set the marker
        writeU32(this._data, offsetPos, 1);
        return;
      }
      // There is existing heap data - remove it
      const oldTarget = offsetPos + oldOffset;
      const oldSize = packedSizeAt(effFt, this._data, oldTarget);
      const { result, delta } = spliceBuffer(this._data, oldTarget, oldSize, new Uint8Array(0));
      this._data = result;
      writeU32(this._data, offsetPos, 1);
      patchSiblings(this._data, this._base, this._headerSize, field, this._fields, oldTarget, delta);
      return;
    }

    if (settingEmpty) {
      if (oldOffset <= 1) {
        writeU32(this._data, offsetPos, 0);
        return;
      }
      // Remove existing heap data
      const oldTarget = offsetPos + oldOffset;
      const oldSize = packedSizeAt(effFt, this._data, oldTarget);
      const { result, delta } = spliceBuffer(this._data, oldTarget, oldSize, new Uint8Array(0));
      this._data = result;
      writeU32(this._data, offsetPos, 0);
      patchSiblings(this._data, this._base, this._headerSize, field, this._fields, oldTarget, delta);
      return;
    }

    // Pack new value
    const innerType = getInnerForPack(ft);
    const newBytes = packStandalone(innerType, value);

    if (oldOffset <= 1) {
      // No existing heap data - insert at end of buffer
      const insertPos = this._data.length;
      const { result } = spliceBuffer(this._data, insertPos, 0, newBytes);
      this._data = result;
      writeU32(this._data, offsetPos, insertPos - offsetPos);
      // No siblings to patch since we appended at the end
      return;
    }

    // Replace existing heap data
    const oldTarget = offsetPos + oldOffset;
    const oldSize = packedSizeAt(effFt, this._data, oldTarget);
    const { result, delta } = spliceBuffer(this._data, oldTarget, oldSize, newBytes);
    this._data = result;
    // The offset from offsetPos to the target doesn't change because we
    // spliced at the target position; the offset stays the same.
    // But we must patch siblings.
    patchSiblings(this._data, this._base, this._headerSize, field, this._fields, oldTarget, delta);
  }

  // ── Variable-size fast writer ──

  private _setVariableFast(field: FieldMeta, value: any): void {
    const ft = field.type;
    const effFt = getEffectiveType(ft);

    // Setting to None
    if (value === null && ft.isOptional) {
      if (isFieldElided(this._data, this._base, this._headerSize, field)) {
        return; // Already elided = None
      }
      writeU32(this._data, this._base + this._headerSize + field.fixedOffset, 1);
      return;
    }

    // Check for trailing optional elision - expand if needed
    if (isFieldElided(this._data, this._base, this._headerSize, field)) {
      const { data } = expandFixedRegion(
        this._data, this._base, this._headerSize, field, this._fields,
      );
      this._data = data;
    }

    const offsetPos = this._base + this._headerSize + field.fixedOffset;
    const oldOffset = readU32(this._data, offsetPos);

    // Setting to empty container
    if (ft.isVariableSize && asImpl(ft)._isEmptyContainer(value)) {
      writeU32(this._data, offsetPos, 0);
      return;
    }

    // Pack new value
    const innerType = getInnerForPack(ft);
    const newBytes = packStandalone(innerType, value);

    if (oldOffset <= 1) {
      // No existing data - append
      const appendPos = this._data.length;
      const extended = new Uint8Array(appendPos + newBytes.length);
      extended.set(this._data);
      extended.set(newBytes, appendPos);
      this._data = extended;
      writeU32(this._data, offsetPos, appendPos - offsetPos);
      return;
    }

    // Measure old data
    const oldTarget = offsetPos + oldOffset;
    const oldSize = packedSizeAt(effFt, this._data, oldTarget);

    if (newBytes.length <= oldSize) {
      // Fits - overwrite in place (dead bytes at the end are OK)
      this._data.set(newBytes, oldTarget);
      return;
    }

    // Doesn't fit - append to end, update offset
    const appendPos = this._data.length;
    const extended = new Uint8Array(appendPos + newBytes.length);
    extended.set(this._data);
    extended.set(newBytes, appendPos);
    this._data = extended;
    writeU32(this._data, offsetPos, appendPos - offsetPos);
  }
}

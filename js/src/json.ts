// json.ts — JSON ↔ fracpack conversion
//
// Schema-aware conversion between fracpack binary data and JSON-friendly
// JavaScript values. Compatible with Rust fracpack JSON conventions.

import type { FracType } from './fracpack.js';

// ========================= Hex encoding =========================

const HEX_CHARS = '0123456789abcdef';

/** Convert Uint8Array to lowercase hex string. */
export function toHex(data: Uint8Array): string {
  let s = '';
  for (let i = 0; i < data.length; i++) {
    s += HEX_CHARS[data[i] >> 4] + HEX_CHARS[data[i] & 0x0f];
  }
  return s;
}

/** Convert hex string to Uint8Array. */
export function fromHex(hex: string): Uint8Array {
  if (hex.length % 2 !== 0) throw new Error('Hex string must have even length');
  const result = new Uint8Array(hex.length / 2);
  for (let i = 0; i < result.length; i++) {
    const hi = parseInt(hex[i * 2], 16);
    const lo = parseInt(hex[i * 2 + 1], 16);
    if (isNaN(hi) || isNaN(lo)) throw new Error(`Invalid hex at position ${i * 2}`);
    result[i] = (hi << 4) | lo;
  }
  return result;
}

// ========================= fracpack → JSON =========================

/**
 * Convert packed fracpack data to a JSON-friendly JavaScript value.
 *
 * Conventions (matching Rust):
 * - bigint (u64/i64) → string (JSON has no 64-bit int)
 * - bytes → hex string
 * - optional null → null
 * - variant → { "CaseName": value }
 * - map → plain object
 */
export function fracToJson(type: FracType<any>, data: Uint8Array): unknown {
  return valueToJson(type, type.view(data));
}

/** Convert a JS value (from unpack/view) to JSON-friendly form using schema. */
export function valueToJson(type: FracType<any>, value: any): unknown {
  const s = type.schema;
  switch (s.kind) {
    case 'bool':
      return value;
    case 'int':
      return s.bits === 64 ? String(value) : value;
    case 'float':
      return value;
    case 'string':
      return value;
    case 'bytes':
      return toHex(value as Uint8Array);
    case 'optional':
      return value === null || value === undefined
        ? null
        : valueToJson(s.inner, value);
    case 'list': {
      const arr = value as ArrayLike<any>;
      const result: unknown[] = new Array(arr.length);
      for (let i = 0; i < arr.length; i++) {
        result[i] = valueToJson(s.element, arr[i]);
      }
      return result;
    }
    case 'array': {
      const arr = value as ArrayLike<any>;
      const result: unknown[] = new Array(arr.length);
      for (let i = 0; i < arr.length; i++) {
        result[i] = valueToJson(s.element, arr[i]);
      }
      return result;
    }
    case 'tuple':
      return s.elements.map((e, i) => valueToJson(e, (value as any[])[i]));
    case 'variant': {
      const v = value as { type: string; value: any };
      return { [v.type]: valueToJson(s.cases[v.type], v.value) };
    }
    case 'struct':
    case 'fixedStruct': {
      const result: Record<string, unknown> = {};
      for (const [name, fieldType] of Object.entries(s.fields)) {
        result[name] = valueToJson(fieldType, value[name]);
      }
      return result;
    }
    case 'map': {
      const result: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(value as Record<string, any>)) {
        result[k] = valueToJson(s.value, v);
      }
      return result;
    }
  }
}

// ========================= JSON → fracpack =========================

/**
 * Convert a JSON-friendly JavaScript value to packed fracpack data.
 *
 * Conventions:
 * - string → bigint for u64/i64
 * - hex string → Uint8Array for bytes
 * - null → optional None
 * - { "CaseName": value } → variant
 * - plain object → map
 */
export function jsonToFrac(type: FracType<any>, json: unknown): Uint8Array {
  return type.pack(jsonToValue(type, json));
}

/** Convert a JSON-friendly value to a pack()-ready JS value using schema. */
export function jsonToValue(type: FracType<any>, json: any): any {
  const s = type.schema;
  switch (s.kind) {
    case 'bool':
      return !!json;
    case 'int':
      if (s.bits === 64) return BigInt(json);
      return typeof json === 'string' ? parseInt(json, 10) : Number(json);
    case 'float':
      return Number(json);
    case 'string':
      return String(json);
    case 'bytes':
      return typeof json === 'string' ? fromHex(json) : json;
    case 'optional':
      return json === null || json === undefined
        ? null
        : jsonToValue(s.inner, json);
    case 'list':
      return (json as any[]).map(v => jsonToValue(s.element, v));
    case 'array':
      return (json as any[]).map(v => jsonToValue(s.element, v));
    case 'tuple':
      return s.elements.map((e, i) => jsonToValue(e, (json as any[])[i]));
    case 'variant': {
      const keys = Object.keys(json);
      if (keys.length !== 1) throw new Error(`Variant JSON must have exactly one key, got ${keys.length}`);
      const key = keys[0];
      if (!(key in s.cases)) throw new Error(`Unknown variant case: ${key}`);
      return { type: key, value: jsonToValue(s.cases[key], json[key]) };
    }
    case 'struct':
    case 'fixedStruct': {
      const result: Record<string, any> = {};
      for (const [name, fieldType] of Object.entries(s.fields)) {
        const val = json?.[name];
        result[name] = val === undefined || val === null
          ? null
          : jsonToValue(fieldType, val);
      }
      return result;
    }
    case 'map': {
      const result: Record<string, any> = {};
      for (const [k, v] of Object.entries(json as Record<string, any>)) {
        result[k] = jsonToValue(s.value, v);
      }
      return result;
    }
  }
}

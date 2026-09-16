// schema.ts — AnyType schema generation compatible with Rust fracpack
//
// Converts FracType schema nodes into the AnyType JSON format used by
// the Rust psio library for cross-language schema exchange.

import type { FracType, SchemaNode } from './fracpack.js';

// ========================= AnyType (matches Rust serde output) =========================

/** JSON-serializable type description, matching Rust's AnyType enum. */
export type AnyType =
  | { Struct: Record<string, AnyType> }
  | { Object: Record<string, AnyType> }
  | { Array: { type: AnyType; len: number } }
  | { List: AnyType }
  | { Option: AnyType }
  | { Variant: Record<string, AnyType> }
  | { Tuple: AnyType[] }
  | { Int: { bits: number; isSigned: boolean } }
  | { Float: { exp: number; mantissa: number } }
  | { FracPack: AnyType }
  | { Custom: { type: AnyType; id: string } }
  | string; // Type reference (e.g. "@0" or named type)

/** A schema registry mapping type names to their AnyType definitions. */
export type Schema = Record<string, AnyType>;

// ========================= Cached leaf constants =========================

const BOOL_SCHEMA: AnyType = { Custom: { type: { Int: { bits: 1, isSigned: false } }, id: 'bool' } };
const STRING_SCHEMA: AnyType = { Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'string' } };
const BYTES_SCHEMA: AnyType = { Custom: { type: { List: { Int: { bits: 8, isSigned: false } } }, id: 'hex' } };

// ========================= Conversion =========================

const anyTypeCache = new WeakMap<FracType<any>, AnyType>();

/** Convert a FracType's schema node to the AnyType JSON format. */
export function typeToAnyType(type: FracType<any>): AnyType {
  let result = anyTypeCache.get(type);
  if (result !== undefined) return result;
  result = nodeToAnyType(type.schema);
  anyTypeCache.set(type, result);
  return result;
}

function nodeToAnyType(node: SchemaNode): AnyType {
  switch (node.kind) {
    case 'bool':
      return BOOL_SCHEMA;

    case 'int':
      return { Int: { bits: node.bits, isSigned: node.signed } };

    case 'float':
      return { Float: { exp: node.exp, mantissa: node.mantissa } };

    case 'string':
      return STRING_SCHEMA;

    case 'bytes':
      return BYTES_SCHEMA;

    case 'optional':
      return { Option: typeToAnyType(node.inner) };

    case 'list':
      return { List: typeToAnyType(node.element) };

    case 'array':
      return { Array: { type: typeToAnyType(node.element), len: node.len } };

    case 'tuple':
      return { Tuple: node.elements.map(e => typeToAnyType(e)) };

    case 'variant': {
      const cases: Record<string, AnyType> = {};
      for (const [name, type] of Object.entries(node.cases)) {
        cases[name] = typeToAnyType(type);
      }
      return { Variant: cases };
    }

    case 'fixedStruct': {
      const fields: Record<string, AnyType> = {};
      for (const [name, type] of Object.entries(node.fields)) {
        fields[name] = typeToAnyType(type);
      }
      return { Struct: fields };
    }

    case 'struct': {
      const fields: Record<string, AnyType> = {};
      for (const [name, type] of Object.entries(node.fields)) {
        fields[name] = typeToAnyType(type);
      }
      return { Object: fields };
    }

    case 'map': {
      // Map → List<Object{key, value}> (same as Rust IndexMap)
      const entryFields: Record<string, AnyType> = {
        key: typeToAnyType(node.key),
        value: typeToAnyType(node.value),
      };
      return { List: { Object: entryFields } };
    }
  }
}

// ========================= Schema registry =========================

/**
 * Generate a Schema registry from named types.
 * Each key becomes a named entry; nested types are inlined.
 *
 * @example
 * const Person = struct({ name: str, age: u32 });
 * const schema = generateSchema({ Person });
 * // { "Person": { "Object": { "name": { "Custom": ... }, "age": { "Int": ... } } } }
 */
export function generateSchema(types: Record<string, FracType<any>>): Schema {
  const schema: Schema = {};
  for (const [name, type] of Object.entries(types)) {
    schema[name] = typeToAnyType(type);
  }
  return schema;
}

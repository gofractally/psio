// schema.ts — Schema IR generation from the type combinators
//
// Converts the schema nodes of the type combinators (`struct`, `str`, `vec`,
// ...) into the Schema IR that C++ `psio::SchemaBuilder` and Rust
// `ToPsszSchema` produce, in its compact spelling. The result is exactly a
// `PsszSchema` input: `new PsszSchema(generateSchema({ Person }))`.
//
// Text and bytes, as everywhere in the IR:
//
//   combinator   IR node                          JavaScript value
//   str          Custom{List<u8>, "utf8"}         string (validated UTF-8)
//   bytes        Custom{List<u8>, "hex"}          Uint8Array
//
// The IR's third byte-container id, "string", is a RAW string (C++
// std::string, Rust RawString): any bytes, a Uint8Array as a value. No
// combinator produces it — `str` is text — and the PSSZ codec reads it from
// schemas written elsewhere.

import type { FracType, SchemaNode } from './fracpack.js';
import type { CompactType } from './pssz-core.js';

// ========================= AnyType (the Schema IR, compact spelling) =========================

/** A type of the Schema IR in its compact spelling (`CompactType`). */
export type AnyType = CompactType;

/** Named types: a `PsszSchema` input. */
export type Schema = { [name: string]: AnyType };

// ========================= Cached leaf constants =========================

const U8_SCHEMA: AnyType = { Int: { bits: 8, isSigned: false } };
const BOOL_SCHEMA: AnyType = { Custom: { type: { Int: { bits: 1, isSigned: false } }, id: 'bool' } };
/** Text: UTF-8 on the wire, a `string` in JavaScript. */
const STRING_SCHEMA: AnyType = { Custom: { type: { List: U8_SCHEMA }, id: 'utf8' } };
/** Bytes: a `Uint8Array` in JavaScript (hex in I-JSON, an array of numbers in JSON). */
const BYTES_SCHEMA: AnyType = { Custom: { type: { List: U8_SCHEMA }, id: 'hex' } };

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
 * // { "Person": { "Object": { "name": { "Custom": { ..., "id": "utf8" } }, "age": { "Int": ... } } } }
 * const pssz = new PsszSchema(schema).type('Person');
 */
export function generateSchema(types: Record<string, FracType<any>>): Schema {
  const schema: Schema = {};
  for (const [name, type] of Object.entries(types)) {
    schema[name] = typeToAnyType(type);
  }
  return schema;
}

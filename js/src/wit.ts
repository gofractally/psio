// wit.ts — WIT (WebAssembly Interface Types) text generation
//
// Generates WIT text format from FracType definitions, compatible with
// the C++ psio wit_gen.hpp output.

import type { FracType, SchemaNode } from './fracpack.js';

// ========================= WIT function descriptor =========================

/** Describes a WIT function's parameter and return types. */
export interface WitFunc {
  params: Record<string, FracType<any>>;
  result?: FracType<any>;
}

/** Options for WIT world generation. */
export interface WitOptions {
  types?: Record<string, FracType<any>>;
  exports?: Record<string, WitFunc>;
  imports?: Record<string, WitFunc>;
}

// ========================= Name conversion =========================

const kebabCache = new Map<string, string>();

/** Convert camelCase or snake_case to kebab-case (matching C++ to_kebab_case). */
export function toKebabCase(s: string): string {
  let cached = kebabCache.get(s);
  if (cached !== undefined) return cached;

  let result = '';
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    if (c === '_') {
      if (result.length > 0 && result[result.length - 1] !== '-') result += '-';
    } else if (c >= 'A' && c <= 'Z') {
      if (result.length > 0 && result[result.length - 1] !== '-') result += '-';
      result += c.toLowerCase();
    } else {
      result += c;
    }
  }

  kebabCache.set(s, result);
  return result;
}

// ========================= Type context =========================

interface WitGenCtx {
  /** Named type definitions to emit. Maps name → SchemaNode. */
  typeDefs: Map<string, SchemaNode>;
  /** FracType → registered name (O(1) reverse lookup). */
  nameOf: Map<FracType<any>, string>;
  /** Auto-name counter (local to this context, not global). */
  nextId: number;
}

function newCtx(): WitGenCtx {
  return { typeDefs: new Map(), nameOf: new Map(), nextId: 0 };
}

function autoName(ctx: WitGenCtx): string {
  return `type-${ctx.nextId++}`;
}

// ========================= Type expression =========================

/**
 * Convert a FracType to an inline WIT type expression.
 * Records and variants are registered as named types and referenced by name.
 */
function typeExpr(type: FracType<any>, ctx: WitGenCtx, nameHint?: string): string {
  return nodeToWit(type.schema, type, ctx, nameHint);
}

function nodeToWit(node: SchemaNode, type: FracType<any>, ctx: WitGenCtx, nameHint?: string): string {
  switch (node.kind) {
    case 'bool':
      return 'bool';

    case 'int': {
      const prefix = node.signed ? 's' : 'u';
      return `${prefix}${node.bits}`;
    }

    case 'float':
      return node.exp === 8 ? 'f32' : 'f64';

    case 'string':
      return 'string';

    case 'bytes':
      return 'list<u8>';

    case 'optional':
      return `option<${typeExpr(node.inner, ctx)}>`;

    case 'list':
      return `list<${typeExpr(node.element, ctx)}>`;

    case 'array':
      // WIT has no fixed-length array; use list
      return `list<${typeExpr(node.element, ctx)}>`;

    case 'tuple': {
      const parts: string[] = [];
      for (const e of node.elements) parts.push(typeExpr(e, ctx));
      return `tuple<${parts.join(', ')}>`;
    }

    case 'variant':
    case 'struct':
    case 'fixedStruct': {
      // O(1) lookup for already-registered types
      const existing = ctx.nameOf.get(type);
      if (existing !== undefined) return toKebabCase(existing);

      // Register new named type
      const name = nameHint || autoName(ctx);
      ctx.nameOf.set(type, name);
      ctx.typeDefs.set(name, node);
      // Recursively discover nested named types
      discoverNested(node, ctx);
      return toKebabCase(name);
    }

    case 'map':
      // WIT has no map type; use list<tuple<key, value>>
      return `list<tuple<${typeExpr(node.key, ctx)}, ${typeExpr(node.value, ctx)}>>`;
  }
}

/**
 * Recursively discover named types (struct/variant) nested inside a schema node,
 * including through containers like optional, list, array, tuple.
 */
function discoverNested(node: SchemaNode, ctx: WitGenCtx): void {
  switch (node.kind) {
    case 'struct':
    case 'fixedStruct':
      for (const type of Object.values(node.fields)) {
        discoverInner(type, ctx);
      }
      break;
    case 'variant':
      for (const type of Object.values(node.cases)) {
        discoverInner(type, ctx);
      }
      break;
    case 'optional':
      discoverInner(node.inner, ctx);
      break;
    case 'list':
      discoverInner(node.element, ctx);
      break;
    case 'array':
      discoverInner(node.element, ctx);
      break;
    case 'tuple':
      for (const e of node.elements) {
        discoverInner(e, ctx);
      }
      break;
    case 'map':
      discoverInner(node.key, ctx);
      discoverInner(node.value, ctx);
      break;
  }
}

function discoverInner(type: FracType<any>, ctx: WitGenCtx): void {
  const inner = type.schema;
  if ((inner.kind === 'struct' || inner.kind === 'fixedStruct' || inner.kind === 'variant')
      && !ctx.nameOf.has(type)) {
    typeExpr(type, ctx);
  } else {
    // Recurse into containers to find deeper nested named types
    discoverNested(inner, ctx);
  }
}

// ========================= Type definition emission =========================

function emitTypeDef(name: string, node: SchemaNode, ctx: WitGenCtx): string {
  const kebabName = toKebabCase(name);

  switch (node.kind) {
    case 'struct':
    case 'fixedStruct': {
      const fields = Object.entries(node.fields)
        .map(([fname, ftype]) => `  ${toKebabCase(fname)}: ${typeExpr(ftype, ctx)},`)
        .join('\n');
      return `record ${kebabName} {\n${fields}\n}`;
    }

    case 'variant': {
      const cases = Object.entries(node.cases)
        .map(([cname, ctype]) => {
          const expr = typeExpr(ctype, ctx);
          return `  ${toKebabCase(cname)}(${expr}),`;
        })
        .join('\n');
      return `variant ${kebabName} {\n${cases}\n}`;
    }

    default:
      // For non-composite types, emit a type alias
      return `type ${kebabName} = ${nodeToWit(node, null!, ctx)};`;
  }
}

// ========================= Function emission =========================

function emitFunc(name: string, func: WitFunc, ctx: WitGenCtx): string {
  const params = Object.entries(func.params)
    .map(([pname, ptype]) => `${toKebabCase(pname)}: ${typeExpr(ptype, ctx)}`)
    .join(', ');

  let result = '';
  if (func.result) {
    result = ` -> ${typeExpr(func.result, ctx)}`;
  }

  return `${toKebabCase(name)}: func(${params})${result}`;
}

// ========================= World generation =========================

/**
 * Generate WIT text for a world.
 *
 * @example
 * const Person = struct({ name: str, age: u32, active: bool });
 * const wit = generateWit('my-api', {
 *   types: { Person },
 *   exports: {
 *     getPerson: { params: {}, result: Person },
 *     setPerson: { params: { p: Person }, result: undefined },
 *   },
 * });
 */
export function generateWit(worldName: string, opts: WitOptions = {}): string {
  const ctx = newCtx();

  // Phase 1: register explicitly named types (names first, then nested)
  if (opts.types) {
    // First pass: reserve all explicit names
    for (const [name, type] of Object.entries(opts.types)) {
      ctx.nameOf.set(type, name);
      ctx.typeDefs.set(name, type.schema);
    }
    // Second pass: discover nested named types
    for (const type of Object.values(opts.types)) {
      discoverNested(type.schema, ctx);
    }
  }

  // Phase 2: resolve types referenced by functions
  const exportLines: string[] = [];
  if (opts.exports) {
    for (const [name, func] of Object.entries(opts.exports)) {
      exportLines.push(`  export ${emitFunc(name, func, ctx)};`);
    }
  }

  const importLines: string[] = [];
  if (opts.imports) {
    for (const [name, func] of Object.entries(opts.imports)) {
      importLines.push(`  import ${emitFunc(name, func, ctx)};`);
    }
  }

  // Phase 3: emit type definitions (outside world block)
  const typeBlocks: string[] = [];
  for (const [name, node] of ctx.typeDefs) {
    typeBlocks.push(emitTypeDef(name, node, ctx));
  }

  // Phase 4: assemble
  const lines: string[] = [];

  if (typeBlocks.length > 0) {
    lines.push(typeBlocks.join('\n\n'));
    lines.push('');
  }

  lines.push(`world ${toKebabCase(worldName)} {`);
  for (const line of importLines) lines.push(line);
  for (const line of exportLines) lines.push(line);
  lines.push('}');

  return lines.join('\n');
}

/**
 * Generate just the WIT type expression for a single type (no world wrapper).
 * Useful for inline type references.
 */
export function typeToWitExpr(type: FracType<any>): string {
  const ctx = newCtx();
  return typeExpr(type, ctx);
}

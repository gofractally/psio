// wit.ts — Schema IR -> WIT text.
//
// A port of C++ `psio::emit_wit` (cpp/include/psio/emit_wit.hpp): the
// package, `use` lines, interfaces (their named types and functions), worlds
// and, last, the named types no interface lists. It is driven by the IR
// alone, so it works on a schema fetched at run time and on one generated
// from typed definitions (`generateSchema`).
//
// Type mapping: Int -> u8..u64 / s8..s64; Float -> f32 / f64; bool, char;
// text -> string; List -> list<T>; Option -> option<T>; Tuple -> tuple<..>;
// record (Object / Struct) -> record; Variant -> variant (an empty tuple is
// a case with no payload); Resource -> resource; an Enum is its integer; any
// other Custom is its wire type. What WIT cannot say is written as a
// comment, as in C++: a bounded list is `list<T>/* @psio:max=N */` and a
// fixed-size array `/* TODO: fixed-size array */`.
//
// Two deliberate differences from emit_wit.hpp as of this port: a float is
// recognized in both IR spellings (mantissa 23 / 24 and 52 / 53 — the C++
// schema builder writes 24 / 53, which emit_wit.hpp prints as a TODO
// comment), and a `utf8` string is `string` (emit_wit.hpp maps only the raw
// `string` id and prints `utf8` as `list<u8>`).

import {
  PsszError, type IrAnyType, type IrAttribute, type IrFunc, type IrMember, type IrSchema,
} from './pssz-core.js';
import { PsszSchema } from './pssz.js';

function kebab(name: string): string {
  let out = '';
  for (let i = 0; i < name.length; ++i) {
    const c = name[i];
    if (c === '_') {
      out += '-';
      continue;
    }
    const upper = c >= 'A' && c <= 'Z';
    if (i > 0 && upper && name[i - 1] >= 'a' && name[i - 1] <= 'z') out += '-';
    out += upper ? c.toLowerCase() : c;
  }
  return out;
}

class Emitter {
  private out = '';
  private depth = 0;
  private readonly byName = new Map<string, IrAnyType>();

  constructor(private readonly schema: IrSchema) {
    // The first entry of a name wins, as `Schema::get`.
    for (const m of schema.types) if (!this.byName.has(m.name)) this.byName.set(m.name, m.type);
  }

  run(): string {
    const s = this.schema;
    if (s.package.name !== '')
      this.out += `package ${s.package.name}${s.package.version !== '' ? `@${s.package.version}` : ''};\n\n`;
    if (s.uses.length) {
      for (const u of s.uses)
        this.out += `use ${u.package !== '' ? `${u.package}/` : ''}${kebab(u.interface_name)}${u.version !== '' ? `@${u.version}` : ''};\n`;
      this.out += '\n';
    }
    for (const iface of s.interfaces) {
      this.out += `interface ${kebab(iface.name)} {\n`;
      ++this.depth;
      for (const name of iface.type_names) {
        const type = this.byName.get(name);
        if (type) this.declare(name, type);
        else this.line(`/* TODO: missing type "${name}" */`);
      }
      for (const fn of iface.funcs) this.func(fn);
      --this.depth;
      this.out += '}\n\n';
    }
    for (const world of s.worlds) {
      this.out += `world ${kebab(world.name)} {\n`;
      ++this.depth;
      for (const i of world.imports) this.line(`import ${i.package !== '' ? `${i.package}/` : ''}${kebab(i.interface_name)};`);
      for (const e of world.exports) this.line(`export ${kebab(e)};`);
      --this.depth;
      this.out += '}\n\n';
    }
    const listed = new Set(s.interfaces.flatMap((iface) => iface.type_names));
    for (const m of s.types) if (!listed.has(m.name) && !m.name.startsWith('@')) this.declare(m.name, m.type);
    return this.out;
  }

  private line(text: string): void {
    this.out += '  '.repeat(this.depth) + text + '\n';
  }

  private declare(name: string, type: IrAnyType): void {
    const t = type.value;
    switch (t.type) {
      case 'Object':
      case 'Struct':
        this.attributes(t.value.attributes);
        this.block('record', name, t.value.members, (m) => `${kebab(m.name)}: ${this.inline(m.type, 0)}`);
        break;
      case 'Variant':
        this.block('variant', name, t.value.members, (m) => {
          const empty = m.type.value.type === 'Tuple' && m.type.value.value.members.length === 0;
          return empty ? kebab(m.name) : `${kebab(m.name)}(${this.inline(m.type, 0)})`;
        });
        break;
      case 'Resource':
        this.line(`resource ${kebab(t.value.name)} {`);
        ++this.depth;
        for (const method of t.value.methods) this.func(method);
        --this.depth;
        this.line('}');
        break;
      default:
        this.line(`type ${kebab(name)} = ${this.inline(type, 0)};`);
    }
  }

  private attributes(attributes: IrAttribute[]): void {
    for (const a of attributes)
      if (a.name === 'definitionWillNotChange' || a.name === 'maxFields' || a.name === 'maxDynamicData')
        this.line(`@${a.name}${a.value !== null && a.value !== '' ? `(${a.value})` : ''}`);
  }

  private block(keyword: string, name: string, members: IrMember[], text: (m: IrMember) => string): void {
    this.line(`${keyword} ${kebab(name)} {`);
    ++this.depth;
    members.forEach((m, i) => this.line(text(m) + (i + 1 < members.length ? ',' : '')));
    --this.depth;
    this.line('}');
  }

  private func(fn: IrFunc): void {
    // (C++ `schema_types::parameter_identifier`: a positional parameter "0" is `arg0`)
    const identifier = (name: string) => (name === '' || (name[0] >= '0' && name[0] <= '9') ? `arg${name}` : name);
    const params = fn.params.map((p) => `${kebab(identifier(p.name))}: ${this.inline(p.type, 0)}`).join(', ');
    this.line(`${kebab(fn.name)}: func(${params})${fn.result ? ` -> ${this.inline(fn.result, 0)}` : ''};`);
  }

  private inline(type: IrAnyType, depth: number): string {
    if (depth > 128) throw new PsszError('schema', 'wit: type nesting too deep (cyclic aliases?)');
    const t = type.value;
    const sub = (x: IrAnyType) => this.inline(x, depth + 1);
    switch (t.type) {
      case 'Int':
        return `${t.value.isSigned ? 's' : 'u'}${t.value.bits}`;
      case 'Float':
        if (t.value.exp === 8 && (t.value.mantissa === 23 || t.value.mantissa === 24)) return 'f32';
        if (t.value.exp === 11 && (t.value.mantissa === 52 || t.value.mantissa === 53)) return 'f64';
        return '/* TODO: non-IEEE float */';
      case 'Type': {
        const name = t.value.type;
        if (name === 'bool' || name === 'string' || name === 'char') return name;
        if (name.startsWith('@')) {
          const target = this.byName.get(name);
          return target ? sub(target) : `/* TODO: unresolved ${name} */`;
        }
        return kebab(name);
      }
      case 'List':
        return `list<${sub(t.value.type)}>`;
      case 'BoundedList':
        return `list<${sub(t.value.type)}>/* @psio:max=${t.value.maxCount} */`;
      case 'Option':
        return `option<${sub(t.value.type)}>`;
      case 'Tuple':
        return `tuple<${t.value.members.map(sub).join(', ')}>`;
      case 'Enum':
        return sub(t.value.type);
      case 'Custom':
        if (t.value.id === 'bool' || t.value.id === 'char') return t.value.id;
        if (t.value.id === 'string' || t.value.id === 'utf8') return 'string';
        return sub(t.value.type);
      case 'Resource':
        return `own<${kebab(t.value.name)}>`;
      case 'Object':
      case 'Struct':
        return '/* TODO: inline record not supported */';
      case 'Variant':
        return '/* TODO: inline variant not supported */';
      case 'Array':
        return '/* TODO: fixed-size array */';
      case 'Nested':
        // The PSSZ bytes of the inner value.
        return 'list<u8>';
      default:
        return '/* TODO: unsupported type */';
    }
  }
}

/**
 * The WIT text of a schema: its package, interfaces and worlds, and the
 * named types (records, variants, resources, aliases). Throws a `PsszError`
 * of kind `schema` / `value` for a schema that is not a well-formed
 * `psio::Schema`.
 */
export function emitWit(schema: PsszSchema): string {
  if (!(schema instanceof PsszSchema)) throw new PsszError('schema', 'wit: a PsszSchema is required');
  // Through its own encoding: the envelope is checked like any other value.
  const checked = PsszSchema.decode(schema.encode()).ir;
  try {
    return new Emitter(checked).run();
  } catch (e) {
    if (e instanceof PsszError) throw e;
    throw new PsszError('internal', e instanceof Error ? `${e.name}: ${e.message}` : String(e));
  }
}

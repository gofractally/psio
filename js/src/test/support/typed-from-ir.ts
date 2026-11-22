// Rebuilds typed definitions (`record`, `list`, ...) from a Schema IR that a
// fixture records, so the fixtures can be replayed through the typed layer:
// each IR type is turned into the declaration a programmer would write for
// it. The round trip is checked — `generateSchema` of the declarations must
// give back the fixture's IR, byte for byte.
//
// A type the typed layer cannot declare is reported with the reason, never
// skipped silently.

import {
  PsszSchema, TypeDef, alt, array, bitlist, bitvector, bool, boundedBytes, boundedList, boundedUtf8, box, bytes,
  char, custom, duration, enumeration, f32, f64, field, fixedBytes, generateSchema, i128, i16, i32, i64, i8,
  lazy, list, optional, parseTimeSchemaId, rawString, record, timePoint, tuple, u128, u16, u256, u32, u64, u8,
  utf8, variant, type FieldOptions, type IrAnyType, type IrAttribute, type IrMember, type IrSchema,
  type PsszCustomHandler, type PsszType, type PsszWidth, type RecordOptions,
} from '../../index.js';

type Def = TypeDef<any, any>;
type Handlers = { [id: string]: PsszCustomHandler };

/** One way of getting codecs for the fixture's type ids. */
export interface TypeSource {
  type(id: string, options?: { width?: PsszWidth; requireHandlers?: boolean }): PsszType;
}

class Unsupported extends Error {}
const unsupported = (reason: string): never => {
  throw new Unsupported(reason);
};

const INTS: { [key: string]: Def } = {
  u8, u16, u32, u64, u128, u256, i8, i16, i32, i64, i128,
};

export class TypedFixture implements TypeSource {
  private readonly byName: Map<string, IrMember>;
  private readonly defs = new Map<string, Def>();
  private readonly building = new Set<string>();
  /** Type id -> why the typed layer cannot declare it. */
  readonly unsupported = new Map<string, string>();

  constructor(readonly ir: IrSchema, private readonly handlers: Handlers = {}) {
    this.byName = new Map(ir.types.map((m) => [m.name, m]));
  }

  /** The definition of a named type; throws when it cannot be declared. */
  def(name: string): Def {
    const known = this.defs.get(name);
    if (known) return known;
    const member = this.byName.get(name);
    if (!member) throw new Error(`fixture has no type "${name}"`);
    // A record reached again while it is being declared: a recursive type.
    if (this.building.has(name)) return lazy(() => this.defs.get(name)!);
    this.building.add(name);
    try {
      const def = this.of(member.type, name);
      this.defs.set(name, def);
      return def;
    } finally {
      this.building.delete(name);
    }
  }

  /** The definition, or undefined with the reason recorded in `unsupported`. */
  tryDef(name: string): Def | undefined {
    try {
      return this.def(name);
    } catch (e) {
      if (!(e instanceof Unsupported)) throw e;
      this.unsupported.set(name, e.message);
      return undefined;
    }
  }

  type(id: string, options?: { width?: PsszWidth; requireHandlers?: boolean }): PsszType {
    return this.def(id).type(options) as PsszType;
  }

  private resolved(type: IrAnyType): IrAnyType {
    for (let hops = 0; type.value.type === 'Type' && hops < 64; ++hops) type = this.byName.get(type.value.value.type)!.type;
    return type;
  }

  private isU8(type: IrAnyType): boolean {
    const t = this.resolved(type).value;
    return t.type === 'Int' && t.value.bits === 8 && !t.value.isSigned;
  }

  // `slot`: the name the type is declared under, when it is a named type.
  private of(type: IrAnyType, slot?: string): Def {
    const t = type.value;
    switch (t.type) {
      case 'Type':
        return this.def(t.value.type);
      case 'Int':
        return INTS[`${t.value.isSigned ? 'i' : 'u'}${t.value.bits}`] ?? unsupported(`Int ${t.value.bits}`);
      case 'Float':
        if (t.value.exp === 8 && t.value.mantissa === 24) return f32;
        if (t.value.exp === 11 && t.value.mantissa === 53) return f64;
        return unsupported('a float other than the C++ f32 / f64 spelling');
      case 'Option':
        return optional(this.of(t.value.type));
      case 'List':
        return list(this.of(t.value.type));
      case 'BoundedList':
        return boundedList(this.of(t.value.type), t.value.maxCount);
      case 'Array':
        return array(this.of(t.value.type), t.value.len);
      case 'Tuple':
        return tuple(...t.value.members.map((m) => this.of(m)));
      case 'Variant': {
        if (t.value.attributes.length) return unsupported('variant attributes');
        const alts: { [name: string]: Def | ReturnType<typeof alt> } = {};
        for (const m of t.value.members) {
          const names = m.attributes.filter((a) => a.name === 'ijsonName');
          if (names.length !== m.attributes.length) return unsupported('variant member attributes');
          alts[m.name] = names.length ? alt(this.of(m.type), { ijsonName: names[0].value! }) : this.of(m.type);
        }
        return variant(alts);
      }
      case 'Enum': {
        const members: { [name: string]: bigint } = {};
        const under = this.of(t.value.type);
        const bits = (this.resolved(t.value.type).value as { value: { bits: number; isSigned: boolean } }).value;
        for (const m of t.value.members)
          members[m.name] = bits.isSigned ? BigInt.asIntN(64, m.value) : m.value;
        return enumeration(under, members);
      }
      case 'Object':
        return this.record(slot ?? unsupported('an unnamed record'), t.value.members, t.value.attributes);
      case 'Struct':
        return unsupported('a Struct node (C++ and the typed layer spell a fixed record as Object + definitionWillNotChange)');
      case 'Custom':
        return this.custom(t.value.type, t.value.id, slot);
      default:
        return unsupported(`${t.type} node`);
    }
  }

  private custom(inner: IrAnyType, id: string, slot?: string): Def {
    // An adapter may sit on its container through a named alias.
    const w = this.resolved(inner).value;
    switch (id) {
      case 'bool':
        return bool;
      case 'char':
        return char;
      case 'box':
        return box(this.of(inner));
      case 'bitvector':
        return w.type === 'Array' ? bitvector(Number(w.value.len)) : unsupported('bitvector shape');
      case 'bitlist':
        return w.type === 'BoundedList' ? bitlist(Number(w.value.maxCount)) : unsupported('bitlist shape');
      case 'utf8':
      case 'string':
      case 'hex': {
        if ((w.type !== 'List' && w.type !== 'BoundedList' && w.type !== 'Array') || !this.isU8(w.value.type))
          return unsupported('a byte adapter over something other than u8');
        if (w.type === 'List') return id === 'utf8' ? utf8 : id === 'string' ? rawString : bytes;
        if (w.type === 'BoundedList' && id === 'utf8') return boundedUtf8(Number(w.value.maxCount));
        if (w.type === 'BoundedList' && id === 'hex') return boundedBytes(Number(w.value.maxCount));
        if (w.type === 'Array' && id === 'hex') return fixedBytes(Number(w.value.len));
        return unsupported(`a ${id} adapter over ${w.type} outside a bounded record member`);
      }
      default: {
        const time = parseTimeSchemaId(id);
        if (time) {
          const unit = { num: time.num, den: time.den };
          const tick = this.of(inner);
          return time.point ? timePoint(unit, tick) : duration(unit, tick);
        }
        // A custom type: its text form over the wire type declared inline.
        return custom(id, this.of(inner, slot), this.handlers[id]);
      }
    }
  }

  private record(name: string, members: IrMember[], attributes: IrAttribute[]): Def {
    const options: RecordOptions = {};
    for (const a of attributes) {
      if (a.name === 'definitionWillNotChange') options.fixed = true;
      else if (a.name === 'maxDynamicData') options.maxDynamicData = BigInt(a.value!);
      else if (a.name === 'maxFields') options.maxFields = Number(a.value!);
      else return unsupported(`record attribute ${a.name}`);
    }
    const fields: { [name: string]: Def | ReturnType<typeof field> } = {};
    for (const m of members) {
      const o: FieldOptions = {};
      let def: Def;
      // A member bound written into the type (C++ length_bound{.max}): the
      // only member types that are not references.
      const bounded = m.type.value.type === 'Type' ? null : this.boundedMember(m.type);
      if (bounded) {
        def = bounded.def;
        o.lengthBound = { max: bounded.max };
      } else def = this.of(m.type);
      for (const a of m.attributes) {
        if (a.name === 'jsonInt' || a.name === 'ijsonInt') o[a.name] = a.value as 'number' | 'string';
        else if (a.name === 'lengthBound') {
          const bound: { [key: string]: number } = {};
          for (const item of a.value!.split(',')) bound[item.slice(0, item.indexOf('='))] = Number(item.slice(item.indexOf('=') + 1));
          // A bounded UTF-8 string states its own bound: nothing to declare.
          const direct = this.resolved(m.type).value;
          const inherent = direct.type === 'Custom' && direct.value.id === 'utf8' &&
            direct.value.type.value.type === 'BoundedList' && a.value === `max=${direct.value.type.value.value.maxCount}`;
          if (!inherent) o.lengthBound = bound;
        } else return unsupported(`member attribute ${a.name}`);
      }
      fields[m.name] = Object.keys(o).length ? field(def, o) : def;
    }
    return record(name, fields, options);
  }

  private boundedMember(type: IrAnyType): { def: Def; max: number } | null {
    const t = type.value;
    if (t.type === 'Option') {
      const inner = this.boundedMember(t.value.type);
      return inner && { def: optional(inner.def), max: inner.max };
    }
    if (t.type === 'BoundedList') return { def: list(this.of(t.value.type)), max: Number(t.value.maxCount) };
    if (t.type === 'Custom' && (t.value.id === 'string' || t.value.id === 'hex')) {
      const w = t.value.type.value;
      if (w.type === 'BoundedList' && this.isU8(w.value.type))
        return { def: t.value.id === 'string' ? rawString : bytes, max: Number(w.value.maxCount) };
    }
    return null;
  }

  /**
   * The names C++ passed to `SchemaBuilder::insert<T>(name)`, in order:
   * every entry of the IR that the entries before it did not already bring
   * in. `generateSchema` of these must reproduce the IR.
   */
  insertionOrder(): string[] {
    const names: string[] = [];
    let present = new Set<string>();
    for (const member of this.ir.types) {
      if (present.has(member.name) || member.name.startsWith('@')) continue;
      if (!this.tryDef(member.name)) continue;
      names.push(member.name);
      present = new Set(this.generate(names).ir.types.map((m) => m.name));
    }
    return names;
  }

  /** `generateSchema` of the named definitions, in the given order. */
  generate(names: string[]): PsszSchema {
    const types: { [name: string]: Def } = {};
    for (const name of names) types[name] = this.def(name);
    return generateSchema(types);
  }
}

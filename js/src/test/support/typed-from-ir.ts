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
  lazy, list, map, monotonic, nested, optional, pair, parseTimeSchemaId, rawString, record, set, timePoint, transparent,
  tuple, u128, u16, u256, u32, u64, u8,
  utf8, variant, type FieldOptions, type IrAnyType, type IrAttribute, type IrMember, type IrSchema,
  type PsszCustomHandler, type PsszNestedBytes, type PsszType, type PsszWidth, type RecordOptions,
} from '../../index.js';

type Def = TypeDef<any, any>;
type Handlers = { [id: string]: PsszCustomHandler };

/** One way of getting codecs for the fixture's type ids. */
export interface TypeSource {
  type(id: string, options?: { width?: PsszWidth; requireHandlers?: boolean; nestedBytes?: PsszNestedBytes }): PsszType;
}

// The spelling of a type as the IR writes it, references by name: two
// entries with the same spelling are the same node under two identities.
function spelling(type: IrAnyType): string {
  const t = type.value;
  switch (t.type) {
    case 'Type': return t.value.type;
    case 'Tuple': return `Tuple[${t.value.members.map(spelling).join(',')}]`;
    case 'List': case 'Option': case 'Nested': return `${t.type}<${spelling(t.value.type)}>`;
    case 'BoundedList': return `BoundedList<${spelling(t.value.type)},${t.value.maxCount}>`;
    case 'Custom': return `Custom{${spelling(t.value.type)},${JSON.stringify(t.value.id)}}`;
    default: return '';
  }
}

// Every named type a type refers to.
function references(type: IrAnyType, out: Set<string>): void {
  const t = type.value;
  switch (t.type) {
    case 'Type': out.add(t.value.type); break;
    case 'Tuple': for (const m of t.value.members) references(m, out); break;
    case 'Struct': case 'Object': case 'Variant': for (const m of t.value.members) references(m.type, out); break;
    case 'List': case 'Option': case 'Nested': case 'BoundedList': case 'Array': case 'Custom': case 'Enum':
      references(t.value.type, out);
      break;
    default: break;
  }
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
  // What the IR does not say about an interned (`@N`) node, because C++
  // gives two types the same node (docs/schema-builder.md):
  // - a second 2-tuple of the same spelling is a std::pair (`pair`);
  // - a second map / set of the same spelling is an unordered container
  //   (`{ unordered: true }`);
  // - a nested value whose fixture id says `,deferred>` is deferred.
  private readonly alternate = new Set<string>();
  private readonly deferred = new Set<string>();
  // A named alias that other types refer to (or that precedes its target)
  // is a transparent type, not a name given to `insert<T>(name)`.
  private readonly transparents = new Set<string>();

  constructor(readonly ir: IrSchema, private readonly handlers: Handlers = {}) {
    this.byName = new Map(ir.types.map((m) => [m.name, m]));
    const index = new Map(ir.types.map((m, i) => [m.name, i]));
    const referenced = new Set<string>();
    const mapEntries = new Set<string>();
    for (const m of ir.types) {
      const t = m.type.value;
      // A reference from a root alias is the alias itself, not a use.
      if (t.type !== 'Type') references(m.type, referenced);
      const visit = (type: IrAnyType): void => {
        const v = type.value;
        if (v.type === 'Custom' && v.value.id === 'map') {
          const list = v.value.type.value;
          if ((list.type === 'List' || list.type === 'BoundedList') && list.value.type.value.type === 'Type')
            mapEntries.add(list.value.type.value.value.type);
        }
        if (v.type === 'Object' || v.type === 'Struct' || v.type === 'Variant') for (const x of v.value.members) visit(x.type);
      };
      visit(m.type);
    }
    const groups = new Map<string, string[]>();
    // `@N` is the N-th interned type: the count of the interned entries
    // before it says how many of the named aliases since the previous
    // `@N` are transparent types (interned) and not names given to
    // `insert<T>(name)` (not interned). Which of several unreferenced ones
    // does not change the IR.
    let interned = 0;
    let pending: string[] = [];
    for (const m of ir.types) {
      const t = m.type.value;
      if (m.name.startsWith('@')) {
        const marked = pending.filter((name) => this.transparents.has(name)).length;
        let need = Number(m.name.slice(1)) - interned - marked;
        // A name given to `insert<T>(name)` directly follows the types it
        // brought in; an alias of a type that was there long before is the
        // likelier transparent type.
        const recent = (name: string) => {
          const target = (this.byName.get(name)!.type.value as { value: { type: string } }).value.type;
          let named = index.get(name)! - 1;
          while (named >= 0 && ir.types[named].name.startsWith('@')) --named;
          return (index.get(target) ?? -1) > named;
        };
        for (const name of [...pending.filter((x) => !recent(x)), ...pending.filter(recent)]) {
          if (need <= 0) break;
          if (this.transparents.has(name)) continue;
          this.transparents.add(name);
          --need;
        }
        interned = Number(m.name.slice(1)) + 1;
        pending = [];
      } else if (t.type !== 'Type') ++interned;
      if (t.type === 'Type') {
        if (m.name.startsWith('@')) continue;
        const target = t.value.type;
        if (/^nested<.*,deferred>$/.test(m.name)) this.deferred.add(target);
        else {
          if (referenced.has(m.name) || (index.get(target) ?? -1) > index.get(m.name)!) this.transparents.add(m.name);
          pending.push(m.name);
        }
        continue;
      }
      if (!m.name.startsWith('@')) continue;
      const twoTuple = t.type === 'Tuple' && t.value.members.length === 2;
      const assoc = t.type === 'Custom' && (t.value.id === 'map' || t.value.id === 'set');
      if (!twoTuple && !assoc) continue;
      const key = spelling(m.type);
      groups.set(key, [...(groups.get(key) ?? []), m.name]);
    }
    for (const names of groups.values()) {
      // The entry of a map is the tuple; otherwise the first is.
      const plain = names.find((name) => mapEntries.has(name)) ?? names[0];
      for (const name of names) if (name !== plain) this.alternate.add(name);
    }
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

  type(id: string, options?: { width?: PsszWidth; requireHandlers?: boolean; nestedBytes?: PsszNestedBytes }): PsszType {
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
        // A transparent type: a named alias of its member's type.
        if (slot !== undefined && this.transparents.has(slot)) return transparent(slot, this.def(t.value.type));
        return this.def(t.value.type);
      case 'Nested':
        return slot !== undefined && this.deferred.has(slot)
          ? nested(this.of(t.value.type), { deferred: true })
          : nested(this.of(t.value.type));
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
        if (slot !== undefined && this.alternate.has(slot)) return pair(this.of(t.value.members[0]), this.of(t.value.members[1]));
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
        // An enum sits directly on its integer (through named aliases).
        const under = this.of(this.resolved(t.value.type));
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
      case 'map':
      case 'set': {
        if (w.type !== 'List' && w.type !== 'BoundedList') return unsupported(`a ${id} over ${w.type}`);
        return this.assoc(id, w.value.type, slot !== undefined && this.alternate.has(slot),
          w.type === 'BoundedList' ? w.value.maxCount : undefined);
      }
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
          return time.point ? timePoint(unit, tick) : time.monotonic ? monotonic(unit, tick) : duration(unit, tick);
        }
        // A custom type over a transparent type: the Custom wraps the
        // member's type, under the transparent type's name.
        if (inner.value.type === 'Type') {
          if (slot === undefined || slot.startsWith('@')) return unsupported('a custom type over a reference, not named');
          return custom(id, transparent(slot, this.of(inner)), this.handlers[id]);
        }
        // A custom type: its handler over the wire type declared inline.
        return custom(id, this.of(inner, slot), this.handlers[id]);
      }
    }
  }

  private assoc(id: 'map' | 'set', element: IrAnyType, unordered: boolean, max?: bigint): Def {
    if (id === 'set') return set(this.of(element), { unordered, max });
    const entry = this.resolved(element).value;
    if (entry.type !== 'Tuple' || entry.value.members.length !== 2) return unsupported('a map over something other than 2-tuples');
    return map(this.of(entry.value.members[0]), this.of(entry.value.members[1]), { unordered, max });
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
    if (t.type === 'Custom' && (t.value.id === 'map' || t.value.id === 'set')) {
      const w = t.value.type.value;
      if (w.type !== 'BoundedList') return null;
      return { def: this.assoc(t.value.id, w.value.type, false), max: Number(w.value.maxCount) };
    }
    if (t.type === 'Custom' && (t.value.id === 'string' || t.value.id === 'hex')) {
      const w = t.value.type.value;
      if (w.type === 'BoundedList' && this.isU8(w.value.type))
        return { def: t.value.id === 'string' ? rawString : bytes, max: Number(w.value.maxCount) };
    }
    return null;
  }

  /**
   * The names C++ passed to `SchemaBuilder::insert<T>(name)`, in order. The
   * next one is found from the first entry of the IR that the names so far
   * did not bring in: that entry itself when it is a named type (a record
   * or a transparent type takes its slot before its children), and the name
   * given to it — the alias entry that follows its children — when it is an
   * interned `@N`. `generateSchema` of these must reproduce the IR.
   */
  insertionOrder(): string[] {
    const names: string[] = [];
    const skipped = new Set<string>();
    let present = new Set<string>();
    for (;;) {
      const missing = this.ir.types.find((m) => !present.has(m.name) && !skipped.has(m.name));
      if (!missing) return names;
      let root = missing.name;
      if (root.startsWith('@')) {
        const alias = this.ir.types.find((m) =>
          !m.name.startsWith('@') && !present.has(m.name) && !skipped.has(m.name) &&
          m.type.value.type === 'Type' && m.type.value.value.type === root);
        if (!alias) throw new Error(`no inserted name brings in ${root}`);
        root = alias.name;
      }
      if (!this.tryDef(root)) {
        skipped.add(root);
        skipped.add(missing.name);
        continue;
      }
      names.push(root);
      present = new Set(this.generate(names).ir.types.map((m) => m.name));
      if (!present.has(missing.name)) throw new Error(`inserting ${root} does not bring in ${missing.name}`);
    }
  }

  /** `generateSchema` of the named definitions, in the given order. */
  generate(names: string[]): PsszSchema {
    const types: { [name: string]: Def } = {};
    for (const name of names) types[name] = this.def(name);
    return generateSchema(types);
  }
}

@0xc3a1f6b2d4e59799;
# Everything psio::parse_capnp keeps: capnp_schema_tests.cpp compares the
# layout psio computes from this text with the compiler's, and requires
# emit_capnp(parse_capnp(text)) to declare the same.

using Json = import "/capnp/compat/json.capnp";
using Other = Inner;
const answer :UInt32 = 42;
annotation note(field, struct, group, union, enumerant) :Text;

enum Color { red @0; green @1; blue @2 $Json.name("navy_blue"); }
enum Shuffled { second @1; first @0; third @2; }

struct Inner @0xdeadbeefdeadbeef { a @0 :Int32 = -5; }

struct Scalars {
  b @0 :Bool = true;
  i8 @1 :Int8 = -8;
  u8 @2 :UInt8 = 0xff;
  i16 @3 :Int16 = -300;
  u16 @4 :UInt16 = 65535;
  i32 @5 :Int32 = -70000;
  u32 @6 :UInt32 = 4000000000;
  i64 @7 :Int64 = -9000000000000000000;
  u64 @8 :UInt64 = 18446744073709551615;
  f32 @9 :Float32 = 1.5;
  f64 @10 :Float64 = -2.25e-3;
  nan @11 :Float64 = nan;
  negInf @12 :Float32 = -inf;
  inf @13 :Float64 = inf;
  off @14 :Bool = false;
  zero @15 :Int32 = 0;
  color @16 :Color = blue;
  shuffled @17 :Shuffled = second;
  nothing @18 :Void;
}

struct Outer $note("a struct") {
  id @0 :UInt64 = 7;
  name @1 :Text = "anon" $note("x");
  color @2 :Color = green;
  inner @3 :Inner = (a = 3);
  list @4 :List(Inner);
  union {
    x @5 :Void;
    y @6 :Int16;
    g :group { p @7 :Bool; q @8 :Float32; }
  }
  named :union { a @9 :Text; b @10 :Int8; }
  grp :group $note("a group") {
    m @11 :Int32;
    n @12 :Data = 0x"01 02 ff";
    u :union { s @13 :Void; t @14 :UInt8; }
  }
  late @15 :Float64 = 2.5;
  struct Nested {
    v @0 :Bool = true;
    enum Deep { one @0; two @1; }
    d @1 :Deep = two;
    struct Deeper { w @0 :Outer.Nested; }
  }
  nested @16 :Nested;
  any @17 :AnyPointer;
  f32 @18 :Float32 = -1.5e3;
  other @19 :Other;
  retro @21! :union { r1 @20 :Bool; r2 @22 :Text; }
  maybe :union { none @23 :Void; some @24 :UInt32; }
  snake @25 :Text $Json.name("snake_case_name");
  renamed @26 :Int8 $Json.name("somethingElse");
  lists @27 :List(List(Text)) = [["a"], [], ["b", "c"]];
  bits @28 :List(Bool);
  colors @29 :List(Color) = [red, blue];
  deeper @30 :Nested.Deeper;
  cap @31 :Svc;
  const limit :UInt16 = 9;
}

struct Evolved {
  # Ordinals in another order than the text: members added over time.
  c @3 :UInt8;
  a @0 :UInt64;
  choice :union {
    late @5 :Text;
    first @1 :UInt16;
    second @2 :UInt32;
    grp :group { x @6 :UInt64; y @4 :Bool; }
  }
  b @7 :UInt16;
}

struct OnlyUnion {
  union { number @0 :UInt32; text @1 :Text; nothing @2 :Void; }
}

struct Nesting {
  top :union {
    a :group {
      x @0 :UInt8;
      inner :union { p @1 :UInt32; q @2 :Text; r @3 :UInt64; }
    }
    b @4 :UInt16;
    c :group { y @5 :UInt64; z @6 :Bool; w @7 :Text; }
  }
  tail @8 :UInt8;
}

interface Svc {
  call @0 (a :Int32, b :Text = "x") -> (r :Inner);
  ping @1 ();
  one @2 (x :UInt8) -> (result :Text);
  byStruct @3 Inner -> Inner;
  push @4 (chunk :Data) -> stream;
  struct Options { verbose @0 :Bool; }
  configure @5 (o :Options);
}

interface Extended extends(Svc) {
  more @0 () -> (n :UInt32, m :Text);
}

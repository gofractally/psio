@0xc3a1f6b2d4e59781;
# The schema of the types in capnp_interop_tests.cpp, as psio::capnp lays
# them out: ordinals in member order, a std::variant member as a union with
# one member per alternative, std::optional<scalar> as union {none; some},
# scalar defaults from member initialisers.

struct IoProfile {
  id       @0 :UInt64;
  name     @1 :Text;
  email    @2 :Text;
  age      @3 :UInt32;
  verified @4 :Bool;
}

struct IoItem {
  product   @0 :Text;
  qty       @1 :UInt32;
  unitPrice @2 :Float64;
}

struct IoOrder {
  id       @0 :UInt64;
  customer @1 :IoProfile;
  items    @2 :List(IoItem);
  total    @3 :Float64;
  note     @4 :Text;
}

struct IoMixed {
  flag     @0 :Bool;
  small    @1 :Int8;
  level    @2 :Int16 = 7;
  ratio    @3 :Float32 = 1.5;
  on       @4 :Bool = true;
  payment :union {
    none   @5 :Void;
    cash   @6 :UInt32;
    card   @7 :Text;
    owner  @8 :IoProfile;
    amount @9 :Float64;
  }
  score :union {
    none @10 :Void;
    some @11 :UInt32;
  }
  memo     @12 :Text;
  bits     @13 :List(Bool);
  blob     @14 :Data;
  names    @15 :List(Text);
  grid     @16 :List(List(Int16));
  people   @17 :List(IoProfile);
  wide     @18 :UInt64;
}

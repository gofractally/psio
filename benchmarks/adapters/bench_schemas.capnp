@0x9d628297f01eadea;

# Per-shape Cap'n Proto schemas for the unified psio-vs-externals
# benchmark.  Field declaration order matches psio's reflection
# order; capnp's slot allocator may reorder for optimal packing,
# so wire bytes won't be byte-identical to psio::capnp, but both
# produce VALID capnp wire and the comparison is purely about
# library performance.

struct CpPoint {
   x @0 :Int32;
   y @1 :Int32;
}

struct CpNameRecord {
   account @0 :UInt64;
   limit   @1 :UInt64;
}

# capnp has UInt16 natively
struct CpFlatRecord {
   id     @0 :UInt32;
   label  @1 :Text;
   values @2 :List(UInt16);
}

struct CpRecord {
   id     @0 :UInt32;
   label  @1 :Text;
   values @2 :List(UInt16);
   # capnp doesn't have native optional; use 0 sentinel.
   score  @3 :UInt32;
}

struct CpValidator {
   pubkeyLo          @0 :UInt64;
   pubkeyHi          @1 :UInt64;
   withdrawalLo      @2 :UInt64;
   withdrawalHi      @3 :UInt64;
   effectiveBalance  @4 :UInt64;
   slashed           @5 :Bool;
   activationEpoch   @6 :UInt64;
   exitEpoch         @7 :UInt64;
   withdrawableEpoch @8 :UInt64;
}

struct CpLineItem {
   product   @0 :Text;
   qty       @1 :UInt32;
   unitPrice @2 :Float64;
}

struct CpUserProfile {
   id       @0 :UInt64;
   name     @1 :Text;
   email    @2 :Text;
   age      @3 :UInt32;
   verified @4 :Bool;
}

struct CpOrder {
   id       @0 :UInt64;
   customer @1 :CpUserProfile;
   items    @2 :List(CpLineItem);
   total    @3 :Float64;
   note     @4 :Text;
}

struct CpValidatorList {
   epoch      @0 :UInt64;
   validators @1 :List(CpValidator);
}

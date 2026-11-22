// Every type id the fixture names. Named types first, then the structural
// ones (their id is their shape, computed by `Conf`).
pub fn registry() -> Registry {
    let mut r = Registry::new();
    macro_rules! reg { ($($t:ty),* $(,)?) => { $(register::<$t>(&mut r);)* }; }
    reg!(Color, E16, E32, E64, Scalars);
    reg!(R0, R1, R2, R3, Inner, N1, N2, P1, E1, E2, E3, Bad1, Parent0, Parent1, Parent2, Parent3);
    reg!(C1a, C1b, C1c, C2a, C2b, C2c, I0, I1);
    reg!(Base, Extended, Twice, OddBase, OddExtended, OldOuter, NewOuter, DepthNode);
    reg!(Unit, Empty, Pair, DwncVar, PresMid, PresTail);
    reg!(WidthCheckpoint, WidthRelayBatch, WidthLargeBudget, WidthNarrowEnvelope, WidthReserveParent);
    reg!(WidthGrowV1, WidthGrowV2, WidthGrowParentV1, WidthGrowParentV2);
    reg!(WidthThreshold251, WidthThreshold252, WidthBoundedBatch, WidthStringPair, WidthCappedPair);
    reg!(S247, S248, S65527, S65528, Tiny, NarrowBatch);
    reg!(Mix1x1, Mix1x2, Mix1x4, Mix2x1, Mix2x2, Mix2x4, Mix4x1, Mix4x2, Mix4x4);
    reg!(StrRec, Bounded, BytesRec, EnumRec, TupRec);
    reg!(T16, T300);

    // Structural types.
    reg!(bool, u8, u16, u32, u64, i8, i16, i32, i64, f32, f64, u128, i128, Uint256);
    reg!(Vec<Color>, Vec<E16>, Option<Color>, Option<E32>, V2<Color, E64>, [E32; 2]);
    macro_rules! positions {
        ($($t:ty),*) => { $(reg!(Vec<$t>, Option<$t>, V2<u32, $t>, ($t, Raw), [$t; 2]);)* };
    }
    positions!(R0, R1, R2, R3);
    macro_rules! list_and_optional { ($($t:ty),*) => { $(reg!(Vec<$t>, Option<$t>);)* }; }
    list_and_optional!(N1, N2, P1, E1, E2, E3, C1a, C1b, C1c, C2a, C2b, C2c, I0, I1);
    reg!(Option<DepthNode>);
    reg!(BLeaf, BDirect, BoxRec, BoxCapRec, BoxOptRec, BoxVecRec, BoxVecCapRec, BoxFixedRec, BoxOuter);
    reg!(Box<BLeaf>, Vec<Box<BLeaf>>, Option<Box<BLeaf>>, V2<Box<BLeaf>, u8>, (Box<BLeaf>, u8));
    reg!(Vec<BoxRec>, Option<BoxCapRec>);
    macro_rules! containers { ($($t:ty),*) => { $(reg!(Vec<$t>, [$t; 2], Option<$t>);)* }; }
    containers!(Unit, Empty, Pair, DwncVar);
    reg!(Option<Inner>, Option<Raw>, Option<Vec<u16>>, VarBS, Option<VarBS>, Option<Bits12>);
    reg!([Raw; 2], Option<[Raw; 2]>, [u32; 0], Option<[u32; 0]>);
    reg!(Option<u32>, Option<Option<u32>>, Option<Option<Raw>>);
    reg!(Vec<WidthCheckpoint>, Option<WidthCheckpoint>, V2<WidthCheckpoint, u32>, [WidthCheckpoint; 2]);
    reg!(Vec<Tiny>, V2<u32, C1a>, (C1a, Raw));
    reg!(Raw, String, BoundedString<4>, Vec<Raw>, Vec<String>, Vec<BoundedString<4>>);
    reg!(Option<String>, Option<BoundedString<4>>);
    reg!(Vec<u8>, [u8; 4], Bitvector<12>, Bits12, Vec<u32>, [bool; 2]);
    reg!((), (u32,), (Raw,), TupBS, (u32, Raw, Option<u16>), (u8, Option<u32>, Option<Raw>));
    reg!(Vec<TupBS>, V2<(u8, Option<u16>), u32>, VarUS, Var5);
    r
}

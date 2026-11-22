# C++ schema graph generation

SchemaBuilder emits the actual ordered schema IR for plain tuples (including
zero fields), variants, bounded collections, UTF-8 wrappers and recursive Box
graphs. Plain tuples use Tuple IR, never a numeric-name Object surrogate.
`psio::bounded<T,N>` uses BoundedList; `utf8_string<N>` uses the string adapter
over BoundedList<u8,N>.

An unlabeled `std::variant` emits alternatives named "0", "1", and so on, in
declaration order. These names add no payload bytes; the wire selector remains
the u8 alternative index. A shared C++/Rust SDK type must select matching declared
labels when the API schema needs named alternatives. C++ type names are not
inferred to be labels. Customize metadata explicitly:

```cpp
using Choice = std::variant<std::uint16_t, bool>;
template <> struct psio::schema_variant_labels<Choice>
{
   static constexpr std::array<std::string_view, 2> names{"Code", "Ready"};
};
```

Labels must be nonempty, unique, and match the alternative count; invalid
customizations fail compilation. Reordering alternatives changes the wire ABI.

Bit metadata uses existing IR alternatives: Custom "bitvector" wraps
Array<Int1 unsigned,N>, and Custom "bitlist" wraps
BoundedList<Int1 unsigned,maxCount>. These preserve the logical bit counts,
which a byte-array schema alone cannot express. Their dedicated runtime Bits
adapters must be qualified before adopting generated bit schemas; the metadata
does not imply that a plain Array<Int1> itself uses packed bits.

Box is transparent in the type graph. Interning registers each type before
walking children, so recursive records/lists/options point back to the existing
definition. Private type identity uses per-type inline address tokens within one
linked image; RTTI is unnecessary. Addresses are never serialized, and no
cross-DSO identity API is promised. Output names/order retain the existing
reflection-name/@N policy. A two-translation-unit consumer verifies repeated,
nested generic and recursive interning with exceptions and RTTI disabled.

Unknown type shapes continue to emit an explicit unknown adapter that checked
runtime codecs reject. Arbitrary custom adapter contracts and final width/profile
selection remain separate qualification gates.

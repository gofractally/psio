#!/usr/bin/env python3
"""Checks psio::capnp::layout<T> against the official compiler.

Generates random record types (scalars of every size, Text, lists, unions
with members of mixed sizes, optional scalars, shuffled field numbers),
writes them once as C++ (PSIO_REFLECT) and once as .capnp, asks
`capnp compile -ocapnp` for the layout and the C++ program for psio's, and
compares slot by slot.

    layout_fuzz.py --cxx <compiler> --include <psio include dir> \
                   [--seed N] [--structs N] [--work DIR] [--emit-fixture FILE]

--emit-fixture writes the generated types and the OFFICIAL layout as a C++
header of static_asserts (tests/capnp_layout_fixture.hpp is one such run),
so the check also runs where the capnp tool is not installed.
"""
import argparse, os, random, re, subprocess, sys

SCALARS = [("bool", "Bool", 1), ("std::int8_t", "Int8", 8), ("std::uint8_t", "UInt8", 8),
           ("std::int16_t", "Int16", 16), ("std::uint16_t", "UInt16", 16),
           ("std::int32_t", "Int32", 32), ("std::uint32_t", "UInt32", 32),
           ("std::int64_t", "Int64", 64), ("std::uint64_t", "UInt64", 64),
           ("float", "Float32", 32), ("double", "Float64", 64)]
POINTERS = [("std::string", "Text"), ("std::vector<std::uint8_t>", "Data"),
            ("std::vector<std::int32_t>", "List(Int32)"), ("std::vector<std::string>", "List(Text)")]


def gen_struct(rng, name):
    """Returns (members, cxx, capnp). members: list of dicts in declaration order."""
    n = rng.randint(1, 9)
    members = []
    for i in range(n):
        r = rng.random()
        if r < 0.45:
            c, k, _ = rng.choice(SCALARS)
            members.append({"kind": "plain", "cxx": c, "capnp": k})
        elif r < 0.60:
            c, k = rng.choice(POINTERS)
            members.append({"kind": "plain", "cxx": c, "capnp": k})
        elif r < 0.72:
            c, k, _ = rng.choice(SCALARS)
            members.append({"kind": "union", "cxx": "std::optional<%s>" % c,
                            "alts": [("std::monostate", "Void"), (c, k)], "names": ["none", "some"]})
        elif r < 0.80:
            c, k = rng.choice(POINTERS)
            members.append({"kind": "plain", "cxx": "std::optional<%s>" % c, "capnp": k})
        else:
            alts = []
            for _ in range(rng.randint(2, 6)):
                q = rng.random()
                if q < 0.15:
                    alts.append(("std::monostate", "Void"))
                elif q < 0.8:
                    c, k, _ = rng.choice(SCALARS)
                    alts.append((c, k))
                else:
                    alts.append(rng.choice(POINTERS))
            members.append({"kind": "union", "cxx": "std::variant<%s>" % ", ".join(a[0] for a in alts),
                            "alts": alts, "names": ["a%d" % j for j in range(len(alts))]})
    # Field numbers: declaration order, or (one time in three) shuffled.
    numbers = list(range(1, n + 1))
    explicit = rng.random() < 0.34
    if explicit:
        numbers = rng.sample(range(1, 3 * n + 1), n)
    for m, num in zip(members, numbers):
        m["number"] = num
    cxx = ["struct %s\n{" % name]
    for i, m in enumerate(members):
        cxx.append("   %s m%d{};" % (m["cxx"], i))
    cxx.append("};")
    refl = []
    for i, m in enumerate(members):
        refl.append("attr(m%d, field<%d>)" % (i, m["number"]) if explicit else "m%d" % i)
    cxx.append("PSIO_REFLECT(%s, %s)" % (name, ", ".join(refl)))
    # capnp: ordinals in ascending field number.
    order = sorted(range(n), key=lambda i: (members[i]["number"], i))
    ordinal = 0
    decl = {}
    for i in order:
        m = members[i]
        if m["kind"] == "plain":
            decl[i] = "  m%d @%d :%s;" % (i, ordinal, m["capnp"])
            ordinal += 1
        else:
            lines = ["  m%d :union {" % i]
            for nm, (_, k) in zip(m["names"], m["alts"]):
                lines.append("    %s @%d :%s;" % (nm, ordinal, k))
                ordinal += 1
            lines.append("  }")
            decl[i] = "\n".join(lines)
    capnp = ["struct %s {" % name] + [decl[i] for i in range(n)] + ["}"]
    return members, "\n".join(cxx), "\n".join(capnp)


def official_layout(work, capnp_text):
    """{struct: (data_words, ptrs, {member: slot-or-(tag, [alt slots])})}"""
    path = os.path.join(work, "fuzz.capnp")
    with open(path, "w") as f:
        f.write("@0xd0c5e3f0a1b2c3d4;\n" + capnp_text + "\n")
    out = subprocess.run(["capnp", "compile", "-ocapnp", path], check=True, capture_output=True,
                         text=True).stdout
    res, cur, member = {}, None, None

    def slot(comment):
        m = re.search(r"bits\[(\d+), (\d+)\)", comment)
        if m:
            a, b = int(m.group(1)), int(m.group(2))
            return "void" if a == b else "bits[%d,%d)" % (a, b)
        m = re.search(r"ptr\[(\d+)\]", comment)
        return "ptr[%s]" % m.group(1)

    for line in out.splitlines():
        m = re.match(r"struct (\w+) @0x[0-9a-f]+ \{\s+# (\d+) bytes, (\d+) ptrs", line)
        if m:
            cur = res[m.group(1)] = [int(m.group(2)) // 8, int(m.group(3)), {}]
            continue
        if cur is None:
            continue
        m = re.match(r"\s+(m\d+) :group \{", line)
        if m:
            member = m.group(1)
            cur[2][member] = [None, []]
            continue
        m = re.match(r"\s+union \{\s+# tag bits \[(\d+), (\d+)\)", line)
        if m:
            cur[2][member][0] = "bits[%s,%s)" % (m.group(1), m.group(2))
            continue
        m = re.match(r"\s+(\w+) @\d+ :[^;]+;\s+# (.*)", line)
        if m:
            if "union tag" in m.group(2):
                cur[2][member][1].append(slot(m.group(2)))
            else:
                cur[2][m.group(1)] = slot(m.group(2))
    return res


DRIVER = r'''
#include <psio/capnp.hpp>
#include <cstdio>
#include <optional>
#include <string>
#include <variant>
#include <vector>
using psio::field;
%s
template <typename T>
void dump(const char* name)
{
   using L = psio::capnp::layout<T>;
   using namespace psio::detail::capnp_impl;
   auto show = [](slot s) {
      if (s.kind == slot_kind::none) std::printf("void");
      else if (s.kind == slot_kind::pointer) std::printf("ptr[%%u]", s.offset);
      else std::printf("bits[%%u,%%u)", s.offset << s.lg, (s.offset + 1) << s.lg);
   };
   std::printf("%%s %%u %%u\n", name, unsigned(L::data_words), unsigned(L::ptr_count));
   [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (([&] {
          std::printf(" m%%zu ", Is);
          show(L::field(Is));
          const std::size_t n = union_alts<typename L::template member<Is>>::count;
          for (std::size_t a = 0; a < n; ++a) { std::printf(" "); show(L::alt(Is, a)); }
          std::printf("\n");
       }()), ...);
   }(std::make_index_sequence<L::members>{});
}
int main()
{
%s
}
'''


def psio_layout(args, cxx_text, names):
    src = os.path.join(args.work, "fuzz.cpp")
    exe = os.path.join(args.work, "fuzz")
    with open(src, "w") as f:
        f.write(DRIVER % (cxx_text, "\n".join('   dump<%s>("%s");' % (n, n) for n in names)))
    subprocess.run([args.cxx, "-std=gnu++23", "-O0", "-I", args.include, "-isystem",
                    "/opt/homebrew/include", src, "-o", exe], check=True)
    out = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    res, cur = {}, None
    for line in out.splitlines():
        if not line.startswith(" "):
            name, dw, pc = line.split()
            cur = res[name] = [int(dw), int(pc), {}]
        else:
            parts = line.split()
            cur[2][parts[0]] = parts[1] if len(parts) == 2 else [parts[1], parts[2:]]
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--include", required=True)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--structs", type=int, default=60)
    ap.add_argument("--work", default="/tmp/psio-capnp-layout-fuzz")
    ap.add_argument("--emit-fixture")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    rng = random.Random(args.seed)
    names, cxx, capnp = [], [], []
    for i in range(args.structs):
        name = "S%d" % i
        _, c, k = gen_struct(rng, name)
        names.append(name)
        cxx.append(c)
        capnp.append(k)
    want = official_layout(args.work, "\n".join(capnp))
    got = psio_layout(args, "\n".join(cxx), names)
    bad = 0
    for n in names:
        if want[n] != got[n]:
            bad += 1
            print("MISMATCH %s\n  capnp: %r\n  psio:  %r" % (n, want[n], got[n]))
    print("%d structs, %d mismatches (seed %d)" % (len(names), bad, args.seed))
    if args.emit_fixture and not bad:
        with open(args.emit_fixture, "w") as f:
            f.write("// Generated by tests/capnp/layout_fuzz.py --seed %d --structs %d.\n"
                    "// The expected values are the OFFICIAL compiler's (capnp compile -ocapnp).\n"
                    "// clang-format off\n" % (args.seed, args.structs))
            f.write("namespace capnp_layout_fixture {\nusing psio::field;\n")
            f.write("\n".join(cxx) + "\n")
            f.write("struct expected { const char* name; unsigned data_words, ptrs; const char* slots; };\n")
            f.write("template <typename T> std::string slots_of();\n")
            f.write("#define CAPNP_LAYOUT_FIXTURE(X) \\\n")
            for n in names:
                slots = []
                for i in range(len(want[n][2])):
                    v = want[n][2]["m%d" % i]
                    slots.append(v if isinstance(v, str) else " ".join([v[0]] + v[1]))
                f.write('   X(%s, %d, %d, "%s") \\\n' % (n, want[n][0], want[n][1], ";".join(slots)))
            f.write("\n}  // namespace capnp_layout_fixture\n")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

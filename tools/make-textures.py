#!/usr/bin/env python3
"""Generate src/textures.h from the block art below.

Blocks used to be a flat colour per material with one shared 8x8 noise tile
modulating brightness over it — which world.h was careful to call "not a
texture", because it was not one: the tile carried no colour of its own and was
indexed by SCREEN ROW rather than by a position on the block's face, so it read
as grain rather than as a surface.

This makes it a real texture without making it cost anything. A texel is still
one byte and still an index, exactly as before; what changes is what it indexes.
It used to pick one of four brightness steps of the material's single colour. It
now picks one of TEXELS authored colours, and render.cpp runs each of those
through the same distance/torch/daylight shading it already ran the single
colour through. The inner loop is the same two loads it always was.

That is the difference between grain and a texture: an amplitude ramp of one
colour cannot draw a grass side, because a grass side is brown with a green lip
on it. Two hues is the whole point.

    ./tools/make-textures.py           # rewrite src/textures.h
    ./tools/make-textures.py --show    # unpack the result and print it back

Encoding: one byte per texel, palette index 0..TEXELS-1, stored COLUMN-major as
[u][v] so a wall span — which is one column of the face, top to bottom — reads
sixteen contiguous bytes. There is no transparency: a block face is opaque.

House style for the art:
  glyphs are '0'..'7', the palette index. Low is dark, high is light, so the
  art reads as a picture of the block rather than as arbitrary symbols.
  Palette entry 0 MUST equal the material's colour in world.cpp's kInfo table.
  It is what a block fades to at distance, what its top face is filled with,
  and what its break-particles are made of, so a texture whose entry 0 has
  drifted would make near and far versions of the same block disagree.
  The tile wraps: column 15 sits against column 0 on the next face round, and
  row 15 against row 0 of the block below. Seams show.
"""

import argparse
import re
import sys
from pathlib import Path

TEX_N = 16      # texels across a face
TEXELS = 8      # authored colours per material

# ---- palettes ---------------------------------------------------------------
#
# Entry 0 is checked against world.cpp at build time. The rest are free, and
# are the reason a block can have more than one hue in it.
PALETTES = {
    "grass":   [(82,168,62),(68,148,50),(98,188,76),(58,132,44),
                (140,92,50),(120,76,40),(158,108,62),(104,68,36)],
    "dirt":    [(140,92,50),(122,80,44),(158,106,60),(112,72,38),
                (148,100,56),(132,86,46),(166,114,68),(104,66,34)],
    "stone":   [(128,132,138),(112,116,122),(144,148,154),(102,106,112),
                (136,140,146),(120,124,130),(154,158,164),(94,98,104)],
    "wood":    [(92,60,36),(78,50,30),(108,72,44),(66,42,24),
                (100,66,40),(84,54,32),(118,80,50),(58,36,20)],
    "leaves":  [(46,120,44),(36,100,36),(58,140,54),(28,84,28),
                (52,130,48),(40,110,40),(68,152,62),(22,70,22)],
    "coal":    [(78,80,88),(96,100,108),(64,66,72),(22,22,26),
                (36,36,42),(88,92,100),(14,14,18),(108,112,120)],
    "iron":    [(192,180,166),(120,124,130),(136,140,146),(214,198,168),
                (232,216,186),(108,112,118),(200,186,158),(150,154,160)],
    "sand":    [(228,208,148),(216,196,138),(240,220,160),(206,186,130),
                (234,214,154),(222,202,144),(246,228,172),(198,178,124)],
    "snow":    [(236,242,250),(228,234,244),(246,250,255),(220,228,238),
                (240,246,252),(232,238,246),(250,253,255),(214,222,234)],
    "brick":   [(168,96,84),(150,84,74),(186,110,96),(132,72,64),
                (176,102,88),(158,90,78),(196,120,104),(118,62,56)],
    "plank":   [(206,152,88),(190,138,78),(222,168,102),(172,122,66),
                (212,158,94),(196,144,82),(232,180,114),(158,110,58)],
    "masonry": [(96,104,122),(84,92,108),(110,118,136),(72,78,94),
                (102,110,128),(90,98,114),(122,130,148),(64,70,84)],
    "torch":   [(252,178,56),(236,158,44),(255,204,96),(214,136,34),
                (248,170,50),(240,164,46),(255,222,140),(196,120,28)],
    "lava":    [(238,96,30),(214,74,22),(252,132,48),(186,56,16),
                (244,110,36),(226,86,26),(255,170,70),(160,42,12)],
    "bedrock": [(44,46,52),(34,36,42),(56,58,66),(26,28,32),
                (48,50,58),(38,40,46),(66,68,78),(20,22,26)],
}

# ---- the art ----------------------------------------------------------------

TEXTURES = {
    # Dirt with a green lip: the single most recognisable block face in the
    # genre, and the one thing a brightness ramp of one colour cannot draw.
    "grass": """
        0201020102010201
        1020120102101020
        2012021020120102
        0132103201320132
        3243424334243243
        4546454745464547
        5474654754765456
        4657476545674654
        7454657474546574
        4765474657465476
        5474765454767454
        7546454776454765
        4657476545474654
        5474654757465476
        4745476454756547
        4567454765474654
    """,
    "dirt": """
        0416021740162047
        4102471604270163
        1640163247061524
        0247104627314065
        4016247063140627
        2471603247160432
        1604273140652471
        6247016432704165
        0163247160432716
        4270631647025140
        1647032471605243
        6403271604732160
        2716043270165427
        7160432716047031
        0432716043271604
        3271604327160432
    """,
    # Cobble: irregular patches with dark mortar between them, which is what
    # separates stone from every other grey in the palette.
    "stone": """
        3333333333333333
        3011203300142033
        3120410330204103
        3204120330412001
        3011203330120420
        3333333333333333
        0330412033301120
        4103302041033020
        1203304120330412
        0420330112033012
        3333333333333333
        3011203300142033
        3120410330204103
        3204120330412001
        3011203330120420
        3333333333333333
    """,
    # Vertical grain with a couple of knots: a trunk, seen from the side.
    "wood": """
        1042031420431042
        1042731420431042
        1042031473431042
        1042031420431042
        1047431420431042
        1042031420431742
        1042031420431042
        1042031420431042
        1042031420437042
        1042731420431042
        1042031420431042
        1042031473431042
        1042031420431042
        1047431420431042
        1042031420431742
        1042031420431042
    """,
    # Dense, holed, and darker than grass. The holes are what stop a canopy
    # reading as a solid green cube.
    "leaves": """
        0312703124031270
        3170312703170312
        1203170312031703
        7031270312703127
        0317031203170312
        3120317031270317
        1703127031703120
        0312031703120317
        3127031270317031
        1203170312031270
        7031270312703103
        0317031203170312
        3170317031203170
        1270312703127031
        0312031703120317
        3120317031270312
    """,
    # Stone shot through with black. The base is the stone pattern so the ore
    # reads as being IN the rock rather than painted on it.
    "coal": """
        2201120220142022
        2336410220204103
        2364120336412001
        2331203630120420
        2201203330120422
        2222122222201222
        0223412033634120
        4133362041033620
        1263336120330412
        0426330112033612
        2222222222122222
        2011263300142022
        2120610336204103
        2204120330412001
        2011203330120420
        2222222222222222
    """,
    # ...and the same rock shot through with something pale.
    "iron": """
        1122112211421122
        1236412211224122
        1264122336412221
        1231224634122421
        1121224434122422
        1122122211221222
        2123412211634122
        4123362241123624
        1263334122142412
        2426112212234612
        1122122212122122
        2211263412142122
        1124614236224123
        1224122412412221
        2211224412122421
        1122112211221122
    """,
    # Fine and almost smooth: a dune should not look like gravel.
    "sand": """
        0102010201020102
        1020102010201020
        0201020102010201
        2010201020102010
        0102010601020102
        1020102010201020
        0201020102010201
        2010201020102010
        0102010201020102
        1020102010207020
        0201020102010201
        2010201020102010
        0102010201020102
        1060102010201020
        0201020102010201
        2010201020102010
    """,
    # Smoother still. Snow is the one material that should read as almost flat.
    "snow": """
        0002000200020002
        0200020002000200
        0020002000200020
        2000200020002006
        0002000200020002
        0200060002000200
        0020002000200020
        2000200020002000
        0002000200020002
        0200020002000200
        0020006000200020
        2000200020002000
        0002000200020002
        0200020002000600
        0020002000200020
        2000200020002000
    """,
    # Courses with staggered joints — the pattern that makes brick read as
    # brick at any distance, and the reason the mortar row is a whole texel.
    "brick": """
        3333333333333333
        0240240240243024
        4024024024024302
        2402402402402430
        3333333333333333
        4024302402402402
        0243024024024024
        2430240240240240
        3333333333333333
        0240240240243024
        4024024024024302
        2402402402402430
        3333333333333333
        4024302402402402
        0243024024024024
        2430240240240240
    """,
    # Milled boards: long horizontal seams, and a vertical butt joint that does
    # not line up with the one above it.
    "plank": """
        2402402462402402
        0246024024024624
        3333333333333333
        4024624024024024
        2460240240246024
        3333333333333333
        0240246024024024
        2402402402462402
        3333333333333333
        4624024024024624
        0240240246024024
        3333333333333333
        2402462402402402
        0246024024024624
        3333333333333333
        4024024624024024
    """,
    # Cut stone: big regular blocks, unlike B_STONE's rubble. This is what a
    # tower is made of, and it should look built rather than quarried.
    "masonry": """
        3333333333333333
        3011203330112033
        3120410331204103
        3204120332041203
        3011203330112033
        3333333333333333
        3011203330112033
        3120410331204103
        3204120332041203
        3011203330112033
        3333333333333333
        3011203330112033
        3120410331204103
        3204120332041203
        3011203330112033
        3333333333333333
    """,
    "torch": """
        0621062106210621
        6210621062106210
        2106210621062106
        1062106210621062
        0621062106210621
        6210621062106210
        2106210621062106
        1062106210621062
        0621062106210621
        6210621062106210
        2106210621062106
        1062106210621062
        0621062106210621
        6210621062106210
        2106210621062106
        1062106210621062
    """,
    "lava": """
        0246402064024640
        2464020640246402
        4640206402464020
        6402064024640206
        4020640246402064
        0206402464020640
        2064024640206402
        0640246402064024
        6402464020640246
        4024640206402464
        0246402064024640
        2464020640246402
        4640206402464020
        6402064024640206
        4020640246402064
        0206402464020640
    """,
    "bedrock": """
        3170312703170312
        1703127031270317
        7031270312703120
        0312703127031270
        3127031270312703
        1270312703127031
        2703127031270312
        7031270312703127
        0317031270317031
        3170312703170312
        1703127031270317
        7031270312703120
        0312703127031270
        3127031270312703
        1270312703127031
        2703127031270312
    """,
}

# ---- top faces --------------------------------------------------------------
#
# A material only appears here if its top face is a different surface from its
# sides. Most are not: stone is stone whichever way you look at it, and giving
# every material a second tile would double the texel table for nothing.
#
# Grass is the reason this exists. Its side art is dirt with a green lip, which
# is exactly right for a wall and completely wrong seen from above — the floor
# sampler indexes the same tile by world XY, so the lip came out as a green
# stripe every sixteen texels across the ground. The top of a grass block is
# grass all over, which is what it looks like in the genre this borrows from.
#
# The palette is shared with the side face, so a top tile costs no palette
# entries: grass entries 0..3 are already the four greens the lip is drawn
# with, and this uses only those.
TOP_TEXTURES = {
    "grass": """
        0212003102130021
        2001320210023120
        0120210033201002
        1302003120102310
        0021130202310021
        2130020013002132
        0203201021230210
        1020013202001023
        0312200130212001
        2001032021003210
        0230210012302102
        1002301203010230
        0121002130201021
        3200213002130302
        0013020210023210
        2102301021301020
    """,
}


# The order they are emitted in, which must match world::Block.
ORDER = ["grass", "dirt", "stone", "wood", "leaves", "coal", "iron", "sand",
         "snow", "brick", "plank", "masonry", "torch", "lava", "bedrock"]


def parse(name, art):
    """ASCII art -> [v][u] indices, validating shape and glyphs as it goes."""
    rows = [ln.strip() for ln in art.strip().splitlines()]
    if len(rows) != TEX_N:
        sys.exit(f"{name}: {len(rows)} rows, expected {TEX_N}")
    grid = []
    for y, row in enumerate(rows):
        if len(row) != TEX_N:
            sys.exit(f"{name}: row {y} is {len(row)} wide, expected {TEX_N}")
        out = []
        for x, ch in enumerate(row):
            if not ch.isdigit() or int(ch) >= TEXELS:
                sys.exit(f"{name}: row {y} col {x}: '{ch}' is not 0..{TEXELS - 1}")
            out.append(int(ch))
        grid.append(out)
    return grid


def check_entry0(root):
    """Palette entry 0 has to be the material's colour in world.cpp.

    It is what the block fades to at distance, what fills its top face, and
    what its break-particles are made of. If it drifts, near and far versions
    of the same block stop agreeing and nothing in the build would say so.
    """
    src = (root / "src" / "world.cpp").read_text()
    table = re.search(r"kInfo\[B_COUNT\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not table:
        sys.exit("could not find kInfo[] in src/world.cpp")
    found = {}
    for m in re.finditer(r'\{\s*"(\w+)",\s*(\d+),\s*(\d+),\s*(\d+),', table.group(1)):
        found[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    bad = []
    for name in ORDER:
        want = found.get(name)
        if want is None:
            sys.exit(f"{name} is not in world.cpp's kInfo table")
        if PALETTES[name][0] != want:
            bad.append(f"  {name}: entry 0 is {PALETTES[name][0]}, world.cpp says {want}")
    if bad:
        sys.exit("palette entry 0 has drifted from world.cpp:\n" + "\n".join(bad))


def emit(root):
    check_entry0(root)
    grids = {n: parse(n, TEXTURES[n]) for n in ORDER}
    tops = [n for n in ORDER if n in TOP_TEXTURES]
    top_grids = {n: parse(n + " (top)", TOP_TEXTURES[n]) for n in tops}

    L = []
    L.append("// GENERATED by tools/make-textures.py — do not edit by hand.")
    L.append("//")
    L.append("// One byte per texel, an index into the material's palette below.")
    L.append("// Stored COLUMN-major as [material][u][v]: a wall span is one column of")
    L.append("// the face read top to bottom, so this makes it sixteen contiguous bytes.")
    L.append("#pragma once")
    L.append("")
    L.append("#include <stdint.h>")
    L.append("")
    L.append("namespace textures {")
    L.append("")
    L.append(f"constexpr int TEX_N  = {TEX_N};   // texels across a face")
    L.append(f"constexpr int TEXELS = {TEXELS};   // authored colours per material")
    L.append("")
    L.append("// Palette entry 0 is the material's colour from world.cpp's kInfo table;")
    L.append("// the generator refuses to run if the two have drifted apart.")
    L.append(f"static const uint8_t kTexPal[{len(ORDER)}][TEXELS][3] = {{")
    for name in ORDER:
        entries = ", ".join("{%3d,%3d,%3d}" % c for c in PALETTES[name])
        L.append(f"  {{ {entries} }},   // {name}")
    L.append("};")
    L.append("")
    L.append(f"static const uint8_t kTexel[{len(ORDER)}][TEX_N][TEX_N] = {{")
    for name in ORDER:
        g = grids[name]
        L.append(f"  {{  // {name}")
        for u in range(TEX_N):
            col = ",".join(str(g[v][u]) for v in range(TEX_N))
            L.append(f"    {{ {col} }},")
        L.append("  },")
    L.append("};")
    L.append("")
    L.append("// Top faces, for the materials whose top is not their side.")
    L.append("//")
    L.append("// kTopOf indexes kTopTexel, or is TOP_NONE where the material's own tile")
    L.append("// serves both. Sparse on purpose: a second tile for every material would")
    L.append("// double the texel table, and the texels live in SRAM because reading them")
    L.append("// from flash cost milliseconds a frame.")
    L.append(f"constexpr int TOP_N    = {len(tops)};")
    L.append("constexpr uint8_t TOP_NONE = 255;")
    if tops:
        L.append(f"static const uint8_t kTopTexel[TOP_N][TEX_N][TEX_N] = {{")
        for name in tops:
            g = top_grids[name]
            L.append(f"  {{  // {name}")
            for u in range(TEX_N):
                col = ",".join(str(g[v][u]) for v in range(TEX_N))
                L.append(f"    {{ {col} }},")
            L.append("  },")
        L.append("};")
    else:
        L.append("static const uint8_t kTopTexel[1][TEX_N][TEX_N] = {};")
    idx = {n: i for i, n in enumerate(tops)}
    entries = ", ".join(
        (str(idx[n]) if n in idx else "TOP_NONE") for n in ORDER)
    L.append(f"static const uint8_t kTopOf[{len(ORDER)}] = {{ {entries} }};")
    L.append("")
    L.append("}  // namespace textures")
    L.append("")

    out = root / "src" / "textures.h"
    out.write_text("\n".join(L))
    print(f"wrote {out} — {len(ORDER)} materials, "
          f"{(len(ORDER) + len(tops)) * TEX_N * TEX_N} texels "          f"({len(tops)} top faces), "
          f"{(len(ORDER) + len(tops)) * TEX_N * TEX_N + len(ORDER) * TEXELS * 3} bytes of flash")


def show():
    for name in ORDER:
        g = parse(name, TEXTURES[name])
        print(f"\n{name}:")
        for row in g:
            print("    " + "".join(str(v) for v in row))
        if name in TOP_TEXTURES:
            print(f"{name} (top):")
            for row in parse(name + " (top)", TOP_TEXTURES[name]):
                print("    " + "".join(str(v) for v in row))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--show", action="store_true",
                    help="print the parsed art back instead of writing the header")
    ap.add_argument("--out", default=None, help="repository root")
    a = ap.parse_args()
    root = Path(a.out) if a.out else Path(__file__).resolve().parent.parent
    if a.show:
        show()
    else:
        emit(root)


if __name__ == "__main__":
    main()

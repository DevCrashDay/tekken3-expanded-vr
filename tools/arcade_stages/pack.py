#!/usr/bin/env python3
"""Arcade Stages, step E2: one binary file per arcade stage, for the runtime (E4).

    python tools/arcade_stages/pack.py [--stages 0-12] [--check]

Writes workspace/arcade-stages/pack/stageNN.ast from the user's tekken3.zip
(never committed). --check reloads every file and compares the commands it
gives with the decoder's for every MAME dump (validate.py captures): the file
holds everything the stage drawing needs.

Format, version FORMAT (TIPS section 91), little-endian, offsets from the
start of the file, every section 4-byte aligned:

  header   char[4] 'ASTG', u16 version, u16 stage, u32 sections, u32 file size
  table    sections x (char[4] id, u32 offset, u32 size)

  VRAM  u32 n; n x (u16 x, y, w, h, then w * h u16): rectangles for
        psx_mod_gpu_high_vram_upload, all in y 512..1023: the textures at their
        System 12 place (stage, objects, animation frames), then the palettes
        moved up from rows 96..224 (one rectangle per run of a row).
  CLUT  u32 n; n x (u16 old x, old y, new x, new y, width, 0): the palette
        relocation (format 2). Every CLUT word in this file is already the
        new one: (new y << 6 | new x / 16), bit 15 set (E3: y + 512).
  TILE  u32 tile size, u32 tiles (256, 16 x 16, row z then column x);
        per tile: u8 OT slot (1..15), u8 OT B, u8 skipped by the game, u8 0,
        u16 vertices, u16 primitives, then vertices (s16 x, y, z, 0: world / 10,
        y down) and primitives (PRIM). The game draws all the tiles with one
        matrix; in a slot, the last primitive of the last tile comes first.
  PRIM  40 bytes: u32 TMD header (decode.KINDS), u8 kind (0 FT3, 1 FT4, 2 F4,
        3 GT3, 4 GT4, 5 G3, 6 G4), u8 GPU code, u8 third culling vertex (2, 3),
        u8 colours given (0, 1, 3, 4), u8 vertex index x 4, u8 (u, v) x 4,
        u16 CLUT, u16 tpage (System 12: bit 11 = y + 512), u8 (r, g, b) x 4
        (0x80 when not given), u8 texture window x, y, w, h (all 0: none).
  FARB  s16 columns, rows, ring, y offset, degrees, pitch factor (low
        resolution), u8 cell rgb[3], top rgb[3], top on, bottom rgb[3],
        bottom on, 0; then u16 cell textures rows x columns, u16 CLUTs rows x
        columns, u8 kinds rows x ring (padded).
  FLOR  s16 kind, fog, s32 near; (u32 w0, w1) x 100 cells (row-major,
        10 x 10), x 400 half cells (20 x 20); u8 texture index x 100 (padded);
        u16 vertices, (s16 x, z) x n; per cell (grid order) u8 sub-quads
        (padded), then u32 four vertex indices per sub-quad; corner uv: for
        1, 2, 4 sub-quads, 4 rotations x n x 4 u16 (u | v << 8).
  TRIG  s16 sine x 4096, cosine x 4096 (4.12), SquareRoot0 table x 192,
        u8 fog ramp x 1024, s16 0x801AEC2C square root table x 192.
  OBJS  (stages 3, 11) u32 models; per model u16 vertices, u16 primitives,
        vertices, PRIM records; then u32 n, n x (u8 model, u8 0, u16 main OT
        rank, u16 private OT size, u16 0) in drawing call order.
  ANIM  (stages 6, 12) u16 destination x, y, w, h, u16 period (frames), u16
        n; n x (u16 x, y) source frames in VRAM.
  ROTT  (stages 3, 11; format 3) u32 x 4096: the objects' rotation table
        0x8021688C (sine | cosine << 16, 4.12), RotMatrix / RotMatrixYXZ.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools/arcade_stages')]
import decode  # noqa: E402
import scene  # noqa: E402

FORMAT = 3
OUT = decode.WORK / 'pack'
KIND_IDS = ['FT3', 'FT4', 'F4', 'GT3', 'GT4', 'G3', 'G4']


def pad4(b: bytes) -> bytes:
    return b + bytes(-len(b) % 4)


def prim_bytes(p: decode.Prim) -> bytes:
    n = len(p.idx)
    idx = list(p.idx) + [0] * (4 - n)
    uv = list(p.uv) + [(0, 0)] * (4 - len(p.uv))
    rgb = list(p.colors) + [p.rgb()] * (4 - len(p.colors))
    out = struct.pack('<I4B', p.header, KIND_IDS.index(p.kind), p.code, p.cull[2], len(p.colors))
    out += bytes(idx) + bytes(c for xy in uv for c in xy)
    out += struct.pack('<2H', p.clut, p.tpage) + bytes(c for col in rgb for c in col)
    out += bytes(p.window or (0, 0, 0, 0))
    assert len(out) == 40
    return out


def prim_from(b: bytes, o: int) -> decode.Prim:
    header, kind, code, cull, ncol = struct.unpack_from('<I4B', b, o)
    name = KIND_IDS[kind]
    n = decode.KINDS[header][1]
    idx = tuple(b[o + 8:o + 8 + n])
    uv = tuple((b[o + 12 + 2 * k], b[o + 13 + 2 * k]) for k in range(n)) if decode.KINDS[header][2] else ()
    clut, tpage = struct.unpack_from('<2H', b, o + 20)
    rgb = [tuple(b[o + 24 + 3 * k:o + 27 + 3 * k]) for k in range(4)]
    colors = tuple(rgb[:ncol]) if ncol else ()
    window = tuple(b[o + 36:o + 40]) if header & 0x80000000 else None
    return decode.Prim(header, name, idx, uv, clut if uv else 0, tpage if uv else 0, colors, window)


def tile_section(geo) -> bytes:
    out = struct.pack('<2I', geo.tile_size, len(geo.tiles))
    for t in geo.tiles:
        out += struct.pack('<4B2H', t.slot, int(t.layer_b), int(t.skipped), 0, len(t.verts), len(t.prims))
        out += b''.join(struct.pack('<4h', *v, 0) for v in t.verts)
        out += b''.join(prim_bytes(p) for p in t.prims)
    return out


class Relocation:
    """Palettes moved to the high half (E3: the runtime reads System 12 CLUT
    words, bit 15 = y + 512, but rows 96..224 of the low half belong to the
    PS1 game). Every run of 16-colour cells on a palette row moves as one
    piece, so a CLUT word pointing anywhere in it, 4 or 8 bits, still finds
    its colours; runs go to free places of the high half (no texture there),
    pages without any texture first, bottom right first."""

    def __init__(self, st):
        import numpy as np
        images = [(x, y, w, h) for k, x, y, w, h, b in st.rects if k == 'image']
        assert all(y >= 512 for x, y, w, h in images), 'a stage texture in the low half'
        cells = set()
        for k, x, y, w, h, b in st.rects:
            if k == 'clut':
                assert y + h <= 512, 'a palette in the high half'
                cells |= {(cx, yy) for yy in range(y, y + h) for cx in range(x // 16, (x + w + 15) // 16)}
        for x, y in st.patches:
            cells |= {(x // 16, y)}
        used = np.zeros((512, 1024), bool)
        for x, y, w, h in images:
            used[y - 512:y - 512 + h, x:x + w] = True
        page_used = used.reshape(2, 256, 16, 64).any(axis=(1, 3))      # [row of pages, column]
        runs = []
        for y in sorted({y for _, y in cells}):
            xs = sorted(cx for cx, yy in cells if yy == y)
            start = prev = xs[0]
            for cx in xs[1:] + [None]:
                if cx != prev + 1:
                    runs.append((y, start, prev + 1 - start))
                    start = cx
                prev = cx
        self.map, self.runs = {}, []
        for y, cx0, n in sorted(runs, key=lambda r: (-r[2], r[0], r[1])):
            spot = self.place(used, page_used, n, prefer_free_pages=True) or self.place(used, page_used, n, False)
            if spot is None:
                raise ValueError(f'stage {st.stage}: no room for a {16 * n}-colour palette run in the high half')
            nx, ny = spot
            used[ny - 512, nx:nx + 16 * n] = True
            self.runs.append((cx0 * 16, y, nx, ny, 16 * n))
            for k in range(n):
                self.map[cx0 + k, y] = (nx // 16 + k, ny)

    @staticmethod
    def place(used, page_used, n, prefer_free_pages):
        w = 16 * n
        for yy in range(511, -1, -1):
            for x in range(1024 - w, -1, -16):
                if used[yy, x:x + w].any():
                    continue
                if prefer_free_pages and page_used[yy // 256, x // 64:(x + w - 1) // 64 + 1].any():
                    continue
                return x, yy + 512
        return None

    def clut(self, word):
        """Old System 12 CLUT word (low half) -> new one (bit 15 set)."""
        if word & 0x8000:
            raise ValueError(f'CLUT {word:04X} already in the high half')
        key = (word & 0x3F, word >> 6 & 0x1FF)
        if key not in self.map:
            raise ValueError(f'CLUT {word:04X} outside the stage palettes')
        nx, ny = self.map[key]
        return (ny << 6 | nx) & 0xFFFF

    def prim(self, p):
        import dataclasses
        return dataclasses.replace(p, clut=self.clut(p.clut)) if p.uv else p

    def word1(self, w1):
        """Floor cell word: CLUT in the high half."""
        return (w1 & 0xFFFF) | self.clut(w1 >> 16) << 16

    def command(self, c):
        """A scene command with its CLUT relocated."""
        return dict(c, clut=self.clut(c['clut'])) if 'clut' in c else c


def save(st: scene.Stage, path: Path):
    import copy
    s = st.stage
    sections = []
    reloc = Relocation(st)
    vram = st.vram
    # VRAM rectangles, in upload order: textures, then the relocated palettes
    blob, count = b'', 0
    for kind, x, y, w, h, bpp in st.rects:
        if kind == 'image':
            blob += struct.pack('<4H', x, y, w, h) + vram[y:y + h, x:x + w].astype('<u2').tobytes()
            count += 1
    for x, y, nx, ny, w in reloc.runs:
        blob += struct.pack('<4H', nx, ny, w, 1) + vram[y, x:x + w].astype('<u2').tobytes()
        count += 1
    sections.append((b'VRAM', struct.pack('<I', count) + blob))
    sections.append((b'CLUT', struct.pack('<I', len(reloc.runs))
                     + b''.join(struct.pack('<6H', x, y, nx, ny, w, 0) for x, y, nx, ny, w in reloc.runs)))
    geo = copy.deepcopy(st.geo)
    for t in geo.tiles:
        t.prims = [reloc.prim(p) for p in t.prims]
    sections.append((b'TILE', tile_section(geo)))
    fb = copy.deepcopy(st.farbg)
    fb.cluts = [[reloc.clut(c) for c in r] for r in fb.cluts]
    farb = struct.pack('<6h', fb.cols, fb.rows, fb.ring, fb.yoff, fb.degrees, fb.pitch)
    farb += bytes(fb.cell_rgb) + bytes(fb.top_rgb) + bytes([fb.top_on]) + bytes(fb.bottom_rgb) + bytes([fb.bottom_on, 0])
    farb += b''.join(struct.pack('<%dH' % fb.cols, *r) for r in fb.desc)
    farb += b''.join(struct.pack('<%dH' % fb.cols, *r) for r in fb.cluts)
    farb += pad4(bytes(k for r in fb.kinds for k in r))
    sections.append((b'FARB', farb))
    fl = copy.deepcopy(st.floor)
    fl.cells = [[(w0, reloc.word1(w1)) for w0, w1 in r] for r in fl.cells]
    fl.halves = [[(w0, reloc.word1(w1)) for w0, w1 in r] for r in fl.halves]
    flor = struct.pack('<2hi', fl.kind, fl.fog, fl.near)
    flor += b''.join(struct.pack('<2I', *c) for r in fl.cells for c in r)
    flor += b''.join(struct.pack('<2I', *c) for r in fl.halves for c in r)
    flor += pad4(bytes(u for r in fl.unique for u in r))
    flor += pad4(struct.pack('<H', len(fl.verts)) + b''.join(struct.pack('<2h', *v) for v in fl.verts))
    flor += pad4(bytes(len(fl.quads[r, c]) for r in range(10) for c in range(10)))
    flor += b''.join(struct.pack('<%dI' % len(fl.quads[r, c]), *fl.quads[r, c]) for r in range(10) for c in range(10))
    for n in (1, 2, 4):
        flor += b''.join(struct.pack('<4H', *q) for rot in fl.uv[n] for q in rot)
    sections.append((b'FLOR', flor))
    t = st.tables
    sections.append((b'TRIG', struct.pack('<4096h4096h192h', *t.sin, *t.cos, *t.sqrt) + t.fog
                     + struct.pack('<192h', *t.sqrt2)))
    if st.objects:
        objs = struct.pack('<I', len(st.objects))
        for m in st.objects:
            objs += struct.pack('<2H', len(m.verts), len(m.prims))
            objs += b''.join(struct.pack('<4h', *v, 0) for v in m.verts)
            objs += b''.join(prim_bytes(reloc.prim(p)) for p in m.prims)
        lay = scene.object_layout(s)
        objs += struct.pack('<I', len(lay)) + b''.join(struct.pack('<2B3H', m, 0, r, n, 0) for m, r, n in lay)
        sections.append((b'OBJS', objs))
    if st.rot_table:
        sections.append((b'ROTT', struct.pack('<4096I', *st.rot_table)))
    if s in decode.ANIMATED:
        frames = st.anim
        anim = struct.pack('<6H', *decode.ANIMATED[s], 16, 64, decode.ANIM_PERIOD, len(frames))
        anim += b''.join(struct.pack('<2H', *f) for f in frames)
        sections.append((b'ANIM', anim))
    head = 16 + 12 * len(sections)
    table, body, off = b'', b'', head
    for sid, data in sections:
        data = pad4(data)
        table += struct.pack('<4s2I', sid, off, len(data))
        body += data
        off += len(data)
    path.write_bytes(struct.pack('<4s2H2I', b'ASTG', FORMAT, s, len(sections), off) + table + body)


def load(path: Path, anim_frame=None) -> scene.Stage:
    """A scene.Stage rebuilt from a pack file alone."""
    import numpy as np
    b = path.read_bytes()
    magic, version, stage, n, size = struct.unpack_from('<4s2H2I', b, 0)
    assert magic == b'ASTG' and version == FORMAT and size == len(b), (magic, version, size, len(b))
    sec = {}
    for i in range(n):
        sid, off, ln = struct.unpack_from('<4s2I', b, 16 + 12 * i)
        sec[sid] = off
    st = scene.Stage.__new__(scene.Stage)
    st.stage = stage
    # VRAM
    o = sec[b'VRAM']
    count = struct.unpack_from('<I', b, o)[0]; o += 4
    v = np.zeros((1024, 1024), np.uint16)
    st.rects = []
    for _ in range(count):
        x, y, w, h = struct.unpack_from('<4H', b, o); o += 8
        v[y:y + h, x:x + w] = np.frombuffer(b, '<u2', w * h, o).reshape(h, w); o += 2 * w * h
        st.rects.append(('rect', x, y, w, h, 0))
    if b'ANIM' in sec and anim_frame is not None:
        a = sec[b'ANIM']
        dx, dy, w, h, period, nf = struct.unpack_from('<6H', b, a)
        sx, sy = struct.unpack_from('<2H', b, a + 12 + 4 * anim_frame)
        v[dy:dy + h, dx:dx + w] = v[sy:sy + h, sx:sx + w].copy()
    st.vram = v
    # tiles
    o = sec[b'TILE']
    tile_size, nt = struct.unpack_from('<2I', b, o); o += 8
    geo = decode.Geometry(tile_size)
    for i in range(nt):
        slot, lb, skipped, _, nv, npr = struct.unpack_from('<4B2H', b, o); o += 8
        verts = [struct.unpack_from('<3h', b, o + 8 * k) for k in range(nv)]; o += 8 * nv
        prims = [prim_from(b, o + 40 * k) for k in range(npr)]; o += 40 * npr
        geo.tiles.append(decode.Tile(i, verts, prims, slot=slot, layer_b=bool(lb), skipped=bool(skipped)))
    st.geo = geo
    # far background
    o = sec[b'FARB']
    cols, rows, ring, yoff, degrees, pitch = struct.unpack_from('<6h', b, o); o += 12
    c = b[o:o + 12]; o += 12
    desc = [list(struct.unpack_from('<%dH' % cols, b, o + 2 * cols * r)) for r in range(rows)]; o += 2 * cols * rows
    cluts = [list(struct.unpack_from('<%dH' % cols, b, o + 2 * cols * r)) for r in range(rows)]; o += 2 * cols * rows
    kinds = [list(b[o + ring * r:o + ring * (r + 1)]) for r in range(rows)]
    st.farbg = scene.FarBg(stage, cols, rows, ring, desc, cluts, kinds, yoff, degrees, pitch,
                           tuple(c[0:3]), tuple(c[3:6]), c[6], tuple(c[7:10]), c[10])
    # floor
    o = sec[b'FLOR']
    kind, fog, near = struct.unpack_from('<2hi', b, o); o += 8
    w = struct.unpack_from('<1000I', b, o); o += 4000
    cells = [[(w[2 * (10 * r + c)], w[2 * (10 * r + c) + 1]) for c in range(10)] for r in range(10)]
    halves = [[(w[200 + 2 * (20 * r + c)], w[201 + 2 * (20 * r + c)]) for c in range(20)] for r in range(20)]
    unique = [list(b[o + 10 * r:o + 10 * r + 10]) for r in range(10)]; o += 100 + (-100 % 4)
    nv = struct.unpack_from('<H', b, o)[0]
    verts = [struct.unpack_from('<2h', b, o + 2 + 4 * k) for k in range(nv)]
    o += len(pad4(bytes(2 + 4 * nv)))
    counts = b[o:o + 100]; o += 100
    quads = {}
    for r in range(10):
        for cc in range(10):
            k = counts[10 * r + cc]
            quads[r, cc] = list(struct.unpack_from('<%dI' % k, b, o)); o += 4 * k
    uv = {}
    for nq in (1, 2, 4):
        uv[nq] = [[struct.unpack_from('<4H', b, o + 8 * (rot * nq + q)) for q in range(nq)] for rot in range(4)]
        o += 8 * 4 * nq
    st.floor = scene.Floor(stage, kind, fog, cells, halves, unique, verts, quads, near, uv)
    o = sec[b'TRIG']
    t = struct.unpack_from('<4096h4096h192h', b, o)
    st.tables = scene.Tables(t[:4096], t[4096:8192], t[8192:], b[o + 16768:o + 16768 + 1024],
                             struct.unpack_from('<192h', b, o + 16768 + 1024))
    st.rot_table = struct.unpack_from('<4096I', b, sec[b'ROTT']) if b'ROTT' in sec else None
    st.objects = []
    if b'OBJS' in sec:
        o = sec[b'OBJS']
        nm = struct.unpack_from('<I', b, o)[0]; o += 4
        for i in range(nm):
            nv, npr = struct.unpack_from('<2H', b, o); o += 4
            verts = [struct.unpack_from('<3h', b, o + 8 * k) for k in range(nv)]; o += 8 * nv
            prims = [prim_from(b, o + 40 * k) for k in range(npr)]; o += 40 * npr
            st.objects.append(decode.Tile(0, verts, prims))
    return st


def check(stages, pictures=True):
    """Every MAME dump: commands from the pack == the decoder's with the
    CLUTs relocated, and the same picture (no palette over a texture)."""
    import render
    import validate
    arcade = decode.Arcade()
    bad = 0
    for s in stages:
        ref = scene.Stage(arcade, s)
        st = load(OUT / f'stage{s:02}.ast')
        images = [(x, y, w, h) for k, x, y, w, h, _ in ref.rects if k == 'image']
        same_vram = all((st.vram[y:y + h, x:x + w] == ref.vram[y:y + h, x:x + w]).all() for x, y, w, h in images)
        d = validate.DUMPS / f'stage{s:02}'
        regs = validate.gte_lines(d / 'mame.log')
        n = 0
        reloc = Relocation(ref)
        pics = 0
        for name in sorted(regs):
            inputs = validate.frame_inputs(validate.Ram(d / f'{name}.bin'), regs[name], stage=s)
            a, b = ref.frame(**inputs), st.frame(**inputs)
            n += 1
            if [reloc.command(c) for c in a] != b:
                bad += 1
                print(f'stage {s:2} {name}: {len(a)} commands from the decoder, {len(b)} from the pack, differ')
            if pictures:
                g1, g2 = render.Gpu(ref.vram), render.Gpu(st.vram)
                render.run(g1, a); render.run(g2, b)
                if (g1.fb != g2.fb).any():
                    bad += 1
                    print(f'stage {s:2} {name}: {(g1.fb != g2.fb).sum()} pixels differ')
                pics += 1
        print(f'stage {s:2}: {n} frames, pack == decoder (CLUTs relocated): {"yes" if not bad else "NO"}, '
              f'textures {"same" if same_vram else "DIFFER"}, {pics} pictures compared, '
              f'{len(reloc.runs)} palette runs moved')
        bad += not same_vram
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--stages', default='0-12')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args(argv)
    stages = decode.numbers(args.stages)
    arcade = decode.Arcade()
    OUT.mkdir(parents=True, exist_ok=True)
    for s in stages:
        st = scene.Stage(arcade, s)
        path = OUT / f'stage{s:02}.ast'
        save(st, path)
        print(f'stage {s:2}: {path.name} {path.stat().st_size} bytes')
    if args.check:
        return 1 if check(stages) else 0
    return 0


if __name__ == '__main__':
    sys.exit(main())

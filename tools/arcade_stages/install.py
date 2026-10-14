#!/usr/bin/env python3
"""Arcade Stages: the 13 stage packs the game reads, made from your tekken3.zip.

    python tools/arcade_stages/install.py --tekken3 tekken3.zip [--out DIR]

Run by the setup (launcher/setup_backend.py) when it is given tekken3.zip
(Tekken 3 arcade, TET2/VER.E1): no MAME, a few seconds. Writes
stage00.ast .. stage12.ast (pack.py, format pack.FORMAT) and version.txt
(version.ARCADE_STAGES_VERSION) into a temporary folder, then puts it in
place of DIR (default workspace/arcade-stages/pack). CMakeLists.txt stages
that folder beside the game as mods/arcade-stages, on PC and Android, where
the Arcade Stages mod reads it (src/tekken3_arcade_stages.c). Without it the
mod keeps the PS1 stages. Nothing from the ROM goes into git.
"""
from __future__ import annotations

import argparse
import shutil
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / 'tools/arcade_stages')]
import version  # noqa: E402

DEFAULT_OUT = ROOT / 'workspace/arcade-stages/pack'


def install(tekken3: Path, out: Path = DEFAULT_OUT, say=print) -> Path:
    import decode
    import pack
    import scene
    start = time.time()
    arcade = decode.Arcade(tekken3, cache=False)
    temp = out.with_name(out.name + '.new')
    shutil.rmtree(temp, ignore_errors=True)
    temp.mkdir(parents=True)
    for s in version.STAGES:
        path = temp / version.pack_name(s)
        pack.save(scene.Stage(arcade, s), path)
        say(f'stage {s:2}: {path.name} {path.stat().st_size} bytes')
    (temp / version.STAMP).write_text(f'{version.ARCADE_STAGES_VERSION}\n', encoding='utf-8')
    shutil.rmtree(out, ignore_errors=True)
    temp.replace(out)
    say(f'Arcade stages version {version.ARCADE_STAGES_VERSION} in {out} ({time.time() - start:.0f} s)')
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--tekken3', type=Path, required=True, help='tekken3.zip (Tekken 3 arcade, TET2/VER.E1)')
    ap.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = ap.parse_args(argv)
    install(args.tekken3.resolve(), args.out.resolve())
    return 0


if __name__ == '__main__':
    sys.exit(main())

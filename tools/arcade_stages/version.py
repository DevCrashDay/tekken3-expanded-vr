"""Arcade Stages: the version of the stage packs the setup makes (install.py).

Increase ARCADE_STAGES_VERSION whenever the packs the setup writes change
(pack.FORMAT, decode.py, scene.py, pack.py): the setup sees the older stamp
and makes them again from the player's tekken3.zip (launcher/setup_backend.py).
No imports here: the setup reads it without numpy.
"""
from __future__ import annotations

from pathlib import Path

ARCADE_STAGES_VERSION = 1
STAMP = 'version.txt'
STAGES = range(13)          # arenas 0..12; Gon's (13) and Dr. B's (14) keep the PS1 stage


def pack_name(stage: int) -> str:
    return f'stage{stage:02}.ast'


def read_stamp(folder: Path) -> int:
    try:
        return int((Path(folder) / STAMP).read_text(encoding='utf-8').strip())
    except (OSError, ValueError):
        return 0


def current(folder: Path) -> bool:
    """The folder holds all the packs of this version."""
    folder = Path(folder)
    return (read_stamp(folder) >= ARCADE_STAGES_VERSION
            and all((folder / pack_name(s)).is_file() for s in STAGES))

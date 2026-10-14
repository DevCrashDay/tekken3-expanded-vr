#!/usr/bin/env python3
"""Play Kazuya's Arcade yourself, his Tekken Tag Tournament (PS2) ending in
place of the ending movie (TEKKEN3_ENDING, src/tekken3_embu_scenes.c).
Debug build, a game window; run through slot.py (tools/ps2/play_kazuya_arcade_self.sh
does, after building the packs).

    python3 tools/ps2/play_kazuya_arcade_self.py [--skip]

Your pad (the [controller] section of TEKKEN3_SETTINGS, by default
../Tekken3Recompiled/build-opt/settings.toml), cold boot, nothing pressed for you: pick Kazuya on the Tag page (R2) of
the Arcade selector. --skip: your first fight is stage 9, Ogre next. The script waits until you close the window.
"""
import json, socket, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ttt1_live as L

PLAN = json.loads((ROOT / 'workspace/ttt-ps2/kazuya-plan.json').read_text())
# The ending's arena: tools/ps2/ttt_endings.json (Kazuya's: Ogre's temple).
ENDING = json.loads((ROOT / 'tools/ps2/ttt_endings.json').read_text())['endings']['kazuya']
ARENA = f"{ENDING['arena']}:{2 if ENDING.get('dark') else 0}"      # arena, then its dark look (True Ogre's) or not
keys = [l.split()[0] for l in (ROOT / 'build-opt-dbg/mods/ttt1/guests.txt').read_text().splitlines() if l.split()]
p2 = dict(kv.split('=') for kv in PLAN['cast'].split(','))['paul']
ENV = {'TEKKEN3_FIGHT_CINE': f"{Path(PLAN['cine']).resolve()}@ending",
       'TEKKEN3_ENDING': f"{L.GUEST_ID + keys.index('kazuya')}:{L.GUEST_ID + keys.index(p2)}:{ARENA}",
       'TEKKEN3_EMBU_CAMERA_TRACK': str(Path(PLAN['camera_track']).resolve())}
for k, p in (('TEKKEN3_EMBU_MODEL_P2', 'model_p2'), ('TEKKEN3_EMBU_MUSIC', 'music'), ('TEKKEN3_EMBU_FADE', 'fade'),
             ('TEKKEN3_EMBU_EFFECT_P1', 'effect_p1'), ('TEKKEN3_EMBU_EFFECT_P2', 'effect_p2'),
             ('TEKKEN3_EMBU_EFFECT_LIGHT', 'effect_light')):
    if p in PLAN: ENV[k] = PLAN[p]


def start(self, log, port):
    """Cold boot, no save state loaded, nothing pressed."""
    deadline = time.monotonic() + 30
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), .2): break
        except OSError:
            if time.monotonic() > deadline: raise
            time.sleep(.1)


L.Session.start = start
# Your pad: the [controller] section of your own build's settings
# (TEKKEN3_SETTINGS, default ../Tekken3Recompiled/build-opt/settings.toml).
import os, re
mine = Path(os.environ.get('TEKKEN3_SETTINGS', ROOT.parent / 'Tekken3Recompiled/build-opt/settings.toml'))
if mine.exists():
    m = re.search(r'^\[controller\]\n(?:(?!\[).*\n?)*', mine.read_text(), re.M)
    if m: L.Session.controller = m.group(0); print('controller settings from', mine, flush=True)
s = L.Session(guest='kazuya', visible=True, env=ENV)
print('Game window open: Arcade, R2 for the Tag page, Kazuya. Close the window to end.', flush=True)
try:
    skipped = '--skip' not in sys.argv
    while s.process.poll() is None:
        if not skipped:
            try:
                # Your first fight counts as stage 9 (index 8 from its loading
                # on; its opponent is already drawn): Ogre, stage 10, is next.
                if s.value(0x800AE204) == 11 and s.value(0x800AFAAC, 1) == 0:
                    s.write(0x800AFAAC, 8, 1); skipped = True; print('first fight: stage 9, Ogre next', flush=True)
            except Exception:
                pass
        time.sleep(.5 if skipped else .01)
finally:
    s.stop()

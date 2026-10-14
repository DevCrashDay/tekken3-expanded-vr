#!/usr/bin/env python3
"""A converted TTT ending played as a cutscene of an Arcade fight, in the
fight's arena (prototype).

    python3 tools/ps2/fight_ending.py OUT PLAN.json [--arena 6] [--p1 kazuya] [--p2 KEY] [--hide-p2]
        [--visible] [--dumps F,...] [--trace LO-HI,...] [--dense A-B]

PLAN.json: tools/ps2/ttt_ending_pack.py (its "cine" script, camera track,
model steps, music). Debug build, one game: run through slot.py. Cold
boot, Arcade, --p1 (Kazuya) on the Tag page; player 2 = the plan's player-2 guest
(or --p2; an ending played alone, Bruce's, has none)
written at loading, the arena (a fighter record's byte 10: Ogre's temple
is 6) held from the selection to the fight. The mod starts the cutscene
3 s into the first round (TEKKEN3_FIGHT_CINE, src/tekken3_embu_scenes.c:
the fight's own cutscene phases, 16 and 17, on our script). --hide-p2 keeps
player 2 far out of the shots during the cutscene (actor base +0x00, x).
Logs the phase per change, a shot every 30 frames to OUT.
"""
import json, re, socket, struct, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import ttt1_live as L

OUT = Path(sys.argv[1]); OUT.mkdir(parents=True, exist_ok=True)
PLAN = json.loads(Path(sys.argv[2]).read_text())
opt = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
ARENA = int(opt('--arena', 6))
STATE, PHASE = 0x800AE204, 0x800AE224
ACTOR = lambda p: 0x800A9228 + p * 0x188C
P1 = opt('--p1', 'kazuya')
p2_guest = opt('--p2') or dict(kv.split('=') for kv in PLAN['cast'].split(',')).get('paul') or P1
ENV = {'TEKKEN3_GUEST_P2': p2_guest, 'TEKKEN3_FIGHT_CINE': f"{Path(PLAN['cine']).resolve()}@180",
       'TEKKEN3_EMBU_CAMERA_TRACK': str(Path(PLAN['camera_track']).resolve())}
for k, p in (('TEKKEN3_EMBU_MODEL_P2', 'model_p2'), ('TEKKEN3_EMBU_MUSIC', 'music'), ('TEKKEN3_EMBU_FADE', 'fade')):
    if p in PLAN: ENV[k] = PLAN[p]
for k, p in (('TEKKEN3_EMBU_EFFECT_P1', 'effect_p1'), ('TEKKEN3_EMBU_EFFECT_P2', 'effect_p2'),
             ('TEKKEN3_EMBU_EFFECT_LIGHT', 'effect_light')):
    if p in PLAN: ENV[k] = PLAN[p]



def start(self, log, port):
    """Cold boot: no selector save state in this worktree."""
    deadline = time.monotonic() + 30
    while True:
        try:
            with socket.create_connection(('127.0.0.1', port), .2): break
        except OSError:
            if time.monotonic() > deadline: raise
            time.sleep(.1)


L.Session.start = start
s = L.Session(guest=P1, visible='--visible' in sys.argv, env=ENV); q = s.q


def press(b):
    q(cmd='input_route_clear'); q(cmd='input_route_append', frames=3, buttons=b)
    q(cmd='input_route_append', frames=12, buttons=0xFFFF); q(cmd='input_route_start')
    while q(cmd='input_route_status').get('active'): time.sleep(.02)


s.press = press
print(json.dumps(s.info), flush=True)
try:
    # The cabinet walk of ttt1_live.cabinet, with the arena held from the
    # selection on (the stage loads from 0x800ADC84 during loading).
    q(cmd='clear_input'); q(cmd='turbo', enabled=1)
    s.wait(lambda: s.value(STATE) in (1, 3), 'title', 120)
    while s.value(STATE) != 4:
        if s.value(STATE) in (1, 3): press(0xFFF7)
        time.sleep(.3)
    time.sleep(.5); press(0xBFFF)                          # ARCADE, under the cursor
    s.wait(lambda: s.value(STATE) == 9 and s.value(0x80118648) == 21, 'selector', 60)
    q(cmd='turbo', enabled=0)
    s.press(L.PAD_R2)
    tag = list(range(L.GUEST_ID, L.GUEST_ID + s.catalogue_size))
    time.sleep(2)
    row, col = L.tag_place(s.index, s.catalogue_size)
    for _ in range(12):
        r, c = L.tag_place(s.value(0x80118650), s.catalogue_size)
        if (r, c) == (row, col): break
        s.press(0xffbf if r < row else 0xffef if r > row else 0xffdf if c < col else 0xff7f)
    s.wait(lambda: s.value(0x80118668) == s.cid, 'guest cell', 10)
    s.press(0x7fff)
    end = time.monotonic() + 120
    while not (s.value(STATE) == 8 and s.value(PHASE, 2) >= 6):
        if time.monotonic() > end: raise RuntimeError(f'no fight: state {s.value(STATE)}')
        st = s.value(STATE)
        if st in (9, 11):
            s.write(0x800ADC84, ARENA, 1); s.write(0x800AFAA2, ARENA, 1)
        if st == 11:
            s.write(0x800add5e, s.cid_p2, 2); s.write(0x800add9a, 0, 2)
        time.sleep(.01)
    print('fight: arena', s.value(0x800ADC84, 1), 'actors', s.actor(0), s.actor(1), flush=True)
    f0 = q(cmd='frame')['frame']; last = None; shot = -99; started = None
    while (f := q(cmd='frame')['frame'] - f0) < 2400:
        st, ph = s.value(STATE), s.value(PHASE, 2)
        if '--hide-p2' in sys.argv and ph in (16, 17): s.write(ACTOR(1), 40000, 4)
        if (st, ph) != last:
            print(f, 'state', st, 'phase', ph, flush=True); last = (st, ph)
            if ph == 17 and started is None:
                started = f
                for r in [v for v in (opt('--trace') or '').split(',') if v]:
                    lo, hi = r.split('-'); q(cmd='wtrace_arm', lo=lo, hi=hi)
        if started is not None and opt('--trace') and f - started >= 950 and not (OUT / 'wtrace.json').exists():
            (OUT / 'wtrace.json').write_text(json.dumps(q(cmd='wtrace_dump', count=2000, newest=1)))
        if started is not None and any(f - started >= int(v) and not (OUT / f'ram_{int(v):05d}.bin').exists() for v in (opt('--dumps') or '').split(',') if v):
            v = max(int(v) for v in opt('--dumps').split(',') if v and f - started >= int(v))
            (OUT / f'ram_{v:05d}.bin').write_bytes(s.read(0x80000000, 0x200000))
        dense = opt('--dense') and started is not None and int(opt('--dense').split('-')[0]) <= f - started <= int(opt('--dense').split('-')[1])
        if f - shot >= (4 if dense else 30):
            q(cmd='screenshot_file', path=str(OUT / f'{f:05d}_p{ph}.png')); shot = f
        if started is not None and ph not in (16, 17) and f > started + 60: break
        time.sleep(.01)
    print('end', f, flush=True)
finally:
    s.stop()

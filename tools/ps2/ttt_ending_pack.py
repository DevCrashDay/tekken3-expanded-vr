#!/usr/bin/env python3
"""Put a retargeted TTT (PS2) ending into guests' combat packs, for Tekken 3.

    python3 tools/ps2/ttt_ending_pack.py OUT_PLAN.json --p1 RETARGET.json [--p2 RETARGET.json]
        [--p2-guest KEY] [--p2-models "Prefix@capture frame,..."] [--p2-aura FIRST-LAST[:BONE,...]] [--aura-step 4]
        [--p2-effect PACK.tim] [--p1-effect PACK.tim] [--effect-light RRGGBB]
        [--stage SRC:NAME] [--start 14] [--arena N] [--fade IN:OUT] [--fade-capture CAPTURE.jsonl] [--music PCM] [--behind "PLAYER:FIRST-LAST:UNITS[:RAMP];..."]
        [--hold-camera "STOP:EASE:LAST;..."]

Each player's poses (tools/ps2/ttt_retarget.py) become clips of at most CHUNK
frames (a clip's length is one byte), new records copied from the guest's
neutral one, appended to its pack in this worktree's own build
(build-opt-dbg/mods/ttt1, never the shared roster). Player 1 is cast on
Hwoarang (the Embu's opening player 1), player 2 on Paul (its opening
player 2) as --p2-guest; --p2-models draws player 2 with other model files
from a frame of the capture on (Jin, then Devil Jin). OUT_PLAN.json is a
tools/embu/play.py plan: the clips from Embu frame `start` (the guests are
grafted by then), the camera track of player 1's retarget, the stop.
"""
import json, math, shutil, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/ttt1'))
from pack_edit import Pack

CHUNK = 240
HOLD = 130                          # frames after the ending until the window closes (end_at + 10)
SLOT_BASE, SLOT_STRIDE = 8192, 4096
BUILD = ROOT / 'build-opt-dbg/mods/ttt1'
LEAD = {0: 3438, 1: 3441}          # an Embu clip each side plays before its guest is grafted


def clips(r, key, player, start, aura=None, step=4):
    """Script records playing r's poses on `player` from `start`; the pack
    of guest `key` gets the clips. `aura` (first, last, bones): capture
    frames lit by the lightning aura (timed property 6, the Supercharger's),
    one entry every `step` frames, each on the next bone of `bones` in turn: the
    property's low byte is the bone the effect shows at (0x800458C8: actor
    +0x908 + 68 * bone)."""
    name = key.capitalize()
    pack = Pack((ROOT / 'workspace/ttt1-import/roster' / f'{name}-TTT1-combat.jmv').read_bytes())
    # The Embu's clip until the guest is grafted (a few frames), then the
    # ending's first pose held until it starts: no stock pose on screen.
    recs, f = [], start
    # The first pose held by a clip of its own (a one-frame record goes on
    # to its next move, the stance at the origin), from the first frame:
    # before its guest is grafted, a guest move falls back on an Embu clip.
    i = pack.add_record(pack.neutral, 0x803F0000 + 0xFFF0)
    pack.set_clip(i, [r['poses'][0]] * start)
    recs.append([0, 2 + player, SLOT_BASE + player * SLOT_STRIDE + i, 0, start - 1])
    for k in range(0, len(r['poses']), CHUNK):
        chunk = r['poses'][k:k + CHUNK]
        i = pack.add_record(pack.neutral, 0x803F0000 + k)  # the validator wants 0x80010000..0x803FFFFF; free there
        pack.set_clip(i, chunk)
        if aura:
            lit = [(f - k + 1, 6, aura[2][f // step % len(aura[2])]) for f in range(max(aura[0], k), min(aura[1], k + len(chunk) - 1) + 1, step)]
            if lit:
                pack.set_props(i, lit)
        recs.append([f, 2 + player, SLOT_BASE + player * SLOT_STRIDE + i, 0, len(chunk) - 1]); f += len(chunk)
    # The last pose held by a clip of its own: a record at its end frame
    # goes on to its next move (the stance, at the origin).
    i = pack.add_record(pack.neutral, 0x803F0000 + len(r['poses']))
    pack.set_clip(i, [r['poses'][-1]] * HOLD)
    recs.append([f, 2 + player, SLOT_BASE + player * SLOT_STRIDE + i, 0, HOLD - 1])
    (BUILD / f'{name}-TTT1-combat.jmv').write_bytes(pack.build())
    return recs, f


def push_behind(r, cameras, first, last, units, ramp=0):
    """Move r's root `units` further from the camera, along the view, over
    capture frames first..last (the root channels: heading, height,
    distance; x = sin(a) d, z = cos(a) d, the camera in the same frame).
    Negative units move it away from the camera, towards what it looks at.
    `ramp` frames ease it in from `first`, inside a shot where a jump of the
    actor would show (end `last` on a cut)."""
    K = 2 * math.pi / 65536
    s16 = lambda v: ((int(v) + 32768) % 65536) - 32768
    for i in range(first, last + 1):
        q, c = r['poses'][i], cameras[i]
        back = [c[k] - c[3 + k] for k in (0, 2)]                  # eye - target, on the floor
        n = math.hypot(*back) or 1.0
        u = units * (min(1.0, (i - first + 1) / ramp) if ramp else 1.0)
        h, d = s16(q[0]) * K, q[2]
        x, z = math.sin(h) * d + back[0] / n * u, math.cos(h) * d + back[1] / n * u
        q[0] = int(round(math.atan2(x, z) / K)) & 0xffff
        q[2] = int(round(math.hypot(x, z))) & 0xffff


def hold_camera(cameras, stop, ease, last):
    """The camera stops at its capture frame `stop` and holds that view up to
    `last` (a cut): an approach that would bring it too close (polygons
    crossing it) ends short. Over `ease` frames it slows down to it: frame
    i of the slowdown shows the track at a = stop - ease / 2 plus
    ease * (u - u^2 / 2), u = (i - a) / ease, reached at stop + ease / 2."""
    src = [list(c) for c in cameras]
    a = stop - ease / 2
    at = lambda t: [v0 + (v1 - v0) * (t - int(t)) for v0, v1 in zip(src[int(t)], src[min(int(t) + 1, len(src) - 1)])]
    for i in range(int(a), last + 1):
        u = min(1.0, (i - a) / ease) if ease else 1.0
        cameras[i] = [round(v) for v in at(a + ease * (u - u * u / 2) if ease else stop)]


def captured_fades(capture):
    """The PS2 cutscene's own fades, one T3 level a capture frame (-256
    black, 0 the image, 256 white), from the quads ttt_capture.py records
    ("fade": black colour, white colour, alpha in the top byte, 0x80
    opaque). The white quad is drawn only while its alpha moves (it keeps
    its last value in between, unseen); the black one always (2 / 128 in a
    shot: nothing)."""
    out, prev_white = [], None
    for line in Path(capture).read_text().splitlines()[1:]:
        d = json.loads(line)
        if 'fade' not in d: continue
        black, white = d['fade'][0] >> 24, d['fade'][1] >> 24
        lit = prev_white is not None and white != prev_white
        prev_white = white
        level = 2 * white if lit else -2 * black
        out.append(0 if abs(level) <= 4 else max(-256, min(256, level)))
    return out


def fade_segments(levels, offset, tol=6):
    """TEKKEN3_EMBU_FADE spans for per-frame levels: straight pieces within
    `tol` of every frame, frames at 0 left out (the switch off)."""
    spans, i, n = [], 0, len(levels)
    while i < n:
        if levels[i] == 0: i += 1; continue
        j = i
        while j + 1 < n and levels[j + 1] != 0:
            k = j + 1
            fits = all(abs(levels[i] + (levels[k] - levels[i]) * (m - i) / (k - i) - levels[m]) <= tol for m in range(i, k + 1))
            if not fits: break
            j = k
        spans.append(f'{offset + i}-{offset + j}:{levels[i]}:{levels[j]}')
        i = j + 1
    return spans


def main():
    out = Path(sys.argv[1])
    opt = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    start = int(opt('--start', 14))                  # the Embu's first 12 frames are black
    r1 = json.loads(Path(opt('--p1')).read_text())
    retargets = {1: r1}
    if opt('--p2'): retargets[2] = json.loads(Path(opt('--p2')).read_text())
    for spec in (opt('--behind') or '').split(';'):
        # A shot filmed with an actor right behind the camera: the PS1 engine
        # projects polygons behind the eye across the screen (the PS2 drops
        # them). Move that actor further back along the view, out of harm.
        if not spec: continue
        who, span, units, *ramp = spec.split(':'); a, b = map(int, span.split('-'))
        push_behind(retargets[int(who)], r1['camera'], a, b, float(units), int(ramp[0]) if ramp else 0)
    for spec in (opt('--hold-camera') or '').split(';'):
        if not spec: continue
        stop, ease, last = map(int, spec.split(':'))
        hold_camera(r1['camera'], stop, ease, last)
    script, end = clips(r1, r1['guest'], 0, start)
    cast, env = [f"hwoarang={r1['guest']}"], {}
    if opt('--p2'):
        r2 = retargets[2]
        key = opt('--p2-guest', r2['guest'])
        if opt('--p2-effect'): env['effect_p2'] = str(Path(opt('--p2-effect')).resolve())
        if opt('--p1-effect'): env['effect_p1'] = str(Path(opt('--p1-effect')).resolve())
        if opt('--effect-light'): env['effect_light'] = opt('--effect-light')      # RRGGBB
        # "first-last[:bone,bone,...]" in capture frames; T3 bones (y down):
        # 2 head, 3 neck, 4 / 7 shoulders, 5 / 8 elbows, 6 / 10 hands,
        # 12 / 15 hips, 13 / 16 knees, 14 / 17 feet. Scattered over the body.
        a = opt('--p2-aura')
        if a:
            span, _, bones = a.partition(':')
            aura = (*map(int, span.split('-')),
                    [int(b) for b in (bones or '2,13,6,17,3,10,16,5,14,7,12,8,15,4').split(',')])
        else:
            aura = None
        recs, end2 = clips(r2, key, 1, start, aura, int(opt('--aura-step', 4)))
        script += recs; end = max(end, end2)
        cast.append(f'paul={key}')
        if opt('--p2-models'):
            steps = [s.split('@') for s in opt('--p2-models').split(',')]
            # Capture frame f is Embu frame start + f; the first model from
            # the Embu's first frame, so the guest is grafted once with it.
            env['model_p2'] = ','.join(f'{p}@{start + int(f) if k else 0}' for k, (p, f) in enumerate(steps))
    # The Embu (state 6) lasts its own ~3 980 frames whatever the script and
    # its way out crashed after a converted ending in every variant tried:
    # the stock tail from frame 3146 keeps its closing state, a record never
    # reached keeps the interpreter from reading on, and the window stops
    # 2 s after the ending (end_at).
    script.sort(key=lambda x: (x[0], x[1]))
    # A fight's cutscene (TEKKEN3_FIGHT_CINE, tools/ps2/fight_ending.py):
    # the same records, ended by a -1 record once the fade to black (over
    # the held last pose) is done.
    cine = out.with_suffix('.cine.txt')
    fout = int(opt('--fade', '40:40').split(':')[1])
    cine.write_text(''.join(' '.join(map(str, r)) + '\n' for r in script + [[end + fout + 10, -1, 0, 0, 0]]))
    stock = json.loads((ROOT / 'workspace/embu/stock0.json').read_text())['stock']
    script += [rec for rec in stock if rec[0] >= max(3146, end + 1)]
    script.append([30000, 5, 8, 0, 0])
    # Model files staged under another prefix (--stage SRC:NAME, SRC without
    # its extension): Jin's own model next to the Devil Jin one.
    for pair in [a for i, a in enumerate(sys.argv) if i and sys.argv[i - 1] == '--stage']:
        src, name = pair.split(':')
        for ext in ('3dm', 'relocs', 'tim'):
            shutil.copyfile(f'{src}.{ext}', BUILD / f'{name}.{ext}')
    track = out.with_suffix('.camera.txt')
    # The Embu turns both its actors a quarter turn (0x4000 at +0x0E and
    # +0x2C): the clips, root and body, land turned by (x, z) -> (-z, x).
    # The camera turns with them.
    turn = lambda c: [-c[2], c[1], c[0], -c[5], c[4], c[3]]
    # From the Embu's first frame to the stop: the first shot held before the
    # ending starts, the last one after it (no stock shot on either side).
    cams = [r1['camera'][0]] * start + r1['camera']
    cams += [r1['camera'][-1]] * max(0, end + 1000 - len(cams))       # held past end_at (the window closes later), short of 3 500 (crashed there)
    track.write_text('0\n' + '\n'.join(' '.join(map(str, turn(c))) for c in cams) + '\n')
    # The Embu's fade (TEKKEN3_EMBU_FADE, -256 black, 0 the image): black
    # until the ending starts, then in over IN frames; out over OUT frames
    # once it is over (its last pose held), black to the Embu's end (the window closes a while
    # after end_at). Like TTT's.
    fin, fout = map(int, opt('--fade', '40:40').split(':'))
    if opt('--fade-capture'):
        # The PS2 cutscene's own fades, frame for frame; black before it, and
        # after it as it ended (black when it faded out, else out over OUT).
        levels = captured_fades(opt('--fade-capture'))
        spans = [f'0-{start - 1}:-256:-256'] + fade_segments(levels, start)
        if levels and levels[-1] <= -200: spans.append(f'{start + len(levels)}-{end + 1000}:-256:-256')
        else: spans += [f'{end}-{end + fout}:0:-256', f'{end + fout}-{end + 1000}:-256:-256']
        env['fade'] = ';'.join(spans)
        print(f'captured fades: {len(spans)} spans', flush=True)
    elif fin or fout:
        env['fade'] = (f'0-{start}:-256:-256;{start}-{start + fin}:-256:0;'
                       f'{end}-{end + fout}:0:-256;{end + fout}-{end + 1000}:-256:-256')
    # TTT's endings' music (tools/ps2/ttt_music.py), from the ending's start.
    if opt('--music'): env['music'] = f"{Path(opt('--music')).resolve()}@{start}"
    if opt('--arena'): env['arena'] = opt('--arena')   # its lighting only: the stage is in the Embu's file
    plan = dict(cast=','.join(cast), script=script, camera_track=str(track), end_at=end + 120, **env)
    plan['cine'] = str(cine)
    out.write_text(json.dumps(plan))
    print(f'plan {out}: {len(script)} records, ending over at Embu frame {end}; cast {plan["cast"]}')


if __name__ == '__main__':
    main()

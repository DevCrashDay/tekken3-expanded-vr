#!/usr/bin/env python3
"""Yoshimitsu's win poses light their effect on the blade (B48, GH#73).

    python tools/test_yoshimitsu_win_sparks.py --build build-rel-lite [--moves ttt1|t3]
        [--button square|triangle|cross|circle] [--tag NAME]

Cold start, Arcade selector: player 1 takes Yoshimitsu (TEKKEN3_NATIVE_MOVES=1
puts him on his TTT1 moves, 0 on T3's). Once the round runs, player 2's life
goes to 0 and --button is held through the K.O. and the replay: it picks the
win pose. The pose's timed properties 6..10 (T3 0x800458C8) light an effect
at the world position of the bone in their argument (actor + 0x908 + 68 * bone).

TTT1's poses (Square, Triangle, Cross; Circle lights nothing) light the
player's strong-hit effect on bone 22 at frames 190 / 205, the tip of
Yoshimitsu's sword in TTT1 (fighter + 0xA8C + 22 * 0x50). His T3 model has
the tip as bone 18 (the middle of the blade 19), which his own T3 poses light
at the same frames; bones 20..23 stay at (0, 0, 0), the arena's centre, where
the TTT1 poses lit theirs. The bench samples the pose's record and frame and,
on the effect's frame, the bone and the position it reads; it fails when the
point is not on the sword (300 to 1200 units from the hand, bone 10). On T3's
moves it prints his own pose as the reference.

Triangle plays TTT1's sitting pose (0x1293): the arcade starts the next round
at its frame 201, before its effect at 205, which is never seen there; T3
lets the pose run on and the spark stayed in the air as the sword swept down.
The native drops that pose's effect: the bench fails if it lights one.
"""
import argparse, os, shutil, socket, struct, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import test_devil_jin as T

T.WORK = ROOT / 'workspace/yoshimitsu-win-sparks'
YOSHIMITSU, HAND = 4, 10
BONES, BONE = 0x8f4, 68                 # bone matrices; translation at +0x14
SHOTS = tuple(int(x) for x in os.environ.get('B48_SHOTS', '3,8,14').split(','))   # frames after the effect
BUTTONS = dict(square=T.pad(T.SQ), triangle=T.pad(T.TR), cross=T.pad(T.CR), circle=T.pad(T.CI))


class Game(T.Game):
    """T.Game run from the build folder itself (Windows: no symbolic links
    without developer mode), cards in a fresh folder, headless software
    renderer. The build's settings.toml stays as it is."""
    def __init__(self, build, tag, env=None):
        cards = T.WORK / f'cards-{os.getpid()}-{tag}'
        shutil.rmtree(cards, ignore_errors=True); cards.mkdir(parents=True)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); self.port = listener.getsockname()[1]
        self.log_path = T.WORK / f'{tag}.log'
        full = {k: v for k, v in os.environ.items() if not k.startswith('TEKKEN3_')}
        full.update(env or {}); full['SDL_AUDIO_DRIVER'] = 'dummy'
        exe = next(p for p in (build / 'Tekken_3_Expanded.exe', build / 'Tekken_3_Expanded') if p.is_file())
        args = [str(exe), '--game', str(ROOT / 'game.toml'), '--disc', str(ROOT / 'disc/Tekken 3 (USA).cue'),
                '--no-launcher', '--renderer', 'software', '--headless',
                '--debug-port', str(self.port), '--memcard-dir', str(cards)]
        with self.log_path.open('w') as out:
            self.process = subprocess.Popen(args, cwd=ROOT, stdout=out, stderr=out, env=full)
        self.run, self.tag = cards, tag
        deadline = time.monotonic() + float(os.environ.get('GAME_START_TIMEOUT', 60))
        while True:
            if self.process.poll() is not None: raise RuntimeError(f'game exited; see {self.log_path}')
            try:
                with socket.create_connection(('127.0.0.1', self.port), .2): break
            except OSError:
                if time.monotonic() > deadline: raise RuntimeError('debug endpoint did not start')
                time.sleep(.1)

    def read(self, a, n):
        return bytes.fromhex(self.q(cmd='read_ram', addr=f'{a:08x}', len=n)['hex'])


def effect_props(g, record):
    """(frame, kind, bone) of the record's effect properties 6..10."""
    p = struct.unpack('<I', g.read(record + 32, 4))[0]
    out = []
    if not (0x80000000 <= p < 0x80200000 or 0x9f000000 <= p < 0xa0000000): return out
    for frame, op in struct.iter_unpack('<HH', g.read(p, 4 * 32)):
        if not frame: break
        if 6 <= op >> 8 <= 10: out.append((frame, op >> 8, op & 255))
    return out


def bone(g, actor, b):
    return struct.unpack('<3i', g.read(actor + BONES + b * BONE + 0x14, 12))


def hand_frame(g, actor):
    """Hand bone 10: rotation (rows, 4.12) and translation."""
    m = g.read(actor + BONES + HAND * BONE, 0x20)
    return struct.unpack('<9h', m[:18]), struct.unpack('<3i', m[0x14:0x20])


def in_hand_space(g, actor, point):
    r, t = hand_frame(g, actor)
    d = [point[i] - t[i] for i in range(3)]
    # The engine's matrix maps the bone's local axes to world (v = R l + t):
    # the transpose takes a world offset back to the hand's frame.
    return tuple(round(sum(r[j * 3 + i] * d[j] for j in range(3)) / 4096) for i in range(3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--moves', choices=('ttt1', 't3'), default='ttt1')
    ap.add_argument('--button', choices=BUTTONS, default='square')
    ap.add_argument('--tag', default=None)
    a = ap.parse_args()
    T.WORK.mkdir(parents=True, exist_ok=True)
    env = {'TEKKEN3_NATIVE_MOVES': '1' if a.moves == 'ttt1' else '0'}
    tag = a.tag or f'{a.moves}-{a.button}'
    g = Game(Path(a.build).resolve(), tag, env)
    actor = T.ACTOR
    try:
        T.to_selector(g)
        T.cursor_to(g, YOSHIMITSU)
        g.press(T.SQUARE, frames=12)
        def step():
            if g.value(T.STATE) == 9: g.press(T.SQUARE, frames=12)
        g.wait(lambda: g.value(T.STATE) == 8 and g.value(T.PHASE, 2) >= 8, 'round did not start', 150, step)
        assert T.actor(g, 0)[2] == YOSHIMITSU, f'P1 is {T.actor(g, 0)[2]}'
        time.sleep(1.5)
        g.q(cmd='write_ram', addr=f'{T.ACTOR + T.STRIDE + T.LIFE:08x}', bytes='0000')
        g.q(cmd='press', buttons=BUTTONS[a.button], frames=60 * 20)
        seen, fired, end = {}, [], time.monotonic() + 22
        last, shots = None, []          # pictures of the effect, a few frames on
        while time.monotonic() < end:
            record = struct.unpack('<I', g.read(actor + 0x54, 4))[0]
            frame = struct.unpack('<h', g.read(actor + 0x58, 2))[0]
            if record not in seen:
                seen[record] = effect_props(g, record)
                print(f'record {record:08X} effect properties {seen[record]}')
            for f, kind, b in seen[record]:
                key = (record, f)
                if f - 2 <= frame <= f + 2 and key not in {x[0] for x in fired}:
                    point = bone(g, actor, b)
                    local = in_hand_space(g, actor, point)
                    fired.append((key, kind, b, point, local, frame))
                    shots += [(record, f, f + d) for d in SHOTS]
                    print(f'  frame {frame} (property at {f}) kind {kind} bone {b}: world {point}, '
                          f'hand {bone(g, actor, HAND)}, in the hand frame {local}')
            for shot in [x for x in shots if x[0] == record and frame >= x[2]]:
                g.shot(f'{shot[1]}+{shot[2] - shot[1]:02d}'); shots.remove(shot)
            last = (record, frame)
        if a.moves == 'ttt1' and a.button == 'triangle':
            # The sitting pose: the arcade cuts it at frame 201, before its
            # effect at 205 (never seen there); the native drops it.
            if fired: raise SystemExit(f'FAIL: the sitting pose lights an effect: {fired}')
            print('PASS: the sitting pose lights no effect, as in the arcade'); return
        if not fired: raise SystemExit(f'FAIL: no effect property fired (last {last}); try another --button')
        bad = [x for x in fired if not 300 < sum(v * v for v in x[4]) ** .5 < 1200]
        if a.moves == 't3': return
        if bad: raise SystemExit(f'FAIL: effect away from the blade: {bad}')
        print('PASS: every effect of the pose lights on the blade')
    finally:
        g.q(cmd='clear_input') if g.process.poll() is None else None
        g.stop()


if __name__ == '__main__':
    main()

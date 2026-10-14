#!/usr/bin/env python3
"""A Tekken Tag Tournament (PS2) ending, from its capture to the cinematic
engine: one tool for every ending (docs/CINE-CONVERT.md).

    venv-import python tools/ps2/ttt_to_cine.py KEY [--capture CAPTURE.jsonl] [--elf SLUS_200.01]
        [--redo] [--check] [--out DIR]

KEY names the ending's declaration, tools/data/ttt_cinematics/KEY.ending.json
(written by hand, in git: the script scene, the T3 arena, the trigger, each
actor's cast, costume, TTT1 skeleton and retarget options, the elements
knowingly left out). From the capture (tools/ps2/ttt_capture.py, format 2),
the scene's script (tools/ps2/ttt_script.py, read in the game's executable)
and the retargets (tools/ps2/ttt_retarget.py, run here) this writes
tools/data/ttt_cinematics/KEY.json.gz, the data that ships (poses, camera,
fades, music start: no disc data), then tools/ttt_cinematics.py makes the
.cine from it at the player's setup. With --out DIR, also the .cine itself
into DIR (a test, e.g. build-rel-lite/mods/ttt-cinematics).

Everything on screen comes from the script or the capture, nothing by hand:
- the frames are the script's (cine frame = script frame "t"), the length
  the script's;
- the poses: each actor's retarget, by script frame (a frame the capture
  missed holds the one before it, more than MAX_GAP in a row is an error);
- the camera: actor 0's retarget (eye, target);
- the fingers: every finger node's rotation, every frame (fingers), whole:
  what each model shows of them is ttt_cinematics.py's (hand_steps);
- the fades: the script's (cmd 19, 20 black, 5, 6 white), checked against
  the captured fade quads (ttt_cine_report.py);
- the music: the frame of the capture's first music packet, which must be
  the endings' piece (MUSIC_PACKET; ttt_cinematics.py's music and its
  Tekken 3 fallback, XA 44);
- the cast: the script's actor list and the capture's fighters must agree
  with the declaration.

- the sparks (cmd 10): the strong-hit effect of actor a on actor c's T3
  bone (NODE_BONE: the PS2 node's nearest T3 bone), as a T3 timed property;
  their engine tasks (SPARK_TASK) are T3's own effect then;
- the sprites at a node (cmd 18): the puffs the command spawns (SPRITE_TASKS),
  each one's captured position frame by frame while it is drawn, in the
  retarget's frame like the camera; their look (radius, alpha ramps) is
  SPRITE_LOOK, measured on the PS2 (TIPS § 103), their colour the declaration's
  "sprite_colour" (additive).

What the engine cannot play yet is never dropped silently: character
changes in the scene (cmd 0 / 1 after frame 0, "relink"), Devil textures
(cmd 12), sprites between two nodes (17), props (25), coloured fades (21),
screen filters (22, 23, 24, 29, 30, 32), per-character states (15, 16,
28), flags (26, 31), figurants ("statics"), engine tasks other than the
fades and the stage's own (alive from the first frame to the last), voices,
other music. Each one is listed in workspace/ttt-ps2/KEY/convert.md and the
tool fails, unless the declaration's "ignore" names it ({"cmd": N, "t": T,
"why": ...} or {"task": "0x...", "why": ...}; the tasks born within two frames
of a left-out command go with it): then it is listed as left out.
--check stops after that report (no retarget, nothing written).
"""
import argparse, gzip, json, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import ttt_script, ttt_cine_report

DATA = ROOT / 'tools/data/ttt_cinematics'
WORK = ROOT / 'workspace/ttt-ps2'
IMPORT = ROOT / 'workspace/ttt1-import'
# The capture session's worktree (wip/ttt-capture-full): its captures and
# executable, read only, when this worktree has none of its own.
CAPTURE_TREE = ROOT.parent / 'Tekken3Recompiled-capture/workspace/ttt-ps2'
MUSIC_PACKET = '45804007'        # sound queue 0x40, piece 7: the endings' music, every ending's
MAX_GAP = 4
FADE_TASK = '0x3ac538'           # the black / white fades of cmd 5, 6, 19, 20
UNSUPPORTED = {                  # script command -> what it is (ttt_script.ENDING_COMMANDS)
    0: 'character change (actor 0 reloaded: instruction to come, docs/CINE-FORMAT.md)',
    1: 'character change (actor 1 reloaded: instruction to come, docs/CINE-FORMAT.md)',
    12: 'Devil textures (`devil`, reserved)',
    17: 'effect sprite between two nodes (not measured yet: Law, Bryan)',
    21: 'coloured fade (task 0x3AC728)', 25: 'held prop (scene model on a hand)',
    15: 'screen task 0x3AC4C0', 16: 'Devil / Angel state', 28: 'True Ogre / Devil state (wings)',
    22: 'screen filter 0x3AC818', 23: 'screen filter (memory / dream)', 24: 'screen filter 0x3ACA18',
    29: 'screen task 0x3AA420', 30: 'screen task 0x3AD140', 32: 'screen task 0x3AA378',
    26: 'flag gp-28292', 31: 'flag 0x34CAE0',
}
PLAYED = {2, 3, 4, 27, 5, 6, 19, 20, 10, 18}   # motions and camera (in the capture), fades, sparks, sprites (below)
SPARK_TASK = '0x346b60'          # cmd 10's hit effect: T3 draws its own (the `spark` property)
SPRITE_TASKS = {'0x3a0668'}      # cmd 18 (kind 3) puffs: position float x y z at +0x20, drawn while +0x74 != 0
EXPECTED_TASKS = {10: {SPARK_TASK}, 18: SPRITE_TASKS}
# PS2 node -> T3 bone (actor + 0x908 + 68 · bone; 0 the pelvis, 6 / 10 the hands):
# 79 is the legs' root, at the hips. A node not here stops the conversion.
NODE_BONE = {0: 0, 1: 0, 79: 0}
# TTT's kind 3 puff, measured on the PS2 (Kunimitsu's ending, one puff alone
# against none, TIPS § 103): about 500 units of radius, a soft disc, drawn
# while flying (landed: gone); alpha ramps so that none pops.
SPRITE_LOOK = dict(radius=500, alpha=256, ramp_in=4, ramp_out=8)


def first_file(*paths):
    for p in paths:
        if p and Path(p).is_file(): return Path(p)
    return None


def load_capture(path):
    """Header, frames by script frame (the last 'done' line and 'relink'
    lines apart), relinks."""
    lines = path.read_text().splitlines()
    head, by_t, relinks, done = json.loads(lines[0]), {}, [], None
    for line in lines[1:]:
        d = json.loads(line)
        if 'relink' in d: relinks.append(d)
        elif d.get('done'): done = d
        elif d.get('actors') and 't' in d: by_t.setdefault(d['t'], d)
    return head, by_t, relinks, done


def check(key, e, scene, head, by_t, relinks, capture_name):
    """What the scene holds that the engine cannot play: (problems, left out
    on purpose, notes), each a report line."""
    problems, left_out, notes = [], [], []
    ignore = e.get('ignore', [])
    def ignored(**k):
        return next((i for i in ignore if all(i.get(a) == b for a, b in k.items())), None)
    def problem(text, also=None, **k):
        i = ignored(**k) or (also and ignored(**also))
        (left_out if i else problems).append(text + (f" — left out: {i['why']}" if i else ''))

    # The cast: the script's list entries at frame 0 against the declaration.
    entries = {a['entry']: a for a in scene['actors']}
    cast = {}
    for ev in scene['events']:
        if ev['cmd'] in (0, 1) and ev['t'] == 0: cast[ev['cmd']] = entries.get(ev['a'])
    for i, a in enumerate(e['actors']):
        s = cast.get(i)
        if not s: problems.append(f'actor {i}: no cast at frame 0 in the script'); continue
        notes.append(f"actor {i}: script {s['key']} (TTT costume {s['costume']}) -> .cine {a['cast']} costume {a['costume']}"
                     + (f" ({a['why']})" if a.get('why') else ''))
        if s['key'] != a['cast'] and not a.get('cast_of'):
            problems.append(f"actor {i}: the script casts {s['key']}, the declaration {a['cast']} (give \"cast_of\" to mean it)")
    if len(e['actors']) != len(cast):
        problems.append(f"{len(cast)} actors in the script, {len(e['actors'])} declared")

    # Script commands.
    for ev in scene['events']:
        c, t = ev['cmd'], ev['t']
        if c in (0, 1) and t == 0: continue
        if c == 10 and ev['b'] not in NODE_BONE:
            problems.append(f"script {t}: cmd 10 on PS2 node {ev['b']}: no T3 bone for it (NODE_BONE)")
        if c in PLAYED: continue
        problem(f"script {t}: cmd {c} ({UNSUPPORTED.get(c, 'unknown command')}) a={ev['a']} b={ev['b']} c={ev['c']}", cmd=c, t=t)

    # Capture: skeletons, figurants, tasks, voices, music.
    for r in relinks:
        problem(f"capture {r.get('t', r.get('relink'))}: skeleton change ({r.get('nodes')} nodes)", relink=r.get('t', r.get('relink')))
    ts = sorted(by_t)
    statics = [t for t in ts if by_t[t].get('statics')]
    if statics: problem(f'capture: figurants at {statics[:10]}{" …" if len(statics) > 10 else ""}', figurants=True)
    alive, life = {}, []
    for t in ts:
        d = by_t[t]
        if 'tasks' not in d: continue
        now = {a: f for a, f in d['tasks']}
        for a, f in list(alive.items()):
            if a not in now or now[a] != f[0]: life.append((f[0], f[1], t - 1)); del alive[a]
        for a, f in now.items():
            if a not in alive: alive[a] = (f, t)
    life += [(f, t0, None) for f, t0 in alive.values()]
    stage_tasks, bursts = {}, {}
    for kind, t0, t1 in life:
        if kind == FADE_TASK: continue
        if t0 <= ts[0] and t1 is None: stage_tasks[kind] = stage_tasks.get(kind, 0) + 1; continue
        bursts[kind, t0, t1] = bursts.get((kind, t0, t1), 0) + 1
    # One line per kind and cause: a burst born just after a script command is
    # most likely that command's; bursts without one (a stage's ambience
    # spawned all along: Baek's and Wang's 0x3A0850) are grouped.
    groups = {}
    for (kind, t0, t1), count in sorted(bursts.items(), key=lambda kv: kv[0][1]):
        cause = next((ev for ev in scene['events'] if 0 <= t0 - ev['t'] <= 2 and (ev['cmd'] not in PLAYED or ev['cmd'] in EXPECTED_TASKS)), None)
        g = groups.setdefault((kind, cause and (cause['t'], cause['cmd'])), [0, 0, t0, t1])
        g[0] += count; g[1] += 1; g[3] = None if t1 is None or g[3] is None else max(g[3], t1)
    for (kind, cause), (count, n, t0, t1) in groups.items():
        if cause and kind in EXPECTED_TASKS.get(cause[1], ()):
            notes.append(f"capture {t0}-{t1 if t1 is not None else 'end'}: engine task {kind} ×{count} of script {cause[0]} cmd {cause[1]}: "
                         + ("T3's own hit effect (`spark`)" if cause[1] == 10 else 'its puffs (`sprite`)'))
            continue
        problem(f"capture {t0}-{t1 if t1 is not None else 'end'}: engine task {kind} ×{count}"
                + (f' in {n} bursts' if n > 1 else '')
                + (f" (after script {cause[0]} cmd {cause[1]})" if cause else ' (no script command)'), task=kind,
                also=dict(t=cause[0], cmd=cause[1]) if cause else None)
    if stage_tasks:
        notes.append('the stage\'s own tasks (alive all along; the T3 arena brings its own): '
                     + ', '.join(f'{k} ×{n}' for k, n in sorted(stage_tasks.items())))
    voices = [t for i, t in enumerate(ts[1:], 1) if by_t[t].get('voices') and by_t[ts[i - 1]].get('voices')
              and any(a[:3] != b[:3] and b[1] for a, b in zip(by_t[ts[i - 1]]['voices'], by_t[t]['voices']))]
    if voices: problem(f'capture: sounds started at {voices[:10]}', voices=True)
    music = [(t, by_t[t]['music']) for t in ts if 'music' in by_t[t]]
    if not music: problems.append('capture: no music packet')
    for t, m in music:
        if m != MUSIC_PACKET: problem(f'capture {t}: music packet {m}, not the endings\' piece {MUSIC_PACKET}', music=m)

    # Frames the capture missed.
    missing = [t for t in range(ts[0], ts[-1] + 1) if t not in by_t]
    run = longest = 0
    for t in range(ts[0], ts[-1] + 1):
        run = run + 1 if t not in by_t else 0; longest = max(longest, run)
    if longest > MAX_GAP: problems.append(f'capture {capture_name}: {longest} script frames missed in a row (more than {MAX_GAP})')
    notes.append(f"capture {capture_name}: script frames {ts[0]}..{ts[-1]} of 0..{scene['length'] - 1}, "
                 f"{len(missing)} missed inside" + (f' (first {missing[:8]})' if missing else '')
                 + (f', frames 0..{ts[0] - 1} hold the first pose' if ts[0] else ''))
    if ts[-1] < scene['length'] - 1:
        notes.append(f"frames {ts[-1] + 1}..{scene['length'] - 1} hold the last pose")
    return problems, left_out, notes


def fades(scene, by_t):
    """The script's fades as (from, to, a, b), and the report lines that
    check them against the captured quads."""
    out = [tuple(f) for f in ttt_cine_report.script_fades(scene)]
    ts = sorted(by_t)
    for t in ts: by_t[t].setdefault('fade', [0, 0, 0])
    whites = [(by_t[t]['fade'][1] >> 24) & 0xFF for t in ts]
    levels = []
    for k, t in enumerate(ts):
        f = by_t[t]['fade']
        moving = any(whites[j] != whites[k] for j in (k - 1, k + 1) if 0 <= j < len(ts))
        black = (f[0] >> 24) & 0xFF
        levels.append(2 * whites[k] if moving and f[2] and whites[k] > 2 else (-2 * black if black > 2 else 0))
    seen = ttt_cine_report.spans(levels, ts)
    lines, problems = [], []
    for a, b, x, y in out:
        hit = [m for m in seen if m[0] <= b + 3 and m[1] >= a - 3]
        lines.append(f"fade {a}-{b} {x} -> {y}: captured {', '.join(f'{m[0]}-{m[1]} (peak {m[2]})' for m in hit) or 'NOT SEEN'}")
        if not hit and a > ts[0]: problems.append(f'script fade {a}-{b} not seen in the capture')
    for m in seen:
        if not any(m[0] <= b + 3 and m[1] >= a - 3 for a, b, _, _ in out):
            problems.append(f'captured fade {m[0]}-{m[1]} (peak {m[2]}) without a script command')
    return out, lines, problems


def retarget(key, e, capture, redo):
    """Each actor's retarget (ttt_retarget.py), kept in workspace/ttt-ps2/KEY/."""
    folder = WORK / key; folder.mkdir(parents=True, exist_ok=True)
    outs = []
    for i, a in enumerate(e['actors']):
        out = folder / f"{a['cast']}-{i}-ttt1.json"
        outs.append(out)
        if out.is_file() and not redo and out.stat().st_mtime > capture.stat().st_mtime: continue
        skel = a.get('skeleton', a['cast'])
        args = [sys.executable, str(HERE / 'ttt_retarget.py'), str(capture), str(i), skel, str(out)]
        if a.get('idle'): args += ['--idle', str(IMPORT / a['idle'])]
        if a.get('model'): args += ['--model', str(IMPORT / a['model'])]
        args += [str(outs[0]) if v == '{actor0}' else v for v in a.get('retarget', [])]
        print('retarget:', ' '.join(args[2:]), flush=True)
        with (folder / f'retarget-{i}.log').open('w') as log:
            subprocess.run(args, check=True, stdout=log, stderr=subprocess.STDOUT)
        print((folder / f'retarget-{i}.log').read_text().strip().splitlines()[-1], flush=True)
    return [json.loads(p.read_text()) for p in outs]


def sparks(scene):
    """cmd 10 as [frame, actor, T3 bone, kind]: kind = the actor whose effect it is."""
    return [[ev['t'], ev['c'] & 1, NODE_BONE[ev['b']], ev['a'] & 1] for ev in scene['events'] if ev['cmd'] == 10]


def sprites(scene, by_t, calib):
    """Each cmd 18's puffs: one block {first, rows: [[x, y, z, alive] per puff]},
    positions through the retarget's calibration (to_t3 of ttt_retarget.py)."""
    import numpy as np, struct
    C, scale, origin = np.array(calib['C']), calib['scale'], np.array(calib['origin'])
    ts = sorted(by_t); out = []
    for ev in (ev for ev in scene['events'] if ev['cmd'] == 18):
        mine, rows, first = [], [], None
        for t in ts:
            if t < ev['t']: continue
            td = {a: bytes.fromhex(v) for a, v in by_t[t].get('task_data', {}).items()}
            live = {a for a, b in td.items() if f'0x{struct.unpack_from("<I", b, 8)[0]:x}' in SPRITE_TASKS}
            if t <= ev['t'] + 2: mine += sorted(a for a in live if a not in mine)
            if not mine: continue
            if not live & set(mine) and t > ev['t'] + 2: break
            first = t if first is None else first
            row = []
            for a in mine:
                b = td.get(a)
                if b is None or a not in live: row.append(None); continue
                p = np.array(struct.unpack_from('<3f', b, 0x20)); drawn = struct.unpack_from('<f', b, 0x74)[0] != 0
                q = scale * (C @ (p - origin))
                row.append([round(v) for v in q] + [int(drawn)])
            while rows and len(rows) + first < t: rows.append(rows[-1])          # a missed frame holds the one before
            rows.append(row)
        if mine: out.append(dict(cmd_t=ev['t'], first=first, rows=rows))
    return out


# The hands, kept whole: each PS2 hand has five fingers of three nodes after
# its wrist (38 and 63, tools/ps2/ttt_retarget.py), the thumb first, each
# node's parent the one before it (the first one's the wrist). The data keeps,
# for every frame, each finger node's rotation in its parent's frame (a unit
# quaternion w x y z, x 10000) and, once, its offset from the parent (the
# median: the bones do not stretch), the opening and closing of every finger
# as the PS2 plays it. What a model can show of it (a few poses from open to
# fist, no finger of its own) is ttt_cinematics.py's, per model (hand_steps).
WRISTS = (38, 63)
FINGER_NODES = 15


def quaternion(R):
    """w x y z of a rotation matrix (column vectors)."""
    import numpy as np
    w = np.sqrt(max(0.0, 1 + R[0, 0] + R[1, 1] + R[2, 2])) / 2
    x = np.copysign(np.sqrt(max(0.0, 1 + R[0, 0] - R[1, 1] - R[2, 2])) / 2, R[2, 1] - R[1, 2])
    y = np.copysign(np.sqrt(max(0.0, 1 - R[0, 0] + R[1, 1] - R[2, 2])) / 2, R[0, 2] - R[2, 0])
    z = np.copysign(np.sqrt(max(0.0, 1 - R[0, 0] - R[1, 1] + R[2, 2])) / 2, R[1, 0] - R[0, 1])
    q = np.array([w, x, y, z]); return q / np.linalg.norm(q)


def fingers(head, by_t, actor, frames):
    """{wrist: {offsets: [[x, y, z] per node], rotations: [[w x y z per node] per frame]}}
    of a PS2 actor, by script frame over `frames` (a missed frame holds the one before,
    the frames before the first captured one take it)."""
    import numpy as np
    parents = head['parents'][actor]
    out = {}
    for wrist in WRISTS:
        nodes = list(range(wrist + 1, wrist + 1 + FINGER_NODES))
        if len(parents) <= nodes[-1] or any(parents[n] != (wrist if (n - wrist - 1) % 3 == 0 else n - 1) for n in nodes):
            raise SystemExit(f'actor {actor}: nodes {nodes[0]}..{nodes[-1]} are not five fingers on wrist {wrist}')
        rows, offsets, last = [], [], None
        for t in frames:
            d = by_t.get(t)
            if d:
                m = d['actors'][actor]
                world = lambda k: np.array(m[k], float).reshape(4, 4)   # row vectors: v' = v M
                row = []
                for n in nodes:
                    local = world(n) @ np.linalg.inv(world(parents[n]))
                    u, _, vt = np.linalg.svd(local[:3, :3].T)            # column vectors, orthonormal
                    row += [round(float(v) * 10000) for v in quaternion(u @ vt)]
                    offsets.append(local[3, :3])
                last = row
            rows.append(last)
        first = next(r for r in rows if r)
        rows = [r or first for r in rows]
        median = np.median(np.array(offsets).reshape(-1, FINGER_NODES, 3), axis=0)
        out[str(wrist)] = dict(offsets=[[round(float(v), 3) for v in o] for o in median], rotations=rows)
    return out


def by_frame(r, field, frames):
    """r[field] by script frame over `frames`, gaps holding the frame before."""
    have = dict(zip(r['frames'], r[field]))
    out, last = [], None
    for t in frames:
        last = have.get(t, last)
        out.append(last)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('key')
    ap.add_argument('--capture', type=Path, help='default: workspace/ttt-ps2/full/KEY.jsonl here, else in the capture worktree')
    ap.add_argument('--elf', type=Path, help='SLUS_200.01; default: workspace/ttt-ps2/ here, else in the capture worktree')
    ap.add_argument('--redo', action='store_true', help='retarget again even if the retargets are newer than the capture')
    ap.add_argument('--check', action='store_true', help='the report only')
    ap.add_argument('--out', type=Path, help='also write KEY.cine there (a test)')
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(encoding='utf-8')
    key = a.key
    e = json.loads((DATA / f'{key}.ending.json').read_text(encoding='utf-8'))
    capture = first_file(a.capture, WORK / f'full/{key}.jsonl', CAPTURE_TREE / f'full/{key}.jsonl')
    elf = first_file(a.elf, WORK / 'SLUS_200.01', CAPTURE_TREE / 'SLUS_200.01')
    if not capture: raise SystemExit(f'{key}: no capture (workspace/ttt-ps2/full/{key}.jsonl, here or in {CAPTURE_TREE})')
    if not elf: raise SystemExit('no SLUS_200.01 (workspace/ttt-ps2/, copied from the TTT disc)')
    scene = ttt_script.find(ttt_script.scenes(ttt_script.Elf(elf)), e['scene'])
    head, by_t, relinks, _ = load_capture(capture)
    if head.get('version') != 2: raise SystemExit(f'{capture}: capture format {head.get("version")}, 2 needed (ttt_capture.py)')

    problems, left_out, notes = check(key, e, scene, head, by_t, relinks, capture.name)
    fade_list, fade_lines, fade_problems = fades(scene, by_t)
    problems += fade_problems
    music = next((t for t in sorted(by_t) if by_t[t].get('music') == MUSIC_PACKET), None)
    report = [f"# {key}: scene {scene['scene']} ({scene['key']}), {scene['length']} frames, TTT stage {scene['stage']} -> T3 arena {e['arena']}", '',
              '## Not convertible yet' + ('' if problems else ': nothing'), ''] + [f'- {p}' for p in problems]
    report += ['', '## Left out on purpose (the declaration\'s "ignore")' + ('' if left_out else ': nothing'), ''] + [f'- {p}' for p in left_out]
    report += ['', '## Converted', ''] + [f'- {n}' for n in notes] + [f'- {l}' for l in fade_lines]
    report += [f'- music: the endings\' piece from frame {music}' if music is not None else '- music: none']
    (WORK / key).mkdir(parents=True, exist_ok=True)
    (WORK / key / 'convert.md').write_text('\n'.join(report) + '\n', encoding='utf-8')
    print('\n'.join(report))
    if problems:
        raise SystemExit(f'\n{key}: {len(problems)} element(s) the engine cannot play yet (above, and in {WORK / key / "convert.md"}): '
                         'convert them in the engine first, or name them in the declaration\'s "ignore" with why')
    if a.check: return

    rs = retarget(key, e, capture, a.redo)
    first, last = min(by_t), max(by_t)
    frames = list(range(first, last + 1))
    fits = [max(r['fit_deg']) for r in rs if r.get('fit_deg')]
    data = dict(
        about=f"{e['about']} Data of tools/ps2/ttt_to_cine.py from capture {capture.name} and the scene's script "
              f"(retargets: {'; '.join(' '.join(x.get('retarget', [])) or 'stance' for x in e['actors'])}). "
              'Poses on the TTT1 arcade skeletons (57 channels), camera eye and target, both in the retarget\'s frame; '
              'frames are the script\'s.',
        version=2, key=key, length=scene['length'], first=first,
        actors=[dict(cast=x['cast'], costume=x['costume'], poses=by_frame(r, 'poses', frames),
                     side=r['calibration'].get('side', 0), fingers=fingers(head, by_t, i, frames))
                for i, (x, r) in enumerate(zip(e['actors'], rs))],
        camera=by_frame(rs[0], 'camera', frames),
        fades=[list(f) for f in fade_list], music=music,
        sparks=sparks(scene), sprites=sprites(scene, by_t, rs[0]['calibration']),
        sprite_look=dict(SPRITE_LOOK, radius=round(SPRITE_LOOK['radius'] * rs[0]['calibration']['scale']),
                         colour=e.get('sprite_colour', 'ffffff')))
    (DATA / f'{key}.json.gz').write_bytes(gzip.compress(json.dumps(data, separators=(',', ':')).encode(), 9, mtime=0))
    print(f"{DATA / f'{key}.json.gz'}: {len(frames)} frames from {first}, worst retarget segment {max(fits):.1f} degrees")
    if a.out:
        sys.path.insert(0, str(ROOT / 'tools'))
        import ttt_cinematics
        a.out.mkdir(parents=True, exist_ok=True)
        ttt_cinematics.converted_cine(key, e, data, a.out)
        print(a.out / f'{key}.cine')


if __name__ == '__main__':
    main()

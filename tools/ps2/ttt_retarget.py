#!/usr/bin/env python3
"""A captured TTT (PS2) ending actor, retargeted to its TTT1 arcade skeleton:
TTT1 poses (57 channels a frame) and the ending's camera, in one frame of
reference for Tekken 3.

    ~/TekkenProject/venv-import/bin/python3 tools/ps2/ttt_retarget.py \\
        CAPTURE.jsonl ACTOR GUEST OUT.json [--model 3DM --idle POSES] [--calib OTHER.json [--own-origin] [--own-head]] [--calib-frame S:F] [--heads-only]
        [--basis canonical[:YAW]] [--contacts] [--twists] [--collars] [--head-fit]
        # e.g. kazuya-cap2.jsonl 0 kazuya kazuya-ttt1.json
        #      kazuya-cap2.jsonl 1 jin jin-ttt1.json --model jin-plain/Jin-TTT1-arcade-P1.3dm
        #        --idle .../DevilJin-TTT1-idle.poses --calib kazuya-ttt1.json

The head follows the PS2 head's whole orientation (the segments leave it
free), through a constant bone basis found on the stance like C.
--heads-only patches an existing OUT.json's heads without solving again.

--calib-frame S:F calibrates on capture sequence S frame F instead of the
last frame, for an ending that does not end in stance (Bruce: 0:328, 0.9
degrees from his TTT1 stance; his last frame is 28 degrees off).

--calib takes the basis, scale and origin of another actor's retarget: a
second actor of the same ending that does not end in stance (Jin) shares
the first one's frame of reference. With --own-origin only its basis, scale
and head basis are taken, the origin is this capture's: an ending whose
actor never quite takes its stance (Armor King: 6.5 degrees at best) uses
another ending's calibration. Every ending so far had the same basis up to
a turn about the vertical (y flipped, scale 1.000). --own-head computes the
head basis on this actor's stance frame (--calib-frame) through the borrowed
basis: another character's head basis can be far off (Bruce's is 125 degrees
from Kunimitsu's, whose own stance tilts the world 6 degrees at best).

--basis canonical[:YAW] needs no stance at all: the basis every measured
ending had, C = Ry(YAW degrees) diag(-1, -1, 1), scale 1, the PS2 nodes'
left and right as Kazuya's (a stance fits only that way), the head basis
the identity (Kazuya's and Bruce's measured ones, within a degree), the
origin under actor 0's first root. For endings whose actors never take
their stance (Michelle's: seated all along, 43 and 23 degrees at best).
With --calib it is the other actor's that counts (same scene: same basis).

--contacts: the arms keep their offsets from the legs where they are near
them on the PS2 (contacts(): a hand on a knee, arms round the knees), not
only the segments' directions; for seated or crouched actors whose TTT1
proportions push the arms into the legs (Michelle's ending).

The output lists, beside the poses and the camera, each pose's script
frame (`frames`: the capture's "t"; capture lines without one, "relink"
and the scene's last "done" line, are left out) and its fit (`fit_deg`: the
worst segment's angle to the PS2 one).

Method of tools/t5/t5_convert.py (devil-jin branch): for each frame, the
TTT1 angles whose skeleton (the guest's TTT1 model) points its segments
(spine, thighs, shins, upper arms, forearms) the way the PS2 displayed
nodes do, by Levenberg-Marquardt from the previous frame, pulled softly
towards the guest's stance for what points leave free (twists).

Calibration: the ending's last frame is the fighter in stance, taken to be
the first frame of its TTT1 stance (<Name>-TTT1-idle.poses): it gives the
PS2 -> TTT1 basis C (both left/right assignments are tried) and the scale
(ratio of the segment lengths). The root and the camera go through the same
C and scale, relative to the actor's root at the first frame.

PS2 nodes (Kazuya's ending, both actors): 1 hips, 5 neck, 34 / 59 elbows,
38 / 63 wrists, 84 / 93 knees, 87 / 96 ankles (tools/ps2/ttt_capture.py).
"""
import json, math, sys
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path[:0] = [str(ROOT / 'tools/ttt1'), str(ROOT / 'tools/ttt1/model')]
import anim_model as AM
from fmt import load, row

WORK = ROOT / 'workspace/ttt1-import'
K = 2 * math.pi / 65536
PS2 = [dict(hip=1, neck=5, elbowA=34, elbowB=59, wristA=38, wristB=63, kneeA=84, kneeB=93, ankleA=87, ankleB=96, shoulderA=31, shoulderB=56),
       dict(hip=1, neck=5, elbowA=59, elbowB=34, wristA=63, wristB=38, kneeA=93, kneeB=84, ankleA=96, ankleB=87, shoulderA=56, shoulderB=31)]
TTT1_ROW = dict(hip=0, neck=19, elbowA=13, elbowB=17, wristA=14, wristB=18, kneeA=6, kneeB=9, ankleA=7, ankleB=10, shoulderA=12, shoulderB=16)
SEGMENTS = [('hip', 'neck'), ('hip', 'kneeA'), ('hip', 'kneeB'), ('kneeA', 'ankleA'), ('kneeB', 'ankleB'),
            ('neck', 'elbowA'), ('neck', 'elbowB'), ('elbowA', 'wristA'), ('elbowB', 'wristB')]
# What the solve fits: the segments, and with --twists the shoulder line too
# (the chest's roll about the spine, free otherwise: Kunimitsu's and
# Yoshimitsu's heads came out on a chest turned off the PS2's, necks broken).
SOLVED = list(SEGMENTS)


def s16(v): return ((int(v) + 32768) % 65536) - 32768
def unit(v): return v / np.linalg.norm(v)


class Skeleton:
    """A guest's TTT1 skeleton: world points of the rows for 54 joint channels."""
    def __init__(self, model):
        self.m = load(str(model))
        self.off = {r: np.array(row(self.m, r)[3:6], dtype=float) for r in AM.ROW_PARENT}
        self.bone = {r: AM.PART_BONE[AM.P2R.index(r)] for r in AM.ROW_PARENT}

    @staticmethod
    def rotation(a):        # tools/ttt1/anim_model.py rotation(), in floats: Rz(z) Ry(-y) Rx(-x)
        x, y, z = -a[0] * K, -a[1] * K, a[2] * K
        cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
        return np.array([[cy * cz, sx * sy * cz - cx * sz, sx * sz + cx * sy * cz],
                         [cy * sz, cx * cz + sx * sy * sz, cx * sy * sz - sx * cz],
                         [-sy, sx * cy, cx * cy]])

    def points(self, joints):
        local = [self.rotation(joints[3 * b:3 * b + 3]) for b in range(18)]
        local[0][1:, :] *= -1
        world = {0: (local[0], np.zeros(3))}
        def get(r):
            if r not in world:
                R, T = get(AM.ROW_PARENT[r])
                world[r] = (R @ local[self.bone[r]], T + R @ self.off[r])
            return world[r]
        for r in AM.ROW_PARENT: get(r)
        return {k: world[r][1] for k, r in TTT1_ROW.items()}

    def rotations(self, joints):
        """World rotation of each row (as points() builds them)."""
        local = [self.rotation(joints[3 * b:3 * b + 3]) for b in range(18)]
        local[0][1:, :] *= -1
        world = {0: local[0]}
        def get(r):
            if r not in world: world[r] = get(AM.ROW_PARENT[r]) @ local[self.bone[r]]
            return world[r]
        for r in AM.ROW_PARENT: get(r)
        return world

    def directions(self, joints):
        P = self.points(joints)
        return [unit(P[b] - P[a]) for a, b in SEGMENTS]

    def lengths(self, joints):
        P = self.points(joints)
        return [np.linalg.norm(P[b] - P[a]) for a, b in SEGMENTS]


ARMS = ('elbowA', 'elbowB', 'wristA', 'wristB')
LEGS = ('kneeA', 'kneeB', 'ankleA', 'ankleB', 'thighA', 'thighB', 'shinA', 'shinB')
MIDS = dict(thighA=('hip', 'kneeA'), thighB=('hip', 'kneeB'), shinA=('kneeA', 'ankleA'), shinB=('kneeB', 'ankleB'))
CONTACT = 300.0                     # PS2 distance (TTT1 units) under which an arm keeps its place by a leg


def with_mids(P):
    P = dict(P)
    for k, (a, b) in MIDS.items(): P[k] = (P[a] + P[b]) / 2
    return P


def contacts(points):
    """--contacts: each arm point (elbows, wrists) near a leg point (knees,
    ankles, thigh and shin middles) on the PS2 keeps its offset from it
    (TTT1 units), weighted by nearness. The segments' directions alone keep
    the limbs' angles, not where a hand is on a knee: with the TTT1 model's
    longer thighs (Michelle's: 70 more) her arms went into her legs."""
    P = with_mids(points)
    out = []
    for a in ARMS:
        for b in LEGS:
            v = P[a] - P[b]; d = float(np.linalg.norm(v))
            if d < 1.5 * CONTACT: out.append((a, b, v, math.exp(-(d / CONTACT) ** 2)))
    return out


# Per joint channel pull towards the stance. --collars: the collarbones (bones
# 3 and 7) COLLAR_PULL times stiffer: from the neck-to-elbow segments alone the
# solve swung them 80 to 100 degrees off the stance in Kunimitsu's ending
# (Kazuya's: 26 to 37), and the shoulder mesh went with them, the upper arm
# hanging while the forearm was raised.
PULL = np.ones(54)
COLLAR_PULL = 20.0


def solve(sk, target, start, prior, weight=0.05, iterations=30, near=()):
    """Levenberg-Marquardt on the segment directions (and the arms' offsets
    from the legs `near`, see contacts()), from `start`."""
    def residual(p):
        P = sk.points(p)
        r = [m - t for m, t in zip([unit(P[b] - P[a]) for a, b in SOLVED], target)]
        if near:
            P = with_mids(P)
            r += [w * ((P[a] - P[b]) - v) / CONTACT for a, b, v, w in near]
        return np.concatenate(r + [weight * PULL * (p - prior) / 8192.0])
    p, mu = start.astype(float).copy(), 1e-3
    for _ in range(iterations):
        r = residual(p); J = np.empty((len(r), len(p)))
        for i in range(len(p)):
            q = p.copy(); q[i] += 16.0; J[:, i] = (residual(q) - r) / 16.0
        A, g = J.T @ J, J.T @ r
        while True:
            step = -np.linalg.solve(A + mu * np.diag(np.diag(A) + 1e-9), g)
            if np.sum(residual(p + step) ** 2) < np.sum(r ** 2): p = p + step; mu = max(mu / 3, 1e-7); break
            mu *= 4
            if mu > 1e6: return p
        if np.linalg.norm(step) < 1: break
    return p


def fresh(sk, target, prior, start=None, near=()):
    """The least twisted solve of a frame: from the stance and from the hips
    turned a quarter, a half, three quarters (and from `start`, the previous
    frame's), the fit within twice the best one that stays nearest the
    stance. From the stance alone, an actor seen from behind (Bruce's
    ending opens on his back) settles in a twisted pose that fits the
    segments as well, until he turns round."""
    tries = []
    for axis, off in [(0, 0)] + [(a, o) for a in range(3) for o in (16384, 32768, -16384)]:
        s = prior.copy(); s[axis] += off
        tries.append(solve(sk, target, s, prior, near=near))
    if start is not None: tries.append(solve(sk, target, start, prior, near=near))
    fit = [float(np.sum(np.concatenate([m - t for m, t in zip(sk.directions(q), target)]) ** 2)) for q in tries]
    good = [q for q, f in zip(tries, fit) if f <= 2 * min(fit) + 1e-6]
    return min(good, key=lambda q: float(np.sum((q - prior) ** 2)))


def euler(M):
    """Angles (16-bit units) of a rotation matrix, for Skeleton.rotation:
    Rz(z) Ry(-y) Rx(-x) (tools/t5/t5_convert.py)."""
    y = math.asin(max(-1.0, min(1.0, -M[2, 0])))
    return np.array([-math.atan2(M[2, 1], M[2, 2]), -y, math.atan2(M[1, 0], M[0, 0])]) / K


HEAD_NODE, HEAD_ROW, CHEST_ROW, HEAD_BONE = 6, 19, 1, 2


def ps2_rotation(d, actor, node):
    """A node's world rotation, column vectors (the PS2 matrices are rows)."""
    return np.array(d['actors'][actor][node]).reshape(4, 4)[:3, :3].T


def heads(sk, frames, actor, poses, C, B):
    """The head from the PS2 head's whole orientation, twist included (the
    segments leave it free): world target C R B, local = chest^T target."""
    for d, q in zip(frames, poses):
        joints = np.array([s16(v) for v in q[3:]], dtype=float)
        chest = sk.rotations(joints)[CHEST_ROW]
        local = chest.T @ (C @ ps2_rotation(d, actor, HEAD_NODE) @ B)
        q[3 + 3 * HEAD_BONE:6 + 3 * HEAD_BONE] = [int(round(v)) & 0xffff for v in euler(local)]


# --twists: TTT1 arm rows and the PS2 nodes they follow (Kunimitsu's ending,
# both sides, as PS2 and TTT1_ROW): upper arm, forearm (the twist about the
# segment only: its direction is the solve's) and hand (its whole orientation).
TWIST_ROWS = [dict(upper=(12, 31), fore=(13, 34), hand=(14, 38)), dict(upper=(16, 56), fore=(17, 59), hand=(18, 63))]


def twists(sk, frames, actor, poses, C, bases, side):
    """The segments leave each arm's roll free: the solve kept the stance's,
    and a rigid TTT1 arm rolled 60 to 90 degrees off the PS2's tore the mesh
    at the shoulder and the elbow (Kunimitsu's ending in the game; the bones
    view cannot show it). Each upper arm and forearm takes the PS2 roll about
    its own segment (target C R B, B its constant bone basis on the
    calibration frame, as the head's), each hand the PS2 orientation."""
    rows = TWIST_ROWS if side == 0 else TWIST_ROWS[::-1]
    for d, q in zip(frames, poses):
        joints = np.array([s16(v) for v in q[3:]], dtype=float)
        for arm, k in zip(rows, range(2)):
            for part in ('upper', 'fore', 'hand'):
                r, n = arm[part]; n = TWIST_ROWS[k][part][1]
                W = sk.rotations(joints)
                target = C @ ps2_rotation(d, actor, n) @ bases[(r, n)]
                parent = W[AM.ROW_PARENT[r]]
                b = sk.bone[r]
                if part == 'hand': joints[3 * b:3 * b + 3] = euler(parent.T @ target); continue
                child = {12: 13, 13: 14, 16: 17, 17: 18}[r]
                local = roll(W, r, target, unit(sk.off[child]), parent)
                joints[3 * b:3 * b + 3] = euler(local)
                # The child keeps its world rotation: rolling a bent arm's upper
                # half swung the forearm round it (40 degrees off the PS2's,
                # Kunimitsu's knife arm across her mask at 416).
                c = sk.bone[child]
                joints[3 * c:3 * c + 3] = euler((parent @ local).T @ W[child])
        q[3:] = [int(round(v)) & 0xffff for v in joints]


def roll(W, r, target, a, parent):
    """Row r's local rotation turned about its own axis a (row frame) as near
    the world target as can be, its direction kept."""
    M = W[r].T @ target
    w = np.array([M[2, 1] - M[1, 2], M[0, 2] - M[2, 0], M[1, 0] - M[0, 1]])
    th = math.atan2(float(a @ w), float(np.trace(M) - a @ M @ a))
    K_ = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return parent.T @ W[r] @ (np.eye(3) + math.sin(th) * K_ + (1 - math.cos(th)) * K_ @ K_)


def head_fit(sk, frames, actor, poses, C, prior):
    """--head-fit: the head's PS2 -> TTT1 bone basis that keeps the neck,
    over the whole scene, nearest the stance's (orthogonal Procrustes on
    every frame). One frame's measure (--own-head) needs the actor in its
    stance there: Kunimitsu at 974 is not, her basis came out 122 degrees
    off and the neck broken. Biased when the actor looks aside most of the
    scene: Kunimitsu's head came out turned 61 degrees to her left (compared
    frame by frame with the PS2). Once the chest's roll is solved
    (--twists), the identity (canonical, no --own-head) matched the PS2's
    heads: prefer it."""
    W0 = sk.rotations(prior); N0 = W0[CHEST_ROW].T @ W0[HEAD_ROW]
    S = np.zeros((3, 3))
    for d, q in zip(frames, poses):
        W = sk.rotations(np.array([s16(v) for v in q[3:]], dtype=float))
        S += (C @ ps2_rotation(d, actor, HEAD_NODE)).T @ (W[CHEST_ROW] @ N0)
    U, _, Vt = np.linalg.svd(S)
    return U @ np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))]) @ Vt


def twist_bases(sk, d, actor, pose, C, side):
    """Each arm row's PS2 -> TTT1 bone basis on the calibration frame, with
    the solve's pose there (its segments already the PS2's; the TTT1 stance
    is no basis: Kunimitsu's upper arm at 974 is 64 degrees off it)."""
    W = sk.rotations(np.array([s16(v) for v in pose[3:]], dtype=float)); out = {}
    rows = TWIST_ROWS if side == 0 else TWIST_ROWS[::-1]
    for arm, k in zip(rows, range(2)):
        for part in ('upper', 'fore', 'hand'):
            r, n = arm[part][0], TWIST_ROWS[k][part][1]
            out[(r, n)] = (C @ ps2_rotation(d, actor, n)).T @ W[r]
    return out


def camera(d):
    """Eye and forward of a frame: the view-projection is the root node's
    +0x40 matrix times the inverse of its world matrix (row vectors); the eye
    is the point it sends to infinity (x = y = w = 0)."""
    W = np.array(d['actors'][0][0]).reshape(4, 4); M = np.array(d['vp']).reshape(4, 4)
    inv = np.eye(4); inv[:3, :3] = W[:3, :3].T; inv[3, :3] = -W[3, :3] @ W[:3, :3].T
    VP = inv @ M
    A = VP[:3][:, [0, 1, 3]].T; b = -VP[3, [0, 1, 3]]
    return np.linalg.solve(A, b), unit(VP[:3, 3])


def main():
    capture, actor, guest, out = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
    opt = lambda k: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else None
    lines = capture.read_text().splitlines()
    frames = [d for d in map(json.loads, lines[1:]) if 'relink' not in d and not d.get('done') and d.get('actors')]
    name = guest.capitalize()
    sk = Skeleton(Path(opt('--model')) if opt('--model') else WORK / guest / 'guest' / f'{name}-TTT1-arcade-P1.3dm')
    idle_path = Path(opt('--idle')) if opt('--idle') else WORK / guest / 'guest' / f'{name}-TTT1-idle.poses'
    idle = [s16(v) for v in np.frombuffer(idle_path.read_bytes()[:114], '<u2')]
    prior = np.array(idle[3:], dtype=float)
    pos = lambda d, n: np.array(d['actors'][actor][n][12:15], dtype=float)
    if '--twists' in sys.argv: SOLVED.append(('shoulderA', 'shoulderB'))
    if '--collars' in sys.argv:
        for b in (3, 7): PULL[3 * b:3 * b + 3] = COLLAR_PULL

    # Calibration on the stance: the basis C (Kabsch on the segment
    # directions) and the scale, for the better left/right assignment.
    last, stance = frames[-1], sk.directions(prior)
    if opt('--calib-frame'):
        s, f = map(int, opt('--calib-frame').split(':'))
        last = next(d for d in frames if d.get('s', 0) == s and d['f'] == f)
    best = None
    calib = json.loads(Path(opt('--calib')).read_text())['calibration'] if opt('--calib') else None
    basis = opt('--basis')
    if basis and not basis.startswith('canonical'): raise SystemExit(f'--basis {basis}: canonical[:YAW] only')
    for side in PS2 if not calib and not basis else []:
        A = np.array([unit(pos(last, side[b]) - pos(last, side[a])) for a, b in SEGMENTS])
        B = np.array(stance)
        U, S, Vt = np.linalg.svd(A.T @ B)
        C = Vt.T @ np.diag([1, 1, np.sign(np.linalg.det(Vt.T @ U.T))]) @ U.T
        err = float(np.degrees(np.median(np.arccos(np.clip(np.sum((C @ A.T).T * B, 1), -1, 1)))))
        if best is None or err < best[0]: best = (err, side, C)
    if calib:
        # The other actor's frame of reference; left and right from which
        # assignment makes the arms and legs longest-lived in one piece:
        # the one whose first frame is nearer the stance's twist is kept.
        C, scale, origin, err = np.array(calib['C']), calib['scale'], np.array(calib['origin']), calib['error_deg']
        if '--own-origin' in sys.argv: origin = pos(frames[0], 0) * np.array([1.0, 0.0, 1.0])
        side = PS2[calib['side']] if 'side' in calib else min(PS2, key=lambda s: sum(np.linalg.norm(a - b) for a, b in zip(
            [C @ unit(pos(frames[0], s[q]) - pos(frames[0], s[p])) for p, q in SEGMENTS], sk.directions(prior))))
        print(f'basis, scale and origin of {opt("--calib")}', flush=True)
    elif basis:
        yaw = math.radians(float(basis.split(':')[1]) if ':' in basis else 0.0)
        C = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]]) @ np.diag([-1.0, -1.0, 1.0])
        err, side, scale = 0.0, PS2[0], 1.0
        origin = pos(frames[0], 0) * np.array([1.0, 0.0, 1.0])
        print(f'canonical basis, yaw {math.degrees(yaw):.0f} degrees', flush=True)
    else:
        err, side, C = best
        scale = float(np.median([t / np.linalg.norm(pos(last, side[b]) - pos(last, side[a]))
                                 for (a, b), t in zip(SEGMENTS, sk.lengths(prior))]))
        print(f'basis on the stance: median {err:.1f} degrees, scale {scale:.3f} (TTT1 units per PS2 unit)', flush=True)
        # On the ground under the actor's first root: the root channels then
        # carry the hips' height above the floor (TTT1's stance: ~1005).
        origin = pos(frames[0], 0) * np.array([1.0, 0.0, 1.0])
    # The head's constant PS2 -> TTT1 bone basis, on the stance like C.
    if calib and calib.get('head_B') and '--own-head' not in sys.argv: HB = np.array(calib['head_B'])
    elif basis and '--own-head' not in sys.argv: HB = np.eye(3)
    else: HB = (C @ ps2_rotation(last, actor, HEAD_NODE)).T @ sk.rotations(prior)[HEAD_ROW]
    if '--heads-only' in sys.argv:
        # Patch an existing output's heads (and its calibration) only.
        r = json.loads(out.read_text())
        heads(sk, frames, actor, r['poses'], C, HB)
        r['calibration']['head_B'] = HB.tolist()
        out.write_text(json.dumps(r)); print(out, 'heads patched'); return
    to_t3 = lambda p: scale * (C @ (p - origin))           # TTT1 world: y down, as Tekken 3's stage
    poses, cams, fit, joints = [], [], [], prior.copy()
    for i, d in enumerate(frames):
        target = [C @ unit(pos(d, side[b]) - pos(d, side[a])) for a, b in SOLVED]
        # A new shot (the root jumps) or the first frame: no previous pose to go on from.
        cut = i == 0 or np.linalg.norm(pos(d, 0) - pos(frames[i - 1], 0)) > 500 or d.get('s', 0) != frames[i - 1].get('s', 0)
        near = contacts({k: scale * (C @ pos(d, side[k])) for k in TTT1_ROW}) if '--contacts' in sys.argv else ()
        joints = fresh(sk, target, prior, None if i == 0 else joints, near) if cut else solve(sk, target, joints, prior, near=near)
        fit.append(max(math.degrees(math.acos(max(-1.0, min(1.0, float(m @ t))))) for m, t in zip(sk.directions(joints), target)))
        r = to_t3(pos(d, 0))
        heading = math.atan2(r[0], r[2]) / K                # decode_pose: x = sin(a) d, z = cos(a) d
        distance = math.hypot(r[0], r[2])
        height = -r[1]                                       # Tekken 3's y is -height
        poses.append([int(round(v)) & 0xffff for v in (heading, height, distance, *joints)])
        eye, fwd = camera(d)
        e = to_t3(eye); t = to_t3(eye + 2000 * fwd)
        cams.append([round(v) for v in (*e, *t)])
        if i % 100 == 0: print(f'frame {i}/{len(frames)}', flush=True)
    if '--twists' in sys.argv:
        twists(sk, frames, actor, poses, C, twist_bases(sk, last, actor, poses[frames.index(last)], C, PS2.index(side)), PS2.index(side))
        # The fit as played (the rolls must keep every segment).
        played = lambda d, q: [math.degrees(math.acos(max(-1.0, min(1.0, float(m @ (C @ unit(pos(d, side[b]) - pos(d, side[a]))))))))
                               for m, (a, b) in zip(sk.directions(np.array([s16(v) for v in q[3:]], dtype=float)), SEGMENTS)]
        fit = [max(played(d, q)) for d, q in zip(frames, poses)]
    if '--head-fit' in sys.argv: HB = head_fit(sk, frames, actor, poses, C, prior)
    heads(sk, frames, actor, poses, C, HB)
    out.write_text(json.dumps(dict(guest=guest, actor=actor, calibration=dict(error_deg=err, scale=scale, C=C.tolist(), origin=[float(v) for v in origin],
                                                                       head_B=HB.tolist(), side=PS2.index(side)),
                                   sequences=[d.get('s', 0) for d in frames], frames=[d.get('t') for d in frames],
                                   fit_deg=[round(v, 2) for v in fit], poses=poses, camera=cams)))
    print(out, len(poses), f'frames, worst segment {max(fit):.1f} degrees (median of the frames {float(np.median(fit)):.1f})')


if __name__ == '__main__':
    main()

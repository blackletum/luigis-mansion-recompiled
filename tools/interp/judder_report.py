"""Ranked report of the interpolation judder detector (LM_INTERP_PROBE, judder_sweep.py).

    judder_report.py <probe.txt> [--log log.txt] [--top 40] [--px 1.0] [--min-frames 2]

probe.txt lines (written by interp.cpp in LM_INTERP_TEST):
  J <frame> <alpha> <cause> <err px> <step px> <draw#> <identity> <display list> <sub> <world x y z> <room>
    <interp partner off by (world units, -1 none)> <noted camera - main camera>
  R <frame> <alpha> judged <n> flagged <n> max <px>        (one per in-between frame)
  M ...                                                     (draw lists that did not line up)
Errors are pixels of a 2160-line picture (4K), measured against the linear blend of the draw's
world position between its two game frames under the linearly blended camera.

Groups flagged draws by thing (display list + rounded world position) and ranks them by how
often and how badly they are off; prints per cause class counts, per room counts and the
in-between frames' error distribution.
"""
import argparse
import collections
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probe")
    ap.add_argument("--log", default="")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--px", type=float, default=1.0)
    ap.add_argument("--min-frames", type=int, default=2)
    a = ap.parse_args()

    stops = []  # (frame, room) from the room tour
    if a.log:
        for line in open(a.log, encoding="utf-8", errors="replace"):
            m = re.search(r"\[tour\] f(\d+) stop \d+ room (\d+) at", line)
            if m:
                stops.append((int(m.group(1)), int(m.group(2))))
    stops.sort()

    def stop_room(frame):
        r = -1
        for f, room in stops:
            if f <= frame:
                r = room
        return r

    groups = collections.defaultdict(lambda: {"frames": set(), "n": 0, "max": 0.0, "sum": 0.0, "causes": collections.Counter(),
                                              "step": 0.0, "room": -1, "where": None, "pairoff": 0.0})
    causes = collections.Counter()
    causes_px = collections.defaultdict(float)
    per_room = collections.defaultdict(lambda: [0, 0, 0.0])  # in-betweens, flagged draws, max
    frames_flagged = 0
    replays = 0
    judged = 0
    flagged = 0
    mismatch = 0
    errs = []
    seen_r, seen_j = set(), set()
    lights = collections.defaultdict(lambda: [0, 0.0, 0.0, collections.Counter(), ""])
    for line in open(a.probe, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p:
            continue
        # A test replay is repeated until its dump arrives: each in-between frame counts once.
        if p[0] == "R":
            if (p[1], p[2]) in seen_r:
                continue
            seen_r.add((p[1], p[2]))
        elif p[0] in ("J", "L"):
            if line in seen_j:
                continue
            seen_j.add(line)
        if p[0] == "L" and len(p) >= 13:
            # L frame alpha cause poserr direrr source slot wx wy wz move M view V
            k = (p[6], p[7])
            l = lights[k]
            l[0] += 1
            l[1] = max(l[1], float(p[4]))
            l[2] = max(l[2], float(p[5]))
            l[3][p[3]] += 1
            l[4] = " ".join(p[8:11])
            continue
        if p[0] == "R":
            replays += 1
            frame = int(p[1])
            judged += int(p[4])
            flagged += int(p[6])
            frames_flagged += int(p[6]) > 0
            room = stop_room(frame)
            pr = per_room[room]
            pr[0] += 1
            pr[1] += int(p[6])
            pr[2] = max(pr[2], float(p[8]))
        elif p[0] == "M":
            mismatch += 1
        elif p[0] == "J" and len(p) >= 16:
            frame, alpha, cause, err, step = int(p[1]), float(p[2]), p[3], float(p[4]), float(p[5])
            if err < a.px:
                continue
            dl, sub = p[8], p[9]
            wx, wy, wz = float(p[10]), float(p[11]), float(p[12])
            room = int(p[13])
            key = (dl if dl != "00000000" else p[7], sub, round(wx / 25), round(wy / 25), round(wz / 25))
            g = groups[key]
            g["frames"].add(frame)
            g["n"] += 1
            g["max"] = max(g["max"], err)
            g["sum"] += err
            g["causes"][cause] += 1
            g["step"] = max(g["step"], step)
            g["room"] = room if room >= 0 else stop_room(frame)
            g["where"] = (wx, wy, wz)
            g["pairoff"] = max(g["pairoff"], float(p[14]))
            causes[cause] += 1
            causes_px[cause] = max(causes_px[cause], err)
            errs.append(err)
    print(f"in-between frames checked {replays}, draws judged {judged}, flagged (> {a.px:g} px at 4K) {flagged} "
          f"in {frames_flagged} frames; draw lists not lining up {mismatch}")
    errs.sort()
    if errs:
        q = lambda f: errs[min(len(errs) - 1, int(f * len(errs)))]
        print(f"flagged errors px: p50 {q(0.5):.2f}  p90 {q(0.9):.2f}  p99 {q(0.99):.2f}  max {errs[-1]:.2f}")
    print("\nby cause class (flagged draw-frames, worst px):")
    for c, n in causes.most_common():
        print(f"  {c:13s} {n:7d}  worst {causes_px[c]:7.2f}")
    things = [(k, g) for k, g in groups.items() if len(g["frames"]) >= a.min_frames]
    things.sort(key=lambda kg: -(kg[1]["max"] * len(kg[1]["frames"])))
    print(f"\nthings flagged in >= {a.min_frames} game frames: {len(things)}  (of {len(groups)})")
    print(f"\n{'rank':>4} {'room':>4} {'display list':>12} {'sub':>3} {'frames':>6} {'max px':>7} {'mean px':>7} "
          f"{'step px':>7} {'pair off':>8}  cause          world")
    for i, (k, g) in enumerate(things[: a.top]):
        cause = g["causes"].most_common(1)[0][0]
        w = g["where"]
        print(f"{i + 1:4d} {g['room']:4d} {k[0]:>12} {k[1]:>3} {len(g['frames']):6d} {g['max']:7.2f} {g['sum'] / g['n']:7.2f} "
              f"{g['step']:7.1f} {g['pairoff']:8.1f}  {cause:13s}  {w[0]:.0f} {w[1]:.0f} {w[2]:.0f}")
    if stops:
        pass
    if lights:
        print("\nGX lights off their ideal (light object, slot: in-between frames, worst position units, worst direction deg, cause):")
        for k, l in sorted(lights.items(), key=lambda kv: -kv[1][0])[:20]:
            print(f"  {k[0]} slot {k[1]}: {l[0]:5d} frames, {l[1]:7.1f} units, {l[2]:6.2f} deg, {l[3].most_common(1)[0][0]}, at {l[4]}")
    if stops:
        print("\nper room stop (tour room: in-between frames, flagged draw-frames, worst px):")
        for room in sorted(per_room):
            r = per_room[room]
            print(f"  room {room:3d}: {r[0]:5d} frames, {r[1]:6d} flagged, worst {r[2]:.2f}")


if __name__ == "__main__":
    main()

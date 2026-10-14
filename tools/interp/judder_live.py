"""Summary of a live judder log (LM_JUDDER_LOG=<file>, docs/INTERPOLATION.md "Pass 6").

    judder_live.py <judder log> [--from MS] [--until MS] [--top 30] [--seconds]

The game writes per second an `S` line (presents, game-frame intervals, draws followed, jumps and
bends, per cause class) and the worst things of that second as `J` lines (first-last host ms, map,
room, cause, number of jumps, max/mean px at the render resolution, the thing's own motion per game
frame, display list, world position), and `C` lines with the guest call chain of each flagged
display list. This ranks the things over the whole session and prints the per-second timeline
(--seconds), so flags can be matched with a screen recording by host time.
"""
import argparse
import collections
import re


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--until", type=float, default=1e18)
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--seconds", action="store_true")
    a = ap.parse_args()

    chains = {}
    things = collections.defaultdict(lambda: {"secs": 0, "jumps": 0, "max": 0.0, "sum": 0.0, "causes": collections.Counter(),
                                              "step": 0.0, "where": "", "room": "", "first": None, "last": 0.0})
    secs = []
    cause_tot = collections.Counter()
    for line in open(a.log, encoding="utf-8", errors="replace"):
        p = line.split()
        if not p:
            continue
        if p[0] == "C" and len(p) >= 3:
            chains[p[1]] = p[2]
        elif p[0] == "S":
            t = float(p[1])
            if not (a.t0 <= t <= a.until):
                continue
            kv = dict(zip(p[2::2], p[3::2]))
            secs.append((t, line.strip()))
            for c in ("unpaired", "snapped", "not-noted", "pairing-flip", "other-camera", "blend", "inherited"):
                if c in kv:
                    cause_tot[c] += int(kv[c])
        elif p[0] == "J":
            m = re.match(r"J ([\d.]+)-([\d.]+) map (\d+) room (\d+) (\S+) jumps (\d+) max ([\d.]+) mean ([\d.]+) px step ([\d.]+) px "
                         r"dl (\S+) id (\S+) sub (\d+) world (\S+) (\S+) (\S+) inroom (\S+) tape (\d+)", line)
            if not m:
                continue
            t0 = float(m.group(1))
            if not (a.t0 <= t0 <= a.until):
                continue
            dl, sub = m.group(10), m.group(12)
            key = (dl if dl != "00000000" else m.group(11), sub, round(float(m.group(13)) / 50), round(float(m.group(15)) / 50))
            g = things[key]
            g["secs"] += 1
            n = int(m.group(6))
            g["jumps"] += n
            g["max"] = max(g["max"], float(m.group(7)))
            g["sum"] += float(m.group(8)) * n
            g["causes"][m.group(5)] += n
            g["step"] = max(g["step"], float(m.group(9)))
            g["where"] = f"{m.group(13)} {m.group(14)} {m.group(15)}"
            g["room"] = f"{m.group(3)}/{m.group(16)}"
            g["first"] = g["first"] if g["first"] is not None else t0
            g["last"] = float(m.group(2))
    print(f"seconds {len(secs)}; flagged jumps by cause: " + ", ".join(f"{c} {n}" for c, n in cause_tot.most_common()))
    ranked = sorted(things.items(), key=lambda kv: -(kv[1]["max"] * kv[1]["jumps"]))
    print(f"\n{'rank':>4} {'map/room':>8} {'display list':>16} {'sub':>3} {'secs':>4} {'jumps':>6} {'max px':>7} {'mean px':>7} "
          f"{'step px':>7}  cause         world             first..last ms   chain")
    for i, (k, g) in enumerate(ranked[: a.top]):
        cause = g["causes"].most_common(1)[0][0]
        print(f"{i + 1:4d} {g['room']:>8} {k[0]:>16} {k[1]:>3} {g['secs']:4d} {g['jumps']:6d} {g['max']:7.2f} "
              f"{g['sum'] / max(1, g['jumps']):7.2f} {g['step']:7.1f}  {cause:12s}  {g['where']:16s} "
              f"{g['first']:.0f}..{g['last']:.0f}  {chains.get(k[0], '')[:80]}")
    if a.seconds:
        print()
        for t, s in secs:
            print(s)


if __name__ == "__main__":
    main()

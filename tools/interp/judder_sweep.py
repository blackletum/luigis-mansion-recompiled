"""Mansion-wide interpolation judder sweep (docs/INTERPOLATION.md, "Pass 6: judder detector").

    judder_sweep.py <out dir> [--map 2] [--rooms 0-71] [--per 100] [--fps 120] [--set section.key=value ...]
                    [--exe PATH] [--disc ISO] [--lock FILE] [--env K=V ...] [--secs S]

Runs the game in the deterministic interpolation test mode (LM_INTERP_TEST, one in-between per
presented frame of the --fps target: 4 steps at 120) with the judder detector on
(LM_INTERP_PROBE): Aurora reports every draw with the matrix and projection it is drawn with, and
every in-between frame is compared draw by draw with where the draw should be (its world position
blended between its two game frames, seen through the blended camera). LM_ROOM_TOUR puts Luigi in
each room in turn; in each room he strafes left and right and walks diagonally so the camera
follows him. Muted, scratch APPDATA / LM_CARD_DIR under <out dir>, no dump files written.

Then prints judder_report.py's ranked report (also written to <out dir>/report.txt). The game
runs only while the machine-wide game lock (--lock, atomic create) is held.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import screen_run  # noqa: E402

MENU = screen_run.MENU


def room_list(spec):
    out = []
    for part in spec.split(","):
        if "-" in part and "@" not in part:
            a, b = part.split("-")
            out += [str(r) for r in range(int(a), int(b) + 1)]
        elif part:
            out.append(part)
    return out


def stop_input(f, per):
    # After the teleport (and the fall to the floor): strafe left and right, then diagonals, so the
    # camera pans and moves in and out.
    return [f"{f + 15}:STICKLEFT*20", f"{f + 37}:STICKRIGHT*20", f"{f + 59}:STICKUP+STICKLEFT*16",
            f"{f + 77}:STICKDOWN+STICKRIGHT*16"][: 2 if per < 80 else 4]


def acquire(path, tag):
    last = 0
    while True:
        try:
            fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, tag.encode())
            os.close(fd)
            return
        except FileExistsError:
            try:
                holder = open(path, encoding="utf-8", errors="replace").read().strip()
            except OSError:
                holder = "?"
            if time.time() - last > 60:
                print(f"waiting for the game lock (held by {holder})", flush=True)
                last = time.time()
            time.sleep(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--map", type=int, default=2)
    ap.add_argument("--rooms", default="0-71")
    ap.add_argument("--per", type=int, default=100)
    ap.add_argument("--fps", type=int, default=120)
    ap.add_argument("--first", type=int, default=2600)
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "windows-clang", "lmrecomp.exe"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "orig", "GLME01.iso"))
    ap.add_argument("--lock", default="")
    ap.add_argument("--lock-tag", default="judder-sweep")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--secs", type=float, default=0)
    ap.add_argument("--px", type=float, default=1.0)
    ap.add_argument("--dumps", default="", help="first,last: write these frames' test dumps (fN_fresh, fN_s1..)")
    ap.add_argument("--python", default=sys.executable)
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    appdata = os.path.join(out, "appdata")
    card = os.path.join(out, "card")
    os.makedirs(card, exist_ok=True)
    sets = [f"video.target_fps={a.fps}", "video.match_display_refresh=false"] + a.set
    screen_run.write_settings(None, os.path.join(appdata, "Luigi's Mansion PC", "settings.ini"), sets)
    rooms = room_list(a.rooms)
    steps = max(2, round(a.fps / 30))
    last = a.first + len(rooms) * a.per
    inp = [MENU]
    for i in range(len(rooms)):
        inp += stop_input(a.first + i * a.per, a.per)
    env = dict(os.environ)
    env.update({
        "APPDATA": appdata, "LM_CARD_DIR": card, "LM_AUDIO": "0", "LM_BACKGROUND": "1",
        "LM_INPUT_CLOCK": "frames", "LM_START_MAP": str(a.map), "LM_START_APPEAR": "6",
        "LM_TEST_STATE": "poltergust", "LM_INTERP_TEST": str(steps),
        "LM_FRAME_DUMP": os.path.join(out, "dump") + f",{a.first},{last}", "LM_FRAME_DUMP_NOWRITE": "0" if a.dumps else "1",
        "LM_INTERP_PROBE": os.path.join(out, "probe.txt"), "LM_INTERP_PROBE_PX": str(a.px),
        "LM_ROOM_TOUR": f"{a.first},{a.per}," + ":".join(rooms), "LM_DUMP_EXIT": "1",
        # EFB copies to RAM off: in the test mode the guest waits for the host after each frame, and a
        # CPU read of copy memory mid-frame waits for a readback only a replay of that frame makes:
        # minutes of 20 s timeouts in some runs. Geometry and pairing do not depend on it.
        "LM_EFB_RAM": "0",
    })
    if a.map != 2:
        env.pop("LM_START_APPEAR")
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    os.makedirs(os.path.join(out, "dump"), exist_ok=True)
    if a.lock:
        acquire(a.lock, a.lock_tag)
    try:
        log = open(os.path.join(out, "log.txt"), "w", encoding="utf-8")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        p = subprocess.Popen([a.exe, "--disc", a.disc, "--input", ",".join(inp)], env=env, stdout=log,
                             stderr=subprocess.STDOUT, creationflags=flags, cwd=ROOT)
        print(f"started pid {p.pid}: map {a.map}, {len(rooms)} stops, frames {a.first}..{last}", flush=True)
        t0 = time.time()
        limit = a.secs or (600 + len(rooms) * a.per * 0.6)
        while p.poll() is None and time.time() - t0 < limit:
            time.sleep(5)
        if p.poll() is None:
            p.kill()  # only the process this script started
            p.wait()
        log.close()
        print(f"game ran {time.time() - t0:.0f} s", flush=True)
    finally:
        if a.lock:
            try:
                os.remove(a.lock)
            except OSError:
                pass
    rep = subprocess.run([a.python, os.path.join(HERE, "judder_report.py"), os.path.join(out, "probe.txt"),
                          "--log", os.path.join(out, "log.txt")], capture_output=True, text=True)
    open(os.path.join(out, "report.txt"), "w", encoding="utf-8").write(rep.stdout + rep.stderr)
    print(rep.stdout + rep.stderr)


if __name__ == "__main__":
    main()

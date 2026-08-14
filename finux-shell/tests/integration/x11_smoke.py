#!/usr/bin/env python3
"""End-to-end check of the X11 backend against a real window manager.

Everything else in the suite renders offscreen. That covers the widget and
text stacks, but it cannot see the two things that make the bar a shell
component rather than a picture of one:

  * whether the window manager honours _NET_WM_STRUT_PARTIAL, and
  * whether the bar ends up where we asked it to.

Both have already failed in practice. openbox silently relocated the bar to
the top of the screen while still reserving space at the bottom, because the
window carried no WM_NORMAL_HINTS -- a mismatch no offscreen test can detect.

Exits 77 (CTest's "skipped") when Xvfb, openbox or xwd are unavailable.
"""

import os
import shutil
import struct
import subprocess
import sys
import time

SKIP = 77
BAR_HEIGHT = 40
START_BUTTON_WIDTH = 48
SCREEN_W, SCREEN_H = 1280, 800

# The shell defaults to the light theme, as Windows 10 ships. The bar is not
# this exact colour on screen: acrylic tints it over a blurred backdrop, so it
# lands darker. TASKBAR_TINT is the flat colour transparency-off would give.
TASKBAR_TINT = (243, 243, 243)
ACCENT = (0, 120, 215)
# A stretch of bar with no buttons or tray icons in it, used as the reference
# for "what does the bar surface look like here".
EMPTY_BAR = (900, 1000)


def skip(reason):
    print(f"SKIP x11_smoke: {reason}")
    sys.exit(SKIP)


class Xwd:
    """Minimal XWD reader; avoids an ImageMagick dependency just to peek."""

    def __init__(self, blob):
        head = struct.unpack(">25I", blob[:100])
        self.header_size = head[0]
        self.width, self.height = head[4], head[5]
        self.bpp, self.bytes_per_line = head[11], head[12]
        ncolors = head[19]
        self.offset = self.header_size + ncolors * 12
        self.blob = blob

    def pixel(self, x, y):
        o = self.offset + y * self.bytes_per_line + x * (self.bpp // 8)
        return (self.blob[o + 2], self.blob[o + 1], self.blob[o])

    def runs_of(self, color, y):
        """Contiguous x-ranges on row `y` matching `color`."""
        runs, start = [], None
        for x in range(self.width):
            if self.pixel(x, y) == color:
                if start is None:
                    start = x
            elif start is not None:
                runs.append((start, x - 1))
                start = None
        if start is not None:
            runs.append((start, self.width - 1))
        return runs

    def count_not(self, color, x0, x1, y0, y1):
        return sum(
            1
            for y in range(y0, y1)
            for x in range(x0, x1)
            if self.pixel(x, y) != color
        )

    def mean_luma(self, x0, x1, y0, y1):
        total = n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                r, g, b = self.pixel(x, y)
                total += 0.2126 * r + 0.7152 * g + 0.0722 * b
                n += 1
        return total / max(1, n)

    def count_differing(self, reference, x0, x1, y0, y1, threshold=30):
        """Pixels differing from `reference` in any channel by > threshold."""
        return sum(
            1
            for y in range(y0, y1)
            for x in range(x0, x1)
            if max(abs(a - b) for a, b in zip(self.pixel(x, y), reference))
            > threshold
        )


class Session:
    def __init__(self):
        self.procs = []
        self.display = None

    def spawn(self, argv, **kwargs):
        env = dict(os.environ)
        if self.display:
            env["DISPLAY"] = self.display
        proc = subprocess.Popen(
            argv, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            **kwargs
        )
        self.procs.append(proc)
        return proc

    def run(self, argv, timeout=10):
        env = dict(os.environ)
        if self.display:
            env["DISPLAY"] = self.display
        return subprocess.run(argv, env=env, capture_output=True,
                              timeout=timeout)

    def wait_for(self, predicate, what, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if predicate():
                return True
            time.sleep(0.1)
        raise TimeoutError(f"timed out waiting for {what}")

    def close(self):
        for proc in reversed(self.procs):
            proc.terminate()
        for proc in reversed(self.procs):
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


def main():
    if len(sys.argv) < 3:
        print("usage: x11_smoke.py <finux-shell> <finux-testwindow>",
              file=sys.stderr)
        return 2
    shell_bin, testwindow_bin = sys.argv[1], sys.argv[2]

    for tool in ("Xvfb", "openbox", "xwd", "xprop", "xwininfo"):
        if not shutil.which(tool):
            skip(f"{tool} is not installed")
    for path in (shell_bin, testwindow_bin):
        if not os.path.exists(path):
            skip(f"{path} was not built")

    session = Session()
    failures = []

    def check(ok, message):
        if not ok:
            failures.append(message)
            print(f"FAIL {message}")
        else:
            print(f"  ok: {message}")

    try:
        # --- display ---------------------------------------------------------
        for number in range(99, 120):
            display = f":{number}"
            proc = subprocess.Popen(
                ["Xvfb", display, "-screen", "0",
                 f"{SCREEN_W}x{SCREEN_H}x24"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            session.procs.append(proc)
            session.display = display
            time.sleep(0.3)
            if proc.poll() is None and session.run(["xdpyinfo"]).returncode == 0:
                break
            session.display = None
        if not session.display:
            skip("could not start Xvfb on any display")

        session.wait_for(
            lambda: session.run(["xdpyinfo"]).returncode == 0, "the X server")

        # --- window manager --------------------------------------------------
        session.spawn(["openbox"])
        session.wait_for(
            lambda: b"_NET_SUPPORTED" in session.run(
                ["xprop", "-root", "_NET_SUPPORTED"]).stdout,
            "openbox to claim the screen")

        # --- client windows --------------------------------------------------
        session.spawn([testwindow_bin, "Untitled - Notepad", "Notepad"])
        session.spawn([testwindow_bin, "Documents", "Explorer"])
        session.wait_for(
            lambda: session.run(
                ["xprop", "-root", "_NET_CLIENT_LIST"]
            ).stdout.count(b"0x") >= 2,
            "two client windows")

        # --- the shell -------------------------------------------------------
        session.spawn([shell_bin])
        session.wait_for(
            lambda: session.run(
                ["xwininfo", "-name", "finux-shell"]).returncode == 0,
            "the taskbar window")
        # Let the shell observe the client list and paint.
        time.sleep(2.0)

        # --- geometry --------------------------------------------------------
        info = session.run(["xwininfo", "-name", "finux-shell"]).stdout.decode()
        geometry = {}
        for line in info.splitlines():
            if "Absolute upper-left Y:" in line:
                geometry["y"] = int(line.split(":")[1])
            elif line.strip().startswith("Width:"):
                geometry["w"] = int(line.split(":")[1])
            elif line.strip().startswith("Height:"):
                geometry["h"] = int(line.split(":")[1])

        check(geometry.get("h") == BAR_HEIGHT,
              f"bar is {BAR_HEIGHT}px tall (got {geometry.get('h')})")
        check(geometry.get("w") == SCREEN_W,
              f"bar spans the screen (got {geometry.get('w')})")
        # The regression that motivated this test: strut on one edge, bar on
        # the other.
        check(geometry.get("y") == SCREEN_H - BAR_HEIGHT,
              f"bar sits on the bottom edge at y={SCREEN_H - BAR_HEIGHT} "
              f"(got {geometry.get('y')})")

        # --- strut -----------------------------------------------------------
        workarea = session.run(["xprop", "-root", "_NET_WORKAREA"]).stdout.decode()
        numbers = [int(n) for n in workarea.replace("=", ",").split(",")
                   if n.strip().lstrip("-").isdigit()]
        check(bool(numbers) and numbers[3] == SCREEN_H - BAR_HEIGHT,
              f"window manager reserved {BAR_HEIGHT}px "
              f"(_NET_WORKAREA height {numbers[3] if numbers else 'unset'})")

        # --- pixels ----------------------------------------------------------
        capture = session.run(["xwd", "-root", "-silent"]).stdout
        check(len(capture) > 1000, "captured the root window")
        if len(capture) > 1000:
            image = Xwd(capture)
            top = image.height - BAR_HEIGHT

            # Acrylic: the bar must be light (light theme) but NOT the flat
            # tint colour, because it is composited over a blurred backdrop.
            # Asserting the flat colour would pass only if acrylic were broken.
            bar_luma = image.mean_luma(EMPTY_BAR[0], EMPTY_BAR[1], top + 2,
                                       image.height - 2)
            tint_luma = (0.2126 * TASKBAR_TINT[0] + 0.7152 * TASKBAR_TINT[1] +
                         0.0722 * TASKBAR_TINT[2])
            check(bar_luma > 120,
                  f"taskbar surface is not light ({bar_luma:.0f})")
            check(bar_luma < tint_luma - 5,
                  f"taskbar is the flat tint ({bar_luma:.0f} vs "
                  f"{tint_luma:.0f}) -- acrylic did not sample the backdrop")

            surface = image.pixel(EMPTY_BAR[0] + 20, top + 6)
            start_ink = image.count_differing(surface, 0, 48, top,
                                              image.height)
            check(start_ink > 0, f"start button drew ({start_ink} px)")

            # One accent run per running window. Buttons are icon-only by
            # default ("combine taskbar buttons: always"), so the runs are
            # narrow, but there must still be exactly one per client.
            #
            # The Start button is accent-filled in the light theme and spans
            # the bar's full height, so it also matches on this row; skip it
            # rather than counting it as an indicator.
            runs = [(a, b) for a, b in image.runs_of(ACCENT, image.height - 2)
                    if a >= START_BUTTON_WIDTH]
            check(len(runs) == 2,
                  f"one running indicator per client window (got {len(runs)}, "
                  f"want 2)")
            for index, (x0, x1) in enumerate(runs):
                icon_ink = image.count_differing(surface, x0, x1 + 1, top + 8,
                                                 top + 32)
                check(icon_ink > 0,
                      f"task button {index} drew no icon above its indicator")

            # The Start button carries the accent colour in the light theme.
            start_accent = sum(
                1 for y in range(top, image.height)
                for x in range(0, START_BUTTON_WIDTH)
                if image.pixel(x, y) == ACCENT)
            check(start_accent > 200,
                  f"start button is not accent-filled ({start_accent} px)")

            clock_ink = image.count_differing(surface, 1195, image.width - 8,
                                              top, image.height)
            check(clock_ink > 0, f"clock drew ({clock_ink} px)")

            # The bar must not list itself, so the count above already proves
            # the dock window was filtered out of _NET_CLIENT_LIST.
            clients = session.run(
                ["xprop", "-root", "_NET_CLIENT_LIST"]).stdout.count(b"0x")
            check(clients >= 3,
                  f"the dock is in the client list ({clients}) yet was not "
                  f"given a task button")
    except TimeoutError as error:
        print(f"FAIL {error}")
        failures.append(str(error))
    finally:
        session.close()

    if failures:
        print(f"FAILED x11_smoke ({len(failures)} check"
              f"{'' if len(failures) == 1 else 's'})")
        return 1
    print("PASS x11_smoke")
    return 0


if __name__ == "__main__":
    sys.exit(main())

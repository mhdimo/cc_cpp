#!/usr/bin/env python3
"""Measures the port against the reference and draws the graph.

The port is a native binary and the reference is a bundle of JavaScript that
runs on Bun, so the two are measured the same way: the session is started
under a pty, the tree of processes is watched, and three numbers are taken.

- **the memory** the session takes once it has settled — the resident memory
  of the process and of every child it has, which is what the system
  charges the session for;
- **the cost of the start** — the wall time and the CPU time until the
  first paint of the screen;
- **the cost of the idling** — the CPU the session burns per second while
  it waits for the user, the spinner and the mascot included.

The numbers are written to `docs/bench.json`, the graph to `docs/bench.svg`
and the page that holds it to `docs/bench.html`. The README shows the
graph; the page holds the graph and the table of the numbers.

    python3 scripts/bench.py
"""

import argparse
import json
import os
import pty
import select
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, "build", "src", "cc++")
REFERENCE = "/opt/homebrew/bin/claude"

DOCS = os.path.join(ROOT, "docs")

# The colours of the graph: the orange of the mascot and the grey of the
# text, the colours of the port.
COLOURS = {"claude": "#d97757", "cc++": "#8a8a8a"}


def children_of(pid):
    """The pids of the children of a process, and of their children."""
    found = []
    try:
        output = subprocess.run(
            ["pgrep", "-P", str(pid)], capture_output=True, text=True, timeout=5
        ).stdout
    except (subprocess.SubprocessError, OSError):
        return found
    for line in output.split():
        try:
            child = int(line)
        except ValueError:
            continue
        found.append(child)
        found.extend(children_of(child))
    return found


def rss_kb(pid):
    """The resident memory of a process, in kilobytes."""
    try:
        output = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True, timeout=5
        ).stdout
    except (subprocess.SubprocessError, OSError):
        return 0
    return int(output.strip() or 0)


def cpu_seconds(pid):
    """The CPU time a process has used, in seconds."""
    try:
        output = subprocess.run(
            ["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True, timeout=5
        ).stdout.strip()
    except (subprocess.SubprocessError, OSError):
        return 0.0
    if not output:
        return 0.0

    # The time of `ps` is "[[dd-]hh:]mm:ss.ss" — the days and the hours are
    # there only when there are many of them.
    days = 0.0
    if "-" in output:
        head, output = output.split("-", 1)
        days = float(head) * 86400
    parts = output.split(":")
    try:
        if len(parts) == 3:
            hours, minutes, seconds = float(parts[0]), float(parts[1]), float(parts[2])
        elif len(parts) == 2:
            hours, minutes, seconds = 0.0, float(parts[0]), float(parts[1])
        else:
            hours, minutes, seconds = 0.0, 0.0, float(parts[0])
    except ValueError:
        return 0.0
    return days + hours * 3600 + minutes * 60 + seconds


class Session:
    """A CLI that runs under a pty."""

    def __init__(self, binary, env=None, cwd=None):
        self.master, slave = pty.openpty()
        os.set_blocking(self.master, False)
        self.process = subprocess.Popen(
            [binary],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            cwd=cwd or ROOT,
            env=env,
            preexec_fn=os.setsid,
        )
        os.close(slave)
        self.binary = binary
        self.first_paint = None
        self.started = time.time()

    def drain(self, seconds):
        """Reads what the session writes, so that it does not block.

        The moment the first paint arrives is remembered: it is the moment
        the user sees something.
        """
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.master], [], [], 0.05)
            if not ready:
                continue
            try:
                data = os.read(self.master, 65536)
            except OSError:
                break
            if not data:
                break
            if self.first_paint is None:
                self.first_paint = time.time()
        return self.first_paint is not None

    def send(self, text):
        os.write(self.master, text.encode())

    def stop(self):
        try:
            self.process.terminate()
            self.process.wait(timeout=5)
        except (subprocess.SubprocessError, OSError):
            try:
                self.process.kill()
            except OSError:
                pass
        try:
            os.close(self.master)
        except OSError:
            pass


def tree_rss_kb(pid):
    """The resident memory of a process and of every child it has."""
    total = rss_kb(pid)
    for child in children_of(pid):
        total += rss_kb(child)
    return total


class CpuTracker:
    """The CPU time of a tree of processes, counted as it is used.

    The CPU of the children that go away must not be lost: a process that
    exits is not in the tree any more, and a sum of the tree would go down
    when it does. Every process is therefore remembered, and only the
    difference between two samples of it is counted.
    """

    def __init__(self):
        self.seen = {}
        self.total = 0.0

    def sample(self, pid):
        """Counts the CPU that was used since the last sample."""
        for process in [pid] + children_of(pid):
            used = cpu_seconds(process)
            previous = self.seen.get(process)
            if previous is None:
                self.total += used
            elif used > previous:
                self.total += used - previous
            self.seen[process] = used
        return self.total


def measure(binary, label, env, idle_seconds, settle_seconds):
    """Measures one session of one binary."""
    session = Session(binary, env=env)
    tracker = CpuTracker()

    # The cost of the start: the wall time from the launch to the first
    # paint, and the CPU that was used until then. The two are measured
    # over the same window, so that they may be read together.
    deadline = time.time() + 15.0
    while time.time() < deadline and session.first_paint is None:
        session.drain(0.05)
        tracker.sample(session.process.pid)
        if session.process.poll() is not None:
            break

    startup_ms = (session.first_paint - session.started) * 1000 if session.first_paint else 0.0
    startup_cpu_ms = tracker.total * 1000

    # The session has to be past the questions of the start before it is
    # measured: the reference asks whether the folder is trusted and whether
    # the key of the environment may be used, the port asks the first one.
    if binary == PORT:
        session.drain(2.0)
        session.send("\r")
        session.drain(1.5)
    else:
        session.drain(2.0)
        session.send("\x1b[B")
        session.drain(0.4)
        session.send("\r")
        session.drain(1.5)
        session.send("\x1b[A")
        session.drain(0.4)
        session.send("\r")
        session.drain(1.5)

    # The session settles: the memory it takes grows a little and then
    # stops growing.
    samples = []
    deadline = time.time() + settle_seconds
    while time.time() < deadline:
        session.drain(0.4)
        if session.process.poll() is not None:
            break
        samples.append(tree_rss_kb(session.process.pid))

    # The cost of the idling: the CPU the session burns while it waits.
    cpu_before = tracker.sample(session.process.pid)
    wall_before = time.time()
    deadline = time.time() + idle_seconds
    while time.time() < deadline:
        session.drain(0.4)
        if session.process.poll() is not None:
            break
    cpu_after = tracker.sample(session.process.pid)
    wall_after = time.time()

    session.stop()

    idle_cpu_ms_per_s = 0.0
    if wall_after > wall_before:
        idle_cpu_ms_per_s = (cpu_after - cpu_before) * 1000 / (wall_after - wall_before)

    return {
        "label": label,
        "key": "claude" if "reference" in label else "cc++",
        "memory_mb": (max(samples) if samples else 0) / 1024,
        "memory_trace_mb": [round(value / 1024, 1) for value in samples],
        "startup_ms": round(startup_ms, 1),
        "startup_cpu_ms": round(startup_cpu_ms, 1),
        "idle_cpu_ms_per_s": round(idle_cpu_ms_per_s, 1),
    }


def bars_svg(results):
    """The graph: three groups of two bars, drawn as an SVG."""
    width = 720
    height = 300
    left = 70
    right = 200
    plot = width - left - right
    top = 44
    bottom = 58

    groups = [
        ("the memory", "MB", "memory_mb"),
        ("the start", "ms", "startup_ms"),
        ("the idling", "ms per second", "idle_cpu_ms_per_s"),
    ]

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="ui-sans-serif, -apple-system, '
        f'Segoe UI, Helvetica, Arial, sans-serif" role="img" '
        f'aria-label="the memory and the CPU of the port and of the reference">',
        f'<rect width="{width}" height="{height}" fill="#12100e"/>',
    ]

    group_width = plot / len(groups)
    for index, (title, unit, key) in enumerate(groups):
        values = [result[key] for result in results]
        biggest = max(values) if values else 1.0
        biggest = biggest if biggest > 0 else 1.0

        x0 = left + index * group_width + 26
        for slot, result in enumerate(results):
            value = result[key]
            bar_width = (group_width - 64) / max(len(results), 1) - 12
            x = x0 + slot * (bar_width + 12)
            height_of_bar = (value / biggest) * (height - top - bottom)
            y = height - bottom - height_of_bar
            colour = COLOURS.get(result["key"], "#8a8a8a")
            out.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_width:.1f}" '
                f'height="{max(height_of_bar, 1):.1f}" rx="3" fill="{colour}"/>'
            )
            out.append(
                f'<text x="{x + bar_width / 2:.1f}" y="{y - 7:.1f}" fill="#e8e6e3" '
                f'font-size="12" text-anchor="middle">{value:.1f}</text>'
            )

        # The name of the group, under its bars, and the unit it counts in.
        centre = left + index * group_width + group_width / 2
        out.append(
            f'<text x="{centre:.1f}" y="{height - bottom + 22}" fill="#c9c5c0" '
            f'font-size="13" text-anchor="middle">{title}</text>'
        )
        out.append(
            f'<text x="{centre:.1f}" y="{height - bottom + 39}" fill="#8a8a8a" '
            f'font-size="11" text-anchor="middle">{unit}</text>'
        )

    # The line of the axis.
    out.append(
        f'<line x1="{left}" y1="{height - bottom}" x2="{width - right}" '
        f'y2="{height - bottom}" stroke="#3a3733" stroke-width="1"/>'
    )

    # The names of the two programs, at the top of the right-hand margin.
    legend = [("the reference (claude)", COLOURS["claude"]),
              ("the port (cc++)", COLOURS["cc++"])]
    for slot, (name, colour) in enumerate(legend):
        y = 26 + slot * 19
        x = width - right + 14
        out.append(f'<rect x="{x}" y="{y - 9}" width="11" height="11" rx="2" fill="{colour}"/>')
        out.append(f'<text x="{x + 18}" y="{y}" fill="#c9c5c0" font-size="12">{name}</text>')

    out.append("</svg>")
    return "\n".join(out)


def html_page(results, svg):
    """The page that holds the graph and the table of the numbers."""
    rows = []
    for result in results:
        rows.append(
            "<tr>"
            f"<td>{result['label']}</td>"
            f"<td>{result['memory_mb']:.1f}</td>"
            f"<td>{result['startup_ms']:.0f}</td>"
            f"<td>{result['startup_cpu_ms']:.0f}</td>"
            f"<td>{result['idle_cpu_ms_per_s']:.1f}</td>"
            "</tr>"
        )

    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>The port and the reference — memory and CPU</title>
<style>
  :root {{ color-scheme: dark; }}
  body {{
    margin: 0; padding: 40px 24px;
    background: #12100e; color: #e8e6e3;
    font-family: ui-sans-serif, -apple-system, "Segoe UI", Helvetica, Arial, sans-serif;
    line-height: 1.55;
  }}
  main {{ max-width: 760px; margin: 0 auto; }}
  h1 {{ font-size: 22px; font-weight: 600; margin: 0 0 4px; }}
  p.lead {{ color: #a09a94; margin: 0 0 28px; }}
  figure {{ margin: 0 0 32px; }}
  figcaption {{ color: #8a8a8a; font-size: 13px; margin-top: 10px; }}
  table {{ border-collapse: collapse; width: 100%; font-size: 14px; }}
  th, td {{ text-align: right; padding: 8px 10px; border-bottom: 1px solid #2b2825; }}
  th:first-child, td:first-child {{ text-align: left; }}
  th {{ color: #a09a94; font-weight: 500; }}
  code {{ background: #1c1a17; padding: 2px 6px; border-radius: 4px; font-size: 13px; }}
  footer {{ color: #6f6a65; font-size: 12px; margin-top: 36px; }}
</style>
</head>
<body>
<main>
  <h1>The port and the reference</h1>
  <p class="lead">The memory a session takes and the CPU it burns, measured the same way for
  the native port and for the bundle of JavaScript the reference runs on.</p>

  <figure>
    {svg}
    <figcaption>The memory of the session once it has settled, the wall time of the start
    until the first paint, and the CPU the session burns per second while it waits.</figcaption>
  </figure>

  <table>
    <thead>
      <tr>
        <th>session</th>
        <th>memory (MB)</th>
        <th>the start (ms)</th>
        <th>the start (CPU ms)</th>
        <th>the idling (CPU ms/s)</th>
      </tr>
    </thead>
    <tbody>
      {''.join(rows)}
    </tbody>
  </table>

  <footer>Measured with <code>python3 scripts/bench.py</code> on macOS. The memory is the
  resident memory of the process and of every child it has. The CPU of the start is the
  whole process, its threads included: the port starts a pool of threads and reaches the
  screen in less time, though the threads cost more CPU together.</footer>
</main>
</body>
</html>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", default=REFERENCE, help="the CLI the port is compared with")
    parser.add_argument("--skip-reference", action="store_true", help="measure the port alone")
    parser.add_argument("--idle", type=float, default=6.0, help="how long the session idles")
    parser.add_argument("--settle", type=float, default=4.0, help="how long the session settles")
    parser.add_argument("--out", default=DOCS, help="where the graph is written")
    args = parser.parse_args()

    env = dict(os.environ)
    env["COLUMNS"] = "80"
    env["LINES"] = "24"
    env["CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC"] = "1"
    # The extension of the IDE is not installed while the measurement runs:
    # both the port and the reference would start the editor of the machine
    # in the background, and the editor is far heavier than either of them.
    env["CLAUDE_CODE_IDE_SKIP_AUTO_INSTALL"] = "1"
    # The address of the API is one that is not there, so that no session
    # of the measurement waits for a network.
    env["ANTHROPIC_BASE_URL"] = "http://127.0.0.1:9"
    env["ANTHROPIC_API_KEY"] = "test-key"

    # The two sessions run in a home of their own, with nothing in it: the
    # settings, the plugins, the servers and the conversations of the
    # machine would be a part of the measurement, and they are not the CLI
    # that is measured.
    home = tempfile.mkdtemp(prefix="cc-bench-")
    with open(os.path.join(home, ".claude.json"), "w") as handle:
        json.dump({"theme": "dark", "hasCompletedOnboarding": True}, handle)
    env["HOME"] = home

    results = []
    if not args.skip_reference and os.path.exists(args.reference):
        print("measuring the reference…")
        results.append(measure(args.reference, "the reference (claude)", env, args.idle,
                                args.settle))
    if os.path.exists(PORT):
        print("measuring the port…")
        results.append(measure(PORT, "the port (cc++)", env, args.idle, args.settle))

    if not results:
        print("neither binary was found")
        return 1

    for result in results:
        print(
            f"  {result['label']:24} {result['memory_mb']:7.1f} MB   "
            f"the start {result['startup_ms']:7.1f} ms   "
            f"the idling {result['idle_cpu_ms_per_s']:6.1f} ms/s"
        )

    os.makedirs(args.out, exist_ok=True)
    svg = bars_svg(results)

    with open(os.path.join(args.out, "bench.svg"), "w") as handle:
        handle.write(svg)
    with open(os.path.join(args.out, "bench.html"), "w") as handle:
        handle.write(html_page(results, svg))
    with open(os.path.join(args.out, "bench.json"), "w") as handle:
        json.dump(results, handle, indent=2)

    print(f"\nwrote {args.out}/bench.svg, {args.out}/bench.html and {args.out}/bench.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())

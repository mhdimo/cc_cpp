#!/usr/bin/env python3
"""Measures how much memory a session of the CLI takes.

The terminal UI only runs on a real terminal, so the binary is started under
a pty and its process tree is watched. The number that is reported is the
resident memory of the process and of every child it has, which is what the
system charges the session for.
"""

import argparse
import os
import pty
import select
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(ROOT, "build", "src", "cc++")
REFERENCE = "/opt/homebrew/bin/claude"


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


def tree_rss_kb(pid):
    """The resident memory of a process and of every child it has."""
    total = rss_kb(pid)
    for child in children_of(pid):
        total += rss_kb(child)
    return total


def own_rss_kb(pid):
    """The resident memory of a process alone."""
    return rss_kb(pid)


class Session:
    """A CLI that runs under a pty."""

    def __init__(self, binary, args=(), env=None):
        self.master, slave = pty.openpty()
        os.set_blocking(self.master, False)
        self.process = subprocess.Popen(
            [binary, *args],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            cwd=ROOT,
            env=env,
            preexec_fn=os.setsid,
        )
        os.close(slave)
        self.binary = binary

    def drain(self, seconds):
        """Reads what the session writes, so that it does not block."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.master], [], [], 0.1)
            if not ready:
                continue
            try:
                if not os.read(self.master, 65536):
                    break
            except OSError:
                break

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


def measure(binary, args=(), settle=6.0, env=None, label=None):
    """The memory a session takes once it has settled."""
    session = Session(binary, args, env)
    samples = []
    own = []
    deadline = time.time() + settle
    while time.time() < deadline:
        session.drain(0.5)
        if session.process.poll() is not None:
            break
        samples.append(tree_rss_kb(session.process.pid))
        own.append(own_rss_kb(session.process.pid))
    session.stop()
    if not samples:
        return None
    return {
        "label": label or os.path.basename(binary),
        "pid": session.process.pid,
        "peak": max(samples),
        "last": samples[-1],
        "samples": samples,
        "own_peak": max(own),
        "own_last": own[-1],
        "own_samples": own,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--reference",
        default=REFERENCE,
        help="the TypeScript CLI the port is compared with",
    )
    parser.add_argument(
        "--skip-reference",
        action="store_true",
        help="measure the port alone",
    )
    parser.add_argument(
        "--settle", type=float, default=6.0, help="how long the session is watched"
    )
    args = parser.parse_args()

    env = dict(os.environ)
    # The colours are the same whether or not the terminal understands them,
    # but the width is taken from the pty, which is 80 columns here.
    env["COLUMNS"] = "80"
    env["LINES"] = "24"

    results = []
    if not args.skip_reference and os.path.exists(args.reference):
        results.append(
            measure(args.reference, settle=args.settle, env=env, label="claude (reference)")
        )
    if os.path.exists(PORT):
        results.append(measure(PORT, settle=args.settle, env=env, label="claude (the port)"))

    print()
    print(f"{'session':22} {'peak':>10} {'settled':>10} {'own':>10}")
    print("-" * 54)
    for result in results:
        if result is None:
            print(f"{'—':22} {'(the session ended)':>10}")
            continue
        print(
            f"{result['label']:22} "
            f"{result['peak'] / 1024:>7.1f} MB {result['last'] / 1024:>7.1f} MB "
            f"{result['own_last'] / 1024:>7.1f} MB"
        )
    print()
    print("peak and settled are the process and its children; own is the process alone")
    print()

    # The curve of the memory, so that a session that keeps growing is seen
    # as one.
    for result in results:
        if result is None or not result["samples"]:
            continue
        trace = " ".join(f"{value / 1024:.0f}" for value in result["samples"])
        print(f"{result['label']} (with the children): {trace} MB")
        trace = " ".join(f"{value / 1024:.0f}" for value in result["own_samples"])
        print(f"{result['label']} (alone):            {trace} MB")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())

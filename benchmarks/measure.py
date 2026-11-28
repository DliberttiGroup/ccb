#!/usr/bin/env python3
"""Run a command and report wall time, CPU time, and peak child RSS.

Usage: measure.py [--log FILE] -- <command...>

Prints one line on stdout:
  wall_ms=<n> user_ms=<n> sys_ms=<n> maxrss_mib=<n>
and exits with the child's exit code. With --log FILE the child's
stdout and stderr go to FILE instead of inheriting this process's.

CPU and RSS come from resource.getrusage(RUSAGE_CHILDREN), so they
cover the command and every child it waited for. maxrss is the peak
resident set of the single largest child (the kernel reports a
maximum, not a sum): for a CCB build that is usually kotlinc or d8,
not the ccb process itself. Treat it as a lower bound on total
memory pressure, not a whole-build total. Wall time is monotonic,
so it is unaffected by wall-clock adjustments during the run.
"""

import resource
import subprocess
import sys
import time


def _ms(seconds):
    return int(seconds * 1000)


def main(argv):
    log_path = None
    args = argv[1:]
    if len(args) >= 2 and args[0] == "--log":
        log_path = args[1]
        args = args[2:]
    if len(args) < 2 or args[0] != "--":
        print("usage: measure.py [--log FILE] -- <command...>", file=sys.stderr)
        return 2
    cmd = args[1:]
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.monotonic()
    if log_path is not None:
        with open(log_path, "wb") as log:
            rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT).returncode
    else:
        rc = subprocess.run(cmd).returncode
    end = time.monotonic()
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    user = _ms(after.ru_utime - before.ru_utime)
    sys_ms = _ms(after.ru_stime - before.ru_stime)
    # ru_maxrss is KiB on Linux and is a running maximum over all
    # waited-for children, so the delta is meaningless; report the
    # post-run value.
    maxrss_mib = int(after.ru_maxrss // 1024)
    print(
        "wall_ms=%d user_ms=%d sys_ms=%d maxrss_mib=%d"
        % (_ms(end - start), user, sys_ms, maxrss_mib)
    )
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))

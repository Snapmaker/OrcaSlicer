"""Runs the SnapmakerLan probe end to end against two fake U1s (fake_moonraker.py) on 127.0.0.1.

    python run_snapmaker_probe_e2e.py --exe <build>/tests/slic3rutils/Release/slic3rutils_tests.exe

Starts two fakes on free loopback ports, runs the hidden Catch2 tag [SnapmakerProbeE2E] with their
ports in FAKE_MOONRAKER_A / FAKE_MOONRAKER_B ("<printer port>:<control port>"), and stops them
again. The test drives each fake through normal -> slow -> back up -> down -> back up and checks
the online/offline state the hub would show at every step. No real printer is contacted.
Exit code: the test executable's.
"""
import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_fake(name):
    port, ctl = free_port(), free_port()
    proc = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "fake_moonraker.py"),
                             "--port", str(port), "--control", str(ctl), "--name", name, "--sn", name.upper()],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    line = proc.stdout.readline().strip()
    if not line.startswith("READY"):
        proc.kill()
        raise SystemExit("fake %s did not start: %r" % (name, line))
    return proc, "%d:%d" % (port, ctl)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True, help="slic3rutils_tests.exe")
    args, rest = ap.parse_known_args()
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        raise SystemExit("no test executable at " + exe)
    fakes = []
    try:
        a, a_ports = start_fake("fake-a")
        fakes.append(a)
        b, b_ports = start_fake("fake-b")
        fakes.append(b)
        env = dict(os.environ, FAKE_MOONRAKER_A=a_ports, FAKE_MOONRAKER_B=b_ports)
        print("fakes: A=%s B=%s" % (a_ports, b_ports), flush=True)
        started = time.time()
        rc = subprocess.call([exe, "[SnapmakerProbeE2E]", "--durations", "yes"] + rest, env=env)
        print("E2E_EXIT=%d (%.0f s)" % (rc, time.time() - started), flush=True)
        return rc
    finally:
        for p in fakes:  # only the processes this script started, by handle
            p.kill()
            p.wait()


if __name__ == "__main__":
    sys.exit(main())

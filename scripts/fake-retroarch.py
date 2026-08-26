#!/usr/bin/env python3
"""Stand-in for RetroArch in the retroarch-runner-stop smoke test.

Speaks just enough of the pinned v1.22.2 contract the runner depends on: it
binds the network command port, and on QUIT it writes its --config file the way
save-on-exit does and exits. FAKE_RA_MODE selects which failure the runner has
to cope with.
"""
import os
import signal
import socket
import sys
import time

PORT = 55355


def config_path(argv):
    for i, arg in enumerate(argv):
        if arg == "--config" and i + 1 < len(argv):
            return argv[i + 1]
    raise SystemExit("fake-retroarch: no --config")


def save(path):
    """What save-on-exit does: rewrite the whole file, one line per key."""
    with open(path, "r", encoding="utf-8", errors="replace") as fp:
        lines = fp.read().splitlines()
    out = []
    seen = False
    for line in lines:
        if line.startswith("rewind_enable"):
            if not seen:
                out.append('rewind_enable = "true"')
                seen = True
            continue
        out.append(line)
    if not seen:
        out.append('rewind_enable = "true"')
    out.append('fake_retroarch_saved = "yes"')
    tmp = path + ".fake"
    with open(tmp, "w", encoding="utf-8") as fp:
        fp.write("\n".join(out) + "\n")
    os.replace(tmp, path)


def main():
    cfg = config_path(sys.argv)
    mode = os.environ.get("FAKE_RA_MODE", "quit")
    quit_log = os.environ.get("FAKE_RA_QUIT_LOG")
    ready = os.environ.get("FAKE_RA_READY")

    if mode == "deaf":
        # A wedged emulator: no command handling, and SIGTERM ignored so the
        # runner has to escalate all the way to SIGKILL.
        signal.signal(signal.SIGTERM, signal.SIG_IGN)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", PORT))
    sock.settimeout(0.2)

    if ready:
        with open(ready, "w", encoding="utf-8") as fp:
            fp.write(str(os.getpid()))

    if mode == "exit-now":
        save(cfg)
        return 0

    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            data, _ = sock.recvfrom(1024)
        except socket.timeout:
            continue
        text = data.decode("utf-8", "replace").strip()
        if quit_log and text:
            with open(quit_log, "a", encoding="utf-8") as fp:
                fp.write(text + "\n")
        if text == "QUIT" and mode == "quit":
            save(cfg)
            return 0
    return 3


if __name__ == "__main__":
    sys.exit(main())

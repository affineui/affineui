#!/usr/bin/env python3
"""affinetools CLI — attach to a running AffineUI app and read telemetry.

The agent-facing half of the affinetools protocol (S1 spine). Stdlib only.
Wire format: 4-byte LE length + UTF-8 JSON (docs/AFFINETOOLS_PROTOCOL.md);
targets opt in with AFFINEUI_TOOLS_LISTEN=1 and advertise via
<tempdir>/affineui-tools/<pid>.json (port + auth token).

Usage:
  affinetools_cli.py info                        list attachable targets
  affinetools_cli.py ping [--pid PID]            handshake + round-trip time
  affinetools_cli.py watch [--pid PID] [--seconds N]
                                                 print live telemetry events
  affinetools_cli.py dump OUT.jsonl [--pid PID] [--frames N] [--seconds N]
                                                 record a session to JSONL
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import sys
import tempfile
import time


def discovery_dir() -> str:
    return os.path.join(tempfile.gettempdir(), "affineui-tools")


def discover() -> list[dict]:
    """All advertised targets, newest first. Stale files (dead pids that
    never removed their file) simply fail to connect."""
    out = []
    d = discovery_dir()
    if not os.path.isdir(d):
        return out
    entries = []
    for name in os.listdir(d):
        if not name.endswith(".json"):
            continue
        path = os.path.join(d, name)
        try:
            with open(path, "r", encoding="utf-8") as f:
                info = json.load(f)
            entries.append((os.path.getmtime(path), info))
        except (OSError, json.JSONDecodeError):
            continue
    entries.sort(key=lambda e: e[0], reverse=True)
    return [info for _, info in entries]


def pick_target(args) -> dict:
    if args.port:
        return {"port": args.port, "token": args.token or ""}
    targets = discover()
    if args.pid:
        for t in targets:
            if t.get("pid") == args.pid:
                return t
        sys.exit(f"no target with pid {args.pid} in {discovery_dir()}")
    for t in targets:  # newest that accepts a connection wins
        try:
            with socket.create_connection(("127.0.0.1", t["port"]), 0.25):
                return t
        except OSError:
            continue
    sys.exit(f"no attachable AffineUI target found in {discovery_dir()} "
             f"(run the app with AFFINEUI_TOOLS_LISTEN=1)")


class Client:
    def __init__(self, port: int, token: str):
        self.sock = socket.create_connection(("127.0.0.1", port), 3.0)
        self.sock.settimeout(3.0)
        self.next_id = 1
        self.hello = self.request("hello", {"token": token, "client": "affinetools_cli"})

    def close(self):
        self.sock.close()

    def send(self, msg: dict):
        payload = json.dumps(msg, separators=(",", ":")).encode("utf-8")
        self.sock.sendall(struct.pack("<I", len(payload)) + payload)

    def recv_msg(self) -> dict | None:
        try:
            hdr = self._recv_exact(4)
        except socket.timeout:
            return None
        if hdr is None:
            raise ConnectionError("server closed the connection")
        (length,) = struct.unpack("<I", hdr)
        if length == 0 or length > (1 << 20):
            raise ConnectionError("bad frame length")
        body = self._recv_exact(length)
        if body is None:
            raise ConnectionError("server closed mid-frame")
        return json.loads(body.decode("utf-8"))

    def _recv_exact(self, n: int) -> bytes | None:
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                return None
            buf += chunk
        return buf

    def request(self, method: str, params: dict | None = None) -> dict:
        rid = self.next_id
        self.next_id += 1
        msg = {"id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        # Events may interleave; wait for our id.
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            reply = self.recv_msg()
            if reply is None:
                continue
            if reply.get("id") == rid:
                if "error" in reply:
                    raise RuntimeError(f"{method}: {reply['error']}")
                return reply.get("result", {})
        raise TimeoutError(f"{method}: no response")


def cmd_info(_args):
    targets = discover()
    if not targets:
        print(f"no targets advertised in {discovery_dir()}")
        return
    for t in targets:
        print(f"pid {t.get('pid'):>7}  port {t.get('port'):>5}  "
              f"affineui {t.get('affineui', '?'):>8}  {t.get('exe', '')}")


def cmd_ping(args):
    t = pick_target(args)
    c = Client(t["port"], t.get("token", ""))
    t0 = time.perf_counter()
    c.request("ping")
    ms = (time.perf_counter() - t0) * 1000.0
    h = c.hello
    print(f"pid {t.get('pid', '?')} affineui {h.get('affineui')} "
          f"protocol {h.get('protocol')} session {h.get('session_id')} "
          f"ping {ms:.1f} ms")
    c.close()


def stream_events(c: Client, seconds: float | None, max_frames: int | None):
    """Yield telemetry events until the time/frame budget is spent."""
    c.request("telemetry.subscribe")
    deadline = time.monotonic() + seconds if seconds else None
    frames = 0
    while True:
        if deadline and time.monotonic() >= deadline:
            return
        if max_frames and frames >= max_frames:
            return
        msg = c.recv_msg()
        if msg is None:
            continue  # timeout tick — idle target between heartbeats
        if "method" not in msg:
            continue
        yield msg
        if msg["method"] == "telemetry.frame":
            frames += 1


def cmd_watch(args):
    t = pick_target(args)
    c = Client(t["port"], t.get("token", ""))
    print(f"attached: session {c.hello.get('session_id')} "
          f"affineui {c.hello.get('affineui')} — Ctrl-C to stop")
    try:
        for msg in stream_events(c, args.seconds, None):
            p = msg.get("params", {})
            if msg["method"] == "telemetry.frame":
                print(f"frame {p.get('frame'):>7}  gap {p.get('gap_ms'):>7.2f} ms  "
                      f"cb {p.get('cb_ms'):>6.2f} ms  "
                      f"layout {p.get('stage_us', {}).get('layout', 0):>6} us  "
                      f"mem {p.get('mem', {}).get('live', 0):>10}")
            elif msg["method"] == "target.idle":
                print(f"idle  t={p.get('t_ms', 0):.0f} ms  "
                      f"skipped {p.get('skipped')}")
            elif msg["method"] == "telemetry.dropped":
                print(f"!! dropped {p.get('count')} records (slow reader)")
    except KeyboardInterrupt:
        pass
    finally:
        c.close()


def cmd_dump(args):
    t = pick_target(args)
    c = Client(t["port"], t.get("token", ""))
    session = dict(c.hello)
    session.update({"v": 1, "type": "session"})
    n = 0
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(json.dumps(session, separators=(",", ":")) + "\n")
        try:
            for msg in stream_events(c, args.seconds, args.frames):
                f.write(json.dumps(msg.get("params", {}),
                                   separators=(",", ":")) + "\n")
                n += 1
        except KeyboardInterrupt:
            pass
    c.close()
    print(f"wrote {n} records to {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pid", type=int, help="attach to this pid")
    ap.add_argument("--port", type=int, help="explicit port (skips discovery)")
    ap.add_argument("--token", help="auth token (with --port)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("info")
    sub.add_parser("ping")
    w = sub.add_parser("watch")
    w.add_argument("--seconds", type=float, default=None)
    d = sub.add_parser("dump")
    d.add_argument("out")
    d.add_argument("--frames", type=int, default=None)
    d.add_argument("--seconds", type=float, default=None)

    args = ap.parse_args()
    {"info": cmd_info, "ping": cmd_ping,
     "watch": cmd_watch, "dump": cmd_dump}[args.cmd](args)


if __name__ == "__main__":
    main()

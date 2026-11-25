#!/usr/bin/env python3
"""Print every stratum line a proxy/pool sends back, for manual inspection.

usage: python scripts/trace_client.py --target 127.0.0.1:3333 \
           --login 4ABC.../worker [--seconds 12] [--send-submit]
"""
import argparse
import json
import socket
import time


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="127.0.0.1:3333")
    ap.add_argument("--login", required=True)
    ap.add_argument("--password", default="x")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--algo", default="rx/0")
    args = ap.parse_args()

    host, _, port = args.target.rpartition(":")
    sock = socket.create_connection((host, int(port)), timeout=10)
    sock.settimeout(0.5)
    started = time.time()

    req = {
        "id": 1,
        "jsonrpc": "2.0",
        "method": "login",
        "params": {
            "login": args.login,
            "pass": args.password,
            "agent": "trace-client/1.0",
            "algo": [args.algo],
        },
    }
    line = json.dumps(req, separators=(",", ":"))
    print(f"{0.0:6.2f}s  >> {line}", flush=True)
    sock.sendall((line + "\n").encode())

    buf = b""
    while time.time() - started < args.seconds:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            print("connection closed")
            break
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            text = raw.decode(errors="replace").strip()
            if not text:
                continue
            age = time.time() - started
            tag = ""
            try:
                msg = json.loads(text)
                if msg.get("method") == "job":
                    job = msg.get("params", {})
                    tag = f"   <-- JOB job_id={job.get('job_id')} height={job.get('height')}"
                elif msg.get("method"):
                    tag = f"   <-- notification {msg.get('method')}"
            except json.JSONDecodeError:
                tag = "   <-- unparsable"
            print(f"{age:6.2f}s  << {text}{tag}", flush=True)

    sock.close()


if __name__ == "__main__":
    main()

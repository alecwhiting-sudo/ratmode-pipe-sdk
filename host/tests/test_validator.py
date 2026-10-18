#!/usr/bin/env python3
"""Runs ratpipe-host against the deliberately bad fixtures and checks that each
one fails exactly the check it was built to break (and that the control passes).

Also runs RatMode's vendored load-time Mach-O check (`--check-macho`) on the
cross-built fixture dylib in <macho dir>: it must be accepted, a truncated copy
must be refused with a reason, and a Linux .so must be reported as not Mach-O.

usage: test_validator.py <ratpipe-host> <dir with bad_*.so> [<dir with *.dylib fixtures>]
"""
import json
import os
import subprocess
import sys
import tempfile

HOST, DIR = sys.argv[1], sys.argv[2]
MACHO_DIR = sys.argv[3] if len(sys.argv) > 3 else None
EXT = ".dylib" if sys.platform == "darwin" else ".so"

# fixture -> (checks that must FAIL, extra args)
CASES = {
    "nan":       (["V4"], []),
    "peak":      (["V4"], []),
    "overwrite": (["V4"], []),
    "oob":       (["V4"], []),
    "dc":        (["V4"], []),
    "alloc":     (["V5"], []),
    "lock":      (["V6"], []),
    "io":        (["V6"], []),
    "ids":       (["V2"], []),
    "struct":    (["V1"], []),
    "desc_size": (["V1"], []),
    "export":    (["V1"], []),
    "bounds":    (["V3"], []),
    "roundtrip": (["V8"], []),
    "state":     (["V9"], []),
    "drone":     (["V10"], []),
    "nondet":    (["V11"], []),
    "slow":      (["V7"], ["--only", "V7"]),
}

failures = 0


def run(lib, extra):
    cmd = [HOST, "validate", lib, "--json", "--quick", "--seeds", "10"] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    try:
        return p.returncode, json.loads(p.stdout)
    except Exception:
        print("  host output not JSON:\n" + p.stdout + p.stderr)
        return p.returncode, None


def status(rep, cid):
    for c in rep["checks"]:
        if c["id"] == cid:
            return c["status"]
    return None


def expect(name, cond, msg):
    global failures
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  -- " + msg))
    if not cond:
        failures += 1


# control: well-behaved variant must pass everything it runs
rc, rep = run(os.path.join(DIR, "control" + EXT), [])
expect("control passes", rc == 0 and rep is not None and rep["result"] == "pass",
       json.dumps(rep, indent=1) if rep else "no report")

for name, (must_fail, extra) in CASES.items():
    rc, rep = run(os.path.join(DIR, "bad_" + name + EXT), extra)
    if rep is None:
        expect(name, False, "no report")
        continue
    got = [c["id"] for c in rep["checks"] if c["status"] == "fail"]
    ok = rc == 1 and all(m in got for m in must_fail)
    expect("bad_%s -> %s fails" % (name, "+".join(must_fail)), ok, "failing checks: %s (rc=%d)" % (got, rc))

# a pipe whose render never returns is killed by the watchdog and reported as a hang, naming the stage
p = subprocess.run([HOST, "validate", os.path.join(DIR, "bad_hang" + EXT), "--json", "--quick", "--only", "V4", "--timeout", "3"],
                   capture_output=True, text=True, timeout=60)
try:
    rep = json.loads(p.stdout)
    expect("hanging pipe reported as hang during V4", p.returncode == 1 and rep.get("result") == "hang" and "V4" in rep.get("during", ""), p.stdout)
except Exception:
    expect("hanging pipe reported as hang during V4", False, p.stdout + p.stderr)

# a pipe that SIGSEGVs is reported, not fatal to the host
p = subprocess.run([HOST, "validate", os.path.join(DIR, "crash" + EXT), "--json", "--quick"], capture_output=True, text=True)
try:
    rep = json.loads(p.stdout)
    expect("crash pipe reported as crash", p.returncode == 1 and rep.get("result") == "crash", p.stdout)
except Exception:
    expect("crash pipe reported as crash", False, p.stdout + p.stderr)

# RatMode's load-time Mach-O check, on any platform (pure C++ over the bytes)
if MACHO_DIR:
    good = os.path.join(MACHO_DIR, "sdk_smoke_fat.dylib")
    p = subprocess.run([HOST, "--check-macho", good], capture_output=True, text=True)
    expect("check-macho accepts the cross-built smoke pipe (fat, 2 slices)",
           p.returncode == 0 and "OK" in p.stdout and "2 slice" in p.stdout, p.stdout + p.stderr)
    with open(good, "rb") as f:
        blob = f.read()
    with tempfile.TemporaryDirectory() as td:
        bad = os.path.join(td, "truncated.dylib")
        with open(bad, "wb") as f:
            f.write(blob[: len(blob) * 2 // 3])
        p = subprocess.run([HOST, "--check-macho", bad], capture_output=True, text=True)
        expect("check-macho refuses a truncated image with a reason",
               p.returncode == 1 and "REFUSED" in p.stdout and len(p.stdout.strip()) > len(bad) + 12, p.stdout + p.stderr)
    p = subprocess.run([HOST, "--check-macho", os.path.join(DIR, "control" + EXT)], capture_output=True, text=True)
    if sys.platform == "darwin":
        expect("check-macho on the control library", p.returncode in (0, 1), p.stdout)
    else:
        expect("check-macho reports a Linux .so as not Mach-O (rc 2)", p.returncode == 2 and "NOT MACH-O" in p.stdout, p.stdout + p.stderr)

sys.exit(1 if failures else 0)

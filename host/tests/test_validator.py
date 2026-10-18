#!/usr/bin/env python3
"""Runs ratpipe-host against the deliberately bad fixtures and checks that each
one fails exactly the check it was built to break (and that the control passes).

usage: test_validator.py <ratpipe-host> <dir with bad_*.so>
"""
import json
import os
import subprocess
import sys

HOST, DIR = sys.argv[1], sys.argv[2]
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

# a pipe that SIGSEGVs is reported, not fatal to the host
p = subprocess.run([HOST, "validate", os.path.join(DIR, "crash" + EXT), "--json", "--quick"], capture_output=True, text=True)
try:
    rep = json.loads(p.stdout)
    expect("crash pipe reported as crash", p.returncode == 1 and rep.get("result") == "crash", p.stdout)
except Exception:
    expect("crash pipe reported as crash", False, p.stdout + p.stderr)

sys.exit(1 if failures else 0)

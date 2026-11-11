#!/usr/bin/env python3
"""MiniVectorDB local test driver (official JSONL protocol).

Replays the public mysqltest package (20 cases) against the binary:
  * "--restart" directive: JSONL shutdown, wait for exit, relaunch with the
    SAME data dir
  * "--mode ann"/"--mode exact": change search_mode for subsequent requests

Usage:
  python3 driver.py <binary> <mysqltest-dir> [case-id ...]
"""
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from transport import Db  # noqa: E402


def check_case(binary, test_path, result_path, work_dir):
    with open(result_path, "r", encoding="utf-8") as f:
        assertions = json.load(f)
    with open(test_path, "r", encoding="utf-8") as f:
        stmts = [l.rstrip("\n") for l in f if l.strip()]

    data_dir = os.path.join(work_dir, "data")
    os.makedirs(data_dir, exist_ok=True)
    db = Db([binary, "--data-dir", data_dir], data_dir)
    db.start()

    idx = 0  # statement counter (directives don't consume assertions)
    failures = []
    mode = "exact"
    try:
        for stmt in stmts:
            if stmt == "--restart":
                db.shutdown()
                db.start()
                continue
            if stmt in ("--mode exact", "--mode ann"):
                mode = stmt.split()[1]
                continue
            resp = db.send(stmt, mode)
            expect = assertions[idx]
            idx += 1
            ok, why = check_response(stmt, expect, resp)
            if not ok:
                failures.append(why)
    finally:
        db.stop()
    return failures


def check_response(stmt, expect, resp):
    if expect == {}:
        if not resp.get("ok"):
            return False, f"expected ok for {stmt!r}, got {resp}"
        return True, None
    if expect.get("error") is not None:
        if resp.get("ok"):
            return False, f"expected error {expect['error']} for {stmt!r}, got {resp}"
        code = (resp.get("error") or {}).get("code")
        if not isinstance(code, str) or not code:
            return False, f"missing error.code for {stmt!r}: {resp}"
        if expect["error"] and code != expect["error"]:
            return False, f"error code mismatch for {stmt!r}: expected {expect['error']}, got {code}"
        return True, None
    if not resp.get("ok"):
        return False, f"expected success for {stmt!r}, got {resp}"
    if "affected_rows" in expect:
        got = resp.get("affected_rows")
        if got != expect["affected_rows"]:
            return False, f"affected_rows mismatch for {stmt!r}: expected {expect['affected_rows']}, got {got} ({resp})"
        return True, None
    if "columns" in expect:
        if resp.get("columns") != expect["columns"]:
            return False, f"columns mismatch for {stmt!r}: expected {expect['columns']}, got {resp.get('columns')}"
        unordered = bool(expect.get("unordered"))
        if not rows_match(expect.get("rows", []), resp.get("rows", []), unordered):
            return False, f"rows mismatch for {stmt!r}:\n  expected {expect.get('rows')}\n  got      {resp.get('rows')}"
        return True, None
    return False, f"unsupported assertion for {stmt!r}: {expect}"


def close_enough(a, b, rtol=1e-5, atol=1e-5):
    return abs(a - b) <= atol + rtol * abs(b)


def row_matches(expected_row, actual_row):
    if len(expected_row) != len(actual_row):
        return False
    for e, a in zip(expected_row, actual_row):
        if isinstance(e, (int, float)) and not isinstance(e, bool):
            if isinstance(a, bool) or not isinstance(a, (int, float)):
                return False
            if isinstance(e, int) and isinstance(a, int):
                if e != a:
                    return False
            else:
                if not close_enough(float(e), float(a)):
                    return False
        else:
            if e != a:
                return False
    return True


def rows_match(expected, actual, unordered):
    if len(expected) != len(actual):
        return False
    if not unordered:
        return all(row_matches(e, a) for e, a in zip(expected, actual))
    used = [False] * len(actual)
    for e in expected:
        found = None
        for i, a in enumerate(actual):
            if used[i]:
                continue
            if row_matches(e, a):
                found = i
                break
        if found is None:
            return False
        used[found] = True
    return True


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    mdir = sys.argv[2]
    only = set(sys.argv[3:])
    with open(os.path.join(mdir, "manifest.json"), "r", encoding="utf-8") as f:
        manifest = json.load(f)
    total_fail = 0
    passed = 0
    for case in manifest["cases"]:
        cid = case["id"]
        if only and cid not in only:
            continue
        work = tempfile.mkdtemp(prefix=f"mvdb-{cid}-")
        try:
            fails = check_case(binary, os.path.join(mdir, case["test"]),
                               os.path.join(mdir, case["result"]), work)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        if fails:
            total_fail += 1
            print(f"FAIL {cid}")
            for f in fails:
                print(f"   {f}")
        else:
            passed += 1
            print(f"PASS {cid}")
    print(f"\n{passed} passed, {total_fail} failed")
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())

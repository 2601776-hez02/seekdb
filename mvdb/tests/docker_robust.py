#!/usr/bin/env python3
"""Run robust.py's suite against the Docker image with resource limits.

Usage: build the image first (`docker build -t minivectordb:test .` at the
repo root), then: python3 tests/docker_robust.py
"""
import json
import os
import subprocess
import sys

sys.argv = ["docker_robust", "/unused"]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import robust  # noqa: E402
from transport import Db as _Base  # noqa: E402

IMAGE = "minivectordb:test"


class DockerDb(_Base):
    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.proc = None
        self.seq = 0
        self.env = None

    def start(self, expect_ready=True):
        self.proc = subprocess.Popen(
            ["docker", "run", "--rm", "-i", "--cpus", "2", "--memory", "2g",
             "-v", self.data_dir + ":/data", IMAGE],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, bufsize=1)
        if expect_ready:
            line = self.proc.stdout.readline()
            assert line, "no ready line"
            msg = json.loads(line)
            assert msg == {"type": "ready"}, msg

    def kill9(self):
        if self.proc is None:
            return
        subprocess.run(["docker", "kill", "-s", "KILL", self._cid()],
                       stdout=subprocess.PIPE)
        self.proc.wait()
        self.proc = None

    def _cid(self):
        out = subprocess.run(["docker", "ps", "-q", "--filter",
                              "ancestor=" + IMAGE, "--latest"],
                             stdout=subprocess.PIPE, universal_newlines=True)
        return out.stdout.strip()

    def stop(self):
        if self.proc is None or self.proc.poll() is not None:
            self.proc = None
            return
        try:
            self.shutdown()
        except Exception:
            try:
                self.proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                subprocess.run(["docker", "kill", self._cid()], stdout=subprocess.PIPE)
                self.proc.wait()
        self.proc = None


robust.Db = DockerDb
robust.main()

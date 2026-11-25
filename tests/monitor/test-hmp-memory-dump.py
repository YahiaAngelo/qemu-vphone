#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run with uv run tests/monitor/test-hmp-memory-dump.py /path/to/qemu-system-aarch64."""

import json
import resource
import subprocess
import sys
import unittest
from pathlib import Path


def disable_core_dumps():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


class HMPMemoryDump(unittest.TestCase):
    def check_instruction_dump(self, command):
        requests = [
            {"execute": "qmp_capabilities", "id": "capabilities"},
            {
                "execute": "human-monitor-command",
                "arguments": {"command-line": command, "cpu-index": 0},
                "id": "instructions",
            },
            {
                "execute": "human-monitor-command",
                "arguments": {"command-line": "xp/4wx 0x40010000"},
                "id": "memory",
            },
            {"execute": "query-status", "id": "status"},
            {"execute": "quit", "id": "quit"},
        ]
        result = subprocess.run(
            [
                QEMU,
                "-machine",
                "virt",
                "-accel",
                "tcg",
                "-cpu",
                "cortex-a57",
                "-m",
                "64M",
                "-S",
                "-nodefaults",
                "-display",
                "none",
                "-monitor",
                "none",
                "-serial",
                "none",
                "-qmp",
                "stdio",
            ],
            input="".join(json.dumps(request) + "\n" for request in requests),
            capture_output=True,
            check=False,
            text=True,
            timeout=15,
            preexec_fn=disable_core_dumps,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        replies = {
            reply["id"]: reply
            for line in result.stdout.splitlines()
            if "id" in (reply := json.loads(line))
        }
        self.assertEqual(
            replies["instructions"]["return"],
            "Instruction disassembly is not supported\r\n",
        )
        self.assertRegex(
            replies["memory"]["return"],
            r"40010000: 0x00000000 0x00000000 0x00000000 0x00000000",
        )
        self.assertEqual(replies["status"]["return"]["status"], "prelaunch")
        self.assertFalse(replies["status"]["return"]["running"])
        self.assertEqual(replies["quit"]["return"], {})

    def test_virtual_instruction_dump(self):
        self.check_instruction_dump("x/16i 0x40000000")

    def test_physical_instruction_dump(self):
        self.check_instruction_dump("xp/16i 0x40000000")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} /path/to/qemu-system-aarch64")
    QEMU = str(Path(sys.argv.pop()).resolve(strict=True))
    unittest.main()

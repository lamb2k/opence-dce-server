"""The shared per-peer budget module (port/third_party/net_budget: lamb2k/dc-halo src/net_budget.h and src/net/net_budget.c at
022f93b5, verbatim; its unit test too) on its own: built with no game headers, strict, fast-math and,
where the compiler has them, address/undefined sanitizers. Run: python3 tools/test_net_budget.py"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "port" / "third_party" / "net_budget"
CC = shutil.which(os.environ.get("CC", "gcc"))


class BudgetModule(unittest.TestCase):
    def build_run(self, extra):
        if not CC:
            self.skipTest("needs a host compiler")
        with tempfile.TemporaryDirectory(prefix="net-budget-unit-") as temp:
            exe = Path(temp) / "unit.exe"
            build = subprocess.run([CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *extra, "-Isrc", "tests/net_budget_unit_test.c",
                                    "src/net/net_budget.c", "-o", str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=120)
            if build.returncode and any(f.startswith("-fsanitize") for f in extra):
                self.skipTest("no sanitizer runtime: " + build.stderr[-200:])
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("net_budget_unit: 0 failures", run.stdout)

    def test_strict(self):
        self.build_run([])

    def test_fast_math(self):
        self.build_run(["-ffast-math", "-fno-math-errno"])

    def test_sanitizers(self):
        self.build_run(["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1"])

    def test_module_is_game_independent(self):
        """The module includes nothing of the game, so the crossplay transport can adopt it unchanged."""
        for path in ("src/net_budget.h", "src/net/net_budget.c"):
            includes = [l for l in (ROOT / path).read_text(encoding="utf-8").splitlines() if l.startswith("#include")]
            for line in includes:
                self.assertIn(line.split()[1].strip('"<>'), ("stdint.h", "string.h", "../net_budget.h", "net_budget.h"), f"{path}: {line}")


if __name__ == "__main__":
    unittest.main()

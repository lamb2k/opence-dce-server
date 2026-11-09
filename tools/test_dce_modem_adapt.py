"""The official server's adaptive modem budget and period (port/linux/game/dce_modem_adapt.c) against a model of a
modem line (port/linux/tests/dce_modem_adapt_test.c: 1440, 2200 and 3360 B/s lines, a drop and a rise, sparse and
missing acknowledgements), built with no game headers: strict, and with address/undefined sanitizers where the
compiler has them. Modelled, not measured. Run: python3 tools/test_dce_modem_adapt.py"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which(os.environ.get("CC", "gcc"))


class ModemAdapt(unittest.TestCase):
    def build_run(self, extra):
        if not CC:
            self.skipTest("needs a host compiler")
        with tempfile.TemporaryDirectory(prefix="dce-modem-adapt-") as temp:
            exe = Path(temp) / "model"
            build = subprocess.run([CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *extra,
                                    "port/linux/tests/dce_modem_adapt_test.c", "port/linux/game/dce_modem_adapt.c", "-o", str(exe)],
                                   cwd=ROOT, capture_output=True, text=True, timeout=120)
            if build.returncode and any(f.startswith("-fsanitize") for f in extra):
                self.skipTest("no sanitizer runtime: " + build.stderr[-200:])
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("dce_modem_adapt: 0 failures", run.stdout)
            return run.stdout

    def test_model(self):
        print("\n" + self.build_run([]))

    def test_sanitizers(self):
        self.build_run(["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-O1"])

    def test_module_is_verbatim(self):
        """The shared module is the client's, unchanged: dce_modem_adapt.c includes it, it includes nothing of the game."""
        module = ROOT / "port/third_party/net_budget/src"
        for path in (module / "net_budget.h", module / "net/net_budget.c"):
            includes = [l for l in path.read_text(encoding="utf-8").splitlines() if l.startswith("#include")]
            for line in includes:
                self.assertIn(line.split()[1].strip('"<>'), ("stdint.h", "string.h", "../net_budget.h"), f"{path}: {line}")
        source = (ROOT / "port/linux/game/dce_modem_adapt.c").read_text(encoding="utf-8")
        self.assertIn('#include "../../third_party/net_budget/src/net/net_budget.c"', source)

    def test_server_wiring(self):
        """Adapt is a runtime switch (HALO_DCE_MODEM_ADAPT, on unless 0); off, the fixed-budget controller runs as before."""
        source = (ROOT / "port/linux/game/network_distributed.c").read_text(encoding="utf-8")
        self.assertIn('getenv("HALO_DCE_MODEM_ADAPT")', source)
        self.assertIn('getenv("HALO_DCE_MODEM_BUDGET_MAX")', source)
        self.assertIn("long wanted = period * rate / budget + 1;", source)
        self.assertIn("else if (rate * 10 < budget * 6 && period > DCE_SLOW_PERIOD_MIN)", source)


if __name__ == "__main__":
    unittest.main()

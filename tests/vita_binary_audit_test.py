#!/usr/bin/env python3
"""Unit checks for the native ELF audit's nm classification (no device needed)."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "native_audit", Path(__file__).resolve().parents[1] / "tools/check_vita_gxm_binary.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class NativeSymbolAudit(unittest.TestCase):
    def test_only_code_is_treated_as_an_entry_point(self):
        self.assertEqual(module.executable_symbols("""
81000000 T sceGxmDraw
81000010 T glDrawElements
81000020 t vglPrivateHelper
81000030 W _glBindTexture
81400000 B glInventory
81400040 D glRenderBuffer
81400080 R glModeNames
"""), {"sceGxmDraw", "glDrawElements", "vglPrivateHelper", "glBindTexture"})

    def test_data_cannot_satisfy_required_gxm_functions(self):
        self.assertEqual(module.executable_symbols("81000000 B sceGxmDraw\n"), set())


if __name__ == "__main__":
    unittest.main()

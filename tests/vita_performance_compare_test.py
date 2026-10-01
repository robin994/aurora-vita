import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("compare", Path(__file__).resolve().parents[1] / "tools/compare_vita_performance.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


class PerformanceSamples(unittest.TestCase):
    def test_sparse_samples_and_warmup(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.log"
            path.write_text("frames=120 frame_us=999\n" + "".join(
                f"[AURORA-VITA][FRAME] frame={i * 300} total_us={time} draws=7\n"
                for i, time in enumerate([999, 10, 20, 30, 40, 50])))
            frames = compare.read_frames(path, 1)
            result = compare.summarize(frames, "fixture")
            self.assertEqual(result["sample_count"], 5)
            self.assertFalse(result["consecutive_samples"])
            self.assertEqual(result["frame_gaps"], [300])
            self.assertEqual(result["frame_us"]["median"], 30)
            self.assertEqual(result["frame_us"]["p95"], 48)

    def test_averages_and_reset_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "run.log"
            path.write_text("frames=120 frame_us=1234\n")
            with self.assertRaises(ValueError):
                compare.read_frames(path)
            path.write_text("[AURORA-VITA][FRAME] frame=1 total_us=10\n" * 2)
            with self.assertRaises(ValueError):
                compare.read_frames(path)


if __name__ == "__main__":
    unittest.main()

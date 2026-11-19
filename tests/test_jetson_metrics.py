import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("jetson_metrics", Path(__file__).parents[1]/"tools"/"jetson_metrics.py")
metrics = importlib.util.module_from_spec(spec)
spec.loader.exec_module(metrics)

class MetricsTests(unittest.TestCase):
    def test_optional_observations(self):
        self.assertEqual(metrics.parse_tegrastats("unrecognized"), {})
        result = metrics.parse_tegrastats("RAM 2031/31918MB GR3D_FREQ 55% cpu@43.5C gpu@45C")
        self.assertEqual(result["ram_used_bytes"], 2031*1024*1024)
        self.assertEqual(result["gpu_percent"], 55)
        self.assertEqual(result["temperatures_celsius"]["gpu"], 45)
        self.assertNotIn("gpu_percent", metrics.parse_tegrastats("RAM 2/100MB GR3D_FREQ @[0,0]"))

if __name__ == "__main__":
    unittest.main()

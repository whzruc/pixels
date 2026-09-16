import importlib.util
import io
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("static_query_buffer_plan.py")
SPEC = importlib.util.spec_from_file_location("static_query_buffer_plan", MODULE_PATH)
planner = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(planner)


class StaticQueryBufferPlanTest(unittest.TestCase):
    def plan(self, text: str, alignment: int = 1) -> dict:
        import csv

        return planner.build_plan(csv.DictReader(io.StringIO(text)), alignment)

    def test_uses_independent_slot_high_water_marks(self):
        result = self.plan(
            "worker,sequence,column,bytes\n"
            "0,0,title,100\n"
            "0,1,title,10\n"
            "0,2,title,80\n"
            "0,3,title,20\n"
        )
        self.assertEqual(result["planned_bytes"], 120)
        self.assertEqual(result["symmetric_bytes"], 200)
        self.assertEqual(result["saved_bytes"], 80)

    def test_keeps_workers_and_columns_independent(self):
        result = self.plan(
            "worker,sequence,column,bytes\n"
            "0,0,a,5\n0,1,a,7\n0,0,b,11\n1,0,a,13\n"
        )
        self.assertEqual(result["planned_bytes"], 36)
        self.assertEqual(result["symmetric_bytes"], 62)

    def test_aligns_each_registered_region(self):
        result = self.plan(
            "worker,sequence,column,bytes\n0,0,a,4097\n0,1,a,1\n",
            alignment=4096,
        )
        self.assertEqual(result["planned_bytes"], 12288)
        self.assertEqual(result["symmetric_bytes"], 16384)

    def test_rejects_invalid_input(self):
        with self.assertRaisesRegex(ValueError, "non-negative"):
            self.plan("worker,sequence,column,bytes\n0,-1,a,1\n")
        with self.assertRaisesRegex(ValueError, "non-negative"):
            self.plan("worker,sequence,column,bytes\n0,0,a,-1\n")


if __name__ == "__main__":
    unittest.main()

import importlib.util
import csv
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('runner', Path(__file__).with_name('run_selective_benchmark.py'))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class ConfigurationTest(unittest.TestCase):
    def config(self, mode):
        text = runner.properties('pixels.dynamic.selective.enabled=true\npixel.enable.globalBytebuffer=true\n', mode, 4, Path('/tmp/sizes.csv'))
        return dict(line.split('=', 1) for line in text.splitlines())

    def test_baselines_disable_selective(self):
        for mode in ('legacy', 'dynamic', 'static', 'static-locked', 'static-lockfree'):
            c = self.config(mode)
            self.assertEqual(c['pixels.dynamic.selective.enabled'], 'false')
            self.assertEqual(c['pixel.enable.globalBytebuffer'], 'false')

    def test_static_lock_ablation(self):
        locked = self.config('static-locked')
        lockfree = self.config('static-lockfree')
        self.assertEqual(locked['pixel.enable.globalStaticBytebuffer'], 'true')
        self.assertEqual(locked['pixels.static.buffer.lock_free_lookup'], 'false')
        self.assertEqual(lockfree['pixel.enable.globalStaticBytebuffer'], 'true')
        self.assertEqual(lockfree['pixels.static.buffer.lock_free_lookup'], 'true')

    def test_queue_sweep(self):
        for limit in (0, 1, 2, 4):
            c = self.config(f'selective-{limit}')
            self.assertEqual(c['pixels.dynamic.selective.enabled'], 'true')
            self.assertEqual(c['pixels.dynamic.selective.queue.limit'], str(limit))
            self.assertEqual(c['pixels.enable.dynamic.buffer'], 'true')
            self.assertEqual(c['pixels.doublebuffer'], 'true')
            self.assertEqual(c['pixel.enable.globalStaticBytebuffer'], 'false')
            self.assertEqual(c['pixel.bufferpool.fixedSize'], 'false')

    def test_query_coverage(self):
        self.assertEqual(len(runner.QUERIES), 21)
        self.assertEqual(len(set(runner.QUERIES)), 21)
        self.assertIn('q24', runner.QUERIES)
        self.assertEqual(runner.FULL_QUERIES, [f'q{i:02d}' for i in range(44)])

    def test_v2_ablation_modes(self):
        expected = {
            'selective-base': ('false', 'false', 'false'),
            'selective-gate': ('true', 'false', 'false'),
            'selective-cost': ('false', 'true', 'false'),
            'selective-adaptive': ('false', 'false', 'true'),
            'selective-gate-cost': ('true', 'true', 'false'),
            'selective-all': ('true', 'true', 'true'),
        }
        for mode, flags in expected.items():
            c = self.config(mode)
            self.assertEqual(c['pixels.dynamic.selective.enabled'], 'true')
            self.assertEqual(c['pixels.dynamic.selective.metrics.enabled'], 'true')
            self.assertEqual(c['pixels.dynamic.selective.growth_gate.enabled'], flags[0])
            self.assertEqual(c['pixels.dynamic.selective.cost.enabled'], flags[1])
            self.assertEqual(c['pixels.dynamic.selective.adaptive.enabled'], flags[2])

    def test_empty_output_path_is_rejected(self):
        with self.assertRaises(Exception):
            runner.nonempty_path('')
        with self.assertRaises(Exception):
            runner.nonempty_path('   ')
        self.assertEqual(runner.nonempty_path('/tmp/result'), Path('/tmp/result'))

    def test_resume_reads_completed_summary_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            summary = Path(directory) / 'summary.csv'
            with summary.open('w', newline='') as output:
                writer = csv.DictWriter(output, fieldnames=runner.SUMMARY_FIELDS)
                writer.writeheader()
                writer.writerow(dict.fromkeys(runner.SUMMARY_FIELDS, ''))
            # Replace the fields needed to form the checkpoint key.
            lines = summary.read_text().splitlines()
            lines[1] = 'q00,legacy,0,,,,,'
            summary.write_text('\n'.join(lines) + '\n')
            self.assertEqual(runner.completed_cases(summary), {('q00', 'legacy', 0)})

    def test_resume_artifact_requirements_include_perf(self):
        with tempfile.TemporaryDirectory() as directory:
            case = Path(directory)
            for name in ('resource.txt', 'timed.stdout'):
                (case / name).write_text('ok')
            (case / 'timed.stderr').touch()
            self.assertTrue(runner.required_artifacts_exist(case, 'legacy', 1, True))
            self.assertFalse(runner.required_artifacts_exist(case, 'legacy', 0, True))

    def test_resume_repairs_incomplete_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            complete = output / 'q00-legacy-r0'
            incomplete = output / 'q00-dynamic-r0'
            complete.mkdir()
            incomplete.mkdir()
            for case in (complete, incomplete):
                (case / 'resource.txt').write_text('resource')
                (case / 'timed.stdout').write_text('output')
                (case / 'timed.stderr').touch()
            summary = output / 'summary.csv'
            with summary.open('w', newline='') as stream:
                writer = csv.DictWriter(stream, fieldnames=runner.SUMMARY_FIELDS)
                writer.writeheader()
                for mode in ('legacy', 'dynamic'):
                    row = dict.fromkeys(runner.SUMMARY_FIELDS, '')
                    row.update(query='q00', mode=mode, repeat=0)
                    writer.writerow(row)
            done = runner.repair_resume_summary(summary, output, perf=False)
            self.assertEqual(done, {('q00', 'legacy', 0), ('q00', 'dynamic', 0)})
            (incomplete / 'timed.stdout').write_text('')
            done = runner.repair_resume_summary(summary, output, perf=False)
            self.assertEqual(done, {('q00', 'legacy', 0)})
            self.assertEqual(runner.completed_cases(summary), {('q00', 'legacy', 0)})


if __name__ == '__main__':
    unittest.main()

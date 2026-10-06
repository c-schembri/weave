"""Synthetic validation of the bounded diagnostic trace format and event pairing."""

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import trace_runtime


def fixture(path, entries, *, thread=1, capacity=16):
    data = bytearray(trace_runtime.HEADER.pack(b"WEAVETR1", 1000, len(entries), 10, thread, capacity, 40))
    data.extend(bytes(capacity * trace_runtime.ENTRY.size))
    for sequence, (ticks, object, value, event) in enumerate(entries):
        trace_runtime.ENTRY.pack_into(data, trace_runtime.HEADER.size + sequence % capacity * 40,
                                     ticks, object, value, sequence, event, 0)
    path.write_bytes(data)


class TraceTests(unittest.TestCase):
    def test_queue_and_thread_intervals_are_paired_across_workers(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = root / "first.weavetrace", root / "second.weavetrace"
            fixture(first, [(100, 25, 0, 1), (110, 50, 1, 10), (140, 50, 1, 11)])
            fixture(second, [(120, 25, 0, 3), (121, 75, 0, 8), (123, 75, 0, 9), (125, 25, 0, 4)], thread=2)
            result = trace_runtime.analyze([first, second])
            self.assertTrue(result["diagnostic_only"])
            self.assertAlmostEqual(result["max_ms"]["queue_to_execute"], 20)
            self.assertAlmostEqual(result["max_ms"]["execute"], 5)
            self.assertAlmostEqual(result["max_ms"]["io_submit"], 2)
            self.assertAlmostEqual(result["max_ms"]["io_wait"], 30)
            self.assertEqual(result["unmatched_begins"], 0)
            self.assertEqual(result["unmatched_enqueues"], 0)

    def test_wrapped_ring_does_not_read_a_possibly_uncommitted_overwrite(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "wrapped.weavetrace"
            fixture(path, [(index, index, 0, 7) for index in range(10)], capacity=4)
            data = bytearray(path.read_bytes())
            trace_runtime.ENTRY.pack_into(data, 64 + 10 % 4 * 40, 10, 10, 0, 10, 7, 0)
            path.write_bytes(data)
            result = trace_runtime.analyze([path])
            self.assertEqual(result["events"]["wake"], 3)
            self.assertEqual(result["files"][0]["overwritten"], 6)
            self.assertEqual(result["files"][0]["retained"], 3)

    def test_invalid_layout_corrupt_sequence_and_time_reversal_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "invalid.weavetrace"
            path.write_bytes(b"short")
            with self.assertRaises(ValueError):
                trace_runtime.analyze([path])
            fixture(path, [(100, 1, 0, 7), (90, 1, 0, 7)])
            with self.assertRaises(ValueError):
                trace_runtime.analyze([path])
            fixture(path, [(100, 1, 0, 7)])
            data = bytearray(path.read_bytes())
            data[0] = 0
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                trace_runtime.analyze([path])


if __name__ == "__main__":
    unittest.main()

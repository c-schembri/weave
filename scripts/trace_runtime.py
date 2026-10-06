"""Inspect opt-in Weave execution traces after every writer process has exited."""

import argparse
from collections import Counter
import heapq
import json
import mmap
from pathlib import Path
import struct

from support.common import cli, require, write_json


HEADER = struct.Struct("<8sQQIIII24x")
ENTRY = struct.Struct("<QQQQII")
EVENTS = ("", "enqueue", "steal", "execute_begin", "execute_end", "park_begin", "park_end", "wake",
          "io_submit_begin", "io_submit_end", "io_wait_begin", "io_wait_end", "io_complete", "local_completion")
PAIRS = {3: 4, 5: 6, 8: 9, 10: 11}


def records(path, info):
    with path.open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        require(len(data) >= HEADER.size, "Truncated trace header.")
        magic, frequency, total, process, thread, capacity, size = HEADER.unpack_from(data)
        require(magic == b"WEAVETR1" and frequency > 0 and capacity > 0 and size == ENTRY.size and
                len(data) == HEADER.size + capacity * size, "Invalid trace layout.")
        info.append({"path": str(path), "process": process, "thread": thread, "committed": total,
                     "overwritten": max(0, total - capacity)})
        previous = 0
        # Termination can interrupt an overwrite of the oldest retained ring slot.
        start = max(0, total - capacity + (1 if total > capacity else 0))
        info[-1]["retained"] = total - start
        for sequence in range(start, total):
            ticks, object, value, stored, event, _ = ENTRY.unpack_from(data, HEADER.size + sequence % capacity * size)
            require(stored == sequence and 0 < event < len(EVENTS) and ticks >= previous, "Incomplete or corrupt trace entry.")
            previous = ticks
            yield ticks / frequency, process, thread, event, object, value


def analyze(paths):
    info = []
    counts = Counter()
    pending, queued = {}, {}
    totals = Counter()
    maxima = {}
    longest = []

    def duration(name, start, end, process, thread, object):
        milliseconds = (end - start) * 1000
        totals[name] += 1
        maxima[name] = max(maxima.get(name, 0), milliseconds)
        value = (milliseconds, name, process, thread, object, start, end)
        if len(longest) < 40:
            heapq.heappush(longest, value)
        else:
            heapq.heappushpop(longest, value)

    reverse = {end: begin for begin, end in PAIRS.items()}
    for timestamp, process, thread, event, object, value in heapq.merge(*(records(path, info) for path in paths)):
        counts[EVENTS[event]] += 1
        if event == 1:
            queued[process, object] = timestamp
        elif event == 3:
            start = queued.pop((process, object), None)
            if start is not None:
                duration("queue_to_execute", start, timestamp, process, thread, object)
        key = process, thread, object
        if event in PAIRS:
            pending[*key, event] = timestamp
        elif event in reverse:
            begin = reverse[event]
            start = pending.pop((*key, begin), None)
            if start is not None:
                duration(EVENTS[begin].removesuffix("_begin"), start, timestamp, process, thread, object)
    return {"diagnostic_only": True, "files": info, "events": dict(counts), "paired_intervals": dict(totals),
            "max_ms": maxima, "unmatched_begins": len(pending), "unmatched_enqueues": len(queued),
            "longest": [{"ms": value[0], "kind": value[1], "process": value[2], "thread": value[3],
                         "object": hex(value[4]), "start_seconds": value[5], "end_seconds": value[6]}
                        for value in sorted(longest, reverse=True)],
            "caution": "Instrumented timings are not benchmark evidence. Wrapped buffers and process termination can leave unmatched events. Long I/O waits alone do not establish a bug."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    paths = sorted(args.directory.glob("*.weavetrace"))
    require(paths, "No traces found.")
    result = analyze(paths)
    if args.output:
        require(not args.output.exists(), "Refusing to overwrite trace analysis.")
        write_json(args.output, result)
        print(json.dumps({key: result[key] for key in ("events", "max_ms", "caution")}, indent=2))
    else:
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    cli(main)

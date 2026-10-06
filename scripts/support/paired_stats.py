"""The gate's fixed-seed, whole-block bootstrap; compatible with archived analysis."""

from functools import lru_cache
import math
from statistics import median


RESAMPLES = 20000


class ProtocolRandom:
    """Seeded System.Random-compatible subtractive generator used by the original protocol.

    Keep the resample stream stable when reanalyzing pre-Python evidence. This is
    deliberately not Python's different random.Random stream or a security RNG.
    """

    def __init__(self, seed):
        self.values = [0] * 56
        previous = 161803398 - abs(seed)
        self.values[55] = previous
        current = 1
        for index in range(1, 55):
            position = (21 * index) % 55
            self.values[position] = current
            current = (previous - current) % 2147483647
            previous = self.values[position]
        for _ in range(4):
            for index in range(1, 56):
                self.values[index] = (self.values[index] - self.values[1 + (index + 30) % 55]) % 2147483647
        self.first, self.second = 0, 21

    def next(self, maximum):
        self.first = self.first % 55 + 1
        self.second = self.second % 55 + 1
        value = (self.values[self.first] - self.values[self.second]) % 2147483647
        self.values[self.first] = value
        return int(value * (1.0 / 2147483647) * maximum)


def summary(values):
    ordered = sorted(values)
    mean = sum(value / len(ordered) for value in ordered)
    variance = sum((value - mean) ** 2 for value in ordered)
    cv = math.sqrt(variance / (len(ordered) - 1)) / mean if mean and len(ordered) > 1 else 0.0
    return median(ordered), cv


@lru_cache(maxsize=16)
def resample_indices(sizes, seed):
    random = ProtocolRandom(seed)
    return tuple(tuple(random.next(size) for size in sizes for _ in range(size)) for _ in range(RESAMPLES))


def bounds(values, upper=19000):
    # .NET sorts undefined ratios before finite values; preserve that convention.
    values.sort(key=lambda value: (not math.isnan(value), value))
    return values[999], values[upper]


def interval(ratios):
    return paired_interval(tuple(ratios))


@lru_cache(maxsize=256)
def paired_interval(ratios):
    indices = resample_indices((len(ratios),), 3601)
    return bounds([median(ratios[index] for index in sample) for sample in indices])


def independent_interval(before, after, seed=9203, *, exploratory=False):
    indices = resample_indices((len(before), len(after)), seed)
    ratios = []
    for sample in indices:
        denominator = median(before[index] for index in sample[:len(before)])
        numerator = median(after[index] for index in sample[len(before):])
        ratio = numerator / denominator if denominator else (math.inf if numerator else math.nan)
        ratios.append(ratio)
    return bounds(ratios, 18999 if exploratory else 19000)

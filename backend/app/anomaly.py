"""Rolling z-score anomaly detection.

Logic ported from the power_signal_tools portfolio library:
a value is anomalous when it deviates more than `threshold` standard
deviations from the rolling mean of the previous `window` samples.
"""
from collections import deque
from dataclasses import dataclass
from math import sqrt


@dataclass
class AnomalyResult:
    is_anomaly: bool
    zscore: float
    mean: float
    std: float


class RollingZScore:
    """Streaming rolling z-score detector with O(1) updates."""

    def __init__(self, window: int = 60, threshold: float = 4.0, min_samples: int = 10):
        if window < 2:
            raise ValueError("window must be >= 2")
        if threshold <= 0:
            raise ValueError("threshold must be > 0")
        self.window = window
        self.threshold = threshold
        self.min_samples = min(min_samples, window)
        self._buf: deque[float] = deque(maxlen=window)
        self._sum = 0.0
        self._sumsq = 0.0

    def _stats(self) -> tuple[float, float]:
        n = len(self._buf)
        mean = self._sum / n
        var = max(self._sumsq / n - mean * mean, 0.0)
        return mean, sqrt(var)

    def update(self, value: float) -> AnomalyResult:
        """Score `value` against history, then add it to the window."""
        if len(self._buf) < self.min_samples:
            self._push(value)
            return AnomalyResult(False, 0.0, value, 0.0)

        mean, std = self._stats()
        if std < 1e-12:
            z = 0.0 if abs(value - mean) < 1e-9 else float("inf")
        else:
            z = (value - mean) / std
        result = AnomalyResult(abs(z) > self.threshold, z, mean, std)
        self._push(value)
        return result

    def _push(self, value: float) -> None:
        if len(self._buf) == self._buf.maxlen:
            old = self._buf[0]
            self._sum -= old
            self._sumsq -= old * old
        self._buf.append(value)
        self._sum += value
        self._sumsq += value * value

    @property
    def n(self) -> int:
        return len(self._buf)

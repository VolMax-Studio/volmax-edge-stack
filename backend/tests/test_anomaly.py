import pytest

from app.anomaly import RollingZScore


def test_warmup_no_anomaly():
    d = RollingZScore(window=20, threshold=3, min_samples=5)
    for _ in range(4):
        assert d.update(100.0).is_anomaly is False


def test_stable_signal_no_anomaly():
    d = RollingZScore(window=20, threshold=3, min_samples=5)
    for _ in range(50):
        assert d.update(10.0).is_anomaly is False


def test_spike_detected():
    d = RollingZScore(window=30, threshold=4, min_samples=10)
    for i in range(30):
        d.update(10.0 + 0.01 * (i % 3))
    assert d.update(25.0).is_anomaly is True


def test_constant_then_step_detected():
    d = RollingZScore(window=20, threshold=3, min_samples=5)
    for _ in range(20):
        d.update(5.0)
    res = d.update(5.5)
    assert res.is_anomaly is True  # std ~ 0, any deviation is infinite z


def test_zscore_sign():
    d = RollingZScore(window=20, threshold=10, min_samples=5)
    for i in range(20):
        d.update(10.0 + 0.5 * ((-1) ** i))
    assert d.update(0.0).zscore < 0
    assert d.update(30.0).zscore > 0


def test_window_eviction():
    d = RollingZScore(window=5, threshold=3, min_samples=2)
    for v in [1, 2, 3, 4, 5, 6, 7]:
        d.update(float(v))
    assert d.n == 5


def test_invalid_params():
    with pytest.raises(ValueError):
        RollingZScore(window=1)
    with pytest.raises(ValueError):
        RollingZScore(threshold=0)

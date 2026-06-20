#include <iostream>
#include <cassert>
#include <cmath>
#include "Preferences.h"
#include "../lib/dsp/DriftDetector.h"

void test_basic_welford() {
    std::cout << "Running test_basic_welford..." << std::endl;
    DriftDetector d(3.0, 5); // threshold=3.0, min_samples=5

    // Learn mode
    double z1 = d.processValue(10.0, true);
    assert(z1 == -1.0); // Not locked/ready yet
    assert(d.getSampleCount() == 1);
    assert(d.getMean() == 10.0);

    d.processValue(12.0, true);
    d.processValue(11.0, true);
    d.processValue(9.0, true);
    d.processValue(8.0, true); // Total 5 samples

    assert(d.getSampleCount() == 5);
    assert(d.getMean() == 10.0);
    // Values: 10, 12, 11, 9, 8. Mean = 10. 
    // Variance = (0^2 + 2^2 + 1^2 + (-1)^2 + (-2)^2) / 4 = (0 + 4 + 1 + 1 + 4)/4 = 10/4 = 2.5
    // StdDev = sqrt(2.5) = 1.58113883
    assert(std::abs(d.getStdDev() - 1.58113883) < 1e-5);

    // Try to lock baseline
    assert(!d.getLockedStatus());
    d.lockBaseline();
    assert(d.getLockedStatus());

    // Monitor mode, active Z-score check
    double z_normal = d.processValue(10.0, false);
    assert(z_normal == 0.0); // Mean is 10.0, so Z-score is 0

    double z_drift = d.processValue(13.162277, false); // 2 standard deviations away (10 + 2 * 1.58113883)
    assert(std::abs(z_drift - 2.0) < 1e-4);

    std::cout << "test_basic_welford passed!" << std::endl;
}

void test_readiness_gate() {
    std::cout << "Running test_readiness_gate..." << std::endl;
    DriftDetector d(3.0, 10); // min_samples = 10

    // Only collect 5 samples
    for (int i = 0; i < 5; i++) {
        d.processValue(5.0, true);
    }
    assert(d.getSampleCount() == 5);

    // Try to lock before min_samples is reached
    d.lockBaseline();
    assert(!d.getLockedStatus()); // Should not lock because n < 10

    // Should still return -1.0
    double z = d.processValue(5.0, false);
    assert(z == -1.0);

    // Collect another 5 samples
    for (int i = 0; i < 5; i++) {
        d.processValue(5.0, true);
    }
    assert(d.getSampleCount() == 10);

    // Lock now
    d.lockBaseline();
    assert(d.getLockedStatus()); // Successfully locked!

    double z2 = d.processValue(5.0, false);
    assert(z2 == 0.0); // Now it returns a valid Z-score

    std::cout << "test_readiness_gate passed!" << std::endl;
}

void test_binned_drift_detector() {
    std::cout << "Running test_binned_drift_detector..." << std::endl;
    BinnedDriftDetector b(3.0, 5);

    // Default bin edges: 1.0, 5.0, 10.0, 20.0
    // Active irms = 0.5 (Bin 0: < 1.0)
    // Active irms = 3.0 (Bin 1: 1.0 <= irms < 5.0)
    // Active irms = 8.0 (Bin 2: 5.0 <= irms < 10.0)
    
    // irms < 1.0 should return -1.0 (empty/low load bypass)
    double z_low = b.process(0.5, 5.0, true);
    assert(z_low == -1.0);

    // Feed Bin 1 (irms = 3.0) 5 samples
    b.process(3.0, 10.0, true);
    b.process(3.0, 12.0, true);
    b.process(3.0, 11.0, true);
    b.process(3.0, 9.0, true);
    b.process(3.0, 8.0, true);

    // Feed Bin 2 (irms = 8.0) only 2 samples
    b.process(8.0, 20.0, true);
    b.process(8.0, 22.0, true);

    // Lock all bins
    b.lockAll();

    // Bin 1 should be locked
    assert(b.getBinLockedStatus(1));
    // Bin 2 should NOT be locked (only 2 samples, min is 5)
    assert(!b.getBinLockedStatus(2));

    // Monitor Bin 1 -> valid Z-score
    double z_bin1 = b.process(3.0, 10.0, false);
    assert(z_bin1 == 0.0);

    // Monitor Bin 2 -> -1.0 (readiness gate prevents false alarms)
    double z_bin2 = b.process(8.0, 20.0, false);
    assert(z_bin2 == -1.0);

    // Test changing bin edges
    double new_edges[4] = {2.0, 4.0, 6.0, 8.0};
    b.setBinEdges(new_edges);

    // All bins should be reset and unlocked
    assert(!b.getBinLockedStatus(1));
    assert(b.getBinSampleCount(1) == 0);

    std::cout << "test_binned_drift_detector passed!" << std::endl;
}

int main() {
    std::cout << "=== Running Firmware Unit Tests ===" << std::endl;
    test_basic_welford();
    test_readiness_gate();
    test_binned_drift_detector();
    std::cout << "=== All Firmware Unit Tests Passed! ===" << std::endl;
    return 0;
}

#ifndef DRIFT_DETECTOR_H
#define DRIFT_DETECTOR_H

#include <math.h>
#include <stdint.h>
#include <Preferences.h>

/**
 * DriftDetector — rekurzivni Welfordov algoritam za računanje srednje vrednosti
 * i varijanse u realnom vremenu (double preciznost). Uvodi freeze-after-lock 
 * mehanizam, sanitizaciju ulaznih podataka (NaN/Inf filter) i minimalni limit uzoraka.
 */
class DriftDetector {
private:
    double n;
    double mean;
    double M2;
    double std_dev;
    bool isLocked;
    double z_threshold;
    uint32_t min_samples;

public:
    DriftDetector(double threshold = 3.0, uint32_t min_s = 50) {
        n = 0;
        mean = 0.0;
        M2 = 0.0;
        std_dev = 0.0;
        isLocked = false;
        z_threshold = threshold;
        min_samples = min_s;
    }

    void reset() {
        n = 0;
        mean = 0.0;
        M2 = 0.0;
        std_dev = 0.0;
        isLocked = false;
    }

    void lockBaseline() {
        if (n >= min_samples) {
            isLocked = true;
        }
    }

    bool isReady() const {
        return isLocked || (n >= min_samples);
    }

    /**
     * Obrađuje novi uzorak feature-a (npr. THD% ili Crest Factor).
     * x: ulazna vrednost.
     * update_baseline: true ako smo u LEARN fazi pa ažuriramo statistiku.
     * Vraća: Trenutni izračunati Z-score (0.0 ako još nema dovoljno uzoraka).
     */
    double processValue(double x, bool update_baseline) {
        // Sanitizacija ulaza: ignoriši NaN, Inf i apsurdne vrednosti (van opsega 0..1000%)
        if (isnan(x) || isinf(x) || x < 0.0 || x > 1000.0) {
            return 0.0;
        }

        if (update_baseline && !isLocked) {
            n++;
            double delta = x - mean;
            mean += delta / n;
            double delta2 = x - mean;
            M2 += delta * delta2;

            if (n > 1) {
                double variance = M2 / (n - 1); // Sample variance
                std_dev = sqrt(variance);
            }
        } 
        
        // Z-score kalkulacija
        if (std_dev > 0.0001) {
            return fabs(x - mean) / std_dev;
        }

        return 0.0;
    }

    double getMean() const { return mean; }
    double getStdDev() const { return std_dev; }
    uint32_t getSampleCount() const { return (uint32_t)n; }
    bool getLockedStatus() const { return isLocked; }

    void save(Preferences& prefs, const char* prefix, int bin_idx) {
        char key[16];
        snprintf(key, sizeof(key), "%s_n_%d", prefix, bin_idx);
        prefs.putDouble(key, n);
        snprintf(key, sizeof(key), "%s_m_%d", prefix, bin_idx);
        prefs.putDouble(key, mean);
        snprintf(key, sizeof(key), "%s_m2_%d", prefix, bin_idx);
        prefs.putDouble(key, M2);
        snprintf(key, sizeof(key), "%s_sd_%d", prefix, bin_idx);
        prefs.putDouble(key, std_dev);
        snprintf(key, sizeof(key), "%s_lk_%d", prefix, bin_idx);
        prefs.putBool(key, isLocked);
    }

    void load(Preferences& prefs, const char* prefix, int bin_idx) {
        char key[16];
        snprintf(key, sizeof(key), "%s_n_%d", prefix, bin_idx);
        n = prefs.getDouble(key, 0.0);
        snprintf(key, sizeof(key), "%s_m_%d", prefix, bin_idx);
        mean = prefs.getDouble(key, 0.0);
        snprintf(key, sizeof(key), "%s_m2_%d", prefix, bin_idx);
        M2 = prefs.getDouble(key, 0.0);
        snprintf(key, sizeof(key), "%s_sd_%d", prefix, bin_idx);
        std_dev = prefs.getDouble(key, 0.0);
        snprintf(key, sizeof(key), "%s_lk_%d", prefix, bin_idx);
        isLocked = prefs.getBool(key, false);
    }
};

/**
 * BinnedDriftDetector — eliminiše zavisnost THD-a od opterećenja (load-dependency).
 * Deli radne režime struje (RMS) u 5 diskretnih opsega (bins) i za svaki vodi
 * nezavisnu DriftDetector instancu. Sprečava lažne alarme pri startovanju motora
 * i promenama režima.
 */
class BinnedDriftDetector {
private:
    static constexpr int NUM_BINS = 5;
    DriftDetector bins[NUM_BINS];
    double bin_edges[NUM_BINS - 1]; // Granice strujnih opsega
    double z_threshold;

public:
    BinnedDriftDetector(double threshold = 3.0, uint32_t min_samples = 50) {
        z_threshold = threshold;
        
        // Definišemo granice opsega struje (RMS Amps):
        // Bin 0: < 1.0 A (Idle / Unloaded — ne pratimo anomalije da izbegnemo noise floor)
        // Bin 1: 1.0 A - 5.0 A (Low Load)
        // Bin 2: 5.0 A - 10.0 A (Medium Load)
        // Bin 3: 10.0 A - 20.0 A (High Load)
        // Bin 4: >= 20.0 A (Overload / Full Load)
        bin_edges[0] = 1.0;
        bin_edges[1] = 5.0;
        bin_edges[2] = 10.0;
        bin_edges[3] = 20.0;
        
        for (int i = 0; i < NUM_BINS; i++) {
            bins[i] = DriftDetector(threshold, min_samples);
        }
    }

    int getBinIndex(double irms) const {
        for (int i = 0; i < NUM_BINS - 1; i++) {
            if (irms < bin_edges[i]) {
                return i;
            }
        }
        return NUM_BINS - 1;
    }

    /**
     * Vodi proces detekcije u odnosu na radnu tačku (irms).
     * irms: RMS struja u amperima (trenutna radna tačka).
     * thd: izmereni THD% (ili Crest Factor).
     * update_baseline: true ako sistem uči normalno stanje.
     * Vraća: Z-score za aktivni opseg opterećenja.
     */
    double process(double irms, double thd, bool update_baseline) {
        int bin_idx = getBinIndex(irms);
        
        // Ako je struja u Bin 0 (< 1.0 A), uređaj je ugašen ili radi u praznom hodu.
        // Preskačemo anomalije kako bismo izbegli šum u praznom hodu.
        if (bin_idx == 0) {
            return 0.0;
        }

        return bins[bin_idx].processValue(thd, update_baseline);
    }

    void lockAll() {
        for (int i = 0; i < NUM_BINS; i++) {
            bins[i].lockBaseline();
        }
    }

    void resetAll() {
        for (int i = 0; i < NUM_BINS; i++) {
            bins[i].reset();
        }
    }

    void save(Preferences& prefs) {
        for (int i = 0; i < NUM_BINS; i++) {
            bins[i].save(prefs, "drift", i);
        }
    }

    void load(Preferences& prefs) {
        for (int i = 0; i < NUM_BINS; i++) {
            bins[i].load(prefs, "drift", i);
        }
    }

    double getBinMean(int idx) const { 
        return (idx >= 0 && idx < NUM_BINS) ? bins[idx].getMean() : 0.0; 
    }
    double getBinStdDev(int idx) const { 
        return (idx >= 0 && idx < NUM_BINS) ? bins[idx].getStdDev() : 0.0; 
    }
    uint32_t getBinSampleCount(int idx) const { 
        return (idx >= 0 && idx < NUM_BINS) ? bins[idx].getSampleCount() : 0; 
    }
    bool getBinLockedStatus(int idx) const { 
        return (idx >= 0 && idx < NUM_BINS) ? bins[idx].getLockedStatus() : false; 
    }
    double getZThreshold() const { return z_threshold; }
};

#endif // DRIFT_DETECTOR_H

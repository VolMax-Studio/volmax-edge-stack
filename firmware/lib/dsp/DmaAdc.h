#pragma once

#include <Arduino.h>
#include <math.h>
#include <driver/adc.h>
#include <esp_adc/adc_continuous.h>

// ESP-DSP zaglavlja
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include "dsps_math.h"

class DmaAdc {
private:
    adc_continuous_handle_t handle = nullptr;
    bool is_initialized = false;
    
    // Veličina prozora za FFT (mora biti stepen dvojke, npr. 1024)
    uint32_t fft_size;
    uint32_t sample_rate;
    
    // ESP-DSP koeficijenti i baferi
    float* hann_window = nullptr;
    float* input_buffer = nullptr;
    float* fft_buffer = nullptr; // Kompleksni bafer (2 * fft_size)

public:
    DmaAdc(uint32_t fft_s = 1024, uint32_t sample_r = 3200) 
        : fft_size(fft_s), sample_rate(sample_r) {
        // Alokacija memorije za FFT bafere
        hann_window = new float[fft_size];
        input_buffer = new float[fft_size];
        fft_buffer = new float[fft_size * 2]; // real, imag parovi
    }

    ~DmaAdc() {
        deinit();
        delete[] hann_window;
        delete[] input_buffer;
        delete[] fft_buffer;
    }

    /**
     * Inicijalizuje kontinuirani ADC drajver sa DMA.
     * pin: GPIO pin na kom je povezan CT senzor (npr. GPIO 5 -> ADC1 Channel 4 na ESP32-S3)
     */
    bool init(int pin) {
        if (is_initialized) return true;

        adc_unit_t unit;
        adc_channel_t channel;
        
        // Mapiranje GPIO pina na ADC jedinicu i kanal
        esp_err_t err = adc_continuous_io_to_channel(pin, &unit, &channel);
        if (err != ESP_OK) {
            Serial.printf("Greška: GPIO %d nije validan ADC pin!\n", pin);
            return false;
        }

        // 1. Konfiguracija DMA handles i skladišnog bafera
        adc_continuous_handle_cfg_t handle_config = {};
        handle_config.max_store_buf_size = fft_size * 4; // Bafer za skladištenje
        handle_config.conv_frame_size = fft_size * 2;   // Veličina jednog frejma konverzije

        err = adc_continuous_new_handle(&handle_config, &handle);
        if (err != ESP_OK) {
            Serial.println("Inicijalizacija DMA ADC ručke nije uspela!");
            return false;
        }

        // 2. Podešavanje parametara kontinuirane konverzije
        adc_continuous_config_t config = {};
        config.pattern_num = 1;
        
        // Konfiguracija pojedinačnog kanala
        adc_digi_pattern_config_t pattern = {};
        pattern.atten = ADC_ATTEN_DB_12; // 0-3.3V opseg za ESP32-S3
        pattern.channel = channel;
        pattern.unit = unit;
        pattern.bit_width = ADC_BITWIDTH_12; // 12-bitna rezolucija

        config.adc_pattern = &pattern;
        config.sample_freq_hz = sample_rate;
        config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
        config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;

        err = adc_continuous_config(handle, &config);
        if (err != ESP_OK) {
            Serial.println("Konfiguracija kontinuiranog ADC-a nije uspela!");
            adc_continuous_del_handle(handle);
            handle = nullptr;
            return false;
        }

        // 3. Pokretanje ADC-a
        err = adc_continuous_start(handle);
        if (err != ESP_OK) {
            Serial.println("Pokretanje kontinuiranog ADC-a nije uspelo!");
            adc_continuous_del_handle(handle);
            handle = nullptr;
            return false;
        }

        // 4. Pre-kalkulacija Hann prozora pomoću ESP-DSP-a
        dsps_wind_hann_f32(hann_window, fft_size);

        // 5. Inicijalizacija FFT tabela za Radix-2
        err = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
        if (err != ESP_OK) {
            Serial.println("Inicijalizacija ESP-DSP FFT-a nije uspela!");
            return false;
        }

        is_initialized = true;
        Serial.println("DMA ADC i ESP-DSP uspešno inicijalizovani na ESP32-S3.");
        return true;
    }

    void deinit() {
        if (handle) {
            adc_continuous_stop(handle);
            adc_continuous_del_handle(handle);
            handle = nullptr;
        }
        is_initialized = false;
    }

    /**
     * Čita sirove podatke iz DMA bafera, vrši Hann prozoriranje, FFT i izdvaja RMS i THD.
     * irms_out: izlaz za proračunatu RMS struju
     * thd_out: izlaz za proračunati THD%
     * ct_a_per_v: kalibracioni koeficijent strujnih klešta
     * cal_gain: hardverski gain offset
     */
    bool acquire_and_process(float &irms_out, float &thd_out, float ct_a_per_v, float cal_gain) {
        if (!is_initialized) return false;

        uint32_t bytes_to_read = fft_size * sizeof(adc_digi_output_data_t);
        uint8_t* raw_buffer = (uint8_t*)malloc(bytes_to_read);
        if (!raw_buffer) return false;

        uint32_t out_length = 0;
        // Čitamo direktno iz DMA skladišta (blokirajući poziv dok se bafer ne napuni)
        esp_err_t err = adc_continuous_read(handle, raw_buffer, bytes_to_read, &out_length, 1000 / portTICK_PERIOD_MS);
        if (err != ESP_OK || out_length < bytes_to_read) {
            free(raw_buffer);
            return false;
        }

        adc_digi_output_data_t* data = (adc_digi_output_data_t*)raw_buffer;
        
        // 1. Ekstrakcija 12-bitnih vrednosti i uklanjanje DC offseta
        double mean_val = 0;
        for (uint32_t i = 0; i < fft_size; i++) {
            input_buffer[i] = (float)data[i].type1.data;
            mean_val += input_buffer[i];
        }
        mean_val /= fft_size;
        for (uint32_t i = 0; i < fft_size; i++) {
            input_buffer[i] -= (float)mean_val;
        }

        free(raw_buffer);

        // 2. Proračun VRMS i IRMS
        double sq_sum = 0;
        for (uint32_t i = 0; i < fft_size; i++) {
            sq_sum += (double)input_buffer[i] * input_buffer[i];
        }
        float vrms = sqrt(sq_sum / fft_size) * (3.3f / 4095.0f); // Pretvaranje u napon
        irms_out = vrms * ct_a_per_v * cal_gain;

        // Maskiranje šuma u praznom hodu
        if (irms_out < 0.5f) {
            thd_out = 0.0f;
            return true;
        }

        // 3. Primena Hann prozora nad signalom
        for (uint32_t i = 0; i < fft_size; i++) {
            // Re realni deo, Im imaginarni deo (za FFT ulaz)
            fft_buffer[i * 2] = input_buffer[i] * hann_window[i];
            fft_buffer[i * 2 + 1] = 0.0f; // Imaginarni deo je 0
        }

        // 4. Izvršavanje Radix-2 FFT-a pomoću optimizovanog ESP-DSP-a
        // Koristi Xtensa LX7 SIMD instrukcije za maksimalne performanse na ESP32-S3
        dsps_fft2r_fc32(fft_buffer, fft_size);
        // Bit-reversal sortiranje rezultata
        dsps_bit_rev_fc32(fft_buffer, fft_size);

        // 5. Proračun harmonika i THD%
        // Spektralna rezolucija df = fs / N = 3200 / 1024 = 3.125 Hz.
        // Fundamentalna frekvencija (50 Hz) pada na bin = 50 / 3.125 = 16.
        int fundamental_bin = 16;
        
        // Amplituda fundamentalnog harmonika
        float h1_real = fft_buffer[fundamental_bin * 2];
        float h1_imag = fft_buffer[fundamental_bin * 2 + 1];
        float h1 = sqrt(h1_real * h1_real + h1_imag * h1_imag);

        if (h1 < 1e-4f) {
            thd_out = 0.0f;
            return true;
        }

        // Sumiramo energije viših harmonika (2. do 8. harmonika)
        // tj. frekvencije 100Hz, 150Hz, 200Hz, 250Hz, 300Hz, 350Hz, 400Hz.
        double higher_harmonics_sum_sq = 0;
        for (int h = 2; h <= 8; h++) {
            int bin = fundamental_bin * h;
            if (bin < (int)fft_size / 2) {
                float h_real = fft_buffer[bin * 2];
                float h_imag = fft_buffer[bin * 2 + 1];
                float amp = sqrt(h_real * h_real + h_imag * h_imag);
                higher_harmonics_sum_sq += (double)amp * amp;
            }
        }

        // THD-F formula
        thd_out = (float)(sqrt(higher_harmonics_sum_sq) / h1 * 100.0);
        return true;
    }
};

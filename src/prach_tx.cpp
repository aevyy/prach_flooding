// prach_tx.cpp — PRACH preamble generator and RF transmitter
//
// Uses srsran_rf_t (srsRAN's RF abstraction) instead of raw UHD to avoid
// Boost.DateTime compatibility issues with newer GCC.
//
// Key design decisions:
//
// 1. FREQUENCY CORRECTNESS
//    RF TX center frequency = ul_freq_hz (1747.5 MHz for n3).
//    srsran_prach_gen() places the preamble at freq_offset PRBs *within*
//    the UL carrier; this is a baseband shift only — NOT added to HW freq.
//
// 2. TIMING
//    srsran_rf_send_timed: schedules burst with end_of_burst flag.
//    The gNB PRACH detector only correlates within the configured RACH occasion.
//    The usable timing-error budget ≈ CP − round-trip ≈ 103 us for format 0
//    (co-located → ~full CP). The device-time anchor must therefore be accurate
//    to well within one OFDM symbol (~71 us at 15 kHz SCS).
//
// 3. RA-RNTI LOGGING
//    Compute per TS 38.321 §5.1.3 and log for gNB correlation.

#include "prach_tx.h"
#include "ra_rnti.h"
extern "C" {
#include "srsran/phy/rf/rf.h"
#include "srsran/phy/common/phy_common_nr.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <thread>
#include <algorithm>

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
prach_tx::~prach_tx() {
    if (m_initialized) {
        srsran_prach_free(&m_prach);
    }
    if (m_rf_open) {
        srsran_rf_close(&m_rf);
    }
}

// ---------------------------------------------------------------------------
// init — configure from cell_config
// ---------------------------------------------------------------------------
bool prach_tx::init(const cell_config& cfg, const tool_config& tc, bool dry_run) {
    m_dry_run     = dry_run;
    m_cell_cfg    = cfg;
    m_cfo_correct = tc.cfo.correct;
    m_cfo_sign    = tc.cfo.sign;

    // Apply msg1-FrequencyStart override if set
    uint32_t freq_offset_raw = cfg.prach_freq_offset;
    if (tc.freq.msg1_freq_start_override >= 0) {
        freq_offset_raw = (uint32_t)tc.freq.msg1_freq_start_override;
        printf("[prach_tx] freq_offset overridden: %u → %u (from yaml/CLI)\n",
               cfg.prach_freq_offset, freq_offset_raw);
    }

    m_cfg.tx_freq_hz  = cfg.ul_freq_hz;
    m_cfg.srate_hz    = cfg.srate_hz;
    m_cfg.rapid       = tc.tx.preamble_index;
    m_cfg.nof_prb     = cfg.nof_prb;
    m_cfg.config_idx  = cfg.prach_config_idx;
    m_cfg.root_seq    = cfg.prach_root_seq_idx;
    m_cfg.zcz         = cfg.prach_zcz;
    m_cfg.freq_offset = freq_offset_raw;
    m_cfg.tx_gain_db  = tc.tx.gain_db;
    m_cfg.dry_run     = dry_run;
    m_cfg.cfo_correct = tc.cfo.correct;
    m_cfg.cfo_sign    = tc.cfo.sign;
    m_ssb_first_symbol_override = tc.timing.ssb_first_symbol_override;
    m_tx_offset_us    = tc.timing.tx_offset_us;

    // Initialize srsRAN PRACH object
    uint32_t max_N_ifft_ul = srsran_min_symbol_sz_rb(cfg.nof_prb);
    printf("[prach_tx] max_N_ifft_ul = %u\n", max_N_ifft_ul);

    // Validate sample rate matches FFT size × SCS
    // If these disagree, the DAC plays the preamble at the wrong rate,
    // stretching it in time and scaling the ZC sequence in frequency.
    uint32_t scs_hz = SRSRAN_SUBC_SPACING_NR(cfg.scs);
    double expected_srate = (double)max_N_ifft_ul * scs_hz;

    if (std::abs(cfg.srate_hz - expected_srate) > 1.0) {
        fprintf(stderr, "[prach_tx] FATAL: Sample rate mismatch\n");
        fprintf(stderr, "  srate_hz from config = %.0f Hz\n", cfg.srate_hz);
        fprintf(stderr, "  expected (FFT×SCS)   = %u × %u = %.0f Hz\n",
                max_N_ifft_ul, scs_hz, expected_srate);
        return false;
    }

    printf("[prach_tx] Sample rate validated: %.2f MHz = %u × %u Hz (FFT × SCS)\n",
           cfg.srate_hz / 1e6, max_N_ifft_ul, scs_hz);

    if (srsran_prach_init(&m_prach, max_N_ifft_ul) != SRSRAN_SUCCESS) {
        fprintf(stderr, "[prach_tx] FATAL: srsran_prach_init failed\n");
        return false;
    }

    srsran_prach_cfg_t prach_cfg = {};
    prach_cfg.is_nr              = true;
    prach_cfg.config_idx         = cfg.prach_config_idx;
    prach_cfg.root_seq_idx       = cfg.prach_root_seq_idx;
    prach_cfg.zero_corr_zone     = cfg.prach_zcz;
    prach_cfg.freq_offset        = cfg.prach_freq_offset;
    prach_cfg.num_ra_preambles   = cfg.num_ra_preambles;
    prach_cfg.hs_flag            = false;
    prach_cfg.enable_successive_cancellation = false;
    prach_cfg.enable_freq_domain_offset_calc = false;

    if (srsran_prach_set_cfg(&m_prach, &prach_cfg, cfg.nof_prb) != SRSRAN_SUCCESS) {
        fprintf(stderr, "[prach_tx] FATAL: srsran_prach_set_cfg failed\n");
        srsran_prach_free(&m_prach);
        return false;
    }

    m_initialized = true;

    printf("[prach_tx] Initialized:\n");
    printf("  config_idx   = %u\n", m_cfg.config_idx);
    printf("  root_seq     = %u\n", m_cfg.root_seq);
    printf("  zcz          = %u\n", m_cfg.zcz);
    printf("  freq_offset  = %u PRBs (baseband shift — NOT added to HW freq)\n", m_cfg.freq_offset);
    printf("  tx_freq_hz   = %.3f MHz (UL center, set once)\n", m_cfg.tx_freq_hz / 1e6);
    printf("  srate_hz     = %.2f MHz\n", m_cfg.srate_hz / 1e6);
    printf("  tx_gain_db   = %.1f dB\n", m_cfg.tx_gain_db);
    printf("  dry_run      = %s\n", m_dry_run ? "YES" : "NO");

    return generate_preamble();
}

// ---------------------------------------------------------------------------
// generate_preamble — pre-compute the TX buffer
// ---------------------------------------------------------------------------
bool prach_tx::generate_preamble() {
    if (!m_initialized) return false;

    // Read actual preamble length: N_cp + N_seq samples
    // This is what srsran_prach_gen actually writes.
    // The old code allocated slot_samples, causing:
    //   1. RMS normalization error (averaging over zero tail)
    //   2. Heap overrun for longer formats (N_cp+N_seq > slot_samples)
    uint32_t preamble_len = m_prach.N_cp + m_prach.N_seq;

    printf("[prach_tx] PRACH format params: N_cp=%u, N_seq=%u, total=%u samples\n",
           m_prach.N_cp, m_prach.N_seq, preamble_len);

    m_tx_buf.assign(preamble_len, {0.0f, 0.0f});

    cf_t* raw_buf = reinterpret_cast<cf_t*>(m_tx_buf.data());

    // srsran_prach_gen() internally derives N_rb_ul from the FFT size via
    // srsran_nof_prb(N_ifft_ul), which returns 100 for a 1536-pt FFT even
    // though the actual cell has 106 PRB.  This shifts the PRACH centre
    // frequency by (106-100)/2 * 12 * 1.25 kHz = 540 kHz — enough for
    // the gNB's detector (which uses the true nof_prb) to miss the preamble.
    //
    // Compensate by reducing the passed freq_offset so the resulting k_0
    // lands at the correct physical frequency despite the smaller N_rb_ul.
    uint32_t freq_offset_corrected = m_cfg.freq_offset;
    {
        int delta_prb = (int)m_cell_cfg.nof_prb -
                        (int)srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb));
        // k_0 = freq*12 - N_rb*6 + N_fft/2  →  freq must change by ΔN_rb/2
        freq_offset_corrected = (uint32_t)((int)m_cfg.freq_offset - delta_prb / 2);
        printf("[prach_tx] freq_offset: raw=%u  srsran_Nrb=%u  actual_Nrb=%u  "
               "corrected=%u\n",
               m_cfg.freq_offset,
               srsran_nof_prb(srsran_symbol_sz(m_cell_cfg.nof_prb)),
               m_cell_cfg.nof_prb, freq_offset_corrected);
    }

    // Generate preamble: seq_index=RAPID=0, freq_offset as configured
    if (srsran_prach_gen(&m_prach, m_cfg.rapid, freq_offset_corrected, raw_buf) != SRSRAN_SUCCESS) {
        fprintf(stderr, "[prach_tx] FATAL: srsran_prach_gen failed\n");
        return false;
    }

    m_preamble_len_samples = preamble_len;

    // Apply CFO correction (pre-rotate TX buffer before normalization)
    if (m_cfo_correct && m_cfo_ul_hz != 0.0f) {
        double w = -2.0 * M_PI * (double)m_cfo_sign * (double)m_cfo_ul_hz / m_cfg.srate_hz;
        for (uint32_t n = 0; n < m_preamble_len_samples; n++) {
            float ph = (float)(w * (double)n);
            m_tx_buf[n] *= std::complex<float>(std::cosf(ph), std::sinf(ph));
        }
        printf("[prach_tx] CFO correction applied: %.1f Hz UL (sign=%+d, w=%.6e rad/sample)\n",
               m_cfo_ul_hz, m_cfo_sign, w);
    }

    // Compute signal RMS power over the actual preamble (no zero tail)
    float power = 0.0f;
    for (uint32_t i = 0; i < preamble_len; i++) {
        power += std::norm(m_tx_buf[i]);
    }
    power /= preamble_len;

    printf("[prach_tx] Preamble generated: RAPID=%u, len=%u samples, RMS=%.4f\n",
           m_cfg.rapid, m_preamble_len_samples, std::sqrt(power));

    if (power < 1e-12f) {
        fprintf(stderr, "[prach_tx] WARNING: Preamble has near-zero power — check config\n");
    }

    // Normalize to 0.7 target amplitude
    float target_amp = 0.7f;
    float scale      = target_amp / (std::sqrt(power) + 1e-12f);
    for (auto& s : m_tx_buf) s *= scale;

    printf("[prach_tx] Amplitude normalized: scale=%.3f (target=%.2f)\n", scale, target_amp);

    return true;
}

// ---------------------------------------------------------------------------
// open_rf
// ---------------------------------------------------------------------------
bool prach_tx::open_rf(const std::string& device_args) {
    if (m_dry_run) {
        printf("[prach_tx] dry_run=true — skipping RF device open\n");
        m_rf_open = true;
        return true;
    }

    memset(&m_rf, 0, sizeof(m_rf));

    printf("[prach_tx] Opening RF device (args='%s')...\n", device_args.c_str());

    srsran_rf_load_plugins();

    char args_cstr[256];
    snprintf(args_cstr, sizeof(args_cstr), "%s", device_args.c_str());

    if (srsran_rf_open(&m_rf, args_cstr) != SRSRAN_SUCCESS) {
        fprintf(stderr, "[prach_tx] FATAL: srsran_rf_open failed\n");
        return false;
    }

    // Set RX gain FIRST (order matters for B210 RX chain init)
    // NOTE: RX is configured even though we don't use it for streaming.
    // This is required because the B210's TX path depends on RX PLL lock.
    double rx_gain_db = 40.0;
    srsran_rf_set_rx_gain(&m_rf, rx_gain_db);
    double actual_rx_gain = srsran_rf_get_rx_gain(&m_rf);
    printf("[prach_tx] RX gain: requested=%.1f dB, actual=%.1f dB (configured for PLL lock)\n",
           rx_gain_db, actual_rx_gain);

    // Set RX sample rate (needed for RX chain PLL)
    double actual_rx_rate = srsran_rf_set_rx_srate(&m_rf, m_cfg.srate_hz);
    printf("[prach_tx] RX rate: requested=%.2f MHz, actual=%.2f MHz\n",
           m_cfg.srate_hz / 1e6, actual_rx_rate / 1e6);

    // Set TX sample rate
    double actual_tx_rate = srsran_rf_set_tx_srate(&m_rf, m_cfg.srate_hz);
    printf("[prach_tx] TX rate: requested=%.2f MHz, actual=%.2f MHz\n",
           m_cfg.srate_hz / 1e6, actual_tx_rate / 1e6);

    if (std::abs(actual_tx_rate - m_cfg.srate_hz) / m_cfg.srate_hz > 0.01) {
        fprintf(stderr, "[prach_tx] WARNING: TX sample rate deviation > 1%%\n");
    }

    // Set RX center frequency — DL freq for SSB sync
    double actual_rx_freq = srsran_rf_set_rx_freq(&m_rf, 0, m_cell_cfg.dl_freq_hz);
    printf("[prach_tx] RX freq: requested=%.3f MHz, actual=%.3f MHz\n",
           m_cell_cfg.dl_freq_hz / 1e6, actual_rx_freq / 1e6);

    if (std::abs(actual_rx_freq - m_cell_cfg.dl_freq_hz) > 100e3) {
        fprintf(stderr, "[prach_tx] FATAL: RX freq %.3f MHz deviates from DL %.3f MHz by > 100 kHz\n",
                actual_rx_freq / 1e6, m_cell_cfg.dl_freq_hz / 1e6);
        srsran_rf_close(&m_rf);
        return false;
    }

    // Set TX center frequency — UL freq for PRACH
    double actual_tx_freq = srsran_rf_set_tx_freq(&m_rf, 0, m_cfg.tx_freq_hz);
    printf("[prach_tx] TX freq: requested=%.3f MHz, actual=%.3f MHz\n",
           m_cfg.tx_freq_hz / 1e6, actual_tx_freq / 1e6);

    if (std::abs(actual_tx_freq - m_cfg.tx_freq_hz) > 100e3) {
        fprintf(stderr, "[prach_tx] FATAL: TX freq %.3f MHz deviates from UL %.3f MHz by > 100 kHz\n",
                actual_tx_freq / 1e6, m_cfg.tx_freq_hz / 1e6);
        srsran_rf_close(&m_rf);
        return false;
    }

    // Set TX gain (after all freq/srate are configured)
    if (srsran_rf_set_tx_gain(&m_rf, m_cfg.tx_gain_db) != SRSRAN_SUCCESS) {
        fprintf(stderr, "[prach_tx] WARNING: srsran_rf_set_tx_gain returned error\n");
    }
    double actual_tx_gain = srsran_rf_get_tx_gain(&m_rf);
    printf("[prach_tx] TX gain: requested=%.1f dB, actual=%.1f dB\n",
           m_cfg.tx_gain_db, actual_tx_gain);

    // Settling delay
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    printf("[prach_tx] RF device ready\n");
    m_rf_open = true;
    return true;
}

// ---------------------------------------------------------------------------
// sync_to_ssb — synchronize to SSB to get device-time ↔ SFN/slot mapping
// ---------------------------------------------------------------------------
bool prach_tx::sync_to_ssb() {
    if (m_dry_run) {
        printf("[prach_tx] DRY RUN: skipping SSB sync\n");
        // Use fake timing for dry run
        m_synced = true;
        m_sync_device_time = 0.0;
        m_sync_sfn = 0;
        m_sync_slot = 0;
        m_sync_t_offset = 0;
        return true;
    }

    if (!m_rf_open) {
        fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
        return false;
    }

    // Initialize SSB sync
    if (!m_ssb_sync.init(m_cell_cfg, m_cfg.srate_hz)) {
        fprintf(stderr, "[prach_tx] WARNING: ssb_sync init failed\n");
        if (m_has_fallback && attempt_sync_from_fallback()) {
            return true;
        }
        fprintf(stderr, "[prach_tx] FATAL: ssb_sync init failed and no fallback\n");
        return false;
    }

    // Allocate RX buffer: capture enough samples for SSB search
    uint32_t samples_per_frame = m_cell_cfg.samples_per_frame();
    m_rx_buf.assign(samples_per_frame, {0.0f, 0.0f});

    printf("[prach_tx] Searching for SSB (PCI=%u)...\n", m_cell_cfg.pci);
    printf("[prach_tx] Capturing %.1f ms of samples\n",
           (double)samples_per_frame / m_cfg.srate_hz * 1000.0);

    // Start RX stream
    int ret = srsran_rf_start_rx_stream(&m_rf, false);
    if (ret != 0) {
        fprintf(stderr, "[prach_tx] WARNING: srsran_rf_start_rx_stream returned %d\n", ret);
        if (m_has_fallback && attempt_sync_from_fallback()) {
            return true;
        }
        fprintf(stderr, "[prach_tx] FATAL: Cannot start RX stream and no fallback\n");
        return false;
    }

    // Stabilization delay (B210 needs time to settle after stream start)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Flush stale samples (~10 ms worth)
    std::vector<std::complex<float>> flush_buf(m_cell_cfg.samples_per_slot());
    for (int i = 0; i < 10; i++) {
        srsran_rf_recv(&m_rf, flush_buf.data(), flush_buf.size(), true);
    }

    // Capture one frame of samples in chunks to avoid srsRAN's
    // RX trial limit (RF_UHD_IMP_MAX_RX_TRIALS=100, chunk_size~2048,
    // so max per call ≈ 204800 samples).
    // FIRST chunk uses recv_with_time to get the device timestamp
    // pinned to the sample boundary — no post-capture jitter.
    cf_t*    rx_data     = reinterpret_cast<cf_t*>(m_rx_buf.data());
    uint32_t chunk_size  = m_cell_cfg.samples_per_slot(); // ~21504
    uint32_t remaining   = samples_per_frame;
    uint32_t total_recv  = 0;
    cf_t*    rx_ptr      = rx_data;
    time_t   first_full_secs = 0;
    double   first_frac_secs = 0.0;
    double   rx_device_time = 0.0;
    bool     have_timestamp = false;

    while (remaining > 0) {
        uint32_t to_recv = (remaining > chunk_size) ? chunk_size : remaining;
        int n;
        if (!have_timestamp) {
            // First chunk: capture WITH device timestamp pinned to sample 0
            n = srsran_rf_recv_with_time(&m_rf, rx_ptr, to_recv,
                                          true, &first_full_secs, &first_frac_secs);
            if (n > 0) {
                rx_device_time = (double)first_full_secs + first_frac_secs;
                have_timestamp = true;
                printf("[prach_tx] First chunk timestamp: %.6f s\n", rx_device_time);
            }
        } else {
            n = srsran_rf_recv(&m_rf, rx_ptr, to_recv, true);
        }
        if (n < 0) {
            fprintf(stderr, "[prach_tx] WARNING: RX chunk failed (nrecv=%d, "
                    "requested=%u)\n", n, to_recv);
            break;
        }
        total_recv += (uint32_t)n;
        rx_ptr     += (uint32_t)n;
        remaining  -= (uint32_t)n;
    }

    if (total_recv < samples_per_frame) {
        if (total_recv > 0) {
            printf("[prach_tx] Partial capture: %u/%u samples, "
                   "using what we have\n", total_recv, samples_per_frame);
        } else {
            fprintf(stderr, "[prach_tx] WARNING: RX failed (0 samples)\n");
        }
    }

    // Stop RX stream
    srsran_rf_stop_rx_stream(&m_rf);

    // If we got no samples at all, try fallback
    if (total_recv == 0) {
        if (m_has_fallback) {
            printf("[prach_tx] Attempting sniffer-based SSB sync fallback...\n");
            if (attempt_sync_from_fallback()) {
                return true;
            }
        }
        fprintf(stderr, "[prach_tx] FATAL: SSB sync failed (no RF RX and no fallback)\n");
        return false;
    }

    // Search for SSB using the captured samples
    uint32_t pci = 0, sfn = 0, ssb_idx = 0, t_offset = 0;
    float snr_db = 0.0f, cfo_hz = 0.0f;
    srsran_mib_nr_t mib = {};
    bool hrf = false;

    if (!m_ssb_sync.search(rx_data, total_recv, &pci, &sfn, &ssb_idx,
                          &t_offset, &snr_db, &mib, &hrf, &cfo_hz)) {
        fprintf(stderr, "[prach_tx] WARNING: SSB not found or PBCH decode failed\n");

        // Try sniffer-based fallback
        if (m_has_fallback) {
            printf("[prach_tx] Attempting sniffer-based SSB sync fallback...\n");
            if (attempt_sync_from_fallback()) {
                return true;
            }
        }

        return false;
    }

    // Verify PCI matches
    if (pci != m_cell_cfg.pci && m_cell_cfg.pci != 0) {
        fprintf(stderr, "[prach_tx] WARNING: Detected PCI %u != expected %u\n",
                pci, m_cell_cfg.pci);
    }

    // Compute the device time when the SSB started.
    // rx_device_time is the timestamp of the FIRST captured sample
    // (captured with srsran_rf_recv_with_time on the first chunk).
    // t_offset is the sample offset of the SSB within the buffer.
    double ssb_start_device_time = rx_device_time +
        ((double)t_offset / m_cfg.srate_hz);

    // Compute the SSB slot from ssb_idx and half-frame flag.
    // Pattern A, 15 kHz SCS, Lmax=4:
    //   hrf=0: ssb_idx 0,1 → slot 0;  ssb_idx 2,3 → slot 1
    //   hrf=1: ssb_idx 0,1 → slot 5;  ssb_idx 2,3 → slot 6
    uint32_t half_slot = (ssb_idx < 2) ? 0 : 1;
    uint32_t hf_offset = hrf ? 5 : 0;  // 5 slots per half-frame at 15 kHz
    uint32_t ssb_slot = hf_offset + half_slot;

    // SSB PSS is at symbol 2 or 8 within its slot (Pattern A, 15 kHz).
    // TS 38.213 §4.1 Case A (15 kHz, carrier <=3 GHz): candidate SSB first
    // symbols are {2, 8, 16, 22}. Within-slot first symbol is 2 (idx 0,2) or
    // 8 (idx 1,3). ssb_slot already accounts for the slot; this is intra-slot.
    // ssb_start_device_time marks the PSS, not the slot boundary.
    // Subtract the intra-slot offset so the anchor aligns to slot start.
    uint32_t ssb_first_symbol = ((ssb_idx == 0) || (ssb_idx == 2)) ? 2 : 8;
    if (m_ssb_first_symbol_override >= 0) {
        ssb_first_symbol = (uint32_t)m_ssb_first_symbol_override;
        printf("[prach_tx] SSB first symbol OVERRIDE: %u\n", ssb_first_symbol);
    }
    double   ssb_intra_slot_s = 1e-3 * (double)ssb_first_symbol / 14.0;

    m_sync_device_time = ssb_start_device_time - ssb_intra_slot_s;
    m_sync_sfn = sfn;
    m_sync_slot = ssb_slot;
    m_sync_t_offset = t_offset;
    m_synced = true;
    m_sync_ssb_idx = ssb_idx;

    // Store CFO and scale DL→UL (LO ppm error is constant, so Hz offset scales with carrier)
    m_cfo_dl_hz = cfo_hz;
    m_cfo_ul_hz = m_cfo_dl_hz * (m_cfg.tx_freq_hz / m_cell_cfg.dl_freq_hz);

    printf("[prach_tx] SSB sync acquired:\n");
    printf("  PCI         = %u\n", pci);
    printf("  SFN (4 LSB) = %u\n", sfn);
    printf("  SSB index   = %u\n", ssb_idx);
    printf("  SNR         = %.1f dB\n", snr_db);
    printf("  CFO (DL)    = %.1f Hz\n", m_cfo_dl_hz);
    printf("  CFO (UL)    = %.1f Hz (scaled by %.3f)\n",
           m_cfo_ul_hz, m_cfg.tx_freq_hz / m_cell_cfg.dl_freq_hz);
    printf("  PSS dev_time= %.6f s  (slot-start = %.6f s, -%.0f us)\n",
           ssb_start_device_time, m_sync_device_time, ssb_intra_slot_s * 1e6);
    printf("  Slot        = %u (from ssb_idx=%u, hrf=%d)\n", ssb_slot, ssb_idx, hrf);

    return true;
}

// ---------------------------------------------------------------------------
// set_sync_fallback — store sniffer-based timing for fallback use
// ---------------------------------------------------------------------------
void prach_tx::set_sync_fallback(double unix_time, uint32_t sfn, uint32_t ssb_slot, uint32_t ssb_idx) {
    m_has_fallback       = true;
    m_fallback_unix_time = unix_time;
    m_fallback_sfn       = sfn;
    m_fallback_ssb_slot  = ssb_slot;
    m_fallback_ssb_idx   = ssb_idx;
    printf("[prach_tx] Sync fallback registered: SFN=%u slot=%u ssb_idx=%u @ unix=%.3f\n",
           sfn, ssb_slot, ssb_idx, unix_time);
}

// ---------------------------------------------------------------------------
// attempt_sync_from_fallback — use sniffer InfluxDB MIB + device-time epoch offset
//
// The B210 device time is NOT Unix time. We calibrate the offset:
//   unix_time = device_time + m_dev_epoch_offset
// Then compute the SFN-to-device-time mapping from the sniffer's MIB.
// TX uses srsran_rf_send_timed_multi for hardware-accurate timing.
// ---------------------------------------------------------------------------
bool prach_tx::attempt_sync_from_fallback() {
    if (!m_has_fallback) return false;

    // Record current device time and wall-clock time simultaneously
    time_t full_secs = 0;
    double frac_secs = 0.0;
    srsran_rf_get_time(&m_rf, &full_secs, &frac_secs);
    double dev_now = (double)full_secs + frac_secs;

    auto now = std::chrono::system_clock::now();
    auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    double unix_now = (double)now_us / 1e6;

    // Calibrate: unix = dev + offset
    m_dev_epoch_offset = unix_now - dev_now;

    // Convert the sniffer's MIB Unix timestamp to device time
    double dev_at_mib = m_fallback_unix_time - m_dev_epoch_offset;

    // SSB PSS intra-slot offset (Pattern A, 15 kHz, as in sync_to_ssb)
    // TS 38.213 §4.1 Case A (15 kHz, carrier <=3 GHz): candidate SSB first
    // symbols are {2, 8, 16, 22}. Within-slot first symbol is 2 (idx 0,2) or
    // 8 (idx 1,3). ssb_slot already accounts for the slot; this is intra-slot.
    uint32_t ssb_first_symbol =
        ((m_fallback_ssb_idx == 0) || (m_fallback_ssb_idx == 2)) ? 2 : 8;
    if (m_ssb_first_symbol_override >= 0) {
        ssb_first_symbol = (uint32_t)m_ssb_first_symbol_override;
    }
    double ssb_intra_slot_s = 1e-3 * (double)ssb_first_symbol / 14.0;

    // Set sync anchor in device-time coordinates (aligned to slot start)
    m_sync_device_time = dev_at_mib - ssb_intra_slot_s;
    m_sync_sfn         = m_fallback_sfn;   // 4-LSB SFN
    m_sync_slot        = m_fallback_ssb_slot;
    m_sync_t_offset    = 0;
    m_synced           = true;
    m_using_fallback   = false;  // use normal device-time TX path now
    m_sync_ssb_idx     = m_fallback_ssb_idx;

    printf("[prach_tx] Fallback sync (device-time calibrated):\n");
    printf("  Epoch offset   = %.3f s (unix = dev + offset)\n", m_dev_epoch_offset);
    printf("  dev_now        = %.6f s\n", dev_now);
    printf("  dev_at_mib     = %.6f s  (slot-start = %.6f s, -%.0f us)\n",
           dev_at_mib, m_sync_device_time, ssb_intra_slot_s * 1e6);
    printf("  anchor: SFN=%u slot=%u ssb_idx=%u\n",
           m_sync_sfn, m_sync_slot, m_sync_ssb_idx);

    return true;
}

// ---------------------------------------------------------------------------
// transmit_preamble
// ---------------------------------------------------------------------------
bool prach_tx::transmit_preamble(uint32_t sfn, uint32_t ro_slot) {
    if (!m_initialized) {
        fprintf(stderr, "[prach_tx] FATAL: not initialized\n");
        return false;
    }

    // Compute RA-RNTI per TS 38.321 §5.1.3
    m_ra_rnti = compute_ra_rnti(0, ro_slot, 0, 0);

    printf("\n[prach_tx] ==================== PRACH TX ====================\n");
    printf("[prach_tx] RAPID         = %u\n", m_cfg.rapid);
    printf("[prach_tx] Target RO     = SFN %u, slot %u\n", sfn, ro_slot);
    printf("[prach_tx] RA-RNTI       = 0x%04x (%u)\n", m_ra_rnti, m_ra_rnti);
    printf("[prach_tx]   = 1 + s_id(0) + 14*t_id(%u) + 14*80*f_id(0) + 14*80*8*ul_carrier(0)\n", ro_slot);
    printf("[prach_tx] TX freq       = %.3f MHz\n", m_cfg.tx_freq_hz / 1e6);
    printf("[prach_tx] TX gain       = %.1f dB\n", m_cfg.tx_gain_db);
    printf("[prach_tx] Preamble len  = %u samples\n", m_preamble_len_samples);
    printf("[prach_tx] =======================================================\n\n");

    if (m_dry_run) {
        printf("[prach_tx] DRY RUN: skipping actual RF transmission\n");
        return true;
    }

    if (!m_rf_open) {
        fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
        return false;
    }

    // Auto-sync to SSB for device-time ↔ SFN/slot mapping
    if (!m_synced) {
        printf("[prach_tx] No SSB sync — acquiring reference timing...\n");
        if (!sync_to_ssb()) {
            fprintf(stderr, "[prach_tx] FATAL: SSB sync failed, cannot align TX\n");
            return false;
        }
    }

    double t_now;
    time_t full_secs_now = 0;
    double frac_secs_now = 0.0;

    // Compute the target TX device time using the sync anchor.
    uint32_t sync_rfn = m_sync_sfn & 0xF;
    uint32_t target_rfn = sfn & 0xF;

    int32_t frames_to_ro;
    if (sync_rfn < target_rfn) {
        frames_to_ro = (int32_t)(target_rfn - sync_rfn);
    } else if (sync_rfn == target_rfn) {
        if ((int32_t)m_sync_slot <= (int32_t)ro_slot) {
            frames_to_ro = 0;
        } else {
            frames_to_ro = 16;
        }
    } else {
        frames_to_ro = (int32_t)(16 - sync_rfn + target_rfn);
    }

    // Frame/slot durations for 15 kHz SCS:
    //   1 slot  = 1 ms
    //   1 frame = 10 ms
    // Use these constants rather than samples_per_slot()/srate which
    // misses CP overhead and gives ~0.933 ms instead of 1 ms.
    const double slot_dur  = 1e-3;    // 1 ms
    const double frame_dur = 10e-3;   // 10 ms
    const double ro_period_dur = 16.0 * frame_dur; // 160 ms

    srsran_rf_get_time(&m_rf, &full_secs_now, &frac_secs_now);
    t_now = (double)full_secs_now + frac_secs_now;

    int64_t slots_from_sync = (int64_t)frames_to_ro * 10 +
                              (int64_t)ro_slot - (int64_t)m_sync_slot;
    double t_tx = m_sync_device_time + (double)slots_from_sync * slot_dur;

    // Manual timing nudge (sweep parameter from tool_config)
    t_tx += m_tx_offset_us * 1e-6;

    // Advance if target is in the past (one RO period = 16 frames = 160ms)
    if (t_tx < t_now - 0.001) {
        double periods_needed = std::ceil((t_now - t_tx) / ro_period_dur);
        t_tx += periods_needed * ro_period_dur;
    }

    if (t_tx <= t_now) {
        fprintf(stderr, "[prach_tx] FATAL: target time %.6f s is in the past (now=%.6f s)\n",
                t_tx, t_now);
        return false;
    }

    printf("[prach_tx] Sync anchor: SFN=%u (4LSB) slot=%u @ dev_time=%.6f s\n",
           m_sync_sfn, m_sync_slot, m_sync_device_time);
    printf("[prach_tx] Target RO:    SFN=%u slot=%u → dev_time=%.6f s (Δ=%+.3f ms)\n",
           sfn, ro_slot, t_tx, (t_tx - t_now) * 1e3);

    void* buffers[1];
    buffers[0] = reinterpret_cast<void*>(m_tx_buf.data());

    int nsent = srsran_rf_send_timed_multi(
        &m_rf,
        buffers,
        m_preamble_len_samples,
        (time_t)t_tx,
        t_tx - (time_t)t_tx,
        true,
        true,
        true
    );

    if (nsent < 0) {
        fprintf(stderr, "[prach_tx] FATAL: srsran_rf_send_timed_multi failed (ret=%d)\n", nsent);
        return false;
    }

    if ((uint32_t)nsent != m_preamble_len_samples) {
        fprintf(stderr, "[prach_tx] WARNING: sent %d/%u samples\n",
                nsent, m_preamble_len_samples);
    }

    printf("[prach_tx] Preamble transmitted: %d samples\n", nsent);
    printf("[prach_tx] Expected gNB response:\n");
    printf("  RAR (Msg2) on RA-RNTI=0x%04x (%u) within 10 slots of RO end\n",
           m_ra_rnti, m_ra_rnti);
    printf("  Check: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra.rnti'\n");

    m_last_tx_time = t_tx;
    return nsent > 0;
}

// ---------------------------------------------------------------------------
// transmit_at_time — transmit preamble at a specific device time
// (for continuous mode: each subsequent RO is 160ms after the previous)
// ---------------------------------------------------------------------------
bool prach_tx::transmit_at_time(double dev_time, uint32_t sfn, uint32_t ro_slot) {
    if (!m_initialized) {
        fprintf(stderr, "[prach_tx] FATAL: not initialized\n");
        return false;
    }

    m_ra_rnti = compute_ra_rnti(0, ro_slot, 0, 0);

    printf("[prach_tx] ==================== PRACH TX ====================\n");
    printf("[prach_tx] RAPID         = %u\n", m_cfg.rapid);
    printf("[prach_tx] Target RO     = SFN %u, slot %u\n", sfn, ro_slot);
    printf("[prach_tx] RA-RNTI       = 0x%04x (%u)\n", m_ra_rnti, m_ra_rnti);
    printf("[prach_tx] TX freq       = %.3f MHz\n", m_cfg.tx_freq_hz / 1e6);
    printf("[prach_tx] TX gain       = %.1f dB\n", m_cfg.tx_gain_db);
    printf("[prach_tx] Preamble len  = %u samples\n", m_preamble_len_samples);
    printf("[prach_tx] =======================================================\n\n");

    if (m_dry_run) {
        printf("[prach_tx] DRY RUN: skipping actual RF transmission\n");
        return true;
    }
    if (!m_rf_open) {
        fprintf(stderr, "[prach_tx] FATAL: RF device not opened\n");
        return false;
    }

    time_t full_secs_now = 0;
    double frac_secs_now = 0.0;
    srsran_rf_get_time(&m_rf, &full_secs_now, &frac_secs_now);
    double t_now = (double)full_secs_now + frac_secs_now;

    // Manual timing nudge
    dev_time += m_tx_offset_us * 1e-6;

    if (dev_time <= t_now) {
        fprintf(stderr, "[prach_tx] FATAL: target time %.6f s is in the past (now=%.6f)\n",
                dev_time, t_now);
        return false;
    }

    printf("[prach_tx] Target RO: SFN=%u slot=%u → dev_time=%.6f s (Δ=%+.3f ms)\n",
           sfn, ro_slot, dev_time, (dev_time - t_now) * 1e3);

    void* buffers[1];
    buffers[0] = reinterpret_cast<void*>(m_tx_buf.data());

    int nsent = srsran_rf_send_timed_multi(
        &m_rf, buffers, m_preamble_len_samples,
        (time_t)dev_time, dev_time - (time_t)dev_time,
        true, true, true
    );

    if (nsent < 0) {
        fprintf(stderr, "[prach_tx] FATAL: srsran_rf_send_timed_multi failed (ret=%d)\n", nsent);
        return false;
    }
    if ((uint32_t)nsent != m_preamble_len_samples) {
        fprintf(stderr, "[prach_tx] WARNING: sent %d/%u samples\n",
                nsent, m_preamble_len_samples);
    }

    printf("[prach_tx] Preamble transmitted: %d samples\n", nsent);
    printf("[prach_tx] Expected gNB response:\n");
    printf("  RAR (Msg2) on RA-RNTI=0x%04x (%u) within 10 slots of RO end\n",
           m_ra_rnti, m_ra_rnti);
    printf("  Check: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra.rnti'\n");

    m_last_tx_time = dev_time;
    return nsent > 0;
}

// dummy main for link test
int prach_tx_link_check() { return 0; }

#pragma once
// prach_tx.h — PRACH preamble generator and RF transmitter for Msg1 (Phase 1)
//
// Uses srsran_rf_t (srsRAN's RF abstraction) to avoid Boost.DateTime GCC
// compatibility issues from raw UHD headers.
//
// Design invariant: TX center freq = ul_freq_hz (set ONCE via srsran_rf_set_tx_freq).
//                  freq_offset is a baseband shift inside srsran_prach_gen,
//                  NOT an additional hardware frequency offset.

#include "cell_config.h"
#include "ssb_sync.h"
#include "tool_config.h"
extern "C" {
#include "srsran/phy/phch/prach.h"
#include "srsran/phy/rf/rf.h"
}
#include <cstdint>
#include <vector>
#include <complex>
#include <string>

struct prach_tx_cfg {
    double   tx_gain_db  = 30.0;
    double   tx_freq_hz  = 1747.5e6; // TX center freq (UL); from cell_config
    double   srate_hz    = 23.04e6;
    uint32_t rapid       = 0;        // preamble index (0 for single-preamble cell)
    uint32_t nof_prb     = 106;
    uint32_t config_idx  = 1;
    uint32_t root_seq    = 1;
    uint32_t zcz         = 0;
    uint32_t freq_offset = 8;        // msg1-FrequencyStart in PRBs
    bool     dry_run     = false;
    bool     cfo_correct = true;     // enable CFO correction
    int      cfo_sign    = +1;       // CFO correction sign (+1 or -1)
};

class prach_tx {
public:
    prach_tx()  = default;
    ~prach_tx();

    // Initialize from cell_config + tool_config (dry_run is in tool_config.run).
    bool init(const cell_config& cfg, const tool_config& tc, bool dry_run);

    // Open RF device (device_args = "" or "type=b200" for B210)
    bool open_rf(const std::string& device_args = "type=b200");

    // Synchronize to SSB to establish device-time ↔ SFN/slot mapping.
    // Returns true on successful sync.
    // Stores the mapping internally: m_sync_device_time, m_sync_sfn, m_sync_slot, m_sync_t_offset
    bool sync_to_ssb();

    // Set fallback timing from the sniffer's InfluxDB MIB data.
    // Used when direct RF RX-based SSB sync is not possible (e.g. device busy).
    // unix_time: UTC timestamp when SFN was observed by the sniffer
    // sfn: SFN (4 LSBs) at that time, ssb_slot: slot index of the detected SSB
    // ssb_idx: SSB index (0-7), needed to compute intra-slot symbol offset
    void set_sync_fallback(double unix_time, uint32_t sfn, uint32_t ssb_slot, uint32_t ssb_idx);

    // Generate + transmit ONE preamble at the given PRACH occasion (sfn, slot).
    // Uses the SSB sync timing to schedule TX at the correct device time.
    // Returns true if transmission succeeded.
    bool transmit_preamble(uint32_t sfn, uint32_t ro_slot);

    // Transmit preamble at an exact device time (for continuous mode,
    // where each subsequent RO is 160ms after the previous).
    bool transmit_at_time(double dev_time, uint32_t sfn, uint32_t ro_slot);

    // Diagnostics
    uint32_t get_preamble_len_samples() const { return m_preamble_len_samples; }
    uint16_t get_ra_rnti()             const { return m_ra_rnti; }
    bool     get_synced()              const { return m_synced; }
    uint32_t get_sync_sfn()            const { return m_sync_sfn; }
    uint32_t get_sync_slot()           const { return m_sync_slot; }
    double   get_last_tx_time()        const { return m_last_tx_time; }

    // Flood mode accessors
    bool        is_flood_enabled()        const { return m_flood_enabled; }
    const std::vector<uint32_t>& get_flood_indices() const { return m_flood_indices; }
    uint32_t    get_flood_num_preambles() const { return m_flood_num_preambles; }
    uint32_t    get_flood_tx_count()      const { return m_flood_tx_count; }

    // Multi-RO accessors
    // Returns all RA-RNTIs that will be allocated by the gNB for this burst.
    // In single-position mode: one entry (f_id=0).
    // In multi-position + sweep_fid mode: one entry per freq position (f_id=0..K-1).
    std::vector<uint16_t> get_multi_ro_ra_rntis(uint32_t ro_slot) const;
    uint32_t get_freq_pos_count() const { return m_multi_freq_pos_count; }

    // Get the effective RAPID(s) for the current TX (for logging)
    // In superimpose mode: returns "0-63" style range
    // In cycle mode: returns the current single index
    // In single mode: returns the configured RAPID
    std::string get_current_rapid_list() const;

private:
    bool           m_initialized = false;
    bool           m_rf_open     = false;
    bool           m_dry_run     = false;
    bool           m_synced      = false;  // SSB sync acquired

    prach_tx_cfg   m_cfg;
    cell_config    m_cell_cfg;  // Store full cell config for sync
    srsran_prach_t m_prach = {};
    srsran_rf_t    m_rf    = {};
    uint32_t       m_preamble_len_samples = 0;
    uint16_t       m_ra_rnti = 0;

    // SSB sync state
    ssb_sync       m_ssb_sync;
    double         m_sync_device_time = 0.0;  // Device time when SSB was detected
    uint32_t       m_sync_sfn         = 0;    // SFN of detected SSB
    uint32_t       m_sync_slot        = 0;    // Slot of detected SSB
    uint32_t       m_sync_t_offset    = 0;    // Sample offset of SSB within buffer
    uint32_t       m_sync_ssb_idx     = 0;    // SSB index (0-7), for intra-slot offset

    // Sniffer-based fallback timing (used when direct RF RX is unavailable)
    bool           m_has_fallback     = false;
    bool           m_using_fallback   = false;  // true if sync was done via fallback
    double         m_fallback_unix_time = 0.0;
    uint32_t       m_fallback_sfn     = 0;
    uint32_t       m_fallback_ssb_slot = 0;
    uint32_t       m_fallback_ssb_idx = 0;

    // Epoch offset: unix_time = device_time + m_dev_epoch_offset
    // Calibrated at sync time so we can convert between device time and Unix time.
    double         m_dev_epoch_offset = 0.0;

    // TX/RX timestamp offset calibration (B210-specific)
    // Positive value means TX arrives this many seconds AFTER the requested time
    double         m_tx_rx_offset_s   = 0.0;
    double         m_last_tx_time     = 0.0;  // device time of last TX

    // CFO correction (measured from DL SSB, scaled to UL)
    float          m_cfo_dl_hz        = 0.0f;  // DL CFO from SSB
    float          m_cfo_ul_hz        = 0.0f;  // UL CFO (scaled by freq ratio)
    bool           m_cfo_correct      = true;  // enable CFO correction
    int            m_cfo_sign         = +1;    // CFO correction sign
    int32_t        m_ssb_first_symbol_override = -1;  // >=0 forces intra-slot symbol
    double         m_tx_offset_us     = 0.0;   // manual timing nudge

    // Single-preamble TX buffer (original path)
    std::vector<std::complex<float>> m_tx_buf;
    std::vector<std::complex<float>> m_rx_buf;  // For SSB sync

    // --- Flood mode state ---
    bool           m_flood_enabled        = false;
    uint32_t       m_flood_num_preambles  = 64; // Count of elements in m_flood_indices
    std::vector<uint32_t> m_flood_indices;      // List of actual indices to spoof
    std::string    m_flood_strategy       = "superimpose";
    float          m_flood_power_backoff_db = 0.0f;
    uint32_t       m_flood_slm_candidates   = 32;
    uint32_t       m_flood_tx_count       = 0;  // monotonic counter for cycle mode

    // Per-preamble buffers: m_flood_bufs[i] = baseband for RAPID=i
    std::vector<std::vector<std::complex<float>>> m_flood_bufs;
    // Superimposed TX buffer (complex sum of all m_flood_bufs)
    std::vector<std::complex<float>> m_flood_tx_buf;
    // SLM/Newman best phase vector for superposition
    std::vector<float> m_flood_phases;

    // --- Multi-frequency-position attack state (#1 + #2) ---
    // m_multi_freq_pos_count: number of freq-domain positions to superimpose.
    uint32_t       m_multi_freq_pos_count = 1;
    // Superimposed buffer combining multiple frequency shifts
    std::vector<std::complex<float>> m_multi_freq_tx_buf;
    std::vector<uint32_t> m_freq_offsets_used;   // per position

    bool generate_preamble();
    bool generate_flood_preambles();
    // Generate multi-frequency-position superimposed buffer.
    // Called after generate_flood_preambles() when freq_pos_count > 1.
    // For single-preamble mode (flood disabled), also called when freq_pos_count > 1.
    bool generate_multi_freq_preambles();
    bool attempt_sync_from_fallback();

    // Helpers
    // Compute corrected freq_offset for a given raw offset (accounts for srsran PRB mismatch).
    uint32_t correct_freq_offset(uint32_t raw_offset) const;
    // Generate and superimpose all RAPIDs into one buffer at the given corrected freq_offset.
    // result must be pre-allocated to m_prach.N_cp + m_prach.N_seq samples.
    bool superimpose_preambles_at_offset(uint32_t corrected_offset,
                                         std::vector<std::complex<float>>& result,
                                         float pos_target_amplitude);

    // Get the TX buffer pointer and length for the current mode
    void* get_tx_buffer();
};

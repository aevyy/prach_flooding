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
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>

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

// --- Preamble ID (spec-correct mapping) ---
struct PreambleId {
    int logical_root;
    int v;      // cyclic shift index within root
    int Cv;     // Cv = v * N_CS (unrestricted set)
};

// --- SSB anchor for timing tracker ---
struct SsbAnchor {
    double  ssb_device_time   = 0.0;   // uhd device time of SSB symbol 0 (seconds)
    int     sfn               = 0;
    int     half_frame        = 0;
    double  clock_drift_ppm   = 0.0;   // estimated USRP-vs-gNB drift
    double  cfo_hz            = 0.0;   // carrier freq offset from SSB
    double  last_update_time  = 0.0;   // device time of last anchor update
};

// --- Per-occasion telemetry record ---
struct TxOccasionRecord {
    double wall_clock;
    int    gnb_sfn, gnb_subframe;
    double target_tx_device_time, actual_tx_device_time;
    double delta_t_us;
    double est_cfo_hz, est_clock_drift_ppm, time_since_last_resync_ms;
    int    n_preambles_tx;
    double tx_gain, composite_papr_db;
    int    n_underflow, n_overflow, n_late, n_seq_error;
};

class prach_tx {
public:
    prach_tx()  = default;
    ~prach_tx();

    // Initialize from cell_config + tool_config (dry_run is in tool_config.run).
    bool init(const cell_config& cfg, const tool_config& tc, bool dry_run);

    // Open RF device (device_args = "" or "type=b200" for B210)
    bool open_rf(const std::string& device_args = "type=b200");

    // Synchronize to SSB to establish device-time <-> SFN/slot mapping.
    bool sync_to_ssb();

    // Set fallback timing from the sniffer's InfluxDB MIB data.
    void set_sync_fallback(double unix_time, uint32_t sfn, uint32_t ssb_slot, uint32_t ssb_idx);

    // Generate + transmit ONE preamble at the given PRACH occasion (sfn, slot).
    bool transmit_preamble(uint32_t sfn, uint32_t ro_slot);

    // Transmit preamble at an exact device time (for continuous mode).
    bool transmit_at_time(double dev_time, uint32_t sfn, uint32_t ro_slot);

    // --- New: SSB-slaved timing ---
    // Update SSB anchor with a new detection (called periodically).
    void update_ssb_anchor(double ssb_device_time, int sfn, int hrf,
                           double cfo_hz, double device_time_now);

    // Compute the absolute USRP device time for a given PRACH occasion.
    double compute_occasion_device_time(uint32_t sfn, uint32_t ro_slot) const;

    // Compute timing error vs the current anchor; returns seconds.
    double compute_timing_error(double target_device_time) const;

    // Regenerate TX buffers with current CFO (call after sync / anchor update).
    bool regenerate_tx_buffers();

    // --- Telemetry ---
    const TxOccasionRecord& get_last_occasion_record() const { return m_last_occasion; }
    const std::deque<SsbAnchor>& get_anchor_history() const { return m_anchor_history; }

    // --- UHD async counters ---
    int get_async_underflow() const  { return m_async_underflow.load(); }
    int get_async_overflow() const   { return m_async_overflow.load(); }
    int get_async_late() const       { return m_async_late.load(); }
    int get_async_seq_error() const  { return m_async_seq_error.load(); }

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
    std::string get_flood_strategy()      const { return m_flood_strategy; }
    const std::vector<PreambleId>& get_preamble_list() const { return m_preamble_list; }

    // Multi-RO accessors
    std::vector<uint16_t> get_multi_ro_ra_rntis(uint32_t ro_slot) const;
    uint32_t get_freq_pos_count() const { return m_multi_freq_pos_count; }

    std::string get_current_rapid_list() const;

    // --- UHD async error counters (public for rf_error_callback access) ---
    std::atomic<int> m_async_underflow{0};
    std::atomic<int> m_async_overflow{0};
    std::atomic<int> m_async_late{0};
    std::atomic<int> m_async_seq_error{0};

private:
    bool           m_initialized = false;
    bool           m_rf_open     = false;
    bool           m_dry_run     = false;
    bool           m_synced      = false;

    prach_tx_cfg   m_cfg;
    cell_config    m_cell_cfg;
    srsran_prach_t m_prach = {};
    srsran_rf_t    m_rf    = {};
    uint32_t       m_preamble_len_samples = 0;
    uint16_t       m_ra_rnti = 0;

    // SSB sync state
    ssb_sync       m_ssb_sync;
    double         m_sync_device_time = 0.0;
    uint32_t       m_sync_sfn         = 0;
    uint32_t       m_sync_slot        = 0;
    uint32_t       m_sync_t_offset    = 0;
    uint32_t       m_sync_ssb_idx     = 0;

    // Sniffer-based fallback timing
    bool           m_has_fallback     = false;
    bool           m_using_fallback   = false;
    double         m_fallback_unix_time = 0.0;
    uint32_t       m_fallback_sfn     = 0;
    uint32_t       m_fallback_ssb_slot = 0;
    uint32_t       m_fallback_ssb_idx = 0;

    double         m_dev_epoch_offset = 0.0;

    // TX/RX timestamp offset calibration (B210-specific)
    double         m_tx_rx_offset_s   = 0.0;
    double         m_last_tx_time     = 0.0;

    // CFO correction
    float          m_cfo_dl_hz        = 0.0f;
    float          m_cfo_ul_hz        = 0.0f;
    bool           m_cfo_correct      = true;
    int            m_cfo_sign         = +1;
    int32_t        m_ssb_first_symbol_override = -1;
    double         m_tx_offset_us     = 0.0;

    // Single-preamble TX buffer
    std::vector<std::complex<float>> m_tx_buf;
    std::vector<std::complex<float>> m_rx_buf;

    // --- Flood mode state ---
    bool           m_flood_enabled        = false;
    uint32_t       m_flood_num_preambles  = 64;
    std::vector<uint32_t> m_flood_indices;
    std::string    m_flood_strategy       = "superimpose";
    float          m_flood_power_backoff_db = 0.0f;
    uint32_t       m_flood_slm_candidates   = 32;
    uint32_t       m_flood_tx_count       = 0;

    // Per-preamble buffers: m_flood_bufs[i] = baseband for RAPID=i
    std::vector<std::vector<std::complex<float>>> m_flood_bufs;
    // Superimposed TX buffer (complex sum of all m_flood_bufs)
    std::vector<std::complex<float>> m_flood_tx_buf;
    // SLM/Newman best phase vector for superposition
    std::vector<float> m_flood_phases;

    // --- Multi-frequency-position attack state ---
    uint32_t       m_multi_freq_pos_count = 1;
    bool           m_multi_sweep_fid = false;
    std::vector<std::complex<float>> m_multi_freq_tx_buf;
    std::vector<uint32_t> m_freq_offsets_used;

    // --- A1: Spec-correct preamble list ---
    std::vector<PreambleId> m_preamble_list;
    uint32_t       m_preamble_occasion_counter = 0;  // round-robin index
    uint32_t       m_max_preambles_per_occasion = 16;
    float          m_papr_backoff_db = 3.0f;
    double         m_composite_papr_db = 0.0;       // per-occasion PAPR
    std::vector<float> m_last_slm_phases;            // telemetry: which phases were used

    // --- B1: SSB timing tracker ---
    std::deque<SsbAnchor> m_anchor_history;
    SsbAnchor       m_current_anchor;
    uint32_t        m_ssb_fit_window_m = 8;
    mutable std::mutex m_anchor_mutex;

    // --- B4: Periodic regen ---
    uint32_t        m_regen_period_occasions = 0;

    // --- B5: Watchdog ---
    double          m_resync_cp_fraction = 0.3;
    double          m_max_time_since_sync_ms = 0.0;
    double          m_rx_to_tx_cal_s = 0.0;

    // --- C1/C2: Telemetry ---
    TxOccasionRecord m_last_occasion;
    double          m_last_actual_tx_time = 0.0;

private:
    bool             m_async_thread_running = false;
    std::thread      m_async_thread;

    void start_async_monitor();
    void stop_async_monitor();

    // --- Preamble generation methods ---
    bool generate_preamble_buffer(std::vector<std::complex<float>>& buf,
                                  uint32_t rapid, uint32_t corrected_offset,
                                  bool apply_cfo = true);
    bool generate_preamble();
    bool generate_flood_preambles();
    bool generate_multi_freq_preambles();
    bool generate_occasion_composite(const std::vector<PreambleId>& subset);

    bool attempt_sync_from_fallback();

    // Helpers
    uint32_t correct_freq_offset(uint32_t raw_offset) const;
    bool superimpose_preambles_at_offset(uint32_t corrected_offset,
                                         std::vector<std::complex<float>>& result,
                                         float pos_target_amplitude);

    // --- A1: Preamble list construction ---
    std::vector<PreambleId> build_preamble_list(int total, int per_root,
                                                 int ncs, int root0) const;

    void* get_tx_buffer();
};

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
};

class prach_tx {
public:
    prach_tx()  = default;
    ~prach_tx();

    // Initialize from cell_config. tx_gain_db overrides default.
    bool init(const cell_config& cfg, double tx_gain_db, bool dry_run);

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
    void set_sync_fallback(double unix_time, uint32_t sfn, uint32_t ssb_slot);

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

    // Sniffer-based fallback timing (used when direct RF RX is unavailable)
    bool           m_has_fallback     = false;
    bool           m_using_fallback   = false;  // true if sync was done via fallback
    double         m_fallback_unix_time = 0.0;
    uint32_t       m_fallback_sfn     = 0;
    uint32_t       m_fallback_ssb_slot = 0;

    // Epoch offset: unix_time = device_time + m_dev_epoch_offset
    // Calibrated at sync time so we can convert between device time and Unix time.
    double         m_dev_epoch_offset = 0.0;

    // TX/RX timestamp offset calibration (B210-specific)
    // Positive value means TX arrives this many seconds AFTER the requested time
    double         m_tx_rx_offset_s   = 0.0;
    double         m_last_tx_time     = 0.0;  // device time of last TX

    std::vector<std::complex<float>> m_tx_buf;
    std::vector<std::complex<float>> m_rx_buf;  // For SSB sync

    bool generate_preamble();
    bool attempt_sync_from_fallback();
};

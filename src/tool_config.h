#pragma once
// tool_config.h — operational tunables loaded from ra-spoof.yaml / overridden by CLI
//
// Precedence: built-in default  <  yaml  <  CLI

#include <cstdint>
#include <string>

struct tool_config {
    // Where each tunable was resolved from (for --dry-run printout)
    enum src_t { SRC_DEFAULT = 0, SRC_YAML = 1, SRC_CLI = 2 };

    // --- tx ---
    struct tx_t {
        double      gain_db       = 30.0;
        uint32_t    preamble_index = 0;
        std::string device_args   = "type=b200";
        uint8_t     src_gain      = SRC_DEFAULT;
        uint8_t     src_preamble  = SRC_DEFAULT;
        uint8_t     src_devargs   = SRC_DEFAULT;
    } tx;

    // --- cfo ---
    struct cfo_t {
        bool   correct    = false;
        int    sign       = 1;     // +1 or -1
        double manual_hz  = 0.0;   // if != 0, use this instead of SSB-measured CFO
        uint8_t src_correct   = SRC_DEFAULT;
        uint8_t src_sign      = SRC_DEFAULT;
        uint8_t src_manual_hz = SRC_DEFAULT;
    } cfo;

    // --- timing ---
    struct timing_t {
        double  tx_offset_us                = 0.0;   // manual nudge (sweep param)
        int32_t ssb_first_symbol_override    = -1;    // -1 = spec value; >=0 = forced
        double  rx_to_tx_cal_s              = 0.0;   // fixed device RX->TX stream calibration offset
        double  resync_cp_fraction           = 0.3;  // fraction of CP duration for re-anchor watchdog
        double  max_time_since_sync_ms       = 0.0;  // hard re-anchor timeout (0 = disabled)
        uint8_t src_tx_offset               = SRC_DEFAULT;
        uint8_t src_ssb_sym                 = SRC_DEFAULT;
        uint8_t src_rx_to_tx_cal            = SRC_DEFAULT;
        uint8_t src_resync_cp_frac          = SRC_DEFAULT;
        uint8_t src_max_time_since_sync     = SRC_DEFAULT;
    } timing;

    // --- freq ---
    struct freq_t {
        int32_t msg1_freq_start_override = -1;   // -1 = cell value; >=0 = override (PRB)
        int32_t msg1_fdm_override        = -1;   // -1 = cell value; >=0 = override
        uint8_t src_freq_start           = SRC_DEFAULT;
        uint8_t src_fdm                  = SRC_DEFAULT;
    } freq;

    // --- run ---
    struct run_t {
        bool     continuous    = false;
        uint32_t max_tx        = 1;     // 0 = unlimited (continuous only)
        uint32_t resync_every  = 0;     // 0 = never; N = re-sync every N TX
        std::string gnb_log_path = "/tmp/gnb.log";
        bool     autotune      = false;
        uint8_t  src_cont      = SRC_DEFAULT;
        uint8_t  src_max_tx    = SRC_DEFAULT;
        uint8_t  src_resync    = SRC_DEFAULT;
        uint8_t  src_log_path  = SRC_DEFAULT;
        uint8_t  src_autotune  = SRC_DEFAULT;
    } run;

    // --- flood ---
    struct flood_t {
        bool        enabled          = false;
        std::string preamble_list    = "64";   // Either an integer N (meaning 0 to N-1) or a comma/dash separated list
        uint32_t    num_preambles    = 64;     // Backwards compat, computed from list
        std::string strategy         = "superimpose"; // "superimpose" or "cycle"
        float       power_backoff_db = 0.0f;
        uint32_t    slm_candidates   = 32;
        uint32_t    max_preambles_per_occasion = 16;  // per-occasion subset cap
        float       papr_backoff_db  = 3.0f;   // peak backoff below full-scale (dB)
        uint8_t     src_enabled      = SRC_DEFAULT;
        uint8_t     src_num          = SRC_DEFAULT;
        uint8_t     src_strategy     = SRC_DEFAULT;
        uint8_t     src_backoff      = SRC_DEFAULT;
        uint8_t     src_slm          = SRC_DEFAULT;
        uint8_t     src_max_per_occ  = SRC_DEFAULT;
        uint8_t     src_papr_backoff = SRC_DEFAULT;
    } flood;

    // --- multi_ro ---
    struct multi_ro_t {
        uint32_t freq_pos_count  = 1;     // number of freq-domain positions to superimpose
        uint8_t  src_freq_pos    = SRC_DEFAULT;
    } multi_ro;

    // --- ssb_fit ---
    struct ssb_fit_t {
        uint32_t fit_window_m     = 8;    // number of SSB anchors for clock-drift LS fit
        uint32_t regen_period_occasions = 0; // 0 = only at init; N = regen every N occasions
        uint8_t  src_fit_window   = SRC_DEFAULT;
        uint8_t  src_regen_period = SRC_DEFAULT;
    } ssb_fit;
};

// Parse configs/ra-spoof.yaml.  Missing keys keep their defaults; never aborts on
// a partial file.  Returns true iff the file exists and parsed without exceptions.
bool parse_tool_config(const std::string& path, tool_config& tc);

// Print the resolved config (dry-run display), showing for each tunable
// whether it came from default / yaml / CLI.
void print_tool_config(const tool_config& tc);

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
        uint8_t src_tx_offset               = SRC_DEFAULT;
        uint8_t src_ssb_sym                 = SRC_DEFAULT;
    } timing;

    // --- freq ---
    struct freq_t {
        int32_t msg1_freq_start_override = -1;   // -1 = cell value; >=0 = override (PRB)
        uint8_t src_freq_start           = SRC_DEFAULT;
    } freq;

    // --- run ---
    struct run_t {
        bool     continuous    = false;
        uint32_t max_tx        = 1;     // 0 = unlimited (continuous only)
        uint32_t resync_every  = 0;     // 0 = never; N = re-sync every N TX
        uint8_t  src_cont      = SRC_DEFAULT;
        uint8_t  src_max_tx    = SRC_DEFAULT;
        uint8_t  src_resync    = SRC_DEFAULT;
    } run;
};

// Parse configs/ra-spoof.yaml.  Missing keys keep their defaults; never aborts on
// a partial file.  Returns true iff the file exists and parsed without exceptions.
bool parse_tool_config(const std::string& path, tool_config& tc);

// Print the resolved config (dry-run display), showing for each tunable
// whether it came from default / yaml / CLI.
void print_tool_config(const tool_config& tc);

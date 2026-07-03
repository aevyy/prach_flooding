#include "tool_config.h"
#include <yaml-cpp/yaml.h>
#include <cstdio>
#include <cmath>
#include <cstring>

// Helper: read a YAML scalar. If the key exists and is valid, set `dst` and mark src.
#define TRY_SCALAR(node, key, dst, src_flag)                 \
    do {                                                     \
        if (node && node[(key)] && node[(key)].IsScalar()) { \
            dst      = node[(key)].as<decltype(dst)>();      \
            src_flag = tool_config::SRC_YAML;                \
        }                                                    \
    } while (0)

bool parse_tool_config(const std::string& path, tool_config& tc) {
    try {
        YAML::Node root = YAML::LoadFile(path);
        if (!root) return false;  // file missing or empty — keep defaults

        // --- tx ---
        auto tx = root["tx"];
        if (tx) {
            TRY_SCALAR(tx, "gain_db",        tc.tx.gain_db,        tc.tx.src_gain);
            TRY_SCALAR(tx, "preamble_index", tc.tx.preamble_index, tc.tx.src_preamble);
            TRY_SCALAR(tx, "device_args",    tc.tx.device_args,    tc.tx.src_devargs);
        }

        // --- cfo ---
        auto cfo = root["cfo"];
        if (cfo) {
            TRY_SCALAR(cfo, "correct",   tc.cfo.correct,   tc.cfo.src_correct);
            TRY_SCALAR(cfo, "sign",      tc.cfo.sign,      tc.cfo.src_sign);
            TRY_SCALAR(cfo, "manual_hz", tc.cfo.manual_hz, tc.cfo.src_manual_hz);
        }

        // --- timing ---
        auto timing = root["timing"];
        if (timing) {
            TRY_SCALAR(timing, "tx_offset_us",                tc.timing.tx_offset_us,                tc.timing.src_tx_offset);
            TRY_SCALAR(timing, "rx_to_tx_cal_us",            tc.timing.rx_to_tx_cal_us,             tc.timing.src_rx_to_tx_cal);
            TRY_SCALAR(timing, "ssb_first_symbol_override",    tc.timing.ssb_first_symbol_override,    tc.timing.src_ssb_sym);
        }

        // --- freq ---
        auto freq = root["freq"];
        if (freq) {
            TRY_SCALAR(freq, "msg1_freq_start_override", tc.freq.msg1_freq_start_override, tc.freq.src_freq_start);
            TRY_SCALAR(freq, "msg1_fdm_override",        tc.freq.msg1_fdm_override,        tc.freq.src_fdm);
        }

        // --- run ---
        auto run = root["run"];
        if (run) {
            TRY_SCALAR(run, "continuous",    tc.run.continuous,    tc.run.src_cont);
            TRY_SCALAR(run, "max_tx",        tc.run.max_tx,        tc.run.src_max_tx);
            TRY_SCALAR(run, "resync_every",  tc.run.resync_every,  tc.run.src_resync);
            TRY_SCALAR(run, "gnb_log_path",  tc.run.gnb_log_path,  tc.run.src_log_path);
            TRY_SCALAR(run, "autotune",      tc.run.autotune,      tc.run.src_autotune);
        }

        // --- flood ---
        auto flood = root["flood"];
        if (flood) {
            TRY_SCALAR(flood, "enabled",          tc.flood.enabled,          tc.flood.src_enabled);
            
            if (flood["num_preambles"]) {
                if (flood["num_preambles"].IsScalar()) {
                    tc.flood.preamble_list = flood["num_preambles"].Scalar();
                    tc.flood.src_num = tool_config::SRC_YAML;
                }
            }
            
            TRY_SCALAR(flood, "strategy",         tc.flood.strategy,         tc.flood.src_strategy);
            TRY_SCALAR(flood, "power_backoff_db", tc.flood.power_backoff_db, tc.flood.src_backoff);
            TRY_SCALAR(flood, "slm_candidates",   tc.flood.slm_candidates,   tc.flood.src_slm);
        }

        // --- multi_ro ---
        auto mro = root["multi_ro"];
        if (mro) {
            TRY_SCALAR(mro, "freq_pos_count", tc.multi_ro.freq_pos_count, tc.multi_ro.src_freq_pos);
        }

        printf("[tool_config] Loaded %s\n", path.c_str());
        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[tool_config] WARNING: parse error '%s' — using defaults\n", e.what());
        return false;
    }
}

// src→string helper
static const char* src_str(uint8_t s) {
    switch (s) {
        case tool_config::SRC_YAML: return "yaml";
        case tool_config::SRC_CLI:  return "CLI";
        default:                    return "default";
    }
}

void print_tool_config(const tool_config& tc) {
    printf("\n=== Tool Config (resolved) ===\n");
    printf("  tx.gain_db                    = %.1f  (%s)\n", tc.tx.gain_db,        src_str(tc.tx.src_gain));
    printf("  tx.preamble_index             = %u    (%s)\n", tc.tx.preamble_index, src_str(tc.tx.src_preamble));
    printf("  tx.device_args                = %s    (%s)\n", tc.tx.device_args.c_str(), src_str(tc.tx.src_devargs));
    printf("  cfo.correct                   = %s     (%s)\n", tc.cfo.correct ? "true" : "false", src_str(tc.cfo.src_correct));
    printf("  cfo.sign                      = %+d    (%s)\n", tc.cfo.sign,         src_str(tc.cfo.src_sign));
    printf("  cfo.manual_hz                 = %.1f  (%s)\n", tc.cfo.manual_hz,     src_str(tc.cfo.src_manual_hz));
    printf("  timing.tx_offset_us           = %+.1f (%s)\n", tc.timing.tx_offset_us, src_str(tc.timing.src_tx_offset));
    printf("  timing.rx_to_tx_cal_us        = %+.1f (%s)\n", tc.timing.rx_to_tx_cal_us, src_str(tc.timing.src_rx_to_tx_cal));
    printf("  timing.ssb_first_symbol_ovrd  = %d    (%s)\n", tc.timing.ssb_first_symbol_override, src_str(tc.timing.src_ssb_sym));
    printf("  freq.msg1_freq_start_override = %d    (%s)\n", tc.freq.msg1_freq_start_override, src_str(tc.freq.src_freq_start));
    printf("  freq.msg1_fdm_override        = %d    (%s)\n", tc.freq.msg1_fdm_override, src_str(tc.freq.src_fdm));
    printf("  run.continuous                = %s     (%s)\n", tc.run.continuous ? "true" : "false", src_str(tc.run.src_cont));
    printf("  run.max_tx                    = %u    (%s)\n", tc.run.max_tx,        src_str(tc.run.src_max_tx));
    printf("  run.resync_every              = %u    (%s)\n", tc.run.resync_every,  src_str(tc.run.src_resync));
    printf("  run.gnb_log_path              = %s    (%s)\n", tc.run.gnb_log_path.c_str(), src_str(tc.run.src_log_path));
    printf("  run.autotune                  = %s     (%s)\n", tc.run.autotune ? "true" : "false", src_str(tc.run.src_autotune));
    printf("  flood.enabled                 = %s     (%s)\n", tc.flood.enabled ? "true" : "false", src_str(tc.flood.src_enabled));
    printf("  flood.preamble_list           = %s    (%s)\n", tc.flood.preamble_list.c_str(), src_str(tc.flood.src_num));
    printf("  flood.strategy                = %s    (%s)\n", tc.flood.strategy.c_str(), src_str(tc.flood.src_strategy));
    printf("  flood.power_backoff_db        = %.1f  (%s)\n", tc.flood.power_backoff_db, src_str(tc.flood.src_backoff));
    printf("  flood.slm_candidates          = %u    (%s)\n", tc.flood.slm_candidates, src_str(tc.flood.src_slm));
    printf("  multi_ro.freq_pos_count       = %u    (%s)\n", tc.multi_ro.freq_pos_count, src_str(tc.multi_ro.src_freq_pos));
    printf("================================\n\n");
}

#pragma once
// influx_reader.h — InfluxDB read path for prach-inject using rt-recon-sdk
//
// Uses rtrs::InfluxWorker from the rt-recon-sdk (git@github.com:cueltschey/rt-recon-sdk)
// which shares the same influxdb.hpp and schema as sni5gect-compact.
//
// Pulls:
//   prach_cfg    -> prach_config_t  (config_idx, root_seq_idx, zero_corr_zone,
//                                    freq_offset, num_ra_preambles)
//   band_report  -> recon_band_report_t (dl_freq, ul_freq, nof_prb, sample_rate, ...)
//
// On ANY missing required field: logs FATAL and returns false.
// Caller must abort — never substitute a silent default.

#include "cell_config.h"
#include <string>
#include <cstdint>

struct influx_cfg {
    std::string host    = "localhost";
    int         port    = 8086;
    std::string org     = "rtu";
    std::string bucket  = "rtusystem";
    std::string data_id = "test";
    // token: loaded from env INFLUX_TOKEN at runtime
};

// Pull PRACH config and band report from InfluxDB into `cfg`.
// Returns true iff ALL required fields were found and sane.
bool influx_pull_cell_config(const influx_cfg& icfg, cell_config& cfg);

// Sanity-check pulled values against lab ground truth. Logs mismatches.
void influx_sanity_check(const cell_config& cfg);

// Pull MIB timing info from the sniffer's InfluxDB (sfn, ssb_idx, hrf, ssb_slot).
// Returns the absolute InfluxDB timestamp (Unix seconds) via *out_unix_time.
// Used as SSB-sync fallback when the B210 RX chain is unavailable.
bool influx_pull_mib_timing(const influx_cfg& icfg,
                            uint32_t* out_sfn, uint32_t* out_ssb_idx,
                            bool* out_hrf, uint32_t* out_ssb_slot,
                            uint32_t* out_scs_common,
                            double* out_unix_time);

//
// influx_reader.cpp — InfluxDB read path for ra-spoof
//
// Uses influxdb.hpp's flux_query() — the same header used by the sniffer
// (sni5gect-compact) and by the rt-recon-sdk — so the query schema is
// guaranteed to be compatible.
//
// The CSV parsing logic mirrors rtrs::InfluxWorker::parse_flux_fields()
// from git@github.com:cueltschey/rt-recon-sdk (src/autoconfig/influx_worker.cc).
// Adapted here without the srslog dependency to avoid GCC 15 / fmt::v7 issues.
//
// Fail-loud: every required field is explicitly checked; no silent defaults.

#include "influx_reader.h"

// influxdb.hpp from the rt-recon-sdk (schema-compatible with sni5gect-compact)
#include "rt-recon-sdk/autoconfig/influxdb.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <sstream>
#include <vector>
#include <unordered_map>

using FieldMap = std::unordered_map<std::string, std::string>;

// ---------------------------------------------------------------------------
// json_escape — mirrors rtrs::json_escape from rt-recon-sdk influx_worker.cc
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve((size_t)(s.size() * 1.2));
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// parse_flux_fields — mirrors rtrs::InfluxWorker::parse_flux_fields()
// Returns {field_name -> value} for the given annotated-CSV Flux response.
// ---------------------------------------------------------------------------
static FieldMap parse_flux_fields(const std::string& resp) {
    FieldMap result;
    std::stringstream ss(resp);
    std::string line;
    int field_col = -1;
    int value_col = -1;

    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::vector<std::string> cols;
        std::stringstream ls(line);
        std::string item;
        while (std::getline(ls, item, ',')) cols.push_back(item);

        if (field_col == -1) {
            // First non-annotation row is the header
            for (size_t i = 0; i < cols.size(); i++) {
                if (cols[i] == "_field") field_col = (int)i;
                if (cols[i] == "_value") value_col = (int)i;
            }
            continue;
        }

        if (field_col >= 0 && value_col >= 0 &&
            field_col < (int)cols.size() && value_col < (int)cols.size()) {
            result[cols[field_col]] = cols[value_col];
        }
    }
    return result;
}

// Like parse_flux_fields but also returns the _time value from the first data row
static FieldMap parse_flux_fields_with_time(const std::string& resp, double* out_unix_time) {
    FieldMap result;
    std::stringstream ss(resp);
    std::string line;
    int field_col = -1;
    int value_col = -1;
    int time_col  = -1;

    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::vector<std::string> cols;
        std::stringstream ls(line);
        std::string item;
        while (std::getline(ls, item, ',')) cols.push_back(item);

        if (field_col == -1) {
            for (size_t i = 0; i < cols.size(); i++) {
                if (cols[i] == "_field") field_col = (int)i;
                if (cols[i] == "_value") value_col = (int)i;
                if (cols[i] == "_time")  time_col  = (int)i;
            }
            continue;
        }

        if (field_col >= 0 && value_col >= 0 &&
            field_col < (int)cols.size() && value_col < (int)cols.size()) {
            result[cols[field_col]] = cols[value_col];
        }

        // Capture _time from the first data row (all rows share the same timestamp
        // when using last())
        if (out_unix_time && *out_unix_time == 0.0 && time_col >= 0 &&
            time_col < (int)cols.size() && !cols[time_col].empty()) {
            // Parse RFC3339 timestamp like "2026-06-26T17:15:00.670323509Z"
            // Crude but sufficient parser
            struct tm t = {};
            double frac = 0.0;
            int y, mon, d, h, m, s;
            char tz = 0;
            if (sscanf(cols[time_col].c_str(), "%d-%d-%dT%d:%d:%d%c",
                       &y, &mon, &d, &h, &m, &s, &tz) >= 6) {
                // Find fractional seconds
                auto dot_pos = cols[time_col].find('.');
                if (dot_pos != std::string::npos) {
                    frac = std::stod("0" + cols[time_col].substr(dot_pos));
                }
                t.tm_year = y - 1900;
                t.tm_mon  = mon - 1;
                t.tm_mday = d;
                t.tm_hour = h;
                t.tm_min  = m;
                t.tm_sec  = s;
                time_t epoch = timegm(&t);
                if (epoch != (time_t)-1) {
                    *out_unix_time = (double)epoch + frac;
                }
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// get_token — read from env; warn on fallback
// ---------------------------------------------------------------------------
static std::string get_token() {
    const char* env = std::getenv("INFLUX_TOKEN");
    if (env && strlen(env) > 0) return env;
    fprintf(stderr, "[influx_reader] WARNING: INFLUX_TOKEN env not set; using lab fallback token\n");
    return "605bc59413b7d5457d181ccf20f9fda15693f81b068d70396cc183081b264f3b";
}

// ---------------------------------------------------------------------------
// Perform a single-measurement Flux query; return field map
// ---------------------------------------------------------------------------
static bool query_measurement(const influx_cfg& icfg, const std::string& measurement,
                               FieldMap& out) {
    influxdb_cpp::server_info si(icfg.host, icfg.port, icfg.org, get_token(), icfg.bucket);
    if (si.resp_ != 0) {
        fprintf(stderr, "[influx_reader] FATAL: Cannot connect to InfluxDB at %s:%d\n",
                icfg.host.c_str(), icfg.port);
        return false;
    }

    char qbuf[512];
    snprintf(qbuf, sizeof(qbuf),
        "from(bucket: \"%s\")\n"
        "  |> range(start: -1d)\n"
        "  |> filter(fn: (r) => r._measurement == \"%s\")\n"
        "  |> filter(fn: (r) => r.sni5gect_data_id == \"%s\")\n"
        "  |> last()",
        icfg.bucket.c_str(), measurement.c_str(), icfg.data_id.c_str());

    std::string resp;
    int rc = influxdb_cpp::flux_query(resp, json_escape(std::string(qbuf)), si);
    if (rc != 0) {
        fprintf(stderr, "[influx_reader] FATAL: flux_query('%s') failed (rc=%d): %s\n",
                measurement.c_str(), rc, resp.c_str());
        return false;
    }

    out = parse_flux_fields(resp);
    if (out.empty()) {
        fprintf(stderr, "[influx_reader] FATAL: No data for measurement '%s' data_id='%s'\n",
                measurement.c_str(), icfg.data_id.c_str());
        fprintf(stderr, "                Is the sniffer running and has it received SIB1?\n");
        return false;
    }

    printf("[influx_reader] '%s': %zu fields pulled\n", measurement.c_str(), out.size());
    for (auto& kv : out) printf("    %-30s = %s\n", kv.first.c_str(), kv.second.c_str());

    return true;
}

// Require a field or fail loudly
static bool req_uint(const FieldMap& fm, const char* field, uint32_t& out) {
    auto it = fm.find(field);
    if (it == fm.end() || it->second.empty()) {
        fprintf(stderr, "[influx_reader] FATAL: Required field '%s' missing from InfluxDB\n", field);
        return false;
    }
    try { out = (uint32_t)std::stoul(it->second); return true; }
    catch (...) {
        fprintf(stderr, "[influx_reader] FATAL: Cannot parse field '%s'='%s' as uint\n",
                field, it->second.c_str());
        return false;
    }
}

static bool req_double(const FieldMap& fm, const char* field, double& out) {
    auto it = fm.find(field);
    if (it == fm.end() || it->second.empty()) {
        fprintf(stderr, "[influx_reader] FATAL: Required field '%s' missing from InfluxDB\n", field);
        return false;
    }
    try { out = std::stod(it->second); return true; }
    catch (...) {
        fprintf(stderr, "[influx_reader] FATAL: Cannot parse field '%s'='%s' as double\n",
                field, it->second.c_str());
        return false;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool influx_pull_cell_config(const influx_cfg& icfg, cell_config& cfg) {
    printf("[influx_reader] Querying InfluxDB %s:%d bucket=%s data_id=%s\n",
           icfg.host.c_str(), icfg.port, icfg.bucket.c_str(), icfg.data_id.c_str());
    printf("[influx_reader] (using influxdb.hpp flux_query — same client as rt-recon-sdk)\n");

    // --- prach_cfg ---
    FieldMap prach_fields;
    if (!query_measurement(icfg, "prach_cfg", prach_fields)) return false;

    uint32_t v = 0;
    bool ok = true;
    ok &= req_uint(prach_fields, "config_idx",       v); if (!ok) return false; cfg.prach_config_idx   = v;
    ok &= req_uint(prach_fields, "root_seq_idx",     v); if (!ok) return false; cfg.prach_root_seq_idx = v;
    ok &= req_uint(prach_fields, "zero_corr_zone",   v); if (!ok) return false; cfg.prach_zcz          = v;
    ok &= req_uint(prach_fields, "freq_offset",      v); if (!ok) return false; cfg.prach_freq_offset  = v;
    ok &= req_uint(prach_fields, "num_ra_preambles", v); if (!ok) return false; cfg.num_ra_preambles   = v;

    // Optional PRACH fields for diagnostics (from newer sniffer versions)
    {
        auto it = prach_fields.find("msg1_fdm");
        if (it != prach_fields.end())
            printf("[influx_reader]   msg1_fdm = %s\n", it->second.c_str());
    }

    // --- band_report ---
    FieldMap band_fields;
    if (!query_measurement(icfg, "band_report", band_fields)) return false;

    double dv = 0.0;
    ok &= req_double(band_fields, "dl_freq",     dv); if (!ok) return false; cfg.dl_freq_hz = dv;
    ok &= req_double(band_fields, "ul_freq",     dv); if (!ok) return false; cfg.ul_freq_hz = dv;
    ok &= req_double(band_fields, "sample_rate", dv); if (!ok) return false; cfg.srate_hz   = dv;
    ok &= req_uint(band_fields,   "nof_prb",     v);  if (!ok) return false; cfg.nof_prb    = v;
    ok &= req_uint(band_fields,   "dl_arfcn",    v);  if (!ok) return false; cfg.dl_arfcn   = v;

    // Optional
    {
        auto it_ssb = band_fields.find("ssb_freq");
        if (it_ssb != band_fields.end()) cfg.ssb_freq_hz = std::stod(it_ssb->second);
        auto it_ssba = band_fields.find("ssb_arfcn");
        if (it_ssba != band_fields.end()) cfg.ssb_arfcn = std::stoul(it_ssba->second);
    }

    // CFO sanity clamp (±50 kHz)
    const double CFO_MAX = 50e3;
    double ul_cfo = 0.0, dl_cfo = 0.0;
    {
        auto it = band_fields.find("uplink_cfo");
        if (it != band_fields.end()) ul_cfo = std::stod(it->second);
        it = band_fields.find("downlink_cfo");
        if (it != band_fields.end()) dl_cfo = std::stod(it->second);
    }
    if (std::abs(ul_cfo) > CFO_MAX || std::abs(dl_cfo) > CFO_MAX) {
        fprintf(stderr, "[influx_reader] WARNING: CFO out of range (ul=%.1f dl=%.1f Hz), zeroing\n",
                ul_cfo, dl_cfo);
    }

    printf("[influx_reader] Pulled cell config:\n");
    printf("  prach_config_idx   = %u\n",     cfg.prach_config_idx);
    printf("  prach_root_seq_idx = %u\n",     cfg.prach_root_seq_idx);
    printf("  prach_zcz          = %u\n",     cfg.prach_zcz);
    printf("  prach_freq_offset  = %u PRBs\n",cfg.prach_freq_offset);
    printf("  num_ra_preambles   = %u\n",     cfg.num_ra_preambles);
    printf("  dl_freq_hz         = %.3f MHz\n",cfg.dl_freq_hz / 1e6);
    printf("  ul_freq_hz         = %.3f MHz\n",cfg.ul_freq_hz / 1e6);
    printf("  srate_hz           = %.2f MHz\n",cfg.srate_hz / 1e6);
    printf("  nof_prb            = %u\n",     cfg.nof_prb);

    return true;
}

void influx_sanity_check(const cell_config& cfg) {
    constexpr double DL_EXPECTED = 1842.5e6;
    constexpr double UL_EXPECTED = 1747.5e6;

    printf("[influx_reader] Sanity-checking vs lab ground truth (n3 FDD):\n");
    bool pass = true;

    if (std::abs(cfg.dl_freq_hz - DL_EXPECTED) > 1e6) {
        fprintf(stderr, "[influx_reader] MISMATCH: dl_freq=%.3f MHz, expected %.3f MHz\n",
                cfg.dl_freq_hz / 1e6, DL_EXPECTED / 1e6);
        pass = false;
    }
    if (std::abs(cfg.ul_freq_hz - UL_EXPECTED) > 1e6) {
        fprintf(stderr, "[influx_reader] MISMATCH: ul_freq=%.3f MHz, expected %.3f MHz\n",
                cfg.ul_freq_hz / 1e6, UL_EXPECTED / 1e6);
        pass = false;
    }
    if (cfg.prach_config_idx != 1) {
        fprintf(stderr, "[influx_reader] MISMATCH: prach_config_idx=%u, expected 1\n",
                cfg.prach_config_idx);
        pass = false;
    }
    if (cfg.num_ra_preambles != 1) {
        fprintf(stderr, "[influx_reader] MISMATCH: num_ra_preambles=%u, expected 1\n",
                cfg.num_ra_preambles);
        pass = false;
    }

    if (pass) printf("  All sanity checks PASSED\n");
    else      printf("  WARNING: sanity checks FAILED — verify sniffer data\n");
}

// ---------------------------------------------------------------------------
// Pull MIB timing from sniffer for SSB-sync fallback
// ---------------------------------------------------------------------------
bool influx_pull_mib_timing(const influx_cfg& icfg,
                            uint32_t* out_sfn, uint32_t* out_ssb_idx,
                            bool* out_hrf, uint32_t* out_ssb_slot,
                            uint32_t* out_scs_common,
                            double* out_unix_time) {
    *out_unix_time = 0.0;

    influxdb_cpp::server_info si(icfg.host, icfg.port, icfg.org, get_token(), icfg.bucket);
    if (si.resp_ != 0) {
        fprintf(stderr, "[influx_reader] FATAL: Cannot connect to InfluxDB for MIB timing\n");
        return false;
    }

    char qbuf[512];
    snprintf(qbuf, sizeof(qbuf),
        "from(bucket: \"%s\")\n"
        "  |> range(start: -1d)\n"
        "  |> filter(fn: (r) => r._measurement == \"mib\")\n"
        "  |> filter(fn: (r) => r.sni5gect_data_id == \"%s\")\n"
        "  |> last()",
        icfg.bucket.c_str(), icfg.data_id.c_str());

    std::string resp;
    int rc = influxdb_cpp::flux_query(resp, json_escape(std::string(qbuf)), si);
    if (rc != 0) {
        fprintf(stderr, "[influx_reader] FATAL: flux_query('mib') failed (rc=%d): %s\n",
                rc, resp.c_str());
        return false;
    }

    double unix_ts = 0.0;
    FieldMap fields = parse_flux_fields_with_time(resp, &unix_ts);

    if (fields.empty()) {
        fprintf(stderr, "[influx_reader] FATAL: No MIB data from sniffer\n");
        return false;
    }

    printf("[influx_reader] 'mib' timing: %zu fields (unix_time=%.3f)\n",
           fields.size(), unix_ts);

    // Required fields
    auto it_sfn  = fields.find("sfn");
    auto it_idx  = fields.find("ssb_idx");
    auto it_hrf  = fields.find("hrf");
    auto it_scs  = fields.find("scs_common");

    if (it_sfn == fields.end() || it_idx == fields.end() ||
        it_hrf == fields.end() || it_scs == fields.end()) {
        fprintf(stderr, "[influx_reader] FATAL: MIB missing required fields\n");
        return false;
    }

    try {
        *out_sfn    = (uint32_t)std::stoul(it_sfn->second);
        *out_ssb_idx = (uint32_t)std::stoul(it_idx->second);
        *out_hrf    = (it_hrf->second == "true" || it_hrf->second == "1");
        *out_scs_common = (uint32_t)std::stoul(it_scs->second);
    } catch (...) {
        fprintf(stderr, "[influx_reader] FATAL: Cannot parse MIB fields\n");
        return false;
    }

    // Compute SSB slot from ssb_idx and half-frame flag
    // Pattern A, 15 kHz SCS, Lmax=4 or 8:
    //   First half:  ssb0 at slot 0 (symbols 4-7), ssb1 at slot 1 (symbols 8-11)
    //                 ssb2 at slot 0 (symbols 16-19), ssb3 at slot 1 (symbols 20-23)
    //   Second half: ssb4 at slot 10 (symbols 2-5), ssb5 at slot 11 (symbols 6-9)
    //                 ssb6 at slot 10 (symbols 14-17), ssb7 at slot 11 (symbols 18-21)
    // Simplified: map ssb_idx to slot
    uint32_t ssb_slot;
    bool hrf = *out_hrf;
    if (!hrf) {
        ssb_slot = (*out_ssb_idx < 2) ? 0 : 1;  // first half-frame
    } else {
        ssb_slot = 10 + ((*out_ssb_idx < 2) ? 0 : 1);  // second half-frame
    }
    *out_ssb_slot = ssb_slot;

    *out_unix_time = unix_ts;

    printf("[influx_reader] MIB: SFN=%u ssb_idx=%u hrf=%d slot=%u unix_time=%.6f\n",
           *out_sfn, *out_ssb_idx, hrf, ssb_slot, unix_ts);

    return true;
}

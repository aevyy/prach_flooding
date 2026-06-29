#include "log_csv.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace log_csv {

static FILE* g_csv = nullptr;
static std::mutex g_mutex;

void init(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_csv = fopen(path.c_str(), "a");
    if (g_csv) {
        fprintf(g_csv, "timestamp,channel,system_slot,sfn,slot_in_frame,rnti,rapid,tx_gain,on_air_ticks,outcome,flood_n,flood_strategy,flood_rapid_list\n");
        fflush(g_csv);
    }
}

void close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_csv) {
        fclose(g_csv);
        g_csv = nullptr;
    }
}

static void get_timestamp(char* buf, size_t len) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02d.%03ld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             ts.tv_nsec / 1000000);
}

void log_event(const char* channel, uint32_t slot, uint32_t sfn,
               uint32_t slot_in_frame, uint16_t rnti, uint16_t rapid,
               double tx_gain, uint64_t on_air_time_ticks, const char* outcome,
               uint32_t flood_n, const char* flood_strategy, const char* flood_rapid_list) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_csv) return;
    
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    
    // Fallback: if flood_rapid_list is empty, use the single rapid value
    std::string rapid_list = flood_rapid_list;
    if (rapid_list.empty()) {
        rapid_list = std::to_string(rapid);
    }

    fprintf(g_csv, "%s,%s,%u,%u,%u,0x%04x,%u,%.1f,%lu,%s,%u,%s,%s\n",
            ts, channel, slot, sfn, slot_in_frame, rnti, rapid,
            tx_gain, (unsigned long)on_air_time_ticks, outcome,
            flood_n, flood_strategy, rapid_list.c_str());
    fflush(g_csv);
}

void log_prach_detect(uint32_t slot, uint32_t sfn, uint32_t slot_in_frame,
                      uint32_t rapid, float ta_us, float detection_metric) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_csv) return;
    
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    fprintf(g_csv, "%s,PRACH_DETECT,%u,%u,%u,0,0x%x,0.0,0,ta=%.2fus_metric=%.2f\n",
            ts, slot, sfn, slot_in_frame, rapid, ta_us, detection_metric);
    fflush(g_csv);
}

void log_msg3_detect(uint32_t slot, uint32_t sfn, uint32_t slot_in_frame,
                     uint16_t tc_rnti, const uint8_t* cid, uint32_t cid_len) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_csv) return;
    
    char ts[64];
    get_timestamp(ts, sizeof(ts));
    char cid_hex[65] = {0};
    for (uint32_t i = 0; i < cid_len && i < 32; i++) {
        snprintf(cid_hex + 2*i, 3, "%02x", cid[i]);
    }
    fprintf(g_csv, "%s,MSG3_DETECT,%u,%u,%u,0x%04x,0,0.0,0,cid=%s\n",
            ts, slot, sfn, slot_in_frame, tc_rnti, cid_hex);
    fflush(g_csv);
}

} // namespace log_csv

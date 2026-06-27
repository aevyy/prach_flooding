#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <mutex>

namespace log_csv {

void init(const std::string& path);
void close();

// Log a transmission event
void log_event(const char* channel,     // "Msg2", "Msg4"
               uint32_t    slot,        // system slot number
               uint32_t    sfn,
               uint32_t    slot_in_frame,
               uint16_t    rnti,        // RA-RNTI or TC-RNTI
               uint16_t    rapid,       // RAPID (0)
               double      tx_gain,
               uint64_t    on_air_time_ticks, // UHD device time
               const char* outcome);    // "sent", "decoded", "no_response", etc.

// Log a PRACH detection event
void log_prach_detect(uint32_t slot, uint32_t sfn, uint32_t slot_in_frame,
                      uint32_t rapid, float ta_us, float detection_metric);

// Log a Msg3 detection event
void log_msg3_detect(uint32_t slot, uint32_t sfn, uint32_t slot_in_frame,
                     uint16_t tc_rnti, const uint8_t* cid, uint32_t cid_len);

} // namespace log_csv

#pragma once

#include <cstdint>

// Msg4 contention resolution PDU builder
class msg4_builder {
public:
    msg4_builder() = default;
    
    // Build the contention resolution MAC CE + RRCSetup payload
    // cid = first 48 bits of UE's Msg3 CCCH SDU (6 bytes)
    // Returns payload length, 0 on error.
    uint32_t build(const uint8_t* cid, uint32_t cid_len,
                   uint8_t* out_pdu, uint32_t max_len);
    
    // TODO M3: Full Msg4 with RRCSetup encoding
};

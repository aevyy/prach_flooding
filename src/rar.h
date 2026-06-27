#pragma once

#include <cstdint>
#include <array>

// MAC RAR PDU assembly for Msg2 (RAR)
class rar_builder {
public:
    rar_builder() = default;
    
    // Build a MAC RAR PDU with:
    //   ta        = timing advance command (12-bit)
    //   tc_rnti   = temporary C-RNTI to assign (16-bit)
    //   ul_grant_bits = 27-bit UL grant (per TS 38.213 Table 8.2-1)
    // Returns the number of bytes written to out_pdu (8 or 10 with padding).
    uint32_t build(uint16_t ta, uint16_t tc_rnti,
                   const std::array<uint8_t, 27>& ul_grant_bits,
                   uint8_t* out_pdu, uint32_t max_len);
    
    // Build the 27-bit UL grant field for Msg3 PUSCH.
    // freq_rb_start: starting RB for Msg3 (8 per gNB log)
    // freq_rb_len:   length in RB (4 per gNB log)
    // tda_idx:       time domain allocation index (0)
    // mcs:           MCS index (0 = QPSK)
    static void build_ul_grant(uint32_t freq_rb_start, uint32_t freq_rb_len,
                                uint32_t tda_idx, uint32_t mcs,
                                std::array<uint8_t, 27>& bits);
};

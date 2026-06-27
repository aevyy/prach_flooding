#include "rar.h"
#include <cstdio>
#include <cstring>

// MAC RAR PDU assembly per TS 38.321 Section 6.2.3
// 
// RAR subPDU = 1-byte E/T/RAPID subheader + 7-byte MAC RAR payload = 8 bytes
//
// E/T/RAPID subheader:
//   E (1 bit): Extension flag (0 = last subPDU)
//   T (1 bit): Type flag (1 = RAPID)
//   RAPID (6 bits): Random Access Preamble Identifier (0 in our case)
//
// MAC RAR payload (7 bytes = 56 bits):
//   R (1 bit): Reserved
//   Timing Advance Command (12 bits)
//   UL Grant (27 bits): per TS 38.213 Table 8.2-1
//      - Frequency hopping flag: 1 bit
//      - PUSCH frequency resource allocation: 14 bits
//      - PUSCH time resource allocation: 4 bits
//      - MCS: 4 bits
//      - TPC command for PUSCH: 3 bits
//      - CSI request: 1 bit
//   Temporary C-RNTI (16 bits)

// The gNB log shows RAR: ra-rnti=0x39 rb=[0..3) tbs=10
// and UL grant: rb=[8..11) for Msg3

uint32_t rar_builder::build(uint16_t ta, uint16_t tc_rnti,
                            const std::array<uint8_t, 27>& ul_grant_bits,
                            uint8_t* out_pdu, uint32_t max_len) {
    // Need at least 8 bytes
    if (max_len < 8) {
        fprintf(stderr, "ERROR: RAR output buffer too small (%u < 8)\n", max_len);
        return 0;
    }
    
    memset(out_pdu, 0, max_len);
    
    // Byte 0: E/T/RAPID subheader
    // E=0 (last), T=1 (RAPID), RAPID=0
    out_pdu[0] = (0 << 7) | (1 << 6) | (0 & 0x3F);
    // = 0x40
    
    // Bytes 1-7: MAC RAR payload (56 bits)
    // Bit positions within 56-bit payload (MSB first):
    // bit 0: R (reserved) = 0
    // bits 1-12: TA (12 bits)
    // bits 13-39: UL Grant (27 bits)
    // bits 40-55: TC-RNTI (16 bits)
    
    uint64_t payload = 0;
    
    // R bit = 0 (already 0)
    
    // TA command (12 bits, TA & 0xFFF)
    payload |= ((uint64_t)(ta & 0xFFF)) << (56 - 1 - 12);
    
    // UL Grant (27 bits)
    for (int i = 0; i < 27; i++) {
        if (ul_grant_bits[i]) {
            payload |= (1ULL << (56 - 1 - 12 - 1 - i));
        }
    }
    // UL grant position: bits 13-39 (0-indexed)
    // payload |= ((uint64_t)ul_grant_bits_val) << (56 - 13 - 27);
    
    // TC-RNTI (16 bits, bits 40-55)
    // Actually: after R(1) + TA(12) + UL(27) = 40 bits, 
    // TC-RNTI goes in bits 40-55
    payload |= ((uint64_t)tc_rnti) << (56 - 40 - 16);
    
    // Now write payload to bytes 1-7 (7 bytes = 56 bits)
    for (int i = 0; i < 7; i++) {
        out_pdu[1 + i] = (payload >> (48 - 8*i)) & 0xFF;
    }
    
    // Pad to tbs=10 as gNB does? The gNB uses tbs=10.
    // If caller passes max_len >= 10, add 2 bytes of zero padding
    if (max_len >= 10) {
        out_pdu[8] = 0;
        out_pdu[9] = 0;
        return 10;
    }
    
    return 8;
}

// Build UL grant bits for the Msg3 PUSCH
// Per gNB log: rb=[8..11) for Msg3
void rar_builder::build_ul_grant(uint32_t freq_rb_start, uint32_t freq_rb_len,
                                  uint32_t tda_idx, uint32_t mcs,
                                  std::array<uint8_t, 27>& bits) {
    bits.fill(0);
    
    // UL grant fields per TS 38.213 Table 8.2-1:
    // Bit 0: Frequency hopping flag
    bits[0] = 0; // no hopping
    
    // Bits 1-14: PUSCH frequency resource allocation (14 bits)
    // RIV encoding: if (L-1) <= floor(N/2): RIV = N*(L-1)+S
    // else: RIV = N*(N-L+1)+(N-1-S)
    // N = 106 PRB, S = freq_rb_start, L = freq_rb_len
    uint32_t N = 106;
    uint32_t S = freq_rb_start; // 8
    uint32_t L = freq_rb_len;    // 4
    uint32_t riv;
    if ((L - 1) <= N/2) {
        riv = N * (L - 1) + S;
    } else {
        riv = N * (N - L + 1) + (N - 1 - S);
    }
    // riv = 106 * (4-1) + 8 = 318 + 8 = 326
    for (int i = 0; i < 14; i++) {
        bits[1 + i] = (riv >> (13 - i)) & 1;
    }
    
    // Bits 15-18: PUSCH time resource allocation (4 bits)
    for (int i = 0; i < 4; i++) {
        bits[15 + i] = (tda_idx >> (3 - i)) & 1;
    }
    
    // Bits 19-22: MCS (4 bits)
    for (int i = 0; i < 4; i++) {
        bits[19 + i] = (mcs >> (3 - i)) & 1;
    }
    
    // Bits 23-25: TPC command for PUSCH (3 bits)
    bits[23] = 0;
    bits[24] = 1; // 010 = +4 dB
    bits[25] = 0;
    
    // Bit 26: CSI request (1 bit)
    bits[26] = 0; // no CSI request
}

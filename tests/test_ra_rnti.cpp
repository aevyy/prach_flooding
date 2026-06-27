#include "ra_rnti.h"
#include <cstdio>
#include <cassert>

int main() {
    printf("=== RA-RNTI Test ===\n\n");
    
    // For this cell: s_id=0, t_id=4, f_id=0, ul_carrier_id=0
    // Expected: 1 + 0 + 14*4 = 57
    uint16_t ra_rnti = cell_ra_rnti();
    printf("cell_ra_rnti() = 0x%04x (%u)\n", ra_rnti, ra_rnti);
    printf("Expected:       0x0039 (57)\n");
    
    bool pass = (ra_rnti == 57) && (ra_rnti == 0x39);
    printf("Result: %s\n\n", pass ? "PASS" : "FAIL");
    
    // Test the formula directly
    uint16_t r1 = compute_ra_rnti(0, 4, 0, 0);
    uint16_t r2 = compute_ra_rnti(1, 5, 0, 0);
    uint16_t r3 = compute_ra_rnti(0, 0, 1, 0);
    uint16_t r4 = compute_ra_rnti(0, 0, 0, 1);
    
    printf("Edge cases:\n");
    printf("  compute_ra_rnti(0,4,0,0) = %u (expected 57)\n", r1);
    printf("  compute_ra_rnti(1,5,0,0) = %u (expected 1+1+14*5=72)\n", r2);
    printf("  compute_ra_rnti(0,0,1,0) = %u (expected 1+14*80=1121)\n", r3);
    printf("  compute_ra_rnti(0,0,0,1) = %u (expected 1+14*80*8=8961)\n", r4);
    
    assert(r1 == 57);
    assert(r2 == 72);
    assert(r3 == 1121);
    assert(r4 == 8961);
    
    // Verify against gNB log: ra-rnti=0x39
    printf("\nAll RA-RNTI tests passed.\n");
    return 0;
}

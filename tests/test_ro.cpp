#include "cell_config.h"
#include "ro.h"
#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <vector>

// Ground truth PRACH occasions from /tmp/gnb.log
// Format: SFN.slot where .4 means slot 4
// All at SFN % 16 == 1
static const uint32_t ground_truth_sfns[] = {
    369, 577, 593, 609, 625, 641, 657, 673, 769
};

int main() {
    cell_config cfg;
    cfg.prach_format   = 0;
    cfg.prach_subframe = 4;
    cfg.prach_x        = 16;
    cfg.prach_y        = 1;

    printf("=== RO Calculator Test ===\n\n");

    // Test 1: is_occasion matches ground truth
    printf("Test 1: Ground truth occasions\n");
    int fails = 0;
    for (size_t i = 0; i < sizeof(ground_truth_sfns)/sizeof(ground_truth_sfns[0]); i++) {
        uint32_t sfn = ground_truth_sfns[i];
        bool ok = ro::is_occasion(cfg, sfn, 4);
        printf("  SFN=%u slot=4: %s\n", sfn, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }
    assert(fails == 0);

    // Test 2: Negative check - nearby SFNs should NOT be occasions
    printf("\nTest 2: Non-occasion SFNs (should all be false)\n");
    uint32_t non_occasions[] = {368, 370, 576, 578, 608, 610, 624, 626, 770};
    for (size_t i = 0; i < sizeof(non_occasions)/sizeof(non_occasions[0]); i++) {
        uint32_t sfn = non_occasions[i];
        bool ok = !ro::is_occasion(cfg, sfn, 4);
        printf("  SFN=%u slot=4: %s (should be false)\n", sfn, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }

    // Test 3: Wrong slot at correct SFN
    printf("\nTest 3: Correct SFN, wrong slot (should be false)\n");
    uint32_t test_sfns[] = {369, 577, 593};
    uint32_t wrong_slots[] = {0, 1, 5, 9};
    for (auto sfn : test_sfns) {
        for (auto slot : wrong_slots) {
            bool ok = !ro::is_occasion(cfg, sfn, slot);
            if (!ok) {
                printf("  SFN=%u slot=%u: FAIL (should be false)\n", sfn, slot);
                fails++;
            }
        }
    }
    if (fails == 0) {
        for (auto sfn : test_sfns) {
            for (auto slot : wrong_slots) {
                printf("  SFN=%u slot=%u: PASS\n", sfn, slot);
            }
        }
    }

    // Test 4: system_slot / from_system_slot round-trip
    printf("\nTest 4: Slot conversion round-trip\n");
    uint32_t test_pairs[][2] = {{0,0}, {1,4}, {369,4}, {1023,9}, {0,9}};
    for (size_t i = 0; i < sizeof(test_pairs)/sizeof(test_pairs[0]); i++) {
        uint32_t sfn = test_pairs[i][0], slot = test_pairs[i][1];
        uint32_t sys_slot = ro::system_slot(sfn, slot);
        uint32_t sfn2, slot2;
        ro::from_system_slot(sys_slot, sfn2, slot2);
        bool ok = (sfn == sfn2 && slot == slot2);
        printf("  SFN=%u slot=%u -> sys_slot=%u -> SFN=%u slot=%u: %s\n",
               sfn, slot, sys_slot, sfn2, slot2, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }

    // Test 5: next_occasion
    printf("\nTest 5: Next occasion after known slots\n");
    struct {
        uint32_t after_slot;
        uint32_t expected_slot;
    } next_tests[] = {
        {0,   10*1 + 4},  // SFN=1 slot=4 -> slot 14
        {14,  10*17 + 4}, // SFN=17 slot=4 -> slot 174
        {174, 10*33 + 4}, // SFN=33 slot=4 -> slot 334
        {3680, 10*369 + 4}, // SFN=369 slot=4 -> slot 3694
    };
    for (auto& t : next_tests) {
        uint32_t n = ro::next_occasion_slot(cfg, t.after_slot);
        uint32_t sfn, slot;
        ro::from_system_slot(n, sfn, slot);
        bool ok = (n == t.expected_slot);
        printf("  after slot=%u: next=%u (SFN=%u.%u) expected=%u: %s\n",
               t.after_slot, n, sfn, slot, t.expected_slot, ok ? "PASS" : "FAIL");
        if (!ok) fails++;
    }

    printf("\n=== Results: %d failures ===\n", fails);
    return fails ? 1 : 0;
}

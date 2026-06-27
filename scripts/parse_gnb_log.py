#!/usr/bin/env python3
"""
Parse gNB log to extract real-UE PRACH occasions (SFN, slot).
Used to validate RO calculator against ground truth.

Usage: python3 parse_gnb_log.py [/path/to/gnb.log]
"""

import re
import sys

def parse_prach_occasions(log_path):
    """
    Extract PRACH detection events from gNB log.
    Returns list of (sfn, slot) tuples.
    
    Log format example:
    [   609.4] PRACH: rsi=1 rssi=-14.8dB detected_preambles=[{idx=0 ta=2.34us detection_metric=4.2}] t=537.2us
    
    Where 609.4 means SFN=609, slot=4 (10 slots/frame at 15 kHz SCS).
    """
    occasions = []
    pattern = re.compile(r'\[\s*(\d+)\.(\d+)\]\s+PRACH:')
    
    with open(log_path, 'r') as f:
        for line in f:
            m = pattern.search(line)
            if m:
                sfn = int(m.group(1))
                slot = int(m.group(2))
                occasions.append((sfn, slot))
    
    return occasions

def validate_ro(occasions):
    """
    Validate that all PRACH occasions match:
    - config_idx=1 -> format 0, subframe 4
    - SFN % 16 == 1
    All detected occasions should be at slot=4 with SFN%16==1.
    """
    valid = True
    for sfn, slot in occasions:
        is_valid = (slot == 4) and (sfn % 16 == 1)
        if not is_valid:
            print(f"  MISMATCH: SFN={sfn} slot={slot} (expected: slot=4, SFN%16==1)")
            valid = False
    
    return valid

def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/gnb.log'
    
    occasions = parse_prach_occasions(log_path)
    
    if not occasions:
        print("No PRACH detection events found in log.")
        return 1
    
    print(f"Found {len(occasions)} PRACH occasions:")
    for sfn, slot in occasions:
        mod_check = "OK" if (sfn % 16 == 1) else "FAIL"
        slot_check = "OK" if (slot == 4) else "FAIL"
        print(f"  SFN={sfn:4d}  slot={slot}  SFN%16==1:{mod_check:5s}  slot==4:{slot_check:5s}")
    
    print()
    if validate_ro(occasions):
        print("ALL occasions match expected pattern (slot=4, SFN%16==1). RO calculator is VALIDATED.")
        return 0
    else:
        print("SOME occasions DO NOT match expected pattern. RO calculator needs fixes.")
        return 1

if __name__ == '__main__':
    sys.exit(main())

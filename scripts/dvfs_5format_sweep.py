#!/usr/bin/env python3
"""DVFS voltage sweep on QNN workload — all 5 MX formats (cw305_live).

Voltage: 1.00V to 0.60V, 10mV steps, 3 trials per voltage.
Reprogram-per-trial protocol ensures transient fault isolation.
J16=0 (PLL clock), no CW-Lite needed.

Authors: Dillibabu Shanmugam, Patrick Schaumont (WPI)
"""

import chipwhisperer as cw
import csv, os, struct, time

BITDIR = os.path.join(os.path.dirname(__file__), '..', '..', 'fpga', 'cw305_live', 'bitstreams')
OUTDIR = os.path.join(os.path.dirname(__file__), '..', 'datasets', 'dvfs_qnn')
os.makedirs(OUTDIR, exist_ok=True)

FORMATS = ['mxint8', 'mxfp8_e4m3', 'mxfp8_e5m2', 'mxlog8', 'mxlog8_logdom']
GOLDEN = {'mxint8': 25, 'mxfp8_e4m3': 14, 'mxfp8_e5m2': 11, 'mxlog8': 8, 'mxlog8_logdom': 0}
VOLTAGES = [round(1.00 - i * 0.01, 2) for i in range(41)]
TRIALS = 3

FIELDS = ['format', 'vccint', 'trial', 'eoc', 'ret', 'golden_ret', 'class',
           'xor', 'bits_diff', 'se_a', 'se_b', 'mac_acc', 'rs1', 'rs2',
           'op', 'fmt_hw', 'inflight', 'rd', 'id']


def read32(t, a):
    return struct.unpack('<I', bytes(t.fpga_read(a, 4)))[0]


def run_trial(bitstream, vcc):
    target = cw.target(None, cw.targets.CW305, bsfile=bitstream, force=True)
    target.vccint_set(1.0); time.sleep(0.2)
    target.pll.pll_enable_set(True)
    target.pll.pll_outenable_set(True, 1)
    target.pll.pll_outfreq_set(20e6, 1)
    target.pll.pll_outsource_set('PLL1', 1); time.sleep(0.2)
    target.vccint_set(vcc); time.sleep(0.3)
    target.fpga_write(0x03, [0x01]); time.sleep(0.02)
    target.fpga_write(0x03, [0x00]); time.sleep(2.0)
    s = read32(target, 0x00)
    rs1 = read32(target, 0x08)
    rs2 = read32(target, 0x09)
    mac = read32(target, 0x0A)
    ctrl = read32(target, 0x0B)
    target.vccint_set(1.0); time.sleep(0.1)
    target.dis(); time.sleep(0.2)
    return {
        'eoc': s & 1, 'ret': s >> 1,
        'rs1': rs1, 'rs2': rs2, 'mac_acc': mac,
        'se_a': ctrl & 0xFF, 'se_b': (ctrl >> 8) & 0xFF,
        'op': (ctrl >> 16) & 0xF, 'fmt_hw': (ctrl >> 19) & 0x7,
        'inflight': (ctrl >> 22) & 0x1, 'rd': (ctrl >> 24) & 0x1F,
        'id': (ctrl >> 29) & 0x7,
    }


def classify(eoc, ret, golden):
    if ret == 0x7FFFFFFF:
        return 'dead'
    if eoc == 0:
        return 'crash'
    if ret == golden:
        return 'normal'
    return 'fault'


all_rows = []
for fmt in FORMATS:
    bitstream = os.path.join(BITDIR, f'{fmt}.bit')
    if not os.path.exists(bitstream):
        print(f'SKIP {fmt}: bitstream not found')
        continue
    golden = GOLDEN[fmt]
    print(f'\n{"="*60}\n  {fmt} (golden={golden})\n{"="*60}')
    rows = []
    for vcc in VOLTAGES:
        for trial in range(TRIALS):
            d = run_trial(bitstream, vcc)
            cls = classify(d['eoc'], d['ret'], golden)
            xor_val = d['ret'] ^ golden if cls not in ('dead', 'crash') else -1
            bits = bin(xor_val).count('1') if xor_val >= 0 else -1
            row = {'format': fmt, 'vccint': vcc, 'trial': trial,
                   'eoc': d['eoc'], 'ret': d['ret'], 'golden_ret': golden,
                   'class': cls, 'xor': xor_val, 'bits_diff': bits,
                   'se_a': d['se_a'], 'se_b': d['se_b'], 'mac_acc': d['mac_acc'],
                   'rs1': d['rs1'], 'rs2': d['rs2'], 'op': d['op'],
                   'fmt_hw': d['fmt_hw'], 'inflight': d['inflight'],
                   'rd': d['rd'], 'id': d['id']}
            rows.append(row)
        classes = [r['class'] for r in rows[-TRIALS:]]
        rets = [r['ret'] for r in rows[-TRIALS:]]
        se_as = [r['se_a'] for r in rows[-TRIALS:]]
        print(f'  V={vcc:.2f}: {classes} ret={rets} se_a={se_as}')
        if all(c == 'dead' for c in classes):
            print(f'  Dead, stopping'); break
    csvpath = os.path.join(OUTDIR, f'dvfs_{fmt}.csv')
    with open(csvpath, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=FIELDS); w.writeheader(); w.writerows(rows)
    print(f'Saved {len(rows)} rows to {csvpath}')
    all_rows.extend(rows)

combined = os.path.join(OUTDIR, 'dvfs_all_formats.csv')
with open(combined, 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=FIELDS); w.writeheader(); w.writerows(all_rows)
print(f'\nCombined: {len(all_rows)} rows to {combined}')

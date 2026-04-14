#!/usr/bin/env python3
"""Clock glitch on BitNet (TinyLLM) workload — all 5 MX formats via CW-Lite.

Uses cw305_live bitstreams patched with BitNet firmware (*_bitnet.bit).
J16=1 (tio_clkin from CW-Lite), CW-Lite connected.

Golden values auto-discovered per format at nominal.
"""

import chipwhisperer as cw
import csv, os, struct, time

PROJ = os.path.join(os.path.dirname(__file__), '..')
BITDIR = f'{PROJ}/fpga/cw305_live/bitstreams'
OUTDIR = f'{PROJ}/fi_artifact/datasets/clock_glitch'
os.makedirs(OUTDIR, exist_ok=True)

FORMATS = ['mxint8', 'mxfp8_e4m3', 'mxfp8_e5m2', 'mxlog8', 'mxlog8_logdom']

FIELDS = ['workload', 'format', 'width', 'offset', 'ext_offset', 'trial',
          'eoc', 'ret', 'golden_ret', 'class', 'se_a', 'se_b',
          'mac_acc', 'rs1', 'rs2', 'op', 'fmt_hw', 'inflight', 'rd', 'id']


def read32(t, a):
    return struct.unpack('<I', bytes(t.fpga_read(a, 4)))[0]


def setup_scope():
    scope = cw.scope()
    scope.clock.clkgen_freq = 20e6
    scope.glitch.clk_src = 'clkgen'
    scope.glitch.output = 'clock_xor'
    scope.glitch.trigger_src = 'ext_single'
    scope.glitch.repeat = 1
    scope.io.hs2 = 'glitch'
    scope.adc.timeout = 5
    return scope


def run_glitch_trial(scope, bitstream, width, ext_offset):
    target = cw.target(scope, cw.targets.CW305, bsfile=bitstream, force=True)
    target.pll.pll_enable_set(True)
    target.pll.pll_outenable_set(True, 1)
    target.pll.pll_outfreq_set(20e6, 1)
    target.pll.pll_outsource_set('PLL1', 1)
    time.sleep(0.2)

    scope.glitch.width = width
    scope.glitch.offset = 0
    scope.glitch.ext_offset = ext_offset

    target.fpga_write(0x03, [0x01]); time.sleep(0.02)
    scope.arm()
    target.fpga_write(0x05, [0x01]); time.sleep(0.01)
    target.fpga_write(0x03, [0x00])

    timeout = 5.0
    t0 = time.time()
    while time.time() - t0 < timeout:
        s = read32(target, 0x00)
        if s & 1:
            break
        time.sleep(0.05)

    target.fpga_write(0x05, [0x00])
    s = read32(target, 0x00)
    rs1 = read32(target, 0x08)
    rs2 = read32(target, 0x09)
    mac = read32(target, 0x0A)
    ctrl = read32(target, 0x0B)
    target.dis(); time.sleep(0.1)

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


scope = setup_scope()

for fmt in FORMATS:
    bitstream = f'{BITDIR}/{fmt}_bitnet.bit'
    if not os.path.exists(bitstream):
        print(f'SKIP {fmt}: {bitstream} not found')
        continue

    # Discover golden at nominal (no glitch, just program and run)
    print(f'\nDiscovering golden for {fmt}...')
    d = run_glitch_trial(scope, bitstream, 0, 0)
    golden = d['ret']
    print(f'  Golden ret = {golden} (0x{golden:x}), se_a={d["se_a"]}')

    print(f'\n{"="*60}')
    print(f'  Clock Glitch BitNet: {fmt} (golden={golden})')
    print(f'{"="*60}')

    rows = []

    # Phase 1: Width sweep w=-5 to -24, 3 trials each
    print('  Phase 1: Width sweep w=-5 to -24, ext_offset=50')
    for w in range(-5, -25, -1):
        for trial in range(3):
            d = run_glitch_trial(scope, bitstream, w, 50)
            cls = classify(d['eoc'], d['ret'], golden)
            row = {'workload': 'bitnet_tinyllm', 'format': fmt,
                   'width': w, 'offset': 0, 'ext_offset': 50, 'trial': trial,
                   'eoc': d['eoc'], 'ret': d['ret'], 'golden_ret': golden,
                   'class': cls, 'se_a': d['se_a'], 'se_b': d['se_b'],
                   'mac_acc': d['mac_acc'], 'rs1': d['rs1'], 'rs2': d['rs2'],
                   'op': d['op'], 'fmt_hw': d['fmt_hw'], 'inflight': d['inflight'],
                   'rd': d['rd'], 'id': d['id']}
            rows.append(row)
        classes = [r['class'] for r in rows[-3:]]
        print(f'    w={w}: {classes}')
        if all(c == 'dead' for c in classes):
            print(f'    Dead at w={w}, stopping')
            break

    # Phase 2: Dense ext_offset sweep
    print('  Phase 2: Dense sweep w=-8,-9,-10, ext=50-500 step 50')
    for w in [-8, -9, -10]:
        for ext in range(50, 550, 50):
            d = run_glitch_trial(scope, bitstream, w, ext)
            cls = classify(d['eoc'], d['ret'], golden)
            row = {'workload': 'bitnet_tinyllm', 'format': fmt,
                   'width': w, 'offset': 0, 'ext_offset': ext, 'trial': 0,
                   'eoc': d['eoc'], 'ret': d['ret'], 'golden_ret': golden,
                   'class': cls, 'se_a': d['se_a'], 'se_b': d['se_b'],
                   'mac_acc': d['mac_acc'], 'rs1': d['rs1'], 'rs2': d['rs2'],
                   'op': d['op'], 'fmt_hw': d['fmt_hw'], 'inflight': d['inflight'],
                   'rd': d['rd'], 'id': d['id']}
            rows.append(row)
            if cls != 'normal':
                print(f'    w={w} ext={ext}: {cls} ret={d["ret"]} se_a={d["se_a"]}')

    csvpath = f'{OUTDIR}/clkglitch_{fmt}_bitnet.csv'
    with open(csvpath, 'w', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    print(f'  Saved {len(rows)} rows to {csvpath}')

scope.dis()
print('\nDone. All formats complete.')

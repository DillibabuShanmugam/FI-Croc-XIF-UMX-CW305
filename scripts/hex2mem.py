#!/usr/bin/env python3
"""Convert RISC-V hex file to Xilinx XPM MEMORY_INIT_FILE (.mem) format.

Splits the hex into two SRAM bank init files for the CROC SoC:
  Bank 0: 0x10000000 - 0x100007FF  (1024 words, IMEM)
  Bank 1: 0x10000800 - 0x10000FFF  (1024 words, DMEM)

Usage:
  python3 hex2mem.py <input.hex> <bank0_output.mem> <bank1_output.mem>
"""

import sys

SRAM_BASE = 0x10000000
WORDS_PER_BANK = 1024
BYTES_PER_WORD = 4
BANK_SIZE = WORDS_PER_BANK * BYTES_PER_WORD  # 4096 bytes


def parse_hex(path):
    """Parse Verilog-style hex file (@addr + space-separated bytes)."""
    mem = {}
    addr = 0
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith('@'):
                addr = int(line[1:], 16)
                continue
            for byte_str in line.split():
                mem[addr] = int(byte_str, 16)
                addr += 1
    return mem


def build_bank_mem(mem, bank_base, num_words):
    """Build list of 32-bit word hex strings for one SRAM bank."""
    lines = []
    for w in range(num_words):
        byte_addr = bank_base + w * 4
        b0 = mem.get(byte_addr + 0, 0)
        b1 = mem.get(byte_addr + 1, 0)
        b2 = mem.get(byte_addr + 2, 0)
        b3 = mem.get(byte_addr + 3, 0)
        word = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)  # little-endian
        lines.append(f"{word:08X}")
    return lines


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.hex> <bank0.mem> <bank1.mem>")
        sys.exit(1)

    hex_path, bank0_path, bank1_path = sys.argv[1], sys.argv[2], sys.argv[3]

    mem = parse_hex(hex_path)
    addrs = sorted(mem.keys())
    print(f"Parsed {len(mem)} bytes from {hex_path}")
    print(f"  Address range: 0x{addrs[0]:08X} - 0x{addrs[-1]:08X}")

    bank0_base = SRAM_BASE
    bank1_base = SRAM_BASE + BANK_SIZE

    # Count non-zero words per bank
    bank0_lines = build_bank_mem(mem, bank0_base, WORDS_PER_BANK)
    bank1_lines = build_bank_mem(mem, bank1_base, WORDS_PER_BANK)

    bank0_nonzero = sum(1 for l in bank0_lines if l != "00000000")
    bank1_nonzero = sum(1 for l in bank1_lines if l != "00000000")

    with open(bank0_path, 'w') as f:
        f.write('\n'.join(bank0_lines) + '\n')
    with open(bank1_path, 'w') as f:
        f.write('\n'.join(bank1_lines) + '\n')

    print(f"  Bank 0 (IMEM): {bank0_nonzero}/{WORDS_PER_BANK} non-zero words → {bank0_path}")
    print(f"  Bank 1 (DMEM): {bank1_nonzero}/{WORDS_PER_BANK} non-zero words → {bank1_path}")


if __name__ == "__main__":
    main()

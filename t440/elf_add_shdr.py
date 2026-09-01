#!/usr/bin/env python3
# =============================================================================
# FASM_PLUS T440 — elf_add_shdr.py
# Append a minimal, valid ELF section-header table to a FASM-produced ELF32
# executable so that GRUB2's multiboot/multiboot2 loader accepts it.
#
# Why this exists
# ---------------
# FASM's `format elf executable` emits program headers (segments) but leaves
# e_shoff = 0 and e_shnum = 0 (no section headers). QEMU's `-kernel` multiboot
# loader accepts this, but GRUB2's ELF parser rejects it with:
#     error: invalid section header table offset in e_shoff.
# GRUB2 validates the section-header table offset while opening the ELF.
# Appending a two-entry table (null + .shstrtab) with e_shoff pointing at it
# satisfies the check without changing any program header or code byte.
#
# Usage:
#   elf_add_shdr.py <input.elf> <output.elf>
#
# The input is not modified; the patched image is written to <output.elf>.
# =============================================================================
import struct
import sys

def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: elf_add_shdr.py <in.elf> <out.elf>\n")
        sys.exit(2)
    data = bytearray(open(sys.argv[1], 'rb').read())

    # Sanity: ELF32 little-endian executable.
    if data[:4] != b'\x7fELF':
        sys.stderr.write("error: not an ELF file\n")
        sys.exit(1)
    if data[4] != 1:
        sys.stderr.write("error: not a 32-bit (ELFCLASS32) image\n")
        sys.exit(1)

    # Build the section-name string table: "\0.shstrtab\0".
    shstr = b'\x00.shstrtab\x00'

    # 4-byte-align the file, append the string table, align again.
    while len(data) % 4:
        data.append(0)
    shstr_off = len(data)
    data += shstr
    while len(data) % 4:
        data.append(0)
    shoff = len(data)

    # Two ELF32 section headers (40 bytes each): a mandatory NULL section and
    # the .shstrtab section holding the name strings.
    null_sh = bytes(40)
    shstr_sh = struct.pack(
        '<IIIIIIIIII',
        1,              # sh_name: offset of ".shstrtab" in the string table
        3,              # sh_type: SHT_STRTAB
        0, 0,           # sh_flags, sh_addr
        shstr_off,      # sh_offset
        len(shstr),     # sh_size
        0, 0,           # sh_link, sh_info
        1,              # sh_addralign
        0,              # sh_entsize
    )
    data += null_sh + shstr_sh

    # Patch the ELF header: e_shoff, e_shnum, e_shstrndx.
    struct.pack_into('<I', data, 32, shoff)   # e_shoff
    struct.pack_into('<H', data, 48, 2)       # e_shnum
    struct.pack_into('<H', data, 50, 1)       # e_shstrndx

    open(sys.argv[2], 'wb').write(bytes(data))
    print("elf_add_shdr: %s -> %s (e_shoff=%d, sections=2)" %
          (sys.argv[1], sys.argv[2], shoff))

if __name__ == '__main__':
    main()

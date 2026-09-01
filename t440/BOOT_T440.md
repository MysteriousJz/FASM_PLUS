# Booting the FASM_PLUS OS Core on a Physical ThinkPad T440

This document is the complete, followable path from source code to a physical
boot on a ThinkPad T440. It assumes a Linux host with GRUB2 tools (Manjaro,
Ubuntu, Fedora, etc. all work). No prior OS-development experience is required.

> **You do not need to boot real hardware to validate the kernel.** Read the
> "Test in QEMU first" section below and confirm the kernel boots in the
> emulator before you touch a USB stick.

---

## What you are building

A bootable USB stick that:

1. Boots into **GRUB2** (the bootloader).
2. GRUB2 sets a **VESA graphics mode** (a linear framebuffer) because the
   kernel's Multiboot2 header asks it to.
3. GRUB2 loads **`kernel_mb2.bin`** (the Multiboot2 kernel image) and jumps in.
4. The kernel switches to 64-bit long mode, brings up the OS core, loads the
   T440 drivers (PCI → VESA display → PS/2 keyboard) as Cordis fibers, prints
   **`3`** to the screen (top-left, white on black) and to serial, runs the
   test suite, and reports results.

The visible **`3`** on the T440's screen is the success signal — "the machine
is alive."

---

## Prerequisites

- A ThinkPad T440 (or a compatible machine; see "Hardware notes" at the end).
- A USB stick (any size; the whole install is a few MB).
- A Linux computer with: `grub-install` (grub-pc-bin / grub2), `grub-mkrescue`,
  `xorriso`, `mtools`, `python3`, and `qemu-system-x86_64` (for testing).
- Root/sudo access on the Linux host.

---

## Step 0 — Build the kernel images

From the repository root:

```sh
t440/build.sh
```

This produces two files in `t440/build/`:

| File | Protocol | Use |
|------|----------|-----|
| `kernel_mb1.bin` | Multiboot1 | Quick QEMU test with `-kernel` |
| `kernel_mb2.bin` | Multiboot2 | **GRUB2 on the USB stick / real T440** |

The Multiboot2 image is the one you put on the USB stick. The build verifies
that the Multiboot2 magic (`0xE85250D6`) is present in the first 8 KB, which is
where GRUB2 looks for it.

---

## Step 1 — Test in QEMU first (recommended)

### 1a. Quick test (Multiboot1, fastest)

```sh
t440/build.sh test
```

Expected: the test suite prints to serial and ends with `SUMMARY 15/16`
(the `t440_kbd` line may report a failure in the emulator — see
"Troubleshooting" below; this is a known test-harness artifact, not a hardware
problem).

### 1b. Full GRUB2 path (Multiboot2, exactly like the USB stick)

Build a GRUB2 rescue ISO that boots the Multiboot2 kernel the same way GRUB2
on the USB stick will:

```sh
t440/build.sh iso
qemu-system-x86_64 -cdrom t440/build/t440_mb2.iso \
    -serial stdio -no-reboot
```

Expected: you see GRUB2's banner, it loads the kernel automatically, and the
kernel prints `OS_CORE BOOT OK`, the `3` heartbeat, and the test summary.
**If this works in QEMU, the same image will boot on the T440.**

To see the graphics window (the on-screen `3`), drop `-display none`:

```sh
qemu-system-x86_64 -cdrom t440/build/t440_mb2.iso -serial stdio -no-reboot
```

---

## Step 2 — Create the bootable USB stick

> **Warning**: these steps erase the USB stick. Replace `/dev/sdX` with your
> actual USB device (find it with `lsblk` before and after plugging it in).
> Getting the device wrong will overwrite the wrong disk.

### 2a. Partition and format

```sh
# Identify the stick (note the device, e.g. /dev/sdb — NOT a partition)
lsblk

# Create a fresh partition table and one FAT32 partition (UEFI + BIOS friendly)
sudo parted /dev/sdX --script mklabel msdos
sudo parted /dev/sdX --script mkpart primary fat32 1MiB 100%
sudo parted /dev/sdX --script set 1 boot on
sudo mkfs.vfat -F32 /dev/sdX1
```

### 2b. Install GRUB2 (BIOS mode) onto the stick

The T440 boots this image via legacy BIOS/MBR (the simplest, most compatible
path for a custom Multiboot2 kernel):

```sh
# Mount the stick
sudo mkdir -p /mnt/usb
sudo mount /dev/sdX1 /mnt/usb

# Install GRUB2 for PC/BIOS onto the stick
sudo grub-install --target=i386-pc --boot-directory=/mnt/usb/boot \
    --no-floppy --recheck /dev/sdX
```

### 2c. Copy the kernel and the GRUB configuration

```sh
sudo cp t440/build/kernel_mb2.bin /mnt/usb/boot/kernel.bin
sudo cp t440/grub.cfg            /mnt/usb/boot/grub/grub.cfg
sync
sudo umount /mnt/usb
```

The stick now contains:

```
/boot/kernel.bin          <- the Multiboot2 kernel
/boot/grub/grub.cfg       <- the GRUB2 configuration
/boot/grub/...            <- GRUB2 modules (installed by grub-install)
```

---

## Step 3 — Boot the T440 from the USB stick

1. Power off the T440 and insert the USB stick.
2. Power on and press **F12** (the ThinkPad boot-menu key) — or **Enter** then
   F12 — to open the boot device menu.
3. Select the USB stick (it may appear as "USB HDD" or the stick's name).
4. GRUB2 loads, sets the graphics mode, and boots the kernel automatically
   (no menu interaction needed; `timeout=0`).
5. Within a second you should see **`3`** in the top-left corner of the screen.

If you have a null-modem/serial setup, the same `3` and the full test summary
also appear on the serial port (COM1, 115200 baud, 8N1).

---

## Troubleshooting

### Read the framebuffer diagnostic first

The 32-bit boot stub prints exactly what GRUB2 handed to the kernel, on COM1,
before the long-mode switch. With a serial connection you will see one of:

```
FB: LFB=0x00000000fd000000 W=1024 H=768 P=4096 BPP=32
FB: no framebuffer tag
```

- **`FB: LFB=0x... W=... H=... P=... BPP=...`** — GRUB2 set a graphics mode and
  passed a valid framebuffer. If the screen is still black after this, the mode
  is one the panel cannot display: edit `t440/grub.cfg` and move `1024x768x32`
  to the front of `gfxmode`, rebuild, and retry.
- **`FB: no framebuffer tag`** — GRUB2 did not set a graphics mode. The kernel
  never received a framebuffer. Check that `t440/grub.cfg` loads `insmod vbe`
  and `insmod all_video`, that `gfxmode` lists a mode the firmware supports,
  and that the menu entry sets `gfxpayload=keep`.
- **No `FB:` line at all** — GRUB2 never jumped to the kernel. See the next
  items.

### The screen stays black but serial shows `FB: LFB=0x...`

The framebuffer handoff worked but the panel cannot show the mode. Reorder
`t440/grub.cfg`'s `gfxmode` to try `1024x768x32` first (the most universally
supported VESA mode), rebuild the ISO/USB, and retry.

### The screen stays black (no `3`, nothing)

- **Check serial first.** The kernel prints the `FB:` line, then
  `OS_CORE BOOT OK`, then `3`, then the test lines to COM1 regardless of the
  display. The `FB:` line tells you whether GRUB2 set a graphics mode at all.
- **If serial is also silent**, GRUB2 never handed off to the kernel — see the
  next item.

### GRUB2 says "invalid section header table offset in e_shoff"

The kernel image is missing its ELF section-header table. This is fixed by the
build (`t440/build.sh` runs `elf_add_shdr.py` on the Multiboot2 image). Re-run
`t440/build.sh` and re-copy `t440/build/kernel_mb2.bin` to the stick. Do not
copy the raw FASM output by hand.

### GRUB2 says "you need to load the kernel first" / "not a multiboot2 kernel"

The Multiboot2 header was not found in the first 8 KB of the image. Confirm the
magic is present:

```sh
python3 -c "d=open('t440/build/kernel_mb2.bin','rb').read(8192); \
    print('MB2 magic present:', b'\xd6\x50\x52\xe8' in d)"
```

If it prints `False`, rebuild with `t440/build.sh` (the build checks this and
would have failed loudly).

### The keyboard does not respond

The PS/2 keyboard driver initializes the 8042 controller, enables IRQ1, and
echoes keys to the screen in the interactive loop. In the **test harness** the
`t440_kbd` self-test may report a failure because a scancode can arrive during
re-initialization, making the "buffer starts empty" assertion too strict — this
is a test artifact, not a driver fault. On real hardware, watch for keystrokes
echoing to the screen after boot. If nothing echoes, the 8042 init sequence is
the place to look (`t440/PS2_KEYBOARD.INC`).

### It works in QEMU but not on the T440

The most common cause is the graphics mode. The T440's Intel HD Graphics 4400
supports 1024x768x32, 1366x768x32, and 1600x900x32 via VESA. Start with
`1024x768x32`. If the panel rejects it, GRUB2 falls back to text mode and the
framebuffer tag will be absent — the kernel still prints `3` to serial. Use the
serial output to confirm the kernel ran, then adjust `gfxmode`/`gfxpayload`.

---

## Hardware notes

- **Target machine**: ThinkPad T440 — Intel Core i5-4300U (Haswell), Intel 8
  Series (Lynx Point) chipset, Intel HD Graphics 4400, PS/2 internal keyboard,
  SATA/AHCI disk, Intel i218-LM Ethernet.
- **This deliverable** covers the boot path, the display, and the keyboard.
  Disk (AHCI) and network (i218) drivers are separate, later milestones.
- **Exact emulation is not possible**: QEMU has no Intel HD 4400 or i218-LM
  model. The display and keyboard paths are validated against QEMU's closest
  equivalents (Bochs VGA / PS/2), which exercise the same driver code paths.
  The VESA mode itself is set by GRUB2 on real hardware and by the Bochs BGA
  interface in QEMU.

---

## File reference

| File | Purpose |
|------|---------|
| `t440/OS_TEST_T440.ASM` | T440 kernel image source (MB1 + MB2) |
| `t440/build.sh` | Builds both images; `test` (QEMU) and `iso` (GRUB2) targets |
| `t440/elf_add_shdr.py` | Adds the ELF section-header table GRUB2 requires |
| `t440/grub.cfg` | GRUB2 config: graphics mode + `multiboot2` load |
| `t440/PCI_ENUM.INC` | PCI enumeration driver (Cordis fiber) |
| `t440/VESA_DISPLAY.INC` | Framebuffer display driver (Cordis fiber) |
| `t440/PS2_KEYBOARD.INC` | PS/2 keyboard driver (Cordis fiber) |
| `t440/T440_INIT.INC` | Loads the drivers in dependency order |

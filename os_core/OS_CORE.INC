; =============================================================================
; FASM_PLUS OS Core — OS_CORE.INC
; Top-level include that pulls in every OS Core component in dependency order.
;
; Depends on: nothing (this IS the dependency root)
; Emits: macros and compile-time constants from the foundation layer; the
;        function bodies of the OS core land in whichever section is current
;        at each include site (OS_TEST.ASM arranges the sections).
;
; Dependency order (each layer may only use what is above it)
; -----------------------------------------------------------
;   foundation/CONSTANTS.INC         fundamental definitions (no deps)
;   foundation/CONTRACTS.INC         compile-time verification
;   foundation/STATE_MANAGEMENT.INC  register and stack discipline
;   foundation/BIT_MATH.INC          bit manipulation macros
;   foundation/CONTROL_FLOW.INC      structured control macros
;   SERIAL.INC                       16550 UART driver (pure I/O, no deps)
;   GDT.INC                          Global Descriptor Table
;   IDT.INC                          Interrupt Descriptor Table
;   PAGING.INC                       page table construction
;   EXCEPTIONS.INC                   exception handlers
;   CONTEXT.INC                      task context save/restore/switch
;   SYSCALL.INC                      SYSCALL/SYSRET interface
;   KERNEL_INIT.INC                  bootstrap sequence
;   KERNEL_TEST.INC                  test runner
;   BOOT_STUB.ASM                    Multiboot2 header + 32->64 switch (code)
;
; Every component carries its own include guard, so this file is also safe to
; include more than once. The guards below additionally verify at assembly
; time that each dependency was actually satisfied, so a missing or reordered
; include fails loudly instead of emitting a broken kernel.
; =============================================================================

if ~ definite FP_OS_CORE_INC
FP_OS_CORE_INC = 1

; -----------------------------------------------------------------------------
; Foundation layer (lives in ../foundation relative to this file). These five
; files are pure compile-time definition: they emit no code or data and are
; safe to pull in from any section context.
; -----------------------------------------------------------------------------
include '../foundation/CONSTANTS.INC'
include '../foundation/CONTRACTS.INC'
include '../foundation/STATE_MANAGEMENT.INC'
include '../foundation/BIT_MATH.INC'
include '../foundation/CONTROL_FLOW.INC'

; -----------------------------------------------------------------------------
; OS core layer. SERIAL comes first: every other component reports through
; it. GDT before IDT (the IDT entries carry the kernel code selector).
; CONTEXT after PAGING (contexts carry CR3). SYSCALL after CONTEXT and IDT.
; KERNEL_INIT after everything it bootstraps. KERNEL_TEST after init.
; BOOT_STUB last: it consumes the constants above but emits the entry code.
; -----------------------------------------------------------------------------
include 'SERIAL.INC'
include 'GDT.INC'
include 'IDT.INC'
include 'PAGING.INC'
include 'EXCEPTIONS.INC'
include 'CONTEXT.INC'
include 'SYSCALL.INC'
include 'KERNEL_INIT.INC'
include 'KERNEL_TEST.INC'
include 'BOOT_STUB.ASM'

; -----------------------------------------------------------------------------
; Post-include verification: if any guard constant is missing, an include was
; skipped or renamed. Fail the build here with a named error rather than
; emitting a kernel that halts silently in QEMU.
; -----------------------------------------------------------------------------
if ~ definite FP_CONSTANTS_INC
  err 'OS_CORE.INC: CONSTANTS.INC missing'
end if
if ~ definite FP_CONTRACTS_INC
  err 'OS_CORE.INC: CONTRACTS.INC missing'
end if
if ~ definite FP_STATE_MANAGEMENT_INC
  err 'OS_CORE.INC: STATE_MANAGEMENT.INC missing'
end if
if ~ definite FP_BIT_MATH_INC
  err 'OS_CORE.INC: BIT_MATH.INC missing'
end if
if ~ definite FP_CONTROL_FLOW_INC
  err 'OS_CORE.INC: CONTROL_FLOW.INC missing'
end if
if ~ definite FP_SERIAL_INC
  err 'OS_CORE.INC: SERIAL.INC missing'
end if
if ~ definite FP_GDT_INC
  err 'OS_CORE.INC: GDT.INC missing'
end if
if ~ definite FP_IDT_INC
  err 'OS_CORE.INC: IDT.INC missing'
end if
if ~ definite FP_PAGING_INC
  err 'OS_CORE.INC: PAGING.INC missing'
end if
if ~ definite FP_EXCEPTIONS_INC
  err 'OS_CORE.INC: EXCEPTIONS.INC missing'
end if
if ~ definite FP_CONTEXT_INC
  err 'OS_CORE.INC: CONTEXT.INC missing'
end if
if ~ definite FP_SYSCALL_INC
  err 'OS_CORE.INC: SYSCALL.INC missing'
end if
if ~ definite FP_KERNEL_INIT_INC
  err 'OS_CORE.INC: KERNEL_INIT.INC missing'
end if
if ~ definite FP_KERNEL_TEST_INC
  err 'OS_CORE.INC: KERNEL_TEST.INC missing'
end if
if ~ definite FP_BOOT_STUB_ASM
  err 'OS_CORE.INC: BOOT_STUB.ASM missing'
end if

end if ; FP_OS_CORE_INC

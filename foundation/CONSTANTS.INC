; =============================================================================
; FASM_PLUS Foundation Library — Layer 0
; CONSTANTS.INC — fundamental definitions
;
; Contract format: every public name in this file is a compile-time constant.
; This file has NO dependencies and emits NO code or data. It is pure
; compile-time definition, safe to include anywhere, any number of times.
;
; Philosophy: no magic numbers anywhere else in the library. If a value is
; used by more than one layer, it is named here exactly once.
; =============================================================================

; NOTE on include guards: this library uses `if ~ definite GUARD` (not
; `~defined`). `defined` permits forward references, which makes the `if`
; outcome differ between assembly passes and breaks FASM's output tracking;
; `definite` only considers symbols defined earlier in the source, giving
; deterministic single-evaluation guard semantics.
if ~ definite FP_CONSTANTS_INC
FP_CONSTANTS_INC = 1

; -----------------------------------------------------------------------------
; Fundamental sizes (bytes)
; -----------------------------------------------------------------------------
SIZE_BYTE      = 1
SIZE_WORD      = 2
SIZE_DWORD     = 4
SIZE_QWORD     = 8
SIZE_OWORD     = 16          ; SSE register width
SIZE_YMM       = 32          ; AVX register width
SIZE_ZMM       = 64          ; AVX-512 register width

BITS_PER_BYTE  = 8

; -----------------------------------------------------------------------------
; Alignment values (bytes). Must remain powers of two.
; -----------------------------------------------------------------------------
ALIGN_BYTE     = 1
ALIGN_WORD     = 2
ALIGN_DWORD    = 4
ALIGN_QWORD    = 8
ALIGN_SIMD     = 16          ; required for movaps/movdqa
ALIGN_STACK    = 16          ; System V AMD64 ABI stack alignment
ALIGN_CACHE    = 64          ; typical x86-64 cache line
ALIGN_PAGE     = 4096        ; x86-64 small page

; -----------------------------------------------------------------------------
; Hardware geometry
; -----------------------------------------------------------------------------
PAGE_SIZE      = 4096
PAGE_SHIFT     = 12
CACHE_LINE     = 64
CACHE_SHIFT    = 6

; -----------------------------------------------------------------------------
; Boolean values
; -----------------------------------------------------------------------------
FALSE          = 0
TRUE           = 1

; -----------------------------------------------------------------------------
; Null pointer
; -----------------------------------------------------------------------------
NULL           = 0

; -----------------------------------------------------------------------------
; Platform detection. Exactly one PLATFORM_* must be 1. This foundation
; currently targets x86-64 Linux; other platforms are named so call sites
; can be written portably and will fail loudly at assembly time if selected.
; -----------------------------------------------------------------------------
PLATFORM_LINUX   = 1
PLATFORM_WINDOWS = 0
PLATFORM_BARE    = 0

if PLATFORM_LINUX + PLATFORM_WINDOWS + PLATFORM_BARE <> 1
  err 'CONSTANTS.INC: exactly one PLATFORM_* must be selected'
end if

; CPU target width; the foundation is 64-bit only.
CPU_BITS       = 64

; -----------------------------------------------------------------------------
; System V AMD64 calling convention parameters.
; Volatile (caller-saved):   rax rcx rdx rsi rdi r8 r9 r10 r11
; Non-volatile (callee-saved): rbx rbp r12 r13 r14 r15 rsp
; Integer args:              rdi rsi rdx rcx r8 r9
; Syscall args:              rdi rsi rdx r10 r8 r9  (nr in rax)
; Return value:              rax (rdx:rax for 128-bit)
; Red zone:                  128 bytes below rsp usable by leaf functions
; -----------------------------------------------------------------------------
CC_ARG_COUNT   = 6
REDZONE_SIZE   = 128

; volatile register count (for state save sizing) — rax..r11 volatile set
NVREG_COUNT    = 6           ; rbx rbp r12 r13 r14 r15
VREG_COUNT     = 9           ; rax rcx rdx rsi rdi r8 r9 r10 r11

; -----------------------------------------------------------------------------
; Linux syscall numbers (x86-64). Subset used by the foundation layer.
; Names follow the kernel ABI; values are fixed by the kernel.
; -----------------------------------------------------------------------------
SYS_read            = 0
SYS_write           = 1
SYS_open            = 2
SYS_close           = 3
SYS_stat            = 4
SYS_lseek           = 8
SYS_mmap            = 9
SYS_mprotect        = 10
SYS_munmap          = 11
SYS_rt_sigaction    = 13
SYS_rt_sigprocmask  = 14
SYS_rt_sigreturn    = 15
SYS_ioctl           = 16
SYS_writev          = 20
SYS_nanosleep       = 35
SYS_getpid          = 39
SYS_exit            = 60
SYS_kill            = 62
SYS_gettimeofday    = 96
SYS_getrlimit       = 97
SYS_getrusage       = 108
SYS_clock_gettime   = 228
SYS_exit_group      = 231
SYS_openat          = 257
SYS_getrandom       = 318
SYS_rseq            = 334

; open(2) / openat(2) flags
O_RDONLY      = 0
O_WRONLY      = 1
O_RDWR        = 2
O_CREAT       = 0x40
O_TRUNC       = 0x200
O_APPEND      = 0x400

; lseek(2) whence
SEEK_SET      = 0
SEEK_CUR      = 1
SEEK_END      = 2

; mmap(2) protection flags
PROT_NONE     = 0
PROT_READ     = 1
PROT_WRITE    = 2
PROT_EXEC     = 4

; mmap(2) mapping flags
MAP_SHARED    = 0x01
MAP_PRIVATE   = 0x02
MAP_ANONYMOUS = 0x20
MAP_FIXED     = 0x10
MAP_GROWSDOWN = 0x100

; mmap kernel "fd" convention for anonymous maps
MMAP_ANON_FD  = -1

; -----------------------------------------------------------------------------
; Signals (x86-64 Linux values)
; -----------------------------------------------------------------------------
SIGHUP        = 1
SIGINT        = 2
SIGILL        = 4
SIGTRAP       = 5
SIGABRT       = 6
SIGBUS        = 7
SIGFPE        = 8
SIGKILL       = 9
SIGSEGV       = 11
SIGPIPE       = 13
SIGALRM       = 14
SIGTERM       = 15

; sigaction flags
SA_SIGINFO    = 0x00000004
SA_RESTORER   = 0x04000000
SA_RESTART    = 0x10000000

; siginfo_t si_codes of interest
SEGV_MAPERR   = 1            ; address not mapped
SEGV_ACCERR   = 2            ; invalid permissions
ILL_ILLOPN    = 2            ; illegal operand
BUS_ADRERR    = 2            ; non-existent physical address

; standard file descriptors
STDIN_FD      = 0
STDOUT_FD     = 1
STDERR_FD     = 2

; clock sources for clock_gettime
CLOCK_REALTIME  = 0
CLOCK_MONOTONIC = 1

; -----------------------------------------------------------------------------
; Error codes. The foundation uses small negative integers in rax for error
; returns, mirroring the kernel convention so syscall results pass through
; without translation. Library-private codes start at -1000 to avoid
; colliding with errno.
; -----------------------------------------------------------------------------
E_OK                = 0
E_GENERIC           = -1
E_INVALID_ARG       = -2
E_NULL_PTR          = -3
E_OUT_OF_MEMORY     = -4
E_OUT_OF_RANGE      = -5
E_ALIGNMENT         = -6
E_OVERFLOW          = -7
E_UNDERFLOW         = -8
E_NOT_FOUND         = -9
E_IO                = -10
E_PERMISSION        = -13      ; matches EPERM sign-extended usage pattern
E_ASSERTION_FAILED  = -1000
E_CONTRACT_VIOLATED = -1001
E_TEST_FAILED       = -1002
E_STACK_CORRUPT     = -1003    ; stack canary mismatch
E_GUARD_HIT         = -1004    ; guard page / region bound touched
E_TIMEOUT           = -1005
E_UNSUPPORTED       = -1006
E_BAD_STATE         = -1007

; errno values of interest (kernel returns -errno in rax)
ERRNO_EAGAIN      = 11
ERRNO_EFAULT      = 14
ERRNO_EINVAL      = 22
ERRNO_ENOMEM      = 12

; exit codes used by the bootstrap / test runner
EXIT_SUCCESS         = 0
EXIT_FAILURE         = 1
EXIT_TEST_FAILURE    = 2
EXIT_CRASH           = 3
EXIT_BOOTSTRAP_FAIL  = 4

; -----------------------------------------------------------------------------
; Stack canary constant. Fixed high-entropy pattern; a runtime-unique value
; would be stronger, but a fixed pattern still detects linear stack smashes
; and keeps the foundation deterministic (a design decision, see BOOTSTRAP_LOG).
; -----------------------------------------------------------------------------
STACK_CANARY_VALUE = 0x5AFE5AFE5AFE5AFE

; -----------------------------------------------------------------------------
; Debug / serial I/O ports (bare-metal style COM1 output, used only when the
; user opts in; harmless on Linux because we never out to them by default).
; -----------------------------------------------------------------------------
COM1_PORT       = 0x3F8
DEBUG_PORT_QEMU = 0xE9       ; qemu debugcon

; -----------------------------------------------------------------------------
; Common bit masks
; -----------------------------------------------------------------------------
MASK_LOW_NIBBLE  = 0x0F
MASK_LOW_BYTE    = 0xFF
MASK_LOW_WORD    = 0xFFFF
MASK_LOW_DWORD   = 0xFFFFFFFF
MASK_PAGE        = PAGE_SIZE - 1          ; offset within a page
MASK_PAGE_ADDR   = not MASK_PAGE          ; page-aligned address part

; hex digits, useful for io formatting
HEX_ALPHABET_SIZE = 16

end if ; FP_CONSTANTS_INC

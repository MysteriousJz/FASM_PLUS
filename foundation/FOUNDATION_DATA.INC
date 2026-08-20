; =============================================================================
; FASM_PLUS Foundation Library — data segment closer
; FOUNDATION_DATA.INC — all writeable/read-only data for the whole library
;
; Include this file EXACTLY ONCE, as the LAST foundation include, at a point
; where a data segment is appropriate. It opens its own data segment, so it
; is safe in flat executables. Following the HeavyThing convention, all
; library state lives here so the code includes never switch segments on
; the caller.
;
; Depends on: every other foundation layer (it sizes state for all of them).
; =============================================================================

if ~ definite FP_FOUNDATION_DATA_INC
FP_FOUNDATION_DATA_INC = 1

; writeable data segment (flat-executable style; also valid in objects)
segment readable writeable

; --- MEMORY_PRIMITIVES bump allocator state ---
align 16
fp$alloc_base	dq 0		; region base
fp$alloc_ptr	dq 0		; next free byte (16-aligned invariant)
fp$alloc_end	dq 0		; one past last usable byte

; --- TEST_FRAMEWORK state (see TEST_FRAMEWORK.INC for layout) ---
align 16
fp$test_count	dq 0		; number of registered tests
fp$test_failed	dq 0		; number of failed tests
fp$test_current	dq 0		; index of the test currently running
fp$test_halt	dq 0		; non-zero => stop after current test
fp$assert_line	dq 0		; source line of the failing assertion

; --- TEST_FRAMEWORK report strings (read-only content in the data segment;
;     the flat executable maps it readable, so cstr reads are fine) ---
fp$str_fail	db 'FAIL ',0
fp$str_line	db ' line ',0
fp$str_tests	db 'tests: ',0
fp$str_failed	db ' failed: ',0

; --- BOOTSTRAP state ---
align 16
fp$boot_flags	dq 0		; bit0 = initialized; bit1 = crash handler armed
fp$boot_stack_top dq 0		; recorded rsp at init for overflow checks
fp$cpu_features	dq 0		; CPUID-derived feature bits (see BOOTSTRAP.INC)

; --- CRASH_DETECTOR state: captured fault context (see CRASH_DETECTOR.INC) ---
align 16
fp$crash_rip	dq 0		; faulting instruction pointer
fp$crash_addr	dq 0		; faulting memory address (siginfo si_addr)
fp$crash_signal	dq 0		; signal number that was caught
fp$crash_code	dq 0		; si_code (e.g. SEGV_MAPERR)
fp$crash_rsp	dq 0		; stack pointer at fault
fp$crash_regs	rb 16*8		; rax rbx rcx rdx rsi rdi rbp rsp r8-r15 snapshot

; --- BOOTSTRAP report / crash strings ---
fp$str_crash	db 'CRASH sig=',0
fp$str_rip	db ' rip=0x',0
fp$str_addr	db ' addr=0x',0

end if ; FP_FOUNDATION_DATA_INC

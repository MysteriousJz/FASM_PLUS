; =============================================================================
; FASM_PLUS Foundation Library — TEST_RUNNER.ASM
; Discovers (explicitly registers) and runs every smoke test, then reports a
; summary and exits with a status encoding the outcome.
;
; Build (from the repository root):
;   fasm/fasm.x64 foundation/TEST_RUNNER.ASM test_runner
; Run:
;   ./test_runner            # prints per-test failures + a summary
;   echo $?                  # 0 all pass, 2 some failed, 3 crash, 4 boot fail
;
; Registration is explicit and ordered. Add a new test by writing its
; TEST_DEF in SMOKE_TESTS.ASM, its name cstr in SMOKE_DATA.INC, and one
; REGISTER_TEST line in the registry below.
; =============================================================================

format elf64 executable
entry _start

; library layers, bottom-up
include 'CONSTANTS.INC'
include 'CONTRACTS.INC'
include 'STATE_MANAGEMENT.INC'
include 'MEMORY_PRIMITIVES.INC'
include 'CONTROL_FLOW.INC'
include 'IO_BASIC.INC'
include 'TEST_FRAMEWORK.INC'
include 'BOOTSTRAP.INC'
include 'CRASH_DETECTOR.INC'

; the tests under verification
include 'SMOKE_TESTS.ASM'

segment readable executable
_start:
	; bootstrap: align stack, verify CPU, record stack top
	call	fp$init
	cmp	rax, E_OK
	jne	.boot_fail

	; arm the crash detector so a faulting test is reported, not silent
	call	fp$crash_install
	cmp	rax, E_OK
	jne	.boot_fail

	; announce
	lea	rsi, [run$banner]
	call	fp$print_cstr

	; run the registry; returns the failure count in rax
	call	fp$test_run_all
	mov	rbx, rax			; save failure count

	; exit code: 0 if none failed, EXIT_TEST_FAILURE otherwise
	test	rbx, rbx
	jz	.all_pass
	mov	edi, EXIT_TEST_FAILURE
	call	fp$exit
.all_pass:
	mov	edi, EXIT_SUCCESS
	call	fp$exit

.boot_fail:
	lea	rsi, [run$bootfail]
	call	fp$print_cstr
	mov	edi, EXIT_BOOTSTRAP_FAIL
	call	fp$exit

; --- runner-local read-only strings ---
segment readable writeable
run$banner	db 'FASM_PLUS foundation test run',10,0
run$bootfail	db 'bootstrap failed',10,0

; =============================================================================
; Test registry: one REGISTER_TEST per smoke test, in dependency order.
; FP_TEST_TOTAL is computed from the table extent so it cannot drift.
; =============================================================================
align 16
fp$test_registry_begin:
	REGISTER_TEST tn$constants_sane,		test$constants_sane
	REGISTER_TEST tn$constants_power_of_two,	test$constants_power_of_two
	REGISTER_TEST tn$push_pop_volatile,		test$push_pop_volatile
	REGISTER_TEST tn$push_pop_nonvolatile,		test$push_pop_nonvolatile
	REGISTER_TEST tn$frame_enter_leave,		test$frame_enter_leave
	REGISTER_TEST tn$stack_canary_intact,		test$stack_canary_intact
	REGISTER_TEST tn$stack_align_force,		test$stack_align_force
	REGISTER_TEST tn$mem_zero_works,		test$mem_zero_works
	REGISTER_TEST tn$mem_copy_roundtrip,		test$mem_copy_roundtrip
	REGISTER_TEST tn$mem_compare_detects,		test$mem_compare_detects
	REGISTER_TEST tn$mem_move_overlap,		test$mem_move_overlap
	REGISTER_TEST tn$alloc_basic,			test$alloc_basic
	REGISTER_TEST tn$alloc_rewind_reuses,		test$alloc_rewind_reuses
	REGISTER_TEST tn$alloc_exhaustion_detected,	test$alloc_exhaustion_detected
	REGISTER_TEST tn$for_loop_sums,			test$for_loop_sums
	REGISTER_TEST tn$for_loop_break,		test$for_loop_break
	REGISTER_TEST tn$while_loop_counts,		test$while_loop_counts
	REGISTER_TEST tn$repeat_until,			test$repeat_until
	REGISTER_TEST tn$guard_return_zero,		test$guard_return_zero
	REGISTER_TEST tn$print_uint_returns_count,	test$print_uint_returns_count
	REGISTER_TEST tn$print_hex_width,		test$print_hex_width
	REGISTER_TEST tn$print_int_negative,		test$print_int_negative
	REGISTER_TEST tn$file_roundtrip,		test$file_roundtrip
fp$test_registry_end:
FP_TEST_TOTAL = (fp$test_registry_end - fp$test_registry_begin) / FP_TEST_ENTRY_SIZE

; library data + smoke fixtures
include 'FOUNDATION_DATA.INC'
include 'SMOKE_DATA.INC'

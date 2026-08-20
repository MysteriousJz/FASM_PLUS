; =============================================================================
; FASM_PLUS Foundation Library — SMOKE_DATA.INC
; Buffers and fixtures used by SMOKE_TESTS.ASM. Kept separate so the test
; code stays readable and the data layout is explicit.
; =============================================================================

if ~ definite FP_SMOKE_DATA_INC
FP_SMOKE_DATA_INC = 1

segment readable writeable

align 16
sm$buf		rb 64
sm$zeros	rb 64			; stays all-zero (bss-like)
sm$copydst	rb 64
sm$copysrc	db '0123456789abcdef0123456789abcdef'	; 32 bytes
sm$cmpA		db 'AAAAAAA1'
sm$cmpB		db 'AAAAAAA2'
sm$mvbuf	db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
		db 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
sm$mvexpect	db 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
		db 16,17,18,19,20,21,22,23
sm$arena	rb 256
sm$readback	rb 8
sm$filedata	db 'abcde'
sm$tmpname	db '/tmp/fp_smoke_test.bin',0

; test display names (read-only)
align 8
tn$constants_sane		db 'constants_sane',0
tn$constants_power_of_two	db 'constants_power_of_two',0
tn$push_pop_volatile		db 'push_pop_volatile',0
tn$push_pop_nonvolatile		db 'push_pop_nonvolatile',0
tn$frame_enter_leave		db 'frame_enter_leave',0
tn$stack_canary_intact		db 'stack_canary_intact',0
tn$stack_align_force		db 'stack_align_force',0
tn$mem_zero_works		db 'mem_zero_works',0
tn$mem_copy_roundtrip		db 'mem_copy_roundtrip',0
tn$mem_compare_detects		db 'mem_compare_detects',0
tn$mem_move_overlap		db 'mem_move_overlap',0
tn$alloc_basic			db 'alloc_basic',0
tn$alloc_rewind_reuses		db 'alloc_rewind_reuses',0
tn$alloc_exhaustion_detected	db 'alloc_exhaustion_detected',0
tn$for_loop_sums		db 'for_loop_sums',0
tn$for_loop_break		db 'for_loop_break',0
tn$while_loop_counts		db 'while_loop_counts',0
tn$repeat_until			db 'repeat_until',0
tn$guard_return_zero		db 'guard_return_zero',0
tn$print_uint_returns_count	db 'print_uint_returns_count',0
tn$print_hex_width		db 'print_hex_width',0
tn$print_int_negative		db 'print_int_negative',0
tn$file_roundtrip		db 'file_roundtrip',0

end if ; FP_SMOKE_DATA_INC

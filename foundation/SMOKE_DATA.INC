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

; --- Kind Machine extension fixtures (Layers 2.5/4.5/5/5.5) ---
; token definitions used by the token and lexer tests
align 8
DEFINE_TOKEN TOKEN_X86_MOV, ISA_X86 + 1
DEFINE_TOKEN TOKEN_ELF, FILE_MAGIC + 0
DEFINE_TOKEN TOKEN_PNG, FILE_MAGIC + 1
TOKEN_IS_UNIQUE TOKEN_X86_MOV
TOKEN_IS_UNIQUE TOKEN_ELF
TOKEN_IS_UNIQUE TOKEN_PNG

; the smoke lexer table: ELF and PNG magics, audited at assembly time
align 16
sm$lexer_table:
	LEXER_ENTRY TOKEN_ELF, 0x7F, 0x45, 0x4C, 0x46	; "\x7FELF"
	LEXER_ENTRY TOKEN_PNG, 0x89, 0x50, 0x4E, 0x47	; "\x89PNG"
	LEXER_SENTINEL
sm$lexer_table_end:
LEXER_TABLE_AUDIT sm$lexer_table, sm$lexer_table_end

; sparse row + attention fixtures
align 16
SPARSE_ROW sm$row, 8
sm$vsum	dq 0
sm$isum	dq 0
; V matrix: 3 rows of 2 dims
sm$V	dq 10,11, 20,21, 30,31
sm$out	dq 0,0
; one score row for the pruning mock
sm$scores dq 5, 1, 5, 2
; premix outputs
sm$pm_o11 dq 0
sm$pm_o12 dq 0
sm$pm_o21 dq 0
sm$pm_o22 dq 0

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
tn$popcount_basic		db 'popcount_basic',0
tn$count_leading_zeros		db 'count_leading_zeros',0
tn$count_trailing_zeros		db 'count_trailing_zeros',0
tn$bitmath_min_max		db 'bitmath_min_max',0
tn$tokens_in_ranges		db 'tokens_in_ranges',0
tn$tokens_no_collision		db 'tokens_no_collision',0
tn$lexer_init_clears		db 'lexer_init_clears',0
tn$lexer_elf_header		db 'lexer_elf_header',0
tn$lexer_raw_passthrough	db 'lexer_raw_passthrough',0
tn$lexer_partial_flush		db 'lexer_partial_flush',0
tn$lexer_suffix_demotion	db 'lexer_suffix_demotion',0
tn$fib_ladder_monotonic		db 'fib_ladder_monotonic',0
tn$fib_threshold_named		db 'fib_threshold_named',0
tn$closecheck_equal		db 'closecheck_equal',0
tn$closecheck_zero		db 'closecheck_zero',0
tn$closecheck_sign_mismatch	db 'closecheck_sign_mismatch',0
tn$closecheck_known_ratio	db 'closecheck_known_ratio',0
tn$closecheck_out_of_range	db 'closecheck_out_of_range',0
tn$mask_bit_ops			db 'mask_bit_ops',0
tn$mask_iterate_sum		db 'mask_iterate_sum',0
tn$sparse_row_append_iterate	db 'sparse_row_append_iterate',0
tn$premix_2x2			db 'premix_2x2',0
tn$weight_apply			db 'weight_apply',0
tn$attention_prune_mock		db 'attention_prune_mock',0

end if ; FP_SMOKE_DATA_INC

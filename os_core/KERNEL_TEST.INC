; =============================================================================
; FASM_PLUS OS Core — KERNEL_TEST.INC
; Test runner: runs integration tests in QEMU and reports over serial.
;
; Depends on: SERIAL.INC, KERNEL_INIT.INC
; Emits: functions in the current code section; pass/fail counters in the
;        current data area.
;
; Model
; -----
; Tests are leaf functions `fp$test_<name>` that return rax = 0 on pass or a
; non-zero reason code on fail. The runner prints "TEST <name>...", calls the
; test, prints "PASS" or "FAIL <reason>", and keeps a running count. At the
; end it writes the summary and exits QEMU via the isa-debug-exit device:
;   port 0xF4 <- 0  if every test passed
;   port 0xF4 <- 1  if any test failed
; The (value << 1) | 1 transformation QEMU applies means 0 written -> exit 1
; and 1 written -> exit 3; the wrapper script decodes these back to 0/1.
;
; Philosophy
; ----------
; Tests are deterministic, isolated, and self-reporting. A test never depends
; on another test's side effects beyond the boot state KERNEL_INIT provides.
; =============================================================================

if ~ definite FP_KERNEL_TEST_INC
FP_KERNEL_TEST_INC = 1

if ~ definite FP_KERNEL_INIT_INC
  err 'KERNEL_TEST.INC requires KERNEL_INIT.INC (and SERIAL.INC) first'
end if

; QEMU isa-debug-exit port and the two result codes we write to it
QEMU_EXIT_PORT  = 0xF4
QEMU_EXIT_PASS  = 0x00          ; -> QEMU exit code (0<<1)|1 = 1
QEMU_EXIT_FAIL  = 0x01          ; -> QEMU exit code (1<<1)|1 = 3

; =============================================================================
; fp$test_report_name
;   CONTRACT
;     IN        : rdi = test name (null-terminated string)
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;   Prints "TEST <name>..." with no newline (the result follows on the line).
; =============================================================================
align 16
fp$test_report_name:
	push	rdi
	mov	rdi, fp$test_prefix
	call	fp$serial_write
	pop	rdi
	call	fp$serial_write
	mov	rdi, fp$test_dots
	call	fp$serial_write
	ret

; =============================================================================
; fp$test_pass
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;   Prints "PASS" and increments the pass counter.
; =============================================================================
align 16
fp$test_pass:
	mov	rdi, fp$test_pass_str
	call	fp$serial_write
	inc	dword [fp$test_pass_count]
	ret

; =============================================================================
; fp$test_fail
;   CONTRACT
;     IN        : rdi = reason code (integer)
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;   Prints "FAIL <reason>" and increments the fail counter.
; =============================================================================
align 16
fp$test_fail:
	push	rdi
	mov	rdi, fp$test_fail_str
	call	fp$serial_write
	pop	rdi
	call	fp$serial_write_dec
	mov	rdi, fp$test_nl_str
	call	fp$serial_write
	inc	dword [fp$test_fail_count]
	ret

; =============================================================================
; fp$test_summary_and_exit
;   CONTRACT
;     IN        : nothing
;     OUT       : never returns (writes the QEMU exit code and halts)
;     CLOBBERS  : all
;   Prints the pass/fail summary line, writes the result code to the QEMU
;   isa-debug-exit port, and halts. This is the end of the test run.
; =============================================================================
align 16
fp$test_summary_and_exit:
	mov	rdi, fp$test_summary_str
	call	fp$serial_write
	mov	edi, [fp$test_pass_count]
	call	fp$serial_write_dec
	mov	rdi, fp$test_slash_str
	call	fp$serial_write
	mov	edi, [fp$test_pass_count]
	add	edi, [fp$test_fail_count]
	call	fp$serial_write_dec
	mov	rdi, fp$test_nl_str
	call	fp$serial_write
	; choose the exit code: 0 iff no failures
	mov	eax, QEMU_EXIT_PASS
	cmp	dword [fp$test_fail_count], 0
	je	.write
	mov	eax, QEMU_EXIT_FAIL
      .write:
	mov	rdx, QEMU_EXIT_PORT
	out	dx, al
      .halt:
	cli
	hlt
	jmp	.halt

; =============================================================================
; RUN_TEST name_string, test_function
;   Inline test invocation: prints the name, calls the function, and reports
;   PASS (rax = 0) or FAIL (rax = reason). Keeps the runner loop readable.
;   CONTRACT
;     IN        : name_string = cstr label; test_function = code label
;     OUT       : counters updated
;     CLOBBERS  : rax, rcx, rdx, rdi, rsi, flags
; =============================================================================
macro RUN_TEST name_string*, test_function*
{
	local fail, done
	mov	rdi, name_string
	call	fp$test_report_name
	call	test_function
	or	rax, rax
	jnz	fail
	call	fp$test_pass
	jmp	done
      fail:
	mov	rdi, rax
	call	fp$test_fail
      done:
}

; -----------------------------------------------------------------------------
; Data: report strings and the pass/fail counters.
; -----------------------------------------------------------------------------
fp$test_prefix	db 'TEST ', 0
fp$test_dots	db '... ', 0
fp$test_pass_str db 'PASS', 10, 0
fp$test_fail_str db 'FAIL ', 0
fp$test_nl_str	db 10, 0
fp$test_summary_str db 'SUMMARY ', 0
fp$test_slash_str db '/', 0

align 4
fp$test_pass_count dd 0
fp$test_fail_count dd 0

end if ; FP_KERNEL_TEST_INC

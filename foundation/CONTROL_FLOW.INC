; =============================================================================
; FASM_PLUS Foundation Library — Layer 3
; CONTROL_FLOW.INC — structured control-flow macros
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC
; Emits: code where invoked; no data.
;
; These macros give assembly a structured surface while keeping the generated
; instructions completely transparent — each expands to the obvious cmp/jcc
; sequence and nothing more. Loops take an explicit caller-supplied local
; label so nesting is unambiguous and each loop's helper labels derive from
; it (lbl and lbl#_done). Supply a distinct label per loop within a scope.
; =============================================================================

if ~ definite FP_CONTROL_FLOW_INC
FP_CONTROL_FLOW_INC = 1

if ~ definite FP_CONTRACTS_INC
  err 'CONTROL_FLOW.INC requires CONSTANTS.INC and CONTRACTS.INC first'
end if

; -----------------------------------------------------------------------------
; FOR reg, start, limit, step, lbl   ... body ...   NEXT reg, step, lbl
;
;   C-style counted loop: for (reg = start; reg < limit; reg += step) body.
;   Signed comparison. The body may use BREAK/CONTINUE with the same lbl.
;
;   CONTRACT FOR
;     IN        : reg = writable 64-bit counter register
;                 start, limit, step = immediates or registers
;                 lbl = a local label name unique within this scope
;     OUT       : executes body with reg from start while reg < limit
;     CLOBBERS  : reg, flags
;     PRESERVES : start/limit/step operands when they are registers != reg
;     ERRORS    : none; empty or reversed range runs the body zero times
;   CONTRACT NEXT
;     IN        : same reg, step, lbl as the matching FOR
;     OUT       : increments reg and branches back to the loop test
;     CLOBBERS  : reg, flags
; -----------------------------------------------------------------------------
macro FOR reg*, start*, limit*, step*, lbl*
{
	mov	reg, start
  lbl:
	cmp	reg, limit
	jge	lbl#_done
}
macro NEXT reg*, step*, lbl*
{
	add	reg, step
	jmp	lbl
  lbl#_done:
}

; -----------------------------------------------------------------------------
; BREAK lbl / CONTINUE reg, step, lbl
;   BREAK exits the labelled loop; CONTINUE advances the counter and jumps
;   back to the loop test (so the increment is applied exactly once).
;
;   CONTRACT
;     IN        : lbl of the enclosing loop; CONTINUE also needs that loop's
;                 reg and step
;     CLOBBERS  : CONTINUE clobbers reg and flags; BREAK clobbers neither
; -----------------------------------------------------------------------------
macro BREAK lbl*
{
	jmp	lbl#_done
}
macro CONTINUE reg*, step*, lbl*
{
	add	reg, step
	jmp	lbl
}

; -----------------------------------------------------------------------------
; WHILE reg, limit, lbl  ... body ...  WEND lbl
;   Pre-test loop: while (reg < limit) body. Signed. The body must make
;   progress (modify reg or BREAK) — termination is the body's contract.
; -----------------------------------------------------------------------------
macro WHILE reg*, limit*, lbl*
{
  lbl:
	cmp	reg, limit
	jge	lbl#_done
}
macro WEND lbl*
{
	jmp	lbl
  lbl#_done:
}

; -----------------------------------------------------------------------------
; REPEAT lbl  ... body ...  UNTIL reg, value, lbl
;   Post-test (do..while): body runs at least once, repeats while reg != value.
; -----------------------------------------------------------------------------
macro REPEAT lbl*
{
  lbl:
}
macro UNTIL reg*, value*, lbl*
{
	cmp	reg, value
	jne	lbl
}

; -----------------------------------------------------------------------------
; Guarded early returns. Each emits a single conditional jump to a function
; epilogue label after the caller has set flags with cmp/test. Keeping the
; flag-setting in the caller makes the condition explicit and peelable.
; Pair with a single exit point so cleanup is written once.
;
;   CONTRACT (all four)
;     IN        : target = epilogue label; flags already set by caller
;     OUT       : jumps to target when the named condition holds
;     CLOBBERS  : none
; -----------------------------------------------------------------------------
macro RETURN_IF_ZERO target*
{
	jz	target
}
macro RETURN_IF_NOT_ZERO target*
{
	jnz	target
}
macro RETURN_IF_NEGATIVE target*
{
	js	target
}
macro RETURN_IF_CARRY target*
{
	jc	target
}

; -----------------------------------------------------------------------------
; Scope-based cleanup. The body ends with CLEANUP_TO <lbl>; the cleanup code
; at <lbl> is then reached both by normal completion and by any guard that
; jumps to <lbl> directly. This guarantees the release path runs once.
;
;   CONTRACT CLEANUP_TO
;     IN        : cleanup_label = the label of the cleanup block
;     OUT       : control transfers to cleanup_label
;     CLOBBERS  : none
; -----------------------------------------------------------------------------
macro CLEANUP_TO cleanup_label*
{
	jmp	cleanup_label
}

end if ; FP_CONTROL_FLOW_INC

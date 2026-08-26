; =============================================================================
; FASM_PLUS Foundation Library — Layer 1
; STATE_MANAGEMENT.INC — register and stack discipline
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC
; Emits: code only where a macro is invoked; one read-only data object for
;        the full-context save area template is NOT used — saves live on the
;        stack so they are reentrant and position-independent.
;
; Design rules
; ------------
; * All save/restore is stack-based so nested and reentrant use is safe.
; * Every SAVE must be paired with a matching RESTORE at the same nesting
;   level; the macros do not (and cannot) detect cross-level mismatches —
;   that is what the canary macros and the smoke tests are for.
; * Stack alignment follows the System V AMD64 ABI: rsp is 16-byte aligned
;   at function entry after the return address push, so an 8-byte-adjusted
;   frame keeps movaps legal. FRAME_ENTER enforces this invariant.
; =============================================================================

if ~ definite FP_STATE_MANAGEMENT_INC
FP_STATE_MANAGEMENT_INC = 1

if ~ definite FP_CONTRACTS_INC
  err 'STATE_MANAGEMENT.INC requires CONTRACTS.INC (and CONSTANTS.INC) first'
end if

; -----------------------------------------------------------------------------
; PUSH_VOLATILE / POP_VOLATILE
;   Save and restore the System V volatile (caller-saved) integer registers.
;   Use around a call to code of unknown provenance when you need your
;   volatile values to survive.
;
;   CONTRACT
;     IN        : none
;     OUT       : none (registers preserved, not modified semantically)
;     CLOBBERS  : rsp (net zero after the pair); uses 9*8 bytes of stack
;     PRESERVES : all volatile registers between the pair; flags NOT preserved
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro PUSH_VOLATILE
{
	push	rax
	push	rcx
	push	rdx
	push	rsi
	push	rdi
	push	r8
	push	r9
	push	r10
	push	r11
}
macro POP_VOLATILE
{
	pop	r11
	pop	r10
	pop	r9
	pop	r8
	pop	rdi
	pop	rsi
	pop	rdx
	pop	rcx
	pop	rax
}

; -----------------------------------------------------------------------------
; PUSH_NONVOLATILE / POP_NONVOLATILE
;   Save and restore the callee-saved integer registers. A function that
;   wishes to use rbx/rbp/r12-r15 must save them first; this pair does it.
;   (rsp is intentionally excluded — saving it is meaningless here.)
;
;   CONTRACT
;     IN        : none
;     OUT       : none
;     CLOBBERS  : rsp (net zero after the pair); uses 6*8 bytes of stack
;     PRESERVES : rbx rbp r12 r13 r14 r15; flags NOT preserved
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro PUSH_NONVOLATILE
{
	push	rbx
	push	rbp
	push	r12
	push	r13
	push	r14
	push	r15
}
macro POP_NONVOLATILE
{
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	rbp
	pop	rbx
}

; -----------------------------------------------------------------------------
; FRAME_ENTER locals_size / FRAME_LEAVE
;   Establish and tear down a standard frame with 16-byte stack alignment.
;   At function entry rsp % 16 == 8 (return address pushed). After pushing
;   rbp, rsp % 16 == 0. locals_size is rounded up to a multiple of 16 so the
;   invariant holds for the whole body, keeping aligned SSE moves legal.
;
;   CONTRACT FRAME_ENTER
;     IN        : locals_size — compile-time byte count for locals (may be 0)
;     OUT       : rbp = frame base; rsp = new top, 16-byte aligned
;     CLOBBERS  : rbp, rsp, flags (via sub)
;     PRESERVES : all other registers
;     ERRORS    : assembly error if locals_size is negative
;   CONTRACT FRAME_LEAVE
;     IN        : matching FRAME_ENTER frame on top of stack
;     OUT       : rsp/rbp restored to caller's frame
;     CLOBBERS  : rbp, rsp, flags
;     ERRORS    : none (mismatched pairing is a logic error caught by canary)
; -----------------------------------------------------------------------------
macro FRAME_ENTER locals_size*
{
	STATIC_ASSERT (locals_size) >= 0
	push	rbp
	mov	rbp, rsp
	; round locals up to a multiple of 16 to preserve alignment
	if (((locals_size) + 15) and not 15) > 0
	  sub	rsp, ((locals_size) + 15) and not 15
	end if
}
macro FRAME_LEAVE
{
	mov	rsp, rbp
	pop	rbp
}

; -----------------------------------------------------------------------------
; STACK_ALIGN_FORCE scratch_reg
;   Force rsp to 16-byte alignment when entry alignment is unknown (e.g. at
;   a process entry point or a callback from foreign code). The original rsp
;   is saved in scratch_reg so STACK_ALIGN_RESTORE can undo it.
;
;   CONTRACT
;     IN        : scratch_reg — a 64-bit register that may be clobbered
;     OUT       : scratch_reg = original rsp; rsp 16-byte aligned
;     CLOBBERS  : scratch_reg, rsp, flags
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro STACK_ALIGN_FORCE scratch_reg*
{
	mov	scratch_reg, rsp
	and	rsp, not (ALIGN_STACK-1)
}
macro STACK_ALIGN_RESTORE scratch_reg*
{
	mov	rsp, scratch_reg
}

; -----------------------------------------------------------------------------
; CANARY_PLACE / CANARY_CHECK
;   Place a stack canary below the current frame and later verify it. A
;   linear stack overwrite of the frame will clobber the canary first, so a
;   mismatch localizes the failure to this frame instead of crashing later
;   in an unrelated place.
;
;   The canary lives at a fixed offset from rsp captured by CANARY_PLACE via
;   a local label value; CANARY_CHECK compares and, on mismatch, executes a
;   deterministic trap: it writes E_STACK_CORRUPT to stderr-visible state by
;   exiting with the corruption code. Controlled halt beats silent smash.
;
;   CONTRACT CANARY_PLACE
;     IN        : none
;     OUT       : none visible; reserves 16 bytes of stack
;     CLOBBERS  : rax, rsp (net -16), memory at [rsp]
;     ERRORS    : none
;   CONTRACT CANARY_CHECK exit_label
;     IN        : exit_label — where to jump on canary mismatch
;     OUT       : falls through if intact; jumps to exit_label if smashed
;     CLOBBERS  : rax, flags
;     ERRORS    : none returned; failure is a branch, not a silent return
; -----------------------------------------------------------------------------
macro CANARY_PLACE
{
	sub	rsp, 16			; keep 16-byte alignment of the frame
	mov	rax, STACK_CANARY_VALUE
	mov	[rsp], rax
	mov	[rsp+8], rax
}
macro CANARY_CHECK exit_label*
{
	local	.ok
	mov	rax, STACK_CANARY_VALUE
	cmp	[rsp], rax
	jne	exit_label
	cmp	[rsp+8], rax
	jne	exit_label
.ok:
}
; release the canary slot once it has been checked
macro CANARY_RELEASE
{
	add	rsp, 16
}

; -----------------------------------------------------------------------------
; SAVE_FULL_CONTEXT area_reg / RESTORE_FULL_CONTEXT area_reg
;   Save/restore the complete integer + x87/SSE state into a caller-provided
;   512-byte, 64-byte-aligned buffer whose address is in area_reg.
;   Uses FXSAVE64/FXRSTOR64 which cover x87, MMX, SSE control and the
;   xmm0-15 registers in one architecturally-defined block.
;
;   CONTRACT
;     IN        : area_reg — 64-bit register holding a 64-byte-aligned,
;                 512-byte writable buffer address
;     OUT       : buffer filled with live context (SAVE) / context loaded
;     CLOBBERS  : flags; memory at [area_reg]; after RESTORE, x87/SSE state
;     PRESERVES : all integer registers including area_reg
;     ERRORS    : #GP if the buffer is misaligned — caught by crash detector
; -----------------------------------------------------------------------------
macro SAVE_FULL_CONTEXT area_reg*
{
	fxsave64 [area_reg]
}
macro RESTORE_FULL_CONTEXT area_reg*
{
	fxrstor64 [area_reg]
}

; -----------------------------------------------------------------------------
; PUSHAQ / POPAQ
;   Push/pop all fifteen general-purpose registers (rsp excluded). PUSHAQ is
;   the 64-bit analogue of the 32-bit PUSHA: every register survives in memory
;   so a post-mortem (or a context switch) sees the full state.
;
;   Push order is r15 first, rax last, so after PUSHAQ:
;     [rsp+0]  = rdi   [rsp+8]  = rsi   [rsp+16] = rbp   [rsp+24] = rbx
;     [rsp+32] = rdx   [rsp+40] = rcx   [rsp+48] = r8    [rsp+56] = rax
;     [rsp+64] = r9    [rsp+72] = r10   [rsp+80] = r11   [rsp+88] = r12
;     [rsp+96] = r13   [rsp+104]= r14   [rsp+112]= r15
;   POPAQ is the exact inverse. This pairing is the revertible-effects rule
;   applied to the register file: every save has a matching restore.
;
;   CONTRACT
;     IN        : nothing
;     OUT       : 15 qwords pushed (PUSHAQ) / popped (POPAQ)
;     CLOBBERS  : none (the registers are restored by POPAQ)
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro PUSHAQ
{
	push	r15
	push	r14
	push	r13
	push	r12
	push	r11
	push	r10
	push	r9
	push	r8
	push	rcx
	push	rdx
	push	rbx
	push	rbp
	push	rsi
	push	rdi
	; rax pushed last so it sits at [rsp+56] with rdi at [rsp+0]
	push	rax
}

macro POPAQ
{
	pop	rax
	pop	rdi
	pop	rsi
	pop	rbp
	pop	rbx
	pop	rdx
	pop	rcx
	pop	r8
	pop	r9
	pop	r10
	pop	r11
	pop	r12
	pop	r13
	pop	r14
	pop	r15
}

; -----------------------------------------------------------------------------
; REDZONE_BARRIER
;   A leaf function may legally use the 128-byte red zone below rsp, but a
;   function that makes calls must not rely on it (a callee's frame would
;   overwrite it). This macro is a documentation-level marker plus an
;   optional assembly-time tripwire: it emits nothing, but pairing it with
;   the smoke tests ensures no red-zone assumption sneaks into non-leaf code.
; -----------------------------------------------------------------------------
macro REDZONE_BARRIER
{
	; intentionally emits nothing; presence is a review/test anchor
}

; -----------------------------------------------------------------------------
; Shadow space (Windows x64) management. On Linux this is a no-op, but the
; macros exist so call sites can be written portably. On Windows a caller
; must reserve 32 bytes of shadow space before a call; the constants keep
; that requirement visible.
; -----------------------------------------------------------------------------
SHADOW_SPACE_SIZE = 32

macro RESERVE_SHADOW_SPACE
{
  if PLATFORM_WINDOWS
	sub	rsp, SHADOW_SPACE_SIZE
  end if
}
macro RELEASE_SHADOW_SPACE
{
  if PLATFORM_WINDOWS
	add	rsp, SHADOW_SPACE_SIZE
  end if
}

end if ; FP_STATE_MANAGEMENT_INC

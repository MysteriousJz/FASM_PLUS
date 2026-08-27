; =============================================================================
; FASM_PLUS OS Core — CORDIS.INC
; The Cordis layer: dynamic composability primitives for the kernel.
;
; Depends on: SYSCALL.INC (dispatch), CONTEXT.INC, SERIAL.INC, CONSTANTS.INC
; Emits: functions in the current code section; the Context Registry is a
;        fixed-size, zero-initialized region in BSS (fp$cordis_registry).
;
; Philosophy
; ----------
; The kernel is the substrate for dynamic composability. Every component — a
; "fiber" — can be loaded when its dependencies are satisfied, unloaded when
; they disappear, and reverted when unloaded. Three ideas make this safe:
;
;   1. Revertible effects   Every state-changing operation is paired with an
;      inverse. When a fiber unloads, its inverses are applied in LIFO order,
;      returning the system to its pre-fiber state.
;   2. Declared dependencies Every fiber declares what it needs (inject) and
;      what it contributes (provide). The kernel tracks what is provided; when
;      a provider disappears, dependents are deactivated.
;   3. The Context is first-class All state — fibers, provisions, effects —
;      lives in one fixed-size registry. No dynamic allocation; the structure
;      is a compile-time constant laid out in BSS.
;
; Memory model
; ------------
; The registry is a single fixed-size block, sub-divided into three arrays:
;
;   fp$cordis_fibers[64]    fiber entries (state, inject/provide, effects)
;   fp$cordis_coeffects[128] provision entries (key -> provider fiber)
;   fp$cordis_effects[256]  effect entries (fiber, inverse fn, context)
;
; Keys are 64-bit integers supplied by the caller (a hash of a dependency
; name, or any unique token). Using integers keeps resolution division-free
; and comparison a single cmp.
; =============================================================================

if ~ definite FP_CORDIS_INC
FP_CORDIS_INC = 1

if ~ definite FP_SYSCALL_INC
  err 'CORDIS.INC requires SYSCALL.INC (and CONTEXT.INC, SERIAL.INC) first'
end if

; -----------------------------------------------------------------------------
; Registry geometry (fixed size, no dynamic allocation)
; -----------------------------------------------------------------------------
CORDIS_MAX_FIBERS    = 64
CORDIS_MAX_COEFFECTS = 128
CORDIS_MAX_EFFECTS   = 256
CORDIS_MAX_LIST      = 16          ; max inject or provide keys per fiber

; Fiber states
FIBER_INACTIVE   = 0
FIBER_LOADING    = 1
FIBER_ACTIVE     = 2
FIBER_UNLOADING  = 3
FIBER_FAILED     = 4

; -----------------------------------------------------------------------------
; Fiber entry (one loaded component)
; -----------------------------------------------------------------------------
struc cordis_fiber {
    .state        dq ?                  ; FIBER_* state
    .fiber_id     dq ?                  ; index of this entry (0..63)
    .inject_count dq ?                  ; number of dependencies
    .provide_count dq ?                 ; number of provisions
    .effect_head  dq ?                  ; index into effect stack, -1 = none
    .inject       rb CORDIS_MAX_LIST*8  ; dependency keys
    .provide      rb CORDIS_MAX_LIST*8  ; provision keys
    .provide_val  rb CORDIS_MAX_LIST*8  ; value supplied for each provision
}
virtual at 0
    cordis_fiber cordis_fiber
    cordis_fiber_end:
end virtual
CORDIS_FIBER_SIZE = cordis_fiber_end

; -----------------------------------------------------------------------------
; Coeffect entry (one current provision: key -> provider fiber)
; -----------------------------------------------------------------------------
struc cordis_coeffect {
    .used      dq ?                     ; 0 = free, 1 = in use
    .key       dq ?                     ; dependency key
    .provider  dq ?                     ; fiber id providing it
    .value     dq ?                     ; value the provider supplied
}
virtual at 0
    cordis_coeffect cordis_coeffect
    cordis_coeffect_end:
end virtual
CORDIS_COEFFECT_SIZE = cordis_coeffect_end

; -----------------------------------------------------------------------------
; Effect entry (one revertible operation)
; -----------------------------------------------------------------------------
struc cordis_effect {
    .used        dq ?                   ; 0 = free, 1 = in use
    .fiber_id    dq ?                   ; owning fiber
    .inverse_ptr dq ?                   ; inverse function pointer
    .context_ptr dq ?                   ; argument passed to the inverse
    .next        dq ?                   ; next effect index for the fiber, -1
}
virtual at 0
    cordis_effect cordis_effect
    cordis_effect_end:
end virtual
CORDIS_EFFECT_SIZE = cordis_effect_end

; Sentinel for "no link" (index -1)
CORDIS_NIL = -1

; =============================================================================
; fp$cordis_init
;   CONTRACT
;     IN        : nothing
;     OUT       : nothing
;     CLOBBERS  : rax, rcx, rdx, rdi, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi
;     ERRORS    : none
;   Zeroes the entire Context Registry and initializes every fiber's
;   effect_head to CORDIS_NIL. Called once from fp$kernel_init before any
;   fiber is loaded.
; =============================================================================
align 16
fp$cordis_init:
	; zero fibers
	mov	rdi, fp$cordis_fibers
	mov	rcx, CORDIS_MAX_FIBERS * CORDIS_FIBER_SIZE
      .zf:
	mov	byte [rdi], 0
	inc	rdi
	dec	rcx
	jnz	.zf
	; zero coeffects
	mov	rdi, fp$cordis_coeffects
	mov	rcx, CORDIS_MAX_COEFFECTS * CORDIS_COEFFECT_SIZE
      .zc:
	mov	byte [rdi], 0
	inc	rdi
	dec	rcx
	jnz	.zc
	; zero effects
	mov	rdi, fp$cordis_effects
	mov	rcx, CORDIS_MAX_EFFECTS * CORDIS_EFFECT_SIZE
      .ze:
	mov	byte [rdi], 0
	inc	rdi
	dec	rcx
	jnz	.ze
	; set each fiber's id and effect_head = CORDIS_NIL
	mov	rdi, fp$cordis_fibers
	xor	edx, edx		; fiber index
      .init_fiber:
	mov	rax, FIBER_INACTIVE
	mov	[rdi + cordis_fiber.state], rax
	mov	[rdi + cordis_fiber.fiber_id], rdx
	mov	rax, CORDIS_NIL
	mov	[rdi + cordis_fiber.effect_head], rax
	add	rdi, CORDIS_FIBER_SIZE
	inc	edx
	cmp	edx, CORDIS_MAX_FIBERS
	jb	.init_fiber
	; no open effect scope
	mov	rax, CORDIS_NIL
	mov	[fp$cordis_scope_fiber], rax
	ret

; =============================================================================
; Internal helpers (not syscalls)
; =============================================================================

; fp$cordis_fiber_ptr — rdi = fiber id -> rax = &fiber (no bounds check here;
; callers clamp). Helper, clobbers rax, rdx.
align 16
fp$cordis_fiber_ptr:
	mov	rax, rdi
	mov	rdx, CORDIS_FIBER_SIZE
	; multiply by constant via shifts is not possible for arbitrary size;
	; CORDIS_FIBER_SIZE is a compile-time constant, imul is a single
	; latency-3 op and is not a division — allowed.
	imul	rax, rdx
	add	rax, fp$cordis_fibers
	ret

; fp$coeffect_find — rdi = key -> rax = coeffect index or CORDIS_NIL
;   CONTRACT: IN rdi=key; OUT rax=index|-1; CLOBBERS rax, rcx, rdx, flags.
align 16
fp$coeffect_find:
	mov	rcx, fp$cordis_coeffects
	xor	edx, edx
      .scan:
	mov	rax, [rcx + cordis_coeffect.used]
	test	rax, rax
	jz	.next
	mov	rax, [rcx + cordis_coeffect.key]
	cmp	rax, rdi
	je	.found
      .next:
	add	rcx, CORDIS_COEFFECT_SIZE
	inc	edx
	cmp	edx, CORDIS_MAX_COEFFECTS
	jb	.scan
	mov	rax, CORDIS_NIL
	ret
      .found:
	mov	rax, rdx
	ret

; fp$coeffect_find_free -> rax = free coeffect index or CORDIS_NIL
;   CONTRACT: OUT rax=index|-1; CLOBBERS rax, rcx, rdx, flags.
align 16
fp$coeffect_find_free:
	mov	rcx, fp$cordis_coeffects
	xor	edx, edx
      .scan:
	mov	rax, [rcx + cordis_coeffect.used]
	test	rax, rax
	jz	.found
	add	rcx, CORDIS_COEFFECT_SIZE
	inc	edx
	cmp	edx, CORDIS_MAX_COEFFECTS
	jb	.scan
	mov	rax, CORDIS_NIL
	ret
      .found:
	mov	rax, rdx
	ret

; fp$effect_alloc -> rax = free effect index or CORDIS_NIL
;   CONTRACT: OUT rax=index|-1; CLOBBERS rax, rcx, rdx, flags.
align 16
fp$effect_alloc:
	mov	rcx, fp$cordis_effects
	xor	edx, edx
      .scan:
	mov	rax, [rcx + cordis_effect.used]
	test	rax, rax
	jz	.found
	add	rcx, CORDIS_EFFECT_SIZE
	inc	edx
	cmp	edx, CORDIS_MAX_EFFECTS
	jb	.scan
	mov	rax, CORDIS_NIL
	ret
      .found:
	mov	rax, rdx
	ret

; =============================================================================
; Effect tracking
; =============================================================================

; fp$effect_begin (syscall 8) — rdi = fiber id. Starts a scope: records the
; fiber whose subsequent effects accumulate. Returns rax = 0.
align 16
fp$effect_begin:
	mov	[fp$cordis_scope_fiber], rdi
	xor	eax, eax
	ret

; fp$effect_record — rdi = fiber id, rsi = inverse ptr, rdx = context ptr.
; Records a revertible effect on the fiber's stack (LIFO). Internal + used by
; the fiber test. Returns rax = effect index or E_OUT_OF_MEMORY.
;   No division: the free-slot index is kept in a register and turned into a
;   pointer by one imul against the compile-time-constant entry size.
align 16
fp$effect_record:
	push	rbx
	push	r12
	push	r13
	push	r14
	mov	r12, rdi		; r12 = fiber id
	mov	r13, rsi		; r13 = inverse ptr
	mov	r14, rdx		; r14 = context ptr
	call	fp$effect_alloc		; rax = free effect index
	cmp	rax, CORDIS_NIL
	je	.full
	mov	rbx, rax		; rbx = effect index
	; effect pointer = base + index * size
	mov	rcx, CORDIS_EFFECT_SIZE
	imul	rcx, rax
	lea	rax, [fp$cordis_effects]
	add	rax, rcx		; rax = &effect
	mov	qword [rax + cordis_effect.used], 1
	mov	[rax + cordis_effect.fiber_id], r12
	mov	[rax + cordis_effect.inverse_ptr], r13
	mov	[rax + cordis_effect.context_ptr], r14
	; link onto the fiber's effect stack (push head)
	push	rax			; save &effect
	mov	rdi, r12
	call	fp$cordis_fiber_ptr	; rax = &fiber
	mov	rcx, rax		; rcx = &fiber
	pop	rax			; rax = &effect
	mov	rdx, [rcx + cordis_fiber.effect_head]
	mov	[rax + cordis_effect.next], rdx
	mov	[rcx + cordis_fiber.effect_head], rbx
	mov	rax, rbx		; return effect index
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
	ret
      .full:
	mov	rax, E_OUT_OF_MEMORY
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
	ret

; fp$effect_commit (syscall 9) — finalize the current scope. Effects stay on
; the fiber's stack. Returns rax = 0.
align 16
fp$effect_commit:
	mov	rax, CORDIS_NIL
	mov	[fp$cordis_scope_fiber], rax
	xor	eax, eax
	ret

; fp$effect_rollback (syscall 10) — apply every inverse on the scoped fiber's
; effect stack in LIFO order, then free them. Returns rax = 0.
;   CONTRACT: IN nothing; OUT rax=0; CLOBBERS all (calls inverse pointers).
align 16
fp$effect_rollback:
	mov	rdi, [fp$cordis_scope_fiber]
	cmp	rdi, CORDIS_NIL
	je	.done
	mov	rax, CORDIS_NIL
	mov	[fp$cordis_scope_fiber], rax
	call	fp$fiber_revert_effects	; reverts rdi's effects
      .done:
	xor	eax, eax
	ret

; fp$fiber_revert_effects — rdi = fiber id. Applies every inverse on the
; fiber's effect stack in LIFO order and frees each entry. Internal.
;   CONTRACT: IN rdi=fiber id; CLOBBERS all.
align 16
fp$fiber_revert_effects:
	push	rbx
	push	r12
	call	fp$cordis_fiber_ptr
	mov	rbx, rax		; rbx = &fiber
	mov	r12, [rbx + cordis_fiber.effect_head]
      .loop:
	cmp	r12, CORDIS_NIL
	je	.done
	; r12 = effect index -> address
	mov	rax, r12
	mov	rcx, CORDIS_EFFECT_SIZE
	imul	rax, rcx
	add	rax, fp$cordis_effects	; rax = &effect
	; call inverse(context_ptr) if present
	mov	rdx, [rax + cordis_effect.inverse_ptr]
	test	rdx, rdx
	jz	.free
	mov	rdi, [rax + cordis_effect.context_ptr]
	push	rax
	call	rdx
	pop	rax
      .free:
	mov	rcx, [rax + cordis_effect.next]	; next index
	mov	qword [rax + cordis_effect.used], 0
	mov	r12, rcx
	jmp	.loop
      .done:
	mov	rax, CORDIS_NIL
	mov	[rbx + cordis_fiber.effect_head], rax
	pop	r12
	pop	rbx
	ret

; =============================================================================
; Coeffect resolution
; =============================================================================

; fp$coeffect_declare (syscall 11) — rdi = fiber id, rsi = key.
; Registers a dependency key on the fiber's inject list. Returns rax = 0 or
; E_OUT_OF_RANGE (bad fiber) / E_OUT_OF_MEMORY (inject list full).
align 16
fp$coeffect_declare:
	cmp	rdi, CORDIS_MAX_FIBERS
	jae	.bad
	push	rsi
	call	fp$cordis_fiber_ptr
	pop	rsi
	mov	rcx, [rax + cordis_fiber.inject_count]
	cmp	rcx, CORDIS_MAX_LIST
	jae	.full
	; inject[rcx] = key
	lea	rdx, [rax + cordis_fiber.inject]
	mov	[rdx + rcx*8], rsi
	inc	rcx
	mov	[rax + cordis_fiber.inject_count], rcx
	xor	eax, eax
	ret
      .bad:
	mov	rax, E_OUT_OF_RANGE
	ret
      .full:
	mov	rax, E_OUT_OF_MEMORY
	ret

; fp$coeffect_provide (syscall 12) — rdi = fiber id, rsi = key, rdx = value.
; Registers a provision: adds a coeffect entry (key -> fiber, value) and
; records the key on the fiber's provide list. Returns rax = 0 or error.
align 16
fp$coeffect_provide:
	cmp	rdi, CORDIS_MAX_FIBERS
	jae	.bad
	push	rdi
	push	rsi
	push	rdx
	; find a free coeffect slot
	call	fp$coeffect_find_free
	cmp	rax, CORDIS_NIL
	je	.full
	mov	rcx, CORDIS_COEFFECT_SIZE
	imul	rax, rcx
	add	rax, fp$cordis_coeffects	; rax = &coeffect
	pop	rdx
	pop	rsi
	pop	rdi
	mov	qword [rax + cordis_coeffect.used], 1
	mov	[rax + cordis_coeffect.key], rsi
	mov	[rax + cordis_coeffect.provider], rdi
	mov	[rax + cordis_coeffect.value], rdx
	; record on the fiber's provide list
	push	rdi
	push	rsi
	push	rdx
	call	fp$cordis_fiber_ptr		; rax = &fiber
	pop	rdx
	pop	rsi
	pop	rdi
	mov	rcx, [rax + cordis_fiber.provide_count]
	cmp	rcx, CORDIS_MAX_LIST
	jae	.full2
	lea	r8, [rax + cordis_fiber.provide]
	mov	[r8 + rcx*8], rsi		; key
	lea	r8, [rax + cordis_fiber.provide_val]
	mov	[r8 + rcx*8], rdx		; value
	inc	rcx
	mov	[rax + cordis_fiber.provide_count], rcx
	xor	eax, eax
	ret
      .bad:
	mov	rax, E_OUT_OF_RANGE
	ret
      .full:
	pop	rdx
	pop	rsi
	pop	rdi
	mov	rax, E_OUT_OF_MEMORY
	ret
      .full2:
	mov	rax, E_OUT_OF_MEMORY
	ret

; fp$coeffect_resolve (syscall 13) — rdi = fiber id.
; Resolves every key on the fiber's inject list against the coeffect table.
; Returns rax = 0 if all satisfied, else E_NOT_FOUND.
;   The resolved provider value for inject[i] is written back is NOT needed;
;   resolution only checks presence (the fiber can query values via resolve1).
align 16
fp$coeffect_resolve:
	cmp	rdi, CORDIS_MAX_FIBERS
	jae	.bad
	push	rbx
	push	r12
	push	r13
	call	fp$cordis_fiber_ptr
	mov	rbx, rax			; rbx = &fiber
	mov	r12, [rbx + cordis_fiber.inject_count]
	xor	r13, r13			; i = 0
      .loop:
	cmp	r13, r12
	jae	.all_ok
	lea	rdx, [rbx + cordis_fiber.inject]
	mov	rdi, [rdx + r13*8]		; key
	call	fp$coeffect_find
	cmp	rax, CORDIS_NIL
	je	.missing
	inc	r13
	jmp	.loop
      .all_ok:
	xor	eax, eax
	pop	r13
	pop	r12
	pop	rbx
	ret
      .missing:
	mov	rax, E_NOT_FOUND
	pop	r13
	pop	r12
	pop	rbx
	ret
      .bad:
	mov	rax, E_OUT_OF_RANGE
	ret

; fp$coeffect_lookup — rdi = key -> rax = provider value, or CORDIS_NIL if
; unprovided. Used by tests to confirm a fiber sees a specific value.
align 16
fp$coeffect_lookup:
	call	fp$coeffect_find
	cmp	rax, CORDIS_NIL
	je	.none
	mov	rcx, CORDIS_COEFFECT_SIZE
	imul	rax, rcx
	add	rax, fp$cordis_coeffects
	mov	rax, [rax + cordis_coeffect.value]
	ret
      .none:
	mov	rax, CORDIS_NIL
	ret

; fp$coeffect_withdraw — rdi = fiber id. Removes every coeffect entry whose
; provider is this fiber. Internal, called during fiber unload.
align 16
fp$coeffect_withdraw:
	mov	rcx, fp$cordis_coeffects
	xor	edx, edx
      .scan:
	mov	rax, [rcx + cordis_coeffect.used]
	test	rax, rax
	jz	.next
	mov	rax, [rcx + cordis_coeffect.provider]
	cmp	rax, rdi
	jne	.next
	mov	qword [rcx + cordis_coeffect.used], 0
      .next:
	add	rcx, CORDIS_COEFFECT_SIZE
	inc	edx
	cmp	edx, CORDIS_MAX_COEFFECTS
	jb	.scan
	ret

; =============================================================================
; Fiber lifecycle
; =============================================================================

; fp$fiber_state — rdi = fiber id -> rax = current FIBER_* state.
; Query helper for tests and dependents. No side effects.
;   CONTRACT: IN rdi=fiber id; OUT rax=state (FIBER_INACTIVE if out of range);
;             CLOBBERS rax, rdx, flags.
align 16
fp$fiber_state:
	cmp	rdi, CORDIS_MAX_FIBERS
	jae	.bad
	call	fp$cordis_fiber_ptr
	mov	rax, [rax + cordis_fiber.state]
	ret
      .bad:
	mov	rax, FIBER_INACTIVE
	ret

; fp$fiber_load (syscall 14) — rdi = fiber id.
; Marks the fiber Loading, resolves its dependencies, and if all satisfied
; marks it Active. Returns rax = 0 (active) or E_NOT_FOUND (waiting).
align 16
fp$fiber_load:
	cmp	rdi, CORDIS_MAX_FIBERS
	jae	.bad
	push	rbx
	mov	rbx, rdi			; preserve fiber id across calls
	call	fp$cordis_fiber_ptr
	mov	rdx, FIBER_LOADING
	mov	[rax + cordis_fiber.state], rdx
	; resolve dependencies (clobbers rdi)
	mov	rdi, rbx
	call	fp$coeffect_resolve
	test	rax, rax
	jnz	.waiting
	; all satisfied -> Active
	mov	rdi, rbx
	call	fp$cordis_fiber_ptr
	mov	rdx, FIBER_ACTIVE
	mov	[rax + cordis_fiber.state], rdx
	xor	eax, eax
	pop	rbx
	ret
      .waiting:
	mov	rax, E_NOT_FOUND
	pop	rbx
	ret
      .bad:
	mov	rax, E_OUT_OF_RANGE
	ret

; fp$fiber_relied — rdi = fiber id -> rax = 1 if any Active fiber injects a
; key that this fiber provides, else 0. This is the deactivation guard.
;   CONTRACT: IN rdi=fiber id; OUT rax=0/1; CLOBBERS rax,rcx,rdx,rsi,rdi,r8-r11.
align 16
fp$fiber_relied:
	push	rbx
	push	r12
	push	r13
	push	r14
	mov	r14, rdi			; r14 = target fiber id
	; outer loop over fibers
	xor	r12, r12			; provider candidate index
      .fiber_loop:
	cmp	r12, CORDIS_MAX_FIBERS
	jae	.not_relied
	cmp	r12, r14
	je	.next_fiber			; a fiber does not rely on itself
	; only Active (or Loading) fibers can impose a dependency
	mov	rdi, r12
	call	fp$cordis_fiber_ptr
	mov	rbx, rax			; rbx = &candidate fiber
	mov	rax, [rbx + cordis_fiber.state]
	cmp	rax, FIBER_ACTIVE
	je	.check_inject
	cmp	rax, FIBER_LOADING
	je	.check_inject
	jmp	.next_fiber
      .check_inject:
	; for each inject key of the candidate, see if target provides it
	xor	r13, r13			; inject index
      .inject_loop:
	cmp	r13, [rbx + cordis_fiber.inject_count]
	jae	.next_fiber
	lea	rdx, [rbx + cordis_fiber.inject]
	mov	rdi, [rdx + r13*8]		; key candidate needs
	call	fp$coeffect_find
	cmp	rax, CORDIS_NIL
	je	.next_inject
	; found a provider for this key; is it the target fiber?
	mov	rcx, CORDIS_COEFFECT_SIZE
	imul	rax, rcx
	add	rax, fp$cordis_coeffects
	mov	rdx, [rax + cordis_coeffect.provider]
	cmp	rdx, r14
	je	.relied
      .next_inject:
	inc	r13
	jmp	.inject_loop
      .next_fiber:
	inc	r12
	jmp	.fiber_loop
      .relied:
	mov	eax, 1
	jmp	.out
      .not_relied:
	xor	eax, eax
      .out:
	pop	r14
	pop	r13
	pop	r12
	pop	rbx
	ret

; fp$fiber_unload (syscall 15) — rdi = fiber id.
; Two-phase unload:
;   Leave  — mark Unloading, withdraw its provisions (so dependents stop
;            resolving) and cascade-deactivate any fiber that relied on it.
;   Unload — when nothing relies on it, revert its effects, mark Inactive.
; Returns rax = 0 on full unload, E_BUSY if still relied upon.
E_BUSY = -16

align 16
fp$fiber_unload:
	cmp	rdi, CORDIS_MAX_FIBERS
	jae	.bad
	push	rbx
	push	r12
	mov	r12, rdi			; r12 = target fiber
	; ----- Leave phase: withdraw provisions -----
	call	fp$coeffect_withdraw
	call	fp$cordis_fiber_ptr
	mov	rbx, rax			; rbx = &target fiber
	mov	rax, FIBER_UNLOADING
	mov	[rbx + cordis_fiber.state], rax
	; clear its provide list
	mov	qword [rbx + cordis_fiber.provide_count], 0
	; ----- Cascade: deactivate any Active fiber whose deps no longer
	;       resolve (because this fiber's provisions are gone). -----
	call	fp$cordis_deactivate_unsatisfied
	; ----- Unload phase: only when nothing relies on the target -----
	mov	rdi, r12
	call	fp$fiber_relied
	test	rax, rax
	jnz	.busy
	; revert the target's effects
	mov	rdi, r12
	call	fp$fiber_revert_effects
	; mark Inactive and clear its lists
	mov	rdi, r12
	call	fp$cordis_fiber_ptr
	mov	rdx, FIBER_INACTIVE
	mov	[rax + cordis_fiber.state], rdx
	mov	qword [rax + cordis_fiber.inject_count], 0
	xor	eax, eax
	pop	r12
	pop	rbx
	ret
      .busy:
	mov	rax, E_BUSY
	pop	r12
	pop	rbx
	ret
      .bad:
	mov	rax, E_OUT_OF_RANGE
	ret

; fp$cordis_deactivate_unsatisfied — scan all fibers; any Active/Loading fiber
; whose inject list no longer fully resolves is deactivated (marked Inactive)
; and its effects reverted. Internal, iterative until a fixed point.
;   CONTRACT: CLOBBERS all.
align 16
fp$cordis_deactivate_unsatisfied:
	push	rbx
	push	r12
      .pass:
	xor	r9, r9				; changed flag (reset each pass)
	xor	r12, r12			; fiber index
      .scan:
	cmp	r12, CORDIS_MAX_FIBERS
	jae	.pass_done
	mov	rdi, r12
	call	fp$cordis_fiber_ptr
	mov	rbx, rax			; rbx = &fiber
	mov	rax, [rbx + cordis_fiber.state]
	cmp	rax, FIBER_ACTIVE
	je	.check
	cmp	rax, FIBER_LOADING
	je	.check
	jmp	.next
      .check:
	mov	rdi, r12
	call	fp$coeffect_resolve
	test	rax, rax
	jz	.next				; still satisfied
	; deactivate: revert effects, withdraw provisions, mark Inactive
	mov	rdi, r12
	call	fp$coeffect_withdraw
	mov	rdi, r12
	call	fp$fiber_revert_effects
	mov	rdi, r12
	call	fp$cordis_fiber_ptr
	mov	rdx, FIBER_INACTIVE
	mov	[rax + cordis_fiber.state], rdx
	mov	r9, 1				; changed = 1
      .next:
	inc	r12
	jmp	.scan
      .pass_done:
	test	r9, r9
	jnz	.pass				; repeat until no change
	pop	r12
	pop	rbx
	ret

; =============================================================================
; Read-only / BSS data
; =============================================================================
; The currently-scoped fiber for effect tracking (CORDIS_NIL = no open scope).
; Lives in the data area allocated by OS_TEST.ASM.

end if ; FP_CORDIS_INC

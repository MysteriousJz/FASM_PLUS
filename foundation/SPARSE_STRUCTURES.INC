; =============================================================================
; FASM_PLUS Foundation Library — Layer 5 (Kind Machine extension)
; SPARSE_STRUCTURES.INC — bitmasks and sparse rows for pruned attention
;
; Depends on: CONSTANTS.INC, CONTRACTS.INC, BIT_MATH.INC
; Emits: no code or data by itself; macros expand inline or define a layout.
;
; Bit-math standard (see BIT_MATH.INC)
; ------------------------------------
; Every mask operation is a single bit-manipulation instruction where the ISA
; provides one (bts/btr/bt/bsf/blsr); iteration advances by clearing the
; lowest set bit (blsr) and scanning for the next (bsf) — never a division,
; never a modulo, never a branch per bit beyond the loop test.
;
; Masks wider than 64 bits are arrays of qwords; MASK_SET_BIT_ARRAY selects
; the qword with a shift (index shr 6) and the bit with an AND (index and 63).
; =============================================================================

if ~ definite FP_SPARSE_STRUCTURES_INC
FP_SPARSE_STRUCTURES_INC = 1

if ~ definite FP_BIT_MATH_INC
  err 'SPARSE_STRUCTURES.INC requires BIT_MATH.INC (and lower layers) first'
end if

; bits per mask qword (shift-based selection requires this to be 2^n)
MASK_WORD_BITS      = 64
MASK_WORD_SHIFT     = 6           ; log2(64)
MASK_WORD_MASK      = 63          ; 64 - 1, for index mod 64 via AND

STATIC_ASSERT (MASK_WORD_BITS and (MASK_WORD_BITS-1)) = 0

; -----------------------------------------------------------------------------
; MASK_SET_BIT mask_reg, bit_index
;   Set bit `bit_index` in a 64-bit register mask. Single BTS.
;   CONTRACT
;     IN        : mask_reg — 64-bit register; bit_index — register or imm 0..63
;     OUT       : mask_reg with the bit set
;     CLOBBERS  : mask_reg, flags
;     PRESERVES : all other registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro MASK_SET_BIT mask_reg*, bit_index*
{
	bts	mask_reg, bit_index
}

; -----------------------------------------------------------------------------
; MASK_CLEAR_BIT mask_reg, bit_index
;   Clear bit `bit_index` in a 64-bit register mask. Single BTR.
;   CONTRACT
;     IN        : mask_reg — 64-bit register; bit_index — register or imm 0..63
;     OUT       : mask_reg with the bit cleared
;     CLOBBERS  : mask_reg, flags
;     PRESERVES : all other registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro MASK_CLEAR_BIT mask_reg*, bit_index*
{
	btr	mask_reg, bit_index
}

; -----------------------------------------------------------------------------
; MASK_TEST_BIT mask_reg, bit_index
;   Test bit `bit_index`; CF = the bit. Single BT. Follow with setc/jc as
;   needed — the contract exposes the carry flag, not a booleanized register,
;   so callers pay for the conversion only when they need it.
;   CONTRACT
;     IN        : mask_reg — 64-bit register; bit_index — register or imm
;     OUT       : CF = bit value (1 = set); mask_reg unchanged
;     CLOBBERS  : flags
;     PRESERVES : all registers including mask_reg
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro MASK_TEST_BIT mask_reg*, bit_index*
{
	bt	mask_reg, bit_index
}

; -----------------------------------------------------------------------------
; MASK_ITERATE mask_reg, action_macro
;   Expand action_macro once per set bit in mask_reg, ascending bit order.
;   The bit index is left in `rcx` for the action; mask_reg is consumed
;   (copied to a working register and cleared bit by bit). Iteration uses
;   BSF to find the lowest set bit and BLSR to clear it — two instructions
;   per set bit, no per-bit branch.
;
;   The action_macro is expanded with % = bit index available in rcx; it must
;   preserve rcx if it needs the index after clobbering it.
;
;   CONTRACT
;     IN        : mask_reg — 64-bit register (its value is destroyed)
;                 action_macro — a macro name invoked once per set bit
;     OUT       : action expanded per set bit, ascending index
;     CLOBBERS  : mask_reg, rcx, rdx, flags; plus whatever action clobbers
;     PRESERVES : all non-volatile registers (action permitting)
;     ERRORS    : none; an empty mask runs the action zero times
; -----------------------------------------------------------------------------
macro MASK_ITERATE mask_reg*, action_macro*
{
	local	.scan, .done
	mov	rdx, mask_reg			; working copy
.scan:
	bsf	rcx, rdx			; lowest set bit index
	jz	.done				; ZF set when rdx == 0
	action_macro
	blsr	rdx, rdx			; clear lowest set bit
	jmp	.scan
.done:
}

; -----------------------------------------------------------------------------
; Sparse row layout (defined by SPARSE_ROW):
;   +0   count   dq   number of live (index, value) pairs
;   +8   indices dq[max_entries]   column indices, insertion order
;   +... values  dq[max_entries]   the value paired with each index
; Iteration order is insertion order; the attention scaffolding appends in
; ascending column order, so iteration is ascending by construction.
; -----------------------------------------------------------------------------

; -----------------------------------------------------------------------------
; SPARSE_ROW name, max_entries
;   Define a sparse row structure in the current data segment. Also defines
;   the companion labels name#_indices / name#_values and the constant
;   name#_MAX, so callers can take the values base with `lea r, [name_values]`.
;   CONTRACT
;     IN        : name — structure label; max_entries — compile-time capacity
;     OUT       : emits count + 2*max_entries qwords
;     CLOBBERS  : advances the output pointer
;     ERRORS    : assembly error if max_entries < 1
; -----------------------------------------------------------------------------
macro SPARSE_ROW rname*, max_entries*
{
  if (max_entries) < 1
    err 'SPARSE_ROW: max_entries must be >= 1'
  end if
  rname:
  rname#_count	dq 0
  rname#_indices	dq max_entries dup 0
  rname#_values	dq max_entries dup 0
  rname#_MAX = max_entries
}

; -----------------------------------------------------------------------------
; SPARSE_ROW_APPEND row_reg, values_base_reg, index_reg, value_reg
;   Append (index, value) to the row addressed by row_reg, whose values array
;   base is in values_base_reg. Does NOT check capacity — the caller's
;   contract is to append at most max_entries times (overflow would write
;   past the values array; the smoke tests stay within capacity).
;
;   The values base must be supplied explicitly because only the defining
;   SPARSE_ROW knows max_entries; obtain it with `lea r, [rowname_values]`.
;
;   CONTRACT
;     IN        : row_reg — register holding the SPARSE_ROW address
;                 values_base_reg — register holding row's values array base
;                 index_reg, value_reg — registers holding the pair
;     OUT       : pair stored; count incremented
;     CLOBBERS  : rcx, flags; memory written in the row
;     PRESERVES : row_reg, values_base_reg, index_reg, value_reg, rax, rdx,
;                 and all non-volatile registers
;     ERRORS    : none (capacity is the caller's contract)
; -----------------------------------------------------------------------------
macro SPARSE_ROW_APPEND row_reg*, values_base_reg*, index_reg*, value_reg*
{
	mov	rcx, [row_reg]			; count
	; indices[count] = index  (row + 8 + count*8, indexed addressing)
	mov	[row_reg+8+rcx*8], index_reg
	; values[count] = value
	mov	[values_base_reg+rcx*8], value_reg
	inc	rcx
	mov	[row_reg], rcx			; count++
}

; -----------------------------------------------------------------------------
; SPARSE_ROW_ITERATE row_reg, values_base_reg, action_macro
;   Expand action_macro once per live entry, in insertion (ascending) order.
;   On entry to each expansion: rcx = entry number (0-based), and the caller
;   loads the pair as:
;     index = [row_reg + 8 + rcx*8]
;     value = [values_base_reg + rcx*8]
;   The loop counter lives in rcx; action_macro must preserve or reload it.
;
;   CONTRACT
;     IN        : row_reg — register holding the SPARSE_ROW address
;                 values_base_reg — register holding the row's values base
;                 action_macro — expanded per entry
;     OUT       : action expanded count times, ascending
;     CLOBBERS  : rcx, rdx, flags; plus whatever action clobbers
;     PRESERVES : row_reg, values_base_reg, non-volatile registers
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro SPARSE_ROW_ITERATE row_reg*, values_base_reg*, action_macro*
{
	local	.iter, .done
	mov	rdx, [row_reg]			; count
	xor	ecx, ecx			; entry number
.iter:
	cmp	rcx, rdx
	jae	.done
	action_macro
	inc	rcx
	jmp	.iter
.done:
}

end if ; FP_SPARSE_STRUCTURES_INC

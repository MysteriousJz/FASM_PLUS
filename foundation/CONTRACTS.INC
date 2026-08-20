; =============================================================================
; FASM_PLUS Foundation Library — Layer 1
; CONTRACTS.INC — compile-time contract verification helpers
;
; Depends on: CONSTANTS.INC
; Emits: no code or data by itself; all macros are compile-time checks unless
;        explicitly documented otherwise.
;
; Contract philosophy
; -------------------
; Every macro in this library documents a contract of the form:
;
;   ; CONTRACT name
;   ;   IN   : arguments, where they live, required preconditions
;   ;   OUT  : results, where they live, guaranteed postconditions
;   ;   CLOBBERS : registers/flags/memory the macro may modify
;   ;   ERRORS   : values returned or traps raised on failure
;   ;   PRESERVES: registers/flags guaranteed unchanged
;
; The macros below enforce at assembly time the parts of a contract that can
; be checked statically: power-of-two alignment, value ranges, immediate
; sizes. Anything that cannot be checked statically must be covered by a
; runtime assertion (TEST_FRAMEWORK.INC) or a documented trap.
; =============================================================================

if ~ definite FP_CONTRACTS_INC
FP_CONTRACTS_INC = 1

if ~ definite FP_CONSTANTS_INC
  err 'CONTRACTS.INC requires CONSTANTS.INC to be included first'
end if

; -----------------------------------------------------------------------------
; STATIC_ASSERT cond
;   Fails assembly if cond is false. Use for any contract that can be
;   expressed as a compile-time constant expression. The failing macro name
;   and source line are reported by FASM, which keeps failures localizable
;   even though the message text is fixed (FASM's `err` takes a literal).
; -----------------------------------------------------------------------------
macro STATIC_ASSERT cond*
{
  if ~(cond)
    err 'STATIC_ASSERT failed'
  end if
}

; -----------------------------------------------------------------------------
; ASSERT_POWER_OF_TWO value
;   Fails assembly unless value is a non-zero power of two.
; -----------------------------------------------------------------------------
macro ASSERT_POWER_OF_TWO value*
{
  if (value) = 0 | ((value) and ((value)-1)) <> 0
    err 'ASSERT_POWER_OF_TWO: value must be a non-zero power of two'
  end if
}

; -----------------------------------------------------------------------------
; ASSERT_ALIGNMENT value, alignment
;   Fails assembly unless value is a multiple of alignment.
;   alignment itself must be a power of two.
; -----------------------------------------------------------------------------
macro ASSERT_ALIGNMENT value*, alignment*
{
  ASSERT_POWER_OF_TWO alignment
  if ((value) and ((alignment)-1)) <> 0
    err 'ASSERT_ALIGNMENT: value is not aligned to the required boundary'
  end if
}

; -----------------------------------------------------------------------------
; ASSERT_IN_RANGE value, low, high   (inclusive on both ends)
; -----------------------------------------------------------------------------
macro ASSERT_IN_RANGE value*, low*, high*
{
  if (value) < (low) | (value) > (high)
    err 'ASSERT_IN_RANGE: value outside [low, high]'
  end if
}

; -----------------------------------------------------------------------------
; ASSERT_FITS_BYTE / _WORD / _DWORD  — immediate range checks.
;   These guard against silent truncation when a constant is moved into a
;   smaller container. They only accept values representable unsigned.
; -----------------------------------------------------------------------------
macro ASSERT_FITS_BYTE value*
{
  if (value) < 0 | (value) > 0xFF
    err 'ASSERT_FITS_BYTE: constant does not fit in 8 bits'
  end if
}
macro ASSERT_FITS_WORD value*
{
  if (value) < 0 | (value) > 0xFFFF
    err 'ASSERT_FITS_WORD: constant does not fit in 16 bits'
  end if
}
macro ASSERT_FITS_DWORD value*
{
  if (value) < 0 | (value) > 0xFFFFFFFF
    err 'ASSERT_FITS_DWORD: constant does not fit in 32 bits'
  end if
}

; -----------------------------------------------------------------------------
; ASSERT_KNOWN_VALUE symbol
;   Fails assembly if symbol was not defined earlier (catches typos in
;   constant names, which would otherwise become forward references and
;   assemble to something surprising).
; -----------------------------------------------------------------------------
macro ASSERT_KNOWN_VALUE symbol*
{
  if ~ definite symbol
    err 'ASSERT_KNOWN_VALUE: symbol was not defined before this point'
  end if
}

; -----------------------------------------------------------------------------
; ASSERT_DISTINCT a, b
;   Fails assembly if two compile-time values are equal. Useful to enforce
;   that two register-size codes or two tag constants differ.
; -----------------------------------------------------------------------------
macro ASSERT_DISTINCT a*, b*
{
  if (a) = (b)
    err 'ASSERT_DISTINCT: values must differ'
  end if
}

; -----------------------------------------------------------------------------
; Contract self-check: the constants we rely on from layer 0 must satisfy
; the invariants this layer assumes. These run at every inclusion, so a bad
; edit to CONSTANTS.INC is caught at the first build of any consumer.
; -----------------------------------------------------------------------------
STATIC_ASSERT (ALIGN_PAGE and (ALIGN_PAGE-1)) = 0
STATIC_ASSERT (ALIGN_STACK and (ALIGN_STACK-1)) = 0
STATIC_ASSERT PAGE_SIZE = ALIGN_PAGE
STATIC_ASSERT (1 shl PAGE_SHIFT) = PAGE_SIZE
STATIC_ASSERT NULL = 0
STATIC_ASSERT TRUE <> FALSE
ASSERT_ALIGNMENT STACK_CANARY_VALUE, 1   ; trivially true; exercises the macro

end if ; FP_CONTRACTS_INC

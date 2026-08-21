; =============================================================================
; FASM_PLUS OS Core — PAGING.INC
; Page table construction for the identity-mapped first 4 GB.
;
; Depends on: BIT_MATH.INC (bit constants and alignment), CONSTANTS.INC
; Emits: macros (inline at call site) and functions in the code section.
;
; The kernel uses 4-level paging with 2 MB large pages:
;   PML4 -> PDPT -> PD -> (2 MB page)
; A full 4 GB identity map needs 1 PML4, 4 PDPTs, and 4 PDs (one PD per GB),
; because each PD holds 512 entries x 2 MB = 1 GB.
;
; Page table entry bit layout (both 4 KB and 2 MB pages):
;   bit 0   P   present
;   bit 1   RW  read/write
;   bit 2   US  user/supervisor (0 = supervisor)
;   bit 5   A   accessed
;   bit 6   D   dirty (2 MB pages)
;   bit 7   PS  page size (1 = 2 MB large page, only in PD)
;   bit 63  NX  no-execute
;
; Macros expand inline and emit the obvious bit operation — no hidden work.
; =============================================================================

if ~ definite FP_PAGING_INC
FP_PAGING_INC = 1

if ~ definite FP_BIT_MATH_INC
  err 'PAGING.INC requires BIT_MATH.INC (and CONSTANTS.INC, CONTRACTS.INC) first'
end if

; -----------------------------------------------------------------------------
; Page table entry flag bits (bare values for data construction)
; -----------------------------------------------------------------------------
PTE_P_BIT       = 0x001         ; present
PTE_RW_BIT      = 0x002         ; read/write
PTE_US_BIT      = 0x004         ; user
PTE_A_BIT       = 0x020         ; accessed
PTE_D_BIT       = 0x040         ; dirty
PTE_PS_BIT      = 0x080         ; page size (2 MB large page)
PTE_NX_BIT      = 0x8000000000000000  ; no execute
PTE_ADDR_MASK   = 0x000FFFFFFFFFF000  ; physical address bits 51:12

; PML4 layout constants
PML4_ENTRIES    = 512
PDPT_ENTRIES    = 512
PD_ENTRIES      = 512
PD_ENTRY_SPAN   = 0x200000      ; 2 MB per PD entry
PD_SPAN         = 0x40000000    ; 1 GB per PD (512 x 2 MB)
FOUR_GB         = 0x100000000

; -----------------------------------------------------------------------------
; PTE_PRESENT entry   — set the present bit on the qword at [entry]
;   CONTRACT
;     IN        : entry = register holding a PTE address
;     OUT       : [entry] has bit 0 set
;     CLOBBERS  : flags; memory at [entry]
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro PTE_PRESENT entry*
{
	or	qword [entry], PTE_P_BIT
}

; -----------------------------------------------------------------------------
; PTE_WRITABLE entry  — set the read/write bit
; -----------------------------------------------------------------------------
macro PTE_WRITABLE entry*
{
	or	qword [entry], PTE_RW_BIT
}

; -----------------------------------------------------------------------------
; PTE_USER entry      — set the user/supervisor bit (makes the page user)
; -----------------------------------------------------------------------------
macro PTE_USER entry*
{
	or	qword [entry], PTE_US_BIT
}

; -----------------------------------------------------------------------------
; PTE_LARGE entry     — set the page-size bit (marks a PD entry as a 2 MB page)
; -----------------------------------------------------------------------------
macro PTE_LARGE entry*
{
	or	qword [entry], PTE_PS_BIT
}

; -----------------------------------------------------------------------------
; PTE_NX entry        — set the no-execute bit
; -----------------------------------------------------------------------------
macro PTE_NX entry*
{
	or	qword [entry], PTE_NX_BIT
}

; -----------------------------------------------------------------------------
; PTE_ADDR dest, entry  — extract the physical address from a PTE
;   CONTRACT
;     IN        : entry = register holding a PTE address
;     OUT       : dest = physical address (bits 51:12 of the PTE)
;     CLOBBERS  : dest, flags
;     ERRORS    : none
; -----------------------------------------------------------------------------
macro PTE_ADDR dest*, entry*
{
	mov	dest, [entry]
	and	dest, PTE_ADDR_MASK
}

; -----------------------------------------------------------------------------
; PTE_CLEAR entry     — zero a page table entry (marks it not-present)
; -----------------------------------------------------------------------------
macro PTE_CLEAR entry*
{
	mov	qword [entry], 0
}

; =============================================================================
; fp$paging_identity_map_4gb
;   CONTRACT
;     IN        : rdi = PML4 base address (4 KB aligned). The layout expected
;                 is contiguous: PML4 at [rdi], PDPT0..3 at [rdi+0x1000], and
;                 PD0..3 at [rdi+0x5000] — 9 pages total (0x9000 bytes).
;     OUT       : rax = 0 on success, negative errno on failure
;     CLOBBERS  : rax, rcx, rdx, rsi, r8, r9, flags
;     PRESERVES : rbx, rbp, r12-r15, rdi
;     ERRORS    : E_ALIGNMENT if rdi is not 4 KB aligned
;   Sets up the identity mapping for the first 4 GB using 2 MB large pages.
;   The region [rdi, rdi+0x9000) must be writable and is fully overwritten.
; =============================================================================
align 16
fp$paging_identity_map_4gb:
	; contract check: PML4 base must be 4 KB aligned
	mov	rax, rdi
	and	rax, ALIGN_PAGE - 1
	jnz	.bad_align
	; clear the whole 9-page region (PML4 + 4 PDPT + 4 PD)
	mov	rsi, rdi
	mov	ecx, (ALIGN_PAGE * 9) shr 3	; qword count, no division
      .clear:
	xor	eax, eax
	mov	[rsi], rax
	add	rsi, 8
	dec	ecx
	jnz	.clear
	; PML4[0..3] -> PDPT0..3 (present, writable, supervisor)
	mov	rcx, rdi		; PML4 cursor
	mov	rsi, rdi
	add	rsi, ALIGN_PAGE		; PDPT0 address
	mov	edx, 4			; four PDPTs (one per GB)
      .pml4:
	mov	rax, rsi
	or	rax, PTE_P_BIT or PTE_RW_BIT
	mov	[rcx], rax
	add	rcx, 8
	add	rsi, ALIGN_PAGE		; next PDPT page
	dec	edx
	jnz	.pml4
	; each PDPT[0] -> PDn (present, writable), then fill PDn with 512
	; 2 MB large pages covering [n*1GB, (n+1)*1GB). r10 tracks the PD
	; index because the PD base alone cannot be recovered once rcx
	; advances into the entries.
	mov	rsi, rdi
	add	rsi, ALIGN_PAGE		; PDPT cursor
	mov	rcx, rdi
	add	rcx, ALIGN_PAGE * 5	; PD0 address
	xor	r8d, r8d		; PD index 0..3
      .pdpt:
	mov	rax, rcx
	or	rax, PTE_P_BIT or PTE_RW_BIT
	mov	[rsi], rax		; PDPT[0] -> PDn
	add	rsi, ALIGN_PAGE		; next PDPT
	; fill the PD at rcx with 512 large pages
	mov	r9d, PD_ENTRIES
	mov	rax, r8
	shl	rax, 30			; PD index x 1 GB = base physical
      .pd:
	mov	rdx, rax
	or	rdx, PTE_P_BIT or PTE_RW_BIT or PTE_PS_BIT
	mov	[rcx], rdx		; PD entry: present, writable, 2 MB
	add	rcx, 8
	add	rax, PD_ENTRY_SPAN	; next 2 MB physical address
	dec	r9d
	jnz	.pd
	inc	r8d
	cmp	r8d, 4
	jb	.pdpt
	xor	eax, eax		; E_OK
	ret
      .bad_align:
	mov	rax, E_ALIGNMENT
	ret

; =============================================================================
; fp$paging_load_cr3
;   CONTRACT
;     IN        : rdi = PML4 base address (4 KB aligned)
;     OUT       : nothing
;     CLOBBERS  : rax, flags
;     PRESERVES : rbx, rbp, r12-r15, rsi, rdi, rcx, rdx
;     ERRORS    : none
;   Loads CR3 with the PML4 base, activating this page table. The address is
;   masked to the address bits (51:12) so flag bits are not set.
; =============================================================================
align 16
fp$paging_load_cr3:
	mov	rax, rdi
	and	rax, PTE_ADDR_MASK
	mov	cr3, rax
	ret

end if ; FP_PAGING_INC

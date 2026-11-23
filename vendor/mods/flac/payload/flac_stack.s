! SPDX-License-Identifier: GPL-2.0-or-later
! int flac_on_stack(int (*fn)(int, void *, uint32_t), int op, void *arg, uint32_t value)
!
! Calls fn(op, arg, value) with r15 on the decoder's private stack (16 KB in
! the payload's .bss) and returns its result on the caller's stack. dr_flac's
! open takes a 4.4 KB frame; the MAIN task that reads audio (mp3ActTask, 7.5 KB
! of on-chip RAM) cannot spare it. MACH and MACL are saved and restored here,
! because MAIN (Renesas ABI) expects them to survive the call and libgcc's
! helpers, which the decoder calls, are built for GCC's ABI. The caller
! (firmware.c on_stack) lets one call in at a time.
    .text
    .align 2
    .global _flac_on_stack
    .type _flac_on_stack, @function
_flac_on_stack:
    mov.l   .Ltop, r1
    mov     r15, r2
    mov.l   r2, @-r1            ! the caller's r15
    sts.l   pr, @-r1
    sts.l   mach, @-r1
    sts.l   macl, @-r1
    mov     r1, r15             ! now on the private stack
    mov     r4, r1
    mov     r5, r4
    mov     r6, r5
    jsr     @r1
    mov     r7, r6
    lds.l   @r15+, macl
    lds.l   @r15+, mach
    lds.l   @r15+, pr
    rts
    mov.l   @r15, r15           ! back on the caller's stack
    .align 2
.Ltop:
    .long   flac_stack + 16384
    .size _flac_on_stack, . - _flac_on_stack

    .section .bss
    .align 3
    .global _flac_stack
flac_stack:
_flac_stack:
    .space  16384

/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The classifier's entry, moved: patch.py's EntryRedirect replaces the 12
 * bytes at 0x0C03EB94 with a jump into the payload. This runs them and goes
 * on at 0x0C03EBA0. The service-mode gate __gate_flac_file_classify jumps
 * here, and flac_file_classify calls it for the stock result. */
    .text
    .align 2

    .global _flac_stock_classify
_flac_stock_classify:
    .word 0x4cf0, 0x7fdc, 0x6c53, 0x6252, 0x6d43, 0x842b
    mov.l 1f,r3
    jmp @r3
    nop
    .align 2
1:  .long 0x0c03eba0

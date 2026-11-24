! SPDX-License-Identifier: GPL-2.0-or-later
!
! version_display hook for CDJ-850 MAIN 1.13 (SH-2A, big-endian).
! Assembled form: main_version_display.bin (rebuilt by patch_main.py --assemble).
!
! Entered from the shared display_stage_bpm tail trampoline (0x0C0642AA,
! 50 Hz in DisplayTask, outside bootcrit, before FUN_0C064CA4 serializes
! the dot-matrix rows to the panel). When display mode is 12 (UTILITY),
! menu level is 2 (main list), cursor is 6 (VERSION No.), and the running
! version string at 0x0CE6D242 (copied from flash 0x060010 at boot by
! version_label_init) is non-empty, rewrite dot-matrix row 2
! (0x0C9D0B18 + 2*0x105 = 0x0C9D0D22; len at +4 = 0x0C9D0D26, text at +5)
! to "Ver. <running> (1.13)", e.g. "Ver. 1.23 (1.13)".
!
! Clobbers r0..r5 only; the trampoline restores r0..r7.

	.text
	.align	2
version_display:
	mov.l	Lmode,r1		! r1 = 0x0C9D0AF8 (C850_DISPLAY_MODE)
	mov.l	@r1,r0
	cmp/eq	#12,r0			! mode 12 = UTILITY
	bf	done
	mov.l	Llevel,r1		! r1 = 0x0C1E108C (level), +1 = cursor
	mov.b	@r1+,r0
	cmp/eq	#2,r0			! level 2 = UTILITY main list
	bf	done
	mov.b	@r1,r0
	cmp/eq	#6,r0			! item 6 = VERSION No.
	bf	done
	mov.l	Lver,r1			! r1 = 0x0CE6D242 (running version string)
	mov.b	@r1,r0
	tst	r0,r0			! empty? keep stock "Ver. 1.13"
	bt	done
	mov.l	Lrow,r4			! r4 = 0x0C9D0D26 (row 2 len byte)
	mov	r4,r2
	add	#1,r2			! r2 = 0x0C9D0D27 (row 2 text[0])
	mova	Lprefix,r0		! r0 = "Ver. \0"
	mov	r0,r5
copy_pre:
	mov.b	@r5+,r0
	tst	r0,r0
	bt	pre_done
	mov.b	r0,@r2
	bra	copy_pre
	add	#1,r2
pre_done:
	mov	#15,r3			! bound copy to 15 chars (buffer is 16 B)
copy_ver:
	mov.b	@r1+,r0
	tst	r0,r0
	bt	ver_done
	mov.b	r0,@r2
	add	#1,r2
	dt	r3
	bf	copy_ver
ver_done:
	mova	Lbase,r0		! r0 = " (1.13)\0"
	mov	r0,r5
copy_base:
	mov.b	@r5+,r0
	tst	r0,r0
	bt	finish
	mov.b	r0,@r2
	bra	copy_base
	add	#1,r2
finish:
	sub	r4,r2			! r2 = len + 1
	add	#-1,r2			! r2 = len
	mov.b	r2,@r4			! record[4] = len
done:
	rts
	nop

	.align	2
Lmode:
	.long	0x0C9D0AF8
Llevel:
	.long	0x0C1E108C
Lver:
	.long	0x0CE6D242
Lrow:
	.long	0x0C9D0D26
Lprefix:
	.asciz	"Ver. "
	.align	2
Lbase:
	.asciz	" (1.13)"
	.align	2

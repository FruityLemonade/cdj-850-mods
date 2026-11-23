/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Linker script of the CDJ-850 shared payload (mods/payloadbuild.py).
 *
 * The loader (mods/c850loader.py) copies the payload to 0x0DE00020 at boot,
 * behind its 32-byte header at 0x0DE00000, and the region ends at the high
 * guard word 0x0DF6FFFC. Everything is laid out in one loadable image:
 * .bss sits inside the last output section, so `objcopy -O binary` writes it
 * as zeros and the loader's copy clears it (the region is not cleared by
 * MAIN's start-up). The image ends in the marker word ">PLD", which also
 * keeps that section PROGBITS when a payload has no .data of its own.
 */
OUTPUT_FORMAT("elf32-sh")
OUTPUT_ARCH(sh)
ENTRY(__payload_start)

MEMORY
{
    payload (rwx) : ORIGIN = 0x0DE00020, LENGTH = 0x0DF6FFFC - 0x0DE00020
}

SECTIONS
{
    .text : {
        __payload_start = .;
        *(.text .text.*)
    } > payload

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
        . = ALIGN(4);
        /* key filters (C850_UI_KEY_FILTER, c850_ui.h), between c850_ui.c's two
           sentinel words in .c850_ui_filters.a and .c850_ui_filters.z */
        KEEP(*(.c850_ui_filters.a))
        KEEP(*(.c850_ui_filters))
        KEEP(*(.c850_ui_filters.z))
        /* tick hooks (C850_UI_TICK_HOOK, c850_ui.h), between c850_ui.c's two
           sentinel words in .c850_ui_ticks.a and .c850_ui_ticks.z */
        KEEP(*(.c850_ui_ticks.a))
        KEEP(*(.c850_ui_ticks))
        KEEP(*(.c850_ui_ticks.z))
    } > payload

    .data : ALIGN(4) {
        *(.data .data.*)
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __bss_end = .;
        LONG(0x3E504C44)            /* ">PLD", the payload's end marker */
        __payload_end = .;
    } > payload

    /DISCARD/ : {
        *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.stack)
        *(.debug .debug_*)          /* libgcc's soft-float helpers carry DWARF */
        /* sh-elf ld reserves a dynamic-relocation section for references to
         * weak symbols (the shared tick, c850.h C850_LOOP_TICK_DEFINE) even in
         * this static link; it stays empty (payloadbuild checks the image has
         * no relocations left) */
        *(.rela.dyn) *(.rela.text)
    }
}

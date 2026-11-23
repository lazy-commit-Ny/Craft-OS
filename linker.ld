ENTRY(_start)

SECTIONS
{
    . = 0x80200000;
    .text : ALIGN(4K) {
        *(.text.boot)
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data : ALIGN(4K) {
        __global_pointer$ = . + 0x800;
        *(.sdata .sdata.*)
        *(.data .data.*)
    }
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss.stack)
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
    . = ALIGN(4K);
    __kernel_end = .;
}

.section .text.boot
.globl _start
.type _start, @function
_start:
    bnez a0, 1f
    .option push
    .option norelax
    la gp, __global_pointer$
    la sp, stack_top
    .option pop
    call kernel_main
1:
    wfi
    j 1b

.section .bss.stack, "aw", @nobits
.align 16
stack_bottom:
    .space 65536
stack_top:

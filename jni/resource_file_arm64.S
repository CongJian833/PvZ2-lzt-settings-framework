.text
.align 2
.global lzt_call_x8_1
.type lzt_call_x8_1, %function
lzt_call_x8_1:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    mov x0, x1
    mov x8, x2
    blr x9
    ldp x29, x30, [sp], #16
    ret
.size lzt_call_x8_1, .-lzt_call_x8_1

.align 2
.global lzt_call_x8_2
.type lzt_call_x8_2, %function
lzt_call_x8_2:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    mov x9, x0
    mov x0, x1
    mov x1, x2
    mov x8, x3
    blr x9
    ldp x29, x30, [sp], #16
    ret
.size lzt_call_x8_2, .-lzt_call_x8_2

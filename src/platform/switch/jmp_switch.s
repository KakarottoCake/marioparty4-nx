.text
.align 2
.global gcsetjmp
.type gcsetjmp, %function
gcsetjmp:
    // Store link register (x30) at offset 0
    str x30, [x0, #0]
    
    // Store stack pointer (sp) at offset 8
    mov x9, sp
    str x9, [x0, #8]
    
    // Store general purpose registers x19-x29 starting at offset 16
    stp x19, x20, [x0, #16]
    stp x21, x22, [x0, #32]
    stp x23, x24, [x0, #48]
    stp x25, x26, [x0, #64]
    stp x27, x28, [x0, #80]
    str x29, [x0, #96]
    
    // Store floating point registers d8-d15 starting at offset 104
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    
    // Return 0
    mov w0, #0
    ret

.global gclongjmp
.type gclongjmp, %function
gclongjmp:
    // Restore link register (x30) from offset 0
    ldr x30, [x0, #0]
    
    // Restore stack pointer (sp) from offset 8
    ldr x9, [x0, #8]
    mov sp, x9
    
    // Restore general purpose registers x19-x29
    ldp x19, x20, [x0, #16]
    ldp x21, x22, [x0, #32]
    ldp x23, x24, [x0, #48]
    ldp x25, x26, [x0, #64]
    ldp x27, x28, [x0, #80]
    ldr x29, [x0, #96]
    
    // Restore floating point registers d8-d15
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    
    // Return status (w1), if status is 0 return 1
    mov w0, w1
    cbnz w0, 1f
    mov w0, #1
1:
    ret

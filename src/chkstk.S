# Stack probe used by clang for frames larger than a page (same as libgcc's).
    .text
    .globl ___chkstk_ms
___chkstk_ms:
    push %rcx
    push %rax
    cmp $0x1000, %rax
    lea 24(%rsp), %rcx
    jb 1f
2:
    sub $0x1000, %rcx
    orq $0, (%rcx)
    sub $0x1000, %rax
    cmp $0x1000, %rax
    ja 2b
1:
    sub %rax, %rcx
    orq $0, (%rcx)
    pop %rax
    pop %rcx
    ret

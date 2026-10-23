# Replacement for the existing-material checks in the game's voxel blit callback
# (35 bytes at the patch site):
#     cmp r11b, 0x80 / jae SKIP            existing building voxel -> never touched
#     cmp r11, [r10+0x28] / jae SKIP       outside the "may write" table
#     mov rax, [r10+0x20] / cmp byte [r11+rax], 0 / je SKIP
#   then CONT (choose write path by new material) .
#
# Materials: 0 = air, 1..0x7f = terrain, 0x80..0xff = building materials.
# Building voxels are written by the WRITE path (material byte, second byte 0,
# volume += 1.0 per voxel).
#
# Registers at the patch site: r11 = existing material (zero-extended),
# r10 = blit parameters ([r10+0xa1] = material being placed), rcx = voxel index,
# rax = free.
#
# mode byte: 0 = vanilla, 1 = replace overlap only, 2 = replace full shape.
# allow[m] = 1 for materials Replace may repaint (removable at that spot).
# vols[m] counts repainted voxels per old material (refund).
#
# The blob is copied next to the game image at runtime (rip-relative data and the
# jump slots move with it).

    .intel_syntax noprefix
    .section .rdata,"dr"
    .p2align 4
    .globl bt_cave_start
    .globl bt_cave_end
    .globl bt_cave_mode
    .globl bt_cave_cont
    .globl bt_cave_skip
    .globl bt_cave_write
    .globl bt_cave_vols
    .globl bt_cave_allow
    .globl bt_cave_hits
bt_cave_start:
    cmp byte ptr [rip + bt_cave_mode], 0
    jne 1f
    # ---- original checks
0:  cmp r11b, 0x80
    jae 9f
5:  cmp r11, qword ptr [r10 + 0x28]
    jae 9f
    mov rax, qword ptr [r10 + 0x20]
    cmp byte ptr [r11 + rax], 0
    je 9f
    jmp qword ptr [rip + bt_cave_cont]

1:  # ---- mod replace (the new material is a building material)
    lock inc dword ptr [rip + bt_cave_hits]          # hits[0]: covered voxels seen
    cmp r11b, 0x80
    jb 3f
    # existing building voxel
    lock inc dword ptr [rip + bt_cave_hits + 4]      # hits[1]: building voxels seen
    cmp r11b, byte ptr [r10 + 0xa1]
    je 9f                                            # already the new material
    lea rax, [rip + bt_cave_allow]
    cmp byte ptr [rax + r11], 0
    je 9f                                            # not removable here (protected)
    lea rax, [rip + bt_cave_vols]
    lock inc dword ptr [rax + r11*4]                 # refund tally
    jmp qword ptr [rip + bt_cave_write]
3:  # air or terrain: overlap only leaves it, full shape builds it as usual
    cmp byte ptr [rip + bt_cave_mode], 1
    je 9f
    jmp 5b
9:  jmp qword ptr [rip + bt_cave_skip]

    .p2align 3
bt_cave_cont:  .quad 0
bt_cave_skip:  .quad 0
bt_cave_write: .quad 0
bt_cave_mode:  .byte 0
    .p2align 2
bt_cave_hits:  .long 0, 0
bt_cave_allow: .space 256
    .p2align 2
bt_cave_vols:  .space 256 * 4
bt_cave_end:

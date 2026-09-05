// SPDX-License-Identifier: MIT OR Apache-2.0
.text
.align 2
.hidden cdj_gate_cue_original_hot_change
.hidden cdj_gate_cue_original_hot_release
.hidden cdj_gate_cue_original_play_latch

/* Hot Cue hooks: preserve the OEM ABI and replace only x1 with the event
 * pointer returned by cdj_gate_cue_prepare(). */
.macro gate_event_entry name, identifier, original
.global \name
.hidden \name
.type \name, %function
\name:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov w0, #\identifier
    ldp x1, x2, [sp, #16]
    ldp x3, x4, [sp, #32]
    bl cdj_gate_cue_prepare
    str x0, [sp, #88]
    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    ldp x0, x1, [sp, #16]
    ldr x1, [sp, #88]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8, [sp, #80]
    ldp x29, x30, [sp], #224
    adrp x9, \original
    ldr x9, [x9, :lo12:\original]
    br x9
.size \name, .-\name
.endm

gate_event_entry cdj_gate_cue_hot_change, 1, cdj_gate_cue_original_hot_change
gate_event_entry cdj_gate_cue_hot_release, 2, cdj_gate_cue_original_hot_release

/* PLAY differs from the event hooks. The working 3.22 Gate Cue backend
 * consumes PLAY while a gate session is active: it sets the latch and returns
 * without invoking the stock PLAY handler. With no gate active, stock PLAY is
 * called with every original argument/register restored. */
.global cdj_gate_cue_play_latch
.hidden cdj_gate_cue_play_latch
.type cdj_gate_cue_play_latch, %function
cdj_gate_cue_play_latch:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]

    mov w0, #3
    ldp x1, x2, [sp, #16]
    ldp x3, x4, [sp, #32]
    bl cdj_gate_cue_prepare
    mov w9, w0

    ldp q0, q1, [sp, #96]
    ldp q2, q3, [sp, #128]
    ldp q4, q5, [sp, #160]
    ldp q6, q7, [sp, #192]
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8, [sp, #80]

    cbnz w9, .Lgate_play_consumed

    ldp x29, x30, [sp], #224
    adrp x9, cdj_gate_cue_original_play_latch
    ldr x9, [x9, :lo12:cdj_gate_cue_original_play_latch]
    br x9

.Lgate_play_consumed:
    ldp x29, x30, [sp], #224
    ret
.size cdj_gate_cue_play_latch, .-cdj_gate_cue_play_latch

.section .note.GNU-stack,"",%progbits

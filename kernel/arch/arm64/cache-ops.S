// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2014, Google Inc. All rights reserved
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <asm.h>
#include <arch/ops.h>
#include <arch/defines.h>

.text

.macro cache_range_op, cache op
    add     x2, x0, x1                  // calculate the end address
    bic     x3, x0, #(CACHE_LINE-1)     // align the start with a cache line
.Lcache_range_op_loop\@:
    \cache  \op, x3
    add     x3, x3, #CACHE_LINE
    cmp     x3, x2
    blo     .Lcache_range_op_loop\@
    dsb     sy
.endm

    /* void arch_flush_cache_range(addr_t start, size_t len); */
FUNCTION(arch_clean_cache_range)
    cache_range_op dc cvac         // clean cache to PoC by MVA
    ret

    /* void arch_flush_invalidate_cache_range(addr_t start, size_t len); */
FUNCTION(arch_clean_invalidate_cache_range)
    cache_range_op dc civac        // clean & invalidate dcache to PoC by MVA
    ret

    /* void arch_invalidate_cache_range(addr_t start, size_t len); */
FUNCTION(arch_invalidate_cache_range)
    cache_range_op dc ivac         // invalidate dcache to PoC by MVA
    ret

    /* void arch_sync_cache_range(addr_t start, size_t len); */
FUNCTION(arch_sync_cache_range)
    cache_range_op dc cvau         // clean dcache to PoU by MVA
    cache_range_op ic ivau         // invalidate icache to PoU by MVA
    isb
    ret


FUNCTION(arch_clean_dcache_all)
    MRS X0, CLIDR_EL1
    AND W3, W0, #0x07000000
    LSR W3, W3, #23
    CBZ W3, Finished
    MOV W10, #0
    MOV W8, #1
Loop1:
    add     w2, w10, w10, LSR #1
    LSR     W1, W0, W2
    AND     W1, W1, #0x7
    CMP     W1, #2
    B.LT    Skip
    MSR     CSSELR_EL1, X10
    ISB
    MRS     X1, CCSIDR_EL1
    and     W2, W1, #7
    ADD     W2, W2, #4
    UBFX    W4, W1, #3, #10
    CLZ     W5, W4
    LSL     W9, W4, W5

    LSL     W16, W8, W5

Loop2:
    UBFX    W7, W1, #13, #15
    LSL     W7, W7, W2
    LSL     W17, W8, W2
Loop3:
    ORR     W11, W10, W9
    ORR     W11, W11, W7
    DC      ISW, X11
    SUBS    W7, W7, W17
    B.GE    Loop3

    SUBS    X9, X9, X16
    B.GE    Loop2
Skip:
    add     w10, w10, #2
    CMP     W3, W10
    dsb     sy
    B.GT Loop1
Finished:
    ret


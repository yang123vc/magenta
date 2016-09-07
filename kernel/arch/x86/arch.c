// Copyright 2016 The Fuchsia Authors
// Copyright (c) 2009 Corey Tabaka
// Copyright (c) 2015 Intel Corporation
// Copyright (c) 2016 Travis Geiselbrecht
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT


#include <assert.h>
#include <magenta/compiler.h>
#include <debug.h>
#include <err.h>
#include <trace.h>
#include <assert.h>
#include <inttypes.h>
#include <arch.h>
#include <arch/ops.h>
#include <arch/mp.h>
#include <arch/x86.h>
#include <arch/x86/descriptor.h>
#include <arch/x86/feature.h>
#include <arch/x86/mmu.h>
#include <arch/x86/mmu_mem_types.h>
#include <arch/x86/mp.h>
#include <arch/mmu.h>
#include <kernel/vm.h>
#include <lib/console.h>
#include <lk/init.h>
#include <lk/main.h>
#include <platform.h>
#include <sys/types.h>
#include <string.h>

#define LOCAL_TRACE 0

/* early stack */
uint8_t _kstack[PAGE_SIZE] __ALIGNED(16);

/* save a pointer to the multiboot information coming in from whoever called us */
/* make sure it lives in .data to avoid it being wiped out by bss clearing */
__SECTION(".data") void *_multiboot_info;

/* also save a pointer to the boot_params structure */
__SECTION(".data") void *_zero_page_boot_params;

void arch_early_init(void)
{
    x86_mmu_percpu_init();
    x86_mmu_early_init();
}

void arch_init(void)
{
    x86_mmu_init();
}

void arch_chain_load(void *entry, ulong arg0, ulong arg1, ulong arg2, ulong arg3)
{
    PANIC_UNIMPLEMENTED;
}

void arch_enter_uspace(uintptr_t entry_point, uintptr_t sp,
                       uintptr_t arg1, uintptr_t arg2) {
    LTRACEF("entry %#" PRIxPTR " user stack %#" PRIxPTR "\n", entry_point, sp);

    arch_disable_ints();

#if ARCH_X86_32
    PANIC_UNIMPLEMENTED;
#elif ARCH_X86_64
    /* default user space flags:
     * IOPL 0
     * Interrupts enabled
     */
    ulong flags = (0 << X86_FLAGS_IOPL_SHIFT) | X86_FLAGS_IF;

    /* check that we're probably still pointed at the kernel gs */
    DEBUG_ASSERT(is_kernel_address(read_msr(X86_MSR_IA32_GS_BASE)));

    /* set up user's fs: gs: base */
    write_msr(X86_MSR_IA32_FS_BASE, 0);

    /* set the KERNEL_GS_BASE msr here, because we're going to swapgs below */
    write_msr(X86_MSR_IA32_KERNEL_GS_BASE, 0);

    x86_uspace_entry(arg1, arg2, sp, entry_point, flags);
    __UNREACHABLE;
#endif
}

#define IA32_RTIT_CTL 0x570
#define IA32_RTIT_OUTPUT_BASE 0x560
#define IA32_RTIT_OUTPUT_MASK_PTRS 0x561
struct list_node intel_pt_page_lists[SMP_MAX_CPUS];
void intel_pt_init(uint level)
{
    static const uint log2_buf_size = 27;

    uint64_t* table = memalign(PAGE_SIZE, 128);

    paddr_t table_paddr = vaddr_to_paddr(table);

    size_t pages_wanted = (1ULL << log2_buf_size) / PAGE_SIZE;
    paddr_t pa;

    uint cpu = arch_curr_cpu_num();
    if (cpu == 0) {
        for (uint i = 0; i < SMP_MAX_CPUS; ++i) {
            list_initialize(&intel_pt_page_lists[i]);
        }
    }
    size_t pages_alloced = pmm_alloc_contiguous(pages_wanted, 0, log2_buf_size, &pa,
                                &intel_pt_page_lists[cpu]);
    ASSERT(pages_alloced == pages_wanted);

    table[0] = pa;
    // Entry size: 128M
    table[0] |= 15ULL << 6;
    // STOP-bit
    table[0] |= 1ULL << 4;

    // END entry
    table[1] = table_paddr | 1;

    write_msr(IA32_RTIT_OUTPUT_BASE, table_paddr);
    write_msr(IA32_RTIT_OUTPUT_MASK_PTRS, 0);
    write_msr(IA32_RTIT_CTL, 0x250d);
}
LK_INIT_HOOK_FLAGS(intel_pt, &intel_pt_init, LK_INIT_LEVEL_VM + 3, LK_INIT_FLAG_PRIMARY_CPU);

uint64_t intel_pt_trace_sizes[SMP_MAX_CPUS];
static void intel_pt_toggle_trace(void* raw_context) {
    if (read_msr(IA32_RTIT_CTL) & 1) {
        write_msr(IA32_RTIT_CTL, 0);
        uint cpu = arch_curr_cpu_num();
        intel_pt_trace_sizes[cpu] = read_msr(IA32_RTIT_OUTPUT_MASK_PTRS) >> 32;
    } else {
        write_msr(IA32_RTIT_OUTPUT_MASK_PTRS, 0);
        write_msr(IA32_RTIT_CTL, 0x250d);
    }
}

#if WITH_SMP
#include <arch/x86/apic.h>
void x86_secondary_entry(volatile int *aps_still_booting, thread_t *thread)
{
    // Would prefer this to be in init_percpu, but there is a dependency on a
    // page mapping existing, and the BP calls that before the VM subsystem is
    // initialized.
    apic_local_init();

    uint32_t local_apic_id = apic_local_id();
    int cpu_num = x86_apic_id_to_cpu_num(local_apic_id);
    if (cpu_num < 0) {
        // If we could not find our CPU number, do not proceed further
        arch_disable_ints();
        while (1) {
            x86_hlt();
        }
    }

    DEBUG_ASSERT(cpu_num > 0);
    x86_init_percpu((uint)cpu_num);

    // Signal that this CPU is initialized.  It is important that after this
    // operation, we do not touch any resources associated with bootstrap
    // besides our thread_t and stack, since this is the checkpoint the
    // bootstrap process uses to identify completion.
    int old_val = atomic_and(aps_still_booting, ~(1U << cpu_num));
    if (old_val == 0) {
        // If the value is already zero, then booting this CPU timed out.
        goto fail;
    }

    // Defer configuring memory settings until after the atomic_and above.
    // This ensures that we were in no-fill cache mode for the duration of early
    // AP init.
    DEBUG_ASSERT(x86_get_cr0() & X86_CR0_CD);
    x86_mmu_percpu_init();

    // Load the appropriate PAT/MTRRs.  This must happen after init_percpu, so
    // that this CPU is considered online.
    x86_pat_sync(1U << cpu_num);

    /* run early secondary cpu init routines up to the threading level */
    lk_init_level(LK_INIT_FLAG_SECONDARY_CPUS, LK_INIT_LEVEL_EARLIEST, LK_INIT_LEVEL_THREADING - 1);

    thread_secondary_cpu_init_early(thread);
    /* The thread stack and struct are from a single allocation, free it when we
     * exit into the scheduler */
    thread->flags |= THREAD_FLAG_FREE_STRUCT;
    lk_secondary_cpu_entry();

    // lk_secondary_cpu_entry only returns on an error, halt the core in this
    // case.
fail:
    arch_disable_ints();
    while (1) {
      x86_hlt();
    }
}
#endif

static int cmd_cpu(int argc, const cmd_args *argv)
{
    if (argc < 2) {
notenoughargs:
        printf("not enough arguments\n");
usage:
        printf("usage:\n");
        printf("%s features\n", argv[0].str);
        printf("%s unplug <cpu_id>\n", argv[0].str);
        printf("%s hotplug <cpu_id>\n", argv[0].str);
        printf("%s toggle-trace\n", argv[0].str);
        return ERR_INTERNAL;
    }

    if (!strcmp(argv[1].str, "features")) {
        x86_feature_debug();
    } else if (!strcmp(argv[1].str, "unplug")) {
        if (argc < 3) {
            printf("specify a cpu_id\n");
            goto usage;
        }
        status_t status = ERR_NOT_SUPPORTED;
#if WITH_SMP
        status = mp_unplug_cpu(argv[2].u);
#endif
        printf("CPU %lu unplugged: %d\n", argv[2].u, status);
    } else if (!strcmp(argv[1].str, "hotplug")) {
        if (argc < 3) {
            printf("specify a cpu_id\n");
            goto usage;
        }
        status_t status = ERR_NOT_SUPPORTED;
#if WITH_SMP
        status = mp_hotplug_cpu(argv[2].u);
#endif
        printf("CPU %lu hotplugged: %d\n", argv[2].u, status);
    } else if (!strcmp(argv[1].str, "toggle-trace")) {
        mp_sync_exec(MP_CPU_ALL, intel_pt_toggle_trace, NULL);
        uint64_t total = 0;
        for (uint i = 0; i < SMP_MAX_CPUS; ++i) {
            printf("CPU %u got %lld bytes (%llx)\n", i, intel_pt_trace_sizes[i], intel_pt_trace_sizes[i]);
            total += intel_pt_trace_sizes[i];
        }
        printf("Total: %lld bytes (%llx)\n", total, total);
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("cpu", "cpu test commands", &cmd_cpu)
#endif
STATIC_COMMAND_END(cpu);

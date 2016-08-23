// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <acpica/acpi.h>
#include <assert.h>
#include <err.h>
#include <trace.h>

#include <arch/x86/apic.h>
#include <arch/x86/bootstrap16.h>
#include <arch/x86/descriptor.h>
#include <arch/x86/tsc.h>
#include <lib/console.h>
#include <lk/init.h>
#include <kernel/port.h>
#include <kernel/mp.h>
#include <kernel/timer.h>
#include <platform/pc/acpi.h>
#include "platform_p.h"

extern status_t acpi_get_madt_record_limits(uintptr_t *start, uintptr_t *end);

static void acpi_debug_madt(void)
{
    uintptr_t records_start, records_end;
    status_t status = acpi_get_madt_record_limits(&records_start, &records_end);
    if (status != AE_OK) {
        printf("Invalid MADT\n");
        return;
    }

    uintptr_t addr;
    for (addr = records_start; addr < records_end;) {
        ACPI_SUBTABLE_HEADER *record_hdr = (ACPI_SUBTABLE_HEADER *)addr;
        printf("Entry type %2d ", record_hdr->Type);
        switch (record_hdr->Type) {
            case ACPI_MADT_TYPE_LOCAL_APIC: {
                ACPI_MADT_LOCAL_APIC *apic = (ACPI_MADT_LOCAL_APIC *)record_hdr;
                printf("Local APIC\n");
                printf("  ACPI id: %02x\n", apic->ProcessorId);
                printf("  APIC id: %02x\n", apic->Id);
                printf("  flags: %08x\n", apic->LapicFlags);
                break;
            }
            case ACPI_MADT_TYPE_IO_APIC: {
                ACPI_MADT_IO_APIC *apic = (ACPI_MADT_IO_APIC *)record_hdr;
                printf("IO APIC\n");
                printf("  APIC id: %02x\n", apic->Id);
                printf("  phys: %08x\n", apic->Address);
                printf("  global IRQ base: %08x\n", apic->GlobalIrqBase);
                break;
            }
            case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {
                ACPI_MADT_INTERRUPT_OVERRIDE *io =
                        (ACPI_MADT_INTERRUPT_OVERRIDE *)record_hdr;
                printf("Interrupt Source Override\n");
                printf("  bus: %02x (ISA==0)\n", io->Bus);
                printf("  source IRQ: %02x\n", io->SourceIrq);
                printf("  global IRQ: %08x\n", io->GlobalIrq);
                const char *trigger = "";
                const char *polarity = "";
                switch (io->IntiFlags & ACPI_MADT_POLARITY_MASK) {
                    case ACPI_MADT_POLARITY_CONFORMS:
                        polarity = "conforms"; break;
                    case ACPI_MADT_POLARITY_ACTIVE_HIGH:
                        polarity = "high"; break;
                    case ACPI_MADT_POLARITY_RESERVED:
                        polarity = "invalid"; break;
                    case ACPI_MADT_POLARITY_ACTIVE_LOW:
                        polarity = "low"; break;
                }
                switch (io->IntiFlags & ACPI_MADT_TRIGGER_MASK) {
                    case ACPI_MADT_TRIGGER_CONFORMS:
                        trigger = "conforms"; break;
                    case ACPI_MADT_TRIGGER_EDGE:
                        trigger = "edge"; break;
                    case ACPI_MADT_TRIGGER_RESERVED:
                        trigger = "invalid"; break;
                    case ACPI_MADT_TRIGGER_LEVEL:
                        trigger = "level"; break;
                }

                printf("  flags: %04x (trig %s, pol %s)\n",
                       io->IntiFlags, trigger, polarity);
                break;
            }
            case ACPI_MADT_TYPE_LOCAL_APIC_NMI: {
                ACPI_MADT_LOCAL_APIC_NMI *nmi =
                        (ACPI_MADT_LOCAL_APIC_NMI *)record_hdr;
                printf("Local APIC NMI\n");
                printf("  ACPI processor id: %02x\n", nmi->ProcessorId);
                printf("  flags: %04x\n", nmi->IntiFlags);
                printf("  LINTn: %02x\n", nmi->Lint);
                break;
            }
            default:
                printf("Unknown\n");
        }

        addr += record_hdr->Length;
    }
    if (addr != records_end) {
      printf("malformed MADT, last record past the end of the table\n");
    }
}

static void acpi_debug_mcfg(void)
{
    ACPI_TABLE_HEADER *raw_table = NULL;
    ACPI_STATUS status = AcpiGetTable((char *)ACPI_SIG_MCFG, 1, &raw_table);
    if (status != AE_OK) {
        printf("could not find MCFG\n");
        return;
    }
    ACPI_TABLE_MCFG *mcfg = (ACPI_TABLE_MCFG *)raw_table;
    ACPI_MCFG_ALLOCATION *table_start = ((void *)mcfg) + sizeof(*mcfg);
    ACPI_MCFG_ALLOCATION *table_end = ((void *)mcfg) + mcfg->Header.Length;
    ACPI_MCFG_ALLOCATION *table;
    int count = 0;
    if (table_start + 1 > table_end) {
        printf("MCFG has unexpected size\n");
        return;
    }
    for (table = table_start; table < table_end; ++table) {
        printf("Controller %d:\n", count);
        printf("  Physical address: %#016llx\n", table->Address);
        printf("  Segment group: %04x\n", table->PciSegment);
        printf("  Start bus number: %02x\n", table->StartBusNumber);
        printf("  End bus number: %02x\n", table->EndBusNumber);
        ++count;
    }
    if (table != table_end) {
        printf("MCFG has unexpected size\n");
        return;
    }
}

static void acpi_debug_ecdt(void) {
    ACPI_TABLE_HEADER *raw_table = NULL;
    ACPI_STATUS status = AcpiGetTable((char *)ACPI_SIG_ECDT, 1, &raw_table);
    if (status != AE_OK) {
        printf("could not find ECDT\n");
        return;
    }
    ACPI_TABLE_ECDT *ecdt = (ACPI_TABLE_ECDT *)raw_table;
    printf("  Control: %d %#016llx\n", ecdt->Control.SpaceId, ecdt->Control.Address);
    printf("  Data: %d %#016llx\n", ecdt->Data.SpaceId, ecdt->Data.Address);
    printf("  GPE: %#x\n", ecdt->Gpe);
    printf("  Path: %s\n", (char*)ecdt->Id);
}

#if 0
#if ARCH_X86_64
static bool acpi_sleep_prep(uint64_t registers_ptr, vmm_aspace_t **aspace) {
    vmm_aspace_t *bootstrap_aspace = NULL;
    vmm_aspace_t *kernel_aspace = vmm_get_kernel_aspace();

    struct x86_realmode_entry_data *bootstrap_data = NULL;

    status_t status = x86_bootstrap16_prep(
            PHYS_BOOTSTRAP_PAGE,
            (uintptr_t)_x86_suspend_wakeup,
            &bootstrap_aspace,
            (void **)&bootstrap_data);
    if (status != NO_ERROR) {
        return false;
    }

    bootstrap_data->registers_ptr = registers_ptr;
    vmm_free_region(kernel_aspace, (vaddr_t)bootstrap_data);
    *aspace = bootstrap_aspace;
    return true;
}

struct x86_64_registers {
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rsp, rip;
};

static void acpi_sleep(void)
{
    /* Shutdown all of the secondary CPUs */
    mp_cpu_mask_t original_cpus = mp_get_online_mask();
    TRACEF("Starting suspend sequence: online cpu mask: %08x\n", original_cpus);
    mp_cpu_mask_t cpus = original_cpus & ~0x1;
    while (cpus & ~0x1) {
        uint target_cpu = __builtin_ffsl(cpus) - 1;
        TRACEF("Shutting down cpu %d\n", target_cpu);
        status_t status = mp_unplug_cpu(target_cpu);
        TRACEF("Shut down cpu %d with status %d\n", target_cpu, status);
        cpus = mp_get_online_mask() & ~0x1;
    }

    struct x86_64_registers registers;

    vmm_aspace_t *bootstrap_aspace;
    TRACEF("Prepping suspend...\n");
    bool ready = acpi_sleep_prep((uint64_t)&registers, &bootstrap_aspace);
    if (!ready) {
        TRACEF("Failed to prep for suspend\n");
        return;
    }
    TRACEF("About to suspend...\n");
    DEBUG_ASSERT(!arch_ints_disabled());
    arch_disable_ints();

    thread_t *curr_thread = get_current_thread();

    /* Save fs_base and gs_base in case they changed in usermode since last
     * context switch */
    curr_thread->arch.fs_base = read_msr(X86_MSR_IA32_FS_BASE);
    curr_thread->arch.gs_base = read_msr(X86_MSR_IA32_KERNEL_GS_BASE);

    /* Save current TSC position so we can restore it */
    x86_tsc_store_adjustment();

    x86_extended_register_save_state(curr_thread->arch.extended_register_state);

    __UNUSED ACPI_STATUS status = AcpiSetFirmwareWakingVector(PHYS_BOOTSTRAP_PAGE, PHYS_BOOTSTRAP_PAGE);
    DEBUG_ASSERT(status == AE_OK);
    status = AcpiEnterSleepStatePrep(3);
    DEBUG_ASSERT(status == AE_OK || status == AE_NOT_FOUND);

    extern ACPI_STATUS x86_do_suspend(struct x86_64_registers* registers);
    printf("Suspending\n");
    status = x86_do_suspend(&registers);
    DEBUG_ASSERT(status == AE_OK);

    platform_init_debug_early();
    x86_init_percpu(0);
    x86_mmu_percpu_init();

    status = AcpiLeaveSleepStatePrep(3);
    DEBUG_ASSERT(status == AE_OK || status == AE_NOT_FOUND);

    status = AcpiLeaveSleepState(3);
    DEBUG_ASSERT(status == AE_OK || status == AE_NOT_FOUND);

    x86_extended_register_restore_state(curr_thread->arch.extended_register_state);

    /* Free the bootstrap resources we used. */
    vmm_free_aspace(bootstrap_aspace);

    /* Reload usermode fs/gs.  Real kernelmode gs is loaded via x86_init_percpu */
    write_msr(X86_MSR_IA32_FS_BASE, curr_thread->arch.fs_base);
    write_msr(X86_MSR_IA32_KERNEL_GS_BASE, curr_thread->arch.gs_base);
    apic_local_init();

    timer_thaw_percpu();

    arch_enable_ints();
    TRACEF("Resumed, waking up other CPUs\n");

    cpus = original_cpus & ~0x1;
    while (cpus) {
        uint target_cpu = __builtin_ffsl(cpus) - 1;
        TRACEF("Starting up cpu %d\n", target_cpu);
        status_t status = mp_hotplug_cpu(target_cpu);
        TRACEF("Started up cpu %d with status %d\n", target_cpu, status);
        cpus &= ~(1 << target_cpu);
    }
    TRACEF("Done waking up other CPUs\n");
}
#endif
#endif

#undef INDENT_PRINTF

static int cmd_acpi(int argc, const cmd_args *argv)
{
    if (argc < 2) {
notenoughargs:
        printf("not enough arguments\n");
usage:
        printf("usage:\n");
        printf("%s madt\n", argv[0].str);
        printf("%s mcfg\n", argv[0].str);
        printf("%s ecdt\n", argv[0].str);
        return ERR_INTERNAL;
    }

    if (!strcmp(argv[1].str, "madt")) {
        acpi_debug_madt();
    } else if (!strcmp(argv[1].str, "mcfg")) {
        acpi_debug_mcfg();
    } else if (!strcmp(argv[1].str, "ecdt")) {
        acpi_debug_ecdt();
    } else {
        printf("unknown command\n");
        goto usage;
    }

    return NO_ERROR;
}

STATIC_COMMAND_START
#if LK_DEBUGLEVEL > 0
STATIC_COMMAND("acpi", "acpi commands", &cmd_acpi)
#endif
STATIC_COMMAND_END(acpi);

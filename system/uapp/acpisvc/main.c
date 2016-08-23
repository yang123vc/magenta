// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>

#include <acpica/acpi.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls-ddk.h>
#include <mxio/util.h>

#include "ec.h"
#include "pci.h"
#include "powerbtn.h"
#include "processor.h"

#define ACPI_MAX_INIT_TABLES 32

static ACPI_STATUS set_apic_irq_mode(void);
static ACPI_STATUS init(void);

int main(int argc, char **argv) {
    // Get the msg pipe handle that will serve as the root channel.  devmgr
    // should be the owner of the other end.
    mx_handle_t acpi_root = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_USER1, 0));
    if (acpi_root <= 0) {
        printf("Failed to find ACPI root handle\n");
        return 1;
    }

#if 0
    mx_handle_t pcie_ready = mxio_get_startup_handle(MX_HND_INFO(MX_HND_TYPE_USER2, 0));
    if (pcie_ready <= 0) {
        printf("Failed to find pcie_ready handle\n");
        return 2;
    }
#endif

    ACPI_STATUS status = init();
    if (status != NO_ERROR) {
        printf("Failed to initialize ACPI\n");
        return 3;
    }
    printf("Initialized ACPI\n");

    ec_init();

#if 0
    mx_pci_init_arg_t* pci_init_arg;
    mx_status_t mx_status = get_pci_init_arg(&pci_init_arg);
    if (mx_status == NO_ERROR) {
        size_t size = sizeof(*pci_init_arg) +
                sizeof(pci_init_arg->ecam_windows[0]) * pci_init_arg->ecam_window_count;
        mx_status = mx_pci_init(pci_init_arg, size);
        if (mx_status != NO_ERROR) {
            printf("Failed to initialize PCIe: %d\n", mx_status);
        }
    } else {
        printf("Failed to get PCIe init info: %d\n", mx_status);
    }

    // Signal to devmgr that PCIe is up
    mx_object_signal(pcie_ready, MX_SIGNAL_SIGNAL0, 0);
    mx_handle_close(pcie_ready);
#endif

    mx_status_t mx_status = install_powerbtn_handlers();
    if (mx_status != NO_ERROR) {
        printf("Failed to install powerbtn handler\n");
    }

    return begin_processing(acpi_root);
}

static ACPI_STATUS init(void) {
    // This sequence is described in section 10.1.2.1 (ACPICA Initialization With
    // Early ACPI Table Access) of the ACPICA developer's reference.
    ACPI_STATUS status = AcpiInitializeSubsystem();
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI\n");
        return status;
    }

    status = AcpiInitializeTables(NULL, ACPI_MAX_INIT_TABLES, FALSE);
    if (status == AE_NOT_FOUND) {
        printf("WARNING: could not find ACPI tables\n");
        return status;
    } else if (status == AE_NO_MEMORY) {
        printf("WARNING: could not initialize ACPI tables\n");
        return status;
    } else if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI tables for unknown reason\n");
        return status;
    }

    status = AcpiLoadTables();
    if (status != AE_OK) {
        printf("WARNING: could not load ACPI tables: %d\n", status);
        return status;
    }

    status = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not enable ACPI\n");
        return status;
    }

    status = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
    if (status != AE_OK) {
        printf("WARNING: could not initialize ACPI objects\n");
        return status;
    }

    status = set_apic_irq_mode();
    if (status == AE_NOT_FOUND) {
        printf("WARNING: Could not find ACPI IRQ mode switch\n");
    } else if (status != AE_OK) {
        printf("Failed to set APIC IRQ mode\n");
        return status;
    }

    // TODO(teisenbe): Maybe back out of ACPI mode on failure, but we rely on
    // ACPI for some critical things right now, so failure will likely prevent
    // successful boot anyway.
    return AE_OK;
}

/* @brief Switch interrupts to APIC model (controls IRQ routing) */
static ACPI_STATUS set_apic_irq_mode(void)
{
    ACPI_OBJECT selector = {
        .Integer.Type = ACPI_TYPE_INTEGER,
        .Integer.Value = 1, // 1 means APIC mode according to ACPI v5 5.8.1
    };
    ACPI_OBJECT_LIST params = {
        .Count =  1,
        .Pointer = &selector,
    };
    return AcpiEvaluateObject(NULL, (char *)"\\_PIC", &params, NULL);
}

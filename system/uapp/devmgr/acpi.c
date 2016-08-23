// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "acpi.h"

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <launchpad/launchpad.h>

#include <ddk/acpi-proto.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls-ddk.h>

#include "vfs.h"

static mx_handle_t acpi_root;

mx_status_t devmgr_launch_acpisvc(void) {
    const char* binname = "/boot/bin/acpisvc";

    const char* args[3] = {
        binname,
    };

    mx_handle_t pipe[2];
    mx_status_t status = mx_message_pipe_create(pipe, 0);
    if (status != NO_ERROR) {
        return status;
    }

    mx_handle_t hnd[2];
    uint32_t ids[2];
    // TODO(teisenbe): Does acpisvc need FS access?
    ids[0] = MX_HND_TYPE_MXIO_ROOT;
    hnd[0] = vfs_create_root_handle();
    ids[1] = MX_HND_TYPE_USER1;
    hnd[1] = pipe[1];
    printf("devmgr: launch acpisvc\n");
    mx_handle_t proc = launchpad_launch("acpisvc", 1, args, NULL, 2, hnd, ids);
    if (proc < 0) {
        printf("devmgr: acpisvc launch failed: %d\n", proc);
        mx_handle_close(pipe[0]);
        mx_handle_close(pipe[1]);
        return proc;
    }

    mx_handle_close(proc);
    acpi_root = pipe[0];

    return NO_ERROR;
}

// TODO(teisenbe): Instead of doing this as a single function, give the kpci
// driver a handle to the PCIe root complex ACPI node and let it ask for
// the initialization info.
mx_status_t devmgr_init_pcie(void) {
    mx_nanosleep(1e9);
    // TODO: Move most of this into a client library
    struct acpi_cmd_list_children list_children_cmd = {
        .hdr = {
            .version = 0,
            .cmd = ACPI_CMD_LIST_CHILDREN,
            .len = sizeof(list_children_cmd),
        },
    };

    mx_status_t status = mx_message_write(
            acpi_root, &list_children_cmd, sizeof(list_children_cmd),
            NULL, 0, 0);
    if (status != NO_ERROR) {
        return status;
    }

    while (1) {
        mx_signals_state_t state;
        status = mx_handle_wait_one(acpi_root,
                                    MX_SIGNAL_READABLE,
                                    MX_TIME_INFINITE,
                                    &state);
        if (status != NO_ERROR) {
            continue;
        }
        if (!(state.satisfied & MX_SIGNAL_READABLE)) {
            continue;
        }
        break;
    }

    uint32_t len = 0;
    status = mx_message_read(acpi_root, NULL, &len, NULL, NULL, 0);
    if (status != ERR_NOT_ENOUGH_BUFFER) {
        return status;
    }
    if (len < sizeof(struct acpi_rsp_hdr)) {
        return ERR_BAD_STATE;
    }

    struct acpi_rsp_list_children* list_children_rsp = malloc(len);
    if (!list_children_rsp) {
        return ERR_NO_MEMORY;
    }

    status = mx_message_read(acpi_root, list_children_rsp, &len, NULL, NULL, 0);
    if (status != NO_ERROR) {
        free(list_children_rsp);
        return status;
    }

    if (list_children_rsp->hdr.len != len) {
        free(list_children_rsp);
        return ERR_BAD_STATE;
    }
    if (list_children_rsp->hdr.status != NO_ERROR) {
        status = list_children_rsp->hdr.status;
        free(list_children_rsp);
        return status;
    }
    if (len != sizeof(*list_children_rsp) + sizeof(list_children_rsp->children[0]) * list_children_rsp->num_children) {
        free(list_children_rsp);
        return ERR_BAD_STATE;
    }

    char name[4] = { 0 };
    for (uint32_t i = 0; i < list_children_rsp->num_children; ++i) {
        if (!memcmp(list_children_rsp->children[i].hid, "PNP0A08", 7)) {
            memcpy(name, list_children_rsp->children[i].name, 4);
            break;
        }
    }
    free(list_children_rsp);
    list_children_rsp = NULL;

    struct acpi_cmd_get_child_handle get_child_handle_cmd = {
        .hdr = {
            .version = 0,
            .cmd = ACPI_CMD_GET_CHILD_HANDLE,
            .len = sizeof(get_child_handle_cmd),
        },
    };
    memcpy(get_child_handle_cmd.name, name, 4);

    status = mx_message_write(
            acpi_root, &get_child_handle_cmd, sizeof(get_child_handle_cmd),
            NULL, 0, 0);
    if (status != NO_ERROR) {
        return status;
    }

    while (1) {
        mx_signals_state_t state;
        status = mx_handle_wait_one(acpi_root,
                                    MX_SIGNAL_READABLE,
                                    MX_TIME_INFINITE,
                                    &state);
        if (status != NO_ERROR) {
            continue;
        }
        if (!(state.satisfied & MX_SIGNAL_READABLE)) {
            continue;
        }
        break;
    }

    struct acpi_rsp_get_child_handle get_child_handle_rsp;
    len = sizeof(get_child_handle_rsp);
    uint32_t num_handles = 1;
    mx_handle_t pcie_handle;
    status = mx_message_read(acpi_root, &get_child_handle_rsp, &len, &pcie_handle, &num_handles, 0);
    if (status != NO_ERROR) {
        return status;
    }
    if (len != sizeof(get_child_handle_rsp)) {
        return ERR_BAD_STATE;
    }
    if (get_child_handle_rsp.hdr.status != NO_ERROR) {
        if (num_handles > 0) {
            mx_handle_close(pcie_handle);
        }
        return get_child_handle_rsp.hdr.status;
    }
    if (num_handles == 0) {
        return ERR_BAD_STATE;
    }

    struct acpi_cmd_get_pci_init_arg get_pci_init_arg_cmd = {
        .hdr = {
            .version = 0,
            .cmd = ACPI_CMD_GET_PCI_INIT_ARG,
            .len = sizeof(get_pci_init_arg_cmd),
        },
    };

    status = mx_message_write(
            pcie_handle, &get_pci_init_arg_cmd, sizeof(get_pci_init_arg_cmd),
            NULL, 0, 0);
    if (status != NO_ERROR) {
        return status;
    }

    while (1) {
        mx_signals_state_t state;
        status = mx_handle_wait_one(pcie_handle,
                                    MX_SIGNAL_READABLE,
                                    MX_TIME_INFINITE,
                                    &state);
        if (status != NO_ERROR) {
            continue;
        }
        if (!(state.satisfied & MX_SIGNAL_READABLE)) {
            continue;
        }
        break;
    }

    len = 0;
    status = mx_message_read(pcie_handle, NULL, &len, NULL, NULL, 0);
    if (status != ERR_NOT_ENOUGH_BUFFER) {
        return status;
    }
    if (len < sizeof(struct acpi_rsp_hdr)) {
        return ERR_BAD_STATE;
    }

    struct acpi_rsp_get_pci_init_arg* get_pci_init_arg_rsp = malloc(len);
    if (!get_pci_init_arg_rsp) {
        return ERR_NO_MEMORY;
    }

    status = mx_message_read(pcie_handle, get_pci_init_arg_rsp, &len, NULL, NULL, 0);
    if (status != NO_ERROR) {
        free(get_pci_init_arg_rsp);
        return status;
    }

    if (get_pci_init_arg_rsp->hdr.len != len) {
        free(get_pci_init_arg_rsp);
        return ERR_BAD_STATE;
    }
    if (get_pci_init_arg_rsp->hdr.status != NO_ERROR) {
        status = get_pci_init_arg_rsp->hdr.status;
        free(get_pci_init_arg_rsp);
        return status;
    }

    len = get_pci_init_arg_rsp->hdr.len - offsetof(struct acpi_rsp_get_pci_init_arg, arg);
    status = mx_pci_init(&get_pci_init_arg_rsp->arg, len);
    if (status != NO_ERROR) {
        free(get_pci_init_arg_rsp);
        return status;
    }

    free(get_pci_init_arg_rsp);
    // TODO: don't leak this handle on errors
    mx_handle_close(pcie_handle);
    return NO_ERROR;
}

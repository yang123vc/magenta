// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/types.h>
#include <system/compiler.h>

enum {
    ACPI_CMD_LIST_CHILDREN = 1,
    ACPI_CMD_GET_CHILD_HANDLE = 2,
    ACPI_CMD_GET_PCI_INIT_ARG = 3,
};

struct acpi_cmd_hdr {
    uint32_t len; // Total length, including header
    uint16_t cmd; // CMD code from above enum
    uint8_t version; // Protocol version, currently only 0 defined.
    uint8_t _reserved;
} __PACKED;

struct acpi_rsp_hdr {
    mx_status_t status;
    uint32_t len;
} __PACKED;

// List all children of the node associated with the handle used to issue the
// request.
struct acpi_cmd_list_children {
    struct acpi_cmd_hdr hdr;
} __PACKED;
struct acpi_rsp_list_children {
    struct acpi_rsp_hdr hdr;

    uint32_t num_children;
    struct {
        // All of these values are non-NULL terminated.  name is a unique
        // identifier (scoped to the handle associated with the request)
        // that may be used to request a handle to a child below.
        char name[4];
        char hid[8];
        // We return the first 4 PNP/ACPI IDs found in the CID list
        char cid[4][8];
    } children[];
} __PACKED;

// Request a handle to a child node by name
struct acpi_cmd_get_child_handle {
    struct acpi_cmd_hdr hdr;

    char name[4];
} __PACKED;
struct acpi_rsp_get_child_handle {
    struct acpi_rsp_hdr hdr;
} __PACKED;

// Request information for initializing a PCIe bus.  Only valid if
// the associated node corresponds to a PCI root bridge.
struct acpi_cmd_get_pci_init_arg {
    struct acpi_cmd_hdr hdr;
} __PACKED;
struct acpi_rsp_get_pci_init_arg {
    struct acpi_rsp_hdr hdr;

    mx_pci_init_arg_t arg;
} __PACKED;

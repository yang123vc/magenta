// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include "completion.h"
#include "xhci.h"
#include "xhci_transfer.h"

typedef struct {
    xhci_command_context_t context;
    completion_t completion;
    // from command completion event TRB
    uint32_t status;
    uint32_t control;
} xhci_sync_command_t;

typedef struct {
    xhci_transfer_context_t context;
    completion_t completion;
    int result;
} xhci_sync_transfer_t;

void xhci_sync_command_init(xhci_sync_command_t* command);

// returns condition code
int xhci_sync_command_wait(xhci_sync_command_t* command);

inline int xhci_sync_command_slot_id(xhci_sync_command_t* command) {
    return (command->control & XHCI_MASK(TRB_SLOT_ID_START, TRB_SLOT_ID_BITS)) >> TRB_SLOT_ID_START;
}

void xhci_sync_transfer_init(xhci_sync_transfer_t* xfer);
mx_status_t xhci_sync_transfer_wait(xhci_sync_transfer_t* xfer);

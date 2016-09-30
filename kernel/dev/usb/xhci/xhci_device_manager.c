// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <endian.h>
#include <magenta/hw/usb-hub.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kernel/thread.h>

#include "xhci_device_manager.h"
#include "xhci_root_hub.h"
#include "xhci_util.h"

//#define TRACE 1
#include "xhci_debug.h"

// list of devices pending result of enable slot command
// list is kept on xhci_t.command_queue
typedef struct {
    enum {
        ENUMERATE_DEVICE,
        DISCONNECT_DEVICE,
        START_ROOT_HUBS,
    } command;
    list_node_t node;
    uint32_t hub_address;
    uint32_t port;
    usb_speed_t speed;
} xhci_device_command_t;

static uint32_t xhci_get_route_string(xhci_t* xhci, uint32_t hub_address, uint32_t port) {
    if (hub_address == 0) {
        return 0;
    }

    xhci_slot_t* hub_slot = &xhci->slots[hub_address];
    uint32_t route = XHCI_GET_BITS32(&hub_slot->sc->sc0, SLOT_CTX_ROUTE_STRING_START, SLOT_CTX_ROUTE_STRING_BITS);
    int shift = 0;
    while (shift < 20) {
        if ((route & (0xF << shift)) == 0) {
            // reached end of parent hub's route string
            route |= ((port & 0xF) << shift);
            break;
        }
        shift += 4;
    }
    return route;
}

static mx_status_t xhci_address_device(xhci_t* xhci, uint32_t slot_id, uint32_t hub_address,
                                       uint32_t port, usb_speed_t speed) {
    xprintf("xhci_address_device slot_id: %d port: %d hub_address: %d speed: %d\n",
            slot_id, port, hub_address, speed);

    int rh_index = xhci_get_root_hub_index(xhci, hub_address);
    if (rh_index >= 0) {
        // For virtual root hub devices, real hub_address is 0
        hub_address = 0;
        // convert virtual root hub port number to real port number
        port = xhci->root_hubs[rh_index].port_map[port - 1] + 1;
    }

    xhci_slot_t* slot = &xhci->slots[slot_id];
    if (slot->sc)
        return ERR_BAD_STATE;
    slot->hub_address = hub_address;
    slot->port = port;
    slot->rh_port = (hub_address == 0 ? port : xhci->slots[hub_address].rh_port);
    slot->speed = speed;

    // allocate DMA memory for device context
    uint8_t* device_context = (uint8_t*)xhci_memalign(xhci, 64, xhci->context_size * XHCI_NUM_EPS);
    if (!device_context) {
        printf("out of DMA memory!\n");
        return ERR_NO_MEMORY;
    }

    xhci_transfer_ring_t* transfer_ring = &slot->transfer_rings[0];
    mx_status_t status = xhci_transfer_ring_init(xhci, transfer_ring, TRANSFER_RING_SIZE);
    if (status < 0)
        return status;

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    xhci_slot_context_t* sc = (xhci_slot_context_t*)&xhci->input_context[1 * xhci->context_size];
    xhci_endpoint_context_t* ep0c = (xhci_endpoint_context_t*)&xhci->input_context[2 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)sc, 0, xhci->context_size);
    memset((void*)ep0c, 0, xhci->context_size);

    slot->sc = (xhci_slot_context_t*)device_context;
    device_context += xhci->context_size;
    for (int i = 0; i < XHCI_NUM_EPS; i++) {
        slot->epcs[i] = (xhci_endpoint_context_t*)device_context;
        device_context += xhci->context_size;
    }

    // Enable slot context and ep0 context
    XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_SLOT_FLAG | XHCI_ICC_EP_FLAG(0));

    // Setup slot context
    uint32_t route_string = xhci_get_route_string(xhci, hub_address, port);
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_ROUTE_STRING_START, SLOT_CTX_ROUTE_STRING_BITS, route_string);
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_SPEED_START, SLOT_CTX_SPEED_BITS, speed);
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_CONTEXT_ENTRIES_START, SLOT_CTX_CONTEXT_ENTRIES_BITS, 1);
    XHCI_SET_BITS32(&sc->sc1, SLOT_CTX_ROOT_HUB_PORT_NUM_START, SLOT_CTX_ROOT_HUB_PORT_NUM_BITS, slot->rh_port);

    uint32_t mtt = 0;
    uint32_t tt_hub_slot_id = 0;
    uint32_t tt_port_number = 0;
    if (hub_address != 0 && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
        xhci_slot_t* hub_slot = &xhci->slots[hub_address];
        if (hub_slot->speed == USB_SPEED_HIGH) {
            mtt = XHCI_GET_BITS32(&slot->sc->sc0, SLOT_CTX_MTT_START, SLOT_CTX_MTT_BITS);
            tt_hub_slot_id = hub_address;
            tt_port_number = port;
        }
    }
    XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_MTT_START, SLOT_CTX_MTT_BITS, mtt);
    XHCI_SET_BITS32(&sc->sc2, SLOT_CTX_TT_HUB_SLOT_ID_START, SLOT_CTX_TT_HUB_SLOT_ID_BITS, tt_hub_slot_id);
    XHCI_SET_BITS32(&sc->sc2, SLOT_CTX_TT_PORT_NUM_START, SLOT_CTX_TT_PORT_NUM_BITS, tt_port_number);

    // Setup endpoint context for ep0
    void* tr = (void*)transfer_ring->start;
    uint64_t tr_dequeue = xhci_virt_to_phys(xhci, (mx_vaddr_t)tr);

    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_CERR_START, EP_CTX_CERR_BITS, 3); // ???
    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_EP_TYPE_START, EP_CTX_EP_TYPE_BITS, EP_CTX_EP_TYPE_CONTROL);
    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_MAX_PACKET_SIZE_START, EP_CTX_MAX_PACKET_SIZE_BITS, 8);
    XHCI_WRITE32(&ep0c->epc2, ((uint32_t)tr_dequeue & EP_CTX_TR_DEQUEUE_LO_MASK) | EP_CTX_DCS);
    XHCI_WRITE32(&ep0c->tr_dequeue_hi, (uint32_t)(tr_dequeue >> 32));
    XHCI_SET_BITS32(&ep0c->epc4, EP_CTX_AVG_TRB_LENGTH_START, EP_CTX_AVG_TRB_LENGTH_BITS, 8); // ???

    // install our device context for the slot
    XHCI_WRITE64(&xhci->dcbaa[slot_id], xhci_virt_to_phys(xhci, (mx_vaddr_t)slot->sc));
    // then send the address device command

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_ADDRESS_DEVICE, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc == TRB_CC_SUCCESS) {
        transfer_ring->enabled = true;
        return NO_ERROR;
    } else {
        printf("xhci_address_device failed cc: %d\n", cc);
        return ERR_INTERNAL;
    }
}

#define BOUNDS_CHECK(i, min, max) (i < min ? min : (i > max ? max : i))
#define LOG2(i) (31 - __builtin_clz(i))

static int compute_interval(usb_endpoint_descriptor_t* ep, usb_speed_t speed) {
    int ep_type = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    int interval = ep->bInterval;

    if (ep_type == USB_ENDPOINT_CONTROL || ep_type == USB_ENDPOINT_BULK) {
        if (speed == USB_SPEED_HIGH) {
            return LOG2(interval);
        } else {
            return 0;
        }
    }

    // now we deal with interrupt and isochronous endpoints
    // first make sure bInterval is in legal range
    if (ep_type == USB_ENDPOINT_INTERRUPT && (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL)) {
        interval = BOUNDS_CHECK(interval, 1, 255);
    } else {
        interval = BOUNDS_CHECK(interval, 1, 16);
    }

    switch (speed) {
    case USB_SPEED_LOW:
        return LOG2(interval) + 3; // + 3 to convert 125us to 1ms
    case USB_SPEED_FULL:
        if (ep_type == USB_ENDPOINT_ISOCHRONOUS) {
            return (interval - 1) + 3;
        } else {
            return LOG2(interval) + 3;
        }
    case USB_SPEED_SUPER:
    case USB_SPEED_HIGH:
        return interval - 1;
    default:
        return 0;
    }
}

static void xhci_disable_slot(xhci_t* xhci, uint32_t slot_id) {
    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_DISABLE_SLOT, 0, (slot_id << TRB_SLOT_ID_START), &command.context);
    xhci_sync_command_wait(&command);

    xprintf("cleaning up slot %d\n", slot_id);
    xhci_slot_t* slot = &xhci->slots[slot_id];
    xhci_free(xhci, (void*)slot->sc);
    memset(slot, 0, sizeof(*slot));
}

static mx_status_t xhci_handle_enumerate_device(xhci_t* xhci, uint32_t hub_address, uint32_t port,
                                                usb_speed_t speed) {
    xprintf("xhci_handle_enumerate_device\n");
    mx_status_t result = NO_ERROR;
    uint32_t slot_id = 0;

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_ENABLE_SLOT, 0, 0, &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc == TRB_CC_SUCCESS) {
        slot_id = xhci_sync_command_slot_id(&command);
    } else {
        printf("unable to get a slot\n");
        return ERR_NO_RESOURCES;
    }
    xhci_slot_t* slot = &xhci->slots[slot_id];
    memset(slot, 0, sizeof(*slot));

    result = xhci_address_device(xhci, slot_id, hub_address, port, speed);
    if (result != NO_ERROR) {
        goto disable_slot_exit;
    }

    // read first 8 bytes of device descriptor to fetch ep0 max packet size
    result = xhci_get_descriptor(xhci, slot_id, USB_TYPE_STANDARD, USB_DT_DEVICE << 8, 0,
                                 xhci->device_descriptor, 8);
    if (result != 8) {
        printf("xhci_get_descriptor failed\n");
        goto disable_slot_exit;
    }

    int mps = xhci->device_descriptor->bMaxPacketSize0;
    // enforce correct max packet size for ep0
    switch (speed) {
        case USB_SPEED_LOW:
            mps = 8;
            break;
        case USB_SPEED_FULL:
            if (mps != 8 && mps != 16 && mps != 32 && mps != 64) {
                mps = 8;
            }
            break;
        case USB_SPEED_HIGH:
            mps = 64;
            break;
        case USB_SPEED_SUPER:
            // bMaxPacketSize0 is an exponent for superspeed devices
            mps = 1 << mps;
            break;
        default:
            break;
    }

    // update the max packet size in our device context
    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    xhci_endpoint_context_t* ep0c = (xhci_endpoint_context_t*)&xhci->input_context[2 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)ep0c, 0, xhci->context_size);

    XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_EP_FLAG(0));
    XHCI_SET_BITS32(&ep0c->epc1, EP_CTX_MAX_PACKET_SIZE_START, EP_CTX_MAX_PACKET_SIZE_BITS, mps);

    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_EVAL_CONTEXT, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    cc = xhci_sync_command_wait(&command);
    if (cc != TRB_CC_SUCCESS) {
        printf("TRB_CMD_EVAL_CONTEXT failed\n");
        result = ERR_INTERNAL;
        goto disable_slot_exit;
    }

    xhci_add_device(xhci, slot_id, hub_address, speed);
    return NO_ERROR;

disable_slot_exit:
    xhci_disable_slot(xhci, slot_id);
    printf("xhci_handle_enumerate_device failed %d\n", result);
    return result;
}

// returns true if endpoint was enabled
static bool xhci_stop_endpoint(xhci_t* xhci, uint32_t slot_id, int ep_index) {
    xhci_slot_t* slot = &xhci->slots[slot_id];
    xhci_transfer_ring_t* transfer_ring = &slot->transfer_rings[ep_index];

    if (!transfer_ring->enabled) {
        return false;
    }

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    // command expects device context index, so increment ep_index by 1
    uint32_t control = (slot_id << TRB_SLOT_ID_START) | ((ep_index + 1) << TRB_ENDPOINT_ID_START);
    xhci_post_command(xhci, TRB_CMD_STOP_ENDPOINT, 0, control, &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_CONTEXT_STATE_ERROR) {
        // TRB_CC_CONTEXT_STATE_ERROR is normal here in the case of a disconnected device,
        // since by then the endpoint would already be in error state.
        printf("TRB_CMD_STOP_ENDPOINT failed cc: %d\n", cc);
    }

    list_node_t list;
    list_initialize(&list);
    list_node_t* node;

    mutex_acquire(&transfer_ring->mutex);

    // copy pending requests to a different list so we can complete them outside of the mutex
    while ((node = list_remove_head(&transfer_ring->pending_requests)) != NULL) {
        list_add_tail(&list, node);
    }
    transfer_ring->enabled = false;

    mutex_release(&transfer_ring->mutex);

    // complete pending requests
    xhci_transfer_context_t* context;
    while ((context = list_remove_head_type(&list, xhci_transfer_context_t, node)) != NULL) {
        context->callback(ERR_REMOTE_CLOSED, context->data);
    }
    // and any deferred requests
    xhci_process_deferred_txns(xhci, transfer_ring, true);
    xhci_transfer_ring_free(xhci, transfer_ring);

    return true;
}

static mx_status_t xhci_handle_disconnect_device(xhci_t* xhci, uint32_t hub_address, uint32_t port) {
    xprintf("xhci_handle_disconnect_device\n");
    xhci_slot_t* slot = NULL;
    uint32_t slot_id;

    int rh_index = xhci_get_root_hub_index(xhci, hub_address);
    if (rh_index >= 0) {
        // For virtual root hub devices, real hub_address is 0
        hub_address = 0;
        // convert virtual root hub port number to real port number
        port = xhci->root_hubs[rh_index].port_map[port - 1] + 1;
    }

    for (slot_id = 1; slot_id <= xhci->max_slots; slot_id++) {
        xhci_slot_t* test_slot = &xhci->slots[slot_id];
        if (test_slot->hub_address == hub_address && test_slot->port == port) {
            slot = test_slot;
            break;
        }
    }
    if (!slot) {
        printf("slot not found in xhci_handle_disconnect_device\n");
        return ERR_NOT_FOUND;
    }

    uint32_t drop_flags = 0;
    for (int i = 0; i < XHCI_NUM_EPS; i++) {
        if (xhci_stop_endpoint(xhci, slot_id, i)) {
            drop_flags |= XHCI_ICC_EP_FLAG(i);
         }
    }

    xhci_remove_device(xhci, slot_id);

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    XHCI_WRITE32(&icc->drop_context_flags, drop_flags);

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_CONFIGURE_EP, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    int cc = xhci_sync_command_wait(&command);
    if (cc != TRB_CC_SUCCESS) {
        printf("TRB_CMD_EVAL_CONTEXT failed\n");
    }

    xhci_disable_slot(xhci, slot_id);

    return NO_ERROR;
}

static int xhci_device_thread(void* arg) {
    xhci_t* xhci = (xhci_t*)arg;

    xhci->input_context = (uint8_t*)xhci_memalign(xhci, 64, xhci->context_size * (XHCI_NUM_EPS + 2));
    if (!xhci->input_context) {
        printf("out of DMA memory!\n");
        return ERR_NO_MEMORY;
    }
    xhci->device_descriptor = (usb_device_descriptor_t*)xhci_malloc(xhci, sizeof(usb_device_descriptor_t));
    if (!xhci->device_descriptor) {
        printf("out of DMA memory!\n");
        xhci_free(xhci, xhci->input_context);
        return ERR_NO_MEMORY;
    }

    while (1) {
        xprintf("xhci_device_thread top of loop\n");
        // wait for a device to enumerate
        completion_wait(&xhci->command_queue_completion, MX_TIME_INFINITE);

        mutex_acquire(&xhci->command_queue_mutex);
        list_node_t* node = list_remove_head(&xhci->command_queue);
        xhci_device_command_t* command = (node ? containerof(node, xhci_device_command_t, node) : NULL);
        if (list_is_empty(&xhci->command_queue)) {
            completion_reset(&xhci->command_queue_completion);
        }
        mutex_release(&xhci->command_queue_mutex);

        if (!command) {
            printf("ERROR: command_queue_completion was signaled, but no command was found");
            break;
        }

        switch (command->command) {
        case ENUMERATE_DEVICE:
            xhci_handle_enumerate_device(xhci, command->hub_address, command->port, command->speed);
            break;
        case DISCONNECT_DEVICE:
            xhci_handle_disconnect_device(xhci, command->hub_address, command->port);
            break;
        case START_ROOT_HUBS:
            xhci_start_root_hubs(xhci);
        }
    }

    // free our DMA buffers
    xhci_free(xhci, xhci->input_context);
    xhci_free(xhci, xhci->device_descriptor);
    xhci->input_context = NULL;
    xhci->device_descriptor = NULL;

    return 0;
}

void xhci_start_device_thread(xhci_t* xhci) {
    thread_detach_and_resume(thread_create("XHCI device thread", xhci_device_thread, xhci, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE));
}

static mx_status_t xhci_queue_command(xhci_t* xhci, int command, uint32_t hub_address,
                                      uint32_t port, usb_speed_t speed) {
    xhci_device_command_t* device_command = calloc(1, sizeof(xhci_device_command_t));
    if (!device_command) {
        printf("out of memory\n");
        return ERR_NO_MEMORY;
    }
    device_command->command = command;
    device_command->hub_address = hub_address;
    device_command->port = port;
    device_command->speed = speed;

    mutex_acquire(&xhci->command_queue_mutex);
    list_add_tail(&xhci->command_queue, &device_command->node);
    completion_signal(&xhci->command_queue_completion);
    mutex_release(&xhci->command_queue_mutex);

    return NO_ERROR;
}

mx_status_t xhci_enumerate_device(xhci_t* xhci, uint32_t hub_address, uint32_t port,
                                  usb_speed_t speed) {
    return xhci_queue_command(xhci, ENUMERATE_DEVICE, hub_address, port, speed);
}

mx_status_t xhci_device_disconnected(xhci_t* xhci, uint32_t hub_address, uint32_t port) {
    xprintf("xhci_device_disconnected %d %d\n", hub_address, port);
    mutex_acquire(&xhci->command_queue_mutex);
    // check pending device list first
    xhci_device_command_t* command;
    list_for_every_entry (&xhci->command_queue, command, xhci_device_command_t, node) {
        if (command->command == ENUMERATE_DEVICE && command->hub_address == hub_address &&
            command->port == port) {
            xprintf("found on pending list\n");
            list_delete(&command->node);
            mutex_release(&xhci->command_queue_mutex);
            return NO_ERROR;
        }
    }
    mutex_release(&xhci->command_queue_mutex);

    return xhci_queue_command(xhci, DISCONNECT_DEVICE, hub_address, port, USB_SPEED_UNDEFINED);
}

mx_status_t xhci_queue_start_root_hubs(xhci_t* xhci) {
    return xhci_queue_command(xhci, START_ROOT_HUBS, 0, 0, USB_SPEED_UNDEFINED);
}

mx_status_t xhci_enable_endpoint(xhci_t* xhci, uint32_t slot_id, usb_endpoint_descriptor_t* ep, bool enable) {
    if (xhci_is_root_hub(xhci, slot_id)) {
        // nothing to do for root hubs
        return NO_ERROR;
    }

    xhci_slot_t* slot = &xhci->slots[slot_id];
    usb_speed_t speed = slot->speed;
    uint32_t index = xhci_endpoint_index(ep->bEndpointAddress);
    xhci_transfer_ring_t* transfer_ring = &slot->transfer_rings[index];

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&xhci->input_context[0 * xhci->context_size];
    xhci_slot_context_t* sc = (xhci_slot_context_t*)&xhci->input_context[1 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);

    if (enable) {
        memset((void*)sc, 0, xhci->context_size);

        uint32_t ep_type = ep->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        uint32_t ep_index = ep_type;
        if ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_IN) {
            ep_index += 4;
        }

        // See Table 65 in XHCI spec
        int cerr = (ep_type == USB_ENDPOINT_ISOCHRONOUS ? 0 : 3);
        int max_packet_size = usb_ep_max_packet(ep);
        int max_burst = usb_ep_max_burst(ep);
        int avg_trb_length = max_packet_size * max_burst;
        int max_esit_payload = 0;
        if (ep_type == USB_ENDPOINT_ISOCHRONOUS) {
            // FIXME - more work needed for superspeed here
            max_esit_payload = max_packet_size * max_burst;
        }

        xhci_endpoint_context_t* epc = (xhci_endpoint_context_t*)&xhci->input_context[(index + 2) * xhci->context_size];
        memset((void*)epc, 0, xhci->context_size);
        // allocate a transfer ring for the endpoint
        mx_status_t status = xhci_transfer_ring_init(xhci, transfer_ring, TRANSFER_RING_SIZE);
        if (status < 0)
            return status;

        void* tr = (void*)slot->transfer_rings[index].start;
        uint64_t tr_dequeue = xhci_virt_to_phys(xhci, (mx_vaddr_t)tr);

        XHCI_SET_BITS32(&epc->epc0, EP_CTX_INTERVAL_START, EP_CTX_INTERVAL_BITS, compute_interval(ep, speed));
        XHCI_SET_BITS32(&epc->epc0, EP_CTX_MAX_ESIT_PAYLOAD_HI_START, EP_CTX_MAX_ESIT_PAYLOAD_HI_BITS,
                        max_esit_payload >> EP_CTX_MAX_ESIT_PAYLOAD_LO_BITS);
        XHCI_SET_BITS32(&epc->epc1, EP_CTX_CERR_START, EP_CTX_CERR_BITS, cerr);
        XHCI_SET_BITS32(&epc->epc1, EP_CTX_EP_TYPE_START, EP_CTX_EP_TYPE_BITS, ep_index);
        XHCI_SET_BITS32(&epc->epc1, EP_CTX_MAX_PACKET_SIZE_START, EP_CTX_MAX_PACKET_SIZE_BITS, max_packet_size);

        XHCI_WRITE32(&epc->epc2, ((uint32_t)tr_dequeue & EP_CTX_TR_DEQUEUE_LO_MASK) | EP_CTX_DCS);
        XHCI_WRITE32(&epc->tr_dequeue_hi, (uint32_t)(tr_dequeue >> 32));
        XHCI_SET_BITS32(&epc->epc4, EP_CTX_AVG_TRB_LENGTH_START, EP_CTX_AVG_TRB_LENGTH_BITS, avg_trb_length);
        XHCI_SET_BITS32(&epc->epc4, EP_CTX_MAX_ESIT_PAYLOAD_LO_START, EP_CTX_MAX_ESIT_PAYLOAD_LO_BITS, max_esit_payload);

        XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_SLOT_FLAG | XHCI_ICC_EP_FLAG(index));
        XHCI_WRITE32(&sc->sc0, XHCI_READ32(&slot->sc->sc0));
        XHCI_WRITE32(&sc->sc1, XHCI_READ32(&slot->sc->sc1));
        XHCI_WRITE32(&sc->sc2, XHCI_READ32(&slot->sc->sc2));
        XHCI_SET_BITS32(&sc->sc0, SLOT_CTX_CONTEXT_ENTRIES_START, SLOT_CTX_CONTEXT_ENTRIES_BITS, index + 1);
    } else {
        xhci_stop_endpoint(xhci, slot_id, index);
        XHCI_WRITE32(&icc->drop_context_flags, XHCI_ICC_EP_FLAG(index));
    }

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_CONFIGURE_EP, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);

    xhci_sync_command_wait(&command);

    // xhci_stop_endpoint() will handle the !enable case
    if (enable) {
        transfer_ring->enabled = true;
    }

    return NO_ERROR;
}

mx_status_t xhci_configure_hub(xhci_t* xhci, uint32_t slot_id, usb_speed_t speed,
                               usb_hub_descriptor_t* descriptor) {
    xprintf("xhci_configure_hub slot_id: %d speed: %d\n", slot_id, speed);
    if (xhci_is_root_hub(xhci, slot_id)) {
        // nothing to do for root hubs
        return NO_ERROR;
    }
    if (slot_id > xhci->max_slots) return ERR_INVALID_ARGS;

    xhci_slot_t* slot = &xhci->slots[slot_id];
    uint8_t* input_context = (uint8_t*)xhci_memalign(xhci, 64, xhci->context_size * 2);
    if (!input_context) {
        printf("out of DMA memory!\n");
        return ERR_NO_MEMORY;
    }

    uint32_t num_ports = descriptor->bNbrPorts;
    uint32_t ttt = 0;
    if (speed == USB_SPEED_HIGH) {
        ttt = (descriptor->wHubCharacteristics >> 5) & 3;
    }

    xhci_input_control_context_t* icc = (xhci_input_control_context_t*)&input_context[0 * xhci->context_size];
    xhci_slot_context_t* sc = (xhci_slot_context_t*)&input_context[1 * xhci->context_size];
    memset((void*)icc, 0, xhci->context_size);
    memset((void*)sc, 0, xhci->context_size);

    XHCI_WRITE32(&icc->add_context_flags, XHCI_ICC_SLOT_FLAG);
    XHCI_WRITE32(&sc->sc0, XHCI_READ32(&slot->sc->sc0) | SLOT_CTX_HUB);
    XHCI_WRITE32(&sc->sc1, XHCI_READ32(&slot->sc->sc1));
    XHCI_WRITE32(&sc->sc2, XHCI_READ32(&slot->sc->sc2));

    XHCI_SET_BITS32(&sc->sc1, SLOT_CTX_ROOT_NUM_PORTS_START, SLOT_CTX_ROOT_NUM_PORTS_BITS, num_ports);
    XHCI_SET_BITS32(&sc->sc2, SLOT_CTX_TTT_START, SLOT_CTX_TTT_BITS, ttt);

    xhci_sync_command_t command;
    xhci_sync_command_init(&command);
    xhci_post_command(xhci, TRB_CMD_EVAL_CONTEXT, xhci_virt_to_phys(xhci, (mx_vaddr_t)icc),
                      (slot_id << TRB_SLOT_ID_START), &command.context);
    int cc = xhci_sync_command_wait(&command);

    xhci_free(xhci, input_context);

    if (cc != TRB_CC_SUCCESS) {
        printf("TRB_CMD_EVAL_CONTEXT failed\n");
        return -1;
    }

    if (speed == USB_SPEED_SUPER) {
        // compute hub depth
        int depth = 0;
        while (slot->hub_address != 0) {
            depth++;
            slot = &xhci->slots[slot->hub_address];
        }

        xprintf("USB_HUB_SET_DEPTH %d\n", depth);
        mx_status_t result = xhci_control_request(xhci, slot_id,
                                      USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                                      USB_HUB_SET_DEPTH, depth, 0, (mx_paddr_t)NULL, 0);
        if (result < 0) {
            printf("USB_HUB_SET_DEPTH failed\n");
        }
    }

    return NO_ERROR;
}

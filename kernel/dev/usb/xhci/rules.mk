# Copyright 2016 The Fuchsia Authors
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT

LOCAL_DIR := $(GET_LOCAL_DIR)
MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/xhci.c \
	$(LOCAL_DIR)/xhci_device_manager.c \
	$(LOCAL_DIR)/xhci_root_hub.c \
	$(LOCAL_DIR)/xhci_transfer.c \
	$(LOCAL_DIR)/xhci_trb.c \
	$(LOCAL_DIR)/xhci_util.c

#	$(LOCAL_DIR)/usb_xhci.c \

include make/module.mk


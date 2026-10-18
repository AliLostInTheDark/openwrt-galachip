/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef OMCI_NATIVE_H
#define OMCI_NATIVE_H
#define OMCI_NATIVE_ENGINE 1
#include "native-platform.h"
#include "device.h"
#include "internal.h"
#include "jhash-native.h"
void omci_native_load_board(struct omci_device *odev);
void omci_native_timers(struct omci_device *odev);
void omci_native_reboot(struct omci_device *odev);
int omci_native_receive(struct omci_device *odev, const void *data, size_t len);
int omci_native_put_mib_object(struct sk_buff *msg, const struct omci_mib_object *object,
			       u32 next_index, const char *name);
#endif

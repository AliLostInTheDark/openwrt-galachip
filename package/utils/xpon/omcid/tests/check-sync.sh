#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The backend checks and the public and uAPI headers are shared with the
# kernel. internal.h is not: the kernel keeps only the configuration store.
set -eu
cd "$(dirname "$0")"
for target in realtek-luna/files-6.18 galachip/files zte/files; do
    kernel=../../../../../target/linux/$target
    cmp ../src/backend.c "$kernel/net/xpon/omci/backend.c"
    cmp ../src/device.h "$kernel/include/net/xpon/omci.h"
    cmp ../../libomci-protocol/src/include/linux/omci-backend.h "$kernel/include/uapi/linux/omci-backend.h"
    cmp ../../libomci-protocol/src/include/linux/omci.h "$kernel/include/uapi/linux/omci.h"
done
for copy in libomci-protocol omci-agent omci-monitor; do
    cmp "../../$copy/src/include/linux/omci.h" "$kernel/include/uapi/linux/omci.h"
done

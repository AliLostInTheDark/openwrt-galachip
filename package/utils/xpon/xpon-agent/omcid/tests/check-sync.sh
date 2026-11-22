#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The backend checks and the uAPI headers are shared with the kernel. The
# engine's device.h started as the kernel's omci.h but has the identity and
# configuration hooks the kernel no longer has.
set -eu
cd "$(dirname "$0")"
kernel=../../../../../../target/linux/galachip/files
cmp ../src/backend-layout.c "$kernel/net/xpon/omci/backend-layout.c"
cmp ../../protocol/src/include/linux/omci-backend.h "$kernel/include/uapi/linux/omci-backend.h"
cmp ../../protocol/src/include/linux/omci.h "$kernel/include/uapi/linux/omci.h"
cmp ../../src/include/linux/xpon.h "$kernel/include/uapi/linux/xpon.h"
for copy in ../../protocol ../.. ../../../xpon-report/monitor; do
    cmp "$copy/src/include/linux/omci.h" "$kernel/include/uapi/linux/omci.h"
done

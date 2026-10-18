# Slate regression checks

Run from the OpenWrt checkout. These tests use host tools; they do not build or
flash firmware, contact a router, or add runtime work to LuCI.

```sh
node package/luci/luci-theme-slate/tests/noncss.mjs
PLAYWRIGHT_MODULE=/path/to/node_modules/playwright CHROME=/path/to/chrome \
  node package/luci/luci-theme-slate/tests/layout.mjs
node package/utils/xpon/luci-app-omci/tests/light-status.mjs
```

The template checks need the checkout's host `ucode`, headers and a C compiler.
Set `UCODE` to override its location. Missing `ucode` explicitly skips template
tests. Playwright and Chrome are host-only test dependencies, not package
dependencies. The browser server listens only on loopback.

## Coverage

- Forms, editable tables, file controls, dynamic lists, dropdowns, progress bars,
  interface badges, login, modal scrolling, navigation and status grids.
- Actual GPON/EPON status renderers with fixture data; actual LuCI dropdown and
  action-column sizing methods from this checkout.
- Light/dark layouts at 320, 375, 600, 800, 801, 1024 and 1440 CSS pixels:
  140 layout cases plus 14 dependency-visibility checks. Narrow cases use touch
  emulation. Checks cover viewport/container bounds and popup clipping.
- Sidebar groups (open state, active page, breadcrumbs, leaf links), menu
  aliases, ACL filtering, nested tabs, cancelled login submissions, HTML
  escaping, all three theme variants, and install/upgrade/removal behavior.

`WIDTHS` and `SCENARIOS` select smaller browser runs. `SCREENSHOT_DIR` saves
320/1440-pixel light-mode screenshots into an existing directory; modal images
show the action area after scrolling. `DEBUG_LAYOUT=1` prints overflowing nodes.

## Audit findings addressed

The October 2026 audit fixed constrained controls and table overflow, long-text
wrapping, desktop dropdown expansion, off-screen navigation popups, an overview
grid selector affecting unrelated cards, and missing mobile row classes in the
GPON status view. It also fixed unsafe template interpolation, missing/aliased
menu paths, login retries after cancellation, and theme-registration cleanup
during upgrades. Original template authorship is retained in source and NOTICE.

A live G7 check additionally found interface action labels splitting inside a
narrow action column. Keep native table sizing, constrain the rich dropdown
instead of making it fill the table cell, and preserve whole action words.
The interface fixture uses the checkout's column-sizing method and reproduces
the shipped bug at 1394/1440 pixels; the corrected CSS must pass the same test.

The local browser and host tests do not validate live RPC permissions, device
configuration changes, firmware upgrades or every third-party LuCI package.
Rebuilt-package testing on the target is still required.

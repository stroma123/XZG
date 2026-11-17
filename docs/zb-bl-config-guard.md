# Design note: keep the chip's BL_CONFIG when flashing Zigbee firmware

Status: proposal, not implemented. Branch: `dev/zb-update`. Written 2026-11-17.

## Problem

On CC26x2 chips the ROM bootloader (BSL) is set up by the `BL_CONFIG` register in CCFG. CCFG is the last 88 bytes of
flash, and `BL_CONFIG` sits at CCFG + 0x30, which is `0x57FD8` on a 352 KB chip ("1FD8h" in the last 8 KB page).
That register sets which DIO pin, held at which level during reset, forces the chip into BSL.

XZG enters BSL by holding the board's BSL line during reset (`CCTools::begin`, `_BSL_PIN_MODE == 0`). Which
CC26xx DIO that line reaches depends on the board:

| Board | ESP32 BSL GPIO | CC DIO | Required `BL_CONFIG` |
|---|---|---|---|
| ZG-806Z (`hw.cpp` zbConfig 9) | 22 | DIO8 | `0xC5FE08C5` |
| Most launchpad-style boards | varies | DIO15 (typical) | `0xC5FE0FC5` (typical) |

Field layout (verify against the CC26x2 TRM, CCFG chapter):
`[31:24]` BOOTLOADER_ENABLE (`0xC5` = enabled) · `[16]` BL_LEVEL (0 = active low) · `[15:8]` BL_PIN_NUMBER ·
`[7:0]` BL_ENABLE (`0xC5` = enabled). `0xC5FE08C5` means BSL enabled, active low, DIO8.

A firmware image carries its own CCFG and therefore its own `BL_CONFIG`. If the image's BSL pin differs from the
board's wiring, the new firmware runs fine, but **XZG can never enter BSL again**. Every later flash, NV erase or
chip check fails. Recovery needs a cJTAG probe (e.g. XDS110).

## Current state (dev/zb-update @ b60367c)

- The chip's current value is already read: `CCTools::detectChipInfo` reads `flashSize - 88 + 0x30` into
  `CCTool.chip.bslCfg` (`lib/CCTools/src/CCTools.cpp`). It is only logged (`zbHwCheck`, `src/zb.cpp`), never used.
  **It is byte-swapped:** `_decodeAddr` already takes bytes in little-endian order, but the caller passes
  `b[3], b[2], b[1], b[0]`, so `chip.bslCfg` holds the bytes in memory order as a big-endian number. On a ZG-806Z the
  log shows `bslCfg c508fec5` for a register value of `0xC5FE08C5` (memory bytes `C5 08 FE C5`). Same for `modeCfg`.
  The guard must either fix the decode (pass `b[0..3]` in order, which also changes the log) or handle raw bytes.
  Patching raw bytes (copy the 4 memory bytes read from the chip into the image) avoids the issue entirely.
- Verified on hardware 2026-11-17 (ZG-806Z, IEEE 00:12:4B:00:30:D2:69:4F):
  - URL flash of `SonoffZBPro_coord_20240710.bin`: BSL entry still works, `BL_CONFIG` still `0xC5FE08C5` (DIO8).
    So the `ZBPro_*` images keep the ZG-806Z pin.
  - Upload flash of a generic router `.bin` (v20250403) whose `BL_CONFIG` the user had **patched by hand** to
    `0xC5FE08C5`: the firmware booted and runs as a router, BSL entry still works, and `BL_CONFIG` reads `0xC5FE08C5`.
    Role (router) and version were saved and survived the reboot. This is a manual run of exactly what the
    proposal automates.

## Other issues in the upload path (found while testing)

1. **`.hex` is accepted but not converted.** `tools.html` allows `.bin,.hex`, and the server saves the upload raw as
   `/fw.hex` (`tempFile`, `src/web.cpp`). `eraseWriteZbFile` writes those bytes to the chip as they are. An Intel HEX
   file is ASCII text, so the chip would get garbage, the log would still say "Flashed successfully", and the
   garbage would also land in CCFG and could disable BSL. **Decision: only `.bin` images are supported for ZB
   firmware.** Fix:
   - UI: `accept=".bin"` in `tools.html`.
   - Server: reject non-`.bin` uploads in the upload handler (`src/web.cpp`) before writing to LittleFS, and rename
     `tempFile` to e.g. `/zbfw.bin`.
   - Before erase, reject an image that is larger than `chip.flashSize`, or not a multiple of 4.
   - Optional sanity check: Intel HEX starts with `:`. Reject a file whose first byte is `:`, in case it was
     renamed to `.bin`.
   - The old `zbOta()`/`isHex()` in `functions.js` are dead code and can go.
2. **Role detection after upload only matches full words.** `flashZbFile` matches `COORDINATOR`/`ROUTER`/`THREAD`/
   `_RCP` in the filename. `SonoffZBPro_coord_*.bin` contains only `coord`, so the role is not updated. The URL path
   detects `coord` correctly. Make both paths share one helper.
3. **The chip-model check uses only the filename** (`validateChipModel`), see Current state above.
- `eraseWriteZbFile` (upload path) only runs `validateChipModel`, which matches the chip model from the
  **filename**. No CCFG check.
- `eraseWriteZbUrl` (URL path) has no image check at all.
- The firmware list in the UI comes from `ZB_UPDATE_RELEASE_MANIFEST_URL` (xyzroe `zb_fws/ti/manifest.json`).
  Compatible and incompatible images are **mixed under the same chip key** (`CC2652P2_launchpad`):
  - Compatible with ZG-806Z (Sonoff ZB Pro builds, DIO8 BSL): `SonoffZBPro_coord_20240710.bin`
    (ver `ZBPro_20240710`) and `SonoffZBPro_router_20220125.bin` (ver `ZBPro_20220125`). These are the entries whose
    version starts with `ZBPro_`.
  - All other `CC2652P2_launchpad` entries are generic launchpad builds and would brick BSL on a ZG-806Z.
  - The list is picked by `CCTool.chip.hwRev`, not by board, so the UI shows both kinds to a ZG-806Z.
- The update notifier (`fetchLatestZbFw`, `src/web.cpp`) takes the **first** entry under role + `hwRev` and ignores
  the board. A ZG-806Z can therefore be told about a "new" generic firmware. Auto-install is commented out
  (`checkUpdateAvail`, `src/etc.cpp`), so it only notifies, but clicking through would brick BSL.

## Proposal

**Default: keep the chip's value.** Reaching BSL to read `chip.bslCfg` already proves that value matches the board's
wiring. While streaming the image, when the chunk that covers offset `flashSize - 0x28` is in the buffer, overwrite
those 4 bytes (little-endian) with `chip.bslCfg` before `CCTool.processFlash()`. That makes any firmware built for
the chip safe on every board, and needs no per-board table.

**Fallback: refuse.** If `chip.bslCfg` is unusable, abort **before erase**:
- `0xFFFFFFFF` / `0x00000000` (blank or unreadable CCFG)
- BOOTLOADER_ENABLE or BL_ENABLE ≠ `0xC5`

Optionally, a per-board expected value in `zbConfigs` (`hw.cpp`) could serve as a cross-check, or as the source
when the chip is blank.

### Where to hook it

- Shared helper, e.g. `patchBlConfig(uint8_t *buf, size_t chunkOffset, size_t chunkLen, uint32_t flashSize, uint32_t blCfg)`.
  It patches the 4 bytes if they fall inside the chunk. Handle a value that straddles two chunks
  (`TRANSFER_SIZE` boundary), or align the write so it can't.
- `eraseWriteZbFile`: call it in the write loop. The file is on LittleFS, so the image's original value can also be
  read and logged up front.
- `eraseWriteZbUrl`: call it in the stream loop. With HTTP Range resume, the chunk offset is `loadedSize`.
- `chip.bslCfg` must be current: the setup() path in this branch restores `zbFw` from NVS and may skip the
  probe, so read chip info (`zbHwCheck` / `CCTool.detectChipInfo()`) right before erase. Don't rely on cached info.
- Log both values: `BL_CONFIG image=0x… chip=0x… -> keeping chip value`.

### Image-size cases

- The image covers the whole flash (size == `flashSize`, the usual Koenkk `.bin`): patch as above.
- The image is shorter than `flashSize` (no CCFG in it): after the write, CCFG stays **erased** (`0xFF…`), which on
  CC26x2 means BSL is controlled by the erased defaults. Decide: write CCFG explicitly, or refuse such images.
- The image is larger than `flashSize`: refuse (it is already the wrong chip).

### Related: board-aware firmware list

Separate from the flash-time guard, the UI list and update notifier could filter by board. For example, give
`ZG-806Z` (and Sonoff ZB Bridge Pro) a manifest key or prefix such as `ZBPro_`, so they only offer and notify about
`ZBPro_*` images. This is a usability fix, not a safety one: the flash-time guard is still needed for uploads and
custom URLs.

## Open questions

1. ~~Confirm CCFG has no checksum/CRC that patching would invalidate.~~ Answered in practice for Z-Stack `.bin`
   images: a hand-patched generic router booted and ran (2026-11-17). OAD images are out of scope: only plain `.bin`
   images are supported.
2. Should the user be able to override this (e.g. intentionally move the BSL pin)? Probably an advanced checkbox,
   off by default.
3. Same treatment for the CC2652P7 (704 KB, different `flashSize`)? The offset formula already scales.
4. Side note found while reading: `CCTools.cpp` uses `if (chip.hwRev.indexOf("P7"))`, which is true unless "P7" is
   at index 0, so `page_size` is 8192 for practically every chip. That's correct for CC26x2 (8 KB sectors), but
   the condition is wrong.

## Testing

- ZG-806Z: flash a launchpad image (`0xC5FE0FC5`) and confirm the log shows the patch. After reboot, BSL entry
  still works (chip check, NV erase, second flash).
- A board with DIO15 wiring: flash the same image. The value already matches, so no patch.
- Blank chip (after a JTAG mass erase): the flash must refuse, or use the board table.
- Read back `0x57FD8` after flashing (`_cmdMemRead`) and compare with `chip.bslCfg`, minding the byte order above.
  Quick manual check: run the Zigbee hardware check (`zbHwCheck`) and look for `bslCfg c508fec5` in the log
  (current byte-swapped output for `0xC5FE08C5`).
- Have the cJTAG probe ready for the first ZG-806Z test.

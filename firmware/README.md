# Prebuilt firmware binaries

This directory holds ready-to-flash firmware binaries, one `.bin` file per
`FIRMWARE_VERSION` (see `include/config.h`). Each file is generated
automatically by `copy_firmware_bin.py` at the end of every `pio run`, for
both the USB and OTA build environments - don't hand-edit or hand-add
binaries here.

Because old versions are never overwritten (each build is named
`noiselight-<version>.bin`), any previously built version stays available for
re-flashing.

## Using a binary

1. Open the device's WebUI in a browser.
2. Go to the "Firmware-Update" section.
3. Select the desired `noiselight-<version>.bin` from this folder.
4. Enter the `OTA_PASSWORD` (from `include/config.h`).
5. Upload - the device reboots automatically on success.

# Chromebook audio (Snappy / Reef, Apollo Lake)

Goal: working speakers, headphone jack and mic on Windows for the Google Snappy
Chromebook (Intel Celeron N3350, DA7219 headset codec, MAX98357A speaker amp).

## Pieces

| Folder | What | Source / license |
|---|---|---|
| `sklhdaudbus/` | HD Audio bus driver for the `PCI\VEN_8086&DEV_5A98` controller; exposes the audio DSP to a child driver | CoolStar, BSD-3-Clause (vendored, unmodified) |
| `da7219/` | DA7219 headset codec driver (I2C, jack detect, headset buttons) | CoolStar, Apache-2.0 (vendored, unmodified) |
| *(planned)* `sofdsp/` | Our DSP driver: binds to `CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_5A98`, loads Sound Open Firmware, exposes Windows audio endpoints | This project |

## Building

GitHub Actions builds everything on push (`.github/workflows/build.yml`) and uploads
an unsigned `drivers-unsigned` artifact.

## Installing (on the Chromebook)

1. Test signing must be on: `bcdedit /set testsigning on` (admin), then reboot.
2. Download and unzip the `drivers-unsigned` artifact.
3. In an **admin** PowerShell:
   `powershell -ExecutionPolicy Bypass -File scripts\install-drivers.ps1 -PackageDir <unzipped folder>`
4. Reboot.

If something goes wrong: boot to Safe Mode and remove the driver with
`pnputil /delete-driver oemNN.inf /uninstall`, or use the restore point the script creates.

# Project status

Last updated: 2026-11-10

## Hardware

- Google Snappy (Reef), Celeron N3350 (Apollo Lake), MrChromebox UEFI, Windows 10 IoT LTSC.
- SST audio DSP: PCI `8086:5A98`.
- Headset codec: DA7219 (I2C, bus `5AAC`), on SSP1.
- Speaker amp: MAX98357A, on SSP5.

## Drivers in this repo

| Driver | Origin | Role |
|---|---|---|
| `sklhdaudbus` | CoolStar (BSD), vendored | HDA/DSP bus driver |
| `da7219` | CoolStar (Apache), vendored | Headset codec |
| `max98357a` | CoolStar, vendored | Speaker amp enable |
| `sofapl` | ours | Loads SOF firmware on the DSP and drives IPC3. Binds `CSAUDIO\ADSP&CTLR_VEN_8086&CTLR_DEV_5A98` |
| `sofaudio` | ours (adapted from MS SimpleAudioSample) | Windows audio endpoints. Binds the `SOFAPL\Speaker` child PDO. Interface in `include/sofapl_iface.h` |

Install order on a fresh system: sklhdaudbus, da7219, max98357a, sofapl, sofaudio.

## Build and install

- CI (GitHub Actions, windows-2022, x64 MSBuild, `EnableInf2cat=false`) publishes:
  - the `latest-build` release, with `drivers-unsigned.zip`
  - the `build-logs` release
  Both download without signing in. CI also downloads the SOF firmware (see below).
- Test signing must be on: `bcdedit /set testsigning on`, then reboot.
- Install from an admin PowerShell:
  `scripts\install-drivers.ps1 -PackageDir <dir> -Drivers sofapl,sofaudio,...`
  The first run creates a local test-signing certificate whose key can't be exported.
- Bump `DriverVer`/TimeStamp in the INF on every update.
- `sofapl` writes `BootLog`/`BootStage`/`BootStatus` under
  `HKLM\SYSTEM\CurrentControlSet\Enum\CSAUDIO\ADSP...\Device Parameters`. You can read them without admin.
- Restore points don't work on this machine; WinRE does. See `RECOVERY.md`.

## Firmware

- SOF 2.2.0 from sof-bin `v2026.09.2`, intel-signed `sof-apl.ri`, IPC3.
- Topology values come from the sof-bin v2.2.1 tplg files (`.github/workflows/tplg-dump.yml` dumps them).

## Key findings

- Firmware boot needed SPIB enabled with the payload size.
- IPC mailboxes come from the `.ri` extended manifest (downbox `0xA0000`, upbox `0x81000`, max IPC 384 bytes). The data after FW_READY is all zeros on SOF 2.2, so don't take them from there.
- Speaker pipeline 1: host → buffer → PGA → buffer → SSP5.
- The speaker needs the MAX98357A amp enabled. `sofapl` sends Speaker Start/Stop over `\Callback\CsAudioCallbackAPI`.
- Choppy playback was fixed by reading position from the DPIB register (HDA BAR `0x1084 + 0x20*idx`) and sending notifications based on the hardware position.

## Milestones

- [x] M1: bus + da7219 installed
- [x] M2: SOF firmware boots, FW_READY received
- [x] M3a: IPC polling, speaker pipeline + SSP5 DAI config accepted
- [x] M3b: streaming. The speaker sounds clean (confirmed 2026-11-10).
- [x] Windows volume/mute drive the DSP gain. Speaker: opens at -20 dB, 100% = -6 dB cap.
- [~] Headphones: pipeline 2 on SSP1/DA7219, its own endpoint, capped at -18 dB because the DA7219 headphone amp runs at +6 dB. Also fixes garbled audio after pause/resume. Commit `bece339`, built but **not yet tested on hardware**.
- [~] Internal mic: pipeline 4, DMIC0 → buffer → PGA → buffer → host (capture), S32_LE 48 kHz stereo, PDM controller 0 with mics A+B. The PGA isn't in the Linux topology; we added it for Windows mic level/mute (100% = +20 dB, 0 dB at boot). sofapl 0.7.0 / sofaudio 0.6.0. Capture position comes from the HDA position buffer after a dummy DPIB read (Linux does this). **Not yet tested on hardware**; the BootLog line "Pipelines: … mic ready/FAILED" shows whether the firmware accepted it.

## Next

1. Test the headphone build (start at low volume) and the pause/resume fix.
2. Switch automatically between speaker and headphones on jack detect.
3. Test internal mic capture. Later: headset mic (pipeline 3, SSP1 capture; needs the DA7219 mic path enabled).
4. Consider raising the -6 dB speaker cap.

## Safety rule

Before installing any build that can output sound, give a warning first, and start first-sound tests at low volume.

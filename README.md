# renpho-tape

Lean 4 Bluetooth LE client for the RENPHO smart tape measure (model R-Y001). It runs on macOS, Linux and
Windows and can run as a service that prints one JSON line per reading.

## Protocol (checked on a real R-Y001)

Advertises as `R-Y001`. No pairing, no handshake, no commands. Service `0783B03E-8535-B5A0-7140-A304D2495CB7`, notify
characteristic `…5CB8`, write-without-response characteristic `…5CBA` (never written; its use is unknown).
Each notification is 20 bytes of ASCII:

    *LLLLL;00000;0000<P|S><M|I>\n

- `LLLLL`: length in hundredths of a centimetre, in both unit modes (a 10 cm pull sends `01000`; the display read 3.93 in).
- `P`: live value. `S`: saved with the OK button, sent about four times per press.
- `M`/`I`: the unit the display shows.
- The tape sleeps after roughly 10 to 45 s and drops the link; the vendor app does nothing to prevent it. `serve` waits and
  reconnects. A press of the tape's power button wakes it.

Other projects document the same frame for the older RF-BMF01: DUC750/renpho-tape-ble,
akunopaka/smart-e-tape-measure-web-bluetooth, Jerrys-modz/Open-RENPHO, clunc/ble-tape-gateway.

## Commands

    renpho-tape scan [seconds]
    renpho-tape dump [device]
    renpho-tape listen [device] [--seconds n] [--all] [--json] [--write <uuid>=<hex>]
    renpho-tape serve [device]        # listen --json, forever

`serve` output, one object per line:

    {"event":"reading","ts_ms":1794583338245,"centi_cm":1000,"cm":"10.00","committed":false,"display_unit":"in","raw":"2a 30 ..."}
    {"event":"link_lost","ts_ms":1794583399000}

## Backends

| OS | Source | Notes |
| --- | --- | --- |
| macOS | `ffi/ble_ffi.m` (CoreBluetooth) | Tested on hardware. The first run asks for Bluetooth permission for the terminal or app. |
| Linux | `ffi/ble_ffi_linux.c` (BlueZ over D-Bus, GIO) | Builds, links and starts in an Ubuntu aarch64 container. Not run against a radio. |
| Windows | `ffi/ble_ffi_win.c` + `ffi/win/renpho_ble_win.cpp` (WinRT) | Not built or run yet: CI is the first compile. The WinRT code lives in an MSVC-built DLL that sits beside the exe, so its C++ runtime stays out of the MinGW-built Lean binary. |

## Build

    # macOS
    lake build
    # Linux (libglib2.0-dev or glib2-devel, pkg-config)
    lake build -Kcflags="$(scripts/linux-flags.sh cflags)" -Klinklibs="$(scripts/linux-flags.sh libs)"
    # Windows: lake build, then from an x64 Native Tools prompt
    scripts\build-win-dll.bat

`#guard` checks in `RenphoTape/Protocol.lean` run at build time on captured frames.

## Run as a service

- Linux: copy the binary to `/opt/renpho-tape/`, `deploy/renpho-tape.service` to `/etc/systemd/system/`, then
  `sudo systemctl daemon-reload && sudo systemctl enable --now renpho-tape`; `journalctl -u renpho-tape -f -o cat`.
- macOS: copy the binary to `/usr/local/bin/`, the plist to `~/Library/LaunchAgents/`, then
  `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.chibifire.renpho-tape.plist`. Readings go to `/tmp/renpho-tape.jsonl`.
- Windows: `powershell -ExecutionPolicy Bypass -File install-windows.ps1`: a logon task that restarts on failure
  (WinRT Bluetooth needs the desktop session, so a session-0 service cannot work).

## Release

    git tag v0.1.0 && git push origin v0.1.0           # full release
    git tag v0.2.0-dev.1 && git push origin v0.2.0-dev.1   # prerelease

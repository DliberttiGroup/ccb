# Connecting a device to CCB

[Docs index](index.md)

`ccb install`, `ccb run`, and `ccb logcat` talk to a device through `adb`.
CCB never exposes adb to the public internet. The supported route from this
VM to the user's phone is **wireless debugging over the private Tailscale
network**.

## Why not plain adb over the internet

adb has no authentication: anyone who can reach an adb port can run
commands on the device. Never forward an adb port through ngrok, a
Cloudflare tunnel, or any other public tunnel, and never run `adb tcpip`
on a public network.

## The supported route: wireless debugging over Tailscale

The phone and this VM are on the same private Tailscale tailnet. Only
tailnet devices can reach each other, so adb traffic stays inside the
private mesh.

On the phone (one-time setup):

1. Enable Developer options (Settings → About phone → tap Build number 7
   times, if not already enabled).
2. Open Developer options → **Wireless debugging** → turn it on.
3. Tap **Pair device with pairing code**. The phone shows an IP address, a
   pairing port, and a 6-digit code. Note the IP: it is the phone's tailnet
   address (tailnet name `pixel-7-pro-1.tail428c53.ts.net`).
4. On this VM, pair once (the pairing port is only for pairing):
   `adb pair <phone-tailnet-ip>:<pairing-port>`, enter the code.
5. Then connect (the phone also shows a separate connect port under
   Wireless debugging): `adb connect <phone-tailnet-ip>:<connect-port>`.
6. Verify: `adb devices` shows the phone as `device`.

After that, `ccb install`, `ccb run`, and `ccb logcat` work unchanged. The
pairing survives reconnects; if the phone reboots or the tailnet address
changes, repeat steps 3–5.

## What CCB does when no device is attached

| Command | Behavior |
| ------- | -------- |
| `ccb install`, `ccb run` | Fail with `CCB-E5001` and a concrete next step; never report success without a device. |
| `ccb logcat` | Checks `adb devices` first and fails fast with `CCB-E5001` instead of blocking on adb's "- waiting for device -". |
| `ccb install --dry-run`, `ccb run --dry-run` | Print the exact adb commands that would run, without touching a device. |

## Notes

- The phone's Termux sshd (over the same tailnet) is a separate channel
  for shell access; it is not adb and does not make `ccb install` work.
- `ccb install` installs the debug APK from
  `build/outputs/debug/app.apk` (or `--variant release` for release).
  `ccb run` always builds first, then installs, then launches the
  MAIN/LAUNCHER activity from the manifest.

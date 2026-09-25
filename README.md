# ESP32 BLE WiFi + OTA

Open-source ESP32 firmware that lets a device be configured over Bluetooth
Low Energy — send WiFi credentials, trigger OTA updates, read logs — through
a single BLE GATT characteristic. Includes a browser-based (Web Bluetooth)
control page, no app install needed.

Everything the firmware does is controlled by the config block at the top
of `firmware.ino`. No hidden behavior — this section documents every setting.

## Features

- **BLE WiFi provisioning** — send `WIFI:ssid|password` over BLE, device
  connects and reports back the result.
- **BLE-triggered OTA** — send `OTA`, device fetches and flashes new
  firmware from a configured URL, then reboots.
- **Raw command echo** — any other string sent over BLE is echoed back,
  useful for a debug/serial-style console over BLE.
- **Web control page** (`index.html`) — connect, provision WiFi, trigger
  OTA, and send raw commands from any Web Bluetooth-capable browser.
- **Independent logging channels** — Serial and BLE-notify logging can each
  be turned on/off separately.
- **Optional MAC-based device binding** — firmware can bind itself to the
  first chip it boots on and refuse to run on a different one.
- **Compile-time feature toggles** — disabled features are stripped from
  the binary via `#if`, not just skipped at runtime, keeping flash usage down.

## Hardware

Any ESP32 variant with BLE + WiFi.

## Getting Started

1. Open `firmware.ino` and review the config block (see table below).
2. Flash it via Arduino IDE or PlatformIO.
3. Serve `index.html` over **HTTPS** — Web Bluetooth only
   works in a secure context, it will not work from a plain `http://` host
   or a local `file://` path. Options:
   - Host it on GitHub Pages (serves over HTTPS automatically)
4. Open the page in Chrome or Edge (Web Bluetooth support required).
5. Click **Connect Bluetooth**, then use the WiFi and OTA controls.

## Configuration reference

All settings live in the `GLOBAL CONFIG` block at the top of `firmware.ino`.

| Setting | Type | Description |
|---|---|---|
| `BLE_SERVICE_UUID` / `BLE_CHAR_UUID` | UUID string | BLE GATT service and characteristic UUIDs. Must match between firmware and `index.html`. |
| `BLE_NAME_PREFIX` | string | Prepended to a 12-char hex MAC to form the advertised BLE device name (max ~8 chars — enforced at compile time via `static_assert`). |
| `OTA_PATH_HEX` | byte array | The OTA firmware URL, stored as raw hex bytes and decoded at runtime. This is **not encryption or a secret** — it decodes to a plain URL (see `buildOtaUrl()`); it only avoids the URL showing up on a plain `strings`/grep pass. Since this is open source, feel free to just hardcode a plain string here if you fork it — nothing is hidden by this. |
| `NVS_NAMESPACE` / `NVS_KEY_MAC` | string | Storage namespace/key used for the MAC-binding feature (flash NVS via `Preferences`). |
| `WIFI_CONNECT_TIMEOUT_MS` | int | How long to wait for `WiFi.begin()` to succeed before giving up. |
| `ENABLE_SERIAL_PRINT` | 0/1 | Enable/disable all Serial logging. Disabled = code fully removed at compile time. |
| `ENABLE_BLE_NOTIFY` | 0/1 | Enable/disable logging back over the BLE characteristic (independent of Serial). |
| `ENABLE_HTTPS` | 0/1 | `1` = OTA download over HTTPS (`WiFiClientSecure`, with `setInsecure()` — see note below); `0` = plain HTTP, and the HTTPS library is excluded from the build entirely. |
| `ENABLE_DEVICE_BINDING` | 0/1 | `1` = bind firmware to the first MAC it boots on, refusing to run on any other chip; `0` = disabled, code fully removed at compile time. |
| `SERIAL_BAUD_RATE` | int | Serial port baud rate. |

> **Security note:** `ENABLE_HTTPS=1` encrypts the OTA download but calls
> `client.setInsecure()`, which skips certificate validation. It protects
> against passive eavesdropping, not against an active man-in-the-middle.
> Add certificate pinning if you need stronger guarantees.
>
> The BLE write characteristic has no pairing/authentication — any device
> in range can connect and send WiFi credentials or trigger OTA. This is
> fine for controlled testing but should be hardened (BLE pairing/bonding,
> or an app-layer token) before use on a production device.

## Repo layout

```
firmware.ino    # ESP32 firmware
index.html      # Web Bluetooth control page
```

## License

MIT (see LICENSE)

# AP_OTAUpdater

OTA firmware updater for [AP OTA Server](https://github.com/valachbastl/AP_OTAServer). Supports HTTP and HTTPS, works with any network interface — WiFi, W5500, LAN8720A, or ESP32-P4 built-in EMAC.

Compatible with AP_TaskUtils tasks — watchdog is managed transparently during update.

Detects any version difference (not just newer), so both upgrades and downgrades work.

## Requirements

- ESP-IDF 5.x or 6.x (SHA-256 via mbedTLS on 5.x, PSA Crypto on 6.x — selected automatically)
- AP OTA Server; size and SHA-256 checks need server **1.2.0+** (older servers work without them)
- OTA partition table (`ota_0` + `ota_1`), caller task stack ≥ 8 KB

## Installation

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/valachbastl/AP_OTAUpdater.git
```

Or with specific version:

```ini
lib_deps =
    https://github.com/valachbastl/AP_OTAUpdater.git#v1.3.0
```

## Partition table

Project must have an OTA-capable partition table (`partitions.csv`):

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x5000
otadata,  data, ota,     0xe000,   0x2000
ota_0,    app,  ota_0,   0x10000,  0x1F0000
ota_1,    app,  ota_1,   0x200000, 0x1F0000
```

## Minimal usage

Standalone FreeRTOS task — no additional libraries required:

```cpp
#include "AP_OTAUpdater.h"
#include "esp_mac.h"

static void otaTask(void* pv) {
    uint8_t mac[6];
    esp_base_mac_addr_get(mac);
    char macStr[13];
    snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    AP_OTAUpdater ota("https://ota.example.com", "home", "sensor", "main", "1.0", macStr);
    ota.setCurrentVersion("26.5.0");
    ota.setScheduleInterval(300);  // check every 5 minutes

    while (1) {
        if (ota.isDue() && ota.check()) esp_restart();
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
```

## HTTPS

### Certificate bundle (Let's Encrypt, etc.) — default, no extra setup

Requires `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` in menuconfig (on by default in ESP-IDF).

```cpp
AP_OTAUpdater ota("https://ota.example.com", "prod", "board", "main", "1.0", mac);
```

### Custom CA certificate

```cpp
static const char* ROOT_CA = "-----BEGIN CERTIFICATE-----\n"
                             "...\n"
                             "-----END CERTIFICATE-----\n";
ota.setCACert(ROOT_CA);
```

### Without certificate verification

Requires `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y` in menuconfig.

```cpp
ota.setInsecure();
```

## Authentication (OTA_AUTH)

When the server has `OTA_AUTH = true` set in `config.php`, all requests must include the `X-OTA-Key` header matching `APP_SECRET`:

```cpp
ota.setOTAKey("your-app-secret");
```

## Integrity checks (AP OTA Server ≥ 1.2.0)

The server sends `X-OTA-Size` and `X-OTA-SHA256` with the offered firmware. The library then:

- refuses firmware that does not fit the OTA partition **before downloading** (`Result::TooLarge`),
- verifies the downloaded size and that the stream ended properly,
- computes SHA-256 while writing and compares it **before** the boot partition is switched (`Result::HashMismatch` → image discarded, device stays on the current version).

`Content-Length` is often lost on the way (hosting gzip + Cloudflare → chunked); `X-OTA-Size` always arrives. With an older server everything still works, only without these checks. To refuse updates without a hash:

```cpp
ota.requireHash(true);
```

## Grouping chips into one device (unit)

Chips sending the same unit key are grouped into one device on the server. Typically the main chip sends its own MAC and secondary chips receive it over an internal link at startup (RAM only, no NVS):

```cpp
ota.setUnitFromMAC(mainChipMac);   // or ota.setUnit("aa:bb:cc:dd:ee:ff")
```

Key = `[a-z0-9_-]`, 4–32 characters, lowercase, `:` dropped (same normalization as the server). Until the key is known, don't set it — the parameter is then not sent.

## Result and callbacks

```cpp
AP_OTAUpdater::Result r = ota.update();
if (r == AP_OTAUpdater::Result::Updated) esp_restart();
ESP_LOGI("OTA", "Result: %s", AP_OTAUpdater::resultToString(r));
```

`check()` / `performUpdate()` still return `bool`; the reason is available via `getLastResult()`.

```cpp
ota.setStartCallback([](const char* version, int64_t size, void* ctx) { /* size -1 = unknown */ });
ota.setProgressCallback([](int progress) {
    ESP_LOGI("OTA", "Download: %d%%", progress);   // only when size is known
});
ota.setProgressBytesCallback([](int64_t written, int64_t total, void* ctx) { /* total -1 = unknown */ });
ota.setResultCallback([](AP_OTAUpdater::Result r, void* ctx) { /* after every started update */ });
```

The optional last argument of the setters (`userData`) is passed unchanged to the callback as `ctx` — e.g. a pointer to your own object:

```cpp
ota.setResultCallback([](AP_OTAUpdater::Result r, void* ctx) {
    static_cast<MyApp*>(ctx)->onOtaResult(r);
}, &app);
```

| Result | Meaning |
|--------|---------|
| `UpToDate` | Server offers the running version (also when the server blocks the update) |
| `Updated` | Installed — call `esp_restart()` |
| `NoFirmware` | No firmware on the server for this group/type/component/hw (404) |
| `ServerError` / `NetworkError` | HTTP status / connection problem |
| `NoPartition` / `TooLarge` | No OTA partition / firmware does not fit |
| `NoMemory` / `WriteError` | Allocation / flash write failure |
| `SizeMismatch` | Incomplete or oversized download |
| `HashMissing` / `HashMismatch` | No SHA-256 with `requireHash(true)` / SHA-256 differs |

## Rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`)

The library installs and verifies the image; deciding whether the new firmware *works* is up to the application. With app rollback enabled, the new firmware boots as "pending verify" — the application must confirm it once it is healthy (e.g. network up, peripherals OK), otherwise the bootloader returns to the previous version on the next reset:

```cpp
#include "esp_ota_ops.h"

esp_ota_img_states_t state;
if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
    state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (selfTestOk) esp_ota_mark_app_valid_cancel_rollback();
    else            esp_ota_mark_app_invalid_rollback_and_reboot();
}
```

Without rollback enabled nothing needs to be done.

## Request URL

All query values are percent-encoded (RFC 3986). Slugs, versions and MAC addresses contain only unreserved characters, so the URL stays unchanged for typical values; PHP decodes `$_GET` automatically. A URL that would exceed the internal buffer is refused with an error log instead of being truncated.

## Power management

With `CONFIG_PM_ENABLE` the library holds an `ESP_PM_CPU_FREQ_MAX` lock during the firmware download. This avoids an ESP-IDF `esp_pm` deadlock when the CPU frequency is switched under heavy interrupt load (observed on ESP32-P4 + SDIO → interrupt watchdog). Nothing to configure.

## API Reference

### Constructor

| Parameter    | Description |
|--------------|-------------|
| `serverURL`  | Base server URL, e.g. `"https://ota.example.com"` |
| `group`      | Device group slug, e.g. `"production"` |
| `type`       | Device type slug, e.g. `"display"` |
| `component`  | Component slug, e.g. `"main"` |
| `hwVersion`  | Hardware version, e.g. `"1.0"` |
| `deviceMAC`  | MAC address — any format (`"AA:BB:CC:DD:EE:FF"` or `"aabbccddeeff"`) |

All slug parameters must match the values registered on the server.

### Methods

| Method | Description |
|--------|-------------|
| `setCurrentVersion(ver)` | Set current firmware version for comparison |
| `getCurrentVersion()` | Returns configured version string |
| `getLastServerVersion()` | Returns last version fetched from server |
| `setOTAKey(key)` | Set auth key (X-OTA-Key header, OTA_AUTH on server) |
| `setCACert(pem)` | HTTPS with custom CA certificate |
| `setInsecure()` | HTTPS without certificate verification |
| `setTimeout(ms)` | Network timeout in ms (default 15000) |
| `requireHash(bool)` | Refuse updates without `X-OTA-SHA256` |
| `setUnit(key)` / `setUnitFromMAC(mac)` | Unit key for grouping chips (`&unit=`) |
| `getUnit()` | Current unit key (`""` = not set) |
| `getLastServerSize()` | Firmware size from last check (`X-OTA-Size`, -1 = unknown) |
| `setProgressCallback(cb)` | Download progress callback (0–100, size known only) |
| `setProgressBytesCallback(cb, userData)` | Download progress in bytes (total -1 = unknown) |
| `setStartCallback(cb, userData)` / `setResultCallback(cb, userData)` | Update start (version, size) / final result |
| `setScheduleInterval(s)` | Check every N seconds (min. 30) |
| `setScheduleDaily(h, m)` | Check every day at HH:MM (requires SNTP) |
| `setScheduleWeekly(d, h, m)` | Check on day-of-week at HH:MM (requires SNTP) |
| `isDue()` | Returns true if it is time to check per configured schedule |
| `getLastCheckTime()` | Unix timestamp of last check (0 = never; resets on restart) |
| `getServerVersion(buf, len)` | Fetch version string from server into buf |
| `isUpdateAvailable()` | Returns true if server version differs from current |
| `update()` | Check + download + verify + install; returns `Result` |
| `performUpdate()` | `update()` as bool; true on success |
| `check()` | `performUpdate()` + marks last check time; use with `isDue()` |
| `getLastResult()` / `resultToString(r)` | Result of the last update attempt / its name |

### Schedule modes

| Mode | isDue() behaviour | Recommended task interval |
|------|-------------------|--------------------------|
| `MANUAL` | Always false — call `check()` directly | Any (e.g. EVENT mode) |
| `INTERVAL` | True after N seconds elapsed | Equal to OTA interval |
| `DAILY` | True once per day after HH:MM (requires synced time) | 60 s |
| `WEEKLY` | True once per week on set day after HH:MM (requires synced time) | 60 s |

**Note:** last-check state is stored in RAM and resets on every restart. On boot, `INTERVAL` mode triggers immediately; `DAILY`/`WEEKLY` trigger if the scheduled time has already passed today/this week. `INTERVAL` timing runs off the monotonic `esp_timer`, so it works correctly even without SNTP/RTC sync; `DAILY`/`WEEKLY` need synced wall-clock time (`getLastCheckTime()` reflects it once synced).

## Author

Petr Adámek

## License

MIT — see [LICENSE](LICENSE).

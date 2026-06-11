# AP_OTAUpdater

OTA firmware updater for [AP OTA Server](https://github.com/valachbastl/AP_OTAServer). Supports HTTP and HTTPS, works with any network interface — WiFi, W5500, LAN8720A, or ESP32-P4 built-in EMAC.

Compatible with AP_TaskUtils tasks — watchdog is managed transparently during update.

Detects any version difference (not just newer), so both upgrades and downgrades work.

## Installation

Add to `platformio.ini`:

```ini
lib_deps =
    https://github.com/valachbastl/AP_OTAUpdater.git
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

## Progress callback

```cpp
ota.setProgressCallback([](int progress) {
    ESP_LOGI("OTA", "Download: %d%%", progress);
});
```

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
| `setProgressCallback(cb)` | Download progress callback (0–100) |
| `setScheduleInterval(s)` | Check every N seconds (min. 30) |
| `setScheduleDaily(h, m)` | Check every day at HH:MM (requires SNTP) |
| `setScheduleWeekly(d, h, m)` | Check on day-of-week at HH:MM (requires SNTP) |
| `isDue()` | Returns true if it is time to check per configured schedule |
| `getLastCheckTime()` | Unix timestamp of last check (0 = never; resets on restart) |
| `getServerVersion(buf, len)` | Fetch version string from server into buf |
| `isUpdateAvailable()` | Returns true if server version differs from current |
| `performUpdate()` | Check + download + install; returns true on success |
| `check()` | `performUpdate()` + marks last check time; use with `isDue()` |

### Schedule modes

| Mode | isDue() behaviour | Recommended task interval |
|------|-------------------|--------------------------|
| `MANUAL` | Always false — call `check()` directly | Any (e.g. EVENT mode) |
| `INTERVAL` | True after N seconds elapsed | Equal to OTA interval |
| `DAILY` | True once per day after HH:MM (requires synced time) | 60 s |
| `WEEKLY` | True once per week on set day after HH:MM (requires synced time) | 60 s |

**Note:** `_lastCheckTime` is stored in RAM and resets on every restart. On boot, `INTERVAL` mode triggers immediately; `DAILY`/`WEEKLY` trigger if the scheduled time has already passed today/this week.

## Author

Petr Adámek

## License

MIT — see [LICENSE](LICENSE).

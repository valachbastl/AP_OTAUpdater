# Changelog

## [1.1.0] - 2026-06-11

### Changed
- Interni refaktor: spolecny HTTP boilerplate (`init` + `X-OTA-Key` + WDT delete + `open` + `fetch_headers` + status check + cleanup) vytazen do privatniho `_openClient()` — sdileno `downloadString()` a `downloadFirmware()`, odstranena duplicita ~25 radku. Chovani i WDT logika beze zmeny.
- Licence zmenena na MIT (drive UNLICENSED).

## [1.0.0] - 2026-05-28

### Added
- Protokol AP OTA Server (`ota.php`): dvoukrokový GET — zjištění verze + stažení binárky
- Parametry requestu: `group`, `type`, `component`, `hw`, `device` (MAC), `fw`, `uptime`, `interval`
- `setCurrentVersion()` — nastavení aktuální verze pro porovnání
- `getServerVersion()` — stažení aktuální verze ze serveru do bufferu
- `isUpdateAvailable()` — detekuje libovolný rozdíl verze (upgrade i downgrade)
- `performUpdate()` — kontrola verze + stažení + instalace do OTA oddílu
- `check()` — kombinuje `performUpdate()` + `markChecked()`; hlavní metoda pro periodický task
- `getLastServerVersion()` — vrátí naposledy zjištěnou verzi ze serveru
- `setOTAKey()` — autentizace přes X-OTA-Key hlavičku (OTA_AUTH + APP_SECRET na serveru)
- `setCACert()` — HTTPS s vlastním CA certifikátem (PEM)
- `setInsecure()` — HTTPS bez ověření certifikátu (vyžaduje `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`)
- `setTimeout()` — nastavení síťového timeoutu (default 15 s)
- `setProgressCallback()` — callback s procentem průběhu stahování (0–100)
- Plánovač — `isDue()` rozhoduje kdy spustit kontrolu, task řídí uživatel (typicky AP_TaskUtils):
  - `setScheduleInterval(seconds)` — každých N sekund od poslední kontroly (min. 30 s)
  - `setScheduleDaily(hour, minute)` — každý den v HH:MM (vyžaduje SNTP)
  - `setScheduleWeekly(day, hour, minute)` — konkrétní den v týdnu v HH:MM (vyžaduje SNTP)
  - `getLastCheckTime()` — Unix timestamp poslední kontroly (0 = nikdy, resetuje se restartem)
- Normalizace MAC adresy — přijímá libovolný formát ("AA:BB:CC:DD:EE:FF" nebo "aabbccddeeff")
- Automatická správa WDT — kompatibilní s AP_TaskUtils tasky
- Podpora: WiFi, W5500, LAN8720A, ESP32-P4 EMAC (automaticky přes ESP-IDF netif)

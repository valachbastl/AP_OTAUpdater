# Changelog

## [1.2.0] - 2026-07-27

### Fixed
- `downloadString()` nyní čte HTTP odpověď ve smyčce až do EOF/zaplnění bufferu místo jednoho volání `esp_http_client_read()` — odpověď rozdělená přes více TCP segmentů mohla být dřív tiše useknutá (`downloadFirmware()` už smyčkoval správně).

### Changed
- Plánování `INTERVAL` (`isDue()` / `markChecked()`) přepnuto z wall-clock `time(nullptr)` na monotónní `esp_timer_get_time()` — hardening, ne oprava konkrétního pozorovaného bugu (ten byl v `AP_TaskUtils`, viz jeho changelog v2.6.0 — DFS/esp_pm rozhazovalo FreeRTOS tick, takže periodický task volal `isDue()` méně často, než měl). Monotónní čas je pro měření intervalu obecně robustnější než wall-clock: `time()` může skočit v okamžiku SNTP synchronizace (dřív nesynchronizováno → naráz o desítky let), což by delta výpočet zkreslilo. `DAILY`/`WEEKLY` nejsou dotčené — ty ze své podstaty potřebují wall-clock čas dne a nadále se spoléhají na `time()`/SNTP, jak je zdokumentováno.
- Všechny komentáře ve zdrojácích přeloženy z češtiny do angličtiny, pro konzistenci s `AP_DS18B20`/`AP_TaskUtils` (obecná konvence od 2026-07-27, viz `_LIBRARIES_STATUS.md`).
- Zdokumentováno, že instance není thread-safe (volat jen z jednoho tasku) — beze změny chování, jen explicitní.

**Otestováno na HW** — potvrzeno; ještě proběhne dodatečné ověření před nasazením do produkce.

## [1.1.1] - 2026-06-16

### Added
- ESP-IDF component baleni (`CMakeLists.txt` + `idf_component.yml`) - pouzitelne jako cista ESP-IDF komponenta, nejen pres PlatformIO. Bez zmeny kodu.

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

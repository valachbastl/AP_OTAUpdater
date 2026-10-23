# Changelog

## [1.4.0] - 2026-10-23

Odložená aktualizace — zařízení se dál hlásí podle plánu, ale stahování může aplikace dočasně zakázat (nízká baterie, probíhající proces…) a server vidí proč. Vydáno spolu s AP_OTAServer 1.3.0 (stav `deferred`, pilotní vydání) a taskOTA 2.3.0.

### Added
- `check(const char* deferReason = nullptr)` — s důvodem jen ohlášení (krok 1 s `&deferred=<důvod>`), nic se nestahuje, plánovač se posune. Bez důvodu beze změny proti 1.3.0.
- `Result::Deferred` — server nabízí jinou verzi, stažení odloženo; shodná verze dál `UpToDate`. Result callback se volá i s `Deferred` (start callback ne, nic se nestahuje).
- `isUpdatePending()` — poslední kontrola skončila `Deferred` → po povolení zavolat `update()` hned, bez čekání na další termín.
- Důvod je slug `[a-z0-9_-]{1,32}` se stejnou normalizací jako `unit`, posílá se jen v kroku 1.

### Changed
- Normalizace slugu sdílená pro `setUnit()` a důvod odložení (beze změny chování `setUnit()`).

**Otestováno na HW** 2026-10-23 — měření teploty serverovny (ESP32 / LaskaKit ESPlan, LAN8720A, ESP-IDF 6.1, taskOTA 2.3.0) proti AP_OTAServer 1.3.0: pilotní verze 26.10.2 nabídnuta jen testovacímu zařízení; `check("battery")` → `Deferred`, nic nestaženo, server „Odloženo: battery“; po povolení `isUpdatePending()` → instalace do minuty (taskOTA), 758 832 B, SHA-256 OK, `Updated`, nová verze naběhla; ohlášení s důvodem při shodné verzi → `UpToDate`, server odložení zrušil. Regrese: `check()` bez důvodu ve finálním FW 26.10.3 → běžná kontrola, `UpToDate`.

## [1.3.0] - 2026-10-21

Podpora AP_OTAServer 1.2.0 (hlavičky `X-OTA-Size` / `X-OTA-SHA256`, parametr `unit`) + ESP-IDF 6. Se starším serverem funguje dál, jen bez kontrol velikosti a hashe.

### Added
- Kontrola **SHA-256** staženého obrazu proti `X-OTA-SHA256` — počítá se průběžně při zápisu, při neshodě `esp_ota_abort()` a boot oddíl se nepřepne. Bez hlavičky (server < 1.2.0) se kontrola přeskočí s varováním; `requireHash(true)` takový update odmítne.
- Velikost firmwaru z `X-OTA-Size` (pořadí `X-OTA-Size` → `Content-Length` → neznámá), hlavičky odpovědi čtené přes `event_handler` HTTP klienta.
- Kontrola, že se FW vejde do OTA oddílu — už po kroku 1 (vůbec se nestahuje) a znovu před `esp_ota_begin()`. Flash se dál maže po sektorech během zápisu (`OTA_WITH_SEQUENTIAL_WRITES`) i při známé velikosti — zkoušené jednorázové smazání celého obrazu předem (~2 s na 1 MB) se na HOME-Thermostat BOARD časově shodovalo s `STALE_PACKET_ERROR` na lince k displeji, mimo OTA se tahle chyba neobjevuje.
- `setUnit()` / `setUnitFromMAC()` / `getUnit()` — seskupení čipů do přístroje (`&unit=` v kroku 1), normalizace a validace jako na serveru, jen v RAM.
- `enum class Result` + `update()`, `getLastResult()`, `resultToString()` — důvod, proč update neproběhl (`NoFirmware`, `TooLarge`, `HashMismatch`, …). `bool` varianty (`performUpdate()`, `check()`) zachovány.
- Callbacky `setStartCallback()` (verze + velikost), `setProgressBytesCallback()` (bajty, i při neznámé velikosti), `setResultCallback()` — s volitelným `userData` (kontext předaný do callbacku); `getLastServerSize()`.
- README: sekce o rollbacku (`esp_ota_mark_app_valid_cancel_rollback()` je úkol aplikace) a o kódování URL.
- Během stahování se při `CONFIG_PM_ENABLE` drží PM zámek `ESP_PM_CPU_FREQ_MAX` (RAII, uvolní se na všech cestách) — obchází deadlock v `esp_pm` při přepínání frekvence pod hustým tokem přerušení (zjištěno na ESP32-P4 + SDIO při přenosu v AP_HostedUpdater 2026-10-19, stejná oprava tam ověřena na HW). `esp_pm` přidán do `REQUIRES`.

### Fixed
- Při odpovědi bez `Content-Length` (gzip hostingu + Cloudflare → chunked, ověřeno curlem na produkci) se nevolal progress callback a přeskočila se kontrola úplnosti — teď se velikost bere z `X-OTA-Size` a navíc se kontroluje `esp_http_client_is_complete_data_received()` (předčasně ukončený přenos = `SizeMismatch`).
- `getServerVersion()` nyní opravdu aktualizuje `getLastServerVersion()`, jak tvrdila dokumentace (dřív to dělal jen `isUpdateAvailable()`).

### Changed
- ESP-IDF 6: SHA-256 přes PSA Crypto API (mbedTLS 4 / TF-PSA-Crypto už nemá veřejné `mbedtls_sha256_*`); na IDF 5.x se dál použije `mbedtls_sha256_*`.
- HTTP 404 v kroku 1 (na serveru není FW pro tuto kombinaci) se loguje jako info, ne warning. 404 v kroku 2 (politika změněná mezi kroky) → `ServerError`.
- `setProgressCallback()` se volá jen při změně procenta (dřív po každém 4kB bloku).
- Hodnoty v URL se kódují (RFC 3986) — pro běžné slugy/verze/MAC se URL nemění; příliš dlouhá URL se odmítne s chybou místo tichého useknutí (dřív `snprintf` do 768 B).
- Konstruktor a settery snesou `nullptr` (bere se jako prázdný řetězec / zrušení).

**Otestováno na HW** — HOME-Thermostat BOARD (ESP32-P4, W5500, ESP-IDF 6.1) proti produkčnímu serveru přes Cloudflare: reálný update 26.10.3 → 26.10.4, velikost z `X-OTA-Size`, SHA-256 OK, naběhla nová verze, 404 bez FW i `UpToDate` správně. Dále HOME-Thermostat DISPLAY (ESP32-S3, WiFi): update 26.10.3 → 26.10.4 OK.

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

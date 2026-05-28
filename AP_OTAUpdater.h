#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include <time.h>

/**
 * AP_OTAUpdater — OTA klient pro AP OTA Server
 *
 * Protokol (AP OTA Server ota.php):
 *   Krok 1 – GET /ota.php?group=&type=&component=&hw=&device=&fw=&uptime=&interval=
 *             → 200 plain text "26.5.1"   |  404 žádný firmware
 *   Krok 2 – GET /ota.php?...&download=1
 *             → 200 binární data firmware
 *
 * Autentizace: setOTAKey() — X-OTA-Key header (APP_SECRET na serveru, OTA_AUTH=true)
 * TLS:         embedded certificate bundle (default), vlastní CA nebo bez ověření
 * WDT:         automatická správa kompatibilní s AP_TaskUtils tasky
 * Verzování:   porovnává se libovolný rozdíl verzí → funguje upgrade i downgrade
 *
 * Typické použití v AP_TaskUtils tasku:
 *
 *   static AP_OTAUpdater ota(SERVER_URL, "prod", "board", "main", "1.0", macStr);
 *
 *   static void otaTask(void *pv) {
 *       AP_TaskUtils task("otaTask", 60000, AP_TaskUtils::DELAY);
 *       ota.setCurrentVersion(FIRMWARE_VERSION);
 *       ota.setScheduleDaily(12, 0);
 *       task.waitReady("networkTask");
 *       while (1) {
 *           if (ota.isDue() && ota.check()) esp_restart();
 *           task.wait();
 *       }
 *   }
 */
class AP_OTAUpdater
{
public:
    enum Schedule {
        MANUAL,   ///< isDue() vždy false — check() volat ručně dle potřeby
        INTERVAL, ///< každých N sekund od poslední kontroly
        DAILY,    ///< každý den v HH:MM  (vyžaduje synchronizovaný čas — SNTP)
        WEEKLY    ///< konkrétní den v týdnu v HH:MM (vyžaduje synchronizovaný čas)
    };

    /**
     * @brief Constructor
     * @param serverURL  Základní URL serveru, např. "https://ota.example.com"
     * @param group      Skupina zařízení (slug), např. "production"
     * @param type       Typ zařízení (slug), např. "display"
     * @param component  Komponenta (slug), např. "main"
     * @param hwVersion  HW verze, např. "1.0"
     * @param deviceMAC  MAC adresa — libovolný formát ("AA:BB:CC:DD:EE:FF" nebo
     *                   "aabbccddeeff"); automaticky normalizována na lowercase hex string
     *
     * Slug parametry musí odpovídat hodnotám registrovaným na serveru.
     * Kompatibilní s libovolným síťovým rozhraním (WiFi, W5500, LAN8720A, ESP32-P4 EMAC).
     */
    AP_OTAUpdater(const char* serverURL,
                  const char* group,
                  const char* type,
                  const char* component,
                  const char* hwVersion,
                  const char* deviceMAC);

    // ── Firmware verze ──────────────────────────────────────────────────────────

    /** @brief Nastavit aktuální verzi firmware pro porovnání, např. "26.5.0" */
    void setCurrentVersion(const char* version);

    /** @brief Vrátí aktuálně nastavenou verzi */
    const char* getCurrentVersion() const;

    /** @brief Vrátí poslední verzi zjištěnou ze serveru (po volání getServerVersion / check) */
    const char* getLastServerVersion() const;

    // ── Auth & TLS ─────────────────────────────────────────────────────────────

    /**
     * @brief Nastavit OTA klíč pro autentizaci (APP_SECRET na serveru, OTA_AUTH=true)
     *        Posílá se jako hlavička X-OTA-Key.
     *        Pokud není nastaven, hlavička se neposílá (server bez OTA_AUTH).
     */
    void setOTAKey(const char* key);

    /**
     * @brief CA certifikát pro HTTPS ověření (PEM formát, null-terminated)
     *        Pokud nenastaveno, používá embedded certificate bundle (Let's Encrypt atd.)
     *        Vyžaduje CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y (default v ESP-IDF)
     */
    void setCACert(const char* rootCA);

    /**
     * @brief Vypnout ověření HTTPS certifikátu
     *        Vyžaduje CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y v menuconfig
     */
    void setInsecure(bool insecure = true);

    /** @brief Síťový timeout v ms (default 15000) */
    void setTimeout(int ms);

    // ── Plánovač ───────────────────────────────────────────────────────────────

    /**
     * @brief Periodická kontrola každých N sekund od poslední kontroly.
     *        isDue() vrátí true ihned při prvním volání (lastCheckTime == 0),
     *        poté vždy po uplynutí intervalu.
     * @param seconds Interval v sekundách (min. 30)
     */
    void setScheduleInterval(uint32_t seconds);

    /**
     * @brief Denní kontrola v zadaný čas.
     *        isDue() vrátí true poprvé v daný den od HH:MM; poté false až do dalšího dne.
     *        Pokud systémový čas není synchronizován (Unix timestamp < 1 000 000 000),
     *        isDue() vrátí false.
     *
     *        Před použitím nastavit timezone, např.:
     *          setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
     *          tzset();
     *
     * @param hour    Hodina (0–23)
     * @param minute  Minuta (0–59)
     */
    void setScheduleDaily(int hour, int minute);

    /**
     * @brief Týdenní kontrola v zadaný den a čas.
     *        isDue() vrátí true jednou za týden — v daný den od HH:MM.
     *        Pokud systémový čas není synchronizován, isDue() vrátí false.
     *
     *        Před použitím nastavit timezone, např.:
     *          setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
     *          tzset();
     *
     * @param dayOfWeek 0 = neděle, 1 = pondělí, 2 = úterý, 3 = středa,
     *                  4 = čtvrtek, 5 = pátek, 6 = sobota
     * @param hour      Hodina (0–23)
     * @param minute    Minuta (0–59)
     */
    void setScheduleWeekly(int dayOfWeek, int hour, int minute);

    /**
     * @brief Zjistit, zda je čas na kontrolu aktualizace.
     *
     *   MANUAL   → vždy false
     *   INTERVAL → true pokud uplynul interval od poslední kontroly
     *              (nebo ještě nebyla provedena žádná kontrola)
     *   DAILY    → true pokud aktuální čas >= HH:MM a dnes ještě nebyla kontrola
     *   WEEKLY   → true pokud je správný den týdne, čas >= HH:MM
     *              a tento týden ještě nebyla kontrola
     *
     * Volat z tasku dostatečně často, aby nedošlo k přeskočení okna:
     *   INTERVAL → stačí jednou za interval
     *   DAILY    → doporučeno alespoň jednou za minutu
     *   WEEKLY   → doporučeno alespoň jednou za minutu
     */
    bool isDue() const;

    /**
     * @brief Vrátí čas poslední kontroly jako Unix timestamp (0 = nikdy).
     *        Resetuje se při restartu — uloženo v RAM.
     */
    time_t getLastCheckTime() const;

    // ── OTA operace ────────────────────────────────────────────────────────────

    /**
     * @brief Stáhnout verzi firmware ze serveru do buf.
     *        Zároveň aktualizuje lastServerVersion a registruje check-in na serveru.
     * @return true pokud server odpověděl HTTP 200 s neprázdnou verzí
     */
    bool getServerVersion(char* buf, size_t len);

    /**
     * @brief Zjistit, zda server nabízí jinou verzi než aktuální.
     *        Detekuje jak upgrade, tak downgrade — porovnává se libovolný rozdíl verze.
     * @return true pokud se verze liší
     */
    bool isUpdateAvailable();

    /**
     * @brief Stáhnout firmware a zapsat do OTA oddílu.
     *        Interně zavolá isUpdateAvailable(); pokud se verze liší, stáhne a nainstaluje.
     * @return true pokud byl firmware úspěšně nainstalován
     *         — caller musí zavolat esp_restart() k aplikaci
     *
     * Vyžaduje partition table s ota_0 + ota_1.
     * Doporučená velikost stacku volajícího tasku: >= 8 KB
     */
    bool performUpdate();

    /**
     * @brief Zkontrolovat a případně stáhnout aktualizaci; označí čas poslední kontroly.
     *        Kombinuje performUpdate() a markChecked() — hlavní metoda pro periodický task.
     * @return true pokud byl firmware nainstalován — caller musí zavolat esp_restart()
     *
     * Příklad:
     *   if (ota.isDue() && ota.check()) esp_restart();
     */
    bool check();

    // ── Callback ───────────────────────────────────────────────────────────────

    /** @brief Callback s průběhem stahování firmware (0–100) */
    void setProgressCallback(void (*callback)(int progress));

private:
    static const char* TAG;
    static const int   CHUNK_SIZE = 4096;

    char        _serverURL[256];
    char        _group[64];
    char        _type[64];
    char        _component[64];
    char        _hwVersion[32];
    char        _deviceKey[18];
    char        _currentVersion[32];
    char        _lastServerVersion[32];
    char        _otaKey[128];
    const char* _rootCA;
    bool        _insecure;
    int         _timeoutMs;

    Schedule _schedule;
    uint32_t _intervalSeconds;
    int      _scheduledHour;
    int      _scheduledMinute;
    int      _scheduledDay;
    time_t   _lastCheckTime;

    void (*_progressCallback)(int progress);

    void     normalizeMAC(const char* mac, char* out, size_t len);
    void     buildURL(char* buf, size_t len, bool download);
    void     setupClientConfig(esp_http_client_config_t& config, const char* url);
    bool     downloadString(const char* url, char* buf, size_t len);
    bool     downloadFirmware(const char* url);
    void     markChecked();
    int64_t  getUptimeSeconds() const;
    uint32_t getServerInterval() const;
};

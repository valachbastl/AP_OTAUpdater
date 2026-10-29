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
 * AP_OTAUpdater — OTA client for AP OTA Server
 *
 * Protocol (AP OTA Server ota.php):
 *   Step 1 – GET /ota.php?group=&type=&component=&hw=&device=&fw=&uptime=&interval=[&unit=][&deferred=]
 *             → 200 plain text "26.5.1"   |  404 no firmware
 *             &deferred=<reason> — the device reports in but may not download now
 *             (check(reason)); older servers ignore it
 *   Step 2 – GET /ota.php?...&download=1
 *             → 200 binary firmware data
 *   Both steps (server >= 1.2.0) also send X-OTA-Size (bytes) and X-OTA-SHA256
 *   (lowercase hex) for the offered firmware. Content-Length may be lost on the
 *   way (hosting gzip + Cloudflare → chunked); the custom headers always arrive.
 *
 * Integrity:   the download is verified against X-OTA-Size and X-OTA-SHA256 before
 *              the boot partition is switched; older servers without these headers
 *              still work, just without the checks (see requireHash()).
 * Auth:        setOTAKey() — X-OTA-Key header (APP_SECRET on server, OTA_AUTH=true)
 * TLS:         embedded certificate bundle (default), custom CA, or no verification
 * WDT:         automatic handling, compatible with AP_TaskUtils tasks — the task is
 *              unregistered from the task WDT while it waits on the network (connect
 *              + download); a stalled connection ends with the network timeout
 *              (setTimeout(), default 15 s), not a WDT panic
 * Versioning:  compares any version difference → both upgrade and downgrade work
 *              (the server decides what to offer, incl. frozen devices / no-downgrade)
 * Threading:   instance is not thread-safe — call only from a single task
 * Power mgmt:  with CONFIG_PM_ENABLE an ESP_PM_CPU_FREQ_MAX lock is held while the
 *              firmware is downloaded — avoids an esp_pm frequency-switch deadlock
 *              under heavy interrupt load (seen on ESP32-P4 + SDIO, ESP-IDF 6.1)
 *
 * Time source: INTERVAL scheduling runs off the monotonic esp_timer (immune to
 * NTP/RTC drift or missing time sync). DAILY/WEEKLY inherently need wall-clock
 * time of day → they rely on time()/SNTP, as documented on those methods.
 *
 * Typical use in an AP_TaskUtils task:
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
 *
 * Deferred update — the application temporarily forbids downloading (low battery,
 * running process…) but keeps reporting to the server on schedule:
 *
 *       const char* reason = batteryLow() ? "battery" : nullptr;   // nullptr = allowed
 *       if (ota.isDue() && ota.check(reason)) esp_restart();
 *       if (!reason && ota.isUpdatePending() &&
 *           ota.update() == AP_OTAUpdater::Result::Updated) esp_restart();
 */
class AP_OTAUpdater
{
public:
    enum Schedule {
        MANUAL,   ///< isDue() always false — call check() manually as needed
        INTERVAL, ///< every N seconds since the last check
        DAILY,    ///< every day at HH:MM (requires synchronized time — SNTP)
        WEEKLY    ///< specific day of week at HH:MM (requires synchronized time)
    };

    /** @brief Outcome of an update attempt — see update(), getLastResult(), resultToString() */
    enum class Result {
        None,          ///< no update attempt made yet
        UpToDate,      ///< server offers the running version (also when the server blocks the update)
        Updated,       ///< firmware installed — caller must call esp_restart()
        NoFirmware,    ///< server has no firmware for this group/type/component/hw (HTTP 404 in step 1)
        ServerError,   ///< unexpected HTTP status or empty response
        NetworkError,  ///< connection / TLS / read failure
        NoPartition,   ///< no OTA partition in the partition table
        TooLarge,      ///< firmware does not fit into the OTA partition (nothing downloaded)
        NoMemory,      ///< buffer allocation failed
        WriteError,    ///< esp_ota_begin / write / end / set_boot_partition failed
        SizeMismatch,  ///< downloaded size differs from X-OTA-Size / Content-Length, or stream incomplete
        HashMissing,   ///< requireHash(true) and the server sent no X-OTA-SHA256
        HashMismatch,  ///< SHA-256 of the downloaded image differs from X-OTA-SHA256
        Deferred       ///< server offers a different version, download deferred by check(reason)
    };

    /** @brief Short name of a Result, e.g. "HashMismatch" */
    static const char* resultToString(Result result);

    /**
     * @brief Constructor
     * @param serverURL  Base server URL, e.g. "https://ota.example.com"
     * @param group      Device group (slug), e.g. "production"
     * @param type       Device type (slug), e.g. "display"
     * @param component  Component (slug), e.g. "main"
     * @param hwVersion  HW version, e.g. "1.0"
     * @param deviceMAC  MAC address — any format ("AA:BB:CC:DD:EE:FF" or
     *                   "aabbccddeeff"); automatically normalized to a lowercase hex string
     *
     * Slug parameters must match the values registered on the server.
     * Compatible with any network interface (WiFi, W5500, LAN8720A, ESP32-P4 EMAC).
     */
    AP_OTAUpdater(const char* serverURL,
                  const char* group,
                  const char* type,
                  const char* component,
                  const char* hwVersion,
                  const char* deviceMAC);

    // ── Firmware version ──────────────────────────────────────────────────────

    /** @brief Set current firmware version for comparison, e.g. "26.5.0" */
    void setCurrentVersion(const char* version);

    /** @brief Returns the currently configured version */
    const char* getCurrentVersion() const;

    /** @brief Returns the last version fetched from the server (after getServerVersion / check) */
    const char* getLastServerVersion() const;

    /**
     * @brief Size of the firmware offered by the server in the last version check
     *        (X-OTA-Size, server >= 1.2.0). -1 = unknown.
     */
    int64_t getLastServerSize() const;

    // ── Auth & TLS ─────────────────────────────────────────────────────────────

    /**
     * @brief Set the OTA auth key (APP_SECRET on server, OTA_AUTH=true)
     *        Sent as the X-OTA-Key header.
     *        If not set, the header is omitted (server without OTA_AUTH).
     */
    void setOTAKey(const char* key);

    /**
     * @brief CA certificate for HTTPS verification (PEM format, null-terminated)
     *        If not set, uses the embedded certificate bundle (Let's Encrypt etc.)
     *        Requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y (default in ESP-IDF)
     */
    void setCACert(const char* rootCA);

    /**
     * @brief Disable HTTPS certificate verification
     *        Requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y in menuconfig
     */
    void setInsecure(bool insecure = true);

    /** @brief Network timeout in ms (default 15000, min. 1000 — lower values are raised) */
    void setTimeout(int ms);

    /**
     * @brief Require the X-OTA-SHA256 header (AP OTA Server >= 1.2.0).
     *        false (default): without the header the hash check is skipped with a warning.
     *        true:            without the header the update is refused (Result::HashMissing).
     */
    void requireHash(bool require = true);

    // ── Unit (grouping chips of one device) ───────────────────────────────────

    /**
     * @brief Set the unit key — chips sending the same key are grouped into one
     *        device on the server (sent as &unit= in step 1).
     *        Typically the MAC of the main chip; secondary chips receive it over an
     *        internal link (UART…) at startup. Kept in RAM only — no NVS.
     *        Normalized like on the server: lowercase, ':' removed.
     *        Valid key = [a-z0-9_-], 4–32 characters.
     * @param key  Unit key; nullptr or "" clears it (parameter not sent)
     * @return true if set (or cleared); false if invalid — the key is then
     *         cleared and not sent
     */
    bool setUnit(const char* key);

    /** @brief setUnit() from a raw 6-byte MAC address (e.g. of the main chip) */
    void setUnitFromMAC(const uint8_t mac[6]);

    /** @brief Returns the current unit key ("" = not set) */
    const char* getUnit() const;

    // ── Scheduler ──────────────────────────────────────────────────────────────

    /**
     * @brief Periodic check every N seconds since the last check.
     *        isDue() returns true immediately on the first call (never checked yet),
     *        then again every time the interval elapses. Timed off the monotonic
     *        esp_timer — unaffected by RTC/NTP drift or an unsynchronized clock.
     * @param seconds Interval in seconds (min. 30)
     */
    void setScheduleInterval(uint32_t seconds);

    /**
     * @brief Daily check at a given time.
     *        isDue() returns true the first time it's called that day at/after HH:MM;
     *        false afterwards until the next day.
     *        If system time is not synchronized (Unix timestamp < 1,000,000,000),
     *        isDue() returns false.
     *
     *        Set the timezone before use, e.g.:
     *          setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
     *          tzset();
     *
     * @param hour    Hour (0–23)
     * @param minute  Minute (0–59)
     */
    void setScheduleDaily(int hour, int minute);

    /**
     * @brief Weekly check on a given day and time.
     *        isDue() returns true once a week — on the given day, at/after HH:MM.
     *        If system time is not synchronized, isDue() returns false.
     *
     *        Set the timezone before use, e.g.:
     *          setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
     *          tzset();
     *
     * @param dayOfWeek 0 = Sunday, 1 = Monday, 2 = Tuesday, 3 = Wednesday,
     *                  4 = Thursday, 5 = Friday, 6 = Saturday
     * @param hour      Hour (0–23)
     * @param minute    Minute (0–59)
     */
    void setScheduleWeekly(int dayOfWeek, int hour, int minute);

    /**
     * @brief Check whether it's time for an update check.
     *
     *   MANUAL   → always false
     *   INTERVAL → true once the interval has elapsed since the last check
     *              (or no check has been made yet)
     *   DAILY    → true if current time >= HH:MM and no check has happened today yet
     *   WEEKLY   → true if it's the right day of week, time >= HH:MM,
     *              and no check has happened this week yet
     *
     * Call from the task often enough that the window can't be missed:
     *   INTERVAL → once per interval is enough
     *   DAILY    → at least once a minute recommended
     *   WEEKLY   → at least once a minute recommended
     */
    bool isDue() const;

    /**
     * @brief Returns the last check time as a Unix timestamp (0 = never).
     *        Wall-clock display value only — INTERVAL scheduling uses the
     *        monotonic esp_timer internally and does not depend on this.
     *        Resets on restart — stored in RAM.
     */
    time_t getLastCheckTime() const;

    // ── OTA operations ────────────────────────────────────────────────────────

    /**
     * @brief Fetch the firmware version from the server into buf.
     *        Also updates lastServerVersion and registers the check-in on the server.
     * @return true if the server responded HTTP 200 with a non-empty version
     */
    bool getServerVersion(char* buf, size_t len);

    /**
     * @brief Check whether the server offers a different version than the current one.
     *        Detects both upgrade and downgrade — compares for any version difference.
     * @return true if the versions differ
     */
    bool isUpdateAvailable();

    /**
     * @brief Check the server and, if the version differs, download, verify and
     *        install the firmware.
     *        Before download: the image must fit the OTA partition (size from step 1).
     *        Before switching the boot partition: size and SHA-256 (when the server
     *        sends them) and complete data received.
     * @return Result::Updated if installed — caller must call esp_restart();
     *         otherwise the reason (also available via getLastResult())
     *
     * Requires a partition table with ota_0 + ota_1.
     * Recommended caller task stack size: >= 8 KB
     */
    Result update();

    /**
     * @brief update() as bool (kept for compatibility).
     * @return true if the firmware was installed — caller must call esp_restart()
     */
    bool performUpdate();

    /**
     * @brief Check for and optionally download an update; marks the last check time.
     *        Combines performUpdate() and markChecked() — main entry point for a periodic task.
     *
     * @param deferReason  nullptr (default) = download allowed, same as before.
     *        Otherwise the device only reports in (step 1 with &deferred=<reason>) and
     *        nothing is downloaded: a different offered version ends as Result::Deferred
     *        (isUpdatePending() then true), the same version as UpToDate. The schedule
     *        moves on either way, so the device does not look offline on the server.
     *        Reason = slug [a-z0-9_-], 1–32 chars (e.g. "battery", "busy"), normalized
     *        like setUnit(); an invalid one is sent as "unknown" — still deferred.
     *        Any request without a reason (update(), getServerVersion(), …) tells the
     *        server the device no longer defers.
     * @return true if the firmware was installed — caller must call esp_restart()
     *         (reason otherwise: getLastResult())
     *
     * Example:
     *   if (ota.isDue() && ota.check()) esp_restart();
     */
    bool check(const char* deferReason = nullptr);

    /**
     * @brief true if the last update() / check() ended with Result::Deferred — the
     *        server offers getLastServerVersion() and the application has not allowed
     *        the download yet. Once allowed again, call update() right away instead of
     *        waiting for the next isDue(). Cleared by the next update() / check().
     */
    bool isUpdatePending() const;

    /** @brief Result of the last update() / performUpdate() / check() */
    Result getLastResult() const;

    // ── Callbacks ──────────────────────────────────────────────────────────────

    /**
     * @brief Callback with firmware download progress in percent (0–100).
     *        Called only when the firmware size is known (X-OTA-Size or Content-Length).
     */
    void setProgressCallback(void (*callback)(int progress));

    /// Download progress in bytes; total = -1 when the size is unknown
    using ProgressCallback = void (*)(int64_t written, int64_t total, void* userData);
    /// Update started (server offers a different version); size = -1 when unknown
    using StartCallback    = void (*)(const char* version, int64_t size, void* userData);
    /// Final result of a started update (Updated, or the reason it failed), or Deferred
    using ResultCallback   = void (*)(Result result, void* userData);

    /**
     * @brief Callback with download progress in bytes.
     * @param userData passed unchanged to every call (e.g. a context object)
     */
    void setProgressBytesCallback(ProgressCallback callback, void* userData = nullptr);

    /**
     * @brief Callback when an update starts. Always followed by the result callback.
     * @param userData passed unchanged to every call
     */
    void setStartCallback(StartCallback callback, void* userData = nullptr);

    /**
     * @brief Callback with the final result of a started update, and with
     *        Result::Deferred (no start callback then — nothing is downloaded).
     *        Runs before update() returns — with Updated, before the caller restarts.
     * @param userData passed unchanged to every call
     */
    void setResultCallback(ResultCallback callback, void* userData = nullptr);

private:
    static const char* TAG;
    static const int   CHUNK_SIZE     = 4096;
    static const int   TIMEOUT_MIN_MS = 1000;
    static const int   URL_MAX        = 1280;  // fits all fields fully percent-encoded
    static const int   SLUG_SIZE      = 33;    // unit key / defer reason: 32 chars + '\0'

    // Firmware info from the X-OTA-* response headers
    struct FwInfo {
        int64_t size;        // X-OTA-Size, -1 = not sent
        char    sha256[65];  // X-OTA-SHA256 lowercase hex, "" = not sent / invalid
    };

    char        _serverURL[256];
    char        _group[64];
    char        _type[64];
    char        _component[64];
    char        _hwVersion[32];
    char        _deviceKey[18];
    char        _unitKey[SLUG_SIZE];
    char        _currentVersion[32];
    char        _lastServerVersion[32];
    char        _otaKey[128];
    const char* _rootCA;
    bool        _insecure;
    int         _timeoutMs;
    bool        _requireHash;
    Result      _lastResult;
    FwInfo      _offered;             // headers from step 1 (version check)

    Schedule _schedule;
    uint32_t _intervalSeconds;
    int      _scheduledHour;
    int      _scheduledMinute;
    int      _scheduledDay;
    time_t   _lastCheckTime;      // wall-clock display value + DAILY/WEEKLY scheduling
    int64_t  _lastCheckUptimeUs;  // monotonic esp_timer value used for INTERVAL scheduling; -1 = never checked

    void (*_progressCallback)(int progress);
    ProgressCallback _progressBytesCallback;
    StartCallback    _startCallback;
    ResultCallback   _resultCallback;
    void*            _progressBytesUserData;
    void*            _startUserData;
    void*            _resultUserData;

    void     normalizeMAC(const char* mac, char* out, size_t len);
    bool     buildURL(char* buf, size_t len, bool download, const char* deferReason = nullptr);
    void     setupClientConfig(esp_http_client_config_t& config, const char* url);
    static esp_err_t _httpEventHandler(esp_http_client_event_t* evt);
    static bool      _fitsPartition(int64_t size, const esp_partition_t* partition);
    esp_http_client_handle_t _openClient(const char* url, bool& wdtWasOn, FwInfo* info,
                                         int64_t* contentLength, int* status);
    bool     _fetchVersion(char* buf, size_t len, Result& error, const char* deferReason = nullptr);
    bool     _checkVersion(Result& result, const char* deferReason = nullptr);
    Result   _run(const char* deferReason);
    Result   _finish(Result result, bool started);
    bool     downloadString(const char* url, char* buf, size_t len, FwInfo* info, Result& error);
    Result   downloadFirmware(const char* url, const esp_partition_t* partition);
    void     markChecked();
    int64_t  getUptimeSeconds() const;
    uint32_t getServerInterval() const;
};

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
 *   Step 1 – GET /ota.php?group=&type=&component=&hw=&device=&fw=&uptime=&interval=
 *             → 200 plain text "26.5.1"   |  404 no firmware
 *   Step 2 – GET /ota.php?...&download=1
 *             → 200 binary firmware data
 *
 * Auth:        setOTAKey() — X-OTA-Key header (APP_SECRET on server, OTA_AUTH=true)
 * TLS:         embedded certificate bundle (default), custom CA, or no verification
 * WDT:         automatic handling, compatible with AP_TaskUtils tasks
 * Versioning:  compares any version difference → both upgrade and downgrade work
 * Threading:   instance is not thread-safe — call only from a single task
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

    /** @brief Network timeout in ms (default 15000) */
    void setTimeout(int ms);

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
     * @brief Download the firmware and write it to the OTA partition.
     *        Internally calls isUpdateAvailable(); if versions differ, downloads and installs.
     * @return true if the firmware was installed successfully
     *         — caller must call esp_restart() to apply it
     *
     * Requires a partition table with ota_0 + ota_1.
     * Recommended caller task stack size: >= 8 KB
     */
    bool performUpdate();

    /**
     * @brief Check for and optionally download an update; marks the last check time.
     *        Combines performUpdate() and markChecked() — main entry point for a periodic task.
     * @return true if the firmware was installed — caller must call esp_restart()
     *
     * Example:
     *   if (ota.isDue() && ota.check()) esp_restart();
     */
    bool check();

    // ── Callback ───────────────────────────────────────────────────────────────

    /** @brief Callback with firmware download progress (0–100) */
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
    time_t   _lastCheckTime;      // wall-clock display value + DAILY/WEEKLY scheduling
    int64_t  _lastCheckUptimeUs;  // monotonic esp_timer value used for INTERVAL scheduling; -1 = never checked

    void (*_progressCallback)(int progress);

    void     normalizeMAC(const char* mac, char* out, size_t len);
    void     buildURL(char* buf, size_t len, bool download);
    void     setupClientConfig(esp_http_client_config_t& config, const char* url);
    esp_http_client_handle_t _openClient(const char* url, bool& wdtWasOn, int64_t* contentLength);
    bool     downloadString(const char* url, char* buf, size_t len);
    bool     downloadFirmware(const char* url);
    void     markChecked();
    int64_t  getUptimeSeconds() const;
    uint32_t getServerInterval() const;
};

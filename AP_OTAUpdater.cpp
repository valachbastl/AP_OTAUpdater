#include "AP_OTAUpdater.h"
#include <string.h>
#include <stdlib.h>
#include <cctype>

const char* AP_OTAUpdater::TAG = "AP_OTAUpdater";

// =============================================================================
// Constructor
// =============================================================================

AP_OTAUpdater::AP_OTAUpdater(const char* serverURL,
                             const char* group,
                             const char* type,
                             const char* component,
                             const char* hwVersion,
                             const char* deviceMAC)
    : _rootCA(nullptr)
    , _insecure(false)
    , _timeoutMs(15000)
    , _schedule(MANUAL)
    , _intervalSeconds(3600)
    , _scheduledHour(12)
    , _scheduledMinute(0)
    , _scheduledDay(1)
    , _lastCheckTime(0)
    , _progressCallback(nullptr)
{
    strncpy(_serverURL, serverURL, sizeof(_serverURL) - 1);
    _serverURL[sizeof(_serverURL) - 1] = '\0';
    int len = strlen(_serverURL);
    while (len > 0 && _serverURL[len - 1] == '/') _serverURL[--len] = '\0';

    strncpy(_group,     group,     sizeof(_group)     - 1); _group[sizeof(_group) - 1]         = '\0';
    strncpy(_type,      type,      sizeof(_type)      - 1); _type[sizeof(_type) - 1]            = '\0';
    strncpy(_component, component, sizeof(_component) - 1); _component[sizeof(_component) - 1] = '\0';
    strncpy(_hwVersion, hwVersion, sizeof(_hwVersion) - 1); _hwVersion[sizeof(_hwVersion) - 1] = '\0';

    normalizeMAC(deviceMAC, _deviceKey, sizeof(_deviceKey));

    strncpy(_currentVersion, "0.0.0", sizeof(_currentVersion) - 1);
    _lastServerVersion[0] = '\0';
    _otaKey[0]            = '\0';
}

// =============================================================================
// Setters
// =============================================================================

void AP_OTAUpdater::setCurrentVersion(const char* version)
{
    strncpy(_currentVersion, version, sizeof(_currentVersion) - 1);
    _currentVersion[sizeof(_currentVersion) - 1] = '\0';
}

const char* AP_OTAUpdater::getCurrentVersion()    const { return _currentVersion; }
const char* AP_OTAUpdater::getLastServerVersion() const { return _lastServerVersion; }
time_t      AP_OTAUpdater::getLastCheckTime()      const { return _lastCheckTime; }

void AP_OTAUpdater::setOTAKey(const char* key)
{
    strncpy(_otaKey, key, sizeof(_otaKey) - 1);
    _otaKey[sizeof(_otaKey) - 1] = '\0';
}

void AP_OTAUpdater::setCACert(const char* rootCA)
{
    _rootCA   = rootCA;
    _insecure = false;
}

void AP_OTAUpdater::setInsecure(bool insecure)
{
    _insecure = insecure;
    if (insecure) _rootCA = nullptr;
}

void AP_OTAUpdater::setTimeout(int ms) { _timeoutMs = ms; }

void AP_OTAUpdater::setProgressCallback(void (*callback)(int progress))
{
    _progressCallback = callback;
}

// =============================================================================
// Scheduler
// =============================================================================

void AP_OTAUpdater::setScheduleInterval(uint32_t seconds)
{
    _schedule        = INTERVAL;
    _intervalSeconds = (seconds < 30) ? 30 : seconds;
}

void AP_OTAUpdater::setScheduleDaily(int hour, int minute)
{
    _schedule        = DAILY;
    _scheduledHour   = (hour   < 0) ? 0 : ((hour   > 23) ? 23 : hour);
    _scheduledMinute = (minute < 0) ? 0 : ((minute > 59) ? 59 : minute);
}

void AP_OTAUpdater::setScheduleWeekly(int dayOfWeek, int hour, int minute)
{
    _schedule        = WEEKLY;
    _scheduledDay    = (dayOfWeek < 0) ? 0 : ((dayOfWeek > 6) ? 6 : dayOfWeek);
    _scheduledHour   = (hour      < 0) ? 0 : ((hour      > 23) ? 23 : hour);
    _scheduledMinute = (minute    < 0) ? 0 : ((minute    > 59) ? 59 : minute);
}

bool AP_OTAUpdater::isDue() const
{
    switch (_schedule) {

        case MANUAL:
            return false;

        case INTERVAL:
            if (_lastCheckTime == 0) return true;
            return (time(nullptr) - _lastCheckTime) >= (time_t)_intervalSeconds;

        case DAILY: {
            time_t now = time(nullptr);
            if (now < 1000000000L) return false; // čas nesynchronizován

            struct tm tmNow;
            localtime_r(&now, &tmNow);

            struct tm tmSched   = tmNow;
            tmSched.tm_hour     = _scheduledHour;
            tmSched.tm_min      = _scheduledMinute;
            tmSched.tm_sec      = 0;
            time_t scheduledNow = mktime(&tmSched);

            // true pokud aktuální čas je po naplánovaném a od toho okna ještě nebyla kontrola
            return (now >= scheduledNow) && (_lastCheckTime < scheduledNow);
        }

        case WEEKLY: {
            time_t now = time(nullptr);
            if (now < 1000000000L) return false; // čas nesynchronizován

            struct tm tmNow;
            localtime_r(&now, &tmNow);

            if (tmNow.tm_wday != _scheduledDay) return false;

            struct tm tmSched   = tmNow;
            tmSched.tm_hour     = _scheduledHour;
            tmSched.tm_min      = _scheduledMinute;
            tmSched.tm_sec      = 0;
            time_t scheduledNow = mktime(&tmSched);

            return (now >= scheduledNow) && (_lastCheckTime < scheduledNow);
        }
    }
    return false;
}

void AP_OTAUpdater::markChecked()
{
    _lastCheckTime = time(nullptr);
}

// =============================================================================
// Internal helpers
// =============================================================================

void AP_OTAUpdater::normalizeMAC(const char* mac, char* out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; mac[i] && j < len - 1; i++) {
        char c = mac[i];
        if ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')) {
            out[j++] = (char)tolower((unsigned char)c);
        }
    }
    out[j] = '\0';
}

int64_t AP_OTAUpdater::getUptimeSeconds() const
{
    return (int64_t)(esp_timer_get_time() / 1000000ULL);
}

uint32_t AP_OTAUpdater::getServerInterval() const
{
    switch (_schedule) {
        case INTERVAL: return _intervalSeconds;
        case DAILY:    return 86400;
        case WEEKLY:   return 604800;
        default:       return 86400;
    }
}

void AP_OTAUpdater::buildURL(char* buf, size_t len, bool download)
{
    snprintf(buf, len,
             "%s/ota.php?group=%s&type=%s&component=%s&hw=%s"
             "&device=%s&fw=%s&uptime=%lld&interval=%lu%s",
             _serverURL,
             _group, _type, _component, _hwVersion,
             _deviceKey, _currentVersion,
             (long long)getUptimeSeconds(),
             (unsigned long)getServerInterval(),
             download ? "&download=1" : "");
}

void AP_OTAUpdater::setupClientConfig(esp_http_client_config_t& config, const char* url)
{
    config.url        = url;
    config.timeout_ms = _timeoutMs;

    bool isHttps = (strncmp(url, "https://", 8) == 0);

    if (_rootCA) {
        config.cert_pem = _rootCA;
    } else if (isHttps && _insecure) {
#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
        config.skip_cert_common_name_check = true;
        // bez cert_pem a crt_bundle_attach → TLS bez ověření certifikátu
#else
        ESP_LOGW(TAG, "setInsecure() requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y in sdkconfig");
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    } else if (isHttps) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
}

// =============================================================================
// HTTP helpers
// =============================================================================

// Otevre HTTP klienta: init + X-OTA-Key + odhlaseni z WDT + open + fetch headers
// + overeni statusu 200. Pri uspechu vraci otevreneho clienta (caller ho musi
// zavrit + cleanupnout a podle wdtWasOn znovu prihlasit do WDT). Pri chybe uklidi
// a vrati nullptr. Sdileno downloadString() a downloadFirmware().
esp_http_client_handle_t AP_OTAUpdater::_openClient(const char* url, bool& wdtWasOn, int64_t* contentLength)
{
    wdtWasOn = false;

    esp_http_client_config_t config = {};
    setupClientConfig(config, url);

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return nullptr;
    }

    if (_otaKey[0]) {
        esp_http_client_set_header(client, "X-OTA-Key", _otaKey);
    }

    // Odhlásit z WDT — připojení může blokovat až do timeoutu
    wdtWasOn = (esp_task_wdt_delete(NULL) == ESP_OK);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return nullptr;
    }

    int64_t cl = esp_http_client_fetch_headers(client);
    if (contentLength) *contentLength = cl;

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status: %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return nullptr;
    }

    return client;
}

bool AP_OTAUpdater::downloadString(const char* url, char* buf, size_t len)
{
    bool wdtWasOn;
    esp_http_client_handle_t client = _openClient(url, wdtWasOn, nullptr);
    if (!client) return false;

    int readLen = esp_http_client_read(client, buf, (int)len - 1);
    if (readLen < 0) readLen = 0;
    buf[readLen] = '\0';

    while (readLen > 0 &&
           (buf[readLen - 1] == '\n' || buf[readLen - 1] == '\r' || buf[readLen - 1] == ' ')) {
        buf[--readLen] = '\0';
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (wdtWasOn) esp_task_wdt_add(NULL);

    return readLen > 0;
}

bool AP_OTAUpdater::downloadFirmware(const char* url)
{
    bool    wdtWasOn;
    int64_t contentLength = 0;
    esp_http_client_handle_t client = _openClient(url, wdtWasOn, &contentLength);
    if (!client) return false;

    ESP_LOGI(TAG, "Firmware size: %lld bytes", (long long)contentLength);

    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return false;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return false;
    }

    // Znovu přihlásit do WDT před zápisovou smyčkou — reset každý chunk
    if (wdtWasOn) esp_task_wdt_add(NULL);

    uint8_t* chunk = (uint8_t*)malloc(CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "No memory for chunk buffer");
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int64_t written = 0;
    bool    ok      = true;

    while (true) {
        int readLen = esp_http_client_read(client, (char*)chunk, CHUNK_SIZE);
        if (readLen < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            ok = false;
            break;
        }
        if (readLen == 0) break;

        err = esp_ota_write(ota_handle, chunk, readLen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        written += readLen;

        if (_progressCallback && contentLength > 0) {
            _progressCallback((int)((written * 100) / contentLength));
        }

        if (wdtWasOn) esp_task_wdt_reset();
    }

    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Written: %lld / %lld bytes", (long long)written, (long long)contentLength);

    if (!ok) {
        esp_ota_abort(ota_handle);
        return false;
    }

    if (contentLength > 0 && written != contentLength) {
        ESP_LOGE(TAG, "Size mismatch");
        esp_ota_abort(ota_handle);
        return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "OTA complete — call esp_restart() to apply");
    return true;
}

// =============================================================================
// Public OTA operations
// =============================================================================

bool AP_OTAUpdater::getServerVersion(char* buf, size_t len)
{
    char url[768];
    buildURL(url, sizeof(url), false);
    ESP_LOGI(TAG, "Checking: %s", url);
    return downloadString(url, buf, len);
}

bool AP_OTAUpdater::isUpdateAvailable()
{
    char serverVer[32];
    if (!getServerVersion(serverVer, sizeof(serverVer))) {
        return false;
    }

    strncpy(_lastServerVersion, serverVer, sizeof(_lastServerVersion) - 1);
    _lastServerVersion[sizeof(_lastServerVersion) - 1] = '\0';

    bool differs = strcmp(serverVer, _currentVersion) != 0;
    ESP_LOGI(TAG, "Version: local=%s server=%s → %s",
             _currentVersion, serverVer, differs ? "DIFFERS" : "up to date");
    return differs;
}

bool AP_OTAUpdater::performUpdate()
{
    if (!isUpdateAvailable()) {
        ESP_LOGI(TAG, "No update available");
        return false;
    }

    char url[768];
    buildURL(url, sizeof(url), true);
    ESP_LOGI(TAG, "Downloading: %s", url);

    return downloadFirmware(url);
}

bool AP_OTAUpdater::check()
{
    markChecked();
    return performUpdate();
}

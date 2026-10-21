#include "AP_OTAUpdater.h"
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <cctype>

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

// ESP-IDF 6 ships mbedTLS 4 (TF-PSA-Crypto) — the legacy mbedtls_sha256_* API is
// no longer public there, PSA is the way. ESP-IDF 5.x keeps the legacy API.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define AP_OTA_USE_PSA 1
#include "psa/crypto.h"
#else
#define AP_OTA_USE_PSA 0
#include "mbedtls/sha256.h"
#endif

const char* AP_OTAUpdater::TAG = "AP_OTAUpdater";

namespace {

// Bounded copy, nullptr → ""
void copyStr(char* dst, size_t size, const char* src)
{
    if (size == 0) return;
    if (!src) src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

// Appends to a fixed buffer; remembers truncation instead of cutting silently
class UrlBuilder
{
public:
    UrlBuilder(char* buf, size_t len) : _buf(buf), _len(len), _pos(0), _ok(len > 0), _hasQuery(false)
    {
        if (len > 0) buf[0] = '\0';
    }

    void append(const char* s)
    {
        while (*s) put(*s++);
    }

    // Query value, percent-encoded per RFC 3986 (unreserved chars kept)
    void appendEncoded(const char* s)
    {
        static const char hex[] = "0123456789ABCDEF";
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                put((char)c);
            } else {
                put('%');
                put(hex[c >> 4]);
                put(hex[c & 0x0F]);
            }
        }
    }

    void appendParam(const char* name, const char* value)
    {
        put(_hasQuery ? '&' : '?');
        _hasQuery = true;
        append(name);
        put('=');
        appendEncoded(value);
    }

    void appendParam(const char* name, long long value)
    {
        char num[24];
        snprintf(num, sizeof(num), "%lld", value);
        appendParam(name, num);
    }

    bool ok() const { return _ok; }

private:
    void put(char c)
    {
        if (_pos + 1 >= _len) { _ok = false; return; }
        _buf[_pos++] = c;
        _buf[_pos]   = '\0';
    }

    char*  _buf;
    size_t _len;
    size_t _pos;
    bool   _ok;
    bool   _hasQuery;
};

// Incremental SHA-256 over the downloaded image
class Sha256
{
public:
    Sha256()
    {
#if AP_OTA_USE_PSA
        _op = PSA_HASH_OPERATION_INIT;
        _ok = (psa_crypto_init() == PSA_SUCCESS) &&
              (psa_hash_setup(&_op, PSA_ALG_SHA_256) == PSA_SUCCESS);
#else
        mbedtls_sha256_init(&_ctx);
        _ok = (mbedtls_sha256_starts(&_ctx, 0) == 0);
#endif
    }

    ~Sha256()
    {
#if AP_OTA_USE_PSA
        psa_hash_abort(&_op);
#else
        mbedtls_sha256_free(&_ctx);
#endif
    }

    Sha256(const Sha256&)            = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const uint8_t* data, size_t len)
    {
        if (!_ok) return;
#if AP_OTA_USE_PSA
        _ok = (psa_hash_update(&_op, data, len) == PSA_SUCCESS);
#else
        _ok = (mbedtls_sha256_update(&_ctx, data, len) == 0);
#endif
    }

    // Writes the digest as 64 lowercase hex chars + '\0'. false on any crypto error.
    bool finishHex(char out[65])
    {
        if (!_ok) return false;
        uint8_t hash[32];
#if AP_OTA_USE_PSA
        size_t hashLen = 0;
        if (psa_hash_finish(&_op, hash, sizeof(hash), &hashLen) != PSA_SUCCESS ||
            hashLen != sizeof(hash)) return false;
#else
        if (mbedtls_sha256_finish(&_ctx, hash) != 0) return false;
#endif
        static const char hex[] = "0123456789abcdef";
        for (int i = 0; i < 32; i++) {
            out[i * 2]     = hex[hash[i] >> 4];
            out[i * 2 + 1] = hex[hash[i] & 0x0F];
        }
        out[64] = '\0';
        return true;
    }

private:
    bool _ok;
#if AP_OTA_USE_PSA
    psa_hash_operation_t _op;
#else
    mbedtls_sha256_context _ctx;
#endif
};

// Holds CPU frequency at max for its lifetime. Works around an esp_pm deadlock
// (IDF bug) when DFS switches frequency under a dense interrupt load — one core
// stalls in the IPC MSPI sync while the other spins on the switch lock with
// interrupts masked → interrupt WDT. Seen on ESP32-P4 + SDIO (esp_hosted).
// No-op without CONFIG_PM_ENABLE.
class PmLockGuard
{
public:
    PmLockGuard()
    {
#if CONFIG_PM_ENABLE
        if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ap_ota", &_lock) == ESP_OK) {
            esp_pm_lock_acquire(_lock);
        } else {
            _lock = nullptr;
        }
#endif
    }

    ~PmLockGuard()
    {
#if CONFIG_PM_ENABLE
        if (_lock) {
            esp_pm_lock_release(_lock);
            esp_pm_lock_delete(_lock);
        }
#endif
    }

    PmLockGuard(const PmLockGuard&)            = delete;
    PmLockGuard& operator=(const PmLockGuard&) = delete;

private:
#if CONFIG_PM_ENABLE
    esp_pm_lock_handle_t _lock = nullptr;
#endif
};

} // namespace

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
    , _requireHash(false)
    , _lastResult(Result::None)
    , _schedule(MANUAL)
    , _intervalSeconds(3600)
    , _scheduledHour(12)
    , _scheduledMinute(0)
    , _scheduledDay(1)
    , _lastCheckTime(0)
    , _lastCheckUptimeUs(-1)
    , _progressCallback(nullptr)
    , _progressBytesCallback(nullptr)
    , _startCallback(nullptr)
    , _resultCallback(nullptr)
    , _progressBytesUserData(nullptr)
    , _startUserData(nullptr)
    , _resultUserData(nullptr)
{
    copyStr(_serverURL, sizeof(_serverURL), serverURL);
    size_t len = strlen(_serverURL);
    while (len > 0 && _serverURL[len - 1] == '/') _serverURL[--len] = '\0';

    copyStr(_group,     sizeof(_group),     group);
    copyStr(_type,      sizeof(_type),      type);
    copyStr(_component, sizeof(_component), component);
    copyStr(_hwVersion, sizeof(_hwVersion), hwVersion);

    normalizeMAC(deviceMAC, _deviceKey, sizeof(_deviceKey));

    copyStr(_currentVersion, sizeof(_currentVersion), "0.0.0");
    _lastServerVersion[0] = '\0';
    _otaKey[0]            = '\0';
    _unitKey[0]           = '\0';
    _offered.size         = -1;
    _offered.sha256[0]    = '\0';
}

// =============================================================================
// Setters
// =============================================================================

void AP_OTAUpdater::setCurrentVersion(const char* version)
{
    copyStr(_currentVersion, sizeof(_currentVersion), version);
}

const char* AP_OTAUpdater::getCurrentVersion()    const { return _currentVersion; }
const char* AP_OTAUpdater::getLastServerVersion() const { return _lastServerVersion; }
int64_t     AP_OTAUpdater::getLastServerSize()    const { return _offered.size; }
time_t      AP_OTAUpdater::getLastCheckTime()      const { return _lastCheckTime; }
const char* AP_OTAUpdater::getUnit()              const { return _unitKey; }

AP_OTAUpdater::Result AP_OTAUpdater::getLastResult() const { return _lastResult; }

void AP_OTAUpdater::setOTAKey(const char* key)
{
    copyStr(_otaKey, sizeof(_otaKey), key);
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

void AP_OTAUpdater::requireHash(bool require) { _requireHash = require; }

bool AP_OTAUpdater::setUnit(const char* key)
{
    _unitKey[0] = '\0';
    if (!key || !key[0]) return true;

    // Same normalization as the server: lowercase, ':' dropped, [a-z0-9_-]{4,32}
    char   tmp[sizeof(_unitKey)];
    size_t j  = 0;
    bool   ok = true;
    for (size_t i = 0; key[i]; i++) {
        char c = (char)tolower((unsigned char)key[i]);
        if (c == ':') continue;
        bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid || j >= sizeof(tmp) - 1) { ok = false; break; }
        tmp[j++] = c;
    }
    if (j < 4) ok = false;

    if (!ok) {
        ESP_LOGW(TAG, "Invalid unit key '%s' — must be [a-z0-9_-], 4-32 chars; not sent", key);
        return false;
    }

    tmp[j] = '\0';
    memcpy(_unitKey, tmp, j + 1);
    return true;
}

void AP_OTAUpdater::setUnitFromMAC(const uint8_t mac[6])
{
    if (!mac) { setUnit(nullptr); return; }
    char key[13];
    snprintf(key, sizeof(key), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    setUnit(key);
}

void AP_OTAUpdater::setProgressCallback(void (*callback)(int progress))
{
    _progressCallback = callback;
}

void AP_OTAUpdater::setProgressBytesCallback(ProgressCallback callback, void* userData)
{
    _progressBytesCallback = callback;
    _progressBytesUserData = userData;
}

void AP_OTAUpdater::setStartCallback(StartCallback callback, void* userData)
{
    _startCallback = callback;
    _startUserData = userData;
}

void AP_OTAUpdater::setResultCallback(ResultCallback callback, void* userData)
{
    _resultCallback = callback;
    _resultUserData = userData;
}

const char* AP_OTAUpdater::resultToString(Result result)
{
    switch (result) {
        case Result::None:         return "None";
        case Result::UpToDate:     return "UpToDate";
        case Result::Updated:      return "Updated";
        case Result::NoFirmware:   return "NoFirmware";
        case Result::ServerError:  return "ServerError";
        case Result::NetworkError: return "NetworkError";
        case Result::NoPartition:  return "NoPartition";
        case Result::TooLarge:     return "TooLarge";
        case Result::NoMemory:     return "NoMemory";
        case Result::WriteError:   return "WriteError";
        case Result::SizeMismatch: return "SizeMismatch";
        case Result::HashMissing:  return "HashMissing";
        case Result::HashMismatch: return "HashMismatch";
    }
    return "Unknown";
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
            // Monotonic esp_timer, not time() — immune to RTC/NTP drift or an
            // unsynchronized clock, which would otherwise make this delta unreliable.
            if (_lastCheckUptimeUs < 0) return true;
            return (esp_timer_get_time() - _lastCheckUptimeUs) >= (int64_t)_intervalSeconds * 1000000LL;

        case DAILY: {
            time_t now = time(nullptr);
            if (now < 1000000000L) return false; // time not synchronized

            struct tm tmNow;
            localtime_r(&now, &tmNow);

            struct tm tmSched   = tmNow;
            tmSched.tm_hour     = _scheduledHour;
            tmSched.tm_min      = _scheduledMinute;
            tmSched.tm_sec      = 0;
            time_t scheduledNow = mktime(&tmSched);

            // true if current time is past the scheduled time and no check has happened since
            return (now >= scheduledNow) && (_lastCheckTime < scheduledNow);
        }

        case WEEKLY: {
            time_t now = time(nullptr);
            if (now < 1000000000L) return false; // time not synchronized

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
    _lastCheckTime     = time(nullptr);        // wall-clock display value + DAILY/WEEKLY
    _lastCheckUptimeUs = esp_timer_get_time();  // monotonic — drives INTERVAL scheduling
}

// =============================================================================
// Internal helpers
// =============================================================================

void AP_OTAUpdater::normalizeMAC(const char* mac, char* out, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; mac && mac[i] && j < len - 1; i++) {
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

bool AP_OTAUpdater::buildURL(char* buf, size_t len, bool download)
{
    UrlBuilder url(buf, len);
    url.append(_serverURL);
    url.append("/ota.php");
    url.appendParam("group",     _group);
    url.appendParam("type",      _type);
    url.appendParam("component", _component);
    url.appendParam("hw",        _hwVersion);
    url.appendParam("device",    _deviceKey);
    url.appendParam("fw",        _currentVersion);
    url.appendParam("uptime",    (long long)getUptimeSeconds());
    url.appendParam("interval",  (long long)getServerInterval());
    // &unit= only in step 1 — the server evaluates it there only
    if (!download && _unitKey[0]) url.appendParam("unit", _unitKey);
    if (download) url.appendParam("download", "1");

    if (!url.ok()) {
        ESP_LOGE(TAG, "Request URL too long (max %u chars)", (unsigned)(len - 1));
        return false;
    }
    return true;
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
        // no cert_pem or crt_bundle_attach → TLS without certificate verification
#else
        ESP_LOGW(TAG, "setInsecure() requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y in sdkconfig");
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    } else if (isHttps) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
}

// Collects X-OTA-Size / X-OTA-SHA256 from the response headers into the FwInfo
// passed as user_data. esp_http_client_get_header() returns *request* headers,
// so response headers can only be read here.
esp_err_t AP_OTAUpdater::_httpEventHandler(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data ||
        !evt->header_key || !evt->header_value) {
        return ESP_OK;
    }

    FwInfo*     info  = (FwInfo*)evt->user_data;
    const char* value = evt->header_value;
    while (*value == ' ' || *value == '\t') value++;

    if (strcasecmp(evt->header_key, "X-OTA-Size") == 0) {
        char*     end = nullptr;
        long long v   = strtoll(value, &end, 10);
        if (end != value && v > 0) info->size = v;

    } else if (strcasecmp(evt->header_key, "X-OTA-SHA256") == 0) {
        size_t n = 0;
        bool   ok = true;
        for (; value[n] && value[n] != ' ' && value[n] != '\t' && value[n] != '\r'; n++) {
            if (n >= 64 || !isxdigit((unsigned char)value[n])) { ok = false; break; }
            info->sha256[n] = (char)tolower((unsigned char)value[n]);
        }
        if (ok && n == 64) {
            info->sha256[64] = '\0';
        } else {
            info->sha256[0] = '\0';
            ESP_LOGW(TAG, "Invalid X-OTA-SHA256 header ignored");
        }
    }
    return ESP_OK;
}

// Unknown size (<= 0) passes — esp_ota_write() then fails at the partition end
bool AP_OTAUpdater::_fitsPartition(int64_t size, const esp_partition_t* partition)
{
    if (size <= (int64_t)partition->size) return true;
    ESP_LOGE(TAG, "Firmware (%lld B) does not fit the OTA partition (%lu B)",
             (long long)size, (unsigned long)partition->size);
    return false;
}

// =============================================================================
// HTTP helpers
// =============================================================================

// Opens an HTTP client: init + X-OTA-Key + unregister from WDT + open + fetch headers
// + verify status 200. On success returns the open client (caller must close +
// clean it up, and re-register with WDT per wdtWasOn). On error cleans up and
// returns nullptr; *status = HTTP status, or 0 when no response was received.
// Response X-OTA-* headers are collected into *info (optional).
// Shared by downloadString() and downloadFirmware().
esp_http_client_handle_t AP_OTAUpdater::_openClient(const char* url, bool& wdtWasOn, FwInfo* info,
                                                    int64_t* contentLength, int* status)
{
    wdtWasOn = false;
    if (status) *status = 0;
    if (info) {
        info->size      = -1;
        info->sha256[0] = '\0';
    }

    esp_http_client_config_t config = {};
    setupClientConfig(config, url);
    config.event_handler = _httpEventHandler;
    config.user_data     = info;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return nullptr;
    }

    if (_otaKey[0]) {
        esp_http_client_set_header(client, "X-OTA-Key", _otaKey);
    }

    // Unregister from WDT — the connection may block up to the timeout
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

    int code = esp_http_client_get_status_code(client);
    if (status) *status = (cl < 0) ? 0 : code;
    if (cl < 0 || code != 200) {
        if (cl < 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed");
        } else if (code == 404) {
            ESP_LOGI(TAG, "HTTP status: 404");
        } else {
            ESP_LOGW(TAG, "HTTP status: %d", code);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return nullptr;
    }

    return client;
}

bool AP_OTAUpdater::downloadString(const char* url, char* buf, size_t len, FwInfo* info, Result& error)
{
    if (len > 0) buf[0] = '\0';

    bool wdtWasOn;
    int  status;
    esp_http_client_handle_t client = _openClient(url, wdtWasOn, info, nullptr, &status);
    if (!client) {
        error = (status == 0)   ? Result::NetworkError
              : (status == 404) ? Result::NoFirmware
                                : Result::ServerError;
        return false;
    }

    // Loop until EOF/buffer full rather than a single read — a response can
    // arrive across multiple TCP segments, and esp_http_client_read() is not
    // guaranteed to return the whole body in one call.
    size_t total = 0;
    bool   ok    = true;

    while (len > 1 && total < len - 1) {
        int readLen = esp_http_client_read(client, buf + total, (int)(len - 1 - total));
        if (readLen < 0) { ok = false; break; }
        if (readLen == 0) break; // EOF
        total += (size_t)readLen;
    }
    if (len > 0) buf[total] = '\0';

    while (total > 0 &&
           (buf[total - 1] == '\n' || buf[total - 1] == '\r' || buf[total - 1] == ' ')) {
        buf[--total] = '\0';
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (wdtWasOn) esp_task_wdt_add(NULL);

    if (!ok) {
        ESP_LOGE(TAG, "HTTP read error");
        error = Result::NetworkError;
        return false;
    }
    if (total == 0) {
        ESP_LOGW(TAG, "Empty response from server");
        error = Result::ServerError;
        return false;
    }
    return true;
}

AP_OTAUpdater::Result AP_OTAUpdater::downloadFirmware(const char* url, const esp_partition_t* partition)
{
    // Max CPU frequency for the whole transfer (released on every return path)
    PmLockGuard pmLock;

    bool    wdtWasOn;
    FwInfo  info;
    int64_t contentLength = 0;
    int     status;
    esp_http_client_handle_t client = _openClient(url, wdtWasOn, &info, &contentLength, &status);
    if (!client) {
        if (status == 0) return Result::NetworkError;
        // e.g. admin froze the device / changed policy between step 1 and step 2
        if (status == 404) ESP_LOGW(TAG, "Firmware no longer offered by the server");
        return Result::ServerError;
    }

    auto closeClient = [&]() {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    };

    // Size: X-OTA-Size → Content-Length → unknown. Content-Length is often missing
    // (hosting gzip + Cloudflare → chunked), X-OTA-Size always arrives (server >= 1.2.0).
    int64_t expectedSize = -1;
    if (info.size > 0) {
        expectedSize = info.size;
        if (contentLength > 0 && contentLength != info.size) {
            ESP_LOGW(TAG, "Content-Length %lld differs from X-OTA-Size %lld — using X-OTA-Size",
                     (long long)contentLength, (long long)info.size);
        }
        ESP_LOGI(TAG, "Firmware size: %lld bytes (X-OTA-Size)", (long long)expectedSize);
    } else if (contentLength > 0) {
        expectedSize = contentLength;
        ESP_LOGI(TAG, "Firmware size: %lld bytes (Content-Length)", (long long)expectedSize);
    } else {
        ESP_LOGW(TAG, "Firmware size unknown — size check skipped");
    }

    if (info.sha256[0]) {
        ESP_LOGI(TAG, "Expected SHA-256: %s", info.sha256);
    } else if (_requireHash) {
        ESP_LOGE(TAG, "Server sent no X-OTA-SHA256 and requireHash() is set — update refused");
        closeClient();
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return Result::HashMissing;
    } else {
        ESP_LOGW(TAG, "Server sent no X-OTA-SHA256 (server < 1.2.0?) — hash check skipped");
    }

    if (!_fitsPartition(expectedSize, partition)) {
        closeClient();
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return Result::TooLarge;
    }

    // Always erase sector by sector while writing, even when the size is known —
    // erasing the whole image up front blocks flash for seconds in one go
    // (~2 s per 1 MB) and was observed to stall other tasks of the application
    // (UART link timeouts during OTA).
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        closeClient();
        if (wdtWasOn) esp_task_wdt_add(NULL);
        return Result::WriteError;
    }

    // Re-register with WDT before the write loop — reset every chunk
    if (wdtWasOn) esp_task_wdt_add(NULL);

    uint8_t* chunk = (uint8_t*)malloc(CHUNK_SIZE);
    if (!chunk) {
        ESP_LOGE(TAG, "No memory for chunk buffer");
        esp_ota_abort(ota_handle);
        closeClient();
        return Result::NoMemory;
    }

    Sha256  sha;
    int64_t written     = 0;
    int     lastPercent = -1;
    bool    failed      = false;
    Result  result      = Result::Updated;

    while (true) {
        int readLen = esp_http_client_read(client, (char*)chunk, CHUNK_SIZE);
        if (readLen < 0) {
            ESP_LOGE(TAG, "HTTP read error");
            failed = true;
            result = Result::NetworkError;
            break;
        }
        if (readLen == 0) break;

        if (expectedSize > 0 && written + readLen > expectedSize) {
            ESP_LOGE(TAG, "Server sent more data than expected (%lld B)", (long long)expectedSize);
            failed = true;
            result = Result::SizeMismatch;
            break;
        }

        sha.update(chunk, readLen);

        err = esp_ota_write(ota_handle, chunk, readLen);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            failed = true;
            result = Result::WriteError;
            break;
        }
        written += readLen;

        if (_progressBytesCallback) {
            _progressBytesCallback(written, expectedSize, _progressBytesUserData);
        }
        if (_progressCallback && expectedSize > 0) {
            int percent = (int)((written * 100) / expectedSize);
            if (percent != lastPercent) {
                lastPercent = percent;
                _progressCallback(percent);
            }
        }

        if (wdtWasOn) esp_task_wdt_reset();
    }

    free(chunk);

    // Chunked / Content-Length stream must have ended properly — a read timeout
    // can end the loop early with readLen == 0
    bool canCheckComplete = esp_http_client_is_chunked_response(client) || contentLength > 0;
    bool complete         = !canCheckComplete || esp_http_client_is_complete_data_received(client);
    closeClient();

    ESP_LOGI(TAG, "Written: %lld / %lld bytes", (long long)written, (long long)expectedSize);

    if (!failed && !complete) {
        ESP_LOGE(TAG, "Download incomplete — connection ended early");
        failed = true;
        result = Result::SizeMismatch;
    }
    if (!failed && expectedSize > 0 && written != expectedSize) {
        ESP_LOGE(TAG, "Size mismatch");
        failed = true;
        result = Result::SizeMismatch;
    }

    // Verify the exact uploaded file before the image is finalized
    if (!failed && info.sha256[0]) {
        char actual[65];
        if (!sha.finishHex(actual)) {
            ESP_LOGE(TAG, "SHA-256 computation failed");
            failed = true;
            result = Result::HashMismatch;
        } else if (strcmp(actual, info.sha256) != 0) {
            ESP_LOGE(TAG, "SHA-256 mismatch: expected %s, got %s", info.sha256, actual);
            failed = true;
            result = Result::HashMismatch;
        } else {
            ESP_LOGI(TAG, "SHA-256 OK");
        }
    }

    if (failed) {
        esp_ota_abort(ota_handle);
        return result;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
        return Result::WriteError;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
        return Result::WriteError;
    }

    ESP_LOGI(TAG, "OTA complete — call esp_restart() to apply");
    return Result::Updated;
}

// =============================================================================
// Public OTA operations
// =============================================================================

bool AP_OTAUpdater::_fetchVersion(char* buf, size_t len, Result& error)
{
    char url[URL_MAX];
    if (!buildURL(url, sizeof(url), false)) {
        error = Result::ServerError;
        return false;
    }
    ESP_LOGI(TAG, "Checking: %s", url);

    if (!downloadString(url, buf, len, &_offered, error)) {
        if (error == Result::NoFirmware) {
            ESP_LOGI(TAG, "No firmware on the server for %s/%s/%s/%s", _group, _type, _component, _hwVersion);
        }
        return false;
    }

    copyStr(_lastServerVersion, sizeof(_lastServerVersion), buf);
    return true;
}

// true = server offers a different version; false → result = UpToDate or the error
bool AP_OTAUpdater::_checkVersion(Result& result)
{
    char serverVer[32];
    if (!_fetchVersion(serverVer, sizeof(serverVer), result)) return false;

    // Any difference counts — the server decides what to offer (a blocked
    // update makes it return the device's own version)
    bool differs = strcmp(serverVer, _currentVersion) != 0;
    ESP_LOGI(TAG, "Version: local=%s server=%s → %s",
             _currentVersion, serverVer, differs ? "DIFFERS" : "up to date");
    if (!differs) result = Result::UpToDate;
    return differs;
}

AP_OTAUpdater::Result AP_OTAUpdater::_finish(Result result, bool started)
{
    _lastResult = result;
    if (started) {
        if (result == Result::Updated) {
            ESP_LOGI(TAG, "Update result: %s", resultToString(result));
        } else {
            ESP_LOGE(TAG, "Update result: %s", resultToString(result));
        }
        if (_resultCallback) _resultCallback(result, _resultUserData);
    }
    return result;
}

bool AP_OTAUpdater::getServerVersion(char* buf, size_t len)
{
    if (!buf || len == 0) return false;
    Result error;
    return _fetchVersion(buf, len, error);
}

bool AP_OTAUpdater::isUpdateAvailable()
{
    Result result;
    return _checkVersion(result);
}

AP_OTAUpdater::Result AP_OTAUpdater::update()
{
    Result result;
    if (!_checkVersion(result)) {
        if (result == Result::UpToDate) ESP_LOGI(TAG, "No update available");
        return _finish(result, false);
    }

    if (_startCallback) _startCallback(_lastServerVersion, _offered.size, _startUserData);

    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        return _finish(Result::NoPartition, true);
    }

    // Size from step 1 → refuse before downloading anything
    if (!_fitsPartition(_offered.size, partition)) {
        return _finish(Result::TooLarge, true);
    }

    char url[URL_MAX];
    if (!buildURL(url, sizeof(url), true)) {
        return _finish(Result::ServerError, true);
    }
    ESP_LOGI(TAG, "Downloading: %s", url);

    return _finish(downloadFirmware(url, partition), true);
}

bool AP_OTAUpdater::performUpdate()
{
    return update() == Result::Updated;
}

bool AP_OTAUpdater::check()
{
    markChecked();
    return performUpdate();
}

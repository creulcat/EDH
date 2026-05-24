#include "CurrentEvent.h"
#include "GameEvent.h"
#include "LocaleSettings.h"
#include "WebhookConfiguration.h"

#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_gameplay_events.h"
#include "scssdk_telemetry.h"
#include "scssdk_telemetry_event.h"
#include "amtrucks/scssdk_ats.h"
#include "amtrucks/scssdk_telemetry_ats.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cwctype>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <knownfolders.h>
#include <shlobj.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#endif
#else
#include <curl/curl.h>
#include <unistd.h>
#include <limits.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

static void logMessage(const char* const message);
static void logError(const char* const message);
static void loadConfiguration(const std::string& configPath);
static void executeWebhook(const WebhookConfiguration& configuration, const CurrentEvent& event);
static void dispatchWebhooksForEvent(GameEvent kind, const CurrentEvent& currentEvent);
static SCSAPI_VOID telemetry_configuration_event(const scs_event_t event, const void* const eventInfo, const scs_context_t context);
static SCSAPI_VOID telemetry_gameplay_event(const scs_event_t event, const void* const eventInfo, const scs_context_t context);

static scs_log_t defaultLog = nullptr;
static std::vector<WebhookConfiguration> g_webhookConfigs;
static LocaleSettings g_localeSettings;
static scs_telemetry_unregister_from_event_t g_unregisterFromEvent = nullptr;
static bool g_configurationEventRegistered = false;
static bool g_gameplayEventRegistered = false;

#ifndef _WIN32
static bool g_curlInitialized = false;

static bool initializeCurl() {
    if (g_curlInitialized) {
        return true;
    }
    const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        logError(("curl_global_init failed: " + std::string(curl_easy_strerror(code))).c_str());
        return false;
    }
    g_curlInitialized = true;
    return true;
}

static void shutdownCurl() {
    if (!g_curlInitialized) {
        return;
    }
    curl_global_cleanup();
    g_curlInitialized = false;
}
#endif

namespace {

struct PendingWebhookBlock {
    std::string name;
    std::string webhookURL;
    std::string jsonFile;
};

std::string trimWhitespace(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string stripOptionalQuotes(std::string s) {
    s = trimWhitespace(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

bool parseKeyValueLine(const std::string& line, std::string& key, std::string& value) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    key = trimWhitespace(line.substr(0, colon));
    value = stripOptionalQuotes(line.substr(colon + 1));
    return !key.empty();
}

struct GameEventDescriptor {
    GameEvent kind;
    const char* sdkId;
};

struct GameEventConfigAlias {
    GameEvent kind;
    const char* name;
};

constexpr GameEventDescriptor kGameEventDescriptors[] = {
    {JobCancelled, SCS_TELEMETRY_GAMEPLAY_EVENT_job_cancelled},
    {JobDelivered, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered},
    {PlayerFined, SCS_TELEMETRY_GAMEPLAY_EVENT_player_fined},
    {PlayerTollgatePaid, SCS_TELEMETRY_GAMEPLAY_EVENT_player_tollgate_paid},
    {PlayerUseFerry, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_ferry},
    {PlayerUseTrain, SCS_TELEMETRY_GAMEPLAY_EVENT_player_use_train},
};

constexpr GameEventConfigAlias kGameEventConfigAliases[] = {
    {JobCancelled, "JobCancelled"},
    {JobCancelled, "job.cancelled"},
    {JobCancelled, "Canceljob"},
    {JobDelivered, "JobDelivered"},
    {JobDelivered, "job.delivered"},
    {JobDelivered, "Finishjob"},
    {PlayerFined, "PlayerFined"},
    {PlayerFined, "player.fined"},
    {PlayerTollgatePaid, "PlayerTollgatePaid"},
    {PlayerTollgatePaid, "player.tollgate.paid"},
    {PlayerUseFerry, "PlayerUseFerry"},
    {PlayerUseFerry, "player.use.ferry"},
    {PlayerUseTrain, "PlayerUseTrain"},
    {PlayerUseTrain, "player.use.train"},
};

std::optional<GameEvent> gameEventFromConfigName(const std::string& name) {
    for (const auto& alias : kGameEventConfigAliases) {
        if (name == alias.name) {
            return alias.kind;
        }
    }
    return std::nullopt;
}

GameEvent gameEventFromSdkId(const char* id) {
    if (id == nullptr) {
        return Unknown;
    }
    for (const auto& descriptor : kGameEventDescriptors) {
        if (std::strcmp(id, descriptor.sdkId) == 0) {
            return descriptor.kind;
        }
    }
    return Unknown;
}

bool readEntireFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = f.tellg();
    f.seekg(0);
    if (size < 0) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0 && !f.read(out.data(), size)) {
        return false;
    }
    return true;
}

bool isBlockEmpty(const PendingWebhookBlock& b) {
    return b.name.empty() && b.webhookURL.empty() && b.jsonFile.empty();
}

LocaleSettings defaultLocaleSettings() {
    return LocaleSettings{};
}

bool applyLocalePreset(LocaleSettings& settings, const std::string& presetName) {
    if (presetName == "en") {
        settings = defaultLocaleSettings();
        return true;
    }
    if (presetName == "nl") {
        settings.notAvailable = "-n.v.t.-";
        settings.day = "dag";
        settings.days = "dagen";
        settings.hour = "uur";
        settings.hours = "uren";
        settings.minute = "minuut";
        settings.minutes = "minuten";
        return true;
    }
    if (presetName == "de") {
        settings.notAvailable = "-k.A.-";
        settings.day = "Tag";
        settings.days = "Tage";
        settings.hour = "Stunde";
        settings.hours = "Stunden";
        settings.minute = "Minute";
        settings.minutes = "Minuten";
        return true;
    }
    if (presetName == "fr") {
        settings.notAvailable = "-n/d-";
        settings.day = "jour";
        settings.days = "jours";
        settings.hour = "heure";
        settings.hours = "heures";
        settings.minute = "minute";
        settings.minutes = "minutes";
        return true;
    }
    return false;
}

struct PendingLocaleBlock {
    bool hasPreset = false;
    std::string presetName;
    LocaleSettings overrides;
    bool hasOverrides = false;
};

bool isLocaleBlockEmpty(const PendingLocaleBlock& block) {
    return !block.hasPreset && !block.hasOverrides;
}

void applyLocaleBlock(const PendingLocaleBlock& block) {
    g_localeSettings = defaultLocaleSettings();
    if (block.hasPreset) {
        if (!applyLocalePreset(g_localeSettings, block.presetName)) {
            logError(("Unknown locale preset \"" + block.presetName + "\", using English defaults.").c_str());
            g_localeSettings = defaultLocaleSettings();
        }
    }
    if (block.hasOverrides) {
        const auto& overrides = block.overrides;
        if (!overrides.notAvailable.empty()) {
            g_localeSettings.notAvailable = overrides.notAvailable;
        }
        if (!overrides.day.empty()) {
            g_localeSettings.day = overrides.day;
        }
        if (!overrides.days.empty()) {
            g_localeSettings.days = overrides.days;
        }
        if (!overrides.hour.empty()) {
            g_localeSettings.hour = overrides.hour;
        }
        if (!overrides.hours.empty()) {
            g_localeSettings.hours = overrides.hours;
        }
        if (!overrides.minute.empty()) {
            g_localeSettings.minute = overrides.minute;
        }
        if (!overrides.minutes.empty()) {
            g_localeSettings.minutes = overrides.minutes;
        }
    }
}

void flushLocaleBlock(PendingLocaleBlock& block) {
    if (isLocaleBlockEmpty(block)) {
        return;
    }

    applyLocaleBlock(block);

    std::string msg = "Loaded locale settings";
    if (block.hasPreset) {
        msg += " (preset=\"" + block.presetName + "\")";
    }
    logMessage(msg.c_str());

    block = {};
}

bool applyLocaleKey(PendingLocaleBlock& block, const std::string& key, const std::string& value) {
    if (key == "name") {
        block.hasPreset = true;
        block.presetName = value;
        return true;
    }

    struct LocaleKeyMapping {
        const char* key;
        std::string LocaleSettings::* field;
    };

    static const LocaleKeyMapping mappings[] = {
        {"notAvailable", &LocaleSettings::notAvailable},
        {"day", &LocaleSettings::day},
        {"days", &LocaleSettings::days},
        {"hour", &LocaleSettings::hour},
        {"hours", &LocaleSettings::hours},
        {"minute", &LocaleSettings::minute},
        {"minutes", &LocaleSettings::minutes},
    };

    for (const auto& mapping : mappings) {
        if (key == mapping.key) {
            block.hasOverrides = true;
            block.overrides.*mapping.field = value;
            return true;
        }
    }

    return false;
}

void flushWebhookBlock(PendingWebhookBlock& block, const std::filesystem::path& configDir) {
    if (isBlockEmpty(block)) {
        return;
    }

    const auto eventKind = gameEventFromConfigName(block.name);
    if (!eventKind.has_value()) {
        std::string msg = "Skipping webhook config: unknown event name \"" + block.name + "\"";
        logMessage(msg.c_str());
        block = {};
        return;
    }

    if (block.webhookURL.empty()) {
        std::string msg = "Skipping webhook config for " + block.name + ": empty webhookURL";
        logError(msg.c_str());
        block = {};
        return;
    }

    std::string jsonPayload;
    if (!block.jsonFile.empty()) {
        const auto jsonPath = configDir / block.jsonFile;
        if (!readEntireFile(jsonPath, jsonPayload)) {
            std::string msg = "Skipping webhook config for " + block.name + ": cannot read jsonFile " +
                jsonPath.string();
            logError(msg.c_str());
            block = {};
            return;
        }
    }

    WebhookConfiguration cfg;
    cfg.webhookURL = std::move(block.webhookURL);
    cfg.payloadJSON = std::move(jsonPayload);
    cfg.eventTrigger = *eventKind;
    g_webhookConfigs.push_back(std::move(cfg));

    block = {};
}

} // namespace

/*
* Log information message to the default game.log.txt
*/
static void logMessage(const char* const message) {
    std::string line = std::string("[Towhitch_plugin]: ") + message;
    if (defaultLog) {
        defaultLog(SCS_LOG_TYPE_message, line.c_str());
    }
}

/*
* Log error messages to the default game.log.txt
*/
static void logError(const char* const message) {
    std::string line = std::string("[Towhitch_plugin]: ") + message;
    if (defaultLog) {
        defaultLog(SCS_LOG_TYPE_error, line.c_str());
    }
}

/*
* Read the configurationfile and load the values into memory.
*/
static void loadConfiguration(const std::string& configPath) {
    g_webhookConfigs.clear();
    g_localeSettings = defaultLocaleSettings();

    std::ifstream input(configPath);
    if (!input) {
        std::string msg = "Cannot open configuration file: " + configPath;
        logError(msg.c_str());
        return;
    }

    const std::filesystem::path cfgPath(configPath);
    const std::filesystem::path configDir = cfgPath.has_parent_path() ? cfgPath.parent_path() : std::filesystem::path(".");

    PendingWebhookBlock pending;
    PendingLocaleBlock pendingLocale;
    bool inEvent = false;
    bool inLocale = false;
    std::string line;

    while (std::getline(input, line)) {
        const std::string trimmed = trimWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        if (trimmed == "event:") {
            flushLocaleBlock(pendingLocale);
            flushWebhookBlock(pending, configDir);
            inEvent = true;
            inLocale = false;
            continue;
        }

        if (trimmed == "locale:") {
            flushWebhookBlock(pending, configDir);
            flushLocaleBlock(pendingLocale);
            inEvent = false;
            inLocale = true;
            continue;
        }

        std::string key;
        std::string value;
        if (!parseKeyValueLine(trimmed, key, value)) {
            continue;
        }

        if (inLocale) {
            if (!applyLocaleKey(pendingLocale, key, value)) {
                logError(("Skipping unknown locale key \"" + key + "\".").c_str());
            }
            continue;
        }

        if (!inEvent) {
            continue;
        }

        if (key == "name") {
            pending.name = value;
        } else if (key == "webhookURL") {
            pending.webhookURL = value;
        } else if (key == "jsonFile") {
            pending.jsonFile = value;
        }
    }

    flushLocaleBlock(pendingLocale);
    flushWebhookBlock(pending, configDir);

    std::string summary = "Loaded " + std::to_string(g_webhookConfigs.size()) + " webhook configuration(s).";
    logMessage(summary.c_str());
    if (g_webhookConfigs.empty()) {
        logError("WARNING: No webhook configurations were loaded; the plugin will not send any webhooks.");
    }
}

namespace {

struct TelemetryCache {
    std::string truck;
    std::string cargo;
    std::string startingLocation;
    std::string destination;
};

TelemetryCache g_telemetryCache;
std::filesystem::path g_gameHome;
bool g_isAts = false;

std::string valueToString(const scs_value_t& v) {
    switch (v.type) {
    case SCS_VALUE_TYPE_bool:
        return v.value_bool.value ? std::string("true") : std::string("false");
    case SCS_VALUE_TYPE_s32:
        return std::to_string(v.value_s32.value);
    case SCS_VALUE_TYPE_u32:
        return std::to_string(v.value_u32.value);
    case SCS_VALUE_TYPE_u64:
        return std::to_string(v.value_u64.value);
    case SCS_VALUE_TYPE_s64:
        return std::to_string(v.value_s64.value);
    case SCS_VALUE_TYPE_float:
        return std::to_string(v.value_float.value);
    case SCS_VALUE_TYPE_double:
        return std::to_string(v.value_double.value);
    case SCS_VALUE_TYPE_string:
        return v.value_string.value != nullptr ? std::string(v.value_string.value) : std::string();
    default:
        return {};
    }
}

std::optional<double> valueToDouble(const scs_value_t& v) {
    switch (v.type) {
    case SCS_VALUE_TYPE_float:
        return v.value_float.value;
    case SCS_VALUE_TYPE_double:
        return v.value_double.value;
    case SCS_VALUE_TYPE_s32:
        return static_cast<double>(v.value_s32.value);
    case SCS_VALUE_TYPE_u32:
        return static_cast<double>(v.value_u32.value);
    case SCS_VALUE_TYPE_s64:
        return static_cast<double>(v.value_s64.value);
    case SCS_VALUE_TYPE_u64:
        return static_cast<double>(v.value_u64.value);
    default:
        return std::nullopt;
    }
}

std::optional<int64_t> valueToInt64(const scs_value_t& v) {
    switch (v.type) {
    case SCS_VALUE_TYPE_s64:
        return v.value_s64.value;
    case SCS_VALUE_TYPE_s32:
        return v.value_s32.value;
    case SCS_VALUE_TYPE_u64:
        return static_cast<int64_t>(v.value_u64.value);
    case SCS_VALUE_TYPE_u32:
        return v.value_u32.value;
    case SCS_VALUE_TYPE_float:
        return static_cast<int64_t>(std::llround(v.value_float.value));
    case SCS_VALUE_TYPE_double:
        return static_cast<int64_t>(std::llround(v.value_double.value));
    default:
        return std::nullopt;
    }
}

std::optional<std::string> readUsetConfigValue(const std::filesystem::path& configPath, const std::string& key) {
    std::ifstream input(configPath);
    if (!input) {
        return std::nullopt;
    }

    const std::string needle = "uset " + key;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trimWhitespace(line);
        if (trimmed.rfind(needle, 0) != 0) {
            continue;
        }

        const auto valueStart = trimmed.find_first_not_of(" \t", needle.size());
        if (valueStart == std::string::npos) {
            return std::string();
        }

        return stripOptionalQuotes(trimmed.substr(valueStart));
    }

    return std::nullopt;
}

std::filesystem::file_time_type pathWriteTimeOrMin(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return (std::filesystem::file_time_type::min)();
    }
    return std::filesystem::last_write_time(path, ec);
}

std::filesystem::file_time_type profileActivityTime(const std::filesystem::path& profileDir) {
    std::filesystem::file_time_type latest = pathWriteTimeOrMin(profileDir / "config.cfg");

    const std::filesystem::path saveDir = profileDir / "save";
    std::error_code ec;
    if (!std::filesystem::is_directory(saveDir, ec)) {
        return latest;
    }

    for (std::filesystem::recursive_directory_iterator it(saveDir, ec), end; it != end; ++it) {
        if (it->is_regular_file(ec)) {
            latest = (std::max)(latest, it->last_write_time(ec));
        }
    }

    return latest;
}

std::filesystem::path findActiveProfileDirectory(const std::filesystem::path& gameHome) {
    if (gameHome.empty()) {
        return {};
    }

    const auto rootConfig = gameHome / "config.cfg";
    if (const auto profileId = readUsetConfigValue(rootConfig, "g_profile")) {
        if (!profileId->empty()) {
            for (const char* subdir : {"profiles", "steam_profiles"}) {
                const auto candidate = gameHome / subdir / *profileId;
                std::error_code ec;
                if (std::filesystem::is_regular_file(candidate / "config.cfg", ec)) {
                    return candidate;
                }
            }
        }
    }

    std::filesystem::path bestProfile;
    std::filesystem::file_time_type bestTime = (std::filesystem::file_time_type::min)();
    for (const char* subdir : {"profiles", "steam_profiles"}) {
        const auto base = gameHome / subdir;
        std::error_code ec;
        if (!std::filesystem::is_directory(base, ec)) {
            continue;
        }

        for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
            if (ec || !entry.is_directory()) {
                continue;
            }

            const auto activityTime = profileActivityTime(entry.path());
            if (activityTime > bestTime) {
                bestTime = activityTime;
                bestProfile = entry.path();
            }
        }
    }

    return bestProfile;
}

const char* currencySymbolForIndex(int currencyIndex, bool isAts) {
    if (isAts) {
        switch (currencyIndex) {
        case 0:
            return "$";
        case 1:
            return "\xE2\x82\xAC"; // €
        default:
            return "$";
        }
    }

    switch (currencyIndex) {
    case 0:
        return "\xE2\x82\xAC"; // €
    case 1:
        return "CHF";
    case 2:
        return "K\xC4\x8D"; // Kč
    case 3:
        return "\xC2\xA3"; // £
    case 4:
        return "z\xC5\x82"; // zł
    case 5:
        return "Ft";
    case 6:
        return "kr";
    case 7:
        return "kr";
    case 8:
        return "kr";
    default:
        return "\xE2\x82\xAC"; // €
    }
}

std::string resolveCurrencySymbol() {
    const auto profileDir = findActiveProfileDirectory(g_gameHome);
    if (profileDir.empty()) {
        return currencySymbolForIndex(0, g_isAts);
    }

    const auto currencyValue = readUsetConfigValue(profileDir / "config.cfg", "g_currency");
    if (!currencyValue || currencyValue->empty()) {
        return currencySymbolForIndex(0, g_isAts);
    }

    try {
        return currencySymbolForIndex(std::stoi(*currencyValue), g_isAts);
    } catch (const std::exception&) {
        return currencySymbolForIndex(0, g_isAts);
    }
}

std::string formatCurrencyAmount(int64_t amount) {
    const bool negative = amount < 0;
    if (negative) {
        amount = -amount;
    }

    std::string digits = std::to_string(amount);
    std::string grouped;
    grouped.reserve(digits.size() + digits.size() / 3);
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            grouped += '.';
        }
        grouped += digits[i];
    }

    std::ostringstream formatted;
    formatted << resolveCurrencySymbol();
    if (negative) {
        formatted << '-';
    }
    formatted << grouped;
    return formatted.str();
}

std::string formatCurrencyValue(const scs_value_t& v) {
    const auto amount = valueToInt64(v);
    if (!amount.has_value()) {
        return {};
    }
    return formatCurrencyAmount(*amount);
}

std::string formatCargoDamagePercent(const scs_value_t& v) {
    const auto fraction = valueToDouble(v);
    if (!fraction.has_value()) {
        return {};
    }
    const int percent = static_cast<int>(std::lround(*fraction * 100.0));
    return std::to_string(percent) + '%';
}

std::string formatDistanceKmInteger(const scs_value_t& v) {
    const auto distance = valueToDouble(v);
    if (!distance.has_value()) {
        return {};
    }
    const int rounded = static_cast<int>(std::lround(*distance));
    return std::to_string(rounded);
}

std::string displayOrNotAvailable(const std::string& value) {
    return value.empty() ? g_localeSettings.notAvailable : value;
}

std::string formatGameMinutesDuration(scs_u32_t totalMinutes) {
    constexpr scs_u32_t minutesPerHour = 60;
    constexpr scs_u32_t minutesPerDay = 24 * minutesPerHour;

    scs_u32_t days = totalMinutes / minutesPerDay;
    totalMinutes %= minutesPerDay;
    const scs_u32_t hours = totalMinutes / minutesPerHour;
    const scs_u32_t minutes = totalMinutes % minutesPerHour;

    std::ostringstream formatted;
    if (days > 0) {
        formatted << days << ' ' << (days == 1 ? g_localeSettings.day : g_localeSettings.days);
    }
    if (hours > 0) {
        if (days > 0) {
            formatted << ' ';
        }
        formatted << hours << ' ' << (hours == 1 ? g_localeSettings.hour : g_localeSettings.hours);
    }
    if (minutes > 0 || (days == 0 && hours == 0)) {
        if (days > 0 || hours > 0) {
            formatted << ' ';
        }
        formatted << minutes << ' ' << (minutes == 1 ? g_localeSettings.minute : g_localeSettings.minutes);
    }
    return formatted.str();
}

std::string buildTruckDisplayName(const std::string& brand, const std::string& name) {
    if (!brand.empty() && !name.empty()) {
        return brand + " " + name;
    }
    if (!name.empty()) {
        return name;
    }
    return brand;
}

void updateTelemetryCacheFromConfiguration(const scs_telemetry_configuration_t* config) {
    if (config == nullptr || config->id == nullptr || config->attributes == nullptr) {
        return;
    }

    if (std::strcmp(config->id, SCS_TELEMETRY_CONFIG_truck) == 0) {
        std::string brand;
        std::string name;
        bool hasAny = false;
        for (const scs_named_value_t* nv = config->attributes; nv->name != nullptr; ++nv) {
            if (std::strcmp(nv->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand) == 0) {
                brand = valueToString(nv->value);
                hasAny = hasAny || !brand.empty();
            } else if (std::strcmp(nv->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name) == 0) {
                name = valueToString(nv->value);
                hasAny = hasAny || !name.empty();
            }
        }
        g_telemetryCache.truck = hasAny ? buildTruckDisplayName(brand, name) : std::string();
        return;
    }

    if (std::strcmp(config->id, SCS_TELEMETRY_CONFIG_job) == 0) {
        std::string cargo;
        std::string sourceCity;
        std::string destinationCity;
        for (const scs_named_value_t* nv = config->attributes; nv->name != nullptr; ++nv) {
            if (std::strcmp(nv->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo) == 0) {
                cargo = valueToString(nv->value);
            } else if (std::strcmp(nv->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city) == 0) {
                sourceCity = valueToString(nv->value);
            } else if (std::strcmp(nv->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city) == 0) {
                destinationCity = valueToString(nv->value);
            }
        }

        if (cargo.empty() && sourceCity.empty() && destinationCity.empty()) {
            g_telemetryCache.cargo.clear();
            g_telemetryCache.startingLocation.clear();
            g_telemetryCache.destination.clear();
            return;
        }

        if (!cargo.empty()) {
            g_telemetryCache.cargo = cargo;
        }
        if (!sourceCity.empty()) {
            g_telemetryCache.startingLocation = sourceCity;
        }
        if (!destinationCity.empty()) {
            g_telemetryCache.destination = destinationCity;
        }
    }
}

void fillCurrentEventFromCachedConfiguration(CurrentEvent& out) {
    out.truck = g_telemetryCache.truck;
    out.cargo = g_telemetryCache.cargo;
    out.startingLocation = g_telemetryCache.startingLocation;
    out.destination = g_telemetryCache.destination;
}

void fillCurrentEventFromGameplayAttributes(CurrentEvent& out, GameEvent kind, const scs_telemetry_gameplay_event_t* gameplay) {
    if (gameplay == nullptr || gameplay->attributes == nullptr) {
        return;
    }
    for (const scs_named_value_t* nv = gameplay->attributes; nv->name != nullptr; ++nv) {
        const char* const n = nv->name;
        const scs_value_t& val = nv->value;
        if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_cancel_penalty) == 0) {
            out.penalty = formatCurrencyValue(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_revenue) == 0) {
            out.revenue = formatCurrencyValue(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_earned_xp) == 0) {
            out.earnedXp = valueToString(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_cargo_damage) == 0) {
            out.cargoDamage = formatCargoDamagePercent(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_distance_km) == 0) {
            out.distanceDriven = formatDistanceKmInteger(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_delivery_time) == 0) {
            if (val.type == SCS_VALUE_TYPE_u32) {
                out.deliveryTime = formatGameMinutesDuration(val.value_u32.value);
            } else if (val.type == SCS_VALUE_TYPE_u64) {
                out.deliveryTime = formatGameMinutesDuration(static_cast<scs_u32_t>(val.value_u64.value));
            } else {
                out.deliveryTime = valueToString(val);
            }
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_auto_park_used) == 0) {
            out.autoparkUsed = valueToString(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_auto_load_used) == 0) {
            out.autoloadUsed = valueToString(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_offence) == 0) {
            out.offence = valueToString(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_fine_amount) == 0) {
            out.fineAmount = formatCurrencyValue(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_source_name) == 0) {
            out.useStartingLocation = valueToString(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_target_name) == 0) {
            out.useDestination = valueToString(val);
        } else if (std::strcmp(n, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_pay_amount) == 0) {
            if (kind == PlayerTollgatePaid) {
                out.tollAmount = formatCurrencyValue(val);
            } else if (kind == PlayerUseFerry || kind == PlayerUseTrain) {
                out.useAmount = formatCurrencyValue(val);
            }
        }
    }
}

void replaceAll(std::string& text, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) {
        return;
    }
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

std::string escapeJsonString(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string currentUtcTimestampIso8601() {
    const auto now = std::chrono::system_clock::now();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);

    std::tm utcTime{};
#ifdef _WIN32
    gmtime_s(&utcTime, &seconds);
#else
    gmtime_r(&seconds, &utcTime);
#endif

    std::ostringstream formatted;
    formatted << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%S");
    formatted << '.' << std::setfill('0') << std::setw(3) << millis.count() << 'Z';
    return formatted.str();
}

void applyEventPlaceholders(std::string& json, const CurrentEvent& event) {
    const auto replaceField = [&json](const char* key, const std::string& value, bool allowNotAvailable = true) {
        const std::string& resolved = allowNotAvailable ? displayOrNotAvailable(value) : value;
        replaceAll(json, std::string("{") + key + "}", escapeJsonString(resolved));
    };

    replaceField("truck", event.truck);
    replaceField("cargo", event.cargo);
    replaceField("startingLocation", event.startingLocation);
    replaceField("destination", event.destination);
    replaceField("penalty", event.penalty);
    replaceField("revenue", event.revenue);
    replaceField("earnedXp", event.earnedXp);
    replaceField("cargoDamage", event.cargoDamage);
    replaceField("distanceDriven", event.distanceDriven);
    replaceField("deliveryTime", event.deliveryTime);
    replaceField("autoparkUsed", event.autoparkUsed);
    replaceField("autoloadUsed", event.autoloadUsed);
    replaceField("offence", event.offence);
    replaceField("fineAmount", event.fineAmount);
    replaceField("tollAmount", event.tollAmount);
    replaceField("useAmount", event.useAmount);
    replaceField("useStartingLocation", event.useStartingLocation);
    replaceField("useDestination", event.useDestination);
    replaceField("timestamp", currentUtcTimestampIso8601(), false);
}

constexpr int WEBHOOK_HTTP_TIMEOUT_MS = 30000;

struct WebhookJob {
    std::string webhookURL;
    std::string payload;
};

std::mutex g_webhookQueueMutex;
std::condition_variable g_webhookQueueCv;
std::queue<WebhookJob> g_webhookQueue;
std::thread g_webhookWorkerThread;
std::atomic<bool> g_webhookWorkerRunning{false};

std::string webhookHostForLog(const std::string& webhookURL) {
    const auto schemeEnd = webhookURL.find("://");
    const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
    if (hostStart >= webhookURL.size()) {
        return "webhook";
    }

    const auto pathStart = webhookURL.find('/', hostStart);
    if (pathStart == std::string::npos) {
        return webhookURL.substr(hostStart);
    }
    return webhookURL.substr(hostStart, pathStart - hostStart);
}

bool postJsonWebhook(const std::string& webhookURL, const std::string& jsonUtf8, std::string& failureDetail);

void webhookWorkerLoop() {
    std::unique_lock<std::mutex> lock(g_webhookQueueMutex);
    while (true) {
        g_webhookQueueCv.wait(lock, [] {
            return !g_webhookQueue.empty() || !g_webhookWorkerRunning.load();
        });

        while (!g_webhookQueue.empty()) {
            WebhookJob job = std::move(g_webhookQueue.front());
            g_webhookQueue.pop();
            lock.unlock();

            std::string failureDetail;
            if (!postJsonWebhook(job.webhookURL, job.payload, failureDetail)) {
                if (failureDetail.empty()) {
                    failureDetail = "request failed";
                }
                logError(("Webhook POST failed (" + webhookHostForLog(job.webhookURL) + "): " + failureDetail).c_str());
            }

            lock.lock();
        }

        if (!g_webhookWorkerRunning.load()) {
            break;
        }
    }
}

void enqueueWebhookJob(WebhookJob job) {
    {
        std::lock_guard<std::mutex> lock(g_webhookQueueMutex);
        g_webhookQueue.push(std::move(job));
    }
    g_webhookQueueCv.notify_one();
}

#ifdef _WIN32
bool postJsonWebhook(const std::string& webhookURL, const std::string& jsonUtf8, std::string& failureDetail) {
    failureDetail.clear();
    if (webhookURL.empty()) {
        failureDetail = "empty URL";
        return false;
    }

    const int wchars = MultiByteToWideChar(CP_UTF8, 0, webhookURL.c_str(), -1, nullptr, 0);
    if (wchars <= 1) {
        failureDetail = "invalid URL";
        return false;
    }
    std::vector<wchar_t> wurl(static_cast<size_t>(wchars));
    MultiByteToWideChar(CP_UTF8, 0, webhookURL.c_str(), -1, wurl.data(), wchars);

    URL_COMPONENTS urlComponents{};
    urlComponents.dwStructSize = sizeof(urlComponents);
    urlComponents.dwSchemeLength = static_cast<DWORD>(-1);
    urlComponents.dwHostNameLength = static_cast<DWORD>(-1);
    urlComponents.dwUrlPathLength = static_cast<DWORD>(-1);
    urlComponents.dwExtraInfoLength = static_cast<DWORD>(-1);

    const DWORD urlLen = static_cast<DWORD>(wcslen(wurl.data()));
    if (!WinHttpCrackUrl(wurl.data(), urlLen, 0u, &urlComponents)) {
        failureDetail = "invalid URL";
        return false;
    }

    const std::wstring host(urlComponents.lpszHostName, urlComponents.dwHostNameLength);
    std::wstring urlPath(urlComponents.lpszUrlPath, urlComponents.dwUrlPathLength);
    if (urlComponents.dwExtraInfoLength > 0u && urlComponents.lpszExtraInfo != nullptr) {
        urlPath.append(urlComponents.lpszExtraInfo, urlComponents.dwExtraInfoLength);
    }

    INTERNET_PORT port = urlComponents.nPort;
    if (port == 0) {
        port = (urlComponents.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT
                                                                  : INTERNET_DEFAULT_HTTP_PORT;
    }

    DWORD openFlags = 0u;
    if (urlComponents.nScheme == INTERNET_SCHEME_HTTPS) {
        openFlags |= WINHTTP_FLAG_SECURE;
    }

    HINTERNET session = WinHttpOpen(
        L"Towhitch/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0u);
    if (!session) {
        failureDetail = "connection failed";
        return false;
    }

    if (!WinHttpSetTimeouts(
            session,
            WEBHOOK_HTTP_TIMEOUT_MS,
            WEBHOOK_HTTP_TIMEOUT_MS,
            WEBHOOK_HTTP_TIMEOUT_MS,
            WEBHOOK_HTTP_TIMEOUT_MS)) {
        WinHttpCloseHandle(session);
        failureDetail = "connection failed";
        return false;
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0u);
    if (!connection) {
        WinHttpCloseHandle(session);
        failureDetail = "connection failed";
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"POST",
        urlPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        openFlags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        failureDetail = "request failed";
        return false;
    }

    static const wchar_t contentTypeHeader[] = L"Content-Type: application/json\r\n";
    if (!WinHttpAddRequestHeaders(
            request,
            contentTypeHeader,
            static_cast<DWORD>(-1),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        failureDetail = "request failed";
        return false;
    }

    const DWORD bodyLen = static_cast<DWORD>(jsonUtf8.size());
    if (!WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0u,
            jsonUtf8.empty() ? nullptr : const_cast<char*>(jsonUtf8.data()),
            bodyLen,
            bodyLen,
            0u)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        failureDetail = "request failed";
        return false;
    }

    const BOOL responseOk = WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0u;
    DWORD statusSize = sizeof(status);
    if (responseOk) {
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (!responseOk) {
        failureDetail = "no response";
        return false;
    }
    if (status < 200u || status >= 300u) {
        failureDetail = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}
#else
bool postJsonWebhook(const std::string& webhookURL, const std::string& jsonUtf8, std::string& failureDetail) {
    failureDetail.clear();
    if (webhookURL.empty()) {
        failureDetail = "empty URL";
        return false;
    }
    if (!g_curlInitialized) {
        failureDetail = "libcurl not initialized";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        failureDetail = "request failed";
        return false;
    }

    struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
    if (headers == nullptr) {
        curl_easy_cleanup(curl);
        failureDetail = "request failed";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, webhookURL.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonUtf8.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Towhitch/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(WEBHOOK_HTTP_TIMEOUT_MS / 1000));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode performResult = curl_easy_perform(curl);
    long status = 0;
    if (performResult == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (performResult != CURLE_OK) {
        failureDetail = curl_easy_strerror(performResult);
        return false;
    }
    if (status < 200 || status >= 300) {
        failureDetail = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}
#endif

bool startWebhookWorker() {
    if (g_webhookWorkerRunning.load()) {
        return true;
    }
    g_webhookWorkerRunning = true;
    try {
        g_webhookWorkerThread = std::thread(webhookWorkerLoop);
    } catch (...) {
        g_webhookWorkerRunning = false;
        logError("Failed to start webhook worker thread.");
        return false;
    }
    return true;
}

void stopWebhookWorker() {
    if (!g_webhookWorkerRunning.load()) {
        return;
    }
    g_webhookWorkerRunning = false;
    g_webhookQueueCv.notify_all();
    if (g_webhookWorkerThread.joinable()) {
        g_webhookWorkerThread.join();
    }
}

void rollbackInit(const bool unregisterConfiguration) {
    if (unregisterConfiguration && g_unregisterFromEvent != nullptr && g_configurationEventRegistered) {
        g_unregisterFromEvent(SCS_TELEMETRY_EVENT_configuration);
        g_configurationEventRegistered = false;
    }
    g_webhookConfigs.clear();
    stopWebhookWorker();
#ifndef _WIN32
    shutdownCurl();
#endif
    defaultLog = nullptr;
    g_unregisterFromEvent = nullptr;
}

} // namespace

namespace {

constexpr const char* TOWHITCH_CFG_FILENAME = "Towhitch.cfg";
constexpr const char* GAME_CFG_FILENAME = "config.cfg";

bool isGameHomeFolder(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / GAME_CFG_FILENAME, ec);
}

bool tryAcceptGameHome(const std::filesystem::path& candidate, std::filesystem::path& outHome) {
    if (!isGameHomeFolder(candidate)) {
        return false;
    }
    outHome = candidate;
    logMessage(("Found game home: " + candidate.string()).c_str());
    return true;
}

std::filesystem::path gameHomeFromBase(const std::filesystem::path& base, const std::string& gameFolderName) {
    if (base.empty() || gameFolderName.empty()) {
        return {};
    }
    return base / gameFolderName;
}

std::string stripTrailingGameVersionSuffix(const std::string& name) {
    // SDK game_name may append " <major>.<minor>.<patch><suffix>" (e.g. " 1.59.1.3s").
    const auto pos = name.rfind(' ');
    if (pos == std::string::npos || pos + 1 >= name.size()) {
        return name;
    }

    const char* const rest = name.c_str() + pos + 1;
    if (!std::isdigit(static_cast<unsigned char>(rest[0]))) {
        return name;
    }

    for (const char* p = rest; *p != '\0'; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!std::isdigit(c) && *p != '.' && *p != 's') {
            return name;
        }
    }

    return name.substr(0, pos);
}

std::string resolveGameFolderName(const scs_telemetry_init_params_v101_t* versionParams) {
    if (std::strcmp(versionParams->common.game_id, SCS_GAME_ID_EUT2) == 0) {
        return "Euro Truck Simulator 2";
    }
    if (std::strcmp(versionParams->common.game_id, SCS_GAME_ID_ATS) == 0) {
        return "American Truck Simulator";
    }

    if (versionParams->common.game_name != nullptr && versionParams->common.game_name[0] != '\0') {
        return stripTrailingGameVersionSuffix(versionParams->common.game_name);
    }
    return {};
}

std::filesystem::path getExeDirectory() {
#ifdef _WIN32
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
#elif defined(__linux__)
    char buffer[PATH_MAX]{};
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return {};
    }
    buffer[length] = '\0';
    return std::filesystem::path(buffer).parent_path();
#elif defined(__APPLE__)
    char buffer[PATH_MAX]{};
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
#else
    return {};
#endif
}

std::filesystem::path getCurrentWorkingDirectory() {
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{} : cwd;
}

#ifdef _WIN32
std::filesystem::path parseHomedirFromCommandLineWindows() {
    const wchar_t* const cmdLine = GetCommandLineW();
    if (cmdLine == nullptr) {
        return {};
    }

    const std::wstring commandLine(cmdLine);
    const std::wstring needle = L"-homedir";
    for (size_t pos = 0; pos < commandLine.size();) {
        const size_t found = commandLine.find(needle, pos);
        if (found == std::wstring::npos) {
            break;
        }

        const bool atArgStart = found == 0 || std::iswspace(commandLine[found - 1]);
        if (!atArgStart) {
            pos = found + needle.size();
            continue;
        }

        size_t valueStart = found + needle.size();
        while (valueStart < commandLine.size() && std::iswspace(commandLine[valueStart])) {
            ++valueStart;
        }
        if (valueStart >= commandLine.size()) {
            break;
        }

        if (commandLine[valueStart] == L'"') {
            ++valueStart;
            const size_t valueEnd = commandLine.find(L'"', valueStart);
            if (valueEnd == std::wstring::npos) {
                break;
            }
            return std::filesystem::path(commandLine.substr(valueStart, valueEnd - valueStart));
        }

        size_t valueEnd = valueStart;
        while (valueEnd < commandLine.size() && !std::iswspace(commandLine[valueEnd])) {
            ++valueEnd;
        }
        return std::filesystem::path(commandLine.substr(valueStart, valueEnd - valueStart));
    }

    return {};
}

std::filesystem::path getWindowsDocumentsFolder() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
        return {};
    }
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::filesystem::path findGameHomeWindows(const std::string& gameFolderName) {
    std::filesystem::path gameHome;

    const auto homedir = parseHomedirFromCommandLineWindows();
    if (!homedir.empty()) {
        if (tryAcceptGameHome(homedir, gameHome)) {
            return gameHome;
        }

        const auto candidate = gameHomeFromBase(homedir, gameFolderName);
        if (tryAcceptGameHome(candidate, gameHome)) {
            return gameHome;
        }
    }

    const auto documents = getWindowsDocumentsFolder();
    if (!documents.empty()) {
        const auto candidate = gameHomeFromBase(documents, gameFolderName);
        if (tryAcceptGameHome(candidate, gameHome)) {
            return gameHome;
        }
    }

    const auto exeDir = getExeDirectory();
    if (!exeDir.empty()) {
        if (tryAcceptGameHome(exeDir, gameHome)) {
            return gameHome;
        }
    }

    const auto cwd = getCurrentWorkingDirectory();
    if (!cwd.empty()) {
        if (tryAcceptGameHome(cwd, gameHome)) {
            return gameHome;
        }
    }

    if (const char* const userProfile = std::getenv("USERPROFILE")) {
        const std::filesystem::path profile(userProfile);
        const std::filesystem::path fallbackCandidates[] = {
            profile / "OneDrive" / "Documents" / gameFolderName,
            profile / "Documents" / gameFolderName,
        };
        for (const auto& candidate : fallbackCandidates) {
            if (tryAcceptGameHome(candidate, gameHome)) {
                return gameHome;
            }
        }
    }

    return {};
}
#else
std::filesystem::path parseHomedirFromCommandLineUnix() {
#if defined(__linux__)
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    if (!cmdline) {
        return {};
    }

    std::vector<std::string> args;
    std::string arg;
    char ch = '\0';
    while (cmdline.get(ch)) {
        if (ch == '\0') {
            if (!arg.empty()) {
                args.push_back(std::move(arg));
                arg.clear();
            }
        } else {
            arg.push_back(ch);
        }
    }
    if (!arg.empty()) {
        args.push_back(std::move(arg));
    }

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-homedir" && i + 1 < args.size()) {
            return std::filesystem::path(args[i + 1]);
        }
    }
#endif
    return {};
}

std::filesystem::path findGameHomeUnix(const std::string& gameFolderName) {
    std::filesystem::path gameHome;

    const auto homedir = parseHomedirFromCommandLineUnix();
    if (!homedir.empty()) {
        if (tryAcceptGameHome(homedir, gameHome)) {
            return gameHome;
        }

        const auto candidate = gameHomeFromBase(homedir, gameFolderName);
        if (tryAcceptGameHome(candidate, gameHome)) {
            return gameHome;
        }
    }

    std::filesystem::path dataHome;
    if (const char* const xdgDataHome = std::getenv("XDG_DATA_HOME")) {
        dataHome = xdgDataHome;
    } else if (const char* const home = std::getenv("HOME")) {
#if defined(__APPLE__)
        dataHome = std::filesystem::path(home) / "Library" / "Application Support";
#else
        dataHome = std::filesystem::path(home) / ".local" / "share";
#endif
    }

    if (!dataHome.empty()) {
        const auto candidate = gameHomeFromBase(dataHome, gameFolderName);
        if (tryAcceptGameHome(candidate, gameHome)) {
            return gameHome;
        }
    }

    const auto exeDir = getExeDirectory();
    if (!exeDir.empty()) {
        if (tryAcceptGameHome(exeDir, gameHome)) {
            return gameHome;
        }
    }

    const auto cwd = getCurrentWorkingDirectory();
    if (!cwd.empty()) {
        if (tryAcceptGameHome(cwd, gameHome)) {
            return gameHome;
        }
    }

    return {};
}
#endif

std::filesystem::path findGameHome(const scs_telemetry_init_params_v101_t* versionParams) {
    const std::string gameFolderName = resolveGameFolderName(versionParams);
    if (gameFolderName.empty()) {
        logError("Cannot resolve game folder name for home directory search.");
        return {};
    }

#ifdef _WIN32
    return findGameHomeWindows(gameFolderName);
#else
    return findGameHomeUnix(gameFolderName);
#endif
}

std::filesystem::path findTowhitchConfig(const std::filesystem::path& gameHome) {
    const std::filesystem::path candidates[] = {
        gameHome.empty() ? std::filesystem::path{} : gameHome / TOWHITCH_CFG_FILENAME,
        gameHome.empty() ? std::filesystem::path{} : gameHome / "plugins" / TOWHITCH_CFG_FILENAME,
    };

    for (const auto& candidate : candidates) {
        if (candidate.empty()) {
            continue;
        }

        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            logMessage(("Using Towhitch.cfg: " + candidate.string()).c_str());
            return candidate;
        }
    }

    return {};
}

} // namespace

static void dispatchWebhooksForEvent(GameEvent kind, const CurrentEvent& currentEvent) {
    for (const auto& cfg : g_webhookConfigs) {
        if (cfg.eventTrigger == kind) {
            executeWebhook(cfg, currentEvent);
        }
    }
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_01) {
        return SCS_RESULT_unsupported;
    }
    if (!params) {
        return SCS_RESULT_invalid_parameter;
    }

    const scs_telemetry_init_params_v101_t* const versionParams =
        static_cast<const scs_telemetry_init_params_v101_t*>(params);

    defaultLog = versionParams->common.log;
    g_unregisterFromEvent = versionParams->unregister_from_event;
    g_configurationEventRegistered = false;
    g_gameplayEventRegistered = false;
    g_telemetryCache = {};

    logMessage("Loading Tow Hitch plugin...");

    {
        std::string gameLine = "Game '";
        gameLine += versionParams->common.game_id;
        gameLine += "' ";
        gameLine += std::to_string(SCS_GET_MAJOR_VERSION(versionParams->common.game_version));
        gameLine += '.';
        gameLine += std::to_string(SCS_GET_MINOR_VERSION(versionParams->common.game_version));
        logMessage(gameLine.c_str());
    }

    if (std::strcmp(versionParams->common.game_id, SCS_GAME_ID_EUT2) == 0) {
        const scs_u32_t minimalVersion = SCS_TELEMETRY_EUT2_GAME_VERSION_1_00;
        if (versionParams->common.game_version < minimalVersion) {
            logError("WARNING: Too old version of the game, some features might behave incorrectly");
        }
        const scs_u32_t implementedVersion = SCS_TELEMETRY_EUT2_GAME_VERSION_CURRENT;
        if (SCS_GET_MAJOR_VERSION(versionParams->common.game_version) > SCS_GET_MAJOR_VERSION(implementedVersion)) {
            logError("WARNING: Too new major version of the game, some features might behave incorrectly");
        }
    } else if (std::strcmp(versionParams->common.game_id, SCS_GAME_ID_ATS) == 0) {
        const scs_u32_t minimalVersion = SCS_TELEMETRY_ATS_GAME_VERSION_1_00;
        if (versionParams->common.game_version < minimalVersion) {
            logError("WARNING: Too old version of the game, some features might behave incorrectly");
        }
        const scs_u32_t implementedVersion = SCS_TELEMETRY_ATS_GAME_VERSION_CURRENT;
        if (SCS_GET_MAJOR_VERSION(versionParams->common.game_version) > SCS_GET_MAJOR_VERSION(implementedVersion)) {
            logError("WARNING: Too new major version of the game, some features might behave incorrectly");
        }
    } else {
        logError("WARNING: Unsupported game, some features or values might behave incorrectly");
    }

    const auto gameHome = findGameHome(versionParams);
    g_gameHome = gameHome;
    g_isAts = std::strcmp(versionParams->common.game_id, SCS_GAME_ID_ATS) == 0;
    if (gameHome.empty()) {
        logError("WARNING: Could not locate game home folder (config.cfg not found). Towhitch.cfg must be in the game home directory.");
    }

    const auto configPath = findTowhitchConfig(gameHome);
    if (configPath.empty()) {
        const std::string msg =
            "Configuration file Towhitch.cfg not found. Place it in the game home directory (alongside config.cfg) or in home/plugins/.";
        logError(msg.c_str());
        defaultLog = nullptr;
        g_unregisterFromEvent = nullptr;
        return SCS_RESULT_invalid_parameter;
    }

    loadConfiguration(configPath.string());

#ifndef _WIN32
    if (!initializeCurl()) {
        rollbackInit(false);
        return SCS_RESULT_generic_error;
    }
#endif

    if (!startWebhookWorker()) {
        rollbackInit(false);
        return SCS_RESULT_generic_error;
    }

    if (versionParams->register_for_event(SCS_TELEMETRY_EVENT_configuration, telemetry_configuration_event, nullptr) !=
        SCS_RESULT_ok) {
        logError("Unable to register configuration event callback.");
        rollbackInit(false);
        return SCS_RESULT_generic_error;
    }
    g_configurationEventRegistered = true;

    if (versionParams->register_for_event(SCS_TELEMETRY_EVENT_gameplay, telemetry_gameplay_event, nullptr) != SCS_RESULT_ok) {
        logError("Unable to register gameplay event callback.");
        rollbackInit(true);
        return SCS_RESULT_generic_error;
    }
    g_gameplayEventRegistered = true;

    logMessage("Loading complete.");
    return SCS_RESULT_ok;
}

SCSAPI_VOID telemetry_configuration_event(const scs_event_t event, const void* const eventInfo, const scs_context_t context) {
    (void)event;
    (void)context;

    const auto* const config = static_cast<const scs_telemetry_configuration_t*>(eventInfo);
    updateTelemetryCacheFromConfiguration(config);
}

SCSAPI_VOID telemetry_gameplay_event(const scs_event_t event, const void* const eventInfo, const scs_context_t context) {
    (void)event;
    (void)context;

    const auto* const gameplay = static_cast<const scs_telemetry_gameplay_event_t*>(eventInfo);
    const char* const id = gameplay != nullptr ? gameplay->id : nullptr;
    const GameEvent kind = gameEventFromSdkId(id);

    CurrentEvent currentEvent{};
    fillCurrentEventFromCachedConfiguration(currentEvent);
    if (gameplay != nullptr && kind != Unknown) {
        fillCurrentEventFromGameplayAttributes(currentEvent, kind, gameplay);
    }

    if (kind != Unknown) {
        dispatchWebhooksForEvent(kind, currentEvent);
    }
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    if (g_unregisterFromEvent != nullptr) {
        if (g_gameplayEventRegistered) {
            g_unregisterFromEvent(SCS_TELEMETRY_EVENT_gameplay);
            g_gameplayEventRegistered = false;
        }
        if (g_configurationEventRegistered) {
            g_unregisterFromEvent(SCS_TELEMETRY_EVENT_configuration);
            g_configurationEventRegistered = false;
        }
    }
    g_unregisterFromEvent = nullptr;

    stopWebhookWorker();
    g_webhookConfigs.clear();
    g_localeSettings = defaultLocaleSettings();
    g_telemetryCache = {};
    g_gameHome.clear();
    g_isAts = false;

#ifndef _WIN32
    shutdownCurl();
#endif

    logMessage("Shutdown complete.");
    defaultLog = nullptr;
}

/*
* Send out a postmessage to a preconfigured webhook
*/
static void executeWebhook(const WebhookConfiguration& configuration, const CurrentEvent& event) {
    if (configuration.webhookURL.empty()) {
        logError("Skipping webhook: empty webhookURL.");
        return;
    }

    std::string payload = configuration.payloadJSON.empty() ? std::string("{}") : configuration.payloadJSON;
    applyEventPlaceholders(payload, event);

    WebhookJob job;
    job.webhookURL = configuration.webhookURL;
    job.payload = std::move(payload);
    enqueueWebhookJob(std::move(job));
}

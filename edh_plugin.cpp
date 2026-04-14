/**
 * Minimal ETS2/ATS telemetry plugin: append a line to a file when a job is delivered.
 *
 * Install the built DLL in the game's plugins folder (e.g. ...\bin\win_x64\plugins\).
 * See tools/scs_sdk/readme.txt for load paths and sdk reload commands.
 *
 * Default templates (default.cfg, embed JSON, etc.) are embedded in the DLL at build time; see cmake/edh_gen_embedded.cmake.
 * On init, if missing, edh_webhook.cfg is created under the game's Documents profile folder (Euro Truck Simulator 2 or American Truck Simulator) from the embedded default.
 * The same file is read for language, discord.embed.*, and discord.webhook. Embed template: embed_{language}.json when embedded (e.g. EN, NL); otherwise embed.json. On job delivery the payload is POSTed to discord.webhook; EDH_webhook.log records timestamped status lines (not raw JSON or gameplay spam). Placeholders {distance} and {gains} use the game's config.cfg when present: uset g_mph for km vs miles, uset g_currency for symbol (fallback: € in ETS2, $ in ATS if unset).
 *
 * Debugging:
 * - Game log callback: messages appear in the in-game log (see game.log.txt under Documents\\Euro Truck Simulator 2).
 * - File: Euro Truck Simulator 2 profile folder\\EDH_webhook.log (timestamped lines).
 * - OutputDebugString: use Sysinternals DebugView with "Capture Global Win32" if needed.
 * - Console: sdk reload / sdk reinit (only if telemetry plugins loaded; see tools/scs_sdk/readme.txt).
 */

#ifdef _WIN32
#	define WINVER 0x0500
#	define _WIN32_WINNT 0x0500
#	include <windows.h>
#	include <winhttp.h>
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <string>
#include <sstream>
#include <vector>

#ifndef _WIN32
#	include <sys/stat.h>
#	include <sys/time.h>
#	include <strings.h>
#endif

#ifdef _WIN32
#	define edh_stricmp _stricmp
#else
#	define edh_stricmp strcasecmp
#endif

#include "scssdk_telemetry.h"
#include "amtrucks/scssdk_ats.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_gameplay_events.h"

#include "edh_embedded_resources.h"

static void edh_strncpy_z(char *const dest, const size_t destsz, const char *const src)
{
	if (!dest || destsz == 0 || !src) {
		return;
	}
	strncpy(dest, src, destsz - 1);
	dest[destsz - 1] = '\0';
}

static scs_log_t game_log = NULL;

/** From scs_telemetry_init (e.g. "eut2", "ats") — empty until init; drives Documents\\… game folder. */
static char g_game_id[64];

static void debug_log_line(const char *const text);
static void debug_log_fmt(const char *const fmt, ...);
static void edh_plugin_log(const scs_log_type_t type, const char *const text);
static void edh_plugin_log_fmt(const scs_log_type_t type, const char *const fmt, ...);

/** Parsed from edh_webhook.cfg (discord.embed.*). */
static char g_cfg_color[64];
static char g_cfg_player[256];
/** discord.webhook — HTTPS POST payload is the filled embed JSON. */
static char g_discord_webhook[2048];
/** language — selects embedded embed_{LANG}.json when available (e.g. EN, NL). */
static char g_language[32] = "EN";

/**
 * Documents folder for the running game (Euro Truck Simulator 2 vs American Truck Simulator).
 * Before telemetry init, g_game_id is empty — defaults to ETS2 path so early DLL logs still land somewhere.
 */
static bool edh_get_game_profile_dir(char *const out, const size_t outsz)
{
	if (!out || outsz == 0) {
		return false;
	}
	const char *game_folder = "Euro Truck Simulator 2";
	if (g_game_id[0] && strcmp(g_game_id, SCS_GAME_ID_ATS) == 0) {
		game_folder = "American Truck Simulator";
	}
#ifdef _WIN32
	const char *const prof = getenv("USERPROFILE");
	if (!prof) {
		return false;
	}
	return sprintf_s(out, outsz, "%s\\Documents\\%s", prof, game_folder) > 0;
#else
	const char *xdg = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	if (xdg && xdg[0]) {
		const int n = snprintf(out, outsz, "%s/%s", xdg, game_folder);
		return n > 0 && static_cast<size_t>(n) < outsz;
	}
	if (home && home[0]) {
		const int n = snprintf(out, outsz, "%s/.local/share/%s", home, game_folder);
		return n > 0 && static_cast<size_t>(n) < outsz;
	}
	return false;
#endif
}

#ifdef _WIN32
static void edh_ensure_profile_dir_exists(const char *const dir)
{
	if (!dir || !dir[0]) {
		return;
	}
	CreateDirectoryA(dir, NULL);
}
#else
static void mkdir_p_posix(const char *const path)
{
	if (!path || !path[0]) {
		return;
	}
	char tmp[512];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	for (char *p = tmp + 1; *p; ++p) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

static void edh_ensure_profile_dir_exists(const char *const dir)
{
	if (dir && dir[0]) {
		mkdir_p_posix(dir);
	}
}
#endif

/**
 * Create %USERPROFILE%\\Documents\\…\\edh_webhook.cfg if missing (embedded default.cfg).
 */
static void ensure_edh_webhook_cfg(void)
{
	char dir[512];
	if (!edh_get_game_profile_dir(dir, sizeof(dir))) {
		edh_plugin_log(SCS_LOG_TYPE_error, "edh_plugin: could not resolve profile folder for edh_webhook.cfg");
		return;
	}
	edh_ensure_profile_dir_exists(dir);

	char user_cfg[640];
#ifdef _WIN32
	if (sprintf_s(user_cfg, sizeof(user_cfg), "%s\\edh_webhook.cfg", dir) < 0) {
		edh_plugin_log(SCS_LOG_TYPE_error, "edh_plugin: edh_webhook.cfg path too long");
		return;
	}
	if (GetFileAttributesA(user_cfg) != INVALID_FILE_ATTRIBUTES) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_message, "edh_plugin: edh_webhook.cfg already present at %s", user_cfg);
		return;
	}
#else
	if (snprintf(user_cfg, sizeof(user_cfg), "%s/edh_webhook.cfg", dir) >= (int)sizeof(user_cfg)) {
		edh_plugin_log(SCS_LOG_TYPE_error, "edh_plugin: edh_webhook.cfg path too long");
		return;
	}
	{
		FILE *const existing = fopen(user_cfg, "rb");
		if (existing) {
			fclose(existing);
			edh_plugin_log_fmt(SCS_LOG_TYPE_message, "edh_plugin: edh_webhook.cfg already present at %s", user_cfg);
			return;
		}
	}
#endif

	const void *const payload = edh_embed_default_cfg;
	const size_t payload_len = edh_embed_default_cfg_size;

	FILE *const out = fopen(user_cfg, "wb");
	if (!out) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_error, "edh_plugin: could not create edh_webhook.cfg at %s", user_cfg);
		return;
	}
	const size_t nw = fwrite(payload, 1u, payload_len, out);
	fclose(out);
	if (nw != payload_len) {
#ifdef _WIN32
		DeleteFileA(user_cfg);
#else
		remove(user_cfg);
#endif
		edh_plugin_log_fmt(SCS_LOG_TYPE_error, "edh_plugin: failed writing edh_webhook.cfg at %s", user_cfg);
		return;
	}
	edh_plugin_log_fmt(
		SCS_LOG_TYPE_message,
		"edh_plugin: created edh_webhook.cfg at %s (embedded default)",
		user_cfg
	);
}

static void trim_string(std::string *const s)
{
	if (!s) {
		return;
	}
	while (!s->empty() && isspace(static_cast<unsigned char>((*s)[0]))) {
		s->erase(0, 1);
	}
	while (!s->empty() && isspace(static_cast<unsigned char>(s->back()))) {
		s->pop_back();
	}
}

/**
 * Read edh_webhook.cfg and fill g_cfg_color / g_cfg_player (defaults if keys missing).
 */
static void load_edh_webhook_cfg(void)
{
	edh_strncpy_z(g_cfg_color, sizeof(g_cfg_color), "000000");
	edh_strncpy_z(g_cfg_player, sizeof(g_cfg_player), "Player");
	g_discord_webhook[0] = '\0';
	edh_strncpy_z(g_language, sizeof(g_language), "EN");

	char dir[512];
	if (!edh_get_game_profile_dir(dir, sizeof(dir))) {
		return;
	}
	char path[640];
#ifdef _WIN32
	if (sprintf_s(path, sizeof(path), "%s\\edh_webhook.cfg", dir) < 0) {
		return;
	}
#else
	if (snprintf(path, sizeof(path), "%s/edh_webhook.cfg", dir) >= (int)sizeof(path)) {
		return;
	}
#endif

	FILE *const f = fopen(path, "rb");
	if (!f) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_error, "edh_plugin: could not read edh_webhook.cfg at %s", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	const long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 65536) {
		fclose(f);
		edh_plugin_log_fmt(
			SCS_LOG_TYPE_warning,
			"edh_plugin: edh_webhook.cfg at %s is missing or invalid size; using built-in defaults",
			path
		);
		return;
	}
	std::string content(static_cast<size_t>(sz), '\0');
	if (fread(&content[0], 1, static_cast<size_t>(sz), f) != static_cast<size_t>(sz)) {
		fclose(f);
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "edh_plugin: short read of edh_webhook.cfg at %s; using defaults", path);
		return;
	}
	fclose(f);

	std::istringstream iss(content);
	std::string line;
	while (std::getline(iss, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		trim_string(&line);
		if (line.empty() || line[0] == '#') {
			continue;
		}
		const size_t eq = line.find('=');
		if (eq == std::string::npos) {
			continue;
		}
		std::string key = line.substr(0, eq);
		std::string valpart = line.substr(eq + 1);
		trim_string(&key);
		trim_string(&valpart);
		if (valpart.size() >= 2u && valpart[0] == '"' && valpart.back() == '"') {
			valpart = valpart.substr(1, valpart.size() - 2u);
		}
		if (key == "discord.embed.color") {
			edh_strncpy_z(g_cfg_color, sizeof(g_cfg_color), valpart.c_str());
		}
		else if (key == "discord.embed.playername") {
			edh_strncpy_z(g_cfg_player, sizeof(g_cfg_player), valpart.c_str());
		}
		else if (key == "discord.webhook") {
			edh_strncpy_z(g_discord_webhook, sizeof(g_discord_webhook), valpart.c_str());
		}
		else if (key == "language") {
			edh_strncpy_z(g_language, sizeof(g_language), valpart.c_str());
		}
	}
	edh_plugin_log_fmt(
		SCS_LOG_TYPE_message,
		"edh_plugin: settings from %s — language=%s embed_color=%s player_name=%s discord.webhook=%s",
		path,
		g_language,
		g_cfg_color,
		g_cfg_player,
		g_discord_webhook[0] ? "set" : "(empty)"
	);
}

static void format_timestamp_iso8601_utc(char *const buf, const size_t buflen)
{
	if (!buf || buflen < 28u) {
		return;
	}
#ifdef _WIN32
	SYSTEMTIME st;
	GetSystemTime(&st);
	sprintf_s(
		buf,
		buflen,
		"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
		static_cast<unsigned>(st.wYear),
		static_cast<unsigned>(st.wMonth),
		static_cast<unsigned>(st.wDay),
		static_cast<unsigned>(st.wHour),
		static_cast<unsigned>(st.wMinute),
		static_cast<unsigned>(st.wSecond),
		static_cast<unsigned>(st.wMilliseconds)
	);
#else
	struct timeval tv;
	struct tm tm_utc;
	if (gettimeofday(&tv, NULL) != 0) {
		edh_strncpy_z(buf, buflen, "1970-01-01T00:00:00.000Z");
		return;
	}
	gmtime_r(&tv.tv_sec, &tm_utc);
	const int ms = static_cast<int>(tv.tv_usec / 1000);
	snprintf(
		buf,
		buflen,
		"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
		tm_utc.tm_year + 1900,
		tm_utc.tm_mon + 1,
		tm_utc.tm_mday,
		tm_utc.tm_hour,
		tm_utc.tm_min,
		tm_utc.tm_sec,
		ms
	);
#endif
}

#ifdef _WIN32
/**
 * POST JSON body to discord.webhook URL (UTF-8). Returns true on HTTP 2xx.
 */
static bool edh_post_discord_webhook_json(const std::string &json_utf8)
{
	if (g_discord_webhook[0] == '\0') {
		return false;
	}
	const int wchars = MultiByteToWideChar(CP_UTF8, 0, g_discord_webhook, -1, NULL, 0);
	if (wchars <= 1) {
		return false;
	}
	std::vector<wchar_t> wurl(static_cast<size_t>(wchars));
	MultiByteToWideChar(CP_UTF8, 0, g_discord_webhook, -1, wurl.data(), wchars);

	URL_COMPONENTS uc;
	ZeroMemory(&uc, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.dwSchemeLength = static_cast<DWORD>(-1);
	uc.dwHostNameLength = static_cast<DWORD>(-1);
	uc.dwUrlPathLength = static_cast<DWORD>(-1);
	uc.dwExtraInfoLength = static_cast<DWORD>(-1);

	const DWORD url_len = static_cast<DWORD>(wcslen(wurl.data()));
	if (!WinHttpCrackUrl(wurl.data(), url_len, 0u, &uc)) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpCrackUrl failed: %lu", GetLastError());
		return false;
	}

	std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
	std::wstring urlpath(uc.lpszUrlPath, uc.dwUrlPathLength);
	std::wstring extra;
	if (uc.dwExtraInfoLength > 0u && uc.lpszExtraInfo != NULL) {
		extra.assign(uc.lpszExtraInfo, uc.dwExtraInfoLength);
	}
	const std::wstring full_path = urlpath + extra;

	INTERNET_PORT port = uc.nPort;
	if (port == 0) {
		port = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
	}

	DWORD open_flags = 0u;
	if (uc.nScheme == INTERNET_SCHEME_HTTPS) {
		open_flags |= WINHTTP_FLAG_SECURE;
	}

	HINTERNET h_session = WinHttpOpen(
		L"EuroTruckLog/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,
		0u
	);
	if (!h_session) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpOpen failed: %lu", GetLastError());
		return false;
	}

	HINTERNET h_connect = WinHttpConnect(h_session, host.c_str(), port, 0u);
	if (!h_connect) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpConnect failed: %lu", GetLastError());
		WinHttpCloseHandle(h_session);
		return false;
	}

	HINTERNET h_request = WinHttpOpenRequest(
		h_connect,
		L"POST",
		full_path.c_str(),
		NULL,
		WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES,
		open_flags
	);
	if (!h_request) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpOpenRequest failed: %lu", GetLastError());
		WinHttpCloseHandle(h_connect);
		WinHttpCloseHandle(h_session);
		return false;
	}

	static const wchar_t k_hdr[] = L"Content-Type: application/json\r\n";
	if (!WinHttpAddRequestHeaders(
		    h_request,
		    k_hdr,
		    static_cast<DWORD>(-1),
		    WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE
	    )) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpAddRequestHeaders failed: %lu", GetLastError());
		WinHttpCloseHandle(h_request);
		WinHttpCloseHandle(h_connect);
		WinHttpCloseHandle(h_session);
		return false;
	}

	const DWORD body_len = static_cast<DWORD>(json_utf8.size());
	BOOL ok = WinHttpSendRequest(
		h_request,
		WINHTTP_NO_ADDITIONAL_HEADERS,
		0u,
		json_utf8.empty() ? NULL : (LPVOID)(json_utf8.data()),
		body_len,
		body_len,
		0u
	);
	if (!ok) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpSendRequest failed: %lu", GetLastError());
		WinHttpCloseHandle(h_request);
		WinHttpCloseHandle(h_connect);
		WinHttpCloseHandle(h_session);
		return false;
	}

	ok = WinHttpReceiveResponse(h_request, NULL);
	DWORD status = 0u;
	DWORD status_size = sizeof(status);
	if (ok) {
		WinHttpQueryHeaders(
			h_request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&status,
			&status_size,
			WINHTTP_NO_HEADER_INDEX
		);
	}

	WinHttpCloseHandle(h_request);
	WinHttpCloseHandle(h_connect);
	WinHttpCloseHandle(h_session);

	if (!ok) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] WinHttpReceiveResponse failed: %lu", GetLastError());
		return false;
	}
	if (status < 200u || status >= 300u) {
		edh_plugin_log_fmt(SCS_LOG_TYPE_warning, "[edh_plugin] Discord webhook HTTP status %lu", static_cast<unsigned long>(status));
		return false;
	}
	return true;
}
#else
static bool edh_post_discord_webhook_json(const std::string &json_utf8)
{
	(void)json_utf8;
	return false;
}
#endif

/** Last known job (from configuration "job") — updated as the game sends fields. */
static char g_source_city[256];
static char g_destination_city[256];
static char g_cargo[256];

/** Last known truck (from configuration "truck") — brand + model name for display. */
static char g_truck_brand[128];
static char g_truck_model[256];

static void format_timestamp_local_for_log(char *const buf, const size_t buflen)
{
	if (!buf || buflen < 20u) {
		return;
	}
	const time_t t = time(NULL);
#if defined(_WIN32)
	struct tm lt;
	if (localtime_s(&lt, &t) != 0 || strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", &lt) == 0) {
		edh_strncpy_z(buf, buflen, "(time error)");
	}
#else
	struct tm *const lt = localtime(&t);
	if (!lt || strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", lt) == 0) {
		edh_strncpy_z(buf, buflen, "(time error)");
	}
#endif
}

static void edh_pick_embed_template(const unsigned char **const out_data, size_t *const out_size)
{
	*out_data = edh_embed_embed_json;
	*out_size = edh_embed_embed_json_size;
	if (edh_stricmp(g_language, "NL") == 0) {
		*out_data = edh_embed_embed_nl_json;
		*out_size = edh_embed_embed_nl_json_size;
		return;
	}
	if (edh_stricmp(g_language, "EN") == 0) {
		*out_data = edh_embed_embed_en_json;
		*out_size = edh_embed_embed_en_json_size;
		return;
	}
}

static void debug_log_line(const char *const text)
{
	char dir[512];
	if (!edh_get_game_profile_dir(dir, sizeof(dir))) {
		return;
	}
	char path[768];
#ifdef _WIN32
	if (sprintf_s(path, sizeof(path), "%s\\EDH_webhook.log", dir) < 0) {
		return;
	}
#else
	if (snprintf(path, sizeof(path), "%s/EDH_webhook.log", dir) >= (int)sizeof(path)) {
		return;
	}
#endif

	char ts[40];
	format_timestamp_local_for_log(ts, sizeof(ts));
	char line[2048];
#if defined(_WIN32)
	sprintf_s(line, sizeof(line), "[%s] %s", ts, text);
#else
	snprintf(line, sizeof(line), "[%s] %s", ts, text);
#endif

	FILE *const f = fopen(path, "a");
	if (!f) {
#ifdef _WIN32
		OutputDebugStringA("[edh_plugin] could not open EDH_webhook.log\n");
#endif
		return;
	}
	fprintf(f, "%s\n", line);
	fclose(f);
#ifdef _WIN32
	OutputDebugStringA(line);
	OutputDebugStringA("\n");
#endif
}

static void debug_log_fmt(const char *const fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
#if defined(_WIN32)
	if (vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap) < 0) {
		edh_strncpy_z(buf, sizeof(buf), "(debug_log_fmt error)");
	}
#else
	vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
	va_end(ap);
	debug_log_line(buf);
}

static void edh_plugin_log(const scs_log_type_t type, const char *const text)
{
	debug_log_line(text);
	if (game_log) {
		game_log(type, text);
	}
}

static void edh_plugin_log_fmt(const scs_log_type_t type, const char *const fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
#if defined(_WIN32)
	if (vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap) < 0) {
		edh_strncpy_z(buf, sizeof(buf), "(edh_plugin_log_fmt error)");
	}
#else
	vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
	va_end(ap);
	edh_plugin_log(type, buf);
}

static void merge_job_attribute(const scs_named_value_t *const c)
{
	if (!c || c->index != SCS_U32_NIL || c->value.type != SCS_VALUE_TYPE_string) {
		return;
	}
	const char *const v = c->value.value_string.value;
	if (!v) {
		return;
	}
	if (strcmp(c->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_source_city) == 0) {
		edh_strncpy_z(g_source_city, sizeof(g_source_city), v);
	}
	else if (strcmp(c->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_destination_city) == 0) {
		edh_strncpy_z(g_destination_city, sizeof(g_destination_city), v);
	}
	else if (strcmp(c->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_cargo) == 0) {
		edh_strncpy_z(g_cargo, sizeof(g_cargo), v);
	}
}

static void merge_truck_attribute(const scs_named_value_t *const c)
{
	if (!c || c->index != SCS_U32_NIL || c->value.type != SCS_VALUE_TYPE_string) {
		return;
	}
	const char *const v = c->value.value_string.value;
	if (!v) {
		return;
	}
	if (strcmp(c->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand) == 0) {
		edh_strncpy_z(g_truck_brand, sizeof(g_truck_brand), v);
	}
	else if (strcmp(c->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_name) == 0) {
		edh_strncpy_z(g_truck_model, sizeof(g_truck_model), v);
	}
}

SCSAPI_VOID telemetry_configuration(const scs_event_t event, const void *const event_info, const scs_context_t context)
{
	(void)event;
	(void)context;
	const struct scs_telemetry_configuration_t *const info = static_cast<const struct scs_telemetry_configuration_t *>(event_info);
	if (!info || !info->id) {
		return;
	}
	if (strcmp(info->id, SCS_TELEMETRY_CONFIG_job) == 0) {
		for (const scs_named_value_t *c = info->attributes; c && c->name; ++c) {
			merge_job_attribute(c);
		}
	}
	else if (strcmp(info->id, SCS_TELEMETRY_CONFIG_truck) == 0) {
		for (const scs_named_value_t *c = info->attributes; c && c->name; ++c) {
			merge_truck_attribute(c);
		}
	}
}

static const scs_named_value_t *gameplay_find_nil_attr(const scs_named_value_t *attrs, const char *const key)
{
	for (const scs_named_value_t *c = attrs; c && c->name; ++c) {
		if (strcmp(c->name, key) == 0 && c->index == SCS_U32_NIL) {
			return c;
		}
	}
	return NULL;
}

static bool gameplay_get_s64(const scs_named_value_t *attrs, const char *const key, scs_s64_t *const out)
{
	const scs_named_value_t *const c = gameplay_find_nil_attr(attrs, key);
	if (c && c->value.type == SCS_VALUE_TYPE_s64) {
		*out = c->value.value_s64.value;
		return true;
	}
	return false;
}

static bool gameplay_get_float(const scs_named_value_t *attrs, const char *const key, float *const out)
{
	const scs_named_value_t *const c = gameplay_find_nil_attr(attrs, key);
	if (c && c->value.type == SCS_VALUE_TYPE_float) {
		*out = c->value.value_float.value;
		return true;
	}
	return false;
}

/**
 * Parsed from the game's Documents\\…\\config.cfg (Gameplay: miles vs km; displayed currency).
 * Vanilla indices: https://modding.scssoft.com/wiki/Documentation/Engine/Config_variables/g_currency
 */
struct EdhGameUiPrefs {
	bool use_miles;
	bool has_mph;
	int currency_index;
	bool has_currency;
};

static void edh_read_game_ui_prefs_from_config(EdhGameUiPrefs *const prefs)
{
	if (!prefs) {
		return;
	}
	memset(prefs, 0, sizeof(*prefs));
	prefs->currency_index = 0;

	char base[512];
	if (!edh_get_game_profile_dir(base, sizeof(base))) {
		return;
	}
	char path[640];
#ifdef _WIN32
	if (sprintf_s(path, sizeof(path), "%s\\config.cfg", base) < 0) {
		return;
	}
#else
	if (snprintf(path, sizeof(path), "%s/config.cfg", base) >= (int)sizeof(path)) {
		return;
	}
#endif
	FILE *const f = fopen(path, "rb");
	if (!f) {
		return;
	}
	char line[2048];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		if (strstr(line, "g_mph") != NULL) {
			const char *const q = strchr(line, '"');
			if (q) {
				const int v = atoi(q + 1);
				prefs->has_mph = true;
				prefs->use_miles = (v != 0);
			}
		}
		else if (strstr(line, "g_currency") != NULL) {
			const char *const q = strchr(line, '"');
			if (q) {
				prefs->currency_index = atoi(q + 1);
				prefs->has_currency = true;
			}
		}
	}
	fclose(f);
}

static const char *edh_currency_prefix(const EdhGameUiPrefs *const prefs)
{
	const bool ats = (g_game_id[0] && strcmp(g_game_id, SCS_GAME_ID_ATS) == 0);
	if (prefs && prefs->has_currency) {
		if (ats) {
			switch (prefs->currency_index) {
			case 0:
				return "$";
			case 1:
				return "€";
			default:
				return "$";
			}
		}
		switch (prefs->currency_index) {
		case 0:
			return "€";
		case 1:
			return "CHF ";
		case 2:
			return "Kč ";
		case 3:
			return "£";
		case 4:
			return "PLN ";
		case 5:
			return "Ft ";
		case 6:
			return "DKK ";
		case 7:
			return "SEK ";
		case 8:
			return "NOK ";
		default:
			return "€";
		}
	}
	return ats ? "$" : "€";
}

static void replace_all(std::string *const s, const char *const needle, const char *const repl)
{
	if (!s || !needle || !repl) {
		return;
	}
	const size_t nlen = strlen(needle);
	const size_t rlen = strlen(repl);
	size_t pos = 0;
	while ((pos = s->find(needle, pos)) != std::string::npos) {
		s->replace(pos, nlen, repl);
		pos += rlen;
	}
}

static void append_job_delivered_line(const scs_named_value_t *const attrs)
{
	scs_s64_t revenue = 0;
	float distance_km = 0.f;
	const bool has_revenue = attrs && gameplay_get_s64(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_revenue, &revenue);
	const bool has_distance = attrs && gameplay_get_float(attrs, SCS_TELEMETRY_GAMEPLAY_EVENT_ATTRIBUTE_distance_km, &distance_km);

	EdhGameUiPrefs ui_prefs;
	edh_read_game_ui_prefs_from_config(&ui_prefs);

	char dist_str[64];
	char money_str[96];
	if (has_distance) {
		float display_num = distance_km;
		const char *unit_suffix = " km";
		if (ui_prefs.has_mph && ui_prefs.use_miles) {
			display_num = distance_km * 0.621371f;
			unit_suffix = " mi";
		}
#if defined(_WIN32)
		sprintf_s(dist_str, sizeof(dist_str), "%.2f%s", display_num, unit_suffix);
#else
		snprintf(dist_str, sizeof(dist_str), "%.2f%s", display_num, unit_suffix);
#endif
	}
	else {
		edh_strncpy_z(dist_str, sizeof(dist_str), "?");
	}
	if (has_revenue) {
		const char *const cur = edh_currency_prefix(&ui_prefs);
#if defined(_WIN32)
		sprintf_s(money_str, sizeof(money_str), "%s%" SCS_PF_S64, cur, revenue);
#else
		snprintf(money_str, sizeof(money_str), "%s%" SCS_PF_S64, cur, revenue);
#endif
	}
	else {
		edh_strncpy_z(money_str, sizeof(money_str), "?");
	}

	char truck_line[384];
	if (g_truck_brand[0] && g_truck_model[0]) {
#if defined(_WIN32)
		sprintf_s(truck_line, sizeof(truck_line), "%s %s", g_truck_brand, g_truck_model);
#else
		snprintf(truck_line, sizeof(truck_line), "%s %s", g_truck_brand, g_truck_model);
#endif
	}
	else if (g_truck_brand[0]) {
		edh_strncpy_z(truck_line, sizeof(truck_line), g_truck_brand);
	}
	else if (g_truck_model[0]) {
		edh_strncpy_z(truck_line, sizeof(truck_line), g_truck_model);
	}
	else {
		edh_strncpy_z(truck_line, sizeof(truck_line), "?");
	}

	const char *const pos_start = g_source_city[0] ? g_source_city : "?";
	const char *const pos_end = g_destination_city[0] ? g_destination_city : "?";
	const char *const cargo = g_cargo[0] ? g_cargo : "?";

	const unsigned char *emb_ptr = NULL;
	size_t emb_len = 0;
	edh_pick_embed_template(&emb_ptr, &emb_len);
	std::string out(reinterpret_cast<const char *>(emb_ptr), emb_len);
	replace_all(&out, "{cfg.color}", g_cfg_color);
	replace_all(&out, "{cfg.player}", g_cfg_player);
	char tsbuf[40];
	format_timestamp_iso8601_utc(tsbuf, sizeof(tsbuf));
	replace_all(&out, "{timestamp}", tsbuf);
	replace_all(&out, "{position_start}", pos_start);
	replace_all(&out, "{position_end}", pos_end);
	replace_all(&out, "{distance}", dist_str);
	replace_all(&out, "{truck}", truck_line);
	replace_all(&out, "{cargo}", cargo);
	replace_all(&out, "{gains}", money_str);

	const bool posted = edh_post_discord_webhook_json(out);
	if (g_discord_webhook[0] != '\0') {
		if (posted) {
			edh_plugin_log(SCS_LOG_TYPE_message, "edh_plugin: sent job completion payload to Discord webhook.");
		}
		else {
			edh_plugin_log(SCS_LOG_TYPE_warning, "edh_plugin: failed to send job completion payload to Discord webhook.");
		}
	}
	else {
		edh_plugin_log(SCS_LOG_TYPE_message, "edh_plugin: job completed; discord.webhook not set — nothing sent.");
	}
}

SCSAPI_VOID telemetry_gameplay_event(const scs_event_t event, const void *const event_info, const scs_context_t context)
{
	(void)event;
	(void)context;
	const struct scs_telemetry_gameplay_event_t *const info = static_cast<const struct scs_telemetry_gameplay_event_t *>(event_info);
	if (info && info->id) {
		if (strcmp(info->id, SCS_TELEMETRY_GAMEPLAY_EVENT_job_delivered) == 0) {
			append_job_delivered_line(info->attributes);
		}
	}
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
	if (version != SCS_TELEMETRY_VERSION_1_01) {
		debug_log_fmt("scs_telemetry_init: unsupported API version (got 0x%x, need 1.01)", static_cast<unsigned>(version));
		return SCS_RESULT_unsupported;
	}

	const scs_telemetry_init_params_v101_t *const version_params = static_cast<const scs_telemetry_init_params_v101_t *>(params);
	game_log = version_params->common.log;

	g_game_id[0] = '\0';
	if (version_params->common.game_id) {
		edh_strncpy_z(g_game_id, sizeof(g_game_id), version_params->common.game_id);
	}

	ensure_edh_webhook_cfg();
	load_edh_webhook_cfg();

	edh_plugin_log_fmt(
		SCS_LOG_TYPE_message,
		"edh_plugin: telemetry init game=%s id=%s ver=%u.%u",
		version_params->common.game_name ? version_params->common.game_name : "?",
		version_params->common.game_id ? version_params->common.game_id : "?",
		static_cast<unsigned>(SCS_GET_MAJOR_VERSION(version_params->common.game_version)),
		static_cast<unsigned>(SCS_GET_MINOR_VERSION(version_params->common.game_version))
	);

	if (version_params->register_for_event(SCS_TELEMETRY_EVENT_configuration, telemetry_configuration, NULL) != SCS_RESULT_ok) {
		edh_plugin_log(
			SCS_LOG_TYPE_warning,
			"edh_plugin: register_for_event(configuration) failed — job/truck fields may stay unknown"
		);
	}

	if (version_params->register_for_event(SCS_TELEMETRY_EVENT_gameplay, telemetry_gameplay_event, NULL) != SCS_RESULT_ok) {
		edh_plugin_log(SCS_LOG_TYPE_error, "edh_plugin: register_for_event(gameplay) failed");
		return SCS_RESULT_generic_error;
	}

	edh_plugin_log(SCS_LOG_TYPE_message, "edh_plugin: registered configuration + gameplay events; EDH_webhook.log still records delivery webhook status");
	return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
	edh_plugin_log(SCS_LOG_TYPE_message, "edh_plugin: shutdown");
	game_log = NULL;
}

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE module, DWORD reason_for_call, LPVOID reserved)
{
	(void)module;
	(void)reserved;
	if (reason_for_call == DLL_PROCESS_ATTACH) {
		edh_plugin_log(SCS_LOG_TYPE_message, "edh_plugin: DLL loaded (telemetry init follows)");
	}
	else if (reason_for_call == DLL_PROCESS_DETACH) {
		edh_plugin_log(SCS_LOG_TYPE_message, "edh_plugin: DLL unloaded");
	}
	return TRUE;
}
#endif

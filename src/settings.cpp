// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings.hpp"

#include "credential-vault.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <charconv>
#include <limits>
#include <string>
#include <system_error>

namespace easy_multistream {
namespace {

constexpr char kSection[] = "EasyMultistream";
constexpr char kSchemaVersion[] = "SchemaVersion";
constexpr char kYouTubeEnabled[] = "YouTubeEnabled";
constexpr char kYouTubeServerUrl[] = "YouTubeServerUrl";

// Early development builds never intentionally persist secrets, but remove these
// known plaintext field names defensively so they cannot survive a profile save
// or export. The values are deliberately never read into plugin-owned memory.
constexpr const char *kUnsupportedPlaintextKeys[] = {
	"StreamKey",
	"YouTubeStreamKey",
	"Key",
};

bool parseSchemaVersion(config_t *config, std::uint64_t &schema) noexcept
{
	const char *rawSchema = config_get_string(config, kSection, kSchemaVersion);
	if (rawSchema == nullptr) {
		return false;
	}

	try {
		const std::string value(rawSchema);
		if (value.empty()) {
			return false;
		}

		const char *first = value.data();
		const char *last = first + value.size();
		const auto parsed = std::from_chars(first, last, schema, 10);
		return parsed.ec == std::errc{} && parsed.ptr == last;
	} catch (...) {
		return false;
	}
}

bool parseYouTubeEnabled(config_t *config, bool &enabled) noexcept
{
	const char *rawEnabled = config_get_string(config, kSection, kYouTubeEnabled);
	if (rawEnabled == nullptr) {
		return false;
	}

	const std::string_view value(rawEnabled);
	if (value == "true") {
		enabled = true;
		return true;
	}
	if (value == "false") {
		enabled = false;
		return true;
	}
	return false;
}

struct ConfigValueSnapshot {
	bool present = false;
	std::string value;
};

ConfigValueSnapshot captureValue(config_t *config, const char *name)
{
	ConfigValueSnapshot snapshot;
	snapshot.present = config_has_user_value(config, kSection, name);
	if (!snapshot.present) {
		return snapshot;
	}

	const char *value = config_get_string(config, kSection, name);
	snapshot.value = value != nullptr ? value : "";
	return snapshot;
}

bool hasRtmpsScheme(std::string_view serverUrl) noexcept
{
	constexpr std::string_view kScheme = "rtmps://";
	// OBS 32.2.2's rtmp_custom service recognizes RTMPS with an exact,
	// case-sensitive prefix comparison. Accepting an upper-case variant here
	// would make the service classify the URL as plain RTMP instead.
	return serverUrl.size() >= kScheme.size() && serverUrl.substr(0, kScheme.size()) == kScheme;
}

bool isUrlPathCharacter(unsigned char value) noexcept
{
	return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
	       (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_' || value == '~';
}

bool isValidUtf8(std::string_view value) noexcept
{
	if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		return false;
	}

	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr,
					 0) != 0;
}

bool hasValidHostname(std::string_view host) noexcept
{
	if (host.empty()) {
		return false;
	}

	if (host.front() == '[') {
		return false;
	}

	for (const unsigned char value : host) {
		if (value <= 0x20 || value == 0x7f || value == '@' || value == '/' || value == '\\' || value == '%' ||
		    value == '[' || value == ']' || value == '?' || value == '#') {
			return false;
		}
	}
	return true;
}

bool isYouTubeRtmpsHostname(std::string_view host) noexcept
{
	constexpr std::string_view suffix = ".rtmps.youtube.com";
	if (host.size() <= suffix.size()) {
		return false;
	}

	const std::size_t suffixStart = host.size() - suffix.size();
	for (std::size_t index = 0; index < suffix.size(); ++index) {
		unsigned char value = static_cast<unsigned char>(host[suffixStart + index]);
		if (value >= 'A' && value <= 'Z') {
			value = static_cast<unsigned char>(value - 'A' + 'a');
		}
		if (value != static_cast<unsigned char>(suffix[index])) {
			return false;
		}
	}

	const std::string_view endpoint = host.substr(0, suffixStart);
	if (endpoint.empty() || endpoint.front() == '-' || endpoint.back() == '-') {
		return false;
	}
	for (const unsigned char value : endpoint) {
		if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
		      (value >= '0' && value <= '9') || value == '-')) {
			return false;
		}
	}
	return true;
}

YouTubeServerUrlValidationError validateServerAuthority(std::string_view authority) noexcept
{
	if (authority.empty()) {
		return YouTubeServerUrlValidationError::MissingHostname;
	}
	if (authority.find('@') != std::string_view::npos) {
		return YouTubeServerUrlValidationError::UserInfoNotAllowed;
	}

	std::string_view host = authority;
	if (authority.front() == '[') {
		return YouTubeServerUrlValidationError::UnsupportedHostname;
	}

	const std::size_t firstColon = authority.find(':');
	if (firstColon != std::string_view::npos) {
		if (firstColon != authority.rfind(':')) {
			return YouTubeServerUrlValidationError::InvalidPort;
		}
		host = authority.substr(0, firstColon);
		if (authority.substr(firstColon + 1) != "443") {
			return YouTubeServerUrlValidationError::InvalidPort;
		}
	}

	if (!hasValidHostname(host)) {
		return YouTubeServerUrlValidationError::MissingHostname;
	}
	return isYouTubeRtmpsHostname(host) ? YouTubeServerUrlValidationError::None
					    : YouTubeServerUrlValidationError::UnsupportedHostname;
}

bool isValidServerUrlValue(const std::string &serverUrl) noexcept
{
	return serverUrl.empty() || validateYouTubeServerUrl(serverUrl) == YouTubeServerUrlValidationError::None;
}

void restoreValue(config_t *config, const char *name, const ConfigValueSnapshot &snapshot) noexcept
{
	if (snapshot.present) {
		config_set_string(config, kSection, name, snapshot.value.c_str());
	} else {
		config_remove_value(config, kSection, name);
	}
}

} // namespace

SettingsLoadResult loadProfileSettings(config_t *config) noexcept
{
	if (config == nullptr) {
		return {};
	}

	try {
		const bool hasSchema = config_has_user_value(config, kSection, kSchemaVersion);
		const bool hasEnabled = config_has_user_value(config, kSection, kYouTubeEnabled);
		const bool hasServerUrl = config_has_user_value(config, kSection, kYouTubeServerUrl);
		if (!hasSchema && !hasEnabled && !hasServerUrl) {
			return {{}, SettingsLoadStatus::Defaults, kSettingsSchemaVersion};
		}

		std::uint64_t schema = 0;
		if (!hasSchema || !parseSchemaVersion(config, schema) || schema == 0) {
			return {{}, SettingsLoadStatus::InvalidSchema, schema};
		}
		if (schema > kSettingsSchemaVersion) {
			return {{}, SettingsLoadStatus::UnsupportedFutureSchema, schema};
		}

		Settings settings;
		if (!hasEnabled || !parseYouTubeEnabled(config, settings.youtubeEnabled)) {
			return {{}, SettingsLoadStatus::InvalidSchema, schema};
		}

		const char *rawServerUrl = hasServerUrl ? config_get_string(config, kSection, kYouTubeServerUrl) : nullptr;
		if (!hasServerUrl || rawServerUrl == nullptr || rawServerUrl[0] == '\0') {
			return {settings, SettingsLoadStatus::SetupRequired, schema};
		}

		settings.youtubeServerUrl = rawServerUrl;
		if (validateYouTubeServerUrl(settings.youtubeServerUrl) != YouTubeServerUrlValidationError::None) {
			return {{}, SettingsLoadStatus::InvalidSchema, schema};
		}
		return {std::move(settings), SettingsLoadStatus::Loaded, schema};
	} catch (...) {
		return {{}, SettingsLoadStatus::Unavailable, 0};
	}
}

void writeProfileSettings(config_t *config, const Settings &settings) noexcept
{
	if (config == nullptr) {
		return;
	}

	config_set_uint(config, kSection, kSchemaVersion, kSettingsSchemaVersion);
	config_set_bool(config, kSection, kYouTubeEnabled, settings.youtubeEnabled);
	if (isValidServerUrlValue(settings.youtubeServerUrl)) {
		config_set_string(config, kSection, kYouTubeServerUrl, settings.youtubeServerUrl.c_str());
	}
	for (const char *plaintextKey : kUnsupportedPlaintextKeys) {
		config_remove_value(config, kSection, plaintextKey);
	}
}

int saveProfileSettings(config_t *config, const Settings &settings) noexcept
{
	if (config == nullptr) {
		return CONFIG_ERROR;
	}
	if (!isValidServerUrlValue(settings.youtubeServerUrl)) {
		return CONFIG_ERROR;
	}

	ConfigValueSnapshot schemaSnapshot;
	ConfigValueSnapshot enabledSnapshot;
	ConfigValueSnapshot serverUrlSnapshot;
	try {
		schemaSnapshot = captureValue(config, kSchemaVersion);
		enabledSnapshot = captureValue(config, kYouTubeEnabled);
		serverUrlSnapshot = captureValue(config, kYouTubeServerUrl);
	} catch (...) {
		return CONFIG_ERROR;
	}

	writeProfileSettings(config, settings);
	const int result = config_save_safe(config, "tmp", nullptr);
	if (result != CONFIG_SUCCESS) {
		restoreValue(config, kSchemaVersion, schemaSnapshot);
		restoreValue(config, kYouTubeEnabled, enabledSnapshot);
		restoreValue(config, kYouTubeServerUrl, serverUrlSnapshot);
	}
	return result;
}

StreamKeyValidationError validateYouTubeStreamKey(std::string_view streamKey) noexcept
{
	if (streamKey.empty()) {
		return StreamKeyValidationError::Empty;
	}
	if (streamKey.size() > kMaxCredentialSecretBytes ||
	    streamKey.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		return StreamKeyValidationError::TooLong;
	}

	for (const unsigned char value : streamKey) {
		if (value <= 0x20 || value == 0x7f) {
			return StreamKeyValidationError::WhitespaceOrControlCharacter;
		}
	}

	const int length = static_cast<int>(streamKey.size());
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, streamKey.data(), length, nullptr, 0) == 0) {
		return StreamKeyValidationError::InvalidUtf8;
	}

	return StreamKeyValidationError::None;
}

YouTubeServerUrlValidationError validateYouTubeServerUrl(std::string_view serverUrl) noexcept
{
	if (serverUrl.empty()) {
		return YouTubeServerUrlValidationError::Empty;
	}
	for (const unsigned char value : serverUrl) {
		if (value == 0) {
			return YouTubeServerUrlValidationError::EmbeddedNull;
		}
	}
	if (serverUrl.size() > kMaxYouTubeServerUrlBytes) {
		return YouTubeServerUrlValidationError::TooLong;
	}
	for (const unsigned char value : serverUrl) {
		if (value <= 0x20 || value == 0x7f) {
			return YouTubeServerUrlValidationError::WhitespaceOrControlCharacter;
		}
	}
	if (!isValidUtf8(serverUrl)) {
		return YouTubeServerUrlValidationError::InvalidUtf8;
	}
	if (!hasRtmpsScheme(serverUrl)) {
		return YouTubeServerUrlValidationError::InvalidScheme;
	}

	constexpr std::size_t kSchemeLength = std::string_view("rtmps://").size();
	const std::size_t authorityEnd = serverUrl.find_first_of("/?#", kSchemeLength);
	const std::size_t authorityLength = authorityEnd == std::string_view::npos ? serverUrl.size() - kSchemeLength
										 : authorityEnd - kSchemeLength;
	const std::string_view authority = serverUrl.substr(kSchemeLength, authorityLength);
	const auto authorityError = validateServerAuthority(authority);
	if (authorityError != YouTubeServerUrlValidationError::None) {
		return authorityError;
	}

	if (authorityEnd == std::string_view::npos) {
		return YouTubeServerUrlValidationError::InvalidPath;
	}
	if (serverUrl[authorityEnd] == '?') {
		return YouTubeServerUrlValidationError::QueryNotAllowed;
	}
	if (serverUrl[authorityEnd] == '#') {
		return YouTubeServerUrlValidationError::FragmentNotAllowed;
	}

	const std::size_t pathEnd = serverUrl.find_first_of("?#", authorityEnd);
	if (pathEnd != std::string_view::npos) {
		return serverUrl[pathEnd] == '?' ? YouTubeServerUrlValidationError::QueryNotAllowed
							 : YouTubeServerUrlValidationError::FragmentNotAllowed;
	}
	const std::string_view path = serverUrl.substr(authorityEnd);
	if (path.size() <= 1 || path.front() != '/') {
		return YouTubeServerUrlValidationError::InvalidPath;
	}
	if (path.find('/', 1) != std::string_view::npos) {
		return YouTubeServerUrlValidationError::InvalidPath;
	}
	for (const unsigned char value : path.substr(1)) {
		if (!isUrlPathCharacter(value)) {
			return YouTubeServerUrlValidationError::InvalidPath;
		}
	}

	return YouTubeServerUrlValidationError::None;
}

} // namespace easy_multistream

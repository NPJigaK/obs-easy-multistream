// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings.hpp"

#include <charconv>
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

} // namespace easy_multistream

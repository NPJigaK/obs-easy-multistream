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

#include <limits>

namespace easy_multistream {
namespace {

constexpr char kSection[] = "EasyMultistream";
constexpr char kSchemaVersion[] = "SchemaVersion";
constexpr char kYouTubeEnabled[] = "YouTubeEnabled";

// Early development builds never intentionally persist secrets, but remove these
// known plaintext field names defensively so they cannot survive a profile save
// or export. The values are deliberately never read into plugin-owned memory.
constexpr const char *kUnsupportedPlaintextKeys[] = {
	"StreamKey",
	"YouTubeStreamKey",
	"Key",
};

} // namespace

SettingsLoadResult loadProfileSettings(config_t *config) noexcept
{
	if (config == nullptr) {
		return {};
	}

	const bool hasSchema = config_has_user_value(config, kSection, kSchemaVersion);
	const bool hasEnabled = config_has_user_value(config, kSection, kYouTubeEnabled);
	if (!hasSchema && !hasEnabled) {
		return {{}, SettingsLoadStatus::Defaults, kSettingsSchemaVersion};
	}

	const std::uint64_t schema = hasSchema ? config_get_uint(config, kSection, kSchemaVersion)
					       : kSettingsSchemaVersion;
	if (schema == 0) {
		return {{}, SettingsLoadStatus::InvalidSchema, schema};
	}
	if (schema > kSettingsSchemaVersion) {
		return {{}, SettingsLoadStatus::UnsupportedFutureSchema, schema};
	}

	Settings settings;
	settings.youtubeEnabled = config_get_bool(config, kSection, kYouTubeEnabled);
	return {settings, SettingsLoadStatus::Loaded, schema};
}

void writeProfileSettings(config_t *config, const Settings &settings) noexcept
{
	if (config == nullptr) {
		return;
	}

	config_set_uint(config, kSection, kSchemaVersion, kSettingsSchemaVersion);
	config_set_bool(config, kSection, kYouTubeEnabled, settings.youtubeEnabled);
	for (const char *plaintextKey : kUnsupportedPlaintextKeys) {
		config_remove_value(config, kSection, plaintextKey);
	}
}

int saveProfileSettings(config_t *config, const Settings &settings) noexcept
{
	if (config == nullptr) {
		return CONFIG_ERROR;
	}

	writeProfileSettings(config, settings);
	return config_save_safe(config, "tmp", nullptr);
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

} // namespace easy_multistream

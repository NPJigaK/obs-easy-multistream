// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <util/config-file.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace easy_multistream {

inline constexpr std::uint64_t kSettingsSchemaVersion = 2;
inline constexpr std::size_t kMaxYouTubeServerUrlBytes = 2048U;

struct Settings {
	bool youtubeEnabled = false;
	std::string youtubeServerUrl;
};

enum class SettingsLoadStatus {
	Loaded,
	Defaults,
	SetupRequired,
	InvalidSchema,
	UnsupportedFutureSchema,
	Unavailable,
};

struct SettingsLoadResult {
	Settings settings;
	SettingsLoadStatus status = SettingsLoadStatus::Unavailable;
	std::uint64_t sourceSchemaVersion = 0;
};

enum class StreamKeyValidationError {
	None,
	Empty,
	TooLong,
	WhitespaceOrControlCharacter,
	InvalidUtf8,
};

SettingsLoadResult loadProfileSettings(config_t *config) noexcept;
void writeProfileSettings(config_t *config, const Settings &settings) noexcept;
int saveProfileSettings(config_t *config, const Settings &settings) noexcept;

StreamKeyValidationError validateYouTubeStreamKey(std::string_view streamKey) noexcept;

enum class YouTubeServerUrlValidationError {
	None,
	Empty,
	TooLong,
	EmbeddedNull,
	WhitespaceOrControlCharacter,
	InvalidUtf8,
	InvalidScheme,
	MissingHostname,
	UnsupportedHostname,
	UserInfoNotAllowed,
	InvalidPort,
	QueryNotAllowed,
	FragmentNotAllowed,
	InvalidPath,
};

YouTubeServerUrlValidationError validateYouTubeServerUrl(std::string_view serverUrl) noexcept;

} // namespace easy_multistream

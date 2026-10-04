// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-account-selection.hpp"
#include "youtube-destination.hpp"

#include <util/config-file.h>

#include <cstdint>
#include <optional>
#include <string>

namespace easy_multistream {

inline constexpr std::uint64_t kSettingsSchemaVersion = 4;

struct Settings {
	bool youtubeEnabled = false;
	YouTubeConnectionMode youtubeConnectionMode = YouTubeConnectionMode::Manual;
	std::string youtubeServerUrl = kDefaultYouTubeServerUrl;
	std::optional<YouTubeAccountSelection> youtubeAccountSelection;
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

SettingsLoadResult loadProfileSettings(config_t *config) noexcept;
void writeProfileSettings(config_t *config, const Settings &settings) noexcept;
int saveProfileSettings(config_t *config, const Settings &settings) noexcept;

} // namespace easy_multistream

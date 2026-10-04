// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings.hpp"

#include <array>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace easy_multistream {
namespace {

constexpr char kSection[] = "EasyMultistream";
constexpr char kSchemaVersion[] = "SchemaVersion";
constexpr char kYouTubeEnabled[] = "YouTubeEnabled";
constexpr char kConnectionMode[] = "ConnectionMode";
constexpr char kYouTubeServerUrl[] = "YouTubeServerUrl";
constexpr char kYouTubeChannelId[] = "YouTubeChannelId";
constexpr char kYouTubeChannelLabel[] = "YouTubeChannelLabel";
constexpr char kYouTubeStreamId[] = "YouTubeStreamId";
constexpr char kYouTubeStreamLabel[] = "YouTubeStreamLabel";
constexpr char kManualMode[] = "manual";
constexpr char kAccountMode[] = "account";

constexpr std::array<const char *, 8> kPersistedKeys = {
	kSchemaVersion,    kYouTubeEnabled,      kConnectionMode,  kYouTubeServerUrl,
	kYouTubeChannelId, kYouTubeChannelLabel, kYouTubeStreamId, kYouTubeStreamLabel,
};

constexpr std::array<const char *, 8> kUnsupportedPlaintextKeys = {
	"StreamKey",   "YouTubeStreamKey",  "Key",          "RefreshToken",
	"AccessToken", "AuthorizationCode", "CodeVerifier", "StreamName",
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

bool parseConnectionMode(config_t *config, YouTubeConnectionMode &mode) noexcept
{
	const char *rawMode = config_get_string(config, kSection, kConnectionMode);
	if (rawMode == nullptr) {
		return false;
	}

	const std::string_view value(rawMode);
	if (value == kManualMode) {
		mode = YouTubeConnectionMode::Manual;
		return true;
	}
	if (value == kAccountMode) {
		mode = YouTubeConnectionMode::Account;
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

void restoreValue(config_t *config, const char *name, const ConfigValueSnapshot &snapshot) noexcept
{
	if (snapshot.present) {
		config_set_string(config, kSection, name, snapshot.value.c_str());
	} else {
		config_remove_value(config, kSection, name);
	}
}

bool anyPersistedValue(config_t *config) noexcept
{
	for (const char *key : kPersistedKeys) {
		if (config_has_user_value(config, kSection, key)) {
			return true;
		}
	}
	return false;
}

bool removeUnsupportedPlaintextValues(config_t *config) noexcept
{
	bool removed = false;
	for (const char *plaintextKey : kUnsupportedPlaintextKeys) {
		if (config_has_user_value(config, kSection, plaintextKey)) {
			config_remove_value(config, kSection, plaintextKey);
			removed = true;
		}
	}
	return removed;
}

enum class SelectionLoadState {
	Absent,
	Loaded,
	Invalid,
};

struct SelectionLoadResult {
	SelectionLoadState state = SelectionLoadState::Absent;
	YouTubeAccountSelection selection;
};

SelectionLoadResult loadAccountSelection(config_t *config)
{
	const std::array<const char *, 4> keys = {
		kYouTubeChannelId,
		kYouTubeChannelLabel,
		kYouTubeStreamId,
		kYouTubeStreamLabel,
	};

	std::size_t present = 0;
	for (const char *key : keys) {
		if (config_has_user_value(config, kSection, key)) {
			++present;
		}
	}
	if (present == 0) {
		return {};
	}
	if (present != keys.size()) {
		return {SelectionLoadState::Invalid, {}};
	}

	const char *channelId = config_get_string(config, kSection, kYouTubeChannelId);
	const char *channelLabel = config_get_string(config, kSection, kYouTubeChannelLabel);
	const char *streamId = config_get_string(config, kSection, kYouTubeStreamId);
	const char *streamLabel = config_get_string(config, kSection, kYouTubeStreamLabel);
	if (channelId == nullptr || channelLabel == nullptr || streamId == nullptr || streamLabel == nullptr) {
		return {SelectionLoadState::Invalid, {}};
	}

	YouTubeAccountSelection selection{channelId, channelLabel, streamId, streamLabel};
	if (validateYouTubeAccountSelection(selection) != YouTubeAccountSelectionValidationError::None) {
		return {SelectionLoadState::Invalid, {}};
	}
	return {SelectionLoadState::Loaded, std::move(selection)};
}

bool canSave(const Settings &settings) noexcept
{
	if (settings.youtubeConnectionMode != YouTubeConnectionMode::Manual &&
	    settings.youtubeConnectionMode != YouTubeConnectionMode::Account) {
		return false;
	}
	// The manual destination is deliberately not user-configurable.  Account
	// mode obtains its destination from the YouTube API and never consumes this
	// compatibility field.
	if (settings.youtubeConnectionMode == YouTubeConnectionMode::Manual &&
	    settings.youtubeServerUrl != kDefaultYouTubeServerUrl) {
		return false;
	}
	if (settings.youtubeAccountSelection.has_value() &&
	    validateYouTubeAccountSelection(*settings.youtubeAccountSelection) !=
		    YouTubeAccountSelectionValidationError::None) {
		return false;
	}
	// Account mode without a selection is a valid, durable setup-required
	// state. Disconnecting or importing a profile must not silently switch it
	// back to the manual-key path.
	return true;
}

const char *serializedMode(YouTubeConnectionMode mode) noexcept
{
	return mode == YouTubeConnectionMode::Account ? kAccountMode : kManualMode;
}

} // namespace

SettingsLoadResult loadProfileSettings(config_t *config) noexcept
{
	if (config == nullptr) {
		return {};
	}

	try {
		if (!anyPersistedValue(config)) {
			return {{}, SettingsLoadStatus::Defaults, kSettingsSchemaVersion};
		}

		const bool hasSchema = config_has_user_value(config, kSection, kSchemaVersion);
		const bool hasEnabled = config_has_user_value(config, kSection, kYouTubeEnabled);
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

		if (schema >= 3 && !parseConnectionMode(config, settings.youtubeConnectionMode)) {
			return {{}, SettingsLoadStatus::InvalidSchema, schema};
		}

		// Schema 1-3 stored a user-entered URL.  It was easy to paste the RTMP
		// address shown by Studio even though the plugin only accepted RTMPS.  The
		// standard secure ingestion endpoint is now an internal invariant, so old,
		// missing, or malformed URL values are ignored rather than blocking setup.
		settings.youtubeServerUrl = kDefaultYouTubeServerUrl;

		if (settings.youtubeConnectionMode == YouTubeConnectionMode::Account) {
			settings.youtubeServerUrl.clear();
			SelectionLoadResult selection = loadAccountSelection(config);
			if (selection.state == SelectionLoadState::Invalid) {
				return {{}, SettingsLoadStatus::InvalidSchema, schema};
			}
			if (selection.state == SelectionLoadState::Absent) {
				return {std::move(settings), SettingsLoadStatus::SetupRequired, schema};
			}
			settings.youtubeAccountSelection = std::move(selection.selection);
			return {std::move(settings), SettingsLoadStatus::Loaded, schema};
		}

		if (schema >= 3) {
			// A damaged inactive account selection must not disable a valid manual
			// destination. A later successful save removes incomplete fields.
			SelectionLoadResult selection = loadAccountSelection(config);
			if (selection.state == SelectionLoadState::Loaded) {
				settings.youtubeAccountSelection = std::move(selection.selection);
			}
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
	removeUnsupportedPlaintextValues(config);
	if (!canSave(settings)) {
		return;
	}

	config_set_uint(config, kSection, kSchemaVersion, kSettingsSchemaVersion);
	config_set_bool(config, kSection, kYouTubeEnabled, settings.youtubeEnabled);
	config_set_string(config, kSection, kConnectionMode, serializedMode(settings.youtubeConnectionMode));
	if (settings.youtubeConnectionMode == YouTubeConnectionMode::Manual) {
		config_set_string(config, kSection, kYouTubeServerUrl, kDefaultYouTubeServerUrl);
	} else {
		config_remove_value(config, kSection, kYouTubeServerUrl);
	}

	if (settings.youtubeAccountSelection.has_value()) {
		const YouTubeAccountSelection &selection = *settings.youtubeAccountSelection;
		config_set_string(config, kSection, kYouTubeChannelId, selection.channelId.c_str());
		config_set_string(config, kSection, kYouTubeChannelLabel, selection.channelLabel.c_str());
		config_set_string(config, kSection, kYouTubeStreamId, selection.streamId.c_str());
		config_set_string(config, kSection, kYouTubeStreamLabel, selection.streamLabel.c_str());
	} else {
		config_remove_value(config, kSection, kYouTubeChannelId);
		config_remove_value(config, kSection, kYouTubeChannelLabel);
		config_remove_value(config, kSection, kYouTubeStreamId);
		config_remove_value(config, kSection, kYouTubeStreamLabel);
	}
}

int saveProfileSettings(config_t *config, const Settings &settings) noexcept
{
	if (config == nullptr) {
		return CONFIG_ERROR;
	}

	const bool removedPlaintext = removeUnsupportedPlaintextValues(config);
	if (!canSave(settings)) {
		// Reject the candidate settings, but do not leave a known plaintext
		// credential on disk merely because an unrelated value was invalid.
		if (removedPlaintext) {
			(void)config_save_safe(config, "tmp", nullptr);
		}
		return CONFIG_ERROR;
	}

	std::array<ConfigValueSnapshot, kPersistedKeys.size()> snapshots;
	try {
		for (std::size_t index = 0; index < kPersistedKeys.size(); ++index) {
			snapshots[index] = captureValue(config, kPersistedKeys[index]);
		}
	} catch (...) {
		return CONFIG_ERROR;
	}

	writeProfileSettings(config, settings);
	const int result = config_save_safe(config, "tmp", nullptr);
	if (result != CONFIG_SUCCESS) {
		for (std::size_t index = 0; index < kPersistedKeys.size(); ++index) {
			restoreValue(config, kPersistedKeys[index], snapshots[index]);
		}
	}
	return result;
}

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "onboarding-state.hpp"

#include <string>

namespace easy_multistream {
namespace {

constexpr char kSection[] = "EasyMultistream";
constexpr char kDockAutoShowHandled[] = "DockAutoShowHandled";

struct ConfigValueSnapshot {
	bool present = false;
	std::string value;
};

ConfigValueSnapshot captureValue(config_t *config)
{
	ConfigValueSnapshot snapshot;
	snapshot.present = config_has_user_value(config, kSection, kDockAutoShowHandled);
	if (!snapshot.present) {
		return snapshot;
	}

	const char *value = config_get_string(config, kSection, kDockAutoShowHandled);
	snapshot.value = value != nullptr ? value : "";
	return snapshot;
}

void restoreValue(config_t *config, const ConfigValueSnapshot &snapshot) noexcept
{
	if (snapshot.present) {
		config_set_string(config, kSection, kDockAutoShowHandled, snapshot.value.c_str());
	} else {
		config_remove_value(config, kSection, kDockAutoShowHandled);
	}
}

} // namespace

bool shouldAutoShowDock(config_t *userConfig) noexcept
{
	return userConfig != nullptr &&
	       !config_has_user_value(userConfig, kSection, kDockAutoShowHandled);
}

int markDockAutoShowHandled(config_t *userConfig) noexcept
{
	if (userConfig == nullptr) {
		return CONFIG_ERROR;
	}

	ConfigValueSnapshot snapshot;
	try {
		snapshot = captureValue(userConfig);
	} catch (...) {
		return CONFIG_ERROR;
	}

	config_set_bool(userConfig, kSection, kDockAutoShowHandled, true);
	const int result = config_save_safe(userConfig, "tmp", nullptr);
	if (result != CONFIG_SUCCESS) {
		restoreValue(userConfig, snapshot);
	}
	return result;
}

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-view.hpp"
#include "settings-controller.hpp"
#include "version.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QPointer>
#include <QString>
#include <QThread>

#include <exception>
#include <memory>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-easy-multistream", "en-US")

namespace {

constexpr char kDockId[] = "easy_multistream.dock";

struct PluginState {
	QPointer<easy_multistream::DockView> dock;
	std::unique_ptr<easy_multistream::SettingsController> settingsController;
	bool dockRegistered = false;
	bool callbackRegistered = false;
	bool exitSeen = false;
};

std::unique_ptr<PluginState> pluginState;

QString moduleText(const char *key)
{
	const char *value = obs_module_text(key);
	return QString::fromUtf8(value != nullptr ? value : key);
}

easy_multistream::DockText loadDockText()
{
	easy_multistream::DockText text;
	text.heading = moduleText("Dock.Heading");
	text.destinations = moduleText("Dock.Destinations");
	text.primaryName = moduleText("Dock.Primary");
	text.primaryStatus = moduleText("Dock.PrimaryStatus");
	text.youtubeName = moduleText("Dock.YouTube");
	text.setup = moduleText("Dock.Setup");
	text.profileLabel = moduleText("Dock.Profile");
	text.enableYouTube = moduleText("Dock.EnableYouTube");
	text.credentialLabel = moduleText("Dock.Credential.Label");
	text.streamKeyLabel = moduleText("Dock.StreamKey.Label");
	text.saveKey = moduleText("Dock.SaveKey");
	text.removeKey = moduleText("Dock.RemoveKey");
	text.keyPlaceholderMissing = moduleText("Dock.StreamKey.Placeholder.Missing");
	text.keyPlaceholderPresent = moduleText("Dock.StreamKey.Placeholder.Present");
	text.keyScope = moduleText("Dock.StreamKey.Scope");
	text.credentialMissing = moduleText("Dock.Credential.Missing");
	text.credentialPresent = moduleText("Dock.Credential.Present");
	text.credentialUnavailable = moduleText("Dock.Credential.Unavailable");
	text.youtubeDisabled = moduleText("Dock.YouTube.Disabled");
	text.youtubeReady = moduleText("Dock.YouTube.Ready");
	text.youtubeMissingKey = moduleText("Dock.YouTube.MissingKey");
	text.youtubeUnavailable = moduleText("Dock.YouTube.Unavailable");
	text.noticePreview = moduleText("Dock.Notice.Preview");
	text.noticeProfileSaved = moduleText("Dock.Notice.ProfileSaved");
	text.noticeKeySaved = moduleText("Dock.Notice.KeySaved");
	text.noticeKeyRemoved = moduleText("Dock.Notice.KeyRemoved");
	text.noticeMissingKey = moduleText("Dock.Notice.MissingKey");
	text.noticeKeyTooLong = moduleText("Dock.Notice.KeyTooLong");
	text.noticeKeyInvalidCharacters = moduleText("Dock.Notice.KeyInvalidCharacters");
	text.noticeKeyInvalidUtf8 = moduleText("Dock.Notice.KeyInvalidUtf8");
	text.noticeCredentialUnavailable = moduleText("Dock.Notice.CredentialUnavailable");
	text.noticeCredentialSaveFailed = moduleText("Dock.Notice.CredentialSaveFailed");
	text.noticeCredentialDeleteFailed = moduleText("Dock.Notice.CredentialDeleteFailed");
	text.noticeProfileUnavailable = moduleText("Dock.Notice.ProfileUnavailable");
	text.noticeProfileChanging = moduleText("Dock.Notice.ProfileChanging");
	text.noticeInvalidSettings = moduleText("Dock.Notice.InvalidSettings");
	text.noticeFutureSettings = moduleText("Dock.Notice.FutureSettings");
	text.noticeSettingsSaveFailed = moduleText("Dock.Notice.SettingsSaveFailed");
	text.noticeInternalError = moduleText("Dock.Notice.InternalError");
	text.removeKeyTitle = moduleText("Dock.RemoveKey.Title");
	text.removeKeyMessage = moduleText("Dock.RemoveKey.Message");
	return text;
}

void removeFrontendObjects(PluginState &state) noexcept;

void onFrontendEvent(enum obs_frontend_event event, void *privateData) noexcept
{
	auto *state = static_cast<PluginState *>(privateData);
	if (state == nullptr) {
		return;
	}

	if (event == OBS_FRONTEND_EVENT_EXIT) {
		state->exitSeen = true;
		removeFrontendObjects(*state);
		return;
	}

	if (state->settingsController == nullptr) {
		return;
	}

	try {
		switch (event) {
		case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
			state->settingsController->loadCurrentProfile();
			break;
		case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
			state->settingsController->beginProfileChange();
			break;
		case OBS_FRONTEND_EVENT_THEME_CHANGED:
			state->settingsController->refreshTheme();
			break;
		default:
			break;
		}
	} catch (const std::exception &error) {
		blog(LOG_ERROR, "[obs-easy-multistream] Frontend event handling failed: %s", error.what());
	} catch (...) {
		blog(LOG_ERROR, "[obs-easy-multistream] Frontend event handling failed");
	}
}

void removeFrontendObjects(PluginState &state) noexcept
{
	if (state.callbackRegistered) {
		obs_frontend_remove_event_callback(onFrontendEvent, &state);
		state.callbackRegistered = false;
	}

	if (state.settingsController != nullptr) {
		state.settingsController->shutdown();
	}

	if (state.dockRegistered) {
		obs_frontend_remove_dock(kDockId);
		state.dockRegistered = false;
	}

	state.dock.clear();
}

bool isUiThread() noexcept
{
	QCoreApplication *application = QCoreApplication::instance();
	return application != nullptr && application->thread() == QThread::currentThread();
}

} // namespace

const char *obs_module_description(void)
{
	return "A lightweight OBS Studio multistreaming plugin.";
}

bool obs_module_load(void)
{
	try {
		if (pluginState != nullptr) {
			blog(LOG_ERROR, "[obs-easy-multistream] Refusing duplicate module initialization");
			return false;
		}

		if (!isUiThread()) {
			blog(LOG_ERROR,
			     "[obs-easy-multistream] The frontend dock must be initialized on the Qt UI thread");
			return false;
		}

		auto state = std::make_unique<PluginState>();
		auto dock = std::make_unique<easy_multistream::DockView>(loadDockText());
		state->dock = dock.get();
		state->settingsController = std::make_unique<easy_multistream::SettingsController>(dock.get());

		const QByteArray dockTitle = moduleText("Dock.Title").toUtf8();
		if (!obs_frontend_add_dock_by_id(kDockId, dockTitle.constData(), dock.get())) {
			blog(LOG_ERROR, "[obs-easy-multistream] Failed to register the OBS dock");
			return false;
		}

		state->dockRegistered = true;
		(void)dock.release();
		pluginState = std::move(state);

		try {
			obs_frontend_add_event_callback(onFrontendEvent, pluginState.get());
			pluginState->callbackRegistered = true;
		} catch (...) {
			removeFrontendObjects(*pluginState);
			pluginState.reset();
			throw;
		}
		pluginState->settingsController->loadCurrentProfile();

		blog(LOG_INFO, "[obs-easy-multistream] Loaded version %s", easy_multistream::kVersion);
		return true;
	} catch (const std::exception &error) {
		if (pluginState != nullptr) {
			removeFrontendObjects(*pluginState);
			pluginState.reset();
		}
		blog(LOG_ERROR, "[obs-easy-multistream] Module initialization failed: %s", error.what());
	} catch (...) {
		if (pluginState != nullptr) {
			removeFrontendObjects(*pluginState);
			pluginState.reset();
		}
		blog(LOG_ERROR, "[obs-easy-multistream] Module initialization failed with an unknown exception");
	}

	return false;
}

void obs_module_unload(void)
{
	if (pluginState == nullptr) {
		return;
	}

	if (!pluginState->exitSeen) {
		removeFrontendObjects(*pluginState);
	}

	pluginState.reset();
	blog(LOG_INFO, "[obs-easy-multistream] Unloaded");
}

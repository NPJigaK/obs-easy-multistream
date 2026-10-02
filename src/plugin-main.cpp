// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-visibility.hpp"
#include "dock-view.hpp"
#include "onboarding-state.hpp"
#include "runtime-controller.hpp"
#include "settings-controller.hpp"
#include "version.hpp"
#include "windows-credential-vault.hpp"
#include "youtube-output-adapter.hpp"

#include <obs.hpp>
#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QPointer>
#include <QMetaObject>
#include <QString>
#include <QThread>
#include <QWidget>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-easy-multistream", "en-US")

namespace {

constexpr char kDockId[] = "easy_multistream.dock";

class WindowVisibilityFilter final : public QObject {
public:
	explicit WindowVisibilityFilter(std::function<void()> callback) : callback_(std::move(callback)) {}

protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() == QEvent::Show || event->type() == QEvent::WindowStateChange) {
			try {
				callback_();
			} catch (...) {
				blog(LOG_ERROR, "[obs-easy-multistream] Dock visibility callback failed");
			}
		}
		return false;
	}

private:
	std::function<void()> callback_;
};

struct PluginState {
	PluginState() : credentialVault(credentialApi) {}

	QPointer<easy_multistream::DockView> dock;
	std::unique_ptr<easy_multistream::SettingsController> settingsController;
	QObject runtimeContext;
	easy_multistream::NativeWinCredentialApi credentialApi;
	easy_multistream::WindowsCredentialVault credentialVault;
	std::unique_ptr<easy_multistream::YouTubeOutputAdapter> youtubeAdapter;
	OBSOutputAutoRelease nativeOutput;
	OBSSignal nativeStartingSignal;
	std::optional<easy_multistream::NativeLease> nativeProbeLease;
	easy_multistream::NativeDestination nativeDestination = easy_multistream::NativeDestination::Unknown;
	std::unique_ptr<easy_multistream::RuntimeController> runtime;
	bool dockRegistered = false;
	bool dockAutoShowScheduled = false;
	QPointer<QWidget> dockAutoShowWindow;
	std::unique_ptr<WindowVisibilityFilter> dockAutoShowFilter;
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
	text.gettingStarted = moduleText("Dock.GettingStarted");
	text.gettingStartedBody = moduleText("Dock.GettingStarted.Body");
	text.destinations = moduleText("Dock.Destinations");
	text.primaryName = moduleText("Dock.Primary");
	text.primaryStatus = moduleText("Dock.PrimaryStatus");
	text.nativeTwitch = moduleText("Dock.Native.Twitch");
	text.nativeYouTube = moduleText("Dock.Native.YouTube");
	text.nativeNotStreaming = moduleText("Dock.Native.NotStreaming");
	text.nativeStarting = moduleText("Dock.Native.Starting");
	text.nativeStreaming = moduleText("Dock.Native.Streaming");
	text.nativeStopping = moduleText("Dock.Native.Stopping");
	text.nativeUnavailable = moduleText("Dock.Native.Unavailable");
	text.youtubeName = moduleText("Dock.YouTube");
	text.setup = moduleText("Dock.Setup");
	text.profileLabel = moduleText("Dock.Profile");
	text.enableYouTube = moduleText("Dock.EnableYouTube");
	text.serverUrlLabel = moduleText("Dock.ServerUrl.Label");
	text.serverUrlPlaceholder = moduleText("Dock.ServerUrl.Placeholder");
	text.saveServerUrl = moduleText("Dock.SaveServerUrl");
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
	text.youtubeMissingServerUrl = moduleText("Dock.YouTube.MissingServerUrl");
	text.youtubeMissingKey = moduleText("Dock.YouTube.MissingKey");
	text.youtubeUnavailable = moduleText("Dock.YouTube.Unavailable");
	text.youtubeRequiresTwitch = moduleText("Dock.YouTube.RequiresTwitch");
	text.youtubeNotStreaming = moduleText("Dock.YouTube.NotStreaming");
	text.youtubeConnecting = moduleText("Dock.YouTube.Connecting");
	text.youtubeStreaming = moduleText("Dock.YouTube.Streaming");
	text.youtubeReconnecting = moduleText("Dock.YouTube.Reconnecting");
	text.youtubeStopping = moduleText("Dock.YouTube.Stopping");
	text.youtubeFailed = moduleText("Dock.YouTube.Failed");
	text.youtubeSetupRequired = moduleText("Dock.YouTube.SetupRequired");
	text.retryYouTube = moduleText("Dock.RetryYouTube");
	text.noticePreview = moduleText("Dock.Notice.Preview");
	text.noticeProfileSaved = moduleText("Dock.Notice.ProfileSaved");
	text.noticeServerUrlSaved = moduleText("Dock.Notice.ServerUrlSaved");
	text.noticeMissingServerUrl = moduleText("Dock.Notice.MissingServerUrl");
	text.noticeInvalidServerUrl = moduleText("Dock.Notice.InvalidServerUrl");
	text.noticeServerUrlTooLong = moduleText("Dock.Notice.ServerUrlTooLong");
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
	text.noticeYouTubeFailed = moduleText("Dock.Notice.YouTubeFailed");
	text.removeKeyTitle = moduleText("Dock.RemoveKey.Title");
	text.removeKeyMessage = moduleText("Dock.RemoveKey.Message");
	return text;
}

easy_multistream::NativeDestination currentNativeDestination() noexcept
{
	obs_service_t *service = obs_frontend_get_streaming_service();
	if (service == nullptr) {
		return easy_multistream::NativeDestination::Unknown;
	}

	const char *serviceId = obs_service_get_type(service);
	OBSDataAutoRelease settings(obs_service_get_settings(service));
	const char *provider = settings != nullptr ? obs_data_get_string(settings.Get(), "service") : nullptr;
	return easy_multistream::classifyNativeDestination(serviceId != nullptr ? serviceId : "",
											provider != nullptr ? provider : "");
}

void clearNativeStartingProbe(PluginState &state) noexcept
{
	state.nativeStartingSignal.Disconnect();
	state.nativeOutput = nullptr;
	state.nativeProbeLease.reset();
}

void onNativeOutputStarting(void *privateData, calldata_t *) noexcept
{
	auto *state = static_cast<PluginState *>(privateData);
	if (state == nullptr || state->runtime == nullptr || !state->nativeProbeLease.has_value()) {
		return;
	}
	// OBS emits the output's `starting` signal synchronously from the native
	// start path.  This marker must reach the controller before its queued
	// reconciliation runs, otherwise a synchronous native rejection could be
	// mistaken for a slow network start.
	state->runtime->onNativeOutputStarting(*state->nativeProbeLease);
}

void attachNativeStartingProbe(PluginState &state) noexcept
{
	clearNativeStartingProbe(state);
	if (state.runtime == nullptr) {
		return;
	}
	const auto snapshot = state.runtime->snapshot();
	if (!snapshot.nativeLease.has_value()) {
		return;
	}

	state.nativeOutput = obs_frontend_get_streaming_output();
	if (state.nativeOutput == nullptr) {
		return;
	}
	signal_handler_t *signalHandler = obs_output_get_signal_handler(state.nativeOutput.Get());
	if (signalHandler == nullptr) {
		state.nativeOutput = nullptr;
		return;
	}
	state.nativeProbeLease = snapshot.nativeLease;
	state.nativeStartingSignal.Connect(signalHandler, "starting", &onNativeOutputStarting, &state);
}

bool postToRuntime(QObject *context, std::function<void()> callback) noexcept
{
	if (context == nullptr || !callback) {
		return false;
	}
	try {
		return QMetaObject::invokeMethod(context,
					 [callback = std::move(callback)]() mutable {
						 try {
							 callback();
						 } catch (...) {
							 blog(LOG_ERROR,
							      "[obs-easy-multistream] Runtime callback failed");
						 }
					 },
					 Qt::QueuedConnection);
	} catch (...) {
		blog(LOG_ERROR, "[obs-easy-multistream] Failed to queue a runtime callback");
		return false;
	}
}

void scheduleDockAutoShow(PluginState &state) noexcept;

void stopDockAutoShowWatch(PluginState &state) noexcept
{
	if (state.dockAutoShowWindow != nullptr && state.dockAutoShowFilter != nullptr) {
		state.dockAutoShowWindow->removeEventFilter(state.dockAutoShowFilter.get());
	}
	state.dockAutoShowWindow.clear();
	state.dockAutoShowFilter.reset();
}

void watchForMainWindowVisibility(PluginState &state, QWidget *mainWindow) noexcept
{
	if (mainWindow == nullptr || state.exitSeen || state.dockAutoShowFilter != nullptr) {
		return;
	}

	try {
		PluginState *statePointer = &state;
		state.dockAutoShowFilter = std::make_unique<WindowVisibilityFilter>([statePointer]() noexcept {
			scheduleDockAutoShow(*statePointer);
		});
		state.dockAutoShowWindow = mainWindow;
		mainWindow->installEventFilter(state.dockAutoShowFilter.get());
	} catch (...) {
		state.dockAutoShowWindow.clear();
		state.dockAutoShowFilter.reset();
		blog(LOG_ERROR, "[obs-easy-multistream] Could not watch for the OBS window to become visible");
	}
}

void autoShowDockIfNeeded(PluginState &state) noexcept
{
	if (state.exitSeen || !state.dockRegistered) {
		return;
	}

	config_t *userConfig = obs_frontend_get_user_config();
	if (!easy_multistream::shouldAutoShowDock(userConfig)) {
		stopDockAutoShowWatch(state);
		return;
	}

	auto *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (mainWindow == nullptr || !mainWindow->isVisible() || mainWindow->isMinimized()) {
		// Never pull OBS out of the tray or steal focus.  The event filter
		// retries after the user makes the main window visible.
		watchForMainWindowVisibility(state, mainWindow);
		return;
	}
	if (!easy_multistream::showDockById(mainWindow, kDockId)) {
		blog(LOG_WARNING, "[obs-easy-multistream] Could not auto-show the dock on first use");
		return;
	}

	stopDockAutoShowWatch(state);
	if (easy_multistream::markDockAutoShowHandled(userConfig) != CONFIG_SUCCESS) {
		blog(LOG_WARNING, "[obs-easy-multistream] Could not save the first-use dock preference");
	}
}

void scheduleDockAutoShow(PluginState &state) noexcept
{
	if (state.dockAutoShowScheduled || state.exitSeen) {
		return;
	}

	state.dockAutoShowScheduled = true;
	PluginState *statePointer = &state;
	const bool queued = postToRuntime(&state.runtimeContext, [statePointer]() noexcept {
		statePointer->dockAutoShowScheduled = false;
		autoShowDockIfNeeded(*statePointer);
	});
	if (!queued) {
		state.dockAutoShowScheduled = false;
	}
}

void applyRuntimeSettings(PluginState &state, easy_multistream::Settings settings,
					  easy_multistream::CredentialDisplayState credentialState, bool profileChanged) noexcept
{
	if (state.runtime == nullptr) {
		return;
	}
	easy_multistream::RuntimeSettings runtimeSettings;
	runtimeSettings.nativeDestination = state.nativeDestination;
	runtimeSettings.youtubeEnabled = settings.youtubeEnabled;
	runtimeSettings.youtubeKeyAvailable = credentialState == easy_multistream::CredentialDisplayState::Present;
	runtimeSettings.youtubeServerUrl = std::move(settings.youtubeServerUrl);
	if (profileChanged) {
		state.runtime->onProfileChanged(std::move(runtimeSettings));
	} else {
		state.runtime->setSettings(std::move(runtimeSettings));
	}
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

	try {
		switch (event) {
		case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
			state->nativeDestination = currentNativeDestination();
			if (state->settingsController != nullptr) {
				state->settingsController->loadCurrentProfile(easy_multistream::DockNotice::Preview, true);
			}
			if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
				scheduleDockAutoShow(*state);
			}
			break;
		case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
			clearNativeStartingProbe(*state);
			if (state->runtime != nullptr) {
				state->runtime->onProfileChanging();
			}
			if (state->settingsController != nullptr) {
				state->settingsController->beginProfileChange();
			}
			break;
		case OBS_FRONTEND_EVENT_STREAMING_STARTING:
			state->nativeDestination = currentNativeDestination();
			if (state->runtime != nullptr) {
				state->runtime->onStreamingStarting(state->nativeDestination);
				// This must happen before OBS calls StartStreaming below the
				// frontend event callback.
				attachNativeStartingProbe(*state);
			}
			break;
		case OBS_FRONTEND_EVENT_STREAMING_STARTED:
			if (state->runtime != nullptr) {
				state->runtime->onStreamingStarted();
			}
			break;
		case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
			if (state->runtime != nullptr) {
				state->runtime->onStreamingStopping();
			}
			clearNativeStartingProbe(*state);
			break;
		case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
			if (state->runtime != nullptr) {
				state->runtime->onStreamingStopped();
			}
			clearNativeStartingProbe(*state);
			break;
		case OBS_FRONTEND_EVENT_THEME_CHANGED:
			if (state->settingsController != nullptr) {
				state->settingsController->refreshTheme();
			}
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
	stopDockAutoShowWatch(state);
	if (state.callbackRegistered) {
		obs_frontend_remove_event_callback(onFrontendEvent, &state);
		state.callbackRegistered = false;
	}

	clearNativeStartingProbe(state);
	if (state.runtime != nullptr) {
		state.runtime->onExit();
		// RuntimeController's destructor is the final callback barrier.  It
		// disables its weak sink and joins the adapter before the dock can be
		// removed or the plugin state can be destroyed.
		state.runtime.reset();
	}
	if (state.youtubeAdapter != nullptr) {
		state.youtubeAdapter->shutdown();
		state.youtubeAdapter.reset();
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
		PluginState *statePointer = state.get();
		state->youtubeAdapter = std::make_unique<easy_multistream::YouTubeOutputAdapter>();
		state->runtime = std::make_unique<easy_multistream::RuntimeController>(
			*state->youtubeAdapter,
			[statePointer]() noexcept -> easy_multistream::SecureBuffer {
				easy_multistream::CredentialReadResult result = statePointer->credentialVault.read();
				if (!result.result.succeeded()) {
					return {};
				}
				return std::move(result.secret);
			},
			[context = &state->runtimeContext](std::function<void()> callback) noexcept {
				(void)postToRuntime(context, std::move(callback));
			});
		const QPointer<easy_multistream::SettingsController> settingsGuard(state->settingsController.get());
		state->runtime->setSnapshotSink([settingsGuard](easy_multistream::SessionSnapshot snapshot) mutable {
			if (settingsGuard != nullptr) {
				settingsGuard->applyRuntimeSnapshot(std::move(snapshot));
			}
		});
		state->settingsController->setSettingsChangedHandler(
			[statePointer](easy_multistream::Settings settings,
					      easy_multistream::CredentialDisplayState credentialState,
					      bool profileChanged) noexcept {
				applyRuntimeSettings(*statePointer, std::move(settings), credentialState, profileChanged);
			});
		state->settingsController->setRetryYouTubeHandler([statePointer]() noexcept {
			if (statePointer->runtime != nullptr) {
				statePointer->runtime->retryYouTube();
			}
		});

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
		// OBS is still loading modules here, so rtmp_common may not be
		// registered yet.  FINISHED_LOADING performs the first destination
		// classification and replaces this brief Unknown snapshot.
		pluginState->settingsController->loadCurrentProfile(easy_multistream::DockNotice::Preview, true);

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

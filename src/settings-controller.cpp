// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings-controller.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <QByteArray>
#include <QDesktopServices>
#include <QUrl>

#include <stdexcept>
#include <string_view>
#include <utility>

namespace easy_multistream {
namespace {

constexpr char kYouTubeLiveDashboardUrl[] = "https://www.youtube.com/live_dashboard";

class QByteArrayWiper final {
public:
	explicit QByteArrayWiper(QByteArray &value) noexcept : value_(value) {}
	~QByteArrayWiper()
	{
		if (!value_.isEmpty()) {
			SecureZeroMemory(value_.data(), static_cast<std::size_t>(value_.size()));
		}
		value_.clear();
	}

	QByteArrayWiper(const QByteArrayWiper &) = delete;
	QByteArrayWiper &operator=(const QByteArrayWiper &) = delete;

private:
	QByteArray &value_;
};

DockNotice validationNotice(StreamKeyValidationError error) noexcept
{
	switch (error) {
	case StreamKeyValidationError::None:
		return DockNotice::Preview;
	case StreamKeyValidationError::Empty:
		return DockNotice::MissingKey;
	case StreamKeyValidationError::TooLong:
		return DockNotice::KeyTooLong;
	case StreamKeyValidationError::WhitespaceOrControlCharacter:
		return DockNotice::KeyInvalidCharacters;
	case StreamKeyValidationError::InvalidUtf8:
		return DockNotice::KeyInvalidUtf8;
	}

	return DockNotice::InternalError;
}

QString currentProfileName()
{
	char *rawName = obs_frontend_get_current_profile();
	if (rawName == nullptr) {
		return {};
	}

	try {
		const QString result = QString::fromUtf8(rawName);
		bfree(rawName);
		return result;
	} catch (...) {
		bfree(rawName);
		throw;
	}
}

} // namespace

SettingsController::SettingsController(DockView *view, QObject *parent)
	: QObject(parent),
	  view_(view),
	  credentialVault_(credentialApi_)
{
	if (view_ == nullptr) {
		throw std::invalid_argument("SettingsController requires a dock view");
	}

	view_->bindActions(
		this,
		[this](bool enabled) {
			try {
				handleEnabledChanged(enabled);
			} catch (...) {
				blog(LOG_ERROR,
				     "[obs-easy-multistream] Updating the profile setting failed unexpectedly");
					renderInternalError();
			}
		},
		[this](QByteArray streamKey) {
			try {
				handleSaveKey(std::move(streamKey));
			} catch (...) {
				blog(LOG_ERROR, "[obs-easy-multistream] Saving the credential failed unexpectedly");
				renderInternalError();
			}
		},
		[this]() {
			try {
				handleRemoveKey();
			} catch (...) {
				blog(LOG_ERROR, "[obs-easy-multistream] Removing the credential failed unexpectedly");
				 renderInternalError();
			}
		},
		[this]() {
			if (retryYouTubeHandler_) {
				retryYouTubeHandler_();
			}
		},
		[]() noexcept {
			bool opened = false;
			try {
				opened = QDesktopServices::openUrl(
					QUrl(QString::fromLatin1(kYouTubeLiveDashboardUrl)));
			} catch (...) {
				opened = false;
			}
			if (!opened) {
				blog(LOG_WARNING, "[obs-easy-multistream] Could not open YouTube Studio");
			}
		});
}

void SettingsController::loadCurrentProfile(DockNotice successNotice, bool profileChanged)
{
	if (closing_) {
		return;
	}
	++contextGeneration_;
	if (view_ != nullptr) {
		view_->cancelCredentialRemoval();
	}

	config_t *config = obs_frontend_get_profile_config();
	const SettingsLoadResult loaded = loadProfileSettings(config);
	settings_ = loaded.settings;
	profileName_ = currentProfileName();
	loadStatus_ = loaded.status;
	settingsEditable_ = loadStatus_ == SettingsLoadStatus::Loaded || loadStatus_ == SettingsLoadStatus::Defaults ||
			    loadStatus_ == SettingsLoadStatus::SetupRequired;
	credentialState_ = refreshCredentialState();

	DockNotice notice = successNotice;
	switch (loadStatus_) {
	case SettingsLoadStatus::Loaded:
	case SettingsLoadStatus::Defaults:
		break;
	case SettingsLoadStatus::SetupRequired:
		break;
	case SettingsLoadStatus::InvalidSchema:
		notice = DockNotice::InvalidSettings;
		break;
	case SettingsLoadStatus::UnsupportedFutureSchema:
		notice = DockNotice::FutureSettings;
		break;
	case SettingsLoadStatus::Unavailable:
		notice = DockNotice::ProfileUnavailable;
		break;
	}

	const bool mayReplaceSuccessNotice = notice == DockNotice::Preview || notice == DockNotice::ProfileSaved;
	if (mayReplaceSuccessNotice && settingsEditable_ && settings_.youtubeEnabled &&
	    credentialState_ == CredentialDisplayState::Missing) {
		notice = DockNotice::MissingKey;
	} else if (mayReplaceSuccessNotice && settingsEditable_ && settings_.youtubeEnabled &&
		   credentialState_ == CredentialDisplayState::Unavailable) {
		notice = DockNotice::CredentialUnavailable;
	}
	notifySettingsChanged(profileChanged);
	render(notice);
}

void SettingsController::beginProfileChange()
{
	if (closing_) {
		return;
	}
	++contextGeneration_;

	settings_ = {};
	profileName_.clear();
	loadStatus_ = SettingsLoadStatus::Unavailable;
	settingsEditable_ = false;
	credentialState_ = CredentialDisplayState::Unavailable;
	if (view_ != nullptr) {
		view_->cancelCredentialRemoval();
		view_->clearStreamKey();
	}
	render(DockNotice::ProfileChanging);
}

void SettingsController::refreshTheme()
{
	if (!closing_ && view_ != nullptr) {
		view_->refreshTheme();
	}
}

void SettingsController::setSettingsChangedHandler(SettingsChangedHandler handler)
{
	settingsChangedHandler_ = std::move(handler);
}

void SettingsController::setRetryYouTubeHandler(std::function<void()> handler)
{
	retryYouTubeHandler_ = std::move(handler);
}

void SettingsController::applyRuntimeSnapshot(SessionSnapshot snapshot)
{
	if (closing_) {
		return;
	}
	hasRuntimeSnapshot_ = true;
	runtimeSnapshot_ = std::move(snapshot);
	render(lastNotice_);
}

void SettingsController::shutdown() noexcept
{
	closing_ = true;
	++contextGeneration_;
	settingsEditable_ = false;
	settingsChangedHandler_ = {};
	retryYouTubeHandler_ = {};
	hasRuntimeSnapshot_ = false;
	if (view_ != nullptr) {
		try {
			view_->cancelCredentialRemoval();
			view_->clearStreamKey();
		} catch (...) {
			blog(LOG_ERROR, "[obs-easy-multistream] Failed to clear the secret editor during shutdown");
		}
	}
	view_.clear();
}

void SettingsController::handleEnabledChanged(bool enabled)
{
	if (closing_) {
		return;
	}

	config_t *config = obs_frontend_get_profile_config();
	const SettingsLoadResult loaded = loadProfileSettings(config);
	if (loaded.status == SettingsLoadStatus::Unavailable) {
		loadCurrentProfile(DockNotice::ProfileUnavailable);
		return;
	}
	if (loaded.status != SettingsLoadStatus::Loaded && loaded.status != SettingsLoadStatus::Defaults &&
	    loaded.status != SettingsLoadStatus::SetupRequired) {
		loadCurrentProfile(loaded.status == SettingsLoadStatus::UnsupportedFutureSchema
					   ? DockNotice::FutureSettings
					   : DockNotice::InvalidSettings);
		return;
	}
	settings_ = loaded.settings;
	profileName_ = currentProfileName();
	loadStatus_ = loaded.status;
	settingsEditable_ = true;
	credentialState_ = refreshCredentialState();

	if (enabled && credentialState_ != CredentialDisplayState::Present) {
		render(credentialState_ == CredentialDisplayState::Unavailable ? DockNotice::CredentialUnavailable
									       : DockNotice::MissingKey);
		return;
	}

	Settings desired = settings_;
	desired.youtubeEnabled = enabled;
	if (saveProfileSettings(config, desired) != CONFIG_SUCCESS) {
		blog(LOG_WARNING, "[obs-easy-multistream] Failed to save the current OBS profile settings");
		loadCurrentProfile(DockNotice::SettingsSaveFailed);
		return;
	}

	loadCurrentProfile(DockNotice::ProfileSaved);
}

void SettingsController::handleSaveKey(QByteArray streamKeyBytes)
{
	QByteArrayWiper wipeStreamKey(streamKeyBytes);
	if (closing_) {
		return;
	}

	config_t *config = obs_frontend_get_profile_config();
	const SettingsLoadResult loaded = loadProfileSettings(config);
	if (loaded.status == SettingsLoadStatus::Unavailable) {
		loadCurrentProfile(DockNotice::ProfileUnavailable);
		return;
	}
	if (loaded.status != SettingsLoadStatus::Loaded && loaded.status != SettingsLoadStatus::Defaults &&
	    loaded.status != SettingsLoadStatus::SetupRequired) {
		loadCurrentProfile(loaded.status == SettingsLoadStatus::UnsupportedFutureSchema
					   ? DockNotice::FutureSettings
					   : DockNotice::InvalidSettings);
		return;
	}

	settings_ = loaded.settings;
	profileName_ = currentProfileName();
	loadStatus_ = loaded.status;
	settingsEditable_ = true;

	const std::string_view streamKey(streamKeyBytes.constData(), static_cast<std::size_t>(streamKeyBytes.size()));
	const StreamKeyValidationError validation = validateYouTubeStreamKey(streamKey);
	if (validation != StreamKeyValidationError::None) {
		credentialState_ = refreshCredentialState();
		render(validationNotice(validation));
		return;
	}

	const CredentialResult writeResult = credentialVault_.write(streamKey);
	if (!writeResult.succeeded()) {
		blog(LOG_WARNING, "[obs-easy-multistream] Credential write failed with Windows error %lu",
		     static_cast<unsigned long>(writeResult.nativeError));
		credentialState_ = refreshCredentialState();
		render(DockNotice::CredentialSaveFailed);
		return;
	}

	if (view_ != nullptr) {
		view_->clearStreamKey();
	}
	loadCurrentProfile(DockNotice::KeySaved);
}

void SettingsController::handleRemoveKey()
{
	if (closing_ || view_ == nullptr) {
		return;
	}

	config_t *config = obs_frontend_get_profile_config();
	const SettingsLoadResult loaded = loadProfileSettings(config);
	if (loaded.status != SettingsLoadStatus::Loaded && loaded.status != SettingsLoadStatus::Defaults &&
	    loaded.status != SettingsLoadStatus::SetupRequired) {
		DockNotice notice = DockNotice::InvalidSettings;
		if (loaded.status == SettingsLoadStatus::UnsupportedFutureSchema) {
			notice = DockNotice::FutureSettings;
		} else if (loaded.status == SettingsLoadStatus::Unavailable) {
			notice = DockNotice::ProfileUnavailable;
		}
		loadCurrentProfile(notice);
		return;
	}
	settings_ = loaded.settings;
	profileName_ = currentProfileName();
	loadStatus_ = loaded.status;
	settingsEditable_ = true;
	credentialState_ = refreshCredentialState();
	const std::uint64_t generation = contextGeneration_;
	const QString profileName = profileName_;
	view_->requestCredentialRemovalConfirmation(this, [this, generation, profileName]() {
		try {
			commitRemoveKey(generation, profileName);
		} catch (...) {
			blog(LOG_ERROR, "[obs-easy-multistream] Removing the credential failed unexpectedly");
			renderInternalError();
		}
	});
}

void SettingsController::commitRemoveKey(std::uint64_t generation, QString profileName)
{
	if (closing_ || view_ == nullptr || generation != contextGeneration_) {
		return;
	}
	if (currentProfileName() != profileName) {
		loadCurrentProfile();
		return;
	}

	const SettingsLoadResult loaded = loadProfileSettings(obs_frontend_get_profile_config());
	if (loaded.status != SettingsLoadStatus::Loaded && loaded.status != SettingsLoadStatus::Defaults &&
	    loaded.status != SettingsLoadStatus::SetupRequired) {
		loadCurrentProfile(loaded.status == SettingsLoadStatus::UnsupportedFutureSchema
					   ? DockNotice::FutureSettings
				   : loaded.status == SettingsLoadStatus::Unavailable ? DockNotice::ProfileUnavailable
										      : DockNotice::InvalidSettings);
		return;
	}

	const CredentialResult eraseResult = credentialVault_.erase();
	if (!eraseResult.succeeded()) {
		blog(LOG_WARNING, "[obs-easy-multistream] Credential deletion failed with Windows error %lu",
		     static_cast<unsigned long>(eraseResult.nativeError));
		credentialState_ = refreshCredentialState();
		render(DockNotice::CredentialDeleteFailed);
		return;
	}

	if (view_ != nullptr) {
		view_->clearStreamKey();
	}
	loadCurrentProfile(DockNotice::KeyRemoved);
}

void SettingsController::render(DockNotice notice)
{
	if (closing_ || view_ == nullptr) {
		return;
	}

	DockState state;
	lastNotice_ = notice;
	state.profileName = profileName_;
	state.settingsEditable = settingsEditable_;
	state.youtubeEnabled = settings_.youtubeEnabled;
	state.credential = credentialState_;
	state.notice = notice;
	state.runtimeAvailable = hasRuntimeSnapshot_;
	if (hasRuntimeSnapshot_) {
		state.session = runtimeSnapshot_;
	}
	view_->applyState(state);
}

void SettingsController::notifySettingsChanged(bool profileChanged)
{
	if (closing_ || !settingsChangedHandler_) {
		return;
	}
	try {
		settingsChangedHandler_(settings_, credentialState_, profileChanged);
	} catch (...) {
		blog(LOG_ERROR, "[obs-easy-multistream] Runtime settings notification failed");
	}
}

void SettingsController::renderInternalError() noexcept
{
	try {
		render(DockNotice::InternalError);
	} catch (...) {
		blog(LOG_ERROR, "[obs-easy-multistream] Failed to update the settings UI after an internal error");
	}
}

CredentialDisplayState SettingsController::refreshCredentialState() noexcept
{
	const CredentialStatus status = credentialVault_.status();
	switch (status.state) {
	case CredentialState::Present:
		return CredentialDisplayState::Present;
	case CredentialState::Missing:
		return CredentialDisplayState::Missing;
	case CredentialState::Unavailable:
		return CredentialDisplayState::Unavailable;
	}

	return CredentialDisplayState::Unavailable;
}

} // namespace easy_multistream

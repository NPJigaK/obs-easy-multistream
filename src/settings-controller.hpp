// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "dock-view.hpp"
#include "settings.hpp"
#include "windows-credential-vault.hpp"

#include <QObject>
#include <QPointer>

#include <cstdint>
#include <functional>

namespace easy_multistream {

class SettingsController final : public QObject {
public:
	using SettingsChangedHandler = std::function<void(Settings, CredentialDisplayState, bool)>;

	explicit SettingsController(DockView *view, QObject *parent = nullptr);

	void loadCurrentProfile(DockNotice successNotice = DockNotice::Preview, bool profileChanged = false);
	void beginProfileChange();
	void refreshTheme();
	void shutdown() noexcept;
	void setSettingsChangedHandler(SettingsChangedHandler handler);
	void applyRuntimeSnapshot(SessionSnapshot snapshot);
	void setRetryYouTubeHandler(std::function<void()> handler);

private:
	void handleEnabledChanged(bool enabled);
	void handleSaveKey(QByteArray streamKey);
	void handleRemoveKey();
	void commitRemoveKey(std::uint64_t generation, QString profileName);
	void render(DockNotice notice);
	void notifySettingsChanged(bool profileChanged);
	void renderInternalError() noexcept;
	CredentialDisplayState refreshCredentialState() noexcept;

	QPointer<DockView> view_;
	NativeWinCredentialApi credentialApi_;
	WindowsCredentialVault credentialVault_;
	Settings settings_;
	QString profileName_;
	SettingsLoadStatus loadStatus_ = SettingsLoadStatus::Unavailable;
	CredentialDisplayState credentialState_ = CredentialDisplayState::Unavailable;
	bool settingsEditable_ = false;
	bool closing_ = false;
	std::uint64_t contextGeneration_ = 0;
	DockNotice lastNotice_ = DockNotice::Preview;
	bool hasRuntimeSnapshot_ = false;
	SessionSnapshot runtimeSnapshot_;
	SettingsChangedHandler settingsChangedHandler_;
	std::function<void()> retryYouTubeHandler_;
};

} // namespace easy_multistream

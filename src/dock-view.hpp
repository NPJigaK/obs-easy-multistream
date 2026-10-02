// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "session-coordinator.hpp"

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QEvent;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMessageBox;
class QObject;
class QPushButton;

namespace easy_multistream {

struct DockText {
	QString heading;
	QString gettingStarted;
	QString gettingStartedBody;
	QString destinations;
	QString primaryName;
	QString primaryStatus;
	QString nativeTwitch;
	QString nativeYouTube;
	QString nativeNotStreaming;
	QString nativeStarting;
	QString nativeStreaming;
	QString nativeStopping;
	QString nativeUnavailable;
	QString youtubeName;
	QString setup;
	QString profileLabel;
	QString enableYouTube;
	QString serverUrlLabel;
	QString serverUrlPlaceholder;
	QString saveServerUrl;
	QString credentialLabel;
	QString streamKeyLabel;
	QString saveKey;
	QString removeKey;
	QString keyPlaceholderMissing;
	QString keyPlaceholderPresent;
	QString keyScope;
	QString credentialMissing;
	QString credentialPresent;
	QString credentialUnavailable;
	QString youtubeDisabled;
	QString youtubeReady;
	QString youtubeMissingServerUrl;
	QString youtubeMissingKey;
	QString youtubeUnavailable;
	QString youtubeRequiresTwitch;
	QString youtubeNotStreaming;
	QString youtubeConnecting;
	QString youtubeStreaming;
	QString youtubeReconnecting;
	QString youtubeStopping;
	QString youtubeFailed;
	QString youtubeSetupRequired;
	QString retryYouTube;
	QString noticePreview;
	QString noticeProfileSaved;
	QString noticeServerUrlSaved;
	QString noticeMissingServerUrl;
	QString noticeInvalidServerUrl;
	QString noticeServerUrlTooLong;
	QString noticeKeySaved;
	QString noticeKeyRemoved;
	QString noticeMissingKey;
	QString noticeKeyTooLong;
	QString noticeKeyInvalidCharacters;
	QString noticeKeyInvalidUtf8;
	QString noticeCredentialUnavailable;
	QString noticeCredentialSaveFailed;
	QString noticeCredentialDeleteFailed;
	QString noticeProfileUnavailable;
	QString noticeProfileChanging;
	QString noticeInvalidSettings;
	QString noticeFutureSettings;
	QString noticeSettingsSaveFailed;
	QString noticeInternalError;
	QString noticeYouTubeFailed;
	QString removeKeyTitle;
	QString removeKeyMessage;
};

enum class CredentialDisplayState {
	Missing,
	Present,
	Unavailable,
};

enum class DockNotice {
	Preview,
	ProfileSaved,
	ServerUrlSaved,
	MissingServerUrl,
	InvalidServerUrl,
	ServerUrlTooLong,
	KeySaved,
	KeyRemoved,
	MissingKey,
	KeyTooLong,
	KeyInvalidCharacters,
	KeyInvalidUtf8,
	CredentialUnavailable,
	CredentialSaveFailed,
	CredentialDeleteFailed,
	ProfileUnavailable,
	ProfileChanging,
	InvalidSettings,
	FutureSettings,
	SettingsSaveFailed,
	InternalError,
};

struct DockState {
	QString profileName;
	QString youtubeServerUrl;
	bool settingsEditable = false;
	bool youtubeEnabled = false;
	CredentialDisplayState credential = CredentialDisplayState::Unavailable;
	DockNotice notice = DockNotice::Preview;
	bool runtimeAvailable = false;
	SessionSnapshot session;
};

class DockView final : public QWidget {
public:
	using EnabledHandler = std::function<void(bool)>;
	using SaveServerUrlHandler = std::function<void(QByteArray)>;
	using SaveKeyHandler = std::function<void(QByteArray)>;
	using RemoveKeyHandler = std::function<void()>;
	using RetryYouTubeHandler = std::function<void()>;

	explicit DockView(DockText text, QWidget *parent = nullptr);

	void bindActions(QObject *context, EnabledHandler enabledHandler, SaveServerUrlHandler saveServerUrlHandler,
			 SaveKeyHandler saveKeyHandler, RemoveKeyHandler removeKeyHandler,
			 RetryYouTubeHandler retryYouTubeHandler);
	void applyState(const DockState &state);
	void clearStreamKey();
	void requestCredentialRemovalConfirmation(QObject *context, RemoveKeyHandler confirmedHandler);
	void cancelCredentialRemoval();
	void refreshTheme();

private:
	bool event(QEvent *event) override;
	QString noticeText(DockNotice notice) const;

	DockText text_;
	QGroupBox *gettingStartedGroup_ = nullptr;
	QLabel *profileNameLabel_ = nullptr;
	QLabel *primaryDestinationLabel_ = nullptr;
	QLabel *primaryStatusLabel_ = nullptr;
	QLabel *youtubeStatusLabel_ = nullptr;
	QCheckBox *youtubeEnabledCheckBox_ = nullptr;
	QLineEdit *serverUrlEdit_ = nullptr;
	QPushButton *saveServerUrlButton_ = nullptr;
	QLabel *credentialStatusLabel_ = nullptr;
	QLineEdit *streamKeyEdit_ = nullptr;
	QPushButton *saveKeyButton_ = nullptr;
	QPushButton *removeKeyButton_ = nullptr;
	QPushButton *retryYouTubeButton_ = nullptr;
	QLabel *noticeLabel_ = nullptr;
	QPointer<QMessageBox> credentialRemovalDialog_;
};

} // namespace easy_multistream

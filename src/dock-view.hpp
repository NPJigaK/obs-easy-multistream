// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <QByteArray>
#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>

class QCheckBox;
class QEvent;
class QLabel;
class QLineEdit;
class QMessageBox;
class QObject;
class QPushButton;

namespace easy_multistream {

struct DockText {
	QString heading;
	QString destinations;
	QString primaryName;
	QString primaryStatus;
	QString youtubeName;
	QString setup;
	QString profileLabel;
	QString enableYouTube;
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
	QString youtubeMissingKey;
	QString youtubeUnavailable;
	QString noticePreview;
	QString noticeProfileSaved;
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
	bool settingsEditable = false;
	bool youtubeEnabled = false;
	CredentialDisplayState credential = CredentialDisplayState::Unavailable;
	DockNotice notice = DockNotice::Preview;
};

class DockView final : public QWidget {
public:
	using EnabledHandler = std::function<void(bool)>;
	using SaveKeyHandler = std::function<void(QByteArray)>;
	using RemoveKeyHandler = std::function<void()>;

	explicit DockView(DockText text, QWidget *parent = nullptr);

	void bindActions(QObject *context, EnabledHandler enabledHandler, SaveKeyHandler saveKeyHandler,
			 RemoveKeyHandler removeKeyHandler);
	void applyState(const DockState &state);
	void clearStreamKey();
	void requestCredentialRemovalConfirmation(QObject *context, RemoveKeyHandler confirmedHandler);
	void cancelCredentialRemoval();
	void refreshTheme();

private:
	bool event(QEvent *event) override;
	QString noticeText(DockNotice notice) const;

	DockText text_;
	QLabel *profileNameLabel_ = nullptr;
	QLabel *youtubeStatusLabel_ = nullptr;
	QCheckBox *youtubeEnabledCheckBox_ = nullptr;
	QLabel *credentialStatusLabel_ = nullptr;
	QLineEdit *streamKeyEdit_ = nullptr;
	QPushButton *saveKeyButton_ = nullptr;
	QPushButton *removeKeyButton_ = nullptr;
	QLabel *noticeLabel_ = nullptr;
	QPointer<QMessageBox> credentialRemovalDialog_;
};

} // namespace easy_multistream

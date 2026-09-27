// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-view.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>

#include <iostream>
#include <utility>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                           \
		if (!(expression)) {                                                                                     \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';             \
			++failures;                                                                                         \
		}                                                                                                          \
	} while (false)

easy_multistream::DockText testText()
{
	easy_multistream::DockText text;
	text.heading = QStringLiteral("Easy Multistream");
	text.destinations = QStringLiteral("Destinations");
	text.primaryName = QStringLiteral("Primary");
	text.primaryStatus = QStringLiteral("Managed by OBS");
	text.youtubeName = QStringLiteral("YouTube");
	text.setup = QStringLiteral("Setup");
	text.profileLabel = QStringLiteral("Profile");
	text.enableYouTube = QStringLiteral("Enable YouTube");
	text.credentialLabel = QStringLiteral("Key status");
	text.streamKeyLabel = QStringLiteral("Stream key");
	text.saveKey = QStringLiteral("Save key");
	text.removeKey = QStringLiteral("Remove");
	text.keyPlaceholderMissing = QStringLiteral("Paste key");
	text.keyPlaceholderPresent = QStringLiteral("Saved");
	text.keyScope = QStringLiteral("Shared across profiles");
	text.credentialMissing = QStringLiteral("Missing");
	text.credentialPresent = QStringLiteral("Present");
	text.credentialUnavailable = QStringLiteral("Unavailable");
	text.youtubeDisabled = QStringLiteral("Disabled");
	text.youtubeReady = QStringLiteral("Ready");
	text.youtubeMissingKey = QStringLiteral("Missing key");
	text.youtubeUnavailable = QStringLiteral("Unavailable");
	text.noticePreview = QStringLiteral("Preview");
	text.noticeProfileSaved = QStringLiteral("Profile saved");
	text.noticeKeySaved = QStringLiteral("Key saved");
	text.noticeKeyRemoved = QStringLiteral("Removed");
	text.noticeMissingKey = QStringLiteral("Missing key");
	text.noticeKeyTooLong = QStringLiteral("Too long");
	text.noticeKeyInvalidCharacters = QStringLiteral("Invalid characters");
	text.noticeKeyInvalidUtf8 = QStringLiteral("Invalid UTF-8");
	text.noticeCredentialUnavailable = QStringLiteral("Credential unavailable");
	text.noticeCredentialSaveFailed = QStringLiteral("Credential save failed");
	text.noticeCredentialDeleteFailed = QStringLiteral("Credential delete failed");
	text.noticeProfileUnavailable = QStringLiteral("Profile unavailable");
	text.noticeProfileChanging = QStringLiteral("Profile changing");
	text.noticeInvalidSettings = QStringLiteral("Invalid settings");
	text.noticeFutureSettings = QStringLiteral("Future settings");
	text.noticeSettingsSaveFailed = QStringLiteral("Settings save failed");
	text.noticeInternalError = QStringLiteral("Internal error");
	text.removeKeyTitle = QStringLiteral("Remove key?");
	text.removeKeyMessage = QStringLiteral("Confirm");
	return text;
}

template<typename T> T *requiredChild(easy_multistream::DockView &view, const char *name)
{
	T *child = view.findChild<T *>(QString::fromUtf8(name));
	CHECK(child != nullptr);
	return child;
}

void testStateAndActions()
{
	easy_multistream::DockView view(testText());
	auto *profile = requiredChild<QLabel>(view, "easyMultistreamProfileName");
	auto *youtubeStatus = requiredChild<QLabel>(view, "easyMultistreamYouTubeStatus");
	auto *credentialStatus = requiredChild<QLabel>(view, "easyMultistreamCredentialStatus");
	auto *enabled = requiredChild<QCheckBox>(view, "easyMultistreamYouTubeEnabled");
	auto *key = requiredChild<QLineEdit>(view, "easyMultistreamStreamKey");
	auto *save = requiredChild<QPushButton>(view, "easyMultistreamSaveKey");
	auto *remove = requiredChild<QPushButton>(view, "easyMultistreamRemoveKey");
	if (profile == nullptr || youtubeStatus == nullptr || credentialStatus == nullptr || enabled == nullptr ||
	    key == nullptr || save == nullptr || remove == nullptr) {
		return;
	}

	CHECK(key->echoMode() == QLineEdit::Password);
	CHECK(key->contextMenuPolicy() == Qt::NoContextMenu);
	CHECK(!key->dragEnabled());
	CHECK(!key->acceptDrops());
	CHECK(key->maxLength() == 2560);

	easy_multistream::DockState state;
	state.profileName = QStringLiteral("Gaming");
	state.settingsEditable = true;
	state.credential = easy_multistream::CredentialDisplayState::Missing;
	state.notice = easy_multistream::DockNotice::Preview;
	view.applyState(state);
	CHECK(profile->text() == QStringLiteral("Gaming"));
	CHECK(youtubeStatus->text() == QStringLiteral("Disabled"));
	CHECK(credentialStatus->text() == QStringLiteral("Missing"));
	CHECK(enabled->isEnabled());
	CHECK(key->isEnabled());
	CHECK(save->isEnabled());
	CHECK(!remove->isEnabled());

	QObject actionContext;
	bool enabledCalled = false;
	bool enabledValue = false;
	bool saveKeyCalled = false;
	bool removeCalled = false;
	QByteArray receivedKey;
	view.bindActions(
		&actionContext,
		[&](bool value) {
			enabledCalled = true;
			enabledValue = value;
		},
		[&](QByteArray streamKey) {
			saveKeyCalled = true;
			receivedKey = std::move(streamKey);
		},
		[&]() { removeCalled = true; });

	enabled->setChecked(true);
	CHECK(enabledCalled);
	CHECK(enabledValue);
	key->setText(QStringLiteral("test-key"));
	save->click();
	CHECK(saveKeyCalled);
	CHECK(receivedKey == QByteArray("test-key"));
	receivedKey.fill('\0');
	receivedKey.clear();

	enabledCalled = false;
	state.youtubeEnabled = true;
	state.credential = easy_multistream::CredentialDisplayState::Present;
	view.applyState(state);
	CHECK(!enabledCalled);
	CHECK(youtubeStatus->text() == QStringLiteral("Ready"));
	CHECK(remove->isEnabled());
	remove->click();
	CHECK(removeCalled);

	state.settingsEditable = false;
	state.notice = easy_multistream::DockNotice::FutureSettings;
	view.applyState(state);
	CHECK(!enabled->isEnabled());
	CHECK(!key->isEnabled());
	CHECK(!save->isEnabled());
	CHECK(!remove->isEnabled());
}

void testSecretEditorBlocksExportAndClearsWhenHidden()
{
	easy_multistream::DockView view(testText());
	auto *key = requiredChild<QLineEdit>(view, "easyMultistreamStreamKey");
	if (key == nullptr) {
		return;
	}

	easy_multistream::DockState state;
	state.settingsEditable = true;
	state.credential = easy_multistream::CredentialDisplayState::Missing;
	view.applyState(state);

	key->setText(QStringLiteral("do-not-copy"));
	key->selectAll();
	QApplication::clipboard()->setText(QStringLiteral("sentinel"));
	QKeyEvent copyEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
	QApplication::sendEvent(key, &copyEvent);
	CHECK(QApplication::clipboard()->text() == QStringLiteral("sentinel"));

	QKeyEvent cutEvent(QEvent::KeyPress, Qt::Key_X, Qt::ControlModifier);
	QApplication::sendEvent(key, &cutEvent);
	CHECK(key->text() == QStringLiteral("do-not-copy"));

	view.show();
	QApplication::processEvents();
	view.hide();
	QApplication::processEvents();
	CHECK(key->text().isEmpty());
}

void testCredentialRemovalConfirmationIsAsynchronousAndCancellable()
{
	easy_multistream::DockView view(testText());
	QObject actionContext;
	bool confirmed = false;
	view.requestCredentialRemovalConfirmation(&actionContext, [&]() { confirmed = true; });
	auto *dialog = requiredChild<QMessageBox>(view, "easyMultistreamRemoveKeyConfirmation");
	if (dialog == nullptr) {
		return;
	}

	dialog->button(QMessageBox::Yes)->click();
	QApplication::processEvents();
	CHECK(confirmed);

	confirmed = false;
	view.requestCredentialRemovalConfirmation(&actionContext, [&]() { confirmed = true; });
	view.cancelCredentialRemoval();
	QApplication::processEvents();
	CHECK(!confirmed);
}

} // namespace

int main(int argc, char **argv)
{
	QApplication application(argc, argv);
	testStateAndActions();
	testSecretEditorBlocksExportAndClearsWhenHidden();
	testCredentialRemovalConfirmationIsAsynchronousAndCancellable();

	if (failures != 0) {
		std::cerr << failures << " UI test(s) failed\n";
		return 1;
	}

	std::cout << "All UI tests passed\n";
	return 0;
}

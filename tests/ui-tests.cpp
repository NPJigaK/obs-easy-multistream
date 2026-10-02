// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-visibility.hpp"
#include "dock-view.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDockWidget>
#include <QGroupBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMainWindow>
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
	text.gettingStarted = QStringLiteral("Get started");
	text.gettingStartedBody = QStringLiteral("Complete the YouTube setup, then use OBS as usual.");
	text.destinations = QStringLiteral("Destinations");
	text.primaryName = QStringLiteral("Primary");
	text.primaryStatus = QStringLiteral("Managed by OBS");
	text.nativeTwitch = QStringLiteral("Twitch (OBS)");
	text.nativeYouTube = QStringLiteral("YouTube (OBS)");
	text.nativeNotStreaming = QStringLiteral("Not streaming");
	text.nativeStarting = QStringLiteral("Starting");
	text.nativeStreaming = QStringLiteral("Streaming");
	text.nativeStopping = QStringLiteral("Stopping");
	text.nativeUnavailable = QStringLiteral("Unavailable");
	text.youtubeName = QStringLiteral("YouTube");
	text.setup = QStringLiteral("Setup");
	text.showSettings = QStringLiteral("Change settings");
	text.hideSettings = QStringLiteral("Hide settings");
	text.profileLabel = QStringLiteral("Profile");
	text.enableYouTube = QStringLiteral("Enable YouTube");
	text.serverUrlLabel = QStringLiteral("Server URL");
	text.serverUrlPlaceholder = QStringLiteral("Paste URL");
	text.saveServerUrl = QStringLiteral("Save URL");
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
	text.youtubeMissingServerUrl = QStringLiteral("Missing URL");
	text.youtubeMissingKey = QStringLiteral("Missing key");
	text.youtubeUnavailable = QStringLiteral("Unavailable");
	text.youtubeRequiresTwitch = QStringLiteral("Set OBS to Twitch");
	text.youtubeNotStreaming = QStringLiteral("Not streaming");
	text.youtubeConnecting = QStringLiteral("Connecting");
	text.youtubeStreaming = QStringLiteral("Streaming");
	text.youtubeReconnecting = QStringLiteral("Reconnecting");
	text.youtubeStopping = QStringLiteral("Stopping");
	text.youtubeFailed = QStringLiteral("Stopped — Twitch is still streaming");
	text.youtubeSetupRequired = QStringLiteral("Setup required");
	text.retryYouTube = QStringLiteral("Retry YouTube");
	text.noticePreview = QStringLiteral("Preview");
	text.noticeProfileSaved = QStringLiteral("Profile saved");
	text.noticeServerUrlSaved = QStringLiteral("URL saved");
	text.noticeMissingServerUrl = QStringLiteral("Missing URL");
	text.noticeInvalidServerUrl = QStringLiteral("Invalid URL");
	text.noticeServerUrlTooLong = QStringLiteral("URL too long");
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
	text.noticeYouTubeFailed = QStringLiteral("YouTube stopped. Twitch is still streaming.");
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
	auto *nativeDestination = requiredChild<QLabel>(view, "easyMultistreamNativeDestination");
	auto *nativeStatus = requiredChild<QLabel>(view, "easyMultistreamNativeStatus");
	auto *youtubeStatus = requiredChild<QLabel>(view, "easyMultistreamYouTubeStatus");
	auto *notice = requiredChild<QLabel>(view, "easyMultistreamNotice");
	auto *credentialStatus = requiredChild<QLabel>(view, "easyMultistreamCredentialStatus");
	auto *gettingStarted = requiredChild<QGroupBox>(view, "easyMultistreamGettingStarted");
	auto *setup = requiredChild<QGroupBox>(view, "easyMultistreamSetup");
	auto *toggleSettings = requiredChild<QPushButton>(view, "easyMultistreamToggleSettings");
	auto *enabled = requiredChild<QCheckBox>(view, "easyMultistreamYouTubeEnabled");
	auto *serverUrl = requiredChild<QLineEdit>(view, "easyMultistreamServerUrl");
	auto *saveServerUrl = requiredChild<QPushButton>(view, "easyMultistreamSaveServerUrl");
	auto *key = requiredChild<QLineEdit>(view, "easyMultistreamStreamKey");
	auto *save = requiredChild<QPushButton>(view, "easyMultistreamSaveKey");
	auto *remove = requiredChild<QPushButton>(view, "easyMultistreamRemoveKey");
	if (profile == nullptr || nativeDestination == nullptr || nativeStatus == nullptr || youtubeStatus == nullptr ||
	    notice == nullptr || credentialStatus == nullptr || gettingStarted == nullptr || setup == nullptr ||
	    toggleSettings == nullptr || enabled == nullptr || serverUrl == nullptr || saveServerUrl == nullptr ||
	    key == nullptr || save == nullptr || remove == nullptr) {
		return;
	}

	CHECK(key->echoMode() == QLineEdit::Password);
	CHECK(key->contextMenuPolicy() == Qt::NoContextMenu);
	CHECK(!key->dragEnabled());
	CHECK(!key->acceptDrops());
	CHECK(key->maxLength() == 2560);
	CHECK(serverUrl->maxLength() == 2048);

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
	CHECK(!gettingStarted->isHidden());
	CHECK(!setup->isHidden());
	CHECK(toggleSettings->isHidden());
	CHECK(!notice->isHidden());

	QObject actionContext;
	bool enabledCalled = false;
	bool enabledValue = false;
	bool saveServerUrlCalled = false;
	bool saveKeyCalled = false;
	bool removeCalled = false;
	bool retryCalled = false;
	QByteArray receivedKey;
	QByteArray receivedServerUrl;
	view.bindActions(
		&actionContext,
		[&](bool value) {
			enabledCalled = true;
			enabledValue = value;
		},
		[&](QByteArray value) {
			saveServerUrlCalled = true;
			receivedServerUrl = std::move(value);
		},
		[&](QByteArray streamKey) {
			saveKeyCalled = true;
			receivedKey = std::move(streamKey);
		},
		[&]() { removeCalled = true; }, [&]() { retryCalled = true; });

	enabled->setChecked(true);
	CHECK(enabledCalled);
	CHECK(enabledValue);
	serverUrl->setText(QStringLiteral("rtmps://a.example/live2"));
	saveServerUrl->click();
	CHECK(saveServerUrlCalled);
	CHECK(receivedServerUrl == QByteArray("rtmps://a.example/live2"));
	key->setText(QStringLiteral("test-key"));
	save->click();
	CHECK(saveKeyCalled);
	CHECK(receivedKey == QByteArray("test-key"));
	receivedKey.fill('\0');
	receivedKey.clear();

	enabledCalled = false;
	state.youtubeEnabled = true;
	state.youtubeServerUrl = QStringLiteral("rtmps://a.example/live2");
	state.credential = easy_multistream::CredentialDisplayState::Present;
	view.applyState(state);
	CHECK(!enabledCalled);
	CHECK(youtubeStatus->text() == QStringLiteral("Ready"));
	CHECK(remove->isEnabled());
	CHECK(gettingStarted->isHidden());
	CHECK(setup->isHidden());
	CHECK(!toggleSettings->isHidden());
	CHECK(toggleSettings->text() == QStringLiteral("Change settings"));
	CHECK(notice->isHidden());
	toggleSettings->click();
	CHECK(!setup->isHidden());
	CHECK(toggleSettings->text() == QStringLiteral("Hide settings"));
	CHECK(!notice->isHidden());
	view.applyState(state);
	CHECK(!setup->isHidden());
	toggleSettings->click();
	CHECK(setup->isHidden());
	CHECK(notice->isHidden());
	state.notice = easy_multistream::DockNotice::KeySaved;
	view.applyState(state);
	CHECK(notice->isHidden());
	toggleSettings->click();
	CHECK(!notice->isHidden());
	CHECK(notice->text() == QStringLiteral("Key saved"));
	toggleSettings->click();
	CHECK(notice->isHidden());
	remove->click();
	CHECK(removeCalled);

	state.settingsEditable = false;
	state.notice = easy_multistream::DockNotice::FutureSettings;
	view.applyState(state);
	CHECK(!enabled->isEnabled());
	CHECK(!serverUrl->isEnabled());
	CHECK(!saveServerUrl->isEnabled());
	CHECK(!key->isEnabled());
	CHECK(!save->isEnabled());
	CHECK(!remove->isEnabled());
	CHECK(gettingStarted->isHidden());
	CHECK(!setup->isHidden());
	CHECK(toggleSettings->isHidden());
	CHECK(!notice->isHidden());

	state.settingsEditable = true;
	state.runtimeAvailable = true;
	state.youtubeEnabled = true;
	state.session.nativeDestination = easy_multistream::NativeDestination::Twitch;
	state.session.native = easy_multistream::NativeStreamState::Streaming;
	state.session.youtube = easy_multistream::YouTubeStreamState::Streaming;
	view.applyState(state);
	CHECK(nativeDestination->text() == QStringLiteral("Twitch (OBS)"));
	CHECK(nativeStatus->text() == QStringLiteral("Streaming"));
	CHECK(youtubeStatus->text() == QStringLiteral("Streaming"));
	CHECK(!enabled->isEnabled());
	CHECK(!serverUrl->isEnabled());
	CHECK(!saveServerUrl->isEnabled());
	CHECK(!key->isEnabled());
	CHECK(!save->isEnabled());
	CHECK(!remove->isEnabled());

	state.session.nativeDestination = easy_multistream::NativeDestination::YouTube;
	view.applyState(state);
	CHECK(youtubeStatus->text() == QStringLiteral("Set OBS to Twitch"));

	state.session.nativeDestination = easy_multistream::NativeDestination::Twitch;

	state.session.youtube = easy_multistream::YouTubeStreamState::Failed;
	view.applyState(state);
	CHECK(nativeDestination->text() == QStringLiteral("Twitch (OBS)"));
	CHECK(nativeStatus->text() == QStringLiteral("Streaming"));
	CHECK(youtubeStatus->text() == QStringLiteral("Stopped — Twitch is still streaming"));
	CHECK(notice->text() == QStringLiteral("YouTube stopped. Twitch is still streaming."));
	CHECK(!notice->isHidden());
	auto *retry = requiredChild<QPushButton>(view, "easyMultistreamRetryYouTube");
	CHECK(retry != nullptr && !retry->isHidden() && retry->isEnabled());
	if (retry != nullptr) {
		retry->click();
	}
	CHECK(retryCalled);
}

void testDockAutoShowHelper()
{
	QMainWindow mainWindow;
	QDockWidget dock(QStringLiteral("Easy Multistream"), &mainWindow);
	dock.setObjectName(QStringLiteral("easy_multistream.dock"));
	dock.setWidget(new QWidget(&dock));
	mainWindow.addDockWidget(Qt::RightDockWidgetArea, &dock);
	dock.hide();

	CHECK(!easy_multistream::showDockById(nullptr, "easy_multistream.dock"));
	CHECK(!easy_multistream::showDockById(&mainWindow, "easy_multistream.dock"));

	mainWindow.show();
	QApplication::processEvents();
	CHECK(easy_multistream::showDockById(&mainWindow, "easy_multistream.dock"));
	CHECK(dock.isVisible());
	CHECK(!easy_multistream::showDockById(&mainWindow, "missing.dock"));

	dock.hide();
	mainWindow.showMinimized();
	QApplication::processEvents();
	CHECK(mainWindow.isMinimized());
	CHECK(!easy_multistream::showDockById(&mainWindow, "easy_multistream.dock"));
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
	testDockAutoShowHelper();
	testSecretEditorBlocksExportAndClearsWhenHidden();
	testCredentialRemovalConfirmationIsAsynchronousAndCancellable();

	if (failures != 0) {
		std::cerr << failures << " UI test(s) failed\n";
		return 1;
	}

	std::cout << "All UI tests passed\n";
	return 0;
}

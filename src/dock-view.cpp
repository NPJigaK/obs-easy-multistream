// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-view.hpp"

#include <QCheckBox>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>

#include <utility>

namespace easy_multistream {
namespace {

class SecretLineEdit final : public QLineEdit {
public:
	using QLineEdit::QLineEdit;

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {
			event->accept();
			return;
		}
		QLineEdit::keyPressEvent(event);
	}
};

} // namespace

DockView::DockView(DockText text, QWidget *parent) : QWidget(parent), text_(std::move(text))
{
	setObjectName(QStringLiteral("easyMultistreamDockView"));
	setMinimumWidth(300);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *heading = new QLabel(text_.heading, this);
	QFont headingFont = heading->font();
	headingFont.setBold(true);
	heading->setFont(headingFont);
	layout->addWidget(heading);

	gettingStartedGroup_ = new QGroupBox(text_.gettingStarted, this);
	gettingStartedGroup_->setObjectName(QStringLiteral("easyMultistreamGettingStarted"));
	auto *gettingStartedLayout = new QVBoxLayout(gettingStartedGroup_);
	auto *gettingStartedBody = new QLabel(text_.gettingStartedBody, gettingStartedGroup_);
	gettingStartedBody->setObjectName(QStringLiteral("easyMultistreamGettingStartedBody"));
	gettingStartedBody->setWordWrap(true);
	gettingStartedLayout->addWidget(gettingStartedBody);
	layout->addWidget(gettingStartedGroup_);

	auto *destinations = new QGroupBox(text_.destinations, this);
	auto *destinationsLayout = new QFormLayout(destinations);
	destinationsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	primaryDestinationLabel_ = new QLabel(text_.primaryName, destinations);
	primaryDestinationLabel_->setObjectName(QStringLiteral("easyMultistreamNativeDestination"));
	primaryStatusLabel_ = new QLabel(text_.primaryStatus, destinations);
	primaryStatusLabel_->setObjectName(QStringLiteral("easyMultistreamNativeStatus"));
	primaryStatusLabel_->setWordWrap(true);
	destinationsLayout->addRow(primaryDestinationLabel_, primaryStatusLabel_);
	youtubeStatusLabel_ = new QLabel(destinations);
	youtubeStatusLabel_->setObjectName(QStringLiteral("easyMultistreamYouTubeStatus"));
	youtubeStatusLabel_->setWordWrap(true);
	destinationsLayout->addRow(text_.youtubeName, youtubeStatusLabel_);
	retryYouTubeButton_ = new QPushButton(text_.retryYouTube, destinations);
	retryYouTubeButton_->setObjectName(QStringLiteral("easyMultistreamRetryYouTube"));
	retryYouTubeButton_->setVisible(false);
	destinationsLayout->addRow(QString(), retryYouTubeButton_);
	layout->addWidget(destinations);

	auto *setupControls = new QWidget(this);
	auto *setupControlsLayout = new QHBoxLayout(setupControls);
	setupControlsLayout->setContentsMargins(0, 0, 0, 0);
	youtubeEnabledCheckBox_ = new QCheckBox(text_.enableYouTube, setupControls);
	youtubeEnabledCheckBox_->setObjectName(QStringLiteral("easyMultistreamYouTubeEnabled"));
	setupControlsLayout->addWidget(youtubeEnabledCheckBox_);
	setupControlsLayout->addStretch();
	toggleSettingsButton_ = new QPushButton(text_.showSettings, setupControls);
	toggleSettingsButton_->setObjectName(QStringLiteral("easyMultistreamToggleSettings"));
	setupControlsLayout->addWidget(toggleSettingsButton_);
	layout->addWidget(setupControls);

	setupGroup_ = new QGroupBox(text_.setup, this);
	setupGroup_->setObjectName(QStringLiteral("easyMultistreamSetup"));
	auto *setupLayout = new QVBoxLayout(setupGroup_);
	openYouTubeStudioButton_ = new QPushButton(text_.openYouTubeStudio, setupGroup_);
	openYouTubeStudioButton_->setObjectName(QStringLiteral("easyMultistreamOpenYouTubeStudio"));
	setupLayout->addWidget(openYouTubeStudioButton_);
	auto *setupForm = new QFormLayout();
	setupForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	profileNameLabel_ = new QLabel(setupGroup_);
	profileNameLabel_->setObjectName(QStringLiteral("easyMultistreamProfileName"));
	profileNameLabel_->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
	setupForm->addRow(text_.profileLabel, profileNameLabel_);
	setupLayout->addLayout(setupForm);

	setupForm = new QFormLayout();
	setupForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	serverUrlEdit_ = new QLineEdit(setupGroup_);
	serverUrlEdit_->setObjectName(QStringLiteral("easyMultistreamServerUrl"));
	serverUrlEdit_->setAccessibleName(text_.serverUrlLabel);
	serverUrlEdit_->setPlaceholderText(text_.serverUrlPlaceholder);
	serverUrlEdit_->setMaxLength(2048);
	auto *serverUrlLabel = new QLabel(text_.serverUrlLabel, setupGroup_);
	serverUrlLabel->setBuddy(serverUrlEdit_);
	auto *serverUrlRow = new QWidget(setupGroup_);
	auto *serverUrlRowLayout = new QHBoxLayout(serverUrlRow);
	serverUrlRowLayout->setContentsMargins(0, 0, 0, 0);
	serverUrlRowLayout->addWidget(serverUrlEdit_);
	saveServerUrlButton_ = new QPushButton(text_.saveServerUrl, serverUrlRow);
	saveServerUrlButton_->setObjectName(QStringLiteral("easyMultistreamSaveServerUrl"));
	serverUrlRowLayout->addWidget(saveServerUrlButton_);
	setupForm->addRow(serverUrlLabel, serverUrlRow);

	credentialStatusLabel_ = new QLabel(setupGroup_);
	credentialStatusLabel_->setObjectName(QStringLiteral("easyMultistreamCredentialStatus"));
	credentialStatusLabel_->setWordWrap(true);
	setupForm->addRow(text_.credentialLabel, credentialStatusLabel_);

	streamKeyEdit_ = new SecretLineEdit(setupGroup_);
	streamKeyEdit_->setObjectName(QStringLiteral("easyMultistreamStreamKey"));
	streamKeyEdit_->setAccessibleName(text_.streamKeyLabel);
	streamKeyEdit_->setEchoMode(QLineEdit::Password);
	streamKeyEdit_->setMaxLength(2560);
	streamKeyEdit_->setAcceptDrops(false);
	streamKeyEdit_->setContextMenuPolicy(Qt::NoContextMenu);
	streamKeyEdit_->setDragEnabled(false);
	streamKeyEdit_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
	auto *streamKeyLabel = new QLabel(text_.streamKeyLabel, setupGroup_);
	streamKeyLabel->setBuddy(streamKeyEdit_);
	setupForm->addRow(streamKeyLabel, streamKeyEdit_);
	setupLayout->addLayout(setupForm);
	auto *keyScope = new QLabel(text_.keyScope, setupGroup_);
	keyScope->setWordWrap(true);
	setupLayout->addWidget(keyScope);

	auto *buttonLayout = new QHBoxLayout();
	buttonLayout->addStretch();
	removeKeyButton_ = new QPushButton(text_.removeKey, setupGroup_);
	saveKeyButton_ = new QPushButton(text_.saveKey, setupGroup_);
	removeKeyButton_->setObjectName(QStringLiteral("easyMultistreamRemoveKey"));
	saveKeyButton_->setObjectName(QStringLiteral("easyMultistreamSaveKey"));
	buttonLayout->addWidget(removeKeyButton_);
	buttonLayout->addWidget(saveKeyButton_);
	setupLayout->addLayout(buttonLayout);
	layout->addWidget(setupGroup_);

	noticeLabel_ = new QLabel(this);
	noticeLabel_->setObjectName(QStringLiteral("easyMultistreamNotice"));
	noticeLabel_->setWordWrap(true);
	layout->addWidget(noticeLabel_);
	layout->addStretch();

	QObject::connect(toggleSettingsButton_, &QPushButton::clicked, this, [this]() {
		if (!compactEligible_) {
			return;
		}
		setupExpanded_ = !setupExpanded_;
		updateSetupVisibility(true);
		updateNoticeVisibility();
	});

	applyState({});
}

void DockView::bindActions(QObject *context, EnabledHandler enabledHandler, SaveServerUrlHandler saveServerUrlHandler,
			   SaveKeyHandler saveKeyHandler, RemoveKeyHandler removeKeyHandler,
			   RetryYouTubeHandler retryYouTubeHandler,
			   OpenYouTubeStudioHandler openYouTubeStudioHandler)
{
	Q_ASSERT(context != nullptr);

	QObject::connect(youtubeEnabledCheckBox_, &QCheckBox::toggled, context,
			 [handler = std::move(enabledHandler)](bool enabled) mutable { handler(enabled); });
	QObject::connect(saveServerUrlButton_, &QPushButton::clicked, context,
			 [this, handler = std::move(saveServerUrlHandler)]() mutable {
				 handler(serverUrlEdit_->text().toUtf8());
			 });
	QObject::connect(saveKeyButton_, &QPushButton::clicked, context,
			 [this, handler = std::move(saveKeyHandler)]() mutable {
				 handler(streamKeyEdit_->text().toUtf8());
			 });
	QObject::connect(removeKeyButton_, &QPushButton::clicked, context,
			 [handler = std::move(removeKeyHandler)]() mutable { handler(); });
	QObject::connect(retryYouTubeButton_, &QPushButton::clicked, context,
			 [handler = std::move(retryYouTubeHandler)]() mutable {
				 if (handler) {
					 handler();
				 }
			 });
	QObject::connect(openYouTubeStudioButton_, &QPushButton::clicked, context,
			 [handler = std::move(openYouTubeStudioHandler)]() mutable {
				 if (handler) {
					 handler();
				 }
			 });
}

void DockView::applyState(const DockState &state)
{
	const QSignalBlocker blocker(youtubeEnabledCheckBox_);
	const bool profileChanged = profileNameLabel_->text() != state.profileName;
	profileNameLabel_->setText(state.profileName);
	if (profileChanged || !serverUrlEdit_->hasFocus()) {
		serverUrlEdit_->setText(state.youtubeServerUrl);
	}
	const bool nativeActive = state.runtimeAvailable && state.session.native != NativeStreamState::Stopped;
	const bool youtubeActive = state.runtimeAvailable &&
				   (state.session.youtube == YouTubeStreamState::Connecting ||
				    state.session.youtube == YouTubeStreamState::Streaming ||
				    state.session.youtube == YouTubeStreamState::Reconnecting ||
				    state.session.youtube == YouTubeStreamState::Stopping);
	const bool streamActive = nativeActive || youtubeActive;
	const bool youtubeFailed = state.runtimeAvailable && state.session.youtube == YouTubeStreamState::Failed;
	const bool setupIncomplete = state.youtubeServerUrl.isEmpty() ||
				     state.credential != CredentialDisplayState::Present;
	const bool compactEligible = state.settingsEditable && !setupIncomplete;
	if (!compactEligible) {
		setupExpanded_ = true;
	} else if (!compactEligible_ || profileChanged) {
		setupExpanded_ = false;
	}
	compactEligible_ = compactEligible;
	updateSetupVisibility(compactEligible);
	gettingStartedGroup_->setVisible(state.settingsEditable &&
					 state.credential != CredentialDisplayState::Unavailable && setupIncomplete);
	youtubeEnabledCheckBox_->setChecked(state.youtubeEnabled);
	youtubeEnabledCheckBox_->setEnabled(state.settingsEditable && !streamActive);
	serverUrlEdit_->setEnabled(state.settingsEditable && !streamActive);
	saveServerUrlButton_->setEnabled(state.settingsEditable && !streamActive);
	streamKeyEdit_->setEnabled(state.settingsEditable && !streamActive &&
				   state.credential != CredentialDisplayState::Unavailable);

	const bool credentialAvailable = state.credential != CredentialDisplayState::Unavailable;
	saveKeyButton_->setEnabled(state.settingsEditable && !streamActive && credentialAvailable);
	removeKeyButton_->setEnabled(state.settingsEditable && !streamActive &&
				     state.credential == CredentialDisplayState::Present);
	retryYouTubeButton_->setVisible(youtubeFailed);
	retryYouTubeButton_->setEnabled(youtubeFailed && nativeActive);

	switch (state.credential) {
	case CredentialDisplayState::Missing:
		credentialStatusLabel_->setText(text_.credentialMissing);
		streamKeyEdit_->setPlaceholderText(text_.keyPlaceholderMissing);
		break;
	case CredentialDisplayState::Present:
		credentialStatusLabel_->setText(text_.credentialPresent);
		streamKeyEdit_->setPlaceholderText(text_.keyPlaceholderPresent);
		break;
	case CredentialDisplayState::Unavailable:
		credentialStatusLabel_->setText(text_.credentialUnavailable);
		streamKeyEdit_->setPlaceholderText({});
		break;
	}

	if (state.runtimeAvailable) {
		switch (state.session.nativeDestination) {
		case NativeDestination::Twitch:
			primaryDestinationLabel_->setText(text_.nativeTwitch);
			break;
		case NativeDestination::YouTube:
			primaryDestinationLabel_->setText(text_.nativeYouTube);
			break;
		case NativeDestination::Unknown:
			primaryDestinationLabel_->setText(text_.primaryName);
			break;
		}
		if (state.session.nativeDestination == NativeDestination::Unknown) {
			primaryStatusLabel_->setText(text_.nativeUnavailable);
		} else {
			switch (state.session.native) {
			case NativeStreamState::Stopped:
				primaryStatusLabel_->setText(text_.nativeNotStreaming);
				break;
			case NativeStreamState::Starting:
				primaryStatusLabel_->setText(text_.nativeStarting);
				break;
			case NativeStreamState::Streaming:
				primaryStatusLabel_->setText(text_.nativeStreaming);
				break;
			case NativeStreamState::Stopping:
				primaryStatusLabel_->setText(text_.nativeStopping);
				break;
			}
		}
	} else {
		primaryDestinationLabel_->setText(text_.primaryName);
		primaryStatusLabel_->setText(text_.primaryStatus);
	}

	if (state.runtimeAvailable && state.session.nativeDestination != NativeDestination::Twitch) {
		youtubeStatusLabel_->setText(text_.youtubeRequiresTwitch);
	} else if (state.runtimeAvailable) {
		switch (state.session.youtube) {
		case YouTubeStreamState::Disabled:
			youtubeStatusLabel_->setText(text_.youtubeDisabled);
			break;
		case YouTubeStreamState::SetupRequired:
			if (state.youtubeServerUrl.isEmpty()) {
				youtubeStatusLabel_->setText(text_.youtubeMissingServerUrl);
			} else if (state.credential != CredentialDisplayState::Present) {
				youtubeStatusLabel_->setText(text_.youtubeMissingKey);
			} else {
				youtubeStatusLabel_->setText(text_.youtubeSetupRequired);
			}
			break;
		case YouTubeStreamState::NotStreaming:
			youtubeStatusLabel_->setText(text_.youtubeNotStreaming);
			break;
		case YouTubeStreamState::Connecting:
			youtubeStatusLabel_->setText(text_.youtubeConnecting);
			break;
		case YouTubeStreamState::Streaming:
			youtubeStatusLabel_->setText(text_.youtubeStreaming);
			break;
		case YouTubeStreamState::Reconnecting:
			youtubeStatusLabel_->setText(text_.youtubeReconnecting);
			break;
		case YouTubeStreamState::Stopping:
			youtubeStatusLabel_->setText(text_.youtubeStopping);
			break;
		case YouTubeStreamState::Failed:
			youtubeStatusLabel_->setText(text_.youtubeFailed);
			break;
		}
	} else if (!state.settingsEditable || state.credential == CredentialDisplayState::Unavailable) {
		youtubeStatusLabel_->setText(text_.youtubeUnavailable);
	} else if (!state.youtubeEnabled) {
		youtubeStatusLabel_->setText(text_.youtubeDisabled);
	} else if (state.youtubeServerUrl.isEmpty()) {
		youtubeStatusLabel_->setText(text_.youtubeMissingServerUrl);
	} else if (state.credential == CredentialDisplayState::Present) {
		youtubeStatusLabel_->setText(text_.youtubeReady);
	} else {
		youtubeStatusLabel_->setText(text_.youtubeMissingKey);
	}

	currentNotice_ = state.notice;
	youtubeFailureNotice_ = state.runtimeAvailable && state.session.youtube == YouTubeStreamState::Failed;
	if (youtubeFailureNotice_) {
		noticeLabel_->setText(text_.noticeYouTubeFailed);
	} else {
		noticeLabel_->setText(noticeText(state.notice));
	}
	updateNoticeVisibility();
}

void DockView::updateSetupVisibility(bool compactEligible)
{
	setupGroup_->setVisible(!compactEligible || setupExpanded_);
	toggleSettingsButton_->setVisible(compactEligible);
	toggleSettingsButton_->setText(setupExpanded_ ? text_.hideSettings : text_.showSettings);
}

void DockView::updateNoticeVisibility()
{
	const bool routineNotice =
		currentNotice_ == DockNotice::Preview || currentNotice_ == DockNotice::ProfileSaved ||
		currentNotice_ == DockNotice::ServerUrlSaved || currentNotice_ == DockNotice::KeySaved;
	const bool compactIdle = compactEligible_ && !setupExpanded_;
	noticeLabel_->setVisible(youtubeFailureNotice_ || !compactIdle || !routineNotice);
}

void DockView::clearStreamKey()
{
	streamKeyEdit_->clear();
}

void DockView::requestCredentialRemovalConfirmation(QObject *context, RemoveKeyHandler confirmedHandler)
{
	Q_ASSERT(context != nullptr);
	if (credentialRemovalDialog_ != nullptr) {
		credentialRemovalDialog_->raise();
		credentialRemovalDialog_->activateWindow();
		return;
	}

	auto *dialog = new QMessageBox(QMessageBox::Question, text_.removeKeyTitle, text_.removeKeyMessage,
				       QMessageBox::Yes | QMessageBox::Cancel, this);
	dialog->setObjectName(QStringLiteral("easyMultistreamRemoveKeyConfirmation"));
	dialog->setDefaultButton(QMessageBox::Cancel);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	credentialRemovalDialog_ = dialog;
	QObject::connect(dialog, &QMessageBox::finished, this, [this, dialog](int) {
		if (credentialRemovalDialog_ == dialog) {
			credentialRemovalDialog_.clear();
		}
	});
	const QPointer<QMessageBox> dialogGuard(dialog);
	QObject::connect(dialog, &QMessageBox::finished, context,
			 [dialogGuard, handler = std::move(confirmedHandler)](int) mutable {
				 if (dialogGuard != nullptr &&
				     dialogGuard->standardButton(dialogGuard->clickedButton()) == QMessageBox::Yes) {
					 handler();
				 }
			 });
	dialog->open();
}

void DockView::cancelCredentialRemoval()
{
	if (credentialRemovalDialog_ != nullptr) {
		credentialRemovalDialog_->reject();
		credentialRemovalDialog_.clear();
	}
}

void DockView::refreshTheme()
{
	style()->unpolish(this);
	style()->polish(this);
	update();
}

bool DockView::event(QEvent *event)
{
	const auto dockCloseEvent = static_cast<QEvent::Type>(QEvent::User + QEvent::Close);
	if (event->type() == QEvent::Hide || event->type() == QEvent::Close || event->type() == dockCloseEvent) {
		cancelCredentialRemoval();
		clearStreamKey();
	}
	return QWidget::event(event);
}

QString DockView::noticeText(DockNotice notice) const
{
	switch (notice) {
	case DockNotice::Preview:
		return text_.noticePreview;
	case DockNotice::ProfileSaved:
		return text_.noticeProfileSaved;
	case DockNotice::ServerUrlSaved:
		return text_.noticeServerUrlSaved;
	case DockNotice::MissingServerUrl:
		return text_.noticeMissingServerUrl;
	case DockNotice::InvalidServerUrl:
		return text_.noticeInvalidServerUrl;
	case DockNotice::ServerUrlTooLong:
		return text_.noticeServerUrlTooLong;
	case DockNotice::KeySaved:
		return text_.noticeKeySaved;
	case DockNotice::KeyRemoved:
		return text_.noticeKeyRemoved;
	case DockNotice::MissingKey:
		return text_.noticeMissingKey;
	case DockNotice::KeyTooLong:
		return text_.noticeKeyTooLong;
	case DockNotice::KeyInvalidCharacters:
		return text_.noticeKeyInvalidCharacters;
	case DockNotice::KeyInvalidUtf8:
		return text_.noticeKeyInvalidUtf8;
	case DockNotice::CredentialUnavailable:
		return text_.noticeCredentialUnavailable;
	case DockNotice::CredentialSaveFailed:
		return text_.noticeCredentialSaveFailed;
	case DockNotice::CredentialDeleteFailed:
		return text_.noticeCredentialDeleteFailed;
	case DockNotice::ProfileUnavailable:
		return text_.noticeProfileUnavailable;
	case DockNotice::ProfileChanging:
		return text_.noticeProfileChanging;
	case DockNotice::InvalidSettings:
		return text_.noticeInvalidSettings;
	case DockNotice::FutureSettings:
		return text_.noticeFutureSettings;
	case DockNotice::SettingsSaveFailed:
		return text_.noticeSettingsSaveFailed;
	case DockNotice::InternalError:
		return text_.noticeInternalError;
	}

	return text_.noticeInternalError;
}

} // namespace easy_multistream

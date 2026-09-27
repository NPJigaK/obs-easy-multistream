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

	auto *destinations = new QGroupBox(text_.destinations, this);
	auto *destinationsLayout = new QFormLayout(destinations);
	destinationsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	destinationsLayout->addRow(text_.primaryName, new QLabel(text_.primaryStatus, destinations));
	youtubeStatusLabel_ = new QLabel(destinations);
	youtubeStatusLabel_->setObjectName(QStringLiteral("easyMultistreamYouTubeStatus"));
	youtubeStatusLabel_->setWordWrap(true);
	destinationsLayout->addRow(text_.youtubeName, youtubeStatusLabel_);
	layout->addWidget(destinations);

	auto *setup = new QGroupBox(text_.setup, this);
	auto *setupLayout = new QVBoxLayout(setup);
	auto *setupForm = new QFormLayout();
	setupForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	profileNameLabel_ = new QLabel(setup);
	profileNameLabel_->setObjectName(QStringLiteral("easyMultistreamProfileName"));
	profileNameLabel_->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
	setupForm->addRow(text_.profileLabel, profileNameLabel_);
	setupLayout->addLayout(setupForm);

	youtubeEnabledCheckBox_ = new QCheckBox(text_.enableYouTube, setup);
	youtubeEnabledCheckBox_->setObjectName(QStringLiteral("easyMultistreamYouTubeEnabled"));
	setupLayout->addWidget(youtubeEnabledCheckBox_);

	setupForm = new QFormLayout();
	setupForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	credentialStatusLabel_ = new QLabel(setup);
	credentialStatusLabel_->setObjectName(QStringLiteral("easyMultistreamCredentialStatus"));
	credentialStatusLabel_->setWordWrap(true);
	setupForm->addRow(text_.credentialLabel, credentialStatusLabel_);

	streamKeyEdit_ = new SecretLineEdit(setup);
	streamKeyEdit_->setObjectName(QStringLiteral("easyMultistreamStreamKey"));
	streamKeyEdit_->setAccessibleName(text_.streamKeyLabel);
	streamKeyEdit_->setEchoMode(QLineEdit::Password);
	streamKeyEdit_->setMaxLength(2560);
	streamKeyEdit_->setAcceptDrops(false);
	streamKeyEdit_->setContextMenuPolicy(Qt::NoContextMenu);
	streamKeyEdit_->setDragEnabled(false);
	streamKeyEdit_->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
	auto *streamKeyLabel = new QLabel(text_.streamKeyLabel, setup);
	streamKeyLabel->setBuddy(streamKeyEdit_);
	setupForm->addRow(streamKeyLabel, streamKeyEdit_);
	setupLayout->addLayout(setupForm);
	auto *keyScope = new QLabel(text_.keyScope, setup);
	keyScope->setWordWrap(true);
	setupLayout->addWidget(keyScope);

	auto *buttonLayout = new QHBoxLayout();
	buttonLayout->addStretch();
	removeKeyButton_ = new QPushButton(text_.removeKey, setup);
	saveKeyButton_ = new QPushButton(text_.saveKey, setup);
	removeKeyButton_->setObjectName(QStringLiteral("easyMultistreamRemoveKey"));
	saveKeyButton_->setObjectName(QStringLiteral("easyMultistreamSaveKey"));
	buttonLayout->addWidget(removeKeyButton_);
	buttonLayout->addWidget(saveKeyButton_);
	setupLayout->addLayout(buttonLayout);
	layout->addWidget(setup);

	noticeLabel_ = new QLabel(this);
	noticeLabel_->setObjectName(QStringLiteral("easyMultistreamNotice"));
	noticeLabel_->setWordWrap(true);
	layout->addWidget(noticeLabel_);
	layout->addStretch();

	applyState({});
}

void DockView::bindActions(QObject *context, EnabledHandler enabledHandler, SaveKeyHandler saveKeyHandler,
			   RemoveKeyHandler removeKeyHandler)
{
	Q_ASSERT(context != nullptr);

	QObject::connect(youtubeEnabledCheckBox_, &QCheckBox::toggled, context,
			 [handler = std::move(enabledHandler)](bool enabled) mutable { handler(enabled); });
	QObject::connect(saveKeyButton_, &QPushButton::clicked, context,
			 [this, handler = std::move(saveKeyHandler)]() mutable {
				 handler(streamKeyEdit_->text().toUtf8());
			 });
	QObject::connect(removeKeyButton_, &QPushButton::clicked, context,
			 [handler = std::move(removeKeyHandler)]() mutable { handler(); });
}

void DockView::applyState(const DockState &state)
{
	const QSignalBlocker blocker(youtubeEnabledCheckBox_);
	profileNameLabel_->setText(state.profileName);
	youtubeEnabledCheckBox_->setChecked(state.youtubeEnabled);
	youtubeEnabledCheckBox_->setEnabled(state.settingsEditable);

	const bool credentialAvailable = state.credential != CredentialDisplayState::Unavailable;
	streamKeyEdit_->setEnabled(state.settingsEditable && credentialAvailable);
	saveKeyButton_->setEnabled(state.settingsEditable && credentialAvailable);
	removeKeyButton_->setEnabled(state.settingsEditable && state.credential == CredentialDisplayState::Present);

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

	if (!state.settingsEditable || state.credential == CredentialDisplayState::Unavailable) {
		youtubeStatusLabel_->setText(text_.youtubeUnavailable);
	} else if (!state.youtubeEnabled) {
		youtubeStatusLabel_->setText(text_.youtubeDisabled);
	} else if (state.credential == CredentialDisplayState::Present) {
		youtubeStatusLabel_->setText(text_.youtubeReady);
	} else {
		youtubeStatusLabel_->setText(text_.youtubeMissingKey);
	}

	noticeLabel_->setText(noticeText(state.notice));
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

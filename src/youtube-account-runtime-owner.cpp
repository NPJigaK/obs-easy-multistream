// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-runtime-owner.hpp"

#include "windows-credential-vault.hpp"

#include <QThread>

#include <cassert>
#include <stdexcept>
#include <utility>

namespace easy_multistream {

namespace {

template<typename T> T &requireDependency(const std::unique_ptr<T> &dependency)
{
	if (dependency == nullptr) {
		throw std::invalid_argument("YouTube account runtime owner dependency is required");
	}
	return *dependency;
}

YouTubeAccountLease providerLease(YouTubeAccountConnectionAttempt attempt) noexcept
{
	return {attempt.generation, attempt.attempt};
}

YouTubeAccountConnectionAttempt connectionAttempt(YouTubeAccountLease lease) noexcept
{
	return {lease.generation, lease.attempt};
}

YouTubeAccountConnectionOperationStatus mapStartStatus(YouTubeAccountProviderStartStatus status) noexcept
{
	switch (status) {
	case YouTubeAccountProviderStartStatus::Started:
		return YouTubeAccountConnectionOperationStatus::Started;
	case YouTubeAccountProviderStartStatus::WrongThread:
		return YouTubeAccountConnectionOperationStatus::WrongThread;
	case YouTubeAccountProviderStartStatus::Busy:
		return YouTubeAccountConnectionOperationStatus::Busy;
	case YouTubeAccountProviderStartStatus::Closed:
		return YouTubeAccountConnectionOperationStatus::Closed;
	case YouTubeAccountProviderStartStatus::InvalidClientId:
		return YouTubeAccountConnectionOperationStatus::NotConfigured;
	case YouTubeAccountProviderStartStatus::InvalidProfileBinding:
		return YouTubeAccountConnectionOperationStatus::ProfileChanged;
	case YouTubeAccountProviderStartStatus::InvalidSelectionCommitter:
	case YouTubeAccountProviderStartStatus::OperationFailed:
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
	return YouTubeAccountConnectionOperationStatus::OperationFailed;
}

YouTubeAccountConnectionOperationStatus mapSelectionStatus(YouTubeAccountProviderSelectionStatus status) noexcept
{
	switch (status) {
	case YouTubeAccountProviderSelectionStatus::Accepted:
		return YouTubeAccountConnectionOperationStatus::Accepted;
	case YouTubeAccountProviderSelectionStatus::WrongThread:
		return YouTubeAccountConnectionOperationStatus::WrongThread;
	case YouTubeAccountProviderSelectionStatus::Closed:
		return YouTubeAccountConnectionOperationStatus::Closed;
	case YouTubeAccountProviderSelectionStatus::StaleLease:
		return YouTubeAccountConnectionOperationStatus::StaleAttempt;
	case YouTubeAccountProviderSelectionStatus::WrongStage:
		return YouTubeAccountConnectionOperationStatus::WrongPhase;
	case YouTubeAccountProviderSelectionStatus::UnknownCandidate:
		return YouTubeAccountConnectionOperationStatus::UnknownCandidate;
	case YouTubeAccountProviderSelectionStatus::OperationFailed:
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
	return YouTubeAccountConnectionOperationStatus::OperationFailed;
}

YouTubeAccountConnectionStage mapConnectionStage(YouTubeAccountProviderStage stage) noexcept
{
	switch (stage) {
	case YouTubeAccountProviderStage::Idle:
		return YouTubeAccountConnectionStage::Idle;
	case YouTubeAccountProviderStage::Authorizing:
	case YouTubeAccountProviderStage::ExchangingCode:
	case YouTubeAccountProviderStage::ListingChannels:
	case YouTubeAccountProviderStage::ListingStreams:
		return YouTubeAccountConnectionStage::Connecting;
	case YouTubeAccountProviderStage::AwaitingChannelSelection:
		return YouTubeAccountConnectionStage::SelectingChannel;
	case YouTubeAccountProviderStage::AwaitingStreamSelection:
		return YouTubeAccountConnectionStage::SelectingStream;
	case YouTubeAccountProviderStage::PersistingCredential:
		return YouTubeAccountConnectionStage::Saving;
	case YouTubeAccountProviderStage::Configured:
		return YouTubeAccountConnectionStage::Stored;
	case YouTubeAccountProviderStage::Connected:
		return YouTubeAccountConnectionStage::Connected;
	case YouTubeAccountProviderStage::NeedsReauthorization:
		return YouTubeAccountConnectionStage::NeedsReauthorization;
	case YouTubeAccountProviderStage::Unavailable:
		return YouTubeAccountConnectionStage::Unavailable;
	case YouTubeAccountProviderStage::Failed:
		return YouTubeAccountConnectionStage::Failed;
	case YouTubeAccountProviderStage::Closed:
		return YouTubeAccountConnectionStage::Closed;
	}
	return YouTubeAccountConnectionStage::Unavailable;
}

YouTubeAccountConnectionOperationStatus
connectionStatusForLostRestore(YouTubeAccountProfileRestoreStatus status) noexcept
{
	switch (status) {
	case YouTubeAccountProfileRestoreStatus::NotAccountMode:
		return YouTubeAccountConnectionOperationStatus::NotAccountMode;
	case YouTubeAccountProfileRestoreStatus::ProfileUnavailable:
		return YouTubeAccountConnectionOperationStatus::ProfileUnavailable;
	case YouTubeAccountProfileRestoreStatus::ProfileChanged:
		return YouTubeAccountConnectionOperationStatus::ProfileChanged;
	case YouTubeAccountProfileRestoreStatus::Closed:
		return YouTubeAccountConnectionOperationStatus::Closed;
	case YouTubeAccountProfileRestoreStatus::Restored:
	case YouTubeAccountProfileRestoreStatus::SetupRequired:
	case YouTubeAccountProfileRestoreStatus::ReauthorizationRequired:
	case YouTubeAccountProfileRestoreStatus::CredentialUnavailable:
	case YouTubeAccountProfileRestoreStatus::InvalidSettings:
	case YouTubeAccountProfileRestoreStatus::UnsupportedFutureSettings:
	case YouTubeAccountProfileRestoreStatus::Busy:
	case YouTubeAccountProfileRestoreStatus::WrongThread:
	case YouTubeAccountProfileRestoreStatus::Unavailable:
	case YouTubeAccountProfileRestoreStatus::OperationFailed:
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
	return YouTubeAccountConnectionOperationStatus::OperationFailed;
}

} // namespace

YouTubeAccountRuntimeOwner::YouTubeAccountRuntimeOwner(ProfilePathReader profilePathReader, ConfigReader configReader)
	: YouTubeAccountRuntimeOwner(std::move(profilePathReader), std::move(configReader), QString{}, {},
				     std::make_unique<NativeWinCredentialApi>(),
				     std::make_unique<NativeYouTubeAccountProfileOperationLockApi>())
{
}

YouTubeAccountRuntimeOwner::YouTubeAccountRuntimeOwner(
	ProfilePathReader profilePathReader, ConfigReader configReader, QString clientId,
	GoogleOAuthAuthorizationSession::BrowserOpener browserOpener, std::unique_ptr<WinCredentialApi> credentialApi,
	std::unique_ptr<YouTubeAccountProfileOperationLockApi> profileLockApi)
	: context_(std::move(profilePathReader), std::move(configReader)),
	  credentialApi_(std::move(credentialApi)),
	  refreshTokenStore_(requireDependency(credentialApi_)),
	  profileLockApi_(std::move(profileLockApi)),
	  profileLockProvider_(requireDependency(profileLockApi_))
{
	const std::string initialBinding = context_.currentProfileBinding().value_or(std::string{});
	provider_ = std::make_unique<YouTubeAccountProvider>(
		std::move(clientId), std::move(browserOpener), refreshTokenStore_, profileLockProvider_, initialBinding,
		[this](const std::optional<YouTubeAccountSelection> &selection) noexcept {
			return commitSelection(selection);
		});
	restoreCoordinator_ =
		std::make_unique<YouTubeAccountProfileRestoreCoordinator>(context_, *provider_, profileLockProvider_);
}

YouTubeAccountRuntimeOwner::~YouTubeAccountRuntimeOwner()
{
	assert(QThread::currentThread() == (provider_ != nullptr ? provider_->thread() : QThread::currentThread()));
	(void)shutdown();
	// The coordinator references the provider and must be destroyed first. It
	// also owns the final owner-thread retry for a native lock cleanup failure.
	restoreCoordinator_.reset();
	provider_.reset();
}

bool YouTubeAccountRuntimeOwner::onOwnerThread() const noexcept
{
	return provider_ == nullptr || QThread::currentThread() == provider_->thread();
}

bool YouTubeAccountRuntimeOwner::isUsableProfile(const YouTubeAccountProfileContext::Snapshot &snapshot) noexcept
{
	return snapshot.generation != 0 && !snapshot.profileBinding.empty() &&
	       snapshot.connectionMode == YouTubeConnectionMode::Account;
}

bool YouTubeAccountRuntimeOwner::isAcceptedRestore(const YouTubeAccountProfileRestoreResult &result) noexcept
{
	if (result.profile.status != YouTubeAccountProfileContext::LoadStatus::Loaded &&
	    result.profile.status != YouTubeAccountProfileContext::LoadStatus::Defaults &&
	    result.profile.status != YouTubeAccountProfileContext::LoadStatus::SetupRequired) {
		return false;
	}
	if (!isUsableProfile(result.profile.snapshot)) {
		return false;
	}
	return result.status == YouTubeAccountProfileRestoreStatus::Restored ||
	       result.status == YouTubeAccountProfileRestoreStatus::SetupRequired ||
	       result.status == YouTubeAccountProfileRestoreStatus::ReauthorizationRequired ||
	       result.status == YouTubeAccountProfileRestoreStatus::CredentialUnavailable;
}

bool YouTubeAccountRuntimeOwner::isAcceptedDisconnect(const YouTubeAccountProfileDisconnectResult &result) noexcept
{
	// The disconnect transaction returns the post-commit account snapshot.  Do
	// not accept a status alone: accepting a stale/cleared snapshot here would
	// let a later selection commit use the wrong profile generation.
	if (result.status != YouTubeAccountProfileDisconnectStatus::Disconnected &&
	    result.status != YouTubeAccountProfileDisconnectStatus::AlreadyDisconnected) {
		return false;
	}
	if (result.profile.status != YouTubeAccountProfileContext::LoadStatus::SetupRequired) {
		return false;
	}
	const auto &snapshot = result.profile.snapshot;
	return isUsableProfile(snapshot) && !snapshot.selection.has_value();
}

YouTubeAccountProfileRestoreStatus
YouTubeAccountRuntimeOwner::mapDisconnectStatus(YouTubeAccountProfileDisconnectStatus status) const noexcept
{
	switch (status) {
	case YouTubeAccountProfileDisconnectStatus::Disconnected:
	case YouTubeAccountProfileDisconnectStatus::AlreadyDisconnected:
		return YouTubeAccountProfileRestoreStatus::SetupRequired;
	case YouTubeAccountProfileDisconnectStatus::NotAccountMode:
		return YouTubeAccountProfileRestoreStatus::NotAccountMode;
	case YouTubeAccountProfileDisconnectStatus::ProfileUnavailable:
		return YouTubeAccountProfileRestoreStatus::ProfileUnavailable;
	case YouTubeAccountProfileDisconnectStatus::InvalidSettings:
		return YouTubeAccountProfileRestoreStatus::InvalidSettings;
	case YouTubeAccountProfileDisconnectStatus::UnsupportedFutureSettings:
		return YouTubeAccountProfileRestoreStatus::UnsupportedFutureSettings;
	case YouTubeAccountProfileDisconnectStatus::ProfileChanged:
		return YouTubeAccountProfileRestoreStatus::ProfileChanged;
	case YouTubeAccountProfileDisconnectStatus::Busy:
		return restoreStatus_;
	case YouTubeAccountProfileDisconnectStatus::WrongThread:
		return restoreStatus_;
	case YouTubeAccountProfileDisconnectStatus::Closed:
		return YouTubeAccountProfileRestoreStatus::Closed;
	case YouTubeAccountProfileDisconnectStatus::InvalidProfileBinding:
	case YouTubeAccountProfileDisconnectStatus::InvalidSelection:
	case YouTubeAccountProfileDisconnectStatus::CredentialUnavailable:
	case YouTubeAccountProfileDisconnectStatus::ProfileSaveFailed:
	case YouTubeAccountProfileDisconnectStatus::Unavailable:
	case YouTubeAccountProfileDisconnectStatus::OperationFailed:
		return YouTubeAccountProfileRestoreStatus::OperationFailed;
	}
	return YouTubeAccountProfileRestoreStatus::OperationFailed;
}

void YouTubeAccountRuntimeOwner::clearRestoredBinding() noexcept
{
	restored_ = false;
	restoredGeneration_ = 0;
	restoredBinding_.clear();
}

YouTubeAccountProfileRestoreResult YouTubeAccountRuntimeOwner::restoreActiveProfile() noexcept
{
	YouTubeAccountProfileRestoreResult result;
	if (!onOwnerThread()) {
		result.status = YouTubeAccountProfileRestoreStatus::WrongThread;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::WrongThread;
		return result;
	}
	if (closed_ || restoreCoordinator_ == nullptr) {
		result.status = YouTubeAccountProfileRestoreStatus::Closed;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::Closed;
		return result;
	}
	try {
		// A repeated lifecycle notification must not orphan an interactive
		// attempt by clearing the binding that its cancellation facade requires.
		// A missed PROFILE_CHANGING is still detected by the fresh preflight and
		// cancels the old attempt before the new profile is restored.
		if (provider_ != nullptr && provider_->snapshot().account.lease.has_value()) {
			const auto preflight = preflightConnection();
			if (preflight == YouTubeAccountConnectionOperationStatus::Accepted) {
				result.status = YouTubeAccountProfileRestoreStatus::Busy;
				result.providerStatus = YouTubeAccountProviderRestoreStatus::Busy;
				return result;
			}
			if (preflight == YouTubeAccountConnectionOperationStatus::Closed) {
				result.status = YouTubeAccountProfileRestoreStatus::Closed;
				result.providerStatus = YouTubeAccountProviderRestoreStatus::Closed;
				return result;
			}
			if (preflight == YouTubeAccountConnectionOperationStatus::OperationFailed ||
			    preflight == YouTubeAccountConnectionOperationStatus::NotRestored) {
				result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
				result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
				return result;
			}
		}
	} catch (...) {
		result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
		return result;
	}

	clearRestoredBinding();
	try {
		result = restoreCoordinator_->restore();
		restoreStatus_ = result.status;
		if (isAcceptedRestore(result)) {
			restored_ = true;
			restoredGeneration_ = result.profile.snapshot.generation;
			restoredBinding_ = result.profile.snapshot.profileBinding;
		} else {
			clearRestoredBinding();
		}
	} catch (...) {
		clearRestoredBinding();
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
		result.status = restoreStatus_;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
	}
	return result;
}

bool YouTubeAccountRuntimeOwner::invalidateConnectionContext(YouTubeAccountProfileRestoreStatus status) noexcept
{
	clearRestoredBinding();
	try {
		if (restoreCoordinator_ == nullptr || !restoreCoordinator_->invalidate()) {
			restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
			return false;
		}
		restoreStatus_ = status;
		return true;
	} catch (...) {
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
		return false;
	}
}

YouTubeAccountConnectionOperationStatus YouTubeAccountRuntimeOwner::preflightConnection() noexcept
{
	if (closed_) {
		return YouTubeAccountConnectionOperationStatus::Closed;
	}
	if (!restored_ || restoredGeneration_ == 0 || restoredBinding_.empty()) {
		return YouTubeAccountConnectionOperationStatus::NotRestored;
	}

	switch (context_.checkActiveAccount(restoredGeneration_, restoredBinding_)) {
	case YouTubeAccountProfileContext::ActiveAccountStatus::Current:
		return YouTubeAccountConnectionOperationStatus::Accepted;
	case YouTubeAccountProfileContext::ActiveAccountStatus::Stale:
		return invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::ProfileChanged)
			       ? YouTubeAccountConnectionOperationStatus::ProfileChanged
			       : YouTubeAccountConnectionOperationStatus::OperationFailed;
	case YouTubeAccountProfileContext::ActiveAccountStatus::NotAccountMode:
		return invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::NotAccountMode)
			       ? YouTubeAccountConnectionOperationStatus::NotAccountMode
			       : YouTubeAccountConnectionOperationStatus::OperationFailed;
	case YouTubeAccountProfileContext::ActiveAccountStatus::ProfileUnavailable:
		return invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::ProfileUnavailable)
			       ? YouTubeAccountConnectionOperationStatus::ProfileUnavailable
			       : YouTubeAccountConnectionOperationStatus::OperationFailed;
	case YouTubeAccountProfileContext::ActiveAccountStatus::InvalidSettings:
		(void)invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::InvalidSettings);
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	case YouTubeAccountProfileContext::ActiveAccountStatus::UnsupportedFutureSettings:
		(void)invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::UnsupportedFutureSettings);
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	case YouTubeAccountProfileContext::ActiveAccountStatus::Unavailable:
		(void)invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::OperationFailed);
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
	return YouTubeAccountConnectionOperationStatus::OperationFailed;
}

YouTubeAccountConnectionOperationStatus
YouTubeAccountRuntimeOwner::startConnection(GoogleOAuthConsentMode consentMode) noexcept
{
	if (!onOwnerThread()) {
		return YouTubeAccountConnectionOperationStatus::WrongThread;
	}
	if (closed_ || provider_ == nullptr) {
		return YouTubeAccountConnectionOperationStatus::Closed;
	}
	const auto preflight = preflightConnection();
	if (preflight != YouTubeAccountConnectionOperationStatus::Accepted) {
		return preflight;
	}
	const std::uint64_t expectedGeneration = restoredGeneration_;
	const auto providerStatus = provider_->startConnection(consentMode);
	// The system browser opener may run a nested event loop. Revalidate every
	// owner invariant before interpreting the provider's immediate result so a
	// reentrant profile change or shutdown wins over a stale start status.
	if (closed_) {
		return YouTubeAccountConnectionOperationStatus::Closed;
	}
	if (!restored_) {
		return connectionStatusForLostRestore(restoreStatus_);
	}
	if (restoredGeneration_ != expectedGeneration) {
		return YouTubeAccountConnectionOperationStatus::ProfileChanged;
	}
	const auto postflight = preflightConnection();
	if (postflight != YouTubeAccountConnectionOperationStatus::Accepted) {
		return postflight;
	}
	if (providerStatus == YouTubeAccountProviderStartStatus::InvalidProfileBinding) {
		return invalidateConnectionContext(YouTubeAccountProfileRestoreStatus::ProfileChanged)
			       ? YouTubeAccountConnectionOperationStatus::ProfileChanged
			       : YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
	if (providerStatus != YouTubeAccountProviderStartStatus::Started) {
		return mapStartStatus(providerStatus);
	}
	try {
		const auto snapshot = provider_->snapshot();
		if (snapshot.account.lease.has_value()) {
			return YouTubeAccountConnectionOperationStatus::Started;
		}
		// A nested event loop can, in principle, complete the whole OAuth and
		// discovery sequence before the browser opener returns.  That is still a
		// successfully accepted start.  Every other lease-less state means the
		// operation was cancelled or failed while startConnection() was re-entered.
		return snapshot.stage == YouTubeAccountProviderStage::Connected &&
				       snapshot.account.failure == YouTubeAccountFailure::None
			       ? YouTubeAccountConnectionOperationStatus::Started
			       : YouTubeAccountConnectionOperationStatus::OperationFailed;
	} catch (...) {
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
}

YouTubeAccountConnectionOperationStatus
YouTubeAccountRuntimeOwner::selectChannel(YouTubeAccountConnectionAttempt attempt,
					  YouTubeAccountChannelCandidateHandle candidate) noexcept
{
	if (!onOwnerThread()) {
		return YouTubeAccountConnectionOperationStatus::WrongThread;
	}
	if (closed_ || provider_ == nullptr) {
		return YouTubeAccountConnectionOperationStatus::Closed;
	}
	const auto preflight = preflightConnection();
	if (preflight != YouTubeAccountConnectionOperationStatus::Accepted) {
		return preflight;
	}
	try {
		const auto snapshot = provider_->snapshot();
		const auto lease = providerLease(attempt);
		if (!snapshot.account.lease.has_value() || *snapshot.account.lease != lease) {
			return YouTubeAccountConnectionOperationStatus::StaleAttempt;
		}
		if (snapshot.stage != YouTubeAccountProviderStage::AwaitingChannelSelection) {
			return YouTubeAccountConnectionOperationStatus::WrongPhase;
		}
		if (candidate.revision != snapshot.revision || candidate.index >= snapshot.channels.size()) {
			return YouTubeAccountConnectionOperationStatus::UnknownCandidate;
		}
		return mapSelectionStatus(provider_->selectChannel(lease, snapshot.channels[candidate.index].id));
	} catch (...) {
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
}

YouTubeAccountConnectionOperationStatus
YouTubeAccountRuntimeOwner::selectStream(YouTubeAccountConnectionAttempt attempt,
					 YouTubeAccountStreamCandidateHandle candidate) noexcept
{
	if (!onOwnerThread()) {
		return YouTubeAccountConnectionOperationStatus::WrongThread;
	}
	if (closed_ || provider_ == nullptr) {
		return YouTubeAccountConnectionOperationStatus::Closed;
	}
	const auto preflight = preflightConnection();
	if (preflight != YouTubeAccountConnectionOperationStatus::Accepted) {
		return preflight;
	}
	try {
		const auto snapshot = provider_->snapshot();
		const auto lease = providerLease(attempt);
		if (!snapshot.account.lease.has_value() || *snapshot.account.lease != lease) {
			return YouTubeAccountConnectionOperationStatus::StaleAttempt;
		}
		if (snapshot.stage != YouTubeAccountProviderStage::AwaitingStreamSelection) {
			return YouTubeAccountConnectionOperationStatus::WrongPhase;
		}
		if (candidate.revision != snapshot.revision || candidate.index >= snapshot.streams.size()) {
			return YouTubeAccountConnectionOperationStatus::UnknownCandidate;
		}
		return mapSelectionStatus(provider_->selectStream(lease, snapshot.streams[candidate.index].id));
	} catch (...) {
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
}

YouTubeAccountConnectionOperationStatus
YouTubeAccountRuntimeOwner::cancelConnection(YouTubeAccountConnectionAttempt attempt) noexcept
{
	if (!onOwnerThread()) {
		return YouTubeAccountConnectionOperationStatus::WrongThread;
	}
	if (closed_ || provider_ == nullptr) {
		return YouTubeAccountConnectionOperationStatus::Closed;
	}
	const auto preflight = preflightConnection();
	if (preflight != YouTubeAccountConnectionOperationStatus::Accepted) {
		return preflight;
	}
	try {
		const auto before = provider_->snapshot();
		const auto lease = providerLease(attempt);
		if (!before.account.lease.has_value()) {
			return YouTubeAccountConnectionOperationStatus::NoActiveAttempt;
		}
		if (*before.account.lease != lease) {
			return YouTubeAccountConnectionOperationStatus::StaleAttempt;
		}
		if (provider_->cancel(lease)) {
			return YouTubeAccountConnectionOperationStatus::Cancelled;
		}

		const auto after = provider_->snapshot();
		if (after.stage == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountConnectionOperationStatus::Closed;
		}
		if (after.account.lease.has_value() && *after.account.lease == lease) {
			return YouTubeAccountConnectionOperationStatus::Busy;
		}
		// A cancellation that removed the attempt but failed native cleanup is
		// not a reusable connection context. Keep all later work fail-closed.
		clearRestoredBinding();
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	} catch (...) {
		clearRestoredBinding();
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
		return YouTubeAccountConnectionOperationStatus::OperationFailed;
	}
}

YouTubeAccountConnectionSnapshot YouTubeAccountRuntimeOwner::connectionSnapshot() const
{
	YouTubeAccountConnectionSnapshot value;
	if (!onOwnerThread()) {
		return value;
	}
	value.restored = restored_;
	value.closed = closed_;
	if (provider_ == nullptr) {
		return value;
	}

	const auto providerSnapshot = provider_->snapshot();
	value.revision = providerSnapshot.revision;
	value.stage = mapConnectionStage(providerSnapshot.stage);
	value.failure = providerSnapshot.account.failure;
	value.channelLabel = providerSnapshot.account.channelLabel;
	value.streamLabel = providerSnapshot.account.streamLabel;
	if (providerSnapshot.account.lease.has_value()) {
		value.activeAttempt = connectionAttempt(*providerSnapshot.account.lease);
	}
	value.channels.reserve(providerSnapshot.channels.size());
	for (std::size_t index = 0; index < providerSnapshot.channels.size(); ++index) {
		value.channels.push_back({{providerSnapshot.revision, index}, providerSnapshot.channels[index].label});
	}
	value.streams.reserve(providerSnapshot.streams.size());
	for (std::size_t index = 0; index < providerSnapshot.streams.size(); ++index) {
		value.streams.push_back({{providerSnapshot.revision, index}, providerSnapshot.streams[index].label});
	}
	return value;
}

YouTubeAccountProfileDisconnectResult YouTubeAccountRuntimeOwner::disconnectActiveProfile() noexcept
{
	YouTubeAccountProfileDisconnectResult result;
	if (!onOwnerThread()) {
		result.status = YouTubeAccountProfileDisconnectStatus::WrongThread;
		result.providerStatus = YouTubeAccountProviderDisconnectStatus::WrongThread;
		return result;
	}
	if (closed_ || restoreCoordinator_ == nullptr) {
		result.status = YouTubeAccountProfileDisconnectStatus::Closed;
		result.providerStatus = YouTubeAccountProviderDisconnectStatus::Closed;
		return result;
	}

	try {
		result = restoreCoordinator_->disconnectLocal();
		// Busy and wrong-thread are non-mutating outcomes. In particular, do not
		// overwrite a valid restored generation with a transient lock conflict.
		if (result.status == YouTubeAccountProfileDisconnectStatus::Busy ||
		    result.status == YouTubeAccountProfileDisconnectStatus::WrongThread) {
			return result;
		}

		if (isAcceptedDisconnect(result)) {
			const auto &snapshot = result.profile.snapshot;
			restored_ = true;
			restoredGeneration_ = snapshot.generation;
			restoredBinding_ = snapshot.profileBinding;
			restoreStatus_ = YouTubeAccountProfileRestoreStatus::SetupRequired;
			return result;
		}
		if (result.status == YouTubeAccountProfileDisconnectStatus::CredentialUnavailable &&
		    isUsableProfile(result.profile.snapshot) && result.profile.snapshot.selection.has_value()) {
			// Credential deletion failed before any durable mutation. Rebind the
			// owner to the generation loaded by the failed transaction and keep the
			// previous connection status so the same action can be retried safely.
			restored_ = true;
			restoredGeneration_ = result.profile.snapshot.generation;
			restoredBinding_ = result.profile.snapshot.profileBinding;
			return result;
		}

		// A closed coordinator is terminal. This can only normally happen after
		// shutdown(), but preserve the invariant if a lower layer closed first.
		if (result.status == YouTubeAccountProfileDisconnectStatus::Closed) {
			closed_ = true;
		}
		clearRestoredBinding();
		restoreStatus_ = mapDisconnectStatus(result.status);
		return result;
	} catch (...) {
		clearRestoredBinding();
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
		result.status = YouTubeAccountProfileDisconnectStatus::OperationFailed;
		result.providerStatus = YouTubeAccountProviderDisconnectStatus::OperationFailed;
		return result;
	}
}

bool YouTubeAccountRuntimeOwner::invalidateForProfileChange() noexcept
{
	if (!onOwnerThread()) {
		return false;
	}
	clearRestoredBinding();
	if (closed_ || restoreCoordinator_ == nullptr) {
		return false;
	}
	try {
		const bool result = restoreCoordinator_->invalidate();
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::ProfileChanged;
		return result;
	} catch (...) {
		restoreStatus_ = YouTubeAccountProfileRestoreStatus::OperationFailed;
		return false;
	}
}

bool YouTubeAccountRuntimeOwner::shutdown() noexcept
{
	if (!onOwnerThread()) {
		return false;
	}
	if (shutdownComplete_) {
		return true;
	}
	closed_ = true;
	clearRestoredBinding();
	bool coordinatorClosed = true;
	bool providerClosed = true;
	try {
		if (restoreCoordinator_ != nullptr) {
			coordinatorClosed = restoreCoordinator_->shutdown();
		}
		if (provider_ != nullptr) {
			providerClosed = provider_->shutdown();
		}
	} catch (...) {
		coordinatorClosed = false;
		providerClosed = false;
	}
	restoreStatus_ = YouTubeAccountProfileRestoreStatus::Closed;
	shutdownComplete_ = coordinatorClosed && providerClosed;
	return shutdownComplete_;
}

bool YouTubeAccountRuntimeOwner::commitSelection(const std::optional<YouTubeAccountSelection> &selection) noexcept
{
	if (closed_ || !restored_ || restoredGeneration_ == 0 || restoredBinding_.empty()) {
		return false;
	}
	try {
		const auto current = context_.snapshot();
		if (current.generation != restoredGeneration_ || current.profileBinding != restoredBinding_ ||
		    !isUsableProfile(current)) {
			clearRestoredBinding();
			return false;
		}
		const auto committed = context_.commitSelection(restoredGeneration_, restoredBinding_, selection);
		if (committed.status != YouTubeAccountProfileContext::CommitStatus::Committed) {
			if (committed.status == YouTubeAccountProfileContext::CommitStatus::Stale ||
			    committed.status == YouTubeAccountProfileContext::CommitStatus::ProfileUnavailable) {
				clearRestoredBinding();
			}
			return false;
		}
		return true;
	} catch (...) {
		clearRestoredBinding();
		return false;
	}
}

YouTubeAccountRuntimeOwnerSnapshot YouTubeAccountRuntimeOwner::snapshot() const
{
	YouTubeAccountRuntimeOwnerSnapshot value;
	value.restoreStatus = restoreStatus_;
	value.generation = restoredGeneration_;
	value.profileBinding = restoredBinding_;
	value.restored = restored_;
	value.closed = closed_;
	if (provider_ != nullptr) {
		value.providerStage = provider_->snapshot().stage;
	}
	return value;
}

} // namespace easy_multistream

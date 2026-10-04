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

} // namespace

YouTubeAccountRuntimeOwner::YouTubeAccountRuntimeOwner(ProfilePathReader profilePathReader,
								ConfigReader configReader)
	: YouTubeAccountRuntimeOwner(std::move(profilePathReader), std::move(configReader), QString{}, {},
					     std::make_unique<NativeWinCredentialApi>(),
					     std::make_unique<NativeYouTubeAccountProfileOperationLockApi>())
{
}

YouTubeAccountRuntimeOwner::YouTubeAccountRuntimeOwner(
	ProfilePathReader profilePathReader, ConfigReader configReader, QString clientId,
	GoogleOAuthAuthorizationSession::BrowserOpener browserOpener,
	std::unique_ptr<WinCredentialApi> credentialApi,
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
	restoreCoordinator_ = std::make_unique<YouTubeAccountProfileRestoreCoordinator>(
		context_, *provider_, profileLockProvider_);
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

bool YouTubeAccountRuntimeOwner::isAcceptedDisconnect(
	const YouTubeAccountProfileDisconnectResult &result) noexcept
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

YouTubeAccountProfileRestoreStatus YouTubeAccountRuntimeOwner::mapDisconnectStatus(
	YouTubeAccountProfileDisconnectStatus status) const noexcept
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

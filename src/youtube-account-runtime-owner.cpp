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

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-profile-restore.hpp"

#include <QThread>

#include <cassert>
#include <utility>

namespace easy_multistream {

YouTubeAccountProfileRestoreCoordinator::YouTubeAccountProfileRestoreCoordinator(
	YouTubeAccountProfileContext &context, YouTubeAccountProvider &provider,
	YouTubeAccountProfileOperationLockProvider &lockProvider) noexcept
	: context_(context), provider_(provider), lockProvider_(lockProvider)
{
}

YouTubeAccountProfileRestoreCoordinator::~YouTubeAccountProfileRestoreCoordinator()
{
	assert(onOwnerThread());
	if (!shutdown() && operationLock_.cleanupPending()) {
		// Native release/close can fail permanently. The lock remains fail-closed
		// and its destructor performs one final owner-thread retry, but the provider
		// must still close its ports and become terminal before it is destroyed.
		provider_.shutdownAfterExternalOperationLockFailure();
	}
}

bool YouTubeAccountProfileRestoreCoordinator::onOwnerThread() const noexcept
{
	return QThread::currentThread() == provider_.thread();
}

bool YouTubeAccountProfileRestoreCoordinator::lifecycleRequestPending() const noexcept
{
	return invalidationRequested_ || shutdownRequested_;
}

YouTubeAccountProfileRestoreStatus
YouTubeAccountProfileRestoreCoordinator::mapProviderStatus(YouTubeAccountProviderRestoreStatus status) noexcept
{
	switch (status) {
	case YouTubeAccountProviderRestoreStatus::Configured:
		return YouTubeAccountProfileRestoreStatus::Restored;
	case YouTubeAccountProviderRestoreStatus::SetupRequired:
		return YouTubeAccountProfileRestoreStatus::SetupRequired;
	case YouTubeAccountProviderRestoreStatus::ReauthorizationRequired:
		return YouTubeAccountProfileRestoreStatus::ReauthorizationRequired;
	case YouTubeAccountProviderRestoreStatus::CredentialUnavailable:
		return YouTubeAccountProfileRestoreStatus::CredentialUnavailable;
	case YouTubeAccountProviderRestoreStatus::WrongThread:
		return YouTubeAccountProfileRestoreStatus::WrongThread;
	case YouTubeAccountProviderRestoreStatus::Busy:
		return YouTubeAccountProfileRestoreStatus::Busy;
	case YouTubeAccountProviderRestoreStatus::Closed:
		return YouTubeAccountProfileRestoreStatus::Closed;
	case YouTubeAccountProviderRestoreStatus::InvalidProfileBinding:
		return YouTubeAccountProfileRestoreStatus::InvalidProfileBinding;
	case YouTubeAccountProviderRestoreStatus::InvalidSelection:
		return YouTubeAccountProfileRestoreStatus::InvalidSelection;
	case YouTubeAccountProviderRestoreStatus::OperationFailed:
		return YouTubeAccountProfileRestoreStatus::OperationFailed;
	}
	return YouTubeAccountProfileRestoreStatus::OperationFailed;
}

void YouTubeAccountProfileRestoreCoordinator::invalidateAfterFailure() noexcept
{
	context_.invalidate();
	(void)provider_.invalidateContext();
}

void YouTubeAccountProfileRestoreCoordinator::clearReturnedProfile(
	YouTubeAccountProfileRestoreResult &result) noexcept
{
	// The context has already been invalidated. Do not let a caller accidentally
	// consume the selection copied just before a profile race or cleanup failure.
	result.profile.status = YouTubeAccountProfileContext::LoadStatus::Unavailable;
	result.profile.snapshot.generation = 0;
	result.profile.snapshot.profileBinding.clear();
	result.profile.snapshot.connectionMode = YouTubeConnectionMode::Manual;
	result.profile.snapshot.selection.reset();
	result.profile.snapshot.settingsStatus = SettingsLoadStatus::Unavailable;
}

bool YouTubeAccountProfileRestoreCoordinator::invalidateNow() noexcept
{
	context_.invalidate();
	// Release an unfinished borrowed transaction before asking the provider to
	// leave its guarded state. If native ownership remains uncertain, keep the
	// provider blocked and unavailable until a later owner-thread retry.
	const bool released = operationLock_.release();
	if (!released) {
		provider_.markExternalOperationReleaseFailed();
		return false;
	}
	provider_.externalOperationLockReleased();

	bool providerInvalidated = provider_.invalidateContext();
	if (!providerInvalidated) {
		// snapshot() returns value copies and can allocate. Keep this noexcept
		// cleanup boundary safe if allocation fails while detecting Closed.
		try {
			providerInvalidated = provider_.snapshot().stage == YouTubeAccountProviderStage::Closed;
		} catch (...) {
			providerInvalidated = false;
		}
	}
	return providerInvalidated;
}

void YouTubeAccountProfileRestoreCoordinator::finishTransaction(
	YouTubeAccountProfileRestoreResult &result) noexcept
{
	assert(transactionActive_);

	bool pendingInvalidation = invalidationRequested_;
	bool pendingShutdown = shutdownRequested_;
	invalidationRequested_ = false;
	shutdownRequested_ = false;

	bool cleanupSucceeded = true;
	if (pendingInvalidation || pendingShutdown) {
		// Keep transactionActive_ set while cleanup releases the profile mutex.
		// A nested lifecycle request is therefore deferred instead of releasing
		// an in-flight transaction from inside one of its callbacks.
		cleanupSucceeded = invalidateNow();
	}

	// Treat any lifecycle request made while cleanup itself was running as part
	// of this same completion boundary. invalidateNow() has already performed
	// the required cleanup; shutdown() also made closed_ terminal immediately.
	pendingInvalidation = pendingInvalidation || invalidationRequested_;
	pendingShutdown = pendingShutdown || shutdownRequested_;
	invalidationRequested_ = false;
	shutdownRequested_ = false;
	transactionActive_ = false;

	if (!pendingInvalidation && !pendingShutdown) {
		return;
	}

	clearReturnedProfile(result);
	if (!cleanupSucceeded) {
		result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
		return;
	}
	if (pendingShutdown) {
		result.status = YouTubeAccountProfileRestoreStatus::Closed;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::Closed;
		return;
	}
	result.status = YouTubeAccountProfileRestoreStatus::ProfileChanged;
	result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
}

bool YouTubeAccountProfileRestoreCoordinator::releaseAfterTransaction(
	YouTubeAccountProfileRestoreResult &result) noexcept
{
	if (operationLock_.release()) {
		provider_.externalOperationLockReleased();
		return true;
	}

	// A native release failure means the transaction's ownership boundary is
	// uncertain.  Both profile-bound snapshots are invalidated, and the
	// provider is failed closed without attempting a second credential read.
	context_.invalidate();
	provider_.markExternalOperationReleaseFailed();
	clearReturnedProfile(result);
	result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
	result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
	return false;
}

YouTubeAccountProfileRestoreResult YouTubeAccountProfileRestoreCoordinator::restore() noexcept
{
	YouTubeAccountProfileRestoreResult result;
	if (!onOwnerThread()) {
		result.status = YouTubeAccountProfileRestoreStatus::WrongThread;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::WrongThread;
		return result;
	}
	if (closed_) {
		result.status = YouTubeAccountProfileRestoreStatus::Closed;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::Closed;
		return result;
	}
	if (transactionActive_) {
		result.status = YouTubeAccountProfileRestoreStatus::Busy;
		result.providerStatus = YouTubeAccountProviderRestoreStatus::Busy;
		return result;
	}
	transactionActive_ = true;

	try {
		if (provider_.snapshot().stage == YouTubeAccountProviderStage::Closed) {
			result.status = YouTubeAccountProfileRestoreStatus::Closed;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::Closed;
			finishTransaction(result);
			return result;
		}
		// A retained handle means a previous ReleaseMutex failed.  Do not read
		// the profile or credential store until shutdown()/invalidate() retries it.
		if (operationLock_.cleanupPending()) {
			result.status = YouTubeAccountProfileRestoreStatus::Busy;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::Busy;
			finishTransaction(result);
			return result;
		}

		// This is intentionally the only pre-lock profile read.  It derives an
		// opaque binding from the active path and never touches config/settings.
		const auto candidateBinding = context_.currentProfileBinding();
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		if (!candidateBinding.has_value()) {
			context_.invalidate();
			(void)provider_.invalidateContext();
			result.status = YouTubeAccountProfileRestoreStatus::ProfileUnavailable;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			finishTransaction(result);
			return result;
		}

		const auto lockResult = lockProvider_.acquire(*candidateBinding, operationLock_);
		result.recovered = lockResult.recovered();
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		if (!lockResult.acquired()) {
			switch (lockResult.status) {
			case YouTubeAccountProfileOperationLockStatus::Busy:
				result.status = YouTubeAccountProfileRestoreStatus::Busy;
				result.providerStatus = YouTubeAccountProviderRestoreStatus::Busy;
				break;
			case YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding:
				result.status = YouTubeAccountProfileRestoreStatus::InvalidProfileBinding;
				result.providerStatus = YouTubeAccountProviderRestoreStatus::InvalidProfileBinding;
				break;
			case YouTubeAccountProfileOperationLockStatus::Unavailable:
				result.status = YouTubeAccountProfileRestoreStatus::Unavailable;
				result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
				break;
			case YouTubeAccountProfileOperationLockStatus::Acquired:
			case YouTubeAccountProfileOperationLockStatus::Recovered:
				break;
			}
			finishTransaction(result);
			return result;
		}

		if (!operationLock_.acquiredFor(*candidateBinding)) {
			invalidateAfterFailure();
			clearReturnedProfile(result);
			result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			releaseAfterTransaction(result);
			finishTransaction(result);
			return result;
		}

		// Re-check the active path before asking OBS for its config pointer. This
		// prevents reading profile B's config while holding profile A's mutex when
		// a switch completed during acquisition. The readers are synchronous and
		// must not process events; the post-load check below remains a second guard.
		const auto lockedBinding = context_.currentProfileBinding();
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		if (!lockedBinding.has_value() || *lockedBinding != *candidateBinding) {
			context_.invalidate();
			(void)provider_.invalidateContext();
			clearReturnedProfile(result);
			result.status = lockedBinding.has_value()
					? YouTubeAccountProfileRestoreStatus::ProfileChanged
					: YouTubeAccountProfileRestoreStatus::ProfileUnavailable;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			releaseAfterTransaction(result);
			finishTransaction(result);
			return result;
		}

		// Re-read both active path and config while the exact cross-process lease
		// is held.  This closes the pre-lock stale-selection/profile race.
		result.profile = context_.load();
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		const auto postLoadBinding = context_.currentProfileBinding();
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		if (!postLoadBinding.has_value() || *postLoadBinding != *candidateBinding) {
			context_.invalidate();
			(void)provider_.invalidateContext();
			clearReturnedProfile(result);
			result.status = postLoadBinding.has_value()
					? YouTubeAccountProfileRestoreStatus::ProfileChanged
					: YouTubeAccountProfileRestoreStatus::ProfileUnavailable;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			releaseAfterTransaction(result);
			finishTransaction(result);
			return result;
		}
		const auto &loadedSnapshot = result.profile.snapshot;
		if (loadedSnapshot.profileBinding.empty()) {
			context_.invalidate();
			(void)provider_.invalidateContext();
			clearReturnedProfile(result);
			result.status = YouTubeAccountProfileRestoreStatus::ProfileUnavailable;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			releaseAfterTransaction(result);
			finishTransaction(result);
			return result;
		}
		if (loadedSnapshot.profileBinding != *candidateBinding) {
			context_.invalidate();
			(void)provider_.invalidateContext();
			clearReturnedProfile(result);
			result.status = YouTubeAccountProfileRestoreStatus::ProfileChanged;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			releaseAfterTransaction(result);
			finishTransaction(result);
			return result;
		}

		std::optional<YouTubeAccountSelection> selection;
		switch (result.profile.status) {
		case YouTubeAccountProfileContext::LoadStatus::Loaded:
		case YouTubeAccountProfileContext::LoadStatus::Defaults:
		case YouTubeAccountProfileContext::LoadStatus::SetupRequired:
			if (loadedSnapshot.connectionMode == YouTubeConnectionMode::Account) {
				selection = loadedSnapshot.selection;
			}
			break;
		case YouTubeAccountProfileContext::LoadStatus::ProfileUnavailable:
		case YouTubeAccountProfileContext::LoadStatus::InvalidSettings:
		case YouTubeAccountProfileContext::LoadStatus::UnsupportedFutureSettings:
		case YouTubeAccountProfileContext::LoadStatus::Unavailable:
			selection.reset();
			break;
		}

		result.providerStatus = provider_.restoreSavedStateUnderHeldOperationLock(
			*candidateBinding, std::move(selection), operationLock_);
		result.status = mapProviderStatus(result.providerStatus);
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		// A synchronous dependency must not process frontend events, but verify the
		// active binding once more before releasing the lock so even a broken or
		// test implementation cannot publish profile A state after switching to B.
		const auto finalBinding = context_.currentProfileBinding();
		if (lifecycleRequestPending()) {
			finishTransaction(result);
			return result;
		}
		if (!finalBinding.has_value() || *finalBinding != *candidateBinding) {
			context_.invalidate();
			clearReturnedProfile(result);
			result.status = finalBinding.has_value()
					? YouTubeAccountProfileRestoreStatus::ProfileChanged
					: YouTubeAccountProfileRestoreStatus::ProfileUnavailable;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			if (releaseAfterTransaction(result)) {
				(void)provider_.invalidateContext();
			}
			finishTransaction(result);
			return result;
		}
		const bool providerRestoreApplied =
			result.providerStatus == YouTubeAccountProviderRestoreStatus::Configured ||
			result.providerStatus == YouTubeAccountProviderRestoreStatus::SetupRequired ||
			result.providerStatus == YouTubeAccountProviderRestoreStatus::ReauthorizationRequired ||
			result.providerStatus == YouTubeAccountProviderRestoreStatus::CredentialUnavailable;
		if (!providerRestoreApplied) {
			// The profile was already loaded, but the provider could still be
			// blocked by unfinished work from another profile. Never leave those
			// two snapshots describing different profiles. Release this outer
			// transaction first, then invalidate both sides and let a later event
			// retry from a clean boundary.
			if (releaseAfterTransaction(result)) {
				context_.invalidate();
				clearReturnedProfile(result);
				(void)provider_.invalidateContext();
			}
			finishTransaction(result);
			return result;
		}

		// Preserve the precise config error for callers while still requiring the
		// provider to have cleared its account state under the lock.
		switch (result.profile.status) {
		case YouTubeAccountProfileContext::LoadStatus::InvalidSettings:
			if (result.providerStatus == YouTubeAccountProviderRestoreStatus::SetupRequired) {
				result.status = YouTubeAccountProfileRestoreStatus::InvalidSettings;
			}
			break;
		case YouTubeAccountProfileContext::LoadStatus::UnsupportedFutureSettings:
			if (result.providerStatus == YouTubeAccountProviderRestoreStatus::SetupRequired) {
				result.status = YouTubeAccountProfileRestoreStatus::UnsupportedFutureSettings;
			}
			break;
		case YouTubeAccountProfileContext::LoadStatus::Unavailable:
			if (result.providerStatus == YouTubeAccountProviderRestoreStatus::SetupRequired) {
				result.status = YouTubeAccountProfileRestoreStatus::Unavailable;
			}
			break;
		case YouTubeAccountProfileContext::LoadStatus::Loaded:
		case YouTubeAccountProfileContext::LoadStatus::Defaults:
		case YouTubeAccountProfileContext::LoadStatus::SetupRequired:
		case YouTubeAccountProfileContext::LoadStatus::ProfileUnavailable:
			break;
		}
		if ((result.profile.status == YouTubeAccountProfileContext::LoadStatus::Loaded ||
		     result.profile.status == YouTubeAccountProfileContext::LoadStatus::Defaults ||
		     result.profile.status == YouTubeAccountProfileContext::LoadStatus::SetupRequired) &&
		    loadedSnapshot.connectionMode != YouTubeConnectionMode::Account &&
		    result.providerStatus == YouTubeAccountProviderRestoreStatus::SetupRequired) {
			result.status = YouTubeAccountProfileRestoreStatus::NotAccountMode;
		}

		releaseAfterTransaction(result);
		finishTransaction(result);
		return result;
	} catch (...) {
		// If the lock was acquired, release it on this owner thread.  Any release
		// failure is handled by the same fail-closed path as normal completion.
		if (operationLock_.cleanupPending()) {
			invalidateAfterFailure();
			clearReturnedProfile(result);
			result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
			releaseAfterTransaction(result);
		} else {
			result.status = YouTubeAccountProfileRestoreStatus::OperationFailed;
			result.providerStatus = YouTubeAccountProviderRestoreStatus::OperationFailed;
		}
		finishTransaction(result);
		return result;
	}
}

bool YouTubeAccountProfileRestoreCoordinator::invalidate() noexcept
{
	if (!onOwnerThread()) {
		return false;
	}
	if (transactionActive_) {
		invalidationRequested_ = true;
		return false;
	}
	return invalidateNow();
}

bool YouTubeAccountProfileRestoreCoordinator::shutdown() noexcept
{
	if (!onOwnerThread()) {
		return false;
	}
	// Terminal even when native cleanup needs another owner-thread retry. A
	// later shutdown()/invalidate() may retry release, but restore() stays closed.
	closed_ = true;
	if (transactionActive_) {
		shutdownRequested_ = true;
		invalidationRequested_ = true;
		return false;
	}
	return invalidateNow();
}

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-account-profile-context.hpp"
#include "youtube-account-profile-operation-lock.hpp"
#include "youtube-account-provider.hpp"

#include <cstdint>

namespace easy_multistream {

// A single owner-thread transaction which restores the account selection for
// the currently active OBS profile. This type has no UI and starts no browser,
// listener, or network request; it is a detached seam for future frontend
// integration.
enum class YouTubeAccountProfileRestoreStatus {
	Restored,
	SetupRequired,
	ReauthorizationRequired,
	CredentialUnavailable,
	NotAccountMode,
	ProfileUnavailable,
	InvalidSettings,
	UnsupportedFutureSettings,
	ProfileChanged,
	Busy,
	Unavailable,
	WrongThread,
	Closed,
	InvalidProfileBinding,
	InvalidSelection,
	OperationFailed,
};

struct YouTubeAccountProfileRestoreResult final {
	YouTubeAccountProfileRestoreStatus status = YouTubeAccountProfileRestoreStatus::Unavailable;
	YouTubeAccountProviderRestoreStatus providerStatus =
		YouTubeAccountProviderRestoreStatus::OperationFailed;
	YouTubeAccountProfileContext::LoadResult profile;
	bool recovered = false;
};

// The coordinator is owner-thread-only. Exactly one coordinator may use a
// provider. Its referenced context/provider/lock provider must outlive it, and
// it must be destroyed on the provider's owner thread. It owns the borrowed-
// operation lock for the entire restore sequence and keeps it as a member so a ReleaseMutex
// failure can be retried by shutdown()/invalidate() on the same owner thread.
// Destruction makes the provider terminal even if native cleanup remains
// fail-closed and can only receive the lock object's final teardown retry.
// It never links into the OBS module; callers can adopt it after its
// lifecycle contract is integrated with frontend profile events.
class YouTubeAccountProfileRestoreCoordinator final {
public:
	YouTubeAccountProfileRestoreCoordinator(YouTubeAccountProfileContext &context,
						YouTubeAccountProvider &provider,
						YouTubeAccountProfileOperationLockProvider &lockProvider) noexcept;

	YouTubeAccountProfileRestoreCoordinator(const YouTubeAccountProfileRestoreCoordinator &) = delete;
	YouTubeAccountProfileRestoreCoordinator &operator=(const YouTubeAccountProfileRestoreCoordinator &) = delete;

	~YouTubeAccountProfileRestoreCoordinator();

	// Reads only the profile binding before trying the nonblocking lease.  The
	// active profile and settings are read again while the lease is held.  A
	// failed/Busy acquisition leaves both context and provider untouched.
	YouTubeAccountProfileRestoreResult restore() noexcept;

	// Invalidates profile-bound state and retries any unfinished lock release.
	// This is intended for profile changes and owner-thread shutdown.  It never
	// waits for another process. A request made reentrantly during restore is
	// deferred until that transaction completes and returns false immediately;
	// false also reports native cleanup that still needs an owner-thread retry.
	bool invalidate() noexcept;
	bool shutdown() noexcept;

	// True while native ownership or unfinished owner-thread handle cleanup keeps
	// this transaction from being reused.
	bool lockHeld() const noexcept { return operationLock_.cleanupPending(); }

private:
	bool onOwnerThread() const noexcept;
	bool lifecycleRequestPending() const noexcept;
	bool invalidateNow() noexcept;
	void finishTransaction(YouTubeAccountProfileRestoreResult &result) noexcept;
	void invalidateAfterFailure() noexcept;
	static void clearReturnedProfile(YouTubeAccountProfileRestoreResult &result) noexcept;
	bool releaseAfterTransaction(YouTubeAccountProfileRestoreResult &result) noexcept;
	static YouTubeAccountProfileRestoreStatus mapProviderStatus(
		YouTubeAccountProviderRestoreStatus status) noexcept;

	YouTubeAccountProfileContext &context_;
	YouTubeAccountProvider &provider_;
	YouTubeAccountProfileOperationLockProvider &lockProvider_;
	YouTubeAccountProfileOperationLock operationLock_;
	bool closed_ = false;
	bool transactionActive_ = false;
	bool invalidationRequested_ = false;
	bool shutdownRequested_ = false;
};

} // namespace easy_multistream

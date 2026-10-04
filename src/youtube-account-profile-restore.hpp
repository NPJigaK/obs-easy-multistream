// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-account-profile-context.hpp"
#include "youtube-account-profile-operation-lock.hpp"
#include "youtube-account-provider.hpp"

#include <cstdint>

namespace easy_multistream {

// Result of the owner-thread transaction that restores the account selection
// for the currently active OBS profile. The production lifecycle owner invokes
// this transaction, but it starts no browser, listener, or network request.
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

// A local disconnect clears the saved YouTube account selection and its
// profile-scoped credential without opening a browser or contacting Google.
// The profile remains in account mode so a later connection can be started
// without silently falling back to the manual-key path.
enum class YouTubeAccountProfileDisconnectStatus {
	Disconnected,
	AlreadyDisconnected,
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
	CredentialUnavailable,
	ProfileSaveFailed,
	OperationFailed,
};

struct YouTubeAccountProfileRestoreResult final {
	YouTubeAccountProfileRestoreStatus status = YouTubeAccountProfileRestoreStatus::Unavailable;
	YouTubeAccountProviderRestoreStatus providerStatus =
		YouTubeAccountProviderRestoreStatus::OperationFailed;
	YouTubeAccountProfileContext::LoadResult profile;
	bool recovered = false;
};

struct YouTubeAccountProfileDisconnectResult final {
	YouTubeAccountProfileDisconnectStatus status = YouTubeAccountProfileDisconnectStatus::Unavailable;
	YouTubeAccountProviderDisconnectStatus providerStatus =
		YouTubeAccountProviderDisconnectStatus::OperationFailed;
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
// The production runtime owner uses this coordinator for profile lifecycle
// restoration and the internal local-disconnect seam. Interactive account UI
// and Google network operations remain outside this type.
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

	// Erases the saved account credential and selection for the active OBS
	// profile. The operation is owner-thread-only and deliberately shares the
	// restore transaction/operation-lock state; it can never run concurrently
	// with restore or lifecycle invalidation. Account mode is preserved with an
	// empty selection so the profile cannot silently switch to manual mode.
	YouTubeAccountProfileDisconnectResult disconnectLocal() noexcept;

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
	void finishTransaction(YouTubeAccountProfileDisconnectResult &result) noexcept;
	void invalidateAfterFailure() noexcept;
	static void clearReturnedProfile(YouTubeAccountProfileRestoreResult &result) noexcept;
	static void clearReturnedProfile(YouTubeAccountProfileDisconnectResult &result) noexcept;
	bool releaseAfterTransaction(YouTubeAccountProfileRestoreResult &result) noexcept;
	bool releaseAfterTransaction(YouTubeAccountProfileDisconnectResult &result) noexcept;
	static YouTubeAccountProfileRestoreStatus mapProviderStatus(
		YouTubeAccountProviderRestoreStatus status) noexcept;
	static YouTubeAccountProfileDisconnectStatus mapProviderStatus(
		YouTubeAccountProviderDisconnectStatus status) noexcept;

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

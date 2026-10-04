// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-account-profile-restore.hpp"
#include "windows-credential-vault.hpp"

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace easy_multistream {

// This is the production lifecycle seam for the saved YouTube account state.
// It owns restore/invalidation, the internal local-disconnect transaction, and
// a headless connection facade. The facade remains detached from OBS UI/runtime
// code until the account output and remote-revoke lifecycles are complete.
// All methods are owner-thread-only.
struct YouTubeAccountRuntimeOwnerSnapshot final {
	YouTubeAccountProfileRestoreStatus restoreStatus = YouTubeAccountProfileRestoreStatus::Unavailable;
	YouTubeAccountProviderStage providerStage = YouTubeAccountProviderStage::Idle;
	std::uint64_t generation = 0;
	std::string profileBinding;
	bool restored = false;
	bool closed = false;
};

enum class YouTubeAccountConnectionOperationStatus {
	Started,
	Accepted,
	Cancelled,
	NoActiveAttempt,
	NotConfigured,
	NotRestored,
	NotAccountMode,
	ProfileUnavailable,
	ProfileChanged,
	Busy,
	WrongThread,
	Closed,
	StaleAttempt,
	UnknownCandidate,
	WrongPhase,
	OperationFailed,
};

struct YouTubeAccountConnectionAttempt final {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;
};

constexpr bool operator==(const YouTubeAccountConnectionAttempt &left,
			  const YouTubeAccountConnectionAttempt &right) noexcept
{
	return left.generation == right.generation && left.attempt == right.attempt;
}

struct YouTubeAccountCandidateHandle final {
	std::uint64_t revision = 0;
	std::size_t index = 0;
};

struct YouTubeAccountConnectionCandidate final {
	YouTubeAccountCandidateHandle handle;
	std::string label;
};

// The connection facade never exposes Google resource identifiers or secret
// material. Candidate handles are valid only for the matching snapshot
// revision; labels are copied display values.
struct YouTubeAccountConnectionSnapshot final {
	std::uint64_t revision = 0;
	YouTubeAccountProviderStage stage = YouTubeAccountProviderStage::Idle;
	YouTubeAccountState state = YouTubeAccountState::Disconnected;
	YouTubeAccountFailure failure = YouTubeAccountFailure::None;
	std::optional<YouTubeAccountConnectionAttempt> activeAttempt;
	std::vector<YouTubeAccountConnectionCandidate> channels;
	std::vector<YouTubeAccountConnectionCandidate> streams;
	std::string channelLabel;
	std::string streamLabel;
	bool restored = false;
	bool closed = false;
};

class YouTubeAccountRuntimeOwner final {
public:
	using ProfilePathReader = YouTubeAccountProfileContext::ProfilePathReader;
	using ConfigReader = YouTubeAccountProfileContext::ConfigReader;

	// Production constructor. The empty client id/browser opener are
	// intentional until the account connection UI is integrated; restore never
	// starts those paths.
	YouTubeAccountRuntimeOwner(ProfilePathReader profilePathReader, ConfigReader configReader);

	// Dependency-injected constructor used by deterministic tests. The injected
	// APIs are owned by this object and must outlive the account store/lock
	// provider members below.
	YouTubeAccountRuntimeOwner(ProfilePathReader profilePathReader, ConfigReader configReader, QString clientId,
				   GoogleOAuthAuthorizationSession::BrowserOpener browserOpener,
				   std::unique_ptr<WinCredentialApi> credentialApi,
				   std::unique_ptr<YouTubeAccountProfileOperationLockApi> profileLockApi);

	YouTubeAccountRuntimeOwner(const YouTubeAccountRuntimeOwner &) = delete;
	YouTubeAccountRuntimeOwner &operator=(const YouTubeAccountRuntimeOwner &) = delete;

	~YouTubeAccountRuntimeOwner();

	YouTubeAccountProfileRestoreResult restoreActiveProfile() noexcept;
	// These headless operations are accepted only while the previously restored
	// account-mode profile is still the active profile. The production
	// constructor intentionally has no OAuth client id or browser opener yet, so
	// startConnection() fails before opening a listener, browser, or network path.
	YouTubeAccountConnectionOperationStatus
	startConnection(GoogleOAuthConsentMode consentMode = GoogleOAuthConsentMode::Standard) noexcept;
	YouTubeAccountConnectionOperationStatus selectChannel(YouTubeAccountConnectionAttempt attempt,
							      YouTubeAccountCandidateHandle candidate) noexcept;
	YouTubeAccountConnectionOperationStatus selectStream(YouTubeAccountConnectionAttempt attempt,
							     YouTubeAccountCandidateHandle candidate) noexcept;
	YouTubeAccountConnectionOperationStatus cancelConnection(YouTubeAccountConnectionAttempt attempt) noexcept;
	YouTubeAccountConnectionSnapshot connectionSnapshot() const;
	// Erases the active profile's saved account selection and credential while
	// keeping an account-mode profile ready for a later connection. The
	// coordinator owns the transaction-scoped selection commit; this owner only
	// publishes the resulting generation/binding after the transaction succeeds.
	YouTubeAccountProfileDisconnectResult disconnectActiveProfile() noexcept;
	bool invalidateForProfileChange() noexcept;
	bool shutdown() noexcept;

	YouTubeAccountRuntimeOwnerSnapshot snapshot() const;

private:
	friend class YouTubeAccountRuntimeOwnerTestAccess;

	bool commitSelection(const std::optional<YouTubeAccountSelection> &selection) noexcept;
	YouTubeAccountConnectionOperationStatus preflightConnection() noexcept;
	bool invalidateConnectionContext(YouTubeAccountProfileRestoreStatus status) noexcept;
	bool onOwnerThread() const noexcept;
	void clearRestoredBinding() noexcept;
	static bool isAcceptedRestore(const YouTubeAccountProfileRestoreResult &result) noexcept;
	static bool isUsableProfile(const YouTubeAccountProfileContext::Snapshot &snapshot) noexcept;
	static bool isAcceptedDisconnect(const YouTubeAccountProfileDisconnectResult &result) noexcept;
	YouTubeAccountProfileRestoreStatus
	mapDisconnectStatus(YouTubeAccountProfileDisconnectStatus status) const noexcept;

	YouTubeAccountProfileContext context_;
	std::unique_ptr<WinCredentialApi> credentialApi_;
	WindowsYouTubeAccountRefreshTokenStore refreshTokenStore_;
	std::unique_ptr<YouTubeAccountProfileOperationLockApi> profileLockApi_;
	YouTubeAccountProfileOperationLockProvider profileLockProvider_;
	std::unique_ptr<YouTubeAccountProvider> provider_;
	std::unique_ptr<YouTubeAccountProfileRestoreCoordinator> restoreCoordinator_;

	std::uint64_t restoredGeneration_ = 0;
	std::string restoredBinding_;
	YouTubeAccountProfileRestoreStatus restoreStatus_ = YouTubeAccountProfileRestoreStatus::Unavailable;
	bool restored_ = false;
	bool closed_ = false;
	bool shutdownComplete_ = false;
};

} // namespace easy_multistream

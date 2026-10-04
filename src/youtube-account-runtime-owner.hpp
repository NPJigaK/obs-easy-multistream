// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-account-profile-restore.hpp"
#include "windows-credential-vault.hpp"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace easy_multistream {

// This is the production lifecycle seam for the saved YouTube account state.
// It exposes restore/invalidation and an internal local-disconnect transaction;
// browser, token exchange, discovery, remote revoke, and output paths remain
// unavailable to OBS UI/runtime code.
// All methods are owner-thread-only and are called synchronously from OBS's
// frontend lifecycle callbacks.
struct YouTubeAccountRuntimeOwnerSnapshot final {
	YouTubeAccountProfileRestoreStatus restoreStatus = YouTubeAccountProfileRestoreStatus::Unavailable;
	YouTubeAccountProviderStage providerStage = YouTubeAccountProviderStage::Idle;
	std::uint64_t generation = 0;
	std::string profileBinding;
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
	YouTubeAccountRuntimeOwner(ProfilePathReader profilePathReader, ConfigReader configReader,
					  QString clientId,
					  GoogleOAuthAuthorizationSession::BrowserOpener browserOpener,
					  std::unique_ptr<WinCredentialApi> credentialApi,
					  std::unique_ptr<YouTubeAccountProfileOperationLockApi> profileLockApi);

	YouTubeAccountRuntimeOwner(const YouTubeAccountRuntimeOwner &) = delete;
	YouTubeAccountRuntimeOwner &operator=(const YouTubeAccountRuntimeOwner &) = delete;

	~YouTubeAccountRuntimeOwner();

	YouTubeAccountProfileRestoreResult restoreActiveProfile() noexcept;
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
	bool onOwnerThread() const noexcept;
	void clearRestoredBinding() noexcept;
	static bool isAcceptedRestore(const YouTubeAccountProfileRestoreResult &result) noexcept;
	static bool isUsableProfile(const YouTubeAccountProfileContext::Snapshot &snapshot) noexcept;
	static bool isAcceptedDisconnect(const YouTubeAccountProfileDisconnectResult &result) noexcept;
	YouTubeAccountProfileRestoreStatus mapDisconnectStatus(YouTubeAccountProfileDisconnectStatus status) const noexcept;

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

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
// It deliberately exposes restore/invalidation only: it does not expose the
// browser, token exchange, discovery, or output paths to OBS UI/runtime code.
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

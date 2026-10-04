// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "credential-vault.hpp"
#include "google-oauth-token-transport.hpp"
#include "youtube-account-profile-context.hpp"
#include "youtube-account-profile-operation-lock.hpp"
#include "youtube-account-provider.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace easy_multistream {

struct YouTubeAccountRemoteRevokeRequest final {
	std::uint64_t expectedGeneration = 0;
	std::string expectedProfileBinding;

	bool isValid() const noexcept
	{
		return expectedGeneration != 0 && isValidYouTubeAccountProfileBinding(expectedProfileBinding);
	}
};

struct YouTubeAccountRemoteRevokeAttempt final {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;

	bool isValid() const noexcept { return generation != 0 && attempt != 0; }
	bool operator==(const YouTubeAccountRemoteRevokeAttempt &other) const noexcept
	{
		return generation == other.generation && attempt == other.attempt;
	}
};

enum class YouTubeAccountRemoteRevokeState {
	Idle,
	ReadingCredential,
	Revoking,
	Disconnecting,
	DeliveringCompletion,
	Completed,
	Cancelled,
	Failed,
	Closed,
};

enum class YouTubeAccountRemoteRevokeStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	NotRestored,
	NotAccountMode,
	ProfileUnavailable,
	ProfileChanged,
	InvalidProfileBinding,
	InvalidSelection,
	CredentialUnavailable,
	OperationFailed,
};

enum class YouTubeAccountRemoteRevokeStatus {
	Revoked,
	AlreadyRevoked,
	CredentialUnavailable,
	RemoteRejected,
	NetworkFailure,
	ServiceUnavailable,
	InvalidResponse,
	CredentialEraseFailed,
	ProfileSaveFailed,
	ProfileChanged,
	Cancelled,
	Closed,
	OperationFailed,
};

struct YouTubeAccountRemoteRevokeCompletion final {
	YouTubeAccountRemoteRevokeAttempt attempt;
	YouTubeAccountRemoteRevokeStatus status = YouTubeAccountRemoteRevokeStatus::InvalidResponse;
	bool remoteRevokeAccepted = false;
	bool credentialErased = false;
	bool selectionCleared = false;
	// True only on the fail-closed native cleanup path. A caller must not
	// start another account operation until shutdown/invalidation retries it.
	bool lockCleanupPending = false;
	YouTubeAccountProfileContext::LoadResult profile;

	YouTubeAccountRemoteRevokeCompletion() = default;
	YouTubeAccountRemoteRevokeCompletion(const YouTubeAccountRemoteRevokeCompletion &) = delete;
	YouTubeAccountRemoteRevokeCompletion &operator=(const YouTubeAccountRemoteRevokeCompletion &) = delete;
	YouTubeAccountRemoteRevokeCompletion(YouTubeAccountRemoteRevokeCompletion &&) noexcept = default;
	YouTubeAccountRemoteRevokeCompletion &operator=(YouTubeAccountRemoteRevokeCompletion &&) noexcept = default;
};

// The revoke adapter is intentionally narrower than the account provider's
// exchange port. A remote revoke has no browser, channel discovery, or API
// resource payload; only the fixed Google token endpoint is involved.
// Implementations must deliver completions on the owner thread and must not
// invoke them synchronously from startRevoke(), cancel(), or shutdown().
class YouTubeAccountRemoteRevokePort {
public:
	using CompletionHandler = std::function<void(GoogleOAuthTokenCompletion)>;
	virtual ~YouTubeAccountRemoteRevokePort() = default;

	virtual GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
							CompletionHandler completionHandler) noexcept = 0;
	virtual bool cancel(GoogleOAuthTokenAttempt attempt) noexcept = 0;
	virtual bool shutdown() noexcept = 0;
};

// Owner-thread-only remote account revocation. It retains the profile lock
// from the credential read through Google's revoke request and the existing
// provider held-lock disconnect transaction. This prevents a second process
// from replacing a credential between revoke and local cleanup.
//
// The type is deliberately detached from RuntimeController, docks, and OBS
// output code. Its completion contains only copied non-secret values.
class YouTubeAccountRemoteRevokeCoordinator final : public QObject {
public:
	using CompletionHandler = std::function<void(YouTubeAccountRemoteRevokeCompletion)>;

	YouTubeAccountRemoteRevokeCoordinator(YouTubeAccountProfileContext &context, YouTubeAccountProvider &provider,
					      YouTubeAccountRefreshTokenStore &refreshTokenStore,
					      YouTubeAccountProfileOperationLockProvider &operationLockProvider,
					      QObject *parent = nullptr);
	~YouTubeAccountRemoteRevokeCoordinator() override;

	YouTubeAccountRemoteRevokeCoordinator(const YouTubeAccountRemoteRevokeCoordinator &) = delete;
	YouTubeAccountRemoteRevokeCoordinator &operator=(const YouTubeAccountRemoteRevokeCoordinator &) = delete;
	YouTubeAccountRemoteRevokeCoordinator(YouTubeAccountRemoteRevokeCoordinator &&) = delete;
	YouTubeAccountRemoteRevokeCoordinator &operator=(YouTubeAccountRemoteRevokeCoordinator &&) = delete;

	YouTubeAccountRemoteRevokeStartStatus start(YouTubeAccountRemoteRevokeRequest request,
						    CompletionHandler completionHandler) noexcept;
	bool cancel(YouTubeAccountRemoteRevokeAttempt attempt) noexcept;
	// Cancels the remote request and releases this coordinator's profile lock.
	// Profile generation invalidation remains the caller's lifecycle concern.
	bool invalidateContext() noexcept;
	bool shutdown() noexcept;

	YouTubeAccountRemoteRevokeState state() const noexcept;
	std::optional<YouTubeAccountRemoteRevokeAttempt> activeAttempt() const noexcept;
	bool lockHeld() const noexcept;

private:
	using QObject::moveToThread;
	friend class YouTubeAccountRemoteRevokeCoordinatorTestAccess;
	friend class YouTubeAccountRuntimeOwner;

	YouTubeAccountRemoteRevokeCoordinator(YouTubeAccountProfileContext &context, YouTubeAccountProvider &provider,
					      YouTubeAccountRefreshTokenStore &refreshTokenStore,
					      YouTubeAccountProfileOperationLockProvider &operationLockProvider,
					      std::unique_ptr<YouTubeAccountRemoteRevokePort> revokePort,
					      QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

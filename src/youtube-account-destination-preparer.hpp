// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "credential-vault.hpp"
#include "google-oauth-token-transport.hpp"
#include "youtube-account-selection.hpp"
#include "youtube-api-stream-resolver.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace easy_multistream {

struct YouTubeDestinationPrepareAttempt final {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;

	bool isValid() const noexcept { return generation != 0 && attempt != 0; }
	bool operator==(const YouTubeDestinationPrepareAttempt &other) const noexcept
	{
		return generation == other.generation && attempt == other.attempt;
	}
};

struct YouTubeDestinationPrepareRequest final {
	YouTubeDestinationPrepareAttempt attempt;
	// Non-secret binding supplied by the active OBS profile. It is copied into
	// the attempt scope before the credential store is touched and is never
	// read again from profile/config state while the operation is in flight.
	std::string profileBinding;
	YouTubeAccountSelection selection;
};

enum class YouTubeDestinationPreparerState {
	Idle,
	ReadingCredential,
	RefreshingAccessToken,
	PersistingRefreshToken,
	ResolvingStream,
	DeliveringCompletion,
	Completed,
	Cancelled,
	Closed,
};

enum class YouTubeDestinationPrepareStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	InvalidAttempt,
	InvalidClientId,
	InvalidProfileBinding,
	InvalidSelection,
	InvalidCompletionHandler,
	OperationFailed,
};

// These values are deliberately stable and contain no provider response text,
// token, URL, or stream key. UI and runtime layers can map them to localized
// status without learning about OAuth or YouTube transport details.
enum class YouTubeDestinationPrepareStatus {
	Success,
	ReauthorizationRequired,
	CredentialUnavailable,
	DestinationUnavailable,
	DestinationAlreadyActive,
	NetworkFailure,
	ServiceUnavailable,
	InvalidResponse,
	Cancelled,
};

struct YouTubeDestinationPrepareCompletion final {
	YouTubeDestinationPrepareAttempt attempt;
	YouTubeDestinationPrepareStatus status = YouTubeDestinationPrepareStatus::InvalidResponse;
	std::optional<YouTubeResolvedIngestion> ingestion;

	YouTubeDestinationPrepareCompletion() = default;
	YouTubeDestinationPrepareCompletion(const YouTubeDestinationPrepareCompletion &) = delete;
	YouTubeDestinationPrepareCompletion &operator=(const YouTubeDestinationPrepareCompletion &) = delete;
	YouTubeDestinationPrepareCompletion(YouTubeDestinationPrepareCompletion &&) noexcept = default;
	YouTubeDestinationPrepareCompletion &operator=(YouTubeDestinationPrepareCompletion &&) noexcept = default;

	bool succeeded() const noexcept
	{
		return status == YouTubeDestinationPrepareStatus::Success && ingestion.has_value();
	}
};

// Narrow owner-thread ports keep the orchestration deterministic in tests.
// Implementations must invoke completions on the preparer's owner thread.
// Production adapters below the private constructor wrap only the fixed Google
// token endpoint and fixed YouTube API endpoint implementations.
class YouTubeDestinationRefreshPort {
public:
	using CompletionHandler = std::function<void(GoogleOAuthTokenCompletion)>;
	virtual ~YouTubeDestinationRefreshPort() = default;
	virtual GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
							 CompletionHandler completionHandler) noexcept = 0;
	virtual bool cancel(GoogleOAuthTokenAttempt attempt) noexcept = 0;
	virtual bool shutdown() noexcept = 0;
};

class YouTubeDestinationResolverPort {
public:
	using CompletionHandler = std::function<void(YouTubeStreamResolverCompletion)>;
	virtual ~YouTubeDestinationResolverPort() = default;
	virtual YouTubeStreamResolverStartStatus startResolveStream(YouTubeResolveStreamRequest request,
								    CompletionHandler completionHandler) noexcept = 0;
	virtual bool cancel(YouTubeApiAttempt attempt) noexcept = 0;
	virtual bool shutdown() noexcept = 0;
};

// Owner-thread-only preparation of one selected account destination. It reads
// the saved refresh token just in time, refreshes the access token, durably
// stores any rotated refresh token, and resolves a fresh RTMPS destination.
// Only the move-only successful completion can contain a stream key.
//
// This layer intentionally remains detached from PluginState, RuntimeController,
// docks, and the OBS module until its lifecycle is independently verified.
class YouTubeAccountDestinationPreparer final : public QObject {
public:
	using CompletionHandler = std::function<void(YouTubeDestinationPrepareCompletion)>;

	YouTubeAccountDestinationPreparer(QString clientId, YouTubeAccountRefreshTokenStore &refreshTokenStore,
					  QObject *parent = nullptr);
	~YouTubeAccountDestinationPreparer() override;

	YouTubeAccountDestinationPreparer(const YouTubeAccountDestinationPreparer &) = delete;
	YouTubeAccountDestinationPreparer &operator=(const YouTubeAccountDestinationPreparer &) = delete;
	YouTubeAccountDestinationPreparer(YouTubeAccountDestinationPreparer &&) = delete;
	YouTubeAccountDestinationPreparer &operator=(YouTubeAccountDestinationPreparer &&) = delete;

	YouTubeDestinationPrepareStartStatus start(YouTubeDestinationPrepareRequest request,
						   CompletionHandler completionHandler) noexcept;
	bool cancel(YouTubeDestinationPrepareAttempt attempt) noexcept;
	bool invalidateContext() noexcept;
	bool shutdown() noexcept;

	YouTubeDestinationPreparerState state() const noexcept;
	std::optional<YouTubeDestinationPrepareAttempt> activeAttempt() const noexcept;

private:
	using QObject::moveToThread;
	friend class YouTubeAccountDestinationPreparerTestAccess;

	YouTubeAccountDestinationPreparer(QString clientId, std::unique_ptr<YouTubeDestinationRefreshPort> refreshPort,
					  std::unique_ptr<YouTubeDestinationResolverPort> resolverPort,
					  YouTubeAccountRefreshTokenStore &refreshTokenStore, QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

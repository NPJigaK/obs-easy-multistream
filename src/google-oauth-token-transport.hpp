// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QtGlobal>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

namespace easy_multistream {

inline constexpr char kGoogleOAuthTokenEndpoint[] = "https://oauth2.googleapis.com/token";
inline constexpr char kGoogleOAuthRevokeEndpoint[] = "https://oauth2.googleapis.com/revoke";
inline constexpr std::size_t kGoogleOAuthMaxAuthorizationCodeBytes = 256U;
inline constexpr std::size_t kGoogleOAuthMaxAccessTokenBytes = 2048U;
inline constexpr std::size_t kGoogleOAuthMaxRefreshTokenBytes = 512U;
inline constexpr std::size_t kGoogleOAuthMinPkceVerifierBytes = 43U;
inline constexpr std::size_t kGoogleOAuthMaxPkceVerifierBytes = 128U;
inline constexpr qsizetype kGoogleOAuthMaxTokenResponseBytes = 64 * 1024;
// Defensive local acceptance ceiling, not a Google protocol guarantee. Normal
// Google access tokens are far shorter-lived; this prevents a malformed reply
// from scheduling refresh decades in the future while leaving ample margin.
inline constexpr std::uint32_t kGoogleOAuthMaxExpiresInSeconds = 31U * 24U * 60U * 60U;

struct GoogleOAuthTokenAttempt final {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;

	bool isValid() const noexcept { return generation != 0 && attempt != 0; }
	bool operator==(const GoogleOAuthTokenAttempt &other) const noexcept
	{
		return generation == other.generation && attempt == other.attempt;
	}
};

struct GoogleOAuthTokenTransportOptions final {
	std::chrono::milliseconds operationTimeout{std::chrono::seconds(30)};
	qsizetype maxResponseBytes = kGoogleOAuthMaxTokenResponseBytes;
};

enum class GoogleOAuthTokenOperation {
	ExchangeAuthorizationCode,
	RefreshAccessToken,
	RevokeToken,
};

enum class GoogleOAuthTokenTransportState {
	Idle,
	InFlight,
	Completed,
	Cancelled,
	Closed,
};

enum class GoogleOAuthTokenStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	InvalidOptions,
	InvalidAttempt,
	InvalidClientId,
	InvalidRedirectUri,
	InvalidAuthorizationCode,
	InvalidCodeVerifier,
	InvalidRefreshToken,
	InvalidToken,
	InvalidCompletionHandler,
	RequestCreationFailed,
};

enum class GoogleOAuthTokenCompletionStatus {
	Success,
	ProviderRejected,
	InvalidResponse,
	NetworkFailure,
	TlsFailure,
	RedirectRejected,
	ResponseTooLarge,
	TimedOut,
	HttpFailure,
};

enum class GoogleOAuthTokenProviderError {
	None,
	InvalidRequest,
	InvalidGrant,
	InvalidClient,
	UnauthorizedClient,
	InvalidScope,
	AccessDenied,
	InvalidToken,
	TemporarilyUnavailable,
	RateLimited,
	Unknown,
};

struct GoogleOAuthTokenSet final {
	SecureBuffer accessToken;
	// Exchange returns the newly issued refresh token. Refresh usually leaves
	// this empty, which means the provider must retain its existing credential.
	// A non-empty value is a validated rotation candidate, not a committed
	// credential; only the future account provider writes Credential Manager.
	SecureBuffer refreshToken;
	std::uint32_t expiresInSeconds = 0;

	GoogleOAuthTokenSet() = default;
	GoogleOAuthTokenSet(const GoogleOAuthTokenSet &) = delete;
	GoogleOAuthTokenSet &operator=(const GoogleOAuthTokenSet &) = delete;
	GoogleOAuthTokenSet(GoogleOAuthTokenSet &&) noexcept = default;
	GoogleOAuthTokenSet &operator=(GoogleOAuthTokenSet &&) noexcept = default;

	bool hasAccessToken() const noexcept { return !accessToken.empty(); }
	bool hasRefreshToken() const noexcept { return !refreshToken.empty(); }
};

struct GoogleOAuthTokenCompletion final {
	GoogleOAuthTokenAttempt attempt;
	GoogleOAuthTokenOperation operation = GoogleOAuthTokenOperation::ExchangeAuthorizationCode;
	GoogleOAuthTokenCompletionStatus status = GoogleOAuthTokenCompletionStatus::InvalidResponse;
	GoogleOAuthTokenProviderError providerError = GoogleOAuthTokenProviderError::None;
	int httpStatus = 0;
	GoogleOAuthTokenSet tokens;

	GoogleOAuthTokenCompletion() = default;
	GoogleOAuthTokenCompletion(const GoogleOAuthTokenCompletion &) = delete;
	GoogleOAuthTokenCompletion &operator=(const GoogleOAuthTokenCompletion &) = delete;
	GoogleOAuthTokenCompletion(GoogleOAuthTokenCompletion &&) noexcept = default;
	GoogleOAuthTokenCompletion &operator=(GoogleOAuthTokenCompletion &&) noexcept = default;

	bool succeeded() const noexcept
	{
		if (status != GoogleOAuthTokenCompletionStatus::Success) {
			return false;
		}
		return operation == GoogleOAuthTokenOperation::RevokeToken || tokens.hasAccessToken();
	}
};

struct GoogleOAuthTokenExchangeRequest final {
	GoogleOAuthTokenAttempt attempt;
	QString clientId;
	QUrl redirectUri;
	SecureBuffer authorizationCode;
	SecureBuffer codeVerifier;

	GoogleOAuthTokenExchangeRequest() = default;
	GoogleOAuthTokenExchangeRequest(const GoogleOAuthTokenExchangeRequest &) = delete;
	GoogleOAuthTokenExchangeRequest &operator=(const GoogleOAuthTokenExchangeRequest &) = delete;
	GoogleOAuthTokenExchangeRequest(GoogleOAuthTokenExchangeRequest &&) noexcept = default;
	GoogleOAuthTokenExchangeRequest &operator=(GoogleOAuthTokenExchangeRequest &&) noexcept = default;
};

struct GoogleOAuthTokenRefreshRequest final {
	GoogleOAuthTokenAttempt attempt;
	QString clientId;
	SecureBuffer refreshToken;

	GoogleOAuthTokenRefreshRequest() = default;
	GoogleOAuthTokenRefreshRequest(const GoogleOAuthTokenRefreshRequest &) = delete;
	GoogleOAuthTokenRefreshRequest &operator=(const GoogleOAuthTokenRefreshRequest &) = delete;
	GoogleOAuthTokenRefreshRequest(GoogleOAuthTokenRefreshRequest &&) noexcept = default;
	GoogleOAuthTokenRefreshRequest &operator=(GoogleOAuthTokenRefreshRequest &&) noexcept = default;
};

struct GoogleOAuthTokenRevokeRequest final {
	GoogleOAuthTokenAttempt attempt;
	SecureBuffer token;

	GoogleOAuthTokenRevokeRequest() = default;
	GoogleOAuthTokenRevokeRequest(const GoogleOAuthTokenRevokeRequest &) = delete;
	GoogleOAuthTokenRevokeRequest &operator=(const GoogleOAuthTokenRevokeRequest &) = delete;
	GoogleOAuthTokenRevokeRequest(GoogleOAuthTokenRevokeRequest &&) noexcept = default;
	GoogleOAuthTokenRevokeRequest &operator=(GoogleOAuthTokenRevokeRequest &&) noexcept = default;
};

// Owner-thread-only HTTPS transport for Google's fixed token and revoke
// endpoints. It never accepts an endpoint URL from settings or callers, never
// sends a client secret, and never exposes raw provider text or response
// bodies. Cancellation is silent and invalidates all queued completion work.
//
// This library remains detached from the OBS module until the complete account
// provider is ready. A private test friend can replace the manager so the
// standalone suite exercises request/lifetime code without contacting Google;
// production callers cannot inject a different network implementation.
class GoogleOAuthTokenTransport final : public QObject {
public:
	using CompletionHandler = std::function<void(GoogleOAuthTokenCompletion)>;

	explicit GoogleOAuthTokenTransport(GoogleOAuthTokenTransportOptions options = {}, QObject *parent = nullptr);
	~GoogleOAuthTokenTransport() override;

	GoogleOAuthTokenTransport(const GoogleOAuthTokenTransport &) = delete;
	GoogleOAuthTokenTransport &operator=(const GoogleOAuthTokenTransport &) = delete;
	GoogleOAuthTokenTransport(GoogleOAuthTokenTransport &&) = delete;
	GoogleOAuthTokenTransport &operator=(GoogleOAuthTokenTransport &&) = delete;

	GoogleOAuthTokenStartStatus startExchange(GoogleOAuthTokenExchangeRequest request,
						  CompletionHandler completionHandler) noexcept;
	GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
						 CompletionHandler completionHandler) noexcept;
	GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
						CompletionHandler completionHandler) noexcept;

	// Silent and idempotent for the matching operation. A mismatched attempt
	// cannot cancel newer work. Callers synchronously update their own state.
	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept;
	// Returns false without mutating state when called from the wrong thread.
	// Integration owners must synchronously dispatch shutdown on the owner
	// thread before destroying the transport or ending the Qt event loop.
	bool shutdown() noexcept;

	GoogleOAuthTokenTransportState state() const noexcept;
	std::optional<GoogleOAuthTokenAttempt> activeAttempt() const noexcept;
	std::optional<GoogleOAuthTokenOperation> activeOperation() const noexcept;

private:
	using QObject::moveToThread;
	friend class GoogleOAuthTokenTransportTestAccess;

	// Test-only dependency injection. Production callers can construct only the
	// transport-owned QNetworkAccessManager path above.
	GoogleOAuthTokenTransport(std::unique_ptr<QNetworkAccessManager> networkManager,
				  GoogleOAuthTokenTransportOptions options, QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

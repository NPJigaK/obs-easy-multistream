// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <cstddef>

namespace easy_multistream {

// These values are deliberately kept in one place.  The protocol core does
// not open a browser, create a listener, or perform an HTTP request; callers
// own those integration concerns.
inline constexpr std::size_t kGoogleOAuthRandomBytes = 32U;
inline constexpr qsizetype kGoogleOAuthEncodedRandomLength = 43;
inline constexpr qsizetype kGoogleOAuthMaxClientIdCharacters = 1024;
inline constexpr qsizetype kGoogleOAuthMaxCallbackUrlBytes = 16 * 1024;
inline constexpr qsizetype kGoogleOAuthMaxCallbackParameters = 16;
inline constexpr qsizetype kGoogleOAuthMaxCallbackNameCharacters = 128;
inline constexpr qsizetype kGoogleOAuthMaxCallbackValueCharacters = 8 * 1024;
inline constexpr char kGoogleOAuthAuthorizationEndpoint[] =
	"https://accounts.google.com/o/oauth2/v2/auth";
inline constexpr char kGoogleOAuthYouTubeReadonlyScope[] =
	"https://www.googleapis.com/auth/youtube.readonly";
inline constexpr char kGoogleOAuthLoopbackHost[] = "127.0.0.1";
inline constexpr char kGoogleOAuthCallbackPath[] = "/callback";

// Errors returned while constructing an authorization attempt.  No raw input
// is retained in the error value, which keeps this result safe to put in a
// diagnostic or state snapshot.
enum class GoogleOAuthRequestStatus {
	Success,
	InvalidClientId,
	InvalidRedirectPort,
	RandomGenerationFailed,
};

enum class GoogleOAuthConsentMode {
	Standard,
	ForceConsent,
};

// Callback validation is intentionally classified into safe, non-secret
// states.  In particular, ProviderError never contains Google's raw `error`
// or `error_description` values.
enum class GoogleOAuthCallbackStatus {
	Success,
	InvalidCallbackUrl,
	OriginMismatch,
	PathMismatch,
	FragmentNotAllowed,
	UserInfoNotAllowed,
	DuplicateQueryParameter,
	InvalidQuery,
	MissingState,
	StateMismatch,
	ProviderError,
	CodeAndError,
	MissingCode,
	InvalidCode,
};

enum class GoogleOAuthProviderError {
	None,
	InvalidRequest,
	AccessDenied,
	ServerError,
	TemporarilyUnavailable,
	Unknown,
};

// An authorization attempt owns the two values needed to validate the
// eventual callback.  They are move-only wipeable buffers.  `authorizationUrl`
// necessarily contains the public state/challenge query values because it is
// the URL handed to a system browser; the protocol core provides no snapshot
// or logging API for it.
struct GoogleOAuthAuthorizationRequest final {
	QUrl authorizationUrl;
	QUrl redirectUri;
	SecureBuffer codeVerifier;
	SecureBuffer state;

	GoogleOAuthAuthorizationRequest() = default;
	GoogleOAuthAuthorizationRequest(const GoogleOAuthAuthorizationRequest &) = delete;
	GoogleOAuthAuthorizationRequest &operator=(const GoogleOAuthAuthorizationRequest &) = delete;
	GoogleOAuthAuthorizationRequest(GoogleOAuthAuthorizationRequest &&) noexcept = default;
	GoogleOAuthAuthorizationRequest &operator=(GoogleOAuthAuthorizationRequest &&) noexcept = default;

	bool isValid() const noexcept
	{
		return !authorizationUrl.isEmpty() && !redirectUri.isEmpty() && !codeVerifier.empty() && !state.empty();
	}
};

struct GoogleOAuthAuthorizationResult final {
	GoogleOAuthRequestStatus status = GoogleOAuthRequestStatus::RandomGenerationFailed;
	GoogleOAuthAuthorizationRequest request;

	bool succeeded() const noexcept { return status == GoogleOAuthRequestStatus::Success && request.isValid(); }
};

struct GoogleOAuthCallbackResult final {
	GoogleOAuthCallbackStatus status = GoogleOAuthCallbackStatus::InvalidCallbackUrl;
	GoogleOAuthProviderError providerError = GoogleOAuthProviderError::None;
	SecureBuffer authorizationCode;

	// This is the only success path that returns an authorization code.  It is
	// move-only and wipeable; no raw code or provider error text is exposed.
	bool succeeded() const noexcept
	{
		return status == GoogleOAuthCallbackStatus::Success && !authorizationCode.empty();
	}

	bool hasProviderError() const noexcept { return status == GoogleOAuthCallbackStatus::ProviderError; }
};

// RFC 4648 base64url using the URL-safe alphabet and no trailing '=' signs.
// The returned QByteArray is owned by the caller.  Internal secret-bearing
// callers wipe their input/output temporaries after copying into SecureBuffer.
QByteArray googleOAuthBase64UrlNoPadding(const QByteArray &bytes);

// Compute the RFC 7636 S256 challenge for an ASCII PKCE verifier.  The
// verifier is not retained.  An empty verifier produces an empty result.
QByteArray googleOAuthPkceS256Challenge(const QByteArray &codeVerifier);

// Generate one 32-byte random value with QRandomGenerator::system() and
// encode it as the 43-character base64url token used by PKCE and state.
// Generation failure is represented by an empty SecureBuffer.
SecureBuffer generateGoogleOAuthRandomToken();
SecureBuffer generateGoogleOAuthPkceVerifier();
SecureBuffer generateGoogleOAuthState();

// Build the fixed Google installed-app authorization request for a loopback
// callback.  `port` is checked as an integer so callers cannot accidentally
// wrap 0 or a value above 65535 before validation.
GoogleOAuthAuthorizationResult makeGoogleOAuthAuthorizationRequest(
	const QString &clientId, int port, GoogleOAuthConsentMode consentMode = GoogleOAuthConsentMode::Standard);

// Validate and classify one browser callback against the exact redirect URI,
// state, and one active authorization attempt.  A successful result contains
// only a SecureBuffer authorization code.  The callback URL is not retained
// by this API, so callers may release their raw callback URL immediately after
// this call.
GoogleOAuthCallbackResult parseGoogleOAuthCallback(
	const GoogleOAuthAuthorizationRequest &request, const QUrl &callbackUrl);

// Lower-level form useful to a listener that stores only the redirect URI and
// expected state.  It has the same no-raw-secret result boundary as the
// request overload.
GoogleOAuthCallbackResult parseGoogleOAuthCallback(
	const QUrl &expectedRedirectUri, const SecureBuffer &expectedState, const QUrl &callbackUrl);

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "google-oauth-loopback-listener.hpp"

#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

namespace easy_multistream {

enum class GoogleOAuthAuthorizationSessionState {
	Idle,
	Authorizing,
	DeliveringCompletion,
	Completed,
	Cancelled,
	Closed,
};

enum class GoogleOAuthAuthorizationStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	Cancelled,
	InvalidAttempt,
	InvalidBrowserOpener,
	InvalidCompletionHandler,
	ListenerBindFailed,
	AuthorizationRequestFailed,
	ListenerArmFailed,
	BrowserOpenFailed,
};

enum class GoogleOAuthAuthorizationCompletionStatus {
	Success,
	ProviderRejected,
	CallbackRejected,
	AuthorizationTimedOut,
	RequestLimitReached,
	ListenerFailure,
};

// A successful result contains exactly the values needed by the token
// exchange. It never retains the authorization URL, state, callback URL, or
// provider text. Failure results contain no secret buffers or redirect URI.
struct GoogleOAuthAuthorizationCompletion final {
	GoogleOAuthLoopbackAttempt attempt;
	GoogleOAuthAuthorizationCompletionStatus status =
		GoogleOAuthAuthorizationCompletionStatus::ListenerFailure;
	GoogleOAuthCallbackStatus callbackStatus = GoogleOAuthCallbackStatus::InvalidCallbackUrl;
	GoogleOAuthProviderError providerError = GoogleOAuthProviderError::None;
	QUrl redirectUri;
	SecureBuffer authorizationCode;
	SecureBuffer codeVerifier;

	GoogleOAuthAuthorizationCompletion() = default;
	GoogleOAuthAuthorizationCompletion(const GoogleOAuthAuthorizationCompletion &) = delete;
	GoogleOAuthAuthorizationCompletion &operator=(const GoogleOAuthAuthorizationCompletion &) = delete;
	GoogleOAuthAuthorizationCompletion(GoogleOAuthAuthorizationCompletion &&) noexcept = default;
	GoogleOAuthAuthorizationCompletion &operator=(GoogleOAuthAuthorizationCompletion &&) noexcept = default;

	bool succeeded() const noexcept
	{
		return status == GoogleOAuthAuthorizationCompletionStatus::Success && attempt.isValid() &&
		       !redirectUri.isEmpty() && !authorizationCode.empty() && !codeVerifier.empty();
	}
};

// Owner-thread-only orchestration for one installed-app authorization. The
// injected opener is the only browser boundary: the future production
// provider supplies QDesktopServices::openUrl, while standalone tests supply
// a fake and never open a real browser.
//
// The ordering is fixed: bind the exclusive 127.0.0.1 listener, create the
// PKCE request for its assigned port, arm state validation, then invoke the
// browser opener. Cancellation is silent and attempt-scoped, matching the
// token/API transports. Shutdown is terminal and suppresses queued callbacks.
class GoogleOAuthAuthorizationSession final : public QObject {
public:
	using BrowserOpener = std::function<bool(const QUrl &authorizationUrl)>;
	using CompletionHandler = std::function<void(GoogleOAuthAuthorizationCompletion)>;

	explicit GoogleOAuthAuthorizationSession(BrowserOpener browserOpener,
					 GoogleOAuthLoopbackOptions listenerOptions = {}, QObject *parent = nullptr);
	~GoogleOAuthAuthorizationSession() override;

	GoogleOAuthAuthorizationSession(const GoogleOAuthAuthorizationSession &) = delete;
	GoogleOAuthAuthorizationSession &operator=(const GoogleOAuthAuthorizationSession &) = delete;
	GoogleOAuthAuthorizationSession(GoogleOAuthAuthorizationSession &&) = delete;
	GoogleOAuthAuthorizationSession &operator=(GoogleOAuthAuthorizationSession &&) = delete;

	GoogleOAuthAuthorizationStartStatus start(
		GoogleOAuthLoopbackAttempt attempt, const QString &clientId, GoogleOAuthConsentMode consentMode,
		CompletionHandler completionHandler) noexcept;

	// Silent and idempotent for the matching operation. A stale owner cannot
	// cancel a newer authorization attempt.
	bool cancel(GoogleOAuthLoopbackAttempt attempt) noexcept;
	// Returns false without changing state when called from the wrong thread.
	// Once closed, subsequent owner-thread calls remain successful no-ops.
	bool shutdown() noexcept;

	GoogleOAuthAuthorizationSessionState state() const noexcept;
	std::optional<GoogleOAuthLoopbackAttempt> activeAttempt() const noexcept;

private:
	using QObject::moveToThread;

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

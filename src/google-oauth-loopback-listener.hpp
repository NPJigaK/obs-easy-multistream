// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "google-oauth-protocol.hpp"

#include <QObject>
#include <QUrl>
#include <QtGlobal>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace easy_multistream {

inline constexpr qsizetype kGoogleOAuthLoopbackMaxRequestLineBytes = kGoogleOAuthMaxCallbackUrlBytes + 32;
inline constexpr qsizetype kGoogleOAuthLoopbackMaxHeaderBytes = 8 * 1024;
inline constexpr qsizetype kGoogleOAuthLoopbackMaxHeaderLineBytes = 2 * 1024;
inline constexpr qsizetype kGoogleOAuthLoopbackMaxHeaderCount = 32;

struct GoogleOAuthLoopbackAttempt final {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;

	bool isValid() const noexcept { return generation != 0 && attempt != 0; }
	bool operator==(const GoogleOAuthLoopbackAttempt &other) const noexcept
	{
		return generation == other.generation && attempt == other.attempt;
	}
};

struct GoogleOAuthLoopbackOptions final {
	std::chrono::milliseconds authorizationTimeout{std::chrono::minutes(2)};
	std::chrono::milliseconds clientReadTimeout{std::chrono::seconds(5)};
	std::size_t maxConcurrentConnections = 4;
	std::size_t maxTotalConnections = 16;
};

enum class GoogleOAuthLoopbackState {
	Idle,
	Listening,
	Armed,
	Completed,
	TimedOut,
	RequestLimitReached,
	NetworkFailed,
	Cancelled,
};

enum class GoogleOAuthLoopbackBindStatus {
	Success,
	WrongThread,
	InvalidAttempt,
	InvalidOptions,
	AlreadyActive,
	NetworkInitializationFailed,
	SocketCreationFailed,
	ExclusiveAddressFailed,
	BindFailed,
	ListenFailed,
	SocketAdoptionFailed,
};

struct GoogleOAuthLoopbackBindResult final {
	GoogleOAuthLoopbackBindStatus status = GoogleOAuthLoopbackBindStatus::BindFailed;
	quint16 port = 0;

	bool succeeded() const noexcept { return status == GoogleOAuthLoopbackBindStatus::Success && port != 0; }
};

enum class GoogleOAuthLoopbackArmStatus {
	Success,
	WrongThread,
	NotListening,
	InvalidRedirectUri,
	InvalidState,
	InvalidCompletionHandler,
};

enum class GoogleOAuthLoopbackCompletionStatus {
	Callback,
	AuthorizationTimedOut,
	RequestLimitReached,
	ListenerFailure,
};

struct GoogleOAuthLoopbackCompletion final {
	GoogleOAuthLoopbackCompletionStatus status = GoogleOAuthLoopbackCompletionStatus::ListenerFailure;
	GoogleOAuthLoopbackAttempt attempt;
	GoogleOAuthCallbackResult callback;

	GoogleOAuthLoopbackCompletion() = default;
	GoogleOAuthLoopbackCompletion(const GoogleOAuthLoopbackCompletion &) = delete;
	GoogleOAuthLoopbackCompletion &operator=(const GoogleOAuthLoopbackCompletion &) = delete;
	GoogleOAuthLoopbackCompletion(GoogleOAuthLoopbackCompletion &&) noexcept = default;
	GoogleOAuthLoopbackCompletion &operator=(GoogleOAuthLoopbackCompletion &&) noexcept = default;
};

// One short-lived, owner-thread-only HTTP listener for the installed-app OAuth
// callback.  It binds before the authorization URL is created, accepts only
// 127.0.0.1, and never exposes raw HTTP requests or provider error strings.
//
// The listener is deliberately separate from the OBS module and is not
// instantiated by the current product runtime.  A future provider owns one
// instance per authorization attempt and retains the PKCE verifier itself.
class GoogleOAuthLoopbackListener final : public QObject {
public:
	using CompletionHandler = std::function<void(GoogleOAuthLoopbackCompletion)>;

	explicit GoogleOAuthLoopbackListener(GoogleOAuthLoopbackOptions options = {}, QObject *parent = nullptr);
	~GoogleOAuthLoopbackListener() override;

	GoogleOAuthLoopbackListener(const GoogleOAuthLoopbackListener &) = delete;
	GoogleOAuthLoopbackListener &operator=(const GoogleOAuthLoopbackListener &) = delete;
	GoogleOAuthLoopbackListener(GoogleOAuthLoopbackListener &&) = delete;
	GoogleOAuthLoopbackListener &operator=(GoogleOAuthLoopbackListener &&) = delete;

	// Creates an IPv4 loopback socket with an OS-assigned port.  On Windows,
	// SO_EXCLUSIVEADDRUSE is set before bind so another local process cannot
	// replace the callback endpoint with SO_REUSEADDR.
	GoogleOAuthLoopbackBindResult bind(GoogleOAuthLoopbackAttempt attempt) noexcept;

	// Arms a bound listener after makeGoogleOAuthAuthorizationRequest() has
	// used port().  expectedState is consumed even when validation fails.
	GoogleOAuthLoopbackArmStatus arm(const QUrl &expectedRedirectUri, SecureBuffer expectedState,
					 CompletionHandler completionHandler) noexcept;

	// Cancellation is silent and idempotent.  The owner already knows it
	// requested cancellation, so no re-entrant completion is emitted.
	void cancel() noexcept;

	GoogleOAuthLoopbackState state() const noexcept;
	quint16 port() const noexcept;
	GoogleOAuthLoopbackAttempt attempt() const noexcept;

private:
	// QObject::moveToThread() is intentionally hidden. The listener and its
	// internal server/timers are created, used, and destroyed on one thread.
	using QObject::moveToThread;

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

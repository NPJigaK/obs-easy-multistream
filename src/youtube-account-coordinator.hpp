// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-account-selection.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace easy_multistream {

// This value-only state machine is deliberately independent of Qt, OBS,
// browsers, HTTP, and credential storage. Its owner must serialize calls on
// one thread and post only copied leases and non-secret discovery values.
enum class YouTubeAccountState {
	Disconnected,
	Authorizing,
	ExchangingCode,
	Discovering,
	PersistingCredential,
	Connected,
	NeedsReauthorization,
	Failed,
	Unavailable,
	Closed,
};

// Failures are stable, non-sensitive classifications. Raw provider or HTTP
// text never enters this state machine.
enum class YouTubeAccountFailure {
	None,
	AuthorizationRejected,
	CallbackRejected,
	TokenExchangeFailed,
	ReauthorizationRequired,
	NoCompatibleStream,
	CredentialUnavailable,
	ServiceUnavailable,
	NetworkFailure,
	InvalidResponse,
	DiscoveryFailed,
	Cancelled,
};

struct YouTubeAccountLease {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;
};

constexpr bool operator==(const YouTubeAccountLease &left, const YouTubeAccountLease &right) noexcept
{
	return left.generation == right.generation && left.attempt == right.attempt;
}

constexpr bool operator!=(const YouTubeAccountLease &left, const YouTubeAccountLease &right) noexcept
{
	return !(left == right);
}

// Keep the pre-existing coordinator-facing name as a compatibility alias.
// The shared selection model is also used by profile persistence and future
// account-provider code, so its validation cannot remain coordinator-local.
using YouTubeAccountDiscovery = YouTubeAccountSelection;

struct YouTubeAccountSnapshot {
	std::uint64_t generation = 1;
	std::uint64_t revision = 0;
	YouTubeAccountState state = YouTubeAccountState::Disconnected;
	// `lease` identifies the interactive attempt in progress. A connected
	// account retains its originating `connectionLease` so delayed refresh or
	// API failures from a replaced account cannot invalidate the new account.
	std::optional<YouTubeAccountLease> lease;
	std::optional<YouTubeAccountLease> connectionLease;
	std::string channelId;
	std::string channelLabel;
	std::string streamId;
	std::string streamLabel;
	YouTubeAccountFailure failure = YouTubeAccountFailure::None;
};

struct YouTubeAccountTransition {
	YouTubeAccountSnapshot snapshot;
	bool changed = false;
};

class YouTubeAccountCoordinator final {
public:
	// Return a value so callers cannot retain a reference across a transition,
	// queued UI delivery, or destruction of the coordinator.
	YouTubeAccountSnapshot snapshot() const;

	// A second begin while an attempt is active is a no-op. Reauthorizing a
	// connected account stages a replacement while preserving the committed
	// selection until discovery and credential storage both succeed.
	YouTubeAccountTransition beginAuthorization();
	YouTubeAccountTransition authorizationCallbackAccepted(YouTubeAccountLease lease);
	YouTubeAccountTransition tokenExchangeSucceeded(YouTubeAccountLease lease);
	YouTubeAccountTransition discoverySucceeded(YouTubeAccountLease lease,
						    const YouTubeAccountDiscovery &discovery);
	// The provider stages the new refresh token until discovery succeeds, then
	// persists it and reports that commit here. A replacement connection is not
	// made visible before both discovery and credential storage have completed.
	// The vault write and this call must run back-to-back in one serialized owner-
	// thread dispatch without yielding to the event loop. An asynchronous vault
	// implementation instead needs attempt-scoped staging and rollback.
	YouTubeAccountTransition credentialStored(YouTubeAccountLease lease);
	YouTubeAccountTransition attemptFailed(YouTubeAccountLease lease, YouTubeAccountFailure failure);
	YouTubeAccountTransition cancel(YouTubeAccountLease lease);

	// Context invalidation discards the committed selection and invalidates all
	// outstanding work. Closed is terminal.
	YouTubeAccountTransition invalidateContext();
	YouTubeAccountTransition markUnavailable(YouTubeAccountLease connectionLease, YouTubeAccountFailure failure);
	YouTubeAccountTransition markNeedsReauthorization(YouTubeAccountLease connectionLease);
	YouTubeAccountTransition shutdown();

private:
	struct CommittedConnection {
		YouTubeAccountDiscovery discovery;
		YouTubeAccountLease lease;
	};

	YouTubeAccountTransition result(bool changed = false);
	YouTubeAccountTransition finishAttempt(YouTubeAccountFailure failure);
	bool accepts(YouTubeAccountLease lease) const noexcept;
	bool attemptActive() const noexcept;
	void clearVisibleConnection() noexcept;
	void clearAllConnections() noexcept;
	void restoreCommittedConnection();
	void invalidateGeneration() noexcept;
	YouTubeAccountLease nextLease() noexcept;

	YouTubeAccountSnapshot snapshot_;
	std::uint64_t nextAttempt_ = 0;
	std::optional<CommittedConnection> committedConnection_;
	std::optional<YouTubeAccountDiscovery> pendingDiscovery_;
	std::optional<YouTubeAccountFailure> committedConnectionFailure_;
	bool replacingConnected_ = false;
};

} // namespace easy_multistream

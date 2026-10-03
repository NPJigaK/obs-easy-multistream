// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-coordinator.hpp"

#include <limits>
#include <type_traits>
#include <utility>

namespace easy_multistream {
namespace {

std::uint64_t nextNonZero(std::uint64_t value) noexcept
{
	return value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

bool isAttemptState(YouTubeAccountState state) noexcept
{
	return state == YouTubeAccountState::Authorizing || state == YouTubeAccountState::ExchangingCode ||
	       state == YouTubeAccountState::Discovering || state == YouTubeAccountState::PersistingCredential;
}

bool isUnavailableFailure(YouTubeAccountFailure failure) noexcept
{
	return failure == YouTubeAccountFailure::CredentialUnavailable ||
	       failure == YouTubeAccountFailure::ServiceUnavailable;
}

bool hasRequiredSelection(const YouTubeAccountDiscovery &discovery) noexcept
{
	return validateYouTubeAccountSelection(discovery) == YouTubeAccountSelectionValidationError::None;
}

} // namespace

static_assert(std::is_nothrow_move_assignable<YouTubeAccountSnapshot>::value);
static_assert(std::is_nothrow_move_assignable<YouTubeAccountDiscovery>::value);

YouTubeAccountSnapshot YouTubeAccountCoordinator::snapshot() const
{
	return snapshot_;
}

std::optional<YouTubeAccountLease> YouTubeAccountCoordinator::activeLease() const noexcept
{
	return snapshot_.lease;
}

YouTubeAccountTransition YouTubeAccountCoordinator::beginAuthorization()
{
	if (snapshot_.state == YouTubeAccountState::Closed || attemptActive()) {
		return result();
	}

	replacingCommittedConnection_ = (snapshot_.state == YouTubeAccountState::Connected ||
					 snapshot_.state == YouTubeAccountState::Configured) &&
					committedConnection_.has_value();
	replacedConnectionState_ = replacingCommittedConnection_ ? snapshot_.state : YouTubeAccountState::Disconnected;
	if (replacingCommittedConnection_ && isUnavailableFailure(snapshot_.failure)) {
		committedConnectionFailure_ = snapshot_.failure;
	} else {
		committedConnectionFailure_.reset();
	}
	if (!replacingCommittedConnection_) {
		clearAllConnections();
	}

	snapshot_.failure = YouTubeAccountFailure::None;
	snapshot_.lease = nextLease();
	snapshot_.state = YouTubeAccountState::Authorizing;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::authorizationCallbackAccepted(YouTubeAccountLease lease)
{
	if (!accepts(lease) || snapshot_.state != YouTubeAccountState::Authorizing) {
		return result();
	}
	snapshot_.state = YouTubeAccountState::ExchangingCode;
	snapshot_.failure = YouTubeAccountFailure::None;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::tokenExchangeSucceeded(YouTubeAccountLease lease)
{
	if (!accepts(lease) || snapshot_.state != YouTubeAccountState::ExchangingCode) {
		return result();
	}
	snapshot_.state = YouTubeAccountState::Discovering;
	snapshot_.failure = YouTubeAccountFailure::None;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::discoverySucceeded(YouTubeAccountLease lease,
								       const YouTubeAccountDiscovery &discovery)
{
	if (!accepts(lease) || snapshot_.state != YouTubeAccountState::Discovering) {
		return result();
	}
	if (!hasRequiredSelection(discovery)) {
		return finishAttempt(YouTubeAccountFailure::InvalidResponse);
	}

	// Build every allocation-bearing value before any external credential or
	// profile side effect. The provider can then finish with the noexcept commit
	// below after both stores succeed.
	CommittedConnection nextConnection{discovery, lease};
	YouTubeAccountSnapshot nextSnapshot = snapshot_;
	nextSnapshot.channelId = discovery.channelId;
	nextSnapshot.channelLabel = discovery.channelLabel;
	nextSnapshot.streamId = discovery.streamId;
	nextSnapshot.streamLabel = discovery.streamLabel;
	nextSnapshot.lease.reset();
	nextSnapshot.connectionLease = lease;
	nextSnapshot.state = YouTubeAccountState::Connected;
	nextSnapshot.failure = YouTubeAccountFailure::None;
	nextSnapshot.revision = nextNonZero(nextNonZero(snapshot_.revision));
	PendingConnectionCommit nextCommit{std::move(nextConnection), std::move(nextSnapshot)};

	snapshot_.state = YouTubeAccountState::PersistingCredential;
	snapshot_.failure = YouTubeAccountFailure::None;
	pendingCommit_ = std::move(nextCommit);
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::credentialStored(YouTubeAccountLease lease)
{
	if (!commitCredentialStored(lease)) {
		return result();
	}
	return {snapshot_, true};
}

bool YouTubeAccountCoordinator::commitCredentialStored(YouTubeAccountLease lease) noexcept
{
	static_assert(std::is_nothrow_move_constructible<CommittedConnection>::value);
	static_assert(std::is_nothrow_move_assignable<CommittedConnection>::value);
	static_assert(std::is_nothrow_move_assignable<YouTubeAccountSnapshot>::value);
	if (!accepts(lease) || snapshot_.state != YouTubeAccountState::PersistingCredential ||
	    !pendingCommit_.has_value()) {
		return false;
	}

	snapshot_ = std::move(pendingCommit_->snapshot);
	committedConnection_ = std::move(pendingCommit_->connection);
	pendingCommit_.reset();
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	committedConnectionFailure_.reset();
	return true;
}

YouTubeAccountTransition YouTubeAccountCoordinator::attemptFailed(YouTubeAccountLease lease,
								  YouTubeAccountFailure failure)
{
	if (!accepts(lease) || !attemptActive() || failure == YouTubeAccountFailure::None ||
	    failure == YouTubeAccountFailure::Cancelled) {
		return result();
	}
	return finishAttempt(failure);
}

YouTubeAccountTransition YouTubeAccountCoordinator::finishAttempt(YouTubeAccountFailure failure)
{
	if (replacingCommittedConnection_ && committedConnection_.has_value()) {
		snapshot_.lease.reset();
		pendingCommit_.reset();
		if (committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired) {
			clearAllConnections();
			snapshot_.state = YouTubeAccountState::NeedsReauthorization;
			snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
			replacingCommittedConnection_ = false;
			replacedConnectionState_ = YouTubeAccountState::Disconnected;
			return result(true);
		}
		restoreCommittedConnection();
		snapshot_.state = replacedConnectionState_;
		snapshot_.failure = committedConnectionFailure_.value_or(failure);
		replacingCommittedConnection_ = false;
		replacedConnectionState_ = YouTubeAccountState::Disconnected;
		committedConnectionFailure_.reset();
		return result(true);
	}

	snapshot_.lease.reset();
	pendingCommit_.reset();
	clearAllConnections();
	if (failure == YouTubeAccountFailure::ReauthorizationRequired) {
		snapshot_.state = YouTubeAccountState::NeedsReauthorization;
	} else if (isUnavailableFailure(failure)) {
		snapshot_.state = YouTubeAccountState::Unavailable;
	} else {
		snapshot_.state = YouTubeAccountState::Failed;
	}
	snapshot_.failure = failure;
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	committedConnectionFailure_.reset();
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::cancel(YouTubeAccountLease lease)
{
	if (!accepts(lease) || !attemptActive()) {
		return result();
	}

	snapshot_.lease.reset();
	pendingCommit_.reset();
	if (replacingCommittedConnection_ && committedConnection_.has_value()) {
		if (committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired) {
			clearAllConnections();
			snapshot_.state = YouTubeAccountState::NeedsReauthorization;
			snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
		} else {
			restoreCommittedConnection();
			snapshot_.state = replacedConnectionState_;
			snapshot_.failure = committedConnectionFailure_.value_or(YouTubeAccountFailure::Cancelled);
		}
	} else {
		clearAllConnections();
		snapshot_.state = YouTubeAccountState::Disconnected;
		snapshot_.failure = YouTubeAccountFailure::Cancelled;
	}
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	committedConnectionFailure_.reset();
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::credentialStateUncertain(YouTubeAccountLease lease)
{
	if (!accepts(lease) || !attemptActive()) {
		return result();
	}

	invalidateGeneration();
	snapshot_.lease.reset();
	clearAllConnections();
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	committedConnectionFailure_.reset();
	snapshot_.state = YouTubeAccountState::Unavailable;
	snapshot_.failure = YouTubeAccountFailure::CredentialUnavailable;
	return result(true);
}

YouTubeAccountTransition
YouTubeAccountCoordinator::restoreSavedConnection(const YouTubeAccountDiscovery &discovery,
						  YouTubeAccountSavedCredentialState credentialState)
{
	if (snapshot_.state == YouTubeAccountState::Closed) {
		return result();
	}

	// Construct every allocation-bearing value before replacing the current
	// profile context. The final assignments are moves, matching the interactive
	// connection commit's exception-safety boundary.
	const bool validSelection = hasRequiredSelection(discovery);
	const bool validCredentialState = credentialState == YouTubeAccountSavedCredentialState::Present ||
					  credentialState == YouTubeAccountSavedCredentialState::Missing ||
					  credentialState == YouTubeAccountSavedCredentialState::Unavailable;
	const std::uint64_t nextGeneration = nextNonZero(snapshot_.generation);
	const YouTubeAccountLease restoredLease{nextGeneration, 1};

	YouTubeAccountSnapshot nextSnapshot;
	nextSnapshot.generation = nextGeneration;
	nextSnapshot.revision = snapshot_.revision;
	std::optional<CommittedConnection> nextConnection;

	if (!validSelection || !validCredentialState) {
		nextSnapshot.state = YouTubeAccountState::Failed;
		nextSnapshot.failure = YouTubeAccountFailure::InvalidResponse;
	} else {
		nextSnapshot.channelId = discovery.channelId;
		nextSnapshot.channelLabel = discovery.channelLabel;
		nextSnapshot.streamId = discovery.streamId;
		nextSnapshot.streamLabel = discovery.streamLabel;
		switch (credentialState) {
		case YouTubeAccountSavedCredentialState::Present:
			nextConnection.emplace(CommittedConnection{discovery, restoredLease});
			nextSnapshot.connectionLease = restoredLease;
			nextSnapshot.state = YouTubeAccountState::Configured;
			nextSnapshot.failure = YouTubeAccountFailure::None;
			break;
		case YouTubeAccountSavedCredentialState::Missing:
			nextSnapshot.state = YouTubeAccountState::NeedsReauthorization;
			nextSnapshot.failure = YouTubeAccountFailure::ReauthorizationRequired;
			break;
		case YouTubeAccountSavedCredentialState::Unavailable:
			nextSnapshot.state = YouTubeAccountState::Unavailable;
			nextSnapshot.failure = YouTubeAccountFailure::CredentialUnavailable;
			break;
		}
	}

	snapshot_ = std::move(nextSnapshot);
	committedConnection_ = std::move(nextConnection);
	pendingCommit_.reset();
	committedConnectionFailure_.reset();
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	nextAttempt_ = credentialState == YouTubeAccountSavedCredentialState::Present && validSelection &&
				       validCredentialState
			       ? restoredLease.attempt
			       : 0;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::invalidateContext()
{
	if (snapshot_.state == YouTubeAccountState::Closed) {
		return result();
	}

	invalidateGeneration();
	snapshot_.lease.reset();
	clearAllConnections();
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	committedConnectionFailure_.reset();
	snapshot_.state = YouTubeAccountState::Disconnected;
	snapshot_.failure = YouTubeAccountFailure::None;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::markUnavailable(YouTubeAccountLease connectionLease,
								    YouTubeAccountFailure failure)
{
	if (!isUnavailableFailure(failure) || !committedConnection_.has_value() ||
	    committedConnection_->lease != connectionLease) {
		return result();
	}
	if (attemptActive()) {
		if (!replacingCommittedConnection_ ||
		    committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired ||
		    committedConnectionFailure_ == failure) {
			return result();
		}
		committedConnectionFailure_ = failure;
		snapshot_.failure = failure;
		return result(true);
	}
	if ((snapshot_.state != YouTubeAccountState::Connected && snapshot_.state != YouTubeAccountState::Configured) ||
	    !snapshot_.connectionLease.has_value() || *snapshot_.connectionLease != connectionLease) {
		return result();
	}
	if (snapshot_.failure == failure) {
		return result();
	}
	snapshot_.failure = failure;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::markNeedsReauthorization(YouTubeAccountLease connectionLease)
{
	if (!committedConnection_.has_value() || committedConnection_->lease != connectionLease) {
		return result();
	}
	if (attemptActive()) {
		if (!replacingCommittedConnection_ ||
		    committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired) {
			return result();
		}
		committedConnectionFailure_ = YouTubeAccountFailure::ReauthorizationRequired;
		snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
		return result(true);
	}
	if ((snapshot_.state != YouTubeAccountState::Connected && snapshot_.state != YouTubeAccountState::Configured) ||
	    !snapshot_.connectionLease.has_value() || *snapshot_.connectionLease != connectionLease) {
		return result();
	}
	clearAllConnections();
	snapshot_.state = YouTubeAccountState::NeedsReauthorization;
	snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::shutdown()
{
	if (snapshot_.state == YouTubeAccountState::Closed) {
		return result();
	}

	invalidateGeneration();
	snapshot_.lease.reset();
	clearAllConnections();
	replacingCommittedConnection_ = false;
	replacedConnectionState_ = YouTubeAccountState::Disconnected;
	committedConnectionFailure_.reset();
	snapshot_.state = YouTubeAccountState::Closed;
	snapshot_.failure = YouTubeAccountFailure::None;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::result(bool changed)
{
	if (changed) {
		snapshot_.revision = nextNonZero(snapshot_.revision);
	}
	return {snapshot_, changed};
}

bool YouTubeAccountCoordinator::accepts(YouTubeAccountLease lease) const noexcept
{
	return snapshot_.lease.has_value() && *snapshot_.lease == lease;
}

bool YouTubeAccountCoordinator::attemptActive() const noexcept
{
	return snapshot_.lease.has_value() && isAttemptState(snapshot_.state);
}

void YouTubeAccountCoordinator::clearVisibleConnection() noexcept
{
	snapshot_.connectionLease.reset();
	snapshot_.channelId.clear();
	snapshot_.channelLabel.clear();
	snapshot_.streamId.clear();
	snapshot_.streamLabel.clear();
}

void YouTubeAccountCoordinator::clearAllConnections() noexcept
{
	clearVisibleConnection();
	committedConnection_.reset();
	pendingCommit_.reset();
	committedConnectionFailure_.reset();
}

void YouTubeAccountCoordinator::restoreCommittedConnection()
{
	if (!committedConnection_.has_value()) {
		clearVisibleConnection();
		return;
	}

	YouTubeAccountSnapshot nextSnapshot = snapshot_;
	nextSnapshot.channelId = committedConnection_->discovery.channelId;
	nextSnapshot.channelLabel = committedConnection_->discovery.channelLabel;
	nextSnapshot.streamId = committedConnection_->discovery.streamId;
	nextSnapshot.streamLabel = committedConnection_->discovery.streamLabel;
	nextSnapshot.connectionLease = committedConnection_->lease;
	snapshot_ = std::move(nextSnapshot);
}

void YouTubeAccountCoordinator::invalidateGeneration() noexcept
{
	snapshot_.generation = nextNonZero(snapshot_.generation);
	nextAttempt_ = 0;
}

YouTubeAccountLease YouTubeAccountCoordinator::nextLease() noexcept
{
	nextAttempt_ = nextNonZero(nextAttempt_);
	return {snapshot_.generation, nextAttempt_};
}

} // namespace easy_multistream

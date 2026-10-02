// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-coordinator.hpp"

#include <limits>
#include <string_view>
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

bool isValidIdentifier(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kYouTubeAccountMaxIdentifierBytes) {
		return false;
	}
	for (const unsigned char byte : value) {
		if (byte < 0x21U || byte > 0x7EU) {
			return false;
		}
	}
	return true;
}

bool isValidLabel(std::string_view value) noexcept
{
	if (value.size() > kYouTubeAccountMaxLabelBytes) {
		return false;
	}

	std::size_t index = 0;
	while (index < value.size()) {
		const auto first = static_cast<unsigned char>(value[index++]);
		if (first <= 0x7FU) {
			if (first <= 0x1FU || first == 0x7FU) {
				return false;
			}
			continue;
		}

		std::size_t continuationCount = 0;
		std::uint32_t codePoint = 0;
		std::uint32_t minimum = 0;
		if (first >= 0xC2U && first <= 0xDFU) {
			continuationCount = 1;
			codePoint = first & 0x1FU;
			minimum = 0x80U;
		} else if (first >= 0xE0U && first <= 0xEFU) {
			continuationCount = 2;
			codePoint = first & 0x0FU;
			minimum = 0x800U;
		} else if (first >= 0xF0U && first <= 0xF4U) {
			continuationCount = 3;
			codePoint = first & 0x07U;
			minimum = 0x10000U;
		} else {
			return false;
		}

		if (value.size() - index < continuationCount) {
			return false;
		}
		for (std::size_t offset = 0; offset < continuationCount; ++offset) {
			const auto continuation = static_cast<unsigned char>(value[index++]);
			if ((continuation & 0xC0U) != 0x80U) {
				return false;
			}
			codePoint = (codePoint << 6U) | (continuation & 0x3FU);
		}

		if (codePoint < minimum || codePoint > 0x10FFFFU ||
		    (codePoint >= 0xD800U && codePoint <= 0xDFFFU) ||
		    (codePoint >= 0x80U && codePoint <= 0x9FU)) {
			return false;
		}
	}

	return true;
}

bool hasRequiredSelection(const YouTubeAccountDiscovery &discovery) noexcept
{
	return isValidIdentifier(discovery.channelId) && isValidLabel(discovery.channelLabel) &&
	       isValidIdentifier(discovery.streamId) && isValidLabel(discovery.streamLabel);
}

} // namespace

static_assert(std::is_nothrow_move_assignable<YouTubeAccountSnapshot>::value);
static_assert(std::is_nothrow_move_assignable<YouTubeAccountDiscovery>::value);

YouTubeAccountSnapshot YouTubeAccountCoordinator::snapshot() const
{
	return snapshot_;
}

YouTubeAccountTransition YouTubeAccountCoordinator::beginAuthorization()
{
	if (snapshot_.state == YouTubeAccountState::Closed || attemptActive()) {
		return result();
	}

	replacingConnected_ = snapshot_.state == YouTubeAccountState::Connected && committedConnection_.has_value();
	if (replacingConnected_ && isUnavailableFailure(snapshot_.failure)) {
		committedConnectionFailure_ = snapshot_.failure;
	} else {
		committedConnectionFailure_.reset();
	}
	if (!replacingConnected_) {
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

YouTubeAccountTransition YouTubeAccountCoordinator::discoverySucceeded(
	YouTubeAccountLease lease, const YouTubeAccountDiscovery &discovery)
{
	if (!accepts(lease) || snapshot_.state != YouTubeAccountState::Discovering) {
		return result();
	}
	if (!hasRequiredSelection(discovery)) {
		return finishAttempt(YouTubeAccountFailure::InvalidResponse);
	}

	// Keep the discovered selection private until the provider has durably
	// stored the refresh credential. This prevents a failed replacement from
	// overwriting a known-good connection or presenting an unusable new one.
	pendingDiscovery_ = discovery;
	snapshot_.state = YouTubeAccountState::PersistingCredential;
	snapshot_.failure = YouTubeAccountFailure::None;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::credentialStored(YouTubeAccountLease lease)
{
	if (!accepts(lease) || snapshot_.state != YouTubeAccountState::PersistingCredential ||
	    !pendingDiscovery_.has_value()) {
		return result();
	}

	CommittedConnection nextConnection{*pendingDiscovery_, lease};
	YouTubeAccountSnapshot nextSnapshot = snapshot_;
	nextSnapshot.channelId = pendingDiscovery_->channelId;
	nextSnapshot.channelLabel = pendingDiscovery_->channelLabel;
	nextSnapshot.streamId = pendingDiscovery_->streamId;
	nextSnapshot.streamLabel = pendingDiscovery_->streamLabel;
	nextSnapshot.lease.reset();
	nextSnapshot.connectionLease = lease;
	nextSnapshot.state = YouTubeAccountState::Connected;
	nextSnapshot.failure = YouTubeAccountFailure::None;

	snapshot_ = std::move(nextSnapshot);
	committedConnection_ = std::move(nextConnection);
	pendingDiscovery_.reset();
	replacingConnected_ = false;
	committedConnectionFailure_.reset();
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::attemptFailed(
	YouTubeAccountLease lease, YouTubeAccountFailure failure)
{
	if (!accepts(lease) || !attemptActive() || failure == YouTubeAccountFailure::None ||
	    failure == YouTubeAccountFailure::Cancelled) {
		return result();
	}
	return finishAttempt(failure);
}

YouTubeAccountTransition YouTubeAccountCoordinator::finishAttempt(YouTubeAccountFailure failure)
{
	if (replacingConnected_ && committedConnection_.has_value()) {
		snapshot_.lease.reset();
		pendingDiscovery_.reset();
		if (committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired) {
			clearAllConnections();
			snapshot_.state = YouTubeAccountState::NeedsReauthorization;
			snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
			replacingConnected_ = false;
			return result(true);
		}
		restoreCommittedConnection();
		snapshot_.state = YouTubeAccountState::Connected;
		snapshot_.failure = committedConnectionFailure_.value_or(failure);
		replacingConnected_ = false;
		committedConnectionFailure_.reset();
		return result(true);
	}

	snapshot_.lease.reset();
	pendingDiscovery_.reset();
	clearAllConnections();
	if (failure == YouTubeAccountFailure::ReauthorizationRequired) {
		snapshot_.state = YouTubeAccountState::NeedsReauthorization;
	} else if (isUnavailableFailure(failure)) {
		snapshot_.state = YouTubeAccountState::Unavailable;
	} else {
		snapshot_.state = YouTubeAccountState::Failed;
	}
	snapshot_.failure = failure;
	replacingConnected_ = false;
	committedConnectionFailure_.reset();
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::cancel(YouTubeAccountLease lease)
{
	if (!accepts(lease) || !attemptActive()) {
		return result();
	}

	snapshot_.lease.reset();
	pendingDiscovery_.reset();
	if (replacingConnected_ && committedConnection_.has_value()) {
		if (committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired) {
			clearAllConnections();
			snapshot_.state = YouTubeAccountState::NeedsReauthorization;
			snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
		} else {
			restoreCommittedConnection();
			snapshot_.state = YouTubeAccountState::Connected;
			snapshot_.failure = committedConnectionFailure_.value_or(YouTubeAccountFailure::Cancelled);
		}
	} else {
		clearAllConnections();
		snapshot_.state = YouTubeAccountState::Disconnected;
		snapshot_.failure = YouTubeAccountFailure::Cancelled;
	}
	replacingConnected_ = false;
	committedConnectionFailure_.reset();
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
	replacingConnected_ = false;
	committedConnectionFailure_.reset();
	snapshot_.state = YouTubeAccountState::Disconnected;
	snapshot_.failure = YouTubeAccountFailure::None;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::markUnavailable(
	YouTubeAccountLease connectionLease, YouTubeAccountFailure failure)
{
	if (!isUnavailableFailure(failure) || !committedConnection_.has_value() ||
	    committedConnection_->lease != connectionLease) {
		return result();
	}
	if (attemptActive()) {
		if (!replacingConnected_ || committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired ||
		    committedConnectionFailure_ == failure) {
			return result();
		}
		committedConnectionFailure_ = failure;
		snapshot_.failure = failure;
		return result(true);
	}
	if (snapshot_.state != YouTubeAccountState::Connected || !snapshot_.connectionLease.has_value() ||
	    *snapshot_.connectionLease != connectionLease) {
		return result();
	}
	if (snapshot_.failure == failure) {
		return result();
	}
	snapshot_.failure = failure;
	return result(true);
}

YouTubeAccountTransition YouTubeAccountCoordinator::markNeedsReauthorization(
	YouTubeAccountLease connectionLease)
{
	if (!committedConnection_.has_value() || committedConnection_->lease != connectionLease) {
		return result();
	}
	if (attemptActive()) {
		if (!replacingConnected_ ||
		    committedConnectionFailure_ == YouTubeAccountFailure::ReauthorizationRequired) {
			return result();
		}
		committedConnectionFailure_ = YouTubeAccountFailure::ReauthorizationRequired;
		snapshot_.failure = YouTubeAccountFailure::ReauthorizationRequired;
		return result(true);
	}
	if (snapshot_.state != YouTubeAccountState::Connected || !snapshot_.connectionLease.has_value() ||
	    *snapshot_.connectionLease != connectionLease) {
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
	replacingConnected_ = false;
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
	pendingDiscovery_.reset();
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

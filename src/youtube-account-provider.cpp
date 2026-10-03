// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-provider.hpp"

#include <QPointer>
#include <QThread>

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

namespace easy_multistream {
namespace {

std::uint64_t nextNonZero(std::uint64_t value) noexcept
{
	return value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

GoogleOAuthLoopbackAttempt authorizationAttempt(YouTubeAccountLease lease) noexcept
{
	return {lease.generation, lease.attempt};
}

GoogleOAuthTokenAttempt tokenAttempt(YouTubeAccountLease lease) noexcept
{
	return {lease.generation, lease.attempt};
}

YouTubeApiAttempt apiAttempt(YouTubeAccountLease lease) noexcept
{
	return {lease.generation, lease.attempt};
}

bool matches(GoogleOAuthLoopbackAttempt attempt, YouTubeAccountLease lease) noexcept
{
	return attempt.generation == lease.generation && attempt.attempt == lease.attempt;
}

bool matches(GoogleOAuthTokenAttempt attempt, YouTubeAccountLease lease) noexcept
{
	return attempt.generation == lease.generation && attempt.attempt == lease.attempt;
}

bool matches(YouTubeApiAttempt attempt, YouTubeAccountLease lease) noexcept
{
	return attempt.generation == lease.generation && attempt.attempt == lease.attempt;
}

class AuthorizationAdapter final : public YouTubeAccountAuthorizationPort {
public:
	explicit AuthorizationAdapter(GoogleOAuthAuthorizationSession::BrowserOpener browserOpener)
		: session_(std::move(browserOpener))
	{
	}

	GoogleOAuthAuthorizationStartStatus start(GoogleOAuthLoopbackAttempt attempt, const QString &clientId,
						  GoogleOAuthConsentMode consentMode,
						  CompletionHandler completionHandler) noexcept override
	{
		return session_.start(attempt, clientId, consentMode, std::move(completionHandler));
	}

	bool cancel(GoogleOAuthLoopbackAttempt attempt) noexcept override { return session_.cancel(attempt); }
	bool shutdown() noexcept override { return session_.shutdown(); }

private:
	GoogleOAuthAuthorizationSession session_;
};

class TokenAdapter final : public YouTubeAccountTokenPort {
public:
	GoogleOAuthTokenStartStatus startExchange(GoogleOAuthTokenExchangeRequest request,
						  CompletionHandler completionHandler) noexcept override
	{
		return transport_.startExchange(std::move(request), std::move(completionHandler));
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override { return transport_.cancel(attempt); }
	bool shutdown() noexcept override { return transport_.shutdown(); }

private:
	GoogleOAuthTokenTransport transport_;
};

class DiscoveryAdapter final : public YouTubeAccountDiscoveryPort {
public:
	YouTubeApiPagerStartStatus startListAllChannels(YouTubeListAllChannelsRequest request,
							CompletionHandler completionHandler) noexcept override
	{
		return pager_.startListAllChannels(std::move(request), std::move(completionHandler));
	}

	YouTubeApiPagerStartStatus startListAllStreams(YouTubeListAllStreamsRequest request,
						       CompletionHandler completionHandler) noexcept override
	{
		return pager_.startListAllStreams(std::move(request), std::move(completionHandler));
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept override { return pager_.cancel(attempt); }
	bool shutdown() noexcept override { return pager_.shutdown(); }

private:
	YouTubeApiDiscoveryPager pager_;
};

YouTubeAccountFailure authorizationFailure(const GoogleOAuthAuthorizationCompletion &completion) noexcept
{
	switch (completion.status) {
	case GoogleOAuthAuthorizationCompletionStatus::ProviderRejected:
		return YouTubeAccountFailure::AuthorizationRejected;
	case GoogleOAuthAuthorizationCompletionStatus::CallbackRejected:
		return YouTubeAccountFailure::CallbackRejected;
	case GoogleOAuthAuthorizationCompletionStatus::AuthorizationTimedOut:
	case GoogleOAuthAuthorizationCompletionStatus::ListenerFailure:
		return YouTubeAccountFailure::NetworkFailure;
	case GoogleOAuthAuthorizationCompletionStatus::RequestLimitReached:
		return YouTubeAccountFailure::ServiceUnavailable;
	case GoogleOAuthAuthorizationCompletionStatus::Success:
		return YouTubeAccountFailure::InvalidResponse;
	}
	return YouTubeAccountFailure::InvalidResponse;
}

YouTubeAccountFailure tokenFailure(const GoogleOAuthTokenCompletion &completion) noexcept
{
	if (completion.providerError == GoogleOAuthTokenProviderError::InvalidGrant ||
	    completion.providerError == GoogleOAuthTokenProviderError::InvalidClient ||
	    completion.providerError == GoogleOAuthTokenProviderError::UnauthorizedClient ||
	    completion.providerError == GoogleOAuthTokenProviderError::AccessDenied) {
		return YouTubeAccountFailure::TokenExchangeFailed;
	}
	if (completion.providerError == GoogleOAuthTokenProviderError::TemporarilyUnavailable ||
	    completion.providerError == GoogleOAuthTokenProviderError::RateLimited) {
		return YouTubeAccountFailure::ServiceUnavailable;
	}
	switch (completion.status) {
	case GoogleOAuthTokenCompletionStatus::NetworkFailure:
	case GoogleOAuthTokenCompletionStatus::TlsFailure:
	case GoogleOAuthTokenCompletionStatus::TimedOut:
		return YouTubeAccountFailure::NetworkFailure;
	case GoogleOAuthTokenCompletionStatus::InvalidResponse:
	case GoogleOAuthTokenCompletionStatus::ResponseTooLarge:
	case GoogleOAuthTokenCompletionStatus::RedirectRejected:
		return YouTubeAccountFailure::InvalidResponse;
	case GoogleOAuthTokenCompletionStatus::Success:
		return YouTubeAccountFailure::InvalidResponse;
	case GoogleOAuthTokenCompletionStatus::ProviderRejected:
	case GoogleOAuthTokenCompletionStatus::HttpFailure:
		return YouTubeAccountFailure::TokenExchangeFailed;
	}
	return YouTubeAccountFailure::TokenExchangeFailed;
}

YouTubeAccountFailure discoveryFailure(const YouTubeApiPagedCompletion &completion) noexcept
{
	if (completion.pagingFailure != YouTubeApiPagingFailure::None) {
		return YouTubeAccountFailure::InvalidResponse;
	}
	if (completion.providerError == YouTubeApiProviderError::InvalidToken ||
	    completion.providerError == YouTubeApiProviderError::InsufficientPermissions) {
		return YouTubeAccountFailure::ReauthorizationRequired;
	}
	if (completion.providerError == YouTubeApiProviderError::QuotaExceeded ||
	    completion.providerError == YouTubeApiProviderError::RateLimited ||
	    completion.providerError == YouTubeApiProviderError::TemporarilyUnavailable) {
		return YouTubeAccountFailure::ServiceUnavailable;
	}
	switch (completion.status) {
	case YouTubeApiCompletionStatus::NetworkFailure:
	case YouTubeApiCompletionStatus::TlsFailure:
	case YouTubeApiCompletionStatus::TimedOut:
		return YouTubeAccountFailure::NetworkFailure;
	case YouTubeApiCompletionStatus::InvalidResponse:
	case YouTubeApiCompletionStatus::ResponseTooLarge:
	case YouTubeApiCompletionStatus::RedirectRejected:
		return YouTubeAccountFailure::InvalidResponse;
	case YouTubeApiCompletionStatus::Success:
		return YouTubeAccountFailure::InvalidResponse;
	case YouTubeApiCompletionStatus::ProviderRejected:
	case YouTubeApiCompletionStatus::HttpFailure:
		return YouTubeAccountFailure::DiscoveryFailed;
	}
	return YouTubeAccountFailure::DiscoveryFailed;
}

bool validChannel(const YouTubeOwnedChannel &channel) noexcept
{
	return isValidYouTubeAccountIdentifier(channel.id) && isValidYouTubeAccountLabel(channel.label);
}

bool validStream(const YouTubeReusableStream &stream, std::string_view channelId) noexcept
{
	return stream.channelId == channelId && isValidYouTubeAccountIdentifier(stream.id) &&
	       isValidYouTubeAccountIdentifier(stream.channelId) && isValidYouTubeAccountLabel(stream.label);
}

} // namespace

class YouTubeAccountProvider::Impl final {
public:
	Impl(YouTubeAccountProvider *owner, QString clientId,
	     std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort,
	     std::unique_ptr<YouTubeAccountTokenPort> tokenPort,
	     std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort, CredentialVault &refreshTokenVault,
	     YouTubeAccountSelectionCommitter selectionCommitter)
		: owner_(owner),
		  clientId_(std::move(clientId)),
		  authorizationPort_(std::move(authorizationPort)),
		  tokenPort_(std::move(tokenPort)),
		  discoveryPort_(std::move(discoveryPort)),
		  refreshTokenVault_(refreshTokenVault),
		  selectionCommitter_(std::move(selectionCommitter))
	{
	}

	YouTubeAccountProviderStartStatus startConnection(GoogleOAuthConsentMode consentMode) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeAccountProviderStartStatus::WrongThread;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderStartStatus::Closed;
		}
		if (commitInProgress_ || activeLease().has_value()) {
			return YouTubeAccountProviderStartStatus::Busy;
		}
		if (clientId_.isEmpty()) {
			return YouTubeAccountProviderStartStatus::InvalidClientId;
		}
		if (!selectionCommitter_) {
			return YouTubeAccountProviderStartStatus::InvalidSelectionCommitter;
		}

		try {
			clearEphemeral();
			const auto transition = coordinator_.beginAuthorization();
			if (!transition.changed || !transition.snapshot.lease.has_value()) {
				return transition.snapshot.state == YouTubeAccountState::Closed
					       ? YouTubeAccountProviderStartStatus::Closed
					       : YouTubeAccountProviderStartStatus::Busy;
			}

			const auto lease = *transition.snapshot.lease;
			operationEpoch_ = nextNonZero(operationEpoch_);
			const auto epoch = operationEpoch_;
			setStage(YouTubeAccountProviderStage::Authorizing);

			QPointer<YouTubeAccountProvider> guard(owner_);
			const auto status = authorizationPort_->start(
				authorizationAttempt(lease), clientId_, consentMode,
				[guard, epoch, lease](GoogleOAuthAuthorizationCompletion completion) mutable {
					if (guard) {
						guard->impl_->handleAuthorization(epoch, lease, std::move(completion));
					}
				});
			if (status != GoogleOAuthAuthorizationStartStatus::Started &&
			    isCurrent(epoch, lease, YouTubeAccountProviderStage::Authorizing)) {
				const auto failure = status == GoogleOAuthAuthorizationStartStatus::InvalidAttempt ||
								     status == GoogleOAuthAuthorizationStartStatus::
										       AuthorizationRequestFailed
							     ? YouTubeAccountFailure::InvalidResponse
							     : YouTubeAccountFailure::NetworkFailure;
				failAttempt(lease, failure);
				return status == GoogleOAuthAuthorizationStartStatus::AuthorizationRequestFailed
					       ? YouTubeAccountProviderStartStatus::InvalidClientId
					       : YouTubeAccountProviderStartStatus::OperationFailed;
			}
			return YouTubeAccountProviderStartStatus::Started;
		} catch (...) {
			failActiveNoThrow(YouTubeAccountFailure::ServiceUnavailable);
			return YouTubeAccountProviderStartStatus::OperationFailed;
		}
	}

	YouTubeAccountProviderSelectionStatus selectChannel(YouTubeAccountLease lease,
							    std::string_view channelId) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeAccountProviderSelectionStatus::WrongThread;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderSelectionStatus::Closed;
		}
		if (!leaseIsActive(lease)) {
			return YouTubeAccountProviderSelectionStatus::StaleLease;
		}
		if (stage_ != YouTubeAccountProviderStage::AwaitingChannelSelection) {
			return YouTubeAccountProviderSelectionStatus::WrongStage;
		}
		return selectChannelCandidate(lease, channelId);
	}

	YouTubeAccountProviderSelectionStatus selectStream(YouTubeAccountLease lease,
							   std::string_view streamId) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeAccountProviderSelectionStatus::WrongThread;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderSelectionStatus::Closed;
		}
		if (!leaseIsActive(lease)) {
			return YouTubeAccountProviderSelectionStatus::StaleLease;
		}
		if (stage_ != YouTubeAccountProviderStage::AwaitingStreamSelection) {
			return YouTubeAccountProviderSelectionStatus::WrongStage;
		}
		return selectStreamCandidate(lease, streamId);
	}

	bool cancel(YouTubeAccountLease lease) noexcept
	{
		if (!onOwnerThread() || stage_ == YouTubeAccountProviderStage::Closed || commitInProgress_ ||
		    !leaseIsActive(lease)) {
			return false;
		}
		operationEpoch_ = nextNonZero(operationEpoch_);
		cancelPorts(lease);
		clearEphemeral();
		try {
			const auto transition = coordinator_.cancel(lease);
			if (!transition.changed) {
				return false;
			}
			setStage(transition.snapshot.state == YouTubeAccountState::Connected
					 ? YouTubeAccountProviderStage::Connected
					 : YouTubeAccountProviderStage::Idle);
			return true;
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Failed);
			return false;
		}
	}

	bool invalidateContext() noexcept
	{
		if (!onOwnerThread() || stage_ == YouTubeAccountProviderStage::Closed || commitInProgress_) {
			return false;
		}
		operationEpoch_ = nextNonZero(operationEpoch_);
		if (const auto lease = activeLease(); lease.has_value()) {
			cancelPorts(*lease);
		}
		clearEphemeral();
		try {
			coordinator_.invalidateContext();
			setStage(YouTubeAccountProviderStage::Idle);
			return true;
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Failed);
			return false;
		}
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread() || commitInProgress_) {
			return false;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return true;
		}
		operationEpoch_ = nextNonZero(operationEpoch_);
		if (const auto lease = activeLease(); lease.has_value()) {
			cancelPorts(*lease);
		}
		clearEphemeral();
		const bool authorizationClosed = authorizationPort_->shutdown();
		const bool tokenClosed = tokenPort_->shutdown();
		const bool discoveryClosed = discoveryPort_->shutdown();
		const bool portsClosed = authorizationClosed && tokenClosed && discoveryClosed;
		try {
			coordinator_.shutdown();
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Closed);
			return false;
		}
		setStage(YouTubeAccountProviderStage::Closed);
		return portsClosed;
	}

	YouTubeAccountProviderSnapshot snapshot() const
	{
		YouTubeAccountProviderSnapshot value;
		value.revision = revision_;
		value.stage = stage_;
		value.account = coordinator_.snapshot();
		value.channels = channels_;
		value.streams = streams_;
		if (selectedChannel_.has_value()) {
			value.selectedChannelId = selectedChannel_->id;
		}
		if (selectedStream_.has_value()) {
			value.selectedStreamId = selectedStream_->id;
		}
		return value;
	}

private:
	bool onOwnerThread() const noexcept { return QThread::currentThread() == owner_->thread(); }

	std::optional<YouTubeAccountLease> activeLease() const noexcept { return coordinator_.activeLease(); }

	bool leaseIsActive(YouTubeAccountLease lease) const noexcept
	{
		const auto active = activeLease();
		return active.has_value() && *active == lease;
	}

	bool isCurrent(std::uint64_t epoch, YouTubeAccountLease lease, YouTubeAccountProviderStage stage) const noexcept
	{
		return operationEpoch_ == epoch && stage_ == stage && leaseIsActive(lease);
	}

	void setStage(YouTubeAccountProviderStage stage) noexcept
	{
		if (stage_ != stage) {
			stage_ = stage;
			revision_ = nextNonZero(revision_);
		}
	}

	void touch() noexcept { revision_ = nextNonZero(revision_); }

	void clearEphemeral() noexcept
	{
		accessToken_.clear();
		refreshToken_.clear();
		channels_.clear();
		streams_.clear();
		selectedChannel_.reset();
		selectedStream_.reset();
	}

	void cancelPorts(YouTubeAccountLease lease) noexcept
	{
		authorizationPort_->cancel(authorizationAttempt(lease));
		tokenPort_->cancel(tokenAttempt(lease));
		discoveryPort_->cancel(apiAttempt(lease));
	}

	void failActiveNoThrow(YouTubeAccountFailure failure) noexcept
	{
		if (const auto lease = activeLease(); lease.has_value()) {
			failAttempt(*lease, failure);
		} else if (stage_ != YouTubeAccountProviderStage::Closed) {
			setStage(YouTubeAccountProviderStage::Failed);
		}
	}

	void failAttempt(YouTubeAccountLease lease, YouTubeAccountFailure failure) noexcept
	{
		operationEpoch_ = nextNonZero(operationEpoch_);
		cancelPorts(lease);
		clearEphemeral();
		try {
			const auto transition = coordinator_.attemptFailed(lease, failure);
			setStage(transition.snapshot.state == YouTubeAccountState::Connected
					 ? YouTubeAccountProviderStage::Connected
					 : YouTubeAccountProviderStage::Failed);
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Failed);
		}
	}

	void handleAuthorization(std::uint64_t epoch, YouTubeAccountLease lease,
				 GoogleOAuthAuthorizationCompletion completion) noexcept
	{
		if (!isCurrent(epoch, lease, YouTubeAccountProviderStage::Authorizing) ||
		    !matches(completion.attempt, lease)) {
			return;
		}
		if (!completion.succeeded()) {
			failAttempt(lease, authorizationFailure(completion));
			return;
		}

		try {
			const auto transition = coordinator_.authorizationCallbackAccepted(lease);
			if (!transition.changed) {
				return;
			}
			setStage(YouTubeAccountProviderStage::ExchangingCode);

			GoogleOAuthTokenExchangeRequest request;
			request.attempt = tokenAttempt(lease);
			request.clientId = clientId_;
			request.redirectUri = std::move(completion.redirectUri);
			request.authorizationCode = std::move(completion.authorizationCode);
			request.codeVerifier = std::move(completion.codeVerifier);

			QPointer<YouTubeAccountProvider> guard(owner_);
			const auto status = tokenPort_->startExchange(
				std::move(request),
				[guard, epoch, lease](GoogleOAuthTokenCompletion tokenCompletion) mutable {
					if (guard) {
						guard->impl_->handleToken(epoch, lease, std::move(tokenCompletion));
					}
				});
			if (status != GoogleOAuthTokenStartStatus::Started &&
			    isCurrent(epoch, lease, YouTubeAccountProviderStage::ExchangingCode)) {
				failAttempt(lease, YouTubeAccountFailure::TokenExchangeFailed);
			}
		} catch (...) {
			failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
		}
	}

	void handleToken(std::uint64_t epoch, YouTubeAccountLease lease, GoogleOAuthTokenCompletion completion) noexcept
	{
		if (!isCurrent(epoch, lease, YouTubeAccountProviderStage::ExchangingCode) ||
		    !matches(completion.attempt, lease) ||
		    completion.operation != GoogleOAuthTokenOperation::ExchangeAuthorizationCode) {
			return;
		}
		if (!completion.succeeded() || !completion.tokens.hasRefreshToken()) {
			failAttempt(lease, completion.succeeded() ? YouTubeAccountFailure::InvalidResponse
								  : tokenFailure(completion));
			return;
		}

		try {
			accessToken_ = std::move(completion.tokens.accessToken);
			refreshToken_ = std::move(completion.tokens.refreshToken);
			const auto transition = coordinator_.tokenExchangeSucceeded(lease);
			if (!transition.changed) {
				clearEphemeral();
				return;
			}
			startChannelListing(epoch, lease);
		} catch (...) {
			failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
		}
	}

	void startChannelListing(std::uint64_t epoch, YouTubeAccountLease lease)
	{
		setStage(YouTubeAccountProviderStage::ListingChannels);
		YouTubeListAllChannelsRequest request;
		request.attempt = apiAttempt(lease);
		request.accessToken = SecureBuffer::copyOf(accessToken_.view());

		QPointer<YouTubeAccountProvider> guard(owner_);
		const auto status = discoveryPort_->startListAllChannels(
			std::move(request), [guard, epoch, lease](YouTubeApiPagedCompletion completion) mutable {
				if (guard) {
					guard->impl_->handleChannels(epoch, lease, std::move(completion));
				}
			});
		if (status != YouTubeApiPagerStartStatus::Started &&
		    isCurrent(epoch, lease, YouTubeAccountProviderStage::ListingChannels)) {
			failAttempt(lease, YouTubeAccountFailure::DiscoveryFailed);
		}
	}

	void handleChannels(std::uint64_t epoch, YouTubeAccountLease lease,
			    YouTubeApiPagedCompletion completion) noexcept
	{
		if (!isCurrent(epoch, lease, YouTubeAccountProviderStage::ListingChannels) ||
		    !matches(completion.attempt, lease) ||
		    completion.operation != YouTubeApiOperation::ListOwnedChannels) {
			return;
		}
		if (!completion.succeeded()) {
			failAttempt(lease, discoveryFailure(completion));
			return;
		}
		if (completion.channels.empty()) {
			failAttempt(lease, YouTubeAccountFailure::DiscoveryFailed);
			return;
		}
		for (std::size_t index = 0; index < completion.channels.size(); ++index) {
			if (!validChannel(completion.channels[index])) {
				failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
				return;
			}
			for (std::size_t prior = 0; prior < index; ++prior) {
				if (completion.channels[prior].id == completion.channels[index].id) {
					failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
					return;
				}
			}
		}

		try {
			channels_ = std::move(completion.channels);
			touch();
			if (channels_.size() == 1U) {
				const std::string id = channels_.front().id;
				selectChannelCandidate(lease, id);
			} else {
				setStage(YouTubeAccountProviderStage::AwaitingChannelSelection);
			}
		} catch (...) {
			failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
		}
	}

	YouTubeAccountProviderSelectionStatus selectChannelCandidate(YouTubeAccountLease lease,
								     std::string_view channelId) noexcept
	{
		try {
			const auto found = std::find_if(channels_.begin(), channels_.end(),
							[&](const auto &channel) { return channel.id == channelId; });
			if (found == channels_.end()) {
				return YouTubeAccountProviderSelectionStatus::UnknownCandidate;
			}
			selectedChannel_ = *found;
			streams_.clear();
			selectedStream_.reset();
			touch();
			setStage(YouTubeAccountProviderStage::ListingStreams);

			YouTubeListAllStreamsRequest request;
			request.attempt = apiAttempt(lease);
			request.accessToken = SecureBuffer::copyOf(accessToken_.view());
			request.channelId = selectedChannel_->id;
			const auto epoch = operationEpoch_;
			QPointer<YouTubeAccountProvider> guard(owner_);
			const auto status = discoveryPort_->startListAllStreams(
				std::move(request),
				[guard, epoch, lease](YouTubeApiPagedCompletion completion) mutable {
					if (guard) {
						guard->impl_->handleStreams(epoch, lease, std::move(completion));
					}
				});
			if (status != YouTubeApiPagerStartStatus::Started &&
			    isCurrent(epoch, lease, YouTubeAccountProviderStage::ListingStreams)) {
				failAttempt(lease, YouTubeAccountFailure::DiscoveryFailed);
				return YouTubeAccountProviderSelectionStatus::OperationFailed;
			}
			return YouTubeAccountProviderSelectionStatus::Accepted;
		} catch (...) {
			failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
			return YouTubeAccountProviderSelectionStatus::OperationFailed;
		}
	}

	void handleStreams(std::uint64_t epoch, YouTubeAccountLease lease,
			   YouTubeApiPagedCompletion completion) noexcept
	{
		if (!isCurrent(epoch, lease, YouTubeAccountProviderStage::ListingStreams) ||
		    !matches(completion.attempt, lease) ||
		    completion.operation != YouTubeApiOperation::ListReusableStreams || !selectedChannel_.has_value()) {
			return;
		}
		if (!completion.succeeded()) {
			failAttempt(lease, discoveryFailure(completion));
			return;
		}
		if (completion.streams.empty()) {
			failAttempt(lease, YouTubeAccountFailure::NoCompatibleStream);
			return;
		}
		for (std::size_t index = 0; index < completion.streams.size(); ++index) {
			if (!validStream(completion.streams[index], selectedChannel_->id)) {
				failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
				return;
			}
			for (std::size_t prior = 0; prior < index; ++prior) {
				if (completion.streams[prior].id == completion.streams[index].id) {
					failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
					return;
				}
			}
		}

		try {
			streams_ = std::move(completion.streams);
			accessToken_.clear();
			touch();
			if (streams_.size() == 1U) {
				const std::string id = streams_.front().id;
				selectStreamCandidate(lease, id);
			} else {
				setStage(YouTubeAccountProviderStage::AwaitingStreamSelection);
			}
		} catch (...) {
			failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
		}
	}

	YouTubeAccountProviderSelectionStatus selectStreamCandidate(YouTubeAccountLease lease,
								    std::string_view streamId) noexcept
	{
		try {
			const auto found = std::find_if(streams_.begin(), streams_.end(),
							[&](const auto &stream) { return stream.id == streamId; });
			if (found == streams_.end() || !selectedChannel_.has_value()) {
				return YouTubeAccountProviderSelectionStatus::UnknownCandidate;
			}
			selectedStream_ = *found;
			touch();
			YouTubeAccountSelection selection{selectedChannel_->id, selectedChannel_->label,
							  selectedStream_->id, selectedStream_->label};
			if (validateYouTubeAccountSelection(selection) !=
			    YouTubeAccountSelectionValidationError::None) {
				failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
				return YouTubeAccountProviderSelectionStatus::OperationFailed;
			}
			const auto transition = coordinator_.discoverySucceeded(lease, selection);
			if (!transition.changed) {
				return YouTubeAccountProviderSelectionStatus::StaleLease;
			}
			setStage(YouTubeAccountProviderStage::PersistingCredential);
			return persistConnection(lease, selection)
				       ? YouTubeAccountProviderSelectionStatus::Accepted
				       : YouTubeAccountProviderSelectionStatus::OperationFailed;
		} catch (...) {
			failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
			return YouTubeAccountProviderSelectionStatus::OperationFailed;
		}
	}

	bool persistConnection(YouTubeAccountLease lease, const YouTubeAccountSelection &selection) noexcept
	{
		if (commitInProgress_ || refreshToken_.empty() || !leaseIsActive(lease)) {
			failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
			return false;
		}

		commitInProgress_ = true;
		CredentialReadResult previous;
		try {
			previous = refreshTokenVault_.read();
		} catch (...) {
			commitInProgress_ = false;
			failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			return false;
		}
		const bool credentialWasMissing = previous.result.error == CredentialError::NotFound;
		if (!previous.result.succeeded() && !credentialWasMissing) {
			commitInProgress_ = false;
			failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			return false;
		}

		CredentialResult writeResult;
		try {
			writeResult = refreshTokenVault_.write(refreshToken_.view());
		} catch (...) {
			writeResult.error = CredentialError::OperatingSystemError;
		}
		if (!writeResult.succeeded()) {
			commitInProgress_ = false;
			failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			return false;
		}

		bool selectionSaved = false;
		try {
			selectionSaved = selectionCommitter_(selection);
		} catch (...) {
			selectionSaved = false;
		}
		if (!selectionSaved) {
			const bool rolledBack = rollbackCredential(previous, credentialWasMissing);
			commitInProgress_ = false;
			refreshToken_.clear();
			if (!rolledBack) {
				failCredentialStateUncertain(lease);
			} else {
				failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			}
			return false;
		}

		// Public lifecycle methods reject re-entry while commitInProgress_ is
		// true. The committer contract also forbids yielding, so the lease cannot
		// become stale between the durable writes and this noexcept move commit.
		const bool committed = coordinator_.commitCredentialStored(lease);
		commitInProgress_ = false;
		refreshToken_.clear();
		if (!committed) {
			// The non-secret profile is already saved. Do not claim either the old
			// or new account is usable if the invariant was violated.
			rollbackCredential(previous, credentialWasMissing);
			failCredentialStateUncertain(lease);
			return false;
		}
		previous.secret.clear();

		channels_.clear();
		streams_.clear();
		selectedChannel_.reset();
		selectedStream_.reset();
		setStage(YouTubeAccountProviderStage::Connected);
		return true;
	}

	bool rollbackCredential(CredentialReadResult &previous, bool credentialWasMissing) noexcept
	{
		CredentialResult result;
		try {
			result = credentialWasMissing ? refreshTokenVault_.erase()
						      : refreshTokenVault_.write(previous.secret.view());
		} catch (...) {
			result.error = CredentialError::OperatingSystemError;
		}
		previous.secret.clear();
		return result.succeeded() || (credentialWasMissing && result.error == CredentialError::NotFound);
	}

	void failCredentialStateUncertain(YouTubeAccountLease lease) noexcept
	{
		operationEpoch_ = nextNonZero(operationEpoch_);
		cancelPorts(lease);
		clearEphemeral();
		try {
			coordinator_.credentialStateUncertain(lease);
		} catch (...) {
		}
		setStage(YouTubeAccountProviderStage::Failed);
	}

	YouTubeAccountProvider *owner_ = nullptr;
	QString clientId_;
	std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort_;
	std::unique_ptr<YouTubeAccountTokenPort> tokenPort_;
	std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort_;
	CredentialVault &refreshTokenVault_;
	YouTubeAccountSelectionCommitter selectionCommitter_;
	YouTubeAccountCoordinator coordinator_;
	YouTubeAccountProviderStage stage_ = YouTubeAccountProviderStage::Idle;
	std::uint64_t revision_ = 0;
	std::uint64_t operationEpoch_ = 0;
	SecureBuffer accessToken_;
	SecureBuffer refreshToken_;
	std::vector<YouTubeOwnedChannel> channels_;
	std::vector<YouTubeReusableStream> streams_;
	std::optional<YouTubeOwnedChannel> selectedChannel_;
	std::optional<YouTubeReusableStream> selectedStream_;
	bool commitInProgress_ = false;
};

YouTubeAccountProvider::YouTubeAccountProvider(QString clientId,
					       GoogleOAuthAuthorizationSession::BrowserOpener browserOpener,
					       CredentialVault &refreshTokenVault,
					       YouTubeAccountSelectionCommitter selectionCommitter, QObject *parent)
	: YouTubeAccountProvider(std::move(clientId), std::make_unique<AuthorizationAdapter>(std::move(browserOpener)),
				 std::make_unique<TokenAdapter>(), std::make_unique<DiscoveryAdapter>(),
				 refreshTokenVault, std::move(selectionCommitter), parent)
{
}

YouTubeAccountProvider::YouTubeAccountProvider(QString clientId,
					       std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort,
					       std::unique_ptr<YouTubeAccountTokenPort> tokenPort,
					       std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort,
					       CredentialVault &refreshTokenVault,
					       YouTubeAccountSelectionCommitter selectionCommitter, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(clientId), std::move(authorizationPort), std::move(tokenPort),
				       std::move(discoveryPort), refreshTokenVault, std::move(selectionCommitter)))
{
}

YouTubeAccountProvider::~YouTubeAccountProvider()
{
	assert(QThread::currentThread() == thread());
	impl_->shutdown();
}

YouTubeAccountProviderStartStatus YouTubeAccountProvider::startConnection(GoogleOAuthConsentMode consentMode) noexcept
{
	return impl_->startConnection(consentMode);
}

YouTubeAccountProviderSelectionStatus YouTubeAccountProvider::selectChannel(YouTubeAccountLease lease,
									    std::string_view channelId) noexcept
{
	return impl_->selectChannel(lease, channelId);
}

YouTubeAccountProviderSelectionStatus YouTubeAccountProvider::selectStream(YouTubeAccountLease lease,
									   std::string_view streamId) noexcept
{
	return impl_->selectStream(lease, streamId);
}

bool YouTubeAccountProvider::cancel(YouTubeAccountLease lease) noexcept
{
	return impl_->cancel(lease);
}

bool YouTubeAccountProvider::invalidateContext() noexcept
{
	return impl_->invalidateContext();
}

bool YouTubeAccountProvider::shutdown() noexcept
{
	return impl_->shutdown();
}

YouTubeAccountProviderSnapshot YouTubeAccountProvider::snapshot() const
{
	return impl_->snapshot();
}

} // namespace easy_multistream

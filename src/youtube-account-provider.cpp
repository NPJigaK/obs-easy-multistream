// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-provider.hpp"

#include "google-oauth-protocol.hpp"

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

bool sameSelection(const YouTubeAccountSelection &left, const YouTubeAccountSelection &right) noexcept
{
	return left.channelId == right.channelId && left.channelLabel == right.channelLabel &&
	       left.streamId == right.streamId && left.streamLabel == right.streamLabel;
}

YouTubeAccountProviderStage providerStageForRetainedConnection(YouTubeAccountState state,
							       YouTubeAccountProviderStage fallback) noexcept
{
	if (state == YouTubeAccountState::Connected) {
		return YouTubeAccountProviderStage::Connected;
	}
	if (state == YouTubeAccountState::Configured) {
		return YouTubeAccountProviderStage::Configured;
	}
	return fallback;
}

} // namespace

class YouTubeAccountProvider::Impl final {
public:
	Impl(YouTubeAccountProvider *owner, QString clientId,
	     std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort,
	     std::unique_ptr<YouTubeAccountTokenPort> tokenPort,
	     std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort,
	     YouTubeAccountRefreshTokenStore &refreshTokenStore,
	     YouTubeAccountProfileOperationLockProvider &profileOperationLockProvider, std::string profileBinding,
	     YouTubeAccountSelectionCommitter selectionCommitter)
		: owner_(owner),
		  clientId_(std::move(clientId)),
		  authorizationPort_(std::move(authorizationPort)),
		  tokenPort_(std::move(tokenPort)),
		  discoveryPort_(std::move(discoveryPort)),
		  refreshTokenStore_(refreshTokenStore),
		  profileOperationLockProvider_(profileOperationLockProvider),
		  profileBinding_(std::move(profileBinding)),
		  selectionCommitter_(std::move(selectionCommitter))
	{
		profileBindingValid_ = isValidYouTubeAccountProfileBinding(profileBinding_);
	}

	YouTubeAccountProviderStartStatus startConnection(GoogleOAuthConsentMode consentMode) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeAccountProviderStartStatus::WrongThread;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderStartStatus::Closed;
		}
		if (externalOperationLockBorrowed_) {
			return YouTubeAccountProviderStartStatus::Busy;
		}
		if (!profileBindingValid_) {
			return YouTubeAccountProviderStartStatus::InvalidProfileBinding;
		}
		if (commitInProgress_ || lifecycleMutationInProgress_ || activeLease().has_value()) {
			return YouTubeAccountProviderStartStatus::Busy;
		}
		if (!isValidGoogleOAuthClientId(clientId_)) {
			return YouTubeAccountProviderStartStatus::InvalidClientId;
		}
		if (!selectionCommitter_) {
			return YouTubeAccountProviderStartStatus::InvalidSelectionCommitter;
		}

		const auto lockResult = profileOperationLockProvider_.acquire(profileBinding_, operationLock_);
		switch (lockResult.status) {
		case YouTubeAccountProfileOperationLockStatus::Acquired:
		case YouTubeAccountProfileOperationLockStatus::Recovered:
			break;
		case YouTubeAccountProfileOperationLockStatus::Busy:
			return YouTubeAccountProviderStartStatus::Busy;
		case YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding:
			return YouTubeAccountProviderStartStatus::InvalidProfileBinding;
		case YouTubeAccountProfileOperationLockStatus::Unavailable:
			return YouTubeAccountProviderStartStatus::OperationFailed;
		}

		try {
			clearEphemeral();
			const auto transition = coordinator_.beginAuthorization();
			if (!transition.changed || !transition.snapshot.lease.has_value()) {
				if (!releaseOperationLockOrMarkUnavailable()) {
					return YouTubeAccountProviderStartStatus::OperationFailed;
				}
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
			(void)releaseOperationLockOrMarkUnavailable();
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
		if (lifecycleMutationInProgress_) {
			return YouTubeAccountProviderSelectionStatus::WrongStage;
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
		if (lifecycleMutationInProgress_) {
			return YouTubeAccountProviderSelectionStatus::WrongStage;
		}
		if (!leaseIsActive(lease)) {
			return YouTubeAccountProviderSelectionStatus::StaleLease;
		}
		if (stage_ != YouTubeAccountProviderStage::AwaitingStreamSelection) {
			return YouTubeAccountProviderSelectionStatus::WrongStage;
		}
		return selectStreamCandidate(lease, streamId);
	}

	YouTubeAccountProviderRestoreStatus restoreSavedStateWhileOperationLockHeld(
		std::string profileBinding, std::optional<YouTubeAccountSelection> selection) noexcept
	{
		const bool validSelection = selection.has_value() &&
					    validateYouTubeAccountSelection(*selection) ==
						    YouTubeAccountSelectionValidationError::None;

		lifecycleMutationInProgress_ = true;
		operationEpoch_ = nextNonZero(operationEpoch_);
		clearEphemeral();
		try {
			profileBinding_ = std::move(profileBinding);
			profileBindingValid_ = true;
			if (!selection.has_value()) {
				coordinator_.invalidateContext();
				savedSelection_.reset();
				stage_ = YouTubeAccountProviderStage::Idle;
				touch();
				lifecycleMutationInProgress_ = false;
				return YouTubeAccountProviderRestoreStatus::SetupRequired;
			}

			if (!validSelection) {
				savedSelection_.reset();
				coordinator_.restoreSavedConnection(*selection,
								    YouTubeAccountSavedCredentialState::Present);
				stage_ = YouTubeAccountProviderStage::Failed;
				touch();
				lifecycleMutationInProgress_ = false;
				return YouTubeAccountProviderRestoreStatus::InvalidSelection;
			}

			const CredentialStatus credential = refreshTokenStore_.status(credentialScope(*selection));
			YouTubeAccountSavedCredentialState savedCredential =
				YouTubeAccountSavedCredentialState::Unavailable;
			YouTubeAccountProviderRestoreStatus restoreStatus =
				YouTubeAccountProviderRestoreStatus::CredentialUnavailable;
			YouTubeAccountProviderStage restoredStage = YouTubeAccountProviderStage::Unavailable;

			if (credential.state == CredentialState::Present && credential.result.succeeded()) {
				savedCredential = YouTubeAccountSavedCredentialState::Present;
				restoreStatus = YouTubeAccountProviderRestoreStatus::Configured;
				restoredStage = YouTubeAccountProviderStage::Configured;
			} else if (credential.state == CredentialState::Missing &&
				   credential.result.error == CredentialError::NotFound) {
				savedCredential = YouTubeAccountSavedCredentialState::Missing;
				restoreStatus = YouTubeAccountProviderRestoreStatus::ReauthorizationRequired;
				restoredStage = YouTubeAccountProviderStage::NeedsReauthorization;
			} else if (credential.state == CredentialState::NeedsReauthorization ||
				   credential.result.error == CredentialError::ScopeMismatch) {
				savedCredential = YouTubeAccountSavedCredentialState::Missing;
				restoreStatus = YouTubeAccountProviderRestoreStatus::ReauthorizationRequired;
				restoredStage = YouTubeAccountProviderStage::NeedsReauthorization;
			}

			coordinator_.restoreSavedConnection(*selection, savedCredential);
			savedSelection_ = *selection;
			stage_ = restoredStage;
			touch();
			lifecycleMutationInProgress_ = false;
			return restoreStatus;
		} catch (...) {
			try {
				coordinator_.invalidateContext();
			} catch (...) {
			}
			savedSelection_.reset();
			stage_ = YouTubeAccountProviderStage::Failed;
			touch();
			lifecycleMutationInProgress_ = false;
			return YouTubeAccountProviderRestoreStatus::OperationFailed;
		}
	}

	YouTubeAccountProviderRestoreStatus restoreSavedState(std::string profileBinding,
							      std::optional<YouTubeAccountSelection> selection) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeAccountProviderRestoreStatus::WrongThread;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderRestoreStatus::Closed;
		}
		if (externalOperationLockBorrowed_) {
			return YouTubeAccountProviderRestoreStatus::Busy;
		}
		if (commitInProgress_ || lifecycleMutationInProgress_ || activeLease().has_value()) {
			return YouTubeAccountProviderRestoreStatus::Busy;
		}
		// A prior native cleanup failure retains either this profile's ownership
		// or a handle-close retry. Do not let a credential-free restore rewrite
		// visible context around that unfinished transaction; explicit invalidation
		// or shutdown performs owner-thread cleanup.
		if (operationLock_.cleanupPending()) {
			return YouTubeAccountProviderRestoreStatus::Busy;
		}
		if (!isValidYouTubeAccountProfileBinding(profileBinding)) {
			profileBinding_.clear();
			profileBindingValid_ = false;
			savedSelection_.reset();
			operationEpoch_ = nextNonZero(operationEpoch_);
			clearEphemeral();
			try {
				coordinator_.invalidateContext();
			} catch (...) {
			}
			setStage(YouTubeAccountProviderStage::Failed);
			return YouTubeAccountProviderRestoreStatus::InvalidProfileBinding;
		}

		// A missing or malformed selection never reads the credential store, so
		// it does not need to take the cross-process account-operation lease.
		// For a valid selection, acquire before changing any provider-visible
		// state. A failed acquisition must leave the current profile context and
		// snapshot untouched.
		const bool validSelection = selection.has_value() &&
					    validateYouTubeAccountSelection(*selection) ==
						    YouTubeAccountSelectionValidationError::None;
		bool lockHeld = false;
		if (validSelection) {
			const auto lockResult = profileOperationLockProvider_.acquire(profileBinding, operationLock_);
			switch (lockResult.status) {
			case YouTubeAccountProfileOperationLockStatus::Acquired:
			case YouTubeAccountProfileOperationLockStatus::Recovered:
				lockHeld = true;
				break;
			case YouTubeAccountProfileOperationLockStatus::Busy:
				return YouTubeAccountProviderRestoreStatus::Busy;
			case YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding:
				return YouTubeAccountProviderRestoreStatus::InvalidProfileBinding;
			case YouTubeAccountProfileOperationLockStatus::Unavailable:
				return YouTubeAccountProviderRestoreStatus::OperationFailed;
			}
		}

		const auto restoreStatus = restoreSavedStateWhileOperationLockHeld(std::move(profileBinding),
											 std::move(selection));
		if (!lockHeld) {
			return restoreStatus;
		}
		return releaseOperationLockOrMarkUnavailable() ? restoreStatus
										   : YouTubeAccountProviderRestoreStatus::OperationFailed;
	}

	YouTubeAccountProviderRestoreStatus restoreSavedStateUnderHeldOperationLock(
		std::string profileBinding, std::optional<YouTubeAccountSelection> selection,
		const YouTubeAccountProfileOperationLock &heldLock) noexcept
	{
		// Validate the borrowed lease before examining or changing provider state.
		// This also rejects a lock acquired by a different thread, because the
		// lock object records its owner thread.
		if (!onOwnerThread()) {
			return YouTubeAccountProviderRestoreStatus::WrongThread;
		}
		if (!heldLock.acquiredFor(profileBinding)) {
			return YouTubeAccountProviderRestoreStatus::OperationFailed;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderRestoreStatus::Closed;
		}
		if (externalOperationLockBorrowed_ || commitInProgress_ || lifecycleMutationInProgress_ ||
		    activeLease().has_value() ||
		    operationLock_.cleanupPending()) {
			return YouTubeAccountProviderRestoreStatus::Busy;
		}
		externalOperationLockBorrowed_ = true;
		return restoreSavedStateWhileOperationLockHeld(std::move(profileBinding), std::move(selection));
	}

	YouTubeAccountProviderDisconnectStatus disconnectSavedStateUnderHeldOperationLock(
		std::string profileBinding, std::optional<YouTubeAccountSelection> selection,
		const YouTubeAccountProfileOperationLock &heldLock,
		YouTubeAccountSelectionCommitter selectionCommitter) noexcept
	{
		// The profile restore coordinator owns the lock and remains responsible for
		// releasing it.  Validate every input before setting the borrowed-lock
		// handshake or touching the credential store so a rejected request has no
		// provider-visible side effect.
		if (!onOwnerThread()) {
			return YouTubeAccountProviderDisconnectStatus::WrongThread;
		}
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return YouTubeAccountProviderDisconnectStatus::Closed;
		}
		if (externalOperationLockBorrowed_ || commitInProgress_ || lifecycleMutationInProgress_ ||
		    activeLease().has_value() || operationLock_.cleanupPending()) {
			return YouTubeAccountProviderDisconnectStatus::Busy;
		}
		if (!isValidYouTubeAccountProfileBinding(profileBinding) || !profileBindingValid_ ||
		    profileBinding_ != profileBinding) {
			return YouTubeAccountProviderDisconnectStatus::InvalidProfileBinding;
		}
		if (!heldLock.acquiredFor(profileBinding)) {
			return YouTubeAccountProviderDisconnectStatus::OperationFailed;
		}

		if (selection.has_value()) {
			if (validateYouTubeAccountSelection(*selection) != YouTubeAccountSelectionValidationError::None ||
			    !savedSelection_.has_value() || !sameSelection(*savedSelection_, *selection)) {
				return YouTubeAccountProviderDisconnectStatus::InvalidSelection;
			}
			// Do not erase a credential until the transaction-scoped profile
			// committer is known to be callable. The constructor committer belongs
			// to the last restore generation and is deliberately not used here.
			if (!selectionCommitter) {
				return YouTubeAccountProviderDisconnectStatus::InvalidSelectionCommitter;
			}
		}

		externalOperationLockBorrowed_ = true;
		lifecycleMutationInProgress_ = true;
		operationEpoch_ = nextNonZero(operationEpoch_);

		if (!selection.has_value()) {
			// There is no safe credential scope without a selected channel.  This is
			// a valid setup-required state, and must never probe or erase another
			// channel's credential target.
			clearEphemeral();
			savedSelection_.reset();
			try {
				coordinator_.invalidateContext();
				setStage(YouTubeAccountProviderStage::Idle);
				touch();
				lifecycleMutationInProgress_ = false;
				return YouTubeAccountProviderDisconnectStatus::AlreadyDisconnected;
			} catch (...) {
				setStage(YouTubeAccountProviderStage::Unavailable);
				lifecycleMutationInProgress_ = false;
				return YouTubeAccountProviderDisconnectStatus::OperationFailed;
			}
		}

		CredentialResult eraseResult;
		try {
			eraseResult = refreshTokenStore_.erase(credentialScope(*selection));
		} catch (...) {
			eraseResult.error = CredentialError::OperatingSystemError;
		}
		if (!eraseResult.succeeded() && eraseResult.error != CredentialError::NotFound) {
			// The durable selection and the in-memory usable account remain
			// unchanged. The outer transaction will release the borrowed lock and
			// call externalOperationLockReleased() after this return.
			lifecycleMutationInProgress_ = false;
			return YouTubeAccountProviderDisconnectStatus::CredentialUnavailable;
		}

		bool selectionCleared = false;
		try {
			selectionCleared = selectionCommitter(std::nullopt);
		} catch (...) {
			selectionCleared = false;
		}
		if (!selectionCleared) {
			// The token has already been erased and must never be recreated from an
			// old in-memory value. Keep the non-secret selection visible as an
			// unavailable account until the profile transaction can be retried.
			operationEpoch_ = nextNonZero(operationEpoch_);
			clearEphemeral();
			savedSelection_ = *selection;
			try {
				coordinator_.restoreSavedConnection(*selection,
								    YouTubeAccountSavedCredentialState::Unavailable);
			} catch (...) {
				try {
					coordinator_.invalidateContext();
				} catch (...) {
				}
			}
			setStage(YouTubeAccountProviderStage::Unavailable);
			touch();
			lifecycleMutationInProgress_ = false;
			return YouTubeAccountProviderDisconnectStatus::ProfileSaveFailed;
		}

		clearEphemeral();
		savedSelection_.reset();
		try {
			coordinator_.invalidateContext();
			setStage(YouTubeAccountProviderStage::Idle);
			touch();
			lifecycleMutationInProgress_ = false;
			return YouTubeAccountProviderDisconnectStatus::Disconnected;
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Unavailable);
			lifecycleMutationInProgress_ = false;
			return YouTubeAccountProviderDisconnectStatus::OperationFailed;
		}
	}

	bool cancel(YouTubeAccountLease lease) noexcept
	{
		if (!onOwnerThread() || stage_ == YouTubeAccountProviderStage::Closed || commitInProgress_ ||
		    lifecycleMutationInProgress_ || !leaseIsActive(lease)) {
			return false;
		}
		operationEpoch_ = nextNonZero(operationEpoch_);
		lifecycleMutationInProgress_ = true;
		cancelPorts(lease);
		clearEphemeral();
		try {
			const auto transition = coordinator_.cancel(lease);
			if (!transition.changed) {
				lifecycleMutationInProgress_ = false;
				failAttempt(lease, YouTubeAccountFailure::ServiceUnavailable);
				return false;
			}
			setStage(providerStageForRetainedConnection(transition.snapshot.state,
								    YouTubeAccountProviderStage::Idle));
			lifecycleMutationInProgress_ = false;
			return releaseOperationLockOrMarkUnavailable();
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Failed);
			lifecycleMutationInProgress_ = false;
			(void)releaseOperationLockOrMarkUnavailable();
			return false;
		}
	}

	bool invalidateContext() noexcept
	{
		if (!onOwnerThread() || stage_ == YouTubeAccountProviderStage::Closed || commitInProgress_ ||
		    lifecycleMutationInProgress_ || externalOperationLockBorrowed_) {
			return false;
		}
		lifecycleMutationInProgress_ = true;
		operationEpoch_ = nextNonZero(operationEpoch_);
		if (const auto lease = activeLease(); lease.has_value()) {
			cancelPorts(*lease);
		}
		clearEphemeral();
		savedSelection_.reset();
		try {
			coordinator_.invalidateContext();
			setStage(YouTubeAccountProviderStage::Idle);
			lifecycleMutationInProgress_ = false;
			return releaseOperationLockOrMarkUnavailable();
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Failed);
			lifecycleMutationInProgress_ = false;
			(void)releaseOperationLockOrMarkUnavailable();
			return false;
		}
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread() || commitInProgress_ || externalOperationLockBorrowed_) {
			return false;
		}
		if (lifecycleMutationInProgress_) {
			return false;
		}
		lifecycleMutationInProgress_ = true;
		if (stage_ != YouTubeAccountProviderStage::Closed) {
			const auto lease = activeLease();
			setStage(YouTubeAccountProviderStage::Closed);
			operationEpoch_ = nextNonZero(operationEpoch_);
			if (lease.has_value()) {
				cancelPorts(*lease);
			}
			clearEphemeral();
		}

		if (!coordinatorClosed_) {
			try {
				coordinator_.shutdown();
				coordinatorClosed_ = true;
			} catch (...) {
			}
		}
		if (!authorizationPortClosed_) {
			authorizationPortClosed_ = authorizationPort_->shutdown();
		}
		if (!tokenPortClosed_) {
			tokenPortClosed_ = tokenPort_->shutdown();
		}
		if (!discoveryPortClosed_) {
			discoveryPortClosed_ = discoveryPort_->shutdown();
		}
		const bool lockReleased = operationLock_.release();
		lifecycleMutationInProgress_ = false;
		return coordinatorClosed_ && authorizationPortClosed_ && tokenPortClosed_ && discoveryPortClosed_ &&
		       lockReleased;
	}

	void markExternalOperationReleaseFailed() noexcept
	{
		// The outer transaction owns the borrowed lock. If releasing it fails,
		// invalidate all provider-visible account state without trying to touch
		// that lock again. The transaction retains ownership and is responsible
		// for its owner-thread cleanup retry.
		if (!onOwnerThread()) {
			return;
		}
		externalOperationLockBorrowed_ = true;
		if (stage_ == YouTubeAccountProviderStage::Closed) {
			return;
		}
		operationEpoch_ = nextNonZero(operationEpoch_);
		clearEphemeral();
		try {
			if (savedSelection_.has_value()) {
				coordinator_.restoreSavedConnection(*savedSelection_,
								    YouTubeAccountSavedCredentialState::Unavailable);
			} else {
				coordinator_.invalidateContext();
			}
		} catch (...) {
			try {
				coordinator_.invalidateContext();
			} catch (...) {
			}
		}
		setStage(YouTubeAccountProviderStage::Unavailable);
	}

	void externalOperationLockReleased() noexcept
	{
		if (onOwnerThread()) {
			externalOperationLockBorrowed_ = false;
		}
	}

	void shutdownAfterExternalOperationLockFailure() noexcept
	{
		if (!onOwnerThread()) {
			return;
		}
		// The owning restore coordinator is being destroyed and cannot service
		// another account operation. Keep the native lock fail-closed, but allow
		// provider teardown to close every port and reach its terminal state.
		externalOperationLockBorrowed_ = false;
		(void)shutdown();
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

	YouTubeAccountCredentialScope credentialScope(const YouTubeAccountSelection &selection) const
	{
		return {profileBinding_, selection.channelId};
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

	bool releaseOperationLockOrMarkUnavailable() noexcept
	{
		if (operationLock_.release()) {
			return true;
		}

		// ReleaseMutex failure leaves the native mutex and process-local claim
		// held; CloseHandle failure has already relinquished ownership but still
		// represents incomplete cleanup. Do not expose either case as a usable
		// account result. Preserve durable state so owner-thread shutdown or
		// context invalidation can retry any unfinished native cleanup.
		try {
			if (savedSelection_.has_value()) {
				(void)coordinator_.restoreSavedConnection(
					*savedSelection_, YouTubeAccountSavedCredentialState::Unavailable);
			} else {
				(void)coordinator_.invalidateContext();
			}
		} catch (...) {
			// Both coordinator transitions change the internal snapshot before
			// returning its potentially allocating value. A fallback invalidation
			// also covers an allocation failure before restore could commit.
			try {
				(void)coordinator_.invalidateContext();
			} catch (...) {
			}
		}
		setStage(YouTubeAccountProviderStage::Unavailable);
		return false;
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
			(void)releaseOperationLockOrMarkUnavailable();
		}
	}

	void failAttempt(YouTubeAccountLease lease, YouTubeAccountFailure failure) noexcept
	{
		const bool priorLifecycleMutation = lifecycleMutationInProgress_;
		lifecycleMutationInProgress_ = true;
		operationEpoch_ = nextNonZero(operationEpoch_);
		cancelPorts(lease);
		clearEphemeral();
		try {
			const auto transition = coordinator_.attemptFailed(lease, failure);
			setStage(providerStageForRetainedConnection(transition.snapshot.state,
								    YouTubeAccountProviderStage::Failed));
		} catch (...) {
			setStage(YouTubeAccountProviderStage::Failed);
		}
		lifecycleMutationInProgress_ = priorLifecycleMutation;
		(void)releaseOperationLockOrMarkUnavailable();
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
		if (commitInProgress_ || refreshToken_.empty() || !leaseIsActive(lease) || !profileBindingValid_) {
			failAttempt(lease, YouTubeAccountFailure::InvalidResponse);
			return false;
		}

		commitInProgress_ = true;
		std::optional<YouTubeAccountSelection> previousSelection;
		std::optional<YouTubeAccountSelection> nextSelection;
		std::optional<YouTubeAccountCredentialScope> previousScope;
		CredentialReadResult previous;
		YouTubeAccountCredentialScope scope;
		try {
			previousSelection = savedSelection_;
			nextSelection = selection;
			scope = credentialScope(selection);
			if (previousSelection.has_value()) {
				previousScope = credentialScope(*previousSelection);
				previous = refreshTokenStore_.read(*previousScope);
			} else {
				// A profile with no saved selection has no owned prior account
				// credential. Do not inspect another channel's shared target.
				previous.result = {CredentialError::NotFound, 0};
			}
		} catch (...) {
			commitInProgress_ = false;
			failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			return false;
		}
		const bool previousCredentialPresent = previous.result.succeeded();
		const bool previousCredentialMissing = previous.result.error == CredentialError::NotFound ||
						       previous.result.error == CredentialError::ScopeMismatch ||
						       previous.result.error == CredentialError::CorruptData;
		if (!previousCredentialPresent && !previousCredentialMissing) {
			commitInProgress_ = false;
			failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			return false;
		}

		// Save the non-secret selection first. This makes the profile the source
		// of truth while the new refresh token is being staged. If the token
		// write fails, restore the exact prior optional selection synchronously.
		bool selectionSaved = false;
		try {
			selectionSaved = selectionCommitter_(selection);
		} catch (...) {
			selectionSaved = false;
		}
		if (!selectionSaved) {
			commitInProgress_ = false;
			previous.secret.clear();
			failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			return false;
		}

		CredentialResult writeResult;
		try {
			writeResult = refreshTokenStore_.write(scope, refreshToken_.view());
		} catch (...) {
			writeResult.error = CredentialError::OperatingSystemError;
		}
		if (!writeResult.succeeded()) {
			const bool selectionRolledBack = rollbackSelection(previousSelection);
			previous.secret.clear();
			commitInProgress_ = false;
			if (!selectionRolledBack) {
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
			// The non-secret profile and scoped credential are already saved. Do not
			// claim either the old or new account is usable if the invariant was
			// violated. Restore both durable stores before failing closed.
			const bool credentialRolledBack =
				rollbackCredential(scope, previousScope, previous, previousCredentialPresent);
			const bool selectionRolledBack = rollbackSelection(previousSelection);
			previous.secret.clear();
			if (!credentialRolledBack || !selectionRolledBack) {
				failCredentialStateUncertain(lease);
			} else {
				failAttempt(lease, YouTubeAccountFailure::CredentialUnavailable);
			}
			return false;
		}
		previous.secret.clear();
		// nextSelection was prepared before any durable side effect, so this
		// move cannot allocate. The saved-selection cache advances only after
		// the coordinator's visibility commit succeeds.
		savedSelection_ = std::move(nextSelection);

		channels_.clear();
		streams_.clear();
		selectedChannel_.reset();
		selectedStream_.reset();
		setStage(YouTubeAccountProviderStage::Connected);
		return releaseOperationLockOrMarkUnavailable();
	}

	bool rollbackCredential(const YouTubeAccountCredentialScope &newScope,
				const std::optional<YouTubeAccountCredentialScope> &previousScope,
				CredentialReadResult &previous, bool previousCredentialPresent) noexcept
	{
		CredentialResult result;
		try {
			if (previousCredentialPresent && previousScope.has_value()) {
				// The Windows target is profile-scoped and the channel binding is
				// carried in the credential metadata. Writing the old scope restores
				// the single record; deleting the new scope here would target the same
				// record and report ScopeMismatch after the restore.
				result = refreshTokenStore_.write(*previousScope, previous.secret.view());
			} else {
				result = refreshTokenStore_.erase(newScope);
			}
		} catch (...) {
			result.error = CredentialError::OperatingSystemError;
		}
		previous.secret.clear();
		return result.succeeded() || (!previousCredentialPresent && result.error == CredentialError::NotFound);
	}

	bool rollbackSelection(const std::optional<YouTubeAccountSelection> &previous) noexcept
	{
		try {
			return selectionCommitter_(previous);
		} catch (...) {
			return false;
		}
	}

	void failCredentialStateUncertain(YouTubeAccountLease lease) noexcept
	{
		const bool priorLifecycleMutation = lifecycleMutationInProgress_;
		lifecycleMutationInProgress_ = true;
		operationEpoch_ = nextNonZero(operationEpoch_);
		cancelPorts(lease);
		clearEphemeral();
		savedSelection_.reset();
		try {
			coordinator_.credentialStateUncertain(lease);
		} catch (...) {
		}
		setStage(YouTubeAccountProviderStage::Failed);
		lifecycleMutationInProgress_ = priorLifecycleMutation;
		(void)releaseOperationLockOrMarkUnavailable();
	}

	YouTubeAccountProvider *owner_ = nullptr;
	QString clientId_;
	std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort_;
	std::unique_ptr<YouTubeAccountTokenPort> tokenPort_;
	std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort_;
	YouTubeAccountRefreshTokenStore &refreshTokenStore_;
	YouTubeAccountProfileOperationLockProvider &profileOperationLockProvider_;
	YouTubeAccountProfileOperationLock operationLock_;
	std::string profileBinding_;
	bool profileBindingValid_ = false;
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
	std::optional<YouTubeAccountSelection> savedSelection_;
	bool commitInProgress_ = false;
	bool lifecycleMutationInProgress_ = false;
	bool coordinatorClosed_ = false;
	bool authorizationPortClosed_ = false;
	bool tokenPortClosed_ = false;
	bool discoveryPortClosed_ = false;
	bool externalOperationLockBorrowed_ = false;
};

YouTubeAccountProvider::YouTubeAccountProvider(QString clientId,
					       GoogleOAuthAuthorizationSession::BrowserOpener browserOpener,
					       YouTubeAccountRefreshTokenStore &refreshTokenStore,
					       YouTubeAccountProfileOperationLockProvider &profileOperationLockProvider,
					       std::string profileBinding,
					       YouTubeAccountSelectionCommitter selectionCommitter, QObject *parent)
	: YouTubeAccountProvider(std::move(clientId), std::make_unique<AuthorizationAdapter>(std::move(browserOpener)),
				 std::make_unique<TokenAdapter>(), std::make_unique<DiscoveryAdapter>(),
				 refreshTokenStore, profileOperationLockProvider, std::move(profileBinding),
				 std::move(selectionCommitter), parent)
{
}

YouTubeAccountProvider::YouTubeAccountProvider(QString clientId,
					       std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort,
					       std::unique_ptr<YouTubeAccountTokenPort> tokenPort,
					       std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort,
					       YouTubeAccountRefreshTokenStore &refreshTokenStore,
					       YouTubeAccountProfileOperationLockProvider &profileOperationLockProvider,
					       std::string profileBinding,
					       YouTubeAccountSelectionCommitter selectionCommitter, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(clientId), std::move(authorizationPort), std::move(tokenPort),
				       std::move(discoveryPort), refreshTokenStore, profileOperationLockProvider,
				       std::move(profileBinding), std::move(selectionCommitter)))
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

YouTubeAccountProviderRestoreStatus
YouTubeAccountProvider::restoreSavedState(std::string profileBinding,
						  std::optional<YouTubeAccountSelection> selection) noexcept
{
	return impl_->restoreSavedState(std::move(profileBinding), std::move(selection));
}

YouTubeAccountProviderRestoreStatus YouTubeAccountProvider::restoreSavedStateUnderHeldOperationLock(
	std::string profileBinding, std::optional<YouTubeAccountSelection> selection,
	const YouTubeAccountProfileOperationLock &heldLock) noexcept
{
	return impl_->restoreSavedStateUnderHeldOperationLock(std::move(profileBinding), std::move(selection), heldLock);
}

YouTubeAccountProviderDisconnectStatus YouTubeAccountProvider::disconnectSavedStateUnderHeldOperationLock(
	std::string profileBinding, std::optional<YouTubeAccountSelection> selection,
	const YouTubeAccountProfileOperationLock &heldLock,
	YouTubeAccountSelectionCommitter selectionCommitter) noexcept
{
	return impl_->disconnectSavedStateUnderHeldOperationLock(std::move(profileBinding), std::move(selection),
									 heldLock, std::move(selectionCommitter));
}

void YouTubeAccountProvider::externalOperationLockReleased() noexcept
{
	impl_->externalOperationLockReleased();
}

void YouTubeAccountProvider::markExternalOperationReleaseFailed() noexcept
{
	impl_->markExternalOperationReleaseFailed();
}

void YouTubeAccountProvider::shutdownAfterExternalOperationLockFailure() noexcept
{
	impl_->shutdownAfterExternalOperationLockFailure();
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

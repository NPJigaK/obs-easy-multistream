// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-remote-revoke.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cassert>
#include <limits>
#include <utility>

namespace easy_multistream {
namespace {

std::uint64_t nextNonZero(std::uint64_t value) noexcept
{
	return value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

GoogleOAuthTokenAttempt tokenAttempt(YouTubeAccountRemoteRevokeAttempt attempt) noexcept
{
	return {attempt.generation, attempt.attempt};
}

bool matches(GoogleOAuthTokenAttempt attempt, YouTubeAccountRemoteRevokeAttempt expected) noexcept
{
	return attempt.generation == expected.generation && attempt.attempt == expected.attempt;
}

bool providerCanRevokeSavedState(YouTubeAccountProviderStage stage) noexcept
{
	switch (stage) {
	case YouTubeAccountProviderStage::Configured:
	case YouTubeAccountProviderStage::Connected:
	case YouTubeAccountProviderStage::NeedsReauthorization:
	case YouTubeAccountProviderStage::Unavailable:
		return true;
	case YouTubeAccountProviderStage::Idle:
	case YouTubeAccountProviderStage::Authorizing:
	case YouTubeAccountProviderStage::ExchangingCode:
	case YouTubeAccountProviderStage::ListingChannels:
	case YouTubeAccountProviderStage::AwaitingChannelSelection:
	case YouTubeAccountProviderStage::ListingStreams:
	case YouTubeAccountProviderStage::AwaitingStreamSelection:
	case YouTubeAccountProviderStage::PersistingCredential:
	case YouTubeAccountProviderStage::Failed:
	case YouTubeAccountProviderStage::Closed:
		return false;
	}
	return false;
}

bool providerMatchesSelection(const YouTubeAccountProviderSnapshot &provider,
			      std::string_view profileBinding,
			      const YouTubeAccountSelection &selection) noexcept
{
	return provider.profileBinding == profileBinding &&
	       provider.account.channelId == selection.channelId &&
	       provider.account.channelLabel == selection.channelLabel &&
	       provider.account.streamId == selection.streamId &&
	       provider.account.streamLabel == selection.streamLabel;
}

class ScopedFlag final {
public:
	explicit ScopedFlag(bool &flag) noexcept : flag_(flag) { flag_ = true; }
	~ScopedFlag() { flag_ = false; }

	ScopedFlag(const ScopedFlag &) = delete;
	ScopedFlag &operator=(const ScopedFlag &) = delete;

private:
	bool &flag_;
};

YouTubeAccountRemoteRevokeStartStatus
mapPreflightStatus(YouTubeAccountProfileContext::ActiveAccountStatus status) noexcept
{
	switch (status) {
	case YouTubeAccountProfileContext::ActiveAccountStatus::Current:
		return YouTubeAccountRemoteRevokeStartStatus::Started;
	case YouTubeAccountProfileContext::ActiveAccountStatus::Stale:
		return YouTubeAccountRemoteRevokeStartStatus::ProfileChanged;
	case YouTubeAccountProfileContext::ActiveAccountStatus::NotAccountMode:
		return YouTubeAccountRemoteRevokeStartStatus::NotAccountMode;
	case YouTubeAccountProfileContext::ActiveAccountStatus::ProfileUnavailable:
		return YouTubeAccountRemoteRevokeStartStatus::ProfileUnavailable;
	case YouTubeAccountProfileContext::ActiveAccountStatus::InvalidSettings:
	case YouTubeAccountProfileContext::ActiveAccountStatus::UnsupportedFutureSettings:
		return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
	case YouTubeAccountProfileContext::ActiveAccountStatus::Unavailable:
		return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
	}
	return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
}

YouTubeAccountRemoteRevokeStatus mapStartFailure(GoogleOAuthTokenStartStatus status) noexcept
{
	switch (status) {
	case GoogleOAuthTokenStartStatus::Closed:
		return YouTubeAccountRemoteRevokeStatus::Closed;
	case GoogleOAuthTokenStartStatus::WrongThread:
		return YouTubeAccountRemoteRevokeStatus::OperationFailed;
	case GoogleOAuthTokenStartStatus::InvalidToken:
	case GoogleOAuthTokenStartStatus::InvalidAttempt:
	case GoogleOAuthTokenStartStatus::InvalidCompletionHandler:
	case GoogleOAuthTokenStartStatus::InvalidClientId:
	case GoogleOAuthTokenStartStatus::InvalidRedirectUri:
	case GoogleOAuthTokenStartStatus::InvalidAuthorizationCode:
	case GoogleOAuthTokenStartStatus::InvalidCodeVerifier:
		return YouTubeAccountRemoteRevokeStatus::InvalidResponse;
	case GoogleOAuthTokenStartStatus::Busy:
	case GoogleOAuthTokenStartStatus::InvalidOptions:
	case GoogleOAuthTokenStartStatus::RequestCreationFailed:
		return YouTubeAccountRemoteRevokeStatus::ServiceUnavailable;
	case GoogleOAuthTokenStartStatus::Started:
		return YouTubeAccountRemoteRevokeStatus::InvalidResponse;
	case GoogleOAuthTokenStartStatus::InvalidRefreshToken:
		return YouTubeAccountRemoteRevokeStatus::InvalidResponse;
	}
	return YouTubeAccountRemoteRevokeStatus::OperationFailed;
}

YouTubeAccountRemoteRevokeStatus mapCompletionFailure(const GoogleOAuthTokenCompletion &completion) noexcept
{
	if (completion.providerError == GoogleOAuthTokenProviderError::RateLimited ||
	    completion.providerError == GoogleOAuthTokenProviderError::TemporarilyUnavailable) {
		return YouTubeAccountRemoteRevokeStatus::ServiceUnavailable;
	}
	if (completion.providerError != GoogleOAuthTokenProviderError::None) {
		return YouTubeAccountRemoteRevokeStatus::RemoteRejected;
	}

	switch (completion.status) {
	case GoogleOAuthTokenCompletionStatus::NetworkFailure:
	case GoogleOAuthTokenCompletionStatus::TlsFailure:
	case GoogleOAuthTokenCompletionStatus::TimedOut:
		return YouTubeAccountRemoteRevokeStatus::NetworkFailure;
	case GoogleOAuthTokenCompletionStatus::RedirectRejected:
	case GoogleOAuthTokenCompletionStatus::ResponseTooLarge:
	case GoogleOAuthTokenCompletionStatus::InvalidResponse:
		return YouTubeAccountRemoteRevokeStatus::InvalidResponse;
	case GoogleOAuthTokenCompletionStatus::ProviderRejected:
	case GoogleOAuthTokenCompletionStatus::HttpFailure:
		return YouTubeAccountRemoteRevokeStatus::RemoteRejected;
	case GoogleOAuthTokenCompletionStatus::Success:
		return YouTubeAccountRemoteRevokeStatus::InvalidResponse;
	}
	return YouTubeAccountRemoteRevokeStatus::OperationFailed;
}

} // namespace

class RevokeAdapter final : public YouTubeAccountRemoteRevokePort {
public:
	GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
						CompletionHandler completionHandler) noexcept override
	{
		return transport_.startRevoke(std::move(request), std::move(completionHandler));
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override { return transport_.cancel(attempt); }
	bool shutdown() noexcept override { return transport_.shutdown(); }

private:
	GoogleOAuthTokenTransport transport_;
};

class YouTubeAccountRemoteRevokeCoordinator::Impl final {
public:
	Impl(YouTubeAccountRemoteRevokeCoordinator *owner, YouTubeAccountProfileContext &context,
	     YouTubeAccountProvider &provider, YouTubeAccountRefreshTokenStore &refreshTokenStore,
	     YouTubeAccountProfileOperationLockProvider &operationLockProvider,
	     std::unique_ptr<YouTubeAccountRemoteRevokePort> revokePort)
		: owner_(owner),
		  context_(context),
		  provider_(provider),
		  refreshTokenStore_(refreshTokenStore),
		  operationLockProvider_(operationLockProvider),
		  revokePort_(std::move(revokePort))
	{
	}

	YouTubeAccountRemoteRevokeStartStatus start(YouTubeAccountRemoteRevokeRequest request,
						    CompletionHandler completionHandler) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeAccountRemoteRevokeStartStatus::WrongThread;
		}
		if (state_ == YouTubeAccountRemoteRevokeState::Closed || closed_) {
			return YouTubeAccountRemoteRevokeStartStatus::Closed;
		}
		if (lifecycleMutationInProgress_ || preflightBoundaryInProgress_ || credentialReadInProgress_ ||
		    lockReleaseInProgress_ ||
		    transactionActive_ || activeAttempt_.has_value() ||
		    pendingCompletion_.has_value() || completionDeliveryQueued_ || operationLock_.cleanupPending()) {
			return YouTubeAccountRemoteRevokeStartStatus::Busy;
		}
		if (!request.isValid()) {
			return isValidYouTubeAccountProfileBinding(request.expectedProfileBinding)
				       ? YouTubeAccountRemoteRevokeStartStatus::OperationFailed
				       : YouTubeAccountRemoteRevokeStartStatus::InvalidProfileBinding;
		}
		if (!completionHandler) {
			return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
		}

		try {
			const auto preflight =
				context_.checkActiveAccount(request.expectedGeneration, request.expectedProfileBinding);
			const auto preflightStatus = mapPreflightStatus(preflight);
			if (preflightStatus != YouTubeAccountRemoteRevokeStartStatus::Started) {
				return preflightStatus;
			}

			const auto providerSnapshot = provider_.snapshot();
			if (providerSnapshot.stage == YouTubeAccountProviderStage::Closed) {
				return YouTubeAccountRemoteRevokeStartStatus::Closed;
			}
			if (providerSnapshot.account.lease.has_value()) {
				return YouTubeAccountRemoteRevokeStartStatus::Busy;
			}

			const auto lockResult =
				operationLockProvider_.acquire(request.expectedProfileBinding, operationLock_);
			if (!lockResult.acquired()) {
				switch (lockResult.status) {
				case YouTubeAccountProfileOperationLockStatus::Busy:
					return YouTubeAccountRemoteRevokeStartStatus::Busy;
				case YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding:
					return YouTubeAccountRemoteRevokeStartStatus::InvalidProfileBinding;
				case YouTubeAccountProfileOperationLockStatus::Unavailable:
					return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
				case YouTubeAccountProfileOperationLockStatus::Acquired:
				case YouTubeAccountProfileOperationLockStatus::Recovered:
					break;
				}
				return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
			}

			// A lifecycle call may have been re-entered by a native/injected lock
			// boundary. Never continue into a credential read after that boundary
			// has invalidated or closed the operation.
			if (closed_ || lifecycleRequestPending()) {
				const bool released = releaseOperationLock();
				if (!released) {
					context_.invalidate();
				}
				return closed_ ? YouTubeAccountRemoteRevokeStartStatus::Closed
					       : YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
			}

			YouTubeAccountProfileContext::ActiveAccountStatus activeStatus;
			YouTubeAccountProfileContext::Snapshot profile;
			YouTubeAccountProviderSnapshot restoredProvider;
			{
				ScopedFlag boundary(preflightBoundaryInProgress_);
				activeStatus =
					context_.checkActiveAccount(request.expectedGeneration, request.expectedProfileBinding);
				profile = context_.snapshot();
				restoredProvider = provider_.snapshot();
			}
			if (closed_ || lifecycleRequestPending() ||
			    !operationLock_.acquiredFor(request.expectedProfileBinding)) {
				return releaseStartFailure(closed_ ? YouTubeAccountRemoteRevokeStartStatus::Closed
								   : YouTubeAccountRemoteRevokeStartStatus::ProfileChanged);
			}
			if (activeStatus != YouTubeAccountProfileContext::ActiveAccountStatus::Current) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::ProfileChanged);
			}

			if (profile.generation != request.expectedGeneration ||
			    profile.profileBinding != request.expectedProfileBinding) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::ProfileChanged);
			}
			if (profile.connectionMode != YouTubeConnectionMode::Account) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::NotAccountMode);
			}
			if (!profile.selection.has_value()) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::InvalidSelection);
			}
			if (validateYouTubeAccountSelection(*profile.selection) !=
			    YouTubeAccountSelectionValidationError::None) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::InvalidSelection);
			}

			if (restoredProvider.stage == YouTubeAccountProviderStage::Closed) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::Closed);
			}
			if (restoredProvider.account.lease.has_value()) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::Busy);
			}
			if (!providerCanRevokeSavedState(restoredProvider.stage)) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::NotRestored);
			}
			if (!providerMatchesSelection(restoredProvider, request.expectedProfileBinding,
						      *profile.selection)) {
				return releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus::InvalidSelection);
			}

			request_ = std::move(request);
			activeAttempt_ = YouTubeAccountRemoteRevokeAttempt{request_->expectedGeneration,
									   nextNonZero(attemptSequence_)};
			attemptSequence_ = activeAttempt_->attempt;
			epoch_ = nextNonZero(epoch_);
			transactionActive_ = true;
			completionHandler_ = std::move(completionHandler);
			selection_ = *profile.selection;
			remoteRevokeAccepted_ = false;
			remoteWasAlreadyRevoked_ = false;
			credentialErased_ = false;
			selectionCleared_ = false;
			completionProfile_.reset();
			state_ = YouTubeAccountRemoteRevokeState::ReadingCredential;

			CredentialReadResult credential;
			credentialReadInProgress_ = true;
			try {
				credential = refreshTokenStore_.read(
					{request_->expectedProfileBinding, selection_->channelId});
			} catch (...) {
				credential.result.error = CredentialError::OperatingSystemError;
			}
			credentialReadInProgress_ = false;

			if (lifecycleRequestPending()) {
				credential.secret.clear();
				return finishLifecycleAfterSynchronousBoundary();
			}
			if (!credential.result.succeeded() || credential.secret.empty()) {
				credential.secret.clear();
				finish(YouTubeAccountRemoteRevokeStatus::CredentialUnavailable);
				return YouTubeAccountRemoteRevokeStartStatus::Started;
			}

			state_ = YouTubeAccountRemoteRevokeState::Revoking;
			GoogleOAuthTokenRevokeRequest revokeRequest;
			revokeRequest.attempt = tokenAttempt(*activeAttempt_);
			revokeRequest.token = std::move(credential.secret);
			const auto epoch = epoch_;
			const auto attempt = *activeAttempt_;
			QPointer<YouTubeAccountRemoteRevokeCoordinator> guard(owner_);
			revokeStartInProgress_ = true;
			const auto status = revokePort_->startRevoke(
				std::move(revokeRequest),
				[guard, epoch, attempt](GoogleOAuthTokenCompletion completion) mutable {
					if (guard) {
						guard->impl_->handleRevoke(epoch, attempt, std::move(completion));
					}
				});
			revokeStartInProgress_ = false;

			if (lifecycleRequestPending()) {
				deferredRevokeCompletion_.reset();
				finishLifecycleAfterSynchronousBoundary();
				return YouTubeAccountRemoteRevokeStartStatus::Started;
			}
			if (deferredRevokeCompletion_.has_value()) {
				auto deferred = std::move(*deferredRevokeCompletion_);
				deferredRevokeCompletion_.reset();
				handleRevoke(epoch, attempt, std::move(deferred));
			}
			if (lifecycleRequestPending() && isCurrent(epoch, attempt)) {
				finishLifecycleAfterSynchronousBoundary();
				return YouTubeAccountRemoteRevokeStartStatus::Started;
			}
			if (status != GoogleOAuthTokenStartStatus::Started && isCurrent(epoch, attempt)) {
				finish(mapStartFailure(status));
			}
			return YouTubeAccountRemoteRevokeStartStatus::Started;
		} catch (...) {
			return failStartAfterLock();
		}
	}

	bool cancel(YouTubeAccountRemoteRevokeAttempt attempt) noexcept
	{
		if (!onOwnerThread() || closed_ || !activeAttempt_.has_value() || !(*activeAttempt_ == attempt)) {
			return false;
		}
		if (state_ != YouTubeAccountRemoteRevokeState::ReadingCredential &&
		    state_ != YouTubeAccountRemoteRevokeState::Revoking) {
			return false;
		}
		if (revokeStartInProgress_ || preflightBoundaryInProgress_ || credentialReadInProgress_ ||
		    lockReleaseInProgress_ ||
		    lifecycleMutationInProgress_) {
			cancelRequested_ = true;
			return false;
		}
		lifecycleMutationInProgress_ = true;
		advanceEpoch();
		finishLifecycle(YouTubeAccountRemoteRevokeStatus::Cancelled);
		lifecycleMutationInProgress_ = false;
		return true;
	}

	bool invalidateContext() noexcept
	{
		if (!onOwnerThread() || closed_) {
			return false;
		}
		if (revokeStartInProgress_ || preflightBoundaryInProgress_ || credentialReadInProgress_ ||
		    lockReleaseInProgress_ ||
		    lifecycleMutationInProgress_) {
			invalidationRequested_ = true;
			return false;
		}
		if (!transactionActive_ && !activeAttempt_.has_value() && !pendingCompletion_.has_value()) {
			return releaseOperationLock();
		}
		lifecycleMutationInProgress_ = true;
		advanceEpoch();
		finishLifecycle(YouTubeAccountRemoteRevokeStatus::ProfileChanged);
		lifecycleMutationInProgress_ = false;
		return !operationLock_.cleanupPending();
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		closed_ = true;
		if (revokeStartInProgress_ || preflightBoundaryInProgress_ || credentialReadInProgress_ ||
		    lockReleaseInProgress_ ||
		    lifecycleMutationInProgress_) {
			shutdownRequested_ = true;
			return false;
		}
		lifecycleMutationInProgress_ = true;
		advanceEpoch();
		if (activeAttempt_.has_value()) {
			finishLifecycle(YouTubeAccountRemoteRevokeStatus::Closed);
		} else {
			clearPendingCompletion();
			state_ = YouTubeAccountRemoteRevokeState::Closed;
			transactionActive_ = false;
			(void)releaseOperationLock();
		}
		if (!revokePortClosed_) {
			revokePortClosed_ = revokePort_->shutdown();
		}
		lifecycleMutationInProgress_ = false;
		return revokePortClosed_ && !operationLock_.cleanupPending();
	}

	YouTubeAccountRemoteRevokeState state() const noexcept { return state_; }

	std::optional<YouTubeAccountRemoteRevokeAttempt> activeAttempt() const noexcept { return activeAttempt_; }

	bool lockHeld() const noexcept { return operationLock_.cleanupPending(); }

	void shutdownAfterExternalOperationLockFailure() noexcept
	{
		provider_.shutdownAfterExternalOperationLockFailure();
	}

private:
	bool onOwnerThread() const noexcept
	{
		return owner_ != nullptr && owner_->thread() == QThread::currentThread();
	}

	bool lifecycleRequestPending() const noexcept
	{
		return cancelRequested_ || invalidationRequested_ || shutdownRequested_;
	}

	bool isCurrent(std::uint64_t epoch, YouTubeAccountRemoteRevokeAttempt attempt) const noexcept
	{
		return onOwnerThread() && epoch_ == epoch && activeAttempt_.has_value() && *activeAttempt_ == attempt &&
		       transactionActive_ && state_ == YouTubeAccountRemoteRevokeState::Revoking;
	}

	void advanceEpoch() noexcept
	{
		epoch_ = nextNonZero(epoch_);
		completionDeliveryQueued_ = false;
		deferredRevokeCompletion_.reset();
	}

	void cancelPort(YouTubeAccountRemoteRevokeAttempt attempt) noexcept
	{
		(void)revokePort_->cancel(tokenAttempt(attempt));
	}

	YouTubeAccountRemoteRevokeStartStatus finishLifecycleAfterSynchronousBoundary() noexcept
	{
		const bool closePort = shutdownRequested_ || closed_;
		if (shutdownRequested_) {
			finishLifecycle(YouTubeAccountRemoteRevokeStatus::Closed);
		} else if (invalidationRequested_) {
			finishLifecycle(YouTubeAccountRemoteRevokeStatus::ProfileChanged);
		} else {
			finishLifecycle(YouTubeAccountRemoteRevokeStatus::Cancelled);
		}
		if (closePort && !revokePortClosed_) {
			revokePortClosed_ = revokePort_->shutdown();
		}
		return YouTubeAccountRemoteRevokeStartStatus::Started;
	}

	void handleRevoke(std::uint64_t epoch, YouTubeAccountRemoteRevokeAttempt attempt,
			  GoogleOAuthTokenCompletion completion) noexcept
	{
		if (revokeStartInProgress_) {
			try {
				deferredRevokeCompletion_.emplace(std::move(completion));
			} catch (...) {
				// A completion that cannot be retained is treated as a failed
				// operation. The active owner will clean it up after start returns.
				cancelRequested_ = true;
			}
			return;
		}
		if (!isCurrent(epoch, attempt) || !matches(completion.attempt, attempt)) {
			return;
		}
		if (completion.operation != GoogleOAuthTokenOperation::RevokeToken) {
			finish(YouTubeAccountRemoteRevokeStatus::InvalidResponse);
			return;
		}
		if (lifecycleRequestPending()) {
			finishLifecycleAfterSynchronousBoundary();
			return;
		}
		if (!completion.succeeded()) {
			if (completion.providerError == GoogleOAuthTokenProviderError::InvalidToken) {
				remoteRevokeAccepted_ = true;
				remoteWasAlreadyRevoked_ = true;
				beginProviderDisconnect(epoch, attempt);
				return;
			}
			finish(mapCompletionFailure(completion));
			return;
		}
		remoteRevokeAccepted_ = true;
		remoteWasAlreadyRevoked_ = false;
		beginProviderDisconnect(epoch, attempt);
	}

	void beginProviderDisconnect(std::uint64_t epoch, YouTubeAccountRemoteRevokeAttempt attempt) noexcept
	{
		if (!isCurrent(epoch, attempt) || !request_.has_value() || !selection_.has_value()) {
			return;
		}
		if (context_.checkActiveAccount(request_->expectedGeneration, request_->expectedProfileBinding) !=
		    YouTubeAccountProfileContext::ActiveAccountStatus::Current) {
			finish(YouTubeAccountRemoteRevokeStatus::ProfileChanged);
			return;
		}

		state_ = YouTubeAccountRemoteRevokeState::Disconnecting;
		// The provider call synchronously erases the credential and commits the
		// profile selection while borrowing this coordinator's lock.  Defer any
		// cancel/profile-change/shutdown request made by a test or profile boundary
		// reached from that committer until the borrowed transaction has returned.
		lifecycleMutationInProgress_ = true;
		std::optional<YouTubeAccountProfileContext::CommitResult> commitResult;
		const auto expectedGeneration = request_->expectedGeneration;
		const auto expectedBinding = request_->expectedProfileBinding;
		const auto committer = [this, expectedGeneration, expectedBinding,
					&commitResult](const std::optional<YouTubeAccountSelection> &) noexcept {
			try {
				commitResult =
					context_.commitSelection(expectedGeneration, expectedBinding, std::nullopt);
				return commitResult->status == YouTubeAccountProfileContext::CommitStatus::Committed;
			} catch (...) {
				return false;
			}
		};

		YouTubeAccountProviderDisconnectStatus disconnectStatus;
		try {
			disconnectStatus = provider_.disconnectSavedStateUnderHeldOperationLock(
				request_->expectedProfileBinding, *selection_, operationLock_, committer);
		} catch (...) {
			disconnectStatus = YouTubeAccountProviderDisconnectStatus::OperationFailed;
		}
		lifecycleMutationInProgress_ = false;
		if (disconnectStatus == YouTubeAccountProviderDisconnectStatus::Disconnected ||
		    disconnectStatus == YouTubeAccountProviderDisconnectStatus::AlreadyDisconnected) {
			credentialErased_ = true;
			selectionCleared_ = true;
			try {
				completionProfile_ = YouTubeAccountProfileContext::LoadResult{};
				completionProfile_->snapshot = context_.snapshot();
				completionProfile_->status = YouTubeAccountProfileContext::LoadStatus::SetupRequired;
				completionProfile_->snapshot.selection.reset();
			} catch (...) {
				completionProfile_.reset();
			}
		} else if (disconnectStatus == YouTubeAccountProviderDisconnectStatus::ProfileSaveFailed) {
			credentialErased_ = true;
		}
		if (lifecycleRequestPending()) {
			advanceEpoch();
			finishLifecycleAfterSynchronousBoundary();
			return;
		}

		if (disconnectStatus == YouTubeAccountProviderDisconnectStatus::Disconnected ||
		    disconnectStatus == YouTubeAccountProviderDisconnectStatus::AlreadyDisconnected) {
			finish(remoteWasAlreadyRevoked_ ? YouTubeAccountRemoteRevokeStatus::AlreadyRevoked
							: YouTubeAccountRemoteRevokeStatus::Revoked);
			return;
		}

		if (disconnectStatus == YouTubeAccountProviderDisconnectStatus::CredentialUnavailable) {
			finish(YouTubeAccountRemoteRevokeStatus::CredentialEraseFailed);
			return;
		}
		if (disconnectStatus == YouTubeAccountProviderDisconnectStatus::ProfileSaveFailed) {
			finish(YouTubeAccountRemoteRevokeStatus::ProfileSaveFailed);
			return;
		}
		if (disconnectStatus == YouTubeAccountProviderDisconnectStatus::InvalidSelection ||
		    disconnectStatus == YouTubeAccountProviderDisconnectStatus::InvalidProfileBinding) {
			finish(YouTubeAccountRemoteRevokeStatus::ProfileChanged);
			return;
		}
		finish(YouTubeAccountRemoteRevokeStatus::OperationFailed);
	}

	YouTubeAccountRemoteRevokeStartStatus failStartAfterLock() noexcept
	{
		advanceEpoch();
		if (activeAttempt_.has_value()) {
			cancelPort(*activeAttempt_);
		}
		revokeStartInProgress_ = false;
		credentialReadInProgress_ = false;
		if (operationLock_.cleanupPending()) {
			(void)releaseOperationLock();
		}
		clearOperation();
		cancelRequested_ = false;
		invalidationRequested_ = false;
		shutdownRequested_ = false;
		if (closed_) {
			if (!revokePortClosed_) {
				revokePortClosed_ = revokePort_->shutdown();
			}
			state_ = YouTubeAccountRemoteRevokeState::Closed;
		} else {
			state_ = YouTubeAccountRemoteRevokeState::Failed;
		}
		return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
	}

	YouTubeAccountRemoteRevokeStartStatus releaseStartFailure(YouTubeAccountRemoteRevokeStartStatus status) noexcept
	{
		if (!releaseOperationLock()) {
			context_.invalidate();
			cancelRequested_ = false;
			invalidationRequested_ = false;
			shutdownRequested_ = false;
			return YouTubeAccountRemoteRevokeStartStatus::OperationFailed;
		}
		if (shutdownRequested_ || closed_) {
			shutdownRequested_ = false;
			invalidationRequested_ = false;
			cancelRequested_ = false;
			if (!revokePortClosed_) {
				revokePortClosed_ = revokePort_->shutdown();
			}
			return YouTubeAccountRemoteRevokeStartStatus::Closed;
		}
		if (invalidationRequested_) {
			invalidationRequested_ = false;
			cancelRequested_ = false;
			return YouTubeAccountRemoteRevokeStartStatus::ProfileChanged;
		}
		cancelRequested_ = false;
		return status;
	}

	void finish(YouTubeAccountRemoteRevokeStatus status) noexcept
	{
		if (!activeAttempt_.has_value()) {
			clearOperation();
			state_ = YouTubeAccountRemoteRevokeState::Completed;
			(void)releaseOperationLock();
			return;
		}
		// Completion handlers may immediately start another operation.  The
		// native profile mutex and the provider's borrowed-lock handshake must be
		// complete before the queued callback becomes visible.
		const bool lockReleased = releaseOperationLock();
		if (!lockReleased) {
			context_.invalidate();
			completionProfile_.reset();
			status = YouTubeAccountRemoteRevokeStatus::OperationFailed;
		}
		const bool closePort = shutdownRequested_ || closed_;
		if (lockReleased && shutdownRequested_) {
			status = YouTubeAccountRemoteRevokeStatus::Closed;
		} else if (lockReleased && invalidationRequested_) {
			status = YouTubeAccountRemoteRevokeStatus::ProfileChanged;
		} else if (lockReleased && cancelRequested_) {
			status = YouTubeAccountRemoteRevokeStatus::Cancelled;
		}
		cancelRequested_ = false;
		invalidationRequested_ = false;
		shutdownRequested_ = false;
		if (closePort && !revokePortClosed_) {
			revokePortClosed_ = revokePort_->shutdown();
		}
		try {
			YouTubeAccountRemoteRevokeCompletion completion;
			completion.attempt = *activeAttempt_;
			completion.status = status;
			completion.remoteRevokeAccepted = remoteRevokeAccepted_;
			completion.credentialErased = credentialErased_;
			completion.selectionCleared = selectionCleared_;
			completion.lockCleanupPending = !lockReleased;
			if (completionProfile_.has_value()) {
				completion.profile = *completionProfile_;
			}
			pendingCompletion_.emplace(std::move(completion));
			state_ = status == YouTubeAccountRemoteRevokeStatus::Cancelled
					 ? YouTubeAccountRemoteRevokeState::Cancelled
					 : (status == YouTubeAccountRemoteRevokeStatus::Closed
						    ? YouTubeAccountRemoteRevokeState::Closed
						    : YouTubeAccountRemoteRevokeState::DeliveringCompletion);
			queueCompletion(epoch_);
		} catch (...) {
			clearOperation();
			state_ = YouTubeAccountRemoteRevokeState::Failed;
		}
	}

	void finishLifecycle(YouTubeAccountRemoteRevokeStatus status) noexcept
	{
		if (activeAttempt_.has_value()) {
			cancelPort(*activeAttempt_);
		}
		finish(status);
	}

	void queueCompletion(std::uint64_t epoch)
	{
		if (completionDeliveryQueued_ || !pendingCompletion_.has_value()) {
			return;
		}
		completionDeliveryQueued_ = true;
		const bool queued = QMetaObject::invokeMethod(
			owner_,
			[this, epoch]() {
				if (epoch != epoch_) {
					// An old queued event may remain after a lifecycle epoch bump.
					// Do not clear or deliver state belonging to a newer attempt.
					return;
				}
				completionDeliveryQueued_ = false;
				deliverCompletion();
			},
			Qt::QueuedConnection);
		if (!queued) {
			completionDeliveryQueued_ = false;
			clearOperation();
			state_ = YouTubeAccountRemoteRevokeState::Failed;
		}
	}

	void deliverCompletion() noexcept
	{
		if (!pendingCompletion_.has_value()) {
			return;
		}
		YouTubeAccountRemoteRevokeCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		CompletionHandler handler = std::move(completionHandler_);
		completionHandler_ = {};
		activeAttempt_.reset();
		request_.reset();
		selection_.reset();
		completionProfile_.reset();
		transactionActive_ = false;
		remoteRevokeAccepted_ = false;
		remoteWasAlreadyRevoked_ = false;
		credentialErased_ = false;
		selectionCleared_ = false;
		if (statusIsFailure(completion.status)) {
			state_ = completion.status == YouTubeAccountRemoteRevokeStatus::Cancelled
					 ? YouTubeAccountRemoteRevokeState::Cancelled
					 : (completion.status == YouTubeAccountRemoteRevokeStatus::Closed
						    ? YouTubeAccountRemoteRevokeState::Closed
						    : YouTubeAccountRemoteRevokeState::Failed);
		} else {
			state_ = YouTubeAccountRemoteRevokeState::Completed;
		}
		if (handler) {
			try {
				handler(std::move(completion));
			} catch (...) {
			}
		}
	}

	static bool statusIsFailure(YouTubeAccountRemoteRevokeStatus status) noexcept
	{
		return status != YouTubeAccountRemoteRevokeStatus::Revoked &&
		       status != YouTubeAccountRemoteRevokeStatus::AlreadyRevoked;
	}

	void clearPendingCompletion() noexcept
	{
		pendingCompletion_.reset();
		completionHandler_ = {};
		completionDeliveryQueued_ = false;
	}

	void clearOperation() noexcept
	{
		request_.reset();
		selection_.reset();
		completionProfile_.reset();
		pendingCompletion_.reset();
		completionHandler_ = {};
		activeAttempt_.reset();
		transactionActive_ = false;
		completionDeliveryQueued_ = false;
		remoteRevokeAccepted_ = false;
		remoteWasAlreadyRevoked_ = false;
		credentialErased_ = false;
		selectionCleared_ = false;
	}

	bool releaseOperationLock() noexcept
	{
		if (!operationLock_.cleanupPending()) {
			return true;
		}
		lockReleaseInProgress_ = true;
		const bool released = operationLock_.release();
		if (released) {
			provider_.externalOperationLockReleased();
		} else {
			provider_.markExternalOperationReleaseFailed();
		}
		lockReleaseInProgress_ = false;
		return released;
	}

	YouTubeAccountRemoteRevokeCoordinator *owner_ = nullptr;
	YouTubeAccountProfileContext &context_;
	YouTubeAccountProvider &provider_;
	YouTubeAccountRefreshTokenStore &refreshTokenStore_;
	YouTubeAccountProfileOperationLockProvider &operationLockProvider_;
	std::unique_ptr<YouTubeAccountRemoteRevokePort> revokePort_;
	YouTubeAccountProfileOperationLock operationLock_;
	YouTubeAccountRemoteRevokeState state_ = YouTubeAccountRemoteRevokeState::Idle;
	std::optional<YouTubeAccountRemoteRevokeRequest> request_;
	std::optional<YouTubeAccountRemoteRevokeAttempt> activeAttempt_;
	std::optional<YouTubeAccountSelection> selection_;
	std::optional<YouTubeAccountProfileContext::LoadResult> completionProfile_;
	CompletionHandler completionHandler_;
	std::optional<YouTubeAccountRemoteRevokeCompletion> pendingCompletion_;
	std::optional<GoogleOAuthTokenCompletion> deferredRevokeCompletion_;
	std::uint64_t attemptSequence_ = 0;
	std::uint64_t epoch_ = 0;
	bool transactionActive_ = false;
	bool revokeStartInProgress_ = false;
	bool preflightBoundaryInProgress_ = false;
	bool credentialReadInProgress_ = false;
	bool lockReleaseInProgress_ = false;
	bool completionDeliveryQueued_ = false;
	bool lifecycleMutationInProgress_ = false;
	bool cancelRequested_ = false;
	bool invalidationRequested_ = false;
	bool shutdownRequested_ = false;
	bool closed_ = false;
	bool revokePortClosed_ = false;
	bool remoteRevokeAccepted_ = false;
	bool remoteWasAlreadyRevoked_ = false;
	bool credentialErased_ = false;
	bool selectionCleared_ = false;
};

YouTubeAccountRemoteRevokeCoordinator::YouTubeAccountRemoteRevokeCoordinator(
	YouTubeAccountProfileContext &context, YouTubeAccountProvider &provider,
	YouTubeAccountRefreshTokenStore &refreshTokenStore,
	YouTubeAccountProfileOperationLockProvider &operationLockProvider, QObject *parent)
	: YouTubeAccountRemoteRevokeCoordinator(context, provider, refreshTokenStore, operationLockProvider,
						std::make_unique<RevokeAdapter>(), parent)
{
}

YouTubeAccountRemoteRevokeCoordinator::YouTubeAccountRemoteRevokeCoordinator(
	YouTubeAccountProfileContext &context, YouTubeAccountProvider &provider,
	YouTubeAccountRefreshTokenStore &refreshTokenStore,
	YouTubeAccountProfileOperationLockProvider &operationLockProvider,
	std::unique_ptr<YouTubeAccountRemoteRevokePort> revokePort, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, context, provider, refreshTokenStore, operationLockProvider,
				       std::move(revokePort)))
{
}

YouTubeAccountRemoteRevokeCoordinator::~YouTubeAccountRemoteRevokeCoordinator()
{
	assert(QThread::currentThread() == thread());
	if (!shutdown() && impl_->lockHeld()) {
		impl_->shutdownAfterExternalOperationLockFailure();
	}
}

YouTubeAccountRemoteRevokeStartStatus
YouTubeAccountRemoteRevokeCoordinator::start(YouTubeAccountRemoteRevokeRequest request,
					     CompletionHandler completionHandler) noexcept
{
	return impl_->start(std::move(request), std::move(completionHandler));
}

bool YouTubeAccountRemoteRevokeCoordinator::cancel(YouTubeAccountRemoteRevokeAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool YouTubeAccountRemoteRevokeCoordinator::invalidateContext() noexcept
{
	return impl_->invalidateContext();
}

bool YouTubeAccountRemoteRevokeCoordinator::shutdown() noexcept
{
	return impl_->shutdown();
}

YouTubeAccountRemoteRevokeState YouTubeAccountRemoteRevokeCoordinator::state() const noexcept
{
	return impl_->state();
}

std::optional<YouTubeAccountRemoteRevokeAttempt> YouTubeAccountRemoteRevokeCoordinator::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

bool YouTubeAccountRemoteRevokeCoordinator::lockHeld() const noexcept
{
	return impl_->lockHeld();
}

} // namespace easy_multistream

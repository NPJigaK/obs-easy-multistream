// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-destination-preparer.hpp"

#include "google-oauth-protocol.hpp"
#include "youtube-destination.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace easy_multistream {
namespace {

std::uint64_t nextNonZero(std::uint64_t value) noexcept
{
	return value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

GoogleOAuthTokenAttempt tokenAttempt(YouTubeDestinationPrepareAttempt attempt) noexcept
{
	return {attempt.generation, attempt.attempt};
}

YouTubeApiAttempt apiAttempt(YouTubeDestinationPrepareAttempt attempt) noexcept
{
	return {attempt.generation, attempt.attempt};
}

bool matches(GoogleOAuthTokenAttempt candidate, YouTubeDestinationPrepareAttempt expected) noexcept
{
	return candidate.generation == expected.generation && candidate.attempt == expected.attempt;
}

bool matches(YouTubeApiAttempt candidate, YouTubeDestinationPrepareAttempt expected) noexcept
{
	return candidate.generation == expected.generation && candidate.attempt == expected.attempt;
}

class RefreshAdapter final : public YouTubeDestinationRefreshPort {
public:
	GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
						 CompletionHandler completionHandler) noexcept override
	{
		return transport_.startRefresh(std::move(request), std::move(completionHandler));
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override { return transport_.cancel(attempt); }
	bool shutdown() noexcept override { return transport_.shutdown(); }

private:
	GoogleOAuthTokenTransport transport_;
};

class ResolverAdapter final : public YouTubeDestinationResolverPort {
public:
	YouTubeStreamResolverStartStatus startResolveStream(YouTubeResolveStreamRequest request,
							    CompletionHandler completionHandler) noexcept override
	{
		return resolver_.startResolveStream(std::move(request), std::move(completionHandler));
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept override { return resolver_.cancel(attempt); }
	bool shutdown() noexcept override { return resolver_.shutdown(); }

private:
	YouTubeApiStreamResolver resolver_;
};

YouTubeDestinationPrepareStatus tokenFailure(const GoogleOAuthTokenCompletion &completion) noexcept
{
	if (completion.providerError == GoogleOAuthTokenProviderError::InvalidGrant ||
	    completion.providerError == GoogleOAuthTokenProviderError::InvalidToken ||
	    completion.providerError == GoogleOAuthTokenProviderError::AccessDenied) {
		return YouTubeDestinationPrepareStatus::ReauthorizationRequired;
	}
	if (completion.providerError == GoogleOAuthTokenProviderError::TemporarilyUnavailable ||
	    completion.providerError == GoogleOAuthTokenProviderError::RateLimited) {
		return YouTubeDestinationPrepareStatus::ServiceUnavailable;
	}

	switch (completion.status) {
	case GoogleOAuthTokenCompletionStatus::NetworkFailure:
	case GoogleOAuthTokenCompletionStatus::TlsFailure:
	case GoogleOAuthTokenCompletionStatus::TimedOut:
		return YouTubeDestinationPrepareStatus::NetworkFailure;
	case GoogleOAuthTokenCompletionStatus::InvalidResponse:
	case GoogleOAuthTokenCompletionStatus::RedirectRejected:
	case GoogleOAuthTokenCompletionStatus::ResponseTooLarge:
		return YouTubeDestinationPrepareStatus::InvalidResponse;
	case GoogleOAuthTokenCompletionStatus::ProviderRejected:
	case GoogleOAuthTokenCompletionStatus::HttpFailure:
		return YouTubeDestinationPrepareStatus::ServiceUnavailable;
	case GoogleOAuthTokenCompletionStatus::Success:
		return YouTubeDestinationPrepareStatus::InvalidResponse;
	}
	return YouTubeDestinationPrepareStatus::InvalidResponse;
}

YouTubeDestinationPrepareStatus resolverFailure(const YouTubeStreamResolverCompletion &completion) noexcept
{
	if (completion.providerError == YouTubeApiProviderError::InvalidToken ||
	    completion.providerError == YouTubeApiProviderError::InsufficientPermissions ||
	    completion.providerError == YouTubeApiProviderError::PermissionDenied) {
		return YouTubeDestinationPrepareStatus::ReauthorizationRequired;
	}
	if (completion.providerError == YouTubeApiProviderError::QuotaExceeded ||
	    completion.providerError == YouTubeApiProviderError::RateLimited ||
	    completion.providerError == YouTubeApiProviderError::TemporarilyUnavailable) {
		return YouTubeDestinationPrepareStatus::ServiceUnavailable;
	}
	if (completion.providerError == YouTubeApiProviderError::LiveStreamingNotEnabled ||
	    completion.providerError == YouTubeApiProviderError::NotFound) {
		return YouTubeDestinationPrepareStatus::DestinationUnavailable;
	}

	switch (completion.status) {
	case YouTubeStreamResolverCompletionStatus::StreamNotFound:
	case YouTubeStreamResolverCompletionStatus::StreamMismatch:
	case YouTubeStreamResolverCompletionStatus::StreamNotReady:
		return YouTubeDestinationPrepareStatus::DestinationUnavailable;
	case YouTubeStreamResolverCompletionStatus::StreamAlreadyActive:
		return YouTubeDestinationPrepareStatus::DestinationAlreadyActive;
	case YouTubeStreamResolverCompletionStatus::NetworkFailure:
	case YouTubeStreamResolverCompletionStatus::TlsFailure:
	case YouTubeStreamResolverCompletionStatus::TimedOut:
		return YouTubeDestinationPrepareStatus::NetworkFailure;
	case YouTubeStreamResolverCompletionStatus::InvalidResponse:
	case YouTubeStreamResolverCompletionStatus::RedirectRejected:
	case YouTubeStreamResolverCompletionStatus::ResponseTooLarge:
		return YouTubeDestinationPrepareStatus::InvalidResponse;
	case YouTubeStreamResolverCompletionStatus::ProviderRejected:
	case YouTubeStreamResolverCompletionStatus::HttpFailure:
		return YouTubeDestinationPrepareStatus::ServiceUnavailable;
	case YouTubeStreamResolverCompletionStatus::Success:
		return YouTubeDestinationPrepareStatus::InvalidResponse;
	}
	return YouTubeDestinationPrepareStatus::InvalidResponse;
}

YouTubeDestinationPrepareStatus tokenStartFailure(GoogleOAuthTokenStartStatus status) noexcept
{
	if (status == GoogleOAuthTokenStartStatus::InvalidRefreshToken) {
		return YouTubeDestinationPrepareStatus::ReauthorizationRequired;
	}
	if (status == GoogleOAuthTokenStartStatus::InvalidAttempt ||
	    status == GoogleOAuthTokenStartStatus::InvalidClientId ||
	    status == GoogleOAuthTokenStartStatus::InvalidCompletionHandler) {
		return YouTubeDestinationPrepareStatus::InvalidResponse;
	}
	return YouTubeDestinationPrepareStatus::ServiceUnavailable;
}

YouTubeDestinationPrepareStatus resolverStartFailure(YouTubeStreamResolverStartStatus status) noexcept
{
	if (status == YouTubeStreamResolverStartStatus::InvalidAccessToken) {
		return YouTubeDestinationPrepareStatus::ReauthorizationRequired;
	}
	if (status == YouTubeStreamResolverStartStatus::InvalidAttempt ||
	    status == YouTubeStreamResolverStartStatus::InvalidChannelId ||
	    status == YouTubeStreamResolverStartStatus::InvalidStreamId ||
	    status == YouTubeStreamResolverStartStatus::InvalidCompletionHandler) {
		return YouTubeDestinationPrepareStatus::InvalidResponse;
	}
	return YouTubeDestinationPrepareStatus::ServiceUnavailable;
}

bool validIngestion(const YouTubeResolvedIngestion &ingestion) noexcept
{
	return validateYouTubeServerUrl(ingestion.serverUrl) == YouTubeServerUrlValidationError::None &&
	       validateYouTubeStreamKey(ingestion.streamKey.view()) == StreamKeyValidationError::None;
}

} // namespace

class YouTubeAccountDestinationPreparer::Impl final {
public:
	Impl(YouTubeAccountDestinationPreparer *owner, QString clientId,
	     std::unique_ptr<YouTubeDestinationRefreshPort> refreshPort,
	     std::unique_ptr<YouTubeDestinationResolverPort> resolverPort,
	     YouTubeAccountRefreshTokenStore &refreshTokenStore,
	     YouTubeAccountProfileOperationLockProvider &operationLockProvider)
		: owner_(owner),
		  clientId_(std::move(clientId)),
		  refreshPort_(std::move(refreshPort)),
		  resolverPort_(std::move(resolverPort)),
		  refreshTokenStore_(refreshTokenStore),
		  operationLockProvider_(operationLockProvider)
	{
		if (refreshPort_ == nullptr || resolverPort_ == nullptr) {
			throw std::invalid_argument("YouTube destination ports are required");
		}
	}

	YouTubeDestinationPrepareStartStatus start(YouTubeDestinationPrepareRequest request,
						   CompletionHandler completionHandler) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeDestinationPrepareStartStatus::WrongThread;
		}
		if (state_ == YouTubeDestinationPreparerState::Closed) {
			return YouTubeDestinationPrepareStartStatus::Closed;
		}
		if (lifecycleMutationInProgress_) {
			return YouTubeDestinationPrepareStartStatus::Busy;
		}
		if (activeAttempt_.has_value() || pendingCompletion_.has_value() || completionDeliveryQueued_) {
			return YouTubeDestinationPrepareStartStatus::Busy;
		}
		if (!request.attempt.isValid()) {
			return YouTubeDestinationPrepareStartStatus::InvalidAttempt;
		}
		if (!isValidGoogleOAuthClientId(clientId_)) {
			return YouTubeDestinationPrepareStartStatus::InvalidClientId;
		}
		if (!isValidYouTubeAccountProfileBinding(request.profileBinding)) {
			return YouTubeDestinationPrepareStartStatus::InvalidProfileBinding;
		}
		if (validateYouTubeAccountSelection(request.selection) !=
		    YouTubeAccountSelectionValidationError::None) {
			return YouTubeDestinationPrepareStartStatus::InvalidSelection;
		}
		if (!completionHandler) {
			return YouTubeDestinationPrepareStartStatus::InvalidCompletionHandler;
		}

		const auto lockResult = operationLockProvider_.acquire(request.profileBinding, operationLock_);
		if (!lockResult.acquired()) {
			switch (lockResult.status) {
			case YouTubeAccountProfileOperationLockStatus::Busy:
				return YouTubeDestinationPrepareStartStatus::Busy;
			case YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding:
				return YouTubeDestinationPrepareStartStatus::InvalidProfileBinding;
			case YouTubeAccountProfileOperationLockStatus::Unavailable:
				return YouTubeDestinationPrepareStartStatus::OperationFailed;
			case YouTubeAccountProfileOperationLockStatus::Acquired:
			case YouTubeAccountProfileOperationLockStatus::Recovered:
				break;
			}
			return YouTubeDestinationPrepareStartStatus::OperationFailed;
		}

		try {
			advanceEpoch();
			activeAttempt_ = request.attempt;
			credentialScope_.emplace(YouTubeAccountCredentialScope{std::move(request.profileBinding),
									       request.selection.channelId});
			selection_.emplace(std::move(request.selection));
			completionHandler_ = std::move(completionHandler);
			state_ = YouTubeDestinationPreparerState::ReadingCredential;

			CredentialReadResult credential;
			try {
				if (!credentialScope_.has_value()) {
					credential.result.error = CredentialError::OperatingSystemError;
				} else {
					credential = refreshTokenStore_.read(*credentialScope_);
				}
			} catch (...) {
				credential.result.error = CredentialError::OperatingSystemError;
			}

			if (!isCurrent(operationEpoch_, request.attempt,
				       YouTubeDestinationPreparerState::ReadingCredential)) {
				credential.secret.clear();
				return state_ == YouTubeDestinationPreparerState::Closed
					       ? YouTubeDestinationPrepareStartStatus::Closed
					       : YouTubeDestinationPrepareStartStatus::OperationFailed;
			}
			if (credential.result.error == CredentialError::NotFound ||
			    credential.result.error == CredentialError::CorruptData ||
			    credential.result.error == CredentialError::ScopeMismatch ||
			    (credential.result.succeeded() && credential.secret.empty())) {
				credential.secret.clear();
				finish(YouTubeDestinationPrepareStatus::ReauthorizationRequired);
				return YouTubeDestinationPrepareStartStatus::Started;
			}
			if (!credential.result.succeeded()) {
				credential.secret.clear();
				finish(YouTubeDestinationPrepareStatus::CredentialUnavailable);
				return YouTubeDestinationPrepareStartStatus::Started;
			}

			state_ = YouTubeDestinationPreparerState::RefreshingAccessToken;
			GoogleOAuthTokenRefreshRequest refreshRequest;
			refreshRequest.attempt = tokenAttempt(request.attempt);
			refreshRequest.clientId = clientId_;
			refreshRequest.refreshToken = std::move(credential.secret);

			const auto epoch = operationEpoch_;
			QPointer<YouTubeAccountDestinationPreparer> guard(owner_);
			const auto status = refreshPort_->startRefresh(
				std::move(refreshRequest), [guard, epoch, attempt = request.attempt](
								   GoogleOAuthTokenCompletion completion) mutable {
					if (guard) {
						guard->impl_->handleRefresh(epoch, attempt, std::move(completion));
					}
				});
			if (status != GoogleOAuthTokenStartStatus::Started &&
			    isCurrent(epoch, request.attempt, YouTubeDestinationPreparerState::RefreshingAccessToken)) {
				finish(tokenStartFailure(status));
			}
			// A credential store or transport adapter is allowed to synchronously
			// re-enter profile invalidation/shutdown. Those operations advance the
			// epoch and clear the attempt; do not report the obsolete operation as
			// Started. A normal synchronous provider completion does not advance the
			// epoch and therefore remains a valid Started operation.
			if (operationEpoch_ != epoch) {
				return state_ == YouTubeDestinationPreparerState::Closed
					       ? YouTubeDestinationPrepareStartStatus::Closed
					       : YouTubeDestinationPrepareStartStatus::OperationFailed;
			}
			return YouTubeDestinationPrepareStartStatus::Started;
		} catch (...) {
			clearOperation();
			state_ = YouTubeDestinationPreparerState::Idle;
			releaseOperationLock();
			return YouTubeDestinationPrepareStartStatus::OperationFailed;
		}
	}

	bool cancel(YouTubeDestinationPrepareAttempt attempt) noexcept
	{
		if (!onOwnerThread() || state_ == YouTubeDestinationPreparerState::Closed ||
		    lifecycleMutationInProgress_ || !activeAttempt_.has_value() || !(*activeAttempt_ == attempt)) {
			return false;
		}

		lifecycleMutationInProgress_ = true;
		advanceEpoch();
		cancelPorts(attempt);
		pendingCompletion_.reset();
		selection_.reset();
		finish(YouTubeDestinationPrepareStatus::Cancelled);
		lifecycleMutationInProgress_ = false;
		return true;
	}

	bool invalidateContext() noexcept
	{
		if (!onOwnerThread() || state_ == YouTubeDestinationPreparerState::Closed ||
		    lifecycleMutationInProgress_) {
			return false;
		}
		lifecycleMutationInProgress_ = true;
		advanceEpoch();
		if (activeAttempt_.has_value()) {
			cancelPorts(*activeAttempt_);
		}
		clearOperation();
		state_ = YouTubeDestinationPreparerState::Idle;
		const bool lockReleased = releaseOperationLock();
		lifecycleMutationInProgress_ = false;
		return lockReleased;
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		if (lifecycleMutationInProgress_) {
			return false;
		}

		lifecycleMutationInProgress_ = true;
		if (state_ != YouTubeDestinationPreparerState::Closed) {
			state_ = YouTubeDestinationPreparerState::Closed;
			advanceEpoch();
			if (activeAttempt_.has_value()) {
				cancelPorts(*activeAttempt_);
			}
			clearOperation();
		}
		if (!refreshPortClosed_) {
			refreshPortClosed_ = refreshPort_->shutdown();
		}
		if (!resolverPortClosed_) {
			resolverPortClosed_ = resolverPort_->shutdown();
		}
		const bool lockReleased = releaseOperationLock();
		lifecycleMutationInProgress_ = false;
		return refreshPortClosed_ && resolverPortClosed_ && lockReleased;
	}

	YouTubeDestinationPreparerState state() const noexcept { return state_; }

	std::optional<YouTubeDestinationPrepareAttempt> activeAttempt() const noexcept { return activeAttempt_; }

	bool lockHeld() const noexcept { return operationLock_.cleanupPending(); }

private:
	bool onOwnerThread() const noexcept
	{
		return owner_ != nullptr && QThread::currentThread() == owner_->thread();
	}

	void advanceEpoch() noexcept
	{
		operationEpoch_ = nextNonZero(operationEpoch_);
		completionDeliveryQueued_ = false;
	}

	bool isCurrent(std::uint64_t epoch, YouTubeDestinationPrepareAttempt attempt,
		       YouTubeDestinationPreparerState state) const noexcept
	{
		return onOwnerThread() && operationEpoch_ == epoch && state_ == state && activeAttempt_.has_value() &&
		       *activeAttempt_ == attempt;
	}

	void clearOperation() noexcept
	{
		credentialScope_.reset();
		selection_.reset();
		pendingCompletion_.reset();
		completionHandler_ = {};
		activeAttempt_.reset();
		completionDeliveryQueued_ = false;
	}

	bool releaseOperationLock() noexcept { return operationLock_.release(); }

	void cancelPorts(YouTubeDestinationPrepareAttempt attempt) noexcept
	{
		refreshPort_->cancel(tokenAttempt(attempt));
		resolverPort_->cancel(apiAttempt(attempt));
	}

	void handleRefresh(std::uint64_t epoch, YouTubeDestinationPrepareAttempt attempt,
			   GoogleOAuthTokenCompletion completion) noexcept
	{
		if (!isCurrent(epoch, attempt, YouTubeDestinationPreparerState::RefreshingAccessToken) ||
		    !matches(completion.attempt, attempt)) {
			return;
		}
		if (completion.operation != GoogleOAuthTokenOperation::RefreshAccessToken) {
			finish(YouTubeDestinationPrepareStatus::InvalidResponse);
			return;
		}
		if (!completion.succeeded()) {
			finish(tokenFailure(completion));
			return;
		}

		if (completion.tokens.hasRefreshToken()) {
			state_ = YouTubeDestinationPreparerState::PersistingRefreshToken;
			CredentialResult writeResult;
			try {
				if (!credentialScope_.has_value()) {
					writeResult.error = CredentialError::OperatingSystemError;
				} else {
					writeResult = refreshTokenStore_.write(*credentialScope_,
									       completion.tokens.refreshToken.view());
				}
			} catch (...) {
				writeResult.error = CredentialError::OperatingSystemError;
			}
			completion.tokens.refreshToken.clear();
			if (!isCurrent(epoch, attempt, YouTubeDestinationPreparerState::PersistingRefreshToken)) {
				return;
			}
			if (!writeResult.succeeded()) {
				finish(YouTubeDestinationPrepareStatus::CredentialUnavailable);
				return;
			}
		}

		if (!isCurrent(epoch, attempt, state_)) {
			return;
		}
		startResolver(epoch, attempt, std::move(completion.tokens.accessToken));
	}

	void startResolver(std::uint64_t epoch, YouTubeDestinationPrepareAttempt attempt,
			   SecureBuffer accessToken) noexcept
	{
		if (!selection_.has_value() || accessToken.empty()) {
			finish(YouTubeDestinationPrepareStatus::InvalidResponse);
			return;
		}

		try {
			state_ = YouTubeDestinationPreparerState::ResolvingStream;
			YouTubeResolveStreamRequest request;
			request.attempt = apiAttempt(attempt);
			request.accessToken = std::move(accessToken);
			request.channelId = selection_->channelId;
			request.streamId = selection_->streamId;

			QPointer<YouTubeAccountDestinationPreparer> guard(owner_);
			const auto status = resolverPort_->startResolveStream(
				std::move(request),
				[guard, epoch, attempt](YouTubeStreamResolverCompletion completion) mutable {
					if (guard) {
						guard->impl_->handleResolver(epoch, attempt, std::move(completion));
					}
				});
			if (status != YouTubeStreamResolverStartStatus::Started &&
			    isCurrent(epoch, attempt, YouTubeDestinationPreparerState::ResolvingStream)) {
				finish(resolverStartFailure(status));
			}
		} catch (...) {
			finish(YouTubeDestinationPrepareStatus::ServiceUnavailable);
		}
	}

	void handleResolver(std::uint64_t epoch, YouTubeDestinationPrepareAttempt attempt,
			    YouTubeStreamResolverCompletion completion) noexcept
	{
		if (!isCurrent(epoch, attempt, YouTubeDestinationPreparerState::ResolvingStream) ||
		    !matches(completion.attempt, attempt)) {
			return;
		}
		if (!completion.succeeded()) {
			finish(resolverFailure(completion));
			return;
		}
		if (!completion.ingestion.has_value() || !validIngestion(*completion.ingestion)) {
			finish(YouTubeDestinationPrepareStatus::InvalidResponse);
			return;
		}
		finish(YouTubeDestinationPrepareStatus::Success, std::move(completion.ingestion));
	}

	void finish(YouTubeDestinationPrepareStatus status,
		    std::optional<YouTubeResolvedIngestion> ingestion = std::nullopt) noexcept
	{
		if (!activeAttempt_.has_value() || !completionHandler_) {
			clearOperation();
			state_ = YouTubeDestinationPreparerState::Completed;
			releaseOperationLock();
			return;
		}
		try {
			YouTubeDestinationPrepareCompletion completion;
			completion.attempt = *activeAttempt_;
			completion.status = status;
			completion.ingestion = std::move(ingestion);
			pendingCompletion_.emplace(std::move(completion));
			selection_.reset();
			credentialScope_.reset();
			state_ = YouTubeDestinationPreparerState::DeliveringCompletion;
			queueCompletion(operationEpoch_);
		} catch (...) {
			clearOperation();
			state_ = YouTubeDestinationPreparerState::Completed;
			releaseOperationLock();
		}
	}

	void queueCompletion(std::uint64_t epoch) noexcept
	{
		if (completionDeliveryQueued_ || !pendingCompletion_.has_value()) {
			return;
		}
		completionDeliveryQueued_ = true;
		const bool queued = QMetaObject::invokeMethod(
			owner_,
			[this, epoch]() {
				if (epoch != operationEpoch_) {
					return;
				}
				completionDeliveryQueued_ = false;
				deliverCompletion();
			},
			Qt::QueuedConnection);
		if (!queued) {
			completionDeliveryQueued_ = false;
			clearOperation();
			state_ = YouTubeDestinationPreparerState::Completed;
			releaseOperationLock();
		}
	}

	void deliverCompletion() noexcept
	{
		if (state_ != YouTubeDestinationPreparerState::DeliveringCompletion ||
		    !pendingCompletion_.has_value()) {
			return;
		}

		YouTubeDestinationPrepareCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		CompletionHandler handler = std::move(completionHandler_);
		completionHandler_ = {};
		activeAttempt_.reset();
		state_ = completion.status == YouTubeDestinationPrepareStatus::Cancelled
				 ? YouTubeDestinationPreparerState::Cancelled
				 : YouTubeDestinationPreparerState::Completed;
		const bool lockReleased = releaseOperationLock();
		if (!lockReleased) {
			// The operation cannot be reported as usable while its profile lease is
			// still held. Discard the destination even when preparation succeeded;
			// shutdown() can retry the release on the owner thread.
			completion.status = YouTubeDestinationPrepareStatus::ServiceUnavailable;
			completion.ingestion.reset();
		}
		if (!handler) {
			return;
		}
		try {
			handler(std::move(completion));
		} catch (...) {
			// Integration callbacks must never unwind through the Qt event loop.
		}
	}

	YouTubeAccountDestinationPreparer *owner_ = nullptr;
	QString clientId_;
	std::unique_ptr<YouTubeDestinationRefreshPort> refreshPort_;
	std::unique_ptr<YouTubeDestinationResolverPort> resolverPort_;
	YouTubeAccountRefreshTokenStore &refreshTokenStore_;
	YouTubeAccountProfileOperationLockProvider &operationLockProvider_;
	YouTubeAccountProfileOperationLock operationLock_;
	YouTubeDestinationPreparerState state_ = YouTubeDestinationPreparerState::Idle;
	std::optional<YouTubeDestinationPrepareAttempt> activeAttempt_;
	std::optional<YouTubeAccountCredentialScope> credentialScope_;
	std::optional<YouTubeAccountSelection> selection_;
	CompletionHandler completionHandler_;
	std::optional<YouTubeDestinationPrepareCompletion> pendingCompletion_;
	std::uint64_t operationEpoch_ = 0;
	bool completionDeliveryQueued_ = false;
	bool lifecycleMutationInProgress_ = false;
	bool refreshPortClosed_ = false;
	bool resolverPortClosed_ = false;
};

YouTubeAccountDestinationPreparer::YouTubeAccountDestinationPreparer(
	QString clientId, YouTubeAccountRefreshTokenStore &refreshTokenStore,
	YouTubeAccountProfileOperationLockProvider &operationLockProvider, QObject *parent)
	: YouTubeAccountDestinationPreparer(std::move(clientId), std::make_unique<RefreshAdapter>(),
					    std::make_unique<ResolverAdapter>(), refreshTokenStore,
					    operationLockProvider, parent)
{
}

YouTubeAccountDestinationPreparer::YouTubeAccountDestinationPreparer(
	QString clientId, std::unique_ptr<YouTubeDestinationRefreshPort> refreshPort,
	std::unique_ptr<YouTubeDestinationResolverPort> resolverPort,
	YouTubeAccountRefreshTokenStore &refreshTokenStore,
	YouTubeAccountProfileOperationLockProvider &operationLockProvider, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(clientId), std::move(refreshPort), std::move(resolverPort),
				       refreshTokenStore, operationLockProvider))
{
}

YouTubeAccountDestinationPreparer::~YouTubeAccountDestinationPreparer()
{
	assert(QThread::currentThread() == thread());
	impl_->shutdown();
}

YouTubeDestinationPrepareStartStatus
YouTubeAccountDestinationPreparer::start(YouTubeDestinationPrepareRequest request,
					 CompletionHandler completionHandler) noexcept
{
	return impl_->start(std::move(request), std::move(completionHandler));
}

bool YouTubeAccountDestinationPreparer::cancel(YouTubeDestinationPrepareAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool YouTubeAccountDestinationPreparer::invalidateContext() noexcept
{
	return impl_->invalidateContext();
}

bool YouTubeAccountDestinationPreparer::shutdown() noexcept
{
	return impl_->shutdown();
}

YouTubeDestinationPreparerState YouTubeAccountDestinationPreparer::state() const noexcept
{
	return impl_->state();
}

std::optional<YouTubeDestinationPrepareAttempt> YouTubeAccountDestinationPreparer::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

bool YouTubeAccountDestinationPreparer::lockHeld() const noexcept
{
	return impl_->lockHeld();
}

} // namespace easy_multistream

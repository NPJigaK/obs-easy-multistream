// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-authorization-session.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <cstdint>
#include <optional>
#include <utility>

namespace easy_multistream {

class GoogleOAuthAuthorizationSession::Impl final {
public:
	Impl(GoogleOAuthAuthorizationSession *owner, BrowserOpener browserOpener,
	     GoogleOAuthLoopbackOptions listenerOptions)
		: owner_(owner), browserOpener_(std::move(browserOpener)), listener_(listenerOptions)
	{
	}

	~Impl()
	{
		Q_ASSERT(!owner_ || QThread::currentThread() == owner_->thread());
		if (onOwnerThread()) {
			shutdown();
		}
	}

	GoogleOAuthAuthorizationStartStatus start(GoogleOAuthLoopbackAttempt attempt, const QString &clientId,
						 GoogleOAuthConsentMode consentMode,
						 CompletionHandler completionHandler) noexcept
	{
		if (!onOwnerThread()) {
			return GoogleOAuthAuthorizationStartStatus::WrongThread;
		}
		if (state_ == GoogleOAuthAuthorizationSessionState::Closed) {
			return GoogleOAuthAuthorizationStartStatus::Closed;
		}
		if (isBusy()) {
			return GoogleOAuthAuthorizationStartStatus::Busy;
		}
		if (!attempt.isValid()) {
			return GoogleOAuthAuthorizationStartStatus::InvalidAttempt;
		}
		if (!browserOpener_) {
			return GoogleOAuthAuthorizationStartStatus::InvalidBrowserOpener;
		}
		if (!completionHandler) {
			return GoogleOAuthAuthorizationStartStatus::InvalidCompletionHandler;
		}

		const GoogleOAuthLoopbackBindResult bound = listener_.bind(attempt);
		if (!bound.succeeded()) {
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::ListenerBindFailed;
		}

		GoogleOAuthAuthorizationResult authorization;
		try {
			authorization = makeGoogleOAuthAuthorizationRequest(clientId, bound.port, consentMode);
		} catch (...) {
			listener_.cancel();
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::AuthorizationRequestFailed;
		}
		if (!authorization.succeeded()) {
			listener_.cancel();
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::AuthorizationRequestFailed;
		}

		advanceEpoch();
		const std::uint64_t epoch = operationEpoch_;
		activeAttempt_ = attempt;
		redirectUri_ = std::move(authorization.request.redirectUri);
		codeVerifier_ = std::move(authorization.request.codeVerifier);
		completionHandler_ = std::move(completionHandler);
		state_ = GoogleOAuthAuthorizationSessionState::Authorizing;

		GoogleOAuthLoopbackArmStatus armStatus = GoogleOAuthLoopbackArmStatus::InvalidCompletionHandler;
		try {
			armStatus = listener_.arm(redirectUri_, std::move(authorization.request.state),
					  [this, epoch](GoogleOAuthLoopbackCompletion completion) {
						  handleListenerCompletion(epoch, std::move(completion));
					  });
		} catch (...) {
			listener_.cancel();
			clearOperation();
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::ListenerArmFailed;
		}
		if (armStatus != GoogleOAuthLoopbackArmStatus::Success) {
			listener_.cancel();
			clearOperation();
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::ListenerArmFailed;
		}

		// Copy the std::function before invoking it. A re-entrant shutdown may
		// clear the stored opener while this local callable is still executing.
		BrowserOpener opener;
		try {
			opener = browserOpener_;
		} catch (...) {
			listener_.cancel();
			advanceEpoch();
			clearOperation();
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::BrowserOpenFailed;
		}
		QPointer<GoogleOAuthAuthorizationSession> guard(owner_);
		bool opened = false;
		try {
			opened = opener(authorization.request.authorizationUrl);
		} catch (...) {
			opened = false;
		}
		authorization.request.authorizationUrl.clear();

		// A fake opener can run a nested event loop. If a valid callback, cancel,
		// or shutdown won during that re-entry, its newer state must not be
		// overwritten by a late false return from the opener.
		if (!guard) {
			return GoogleOAuthAuthorizationStartStatus::Started;
		}
		if (operationEpoch_ != epoch) {
			if (state_ == GoogleOAuthAuthorizationSessionState::Closed) {
				return GoogleOAuthAuthorizationStartStatus::Closed;
			}
			if (state_ == GoogleOAuthAuthorizationSessionState::Cancelled) {
				return GoogleOAuthAuthorizationStartStatus::Cancelled;
			}
			return GoogleOAuthAuthorizationStartStatus::Started;
		}
		if (!opened && operationEpoch_ == epoch && state_ == GoogleOAuthAuthorizationSessionState::Authorizing &&
		    activeAttempt_.has_value() && *activeAttempt_ == attempt) {
			listener_.cancel();
			advanceEpoch();
			clearOperation();
			state_ = GoogleOAuthAuthorizationSessionState::Idle;
			return GoogleOAuthAuthorizationStartStatus::BrowserOpenFailed;
		}

		return GoogleOAuthAuthorizationStartStatus::Started;
	}

	bool cancel(GoogleOAuthLoopbackAttempt attempt) noexcept
	{
		if (!onOwnerThread() || state_ == GoogleOAuthAuthorizationSessionState::Closed ||
		    !activeAttempt_.has_value() || !(*activeAttempt_ == attempt) || !isBusy()) {
			return false;
		}

		advanceEpoch();
		listener_.cancel();
		clearOperation();
		state_ = GoogleOAuthAuthorizationSessionState::Cancelled;
		return true;
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		if (state_ == GoogleOAuthAuthorizationSessionState::Closed) {
			return true;
		}

		advanceEpoch();
		listener_.cancel();
		clearOperation();
		browserOpener_ = {};
		state_ = GoogleOAuthAuthorizationSessionState::Closed;
		return true;
	}

	GoogleOAuthAuthorizationSessionState state() const noexcept { return state_; }

	std::optional<GoogleOAuthLoopbackAttempt> activeAttempt() const noexcept { return activeAttempt_; }

private:
	bool onOwnerThread() const noexcept
	{
		QThread *current = QThread::currentThread();
		return owner_ != nullptr && current == owner_->thread() && current == listener_.thread();
	}

	bool isBusy() const noexcept
	{
		return state_ == GoogleOAuthAuthorizationSessionState::Authorizing ||
		       state_ == GoogleOAuthAuthorizationSessionState::DeliveringCompletion ||
		       activeAttempt_.has_value() || pendingCompletion_.has_value();
	}

	void advanceEpoch() noexcept
	{
		++operationEpoch_;
		if (operationEpoch_ == 0) {
			++operationEpoch_;
		}
		completionDeliveryQueued_ = false;
	}

	void clearOperation() noexcept
	{
		codeVerifier_.clear();
		redirectUri_.clear();
		completionHandler_ = {};
		pendingCompletion_.reset();
		activeAttempt_.reset();
		completionDeliveryQueued_ = false;
	}

	void handleListenerCompletion(std::uint64_t epoch, GoogleOAuthLoopbackCompletion listenerCompletion)
	{
		if (!onOwnerThread() || epoch != operationEpoch_ ||
		    state_ != GoogleOAuthAuthorizationSessionState::Authorizing || !activeAttempt_.has_value() ||
		    !(listenerCompletion.attempt == *activeAttempt_)) {
			return;
		}

		GoogleOAuthAuthorizationCompletion completion;
		completion.attempt = listenerCompletion.attempt;
		if (listenerCompletion.status == GoogleOAuthLoopbackCompletionStatus::Callback) {
			completion.callbackStatus = listenerCompletion.callback.status;
			completion.providerError = listenerCompletion.callback.providerError;
			if (listenerCompletion.callback.succeeded() && !codeVerifier_.empty() && !redirectUri_.isEmpty()) {
				completion.status = GoogleOAuthAuthorizationCompletionStatus::Success;
				completion.redirectUri = std::move(redirectUri_);
				completion.authorizationCode =
					std::move(listenerCompletion.callback.authorizationCode);
				completion.codeVerifier = std::move(codeVerifier_);
			} else if (listenerCompletion.callback.hasProviderError()) {
				completion.status = GoogleOAuthAuthorizationCompletionStatus::ProviderRejected;
			} else {
				completion.status = GoogleOAuthAuthorizationCompletionStatus::CallbackRejected;
			}
		} else if (listenerCompletion.status ==
			   GoogleOAuthLoopbackCompletionStatus::AuthorizationTimedOut) {
			completion.status = GoogleOAuthAuthorizationCompletionStatus::AuthorizationTimedOut;
		} else if (listenerCompletion.status == GoogleOAuthLoopbackCompletionStatus::RequestLimitReached) {
			completion.status = GoogleOAuthAuthorizationCompletionStatus::RequestLimitReached;
		} else {
			completion.status = GoogleOAuthAuthorizationCompletionStatus::ListenerFailure;
		}

		codeVerifier_.clear();
		redirectUri_.clear();
		pendingCompletion_.emplace(std::move(completion));
		state_ = GoogleOAuthAuthorizationSessionState::DeliveringCompletion;
		queueCompletion(epoch);
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
			state_ = GoogleOAuthAuthorizationSessionState::Completed;
		}
	}

	void deliverCompletion()
	{
		if (!pendingCompletion_.has_value()) {
			return;
		}

		GoogleOAuthAuthorizationCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		CompletionHandler handler = std::move(completionHandler_);
		completionHandler_ = {};
		activeAttempt_.reset();
		state_ = GoogleOAuthAuthorizationSessionState::Completed;
		if (!handler) {
			return;
		}
		try {
			handler(std::move(completion));
		} catch (...) {
			// Do not allow an integration callback to unwind through Qt.
		}
	}

	GoogleOAuthAuthorizationSession *owner_ = nullptr;
	BrowserOpener browserOpener_;
	GoogleOAuthLoopbackListener listener_;
	GoogleOAuthAuthorizationSessionState state_ = GoogleOAuthAuthorizationSessionState::Idle;
	std::optional<GoogleOAuthLoopbackAttempt> activeAttempt_;
	SecureBuffer codeVerifier_;
	QUrl redirectUri_;
	CompletionHandler completionHandler_;
	std::optional<GoogleOAuthAuthorizationCompletion> pendingCompletion_;
	std::uint64_t operationEpoch_ = 1;
	bool completionDeliveryQueued_ = false;
};

GoogleOAuthAuthorizationSession::GoogleOAuthAuthorizationSession(BrowserOpener browserOpener,
							 GoogleOAuthLoopbackOptions listenerOptions,
							 QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(browserOpener), listenerOptions))
{
}

GoogleOAuthAuthorizationSession::~GoogleOAuthAuthorizationSession()
{
	Q_ASSERT_X(thread() == QThread::currentThread(),
		   "GoogleOAuthAuthorizationSession::~GoogleOAuthAuthorizationSession",
		   "Destroy the authorization session on its owner thread after shutdown");
	if (thread() == QThread::currentThread() && impl_) {
		impl_->shutdown();
	}
}

GoogleOAuthAuthorizationStartStatus GoogleOAuthAuthorizationSession::start(
	GoogleOAuthLoopbackAttempt attempt, const QString &clientId, GoogleOAuthConsentMode consentMode,
	CompletionHandler completionHandler) noexcept
{
	return impl_->start(attempt, clientId, consentMode, std::move(completionHandler));
}

bool GoogleOAuthAuthorizationSession::cancel(GoogleOAuthLoopbackAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool GoogleOAuthAuthorizationSession::shutdown() noexcept
{
	return impl_->shutdown();
}

GoogleOAuthAuthorizationSessionState GoogleOAuthAuthorizationSession::state() const noexcept
{
	return impl_->state();
}

std::optional<GoogleOAuthLoopbackAttempt> GoogleOAuthAuthorizationSession::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

} // namespace easy_multistream

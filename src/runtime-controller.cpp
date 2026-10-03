// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "runtime-controller.hpp"

#include "settings.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace easy_multistream {

namespace {

bool hasManualYouTubeDestination(const RuntimeSettings &settings) noexcept
{
	return settings.youtubeConnectionMode == YouTubeConnectionMode::Manual && settings.youtubeKeyAvailable &&
	       isValidRtmpsUrl(settings.youtubeServerUrl);
}

} // namespace

bool isValidRtmpsUrl(std::string_view value) noexcept
{
	// Keep the runtime gate identical to the settings/profile validator.  This
	// avoids accepting a URL in the state machine that the settings controller
	// would reject (or persisting a value the adapter cannot safely use).
	return validateYouTubeServerUrl(value) == YouTubeServerUrlValidationError::None;
}

struct RuntimeController::CallbackState {
	std::mutex mutex;
	RuntimeController *owner = nullptr;
	RuntimeController::Post post;
};

RuntimeController::RuntimeController(IRuntimeYouTubeOutputAdapter &adapter, ReadYouTubeKey readYouTubeKey, Post post)
	: adapter_(adapter),
	  readYouTubeKey_(std::move(readYouTubeKey)),
	  post_(std::move(post)),
	  callbackState_(std::make_shared<CallbackState>())
{
	if (!post_) {
		post_ = [](std::function<void()> callback) {
			if (callback) {
				callback();
			}
		};
	}

	callbackState_->owner = this;
	callbackState_->post = post_;

	const std::weak_ptr<CallbackState> weakState = callbackState_;
	adapter_.setEventSink([weakState](RuntimeOutputEvent event) noexcept {
		const auto state = weakState.lock();
		if (!state) {
			return;
		}

		RuntimeController::Post post;
		{
			std::lock_guard lock(state->mutex);
			if (state->owner == nullptr) {
				return;
			}
			post = state->post;
		}

		if (!post) {
			return;
		}

		try {
			post([weakState, event]() noexcept {
				const auto queuedState = weakState.lock();
				if (!queuedState) {
					return;
				}

				RuntimeController *owner = nullptr;
				{
					std::lock_guard lock(queuedState->mutex);
					owner = queuedState->owner;
				}
				if (owner != nullptr) {
					owner->onYouTubeOutputEvent(event);
				}
			});
		} catch (...) {
			// Adapter callbacks must not let an exception escape through OBS's
			// signal handler.  The adapter's teardown barrier remains authoritative;
			// the owner will observe its final Released event if it can be queued.
		}
	});
}

RuntimeController::~RuntimeController()
{
	if (callbackState_) {
		std::lock_guard lock(callbackState_->mutex);
		callbackState_->owner = nullptr;
		callbackState_->post = {};
	}

	// Removing the sink first makes already queued adapter notifications inert.
	// The concrete adapter's shutdown is required to be idempotent and to own
	// any last-resort teardown if a caller destroys the controller without first
	// sending EXIT.  Normal plugin shutdown calls onExit() and reaches this path
	// only after the Released barrier.
	adapter_.setEventSink({});
	if (!adapterShutdownCalled_) {
		adapter_.shutdown();
		adapterShutdownCalled_ = true;
	}
}

void RuntimeController::setSnapshotSink(SnapshotSink sink)
{
	snapshotSink_ = std::move(sink);
	if (snapshotSink_) {
		try {
			snapshotSink_(coordinator_.snapshot());
		} catch (...) {
			// A view/presenter is not allowed to break the runtime state machine.
		}
	}
}

void RuntimeController::setSettings(RuntimeSettings settings)
{
	if (exitSeen_) {
		return;
	}

	settings_ = std::move(settings);
	const bool configured = hasManualYouTubeDestination(settings_);
	applyTransition(coordinator_.configure(settings_.nativeDestination, settings_.youtubeEnabled, configured));
}

void RuntimeController::onProfileChanging()
{
	if (exitSeen_) {
		return;
	}

	// The old profile's URL and credential availability must not be used if a
	// queued Released event completes the retirement later.
	settings_ = {};
	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	activeNativeLease_.reset();
	stoppingNativeLease_.reset();
	applyTransition(coordinator_.profileChanging());
}

void RuntimeController::onProfileChanged(RuntimeSettings settings)
{
	if (exitSeen_) {
		return;
	}

	settings_ = std::move(settings);
	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	activeNativeLease_.reset();
	stoppingNativeLease_.reset();
	const bool configured = hasManualYouTubeDestination(settings_);
	applyTransition(coordinator_.profileChanged(settings_.nativeDestination, settings_.youtubeEnabled, configured));
}

void RuntimeController::onStreamingStarting()
{
	onStreamingStarting(settings_.nativeDestination);
}

void RuntimeController::onStreamingStarting(NativeDestination observedDestination)
{
	if (exitSeen_) {
		return;
	}

	// If an old attempt is stopping, retain its lease only for its eventual
	// late STOPPED notification.  The coordinator itself will assign a fresh
	// generation/attempt to this new start.
	if (coordinator_.snapshot().native == NativeStreamState::Stopping && activeNativeLease_) {
		stoppingNativeLease_ = activeNativeLease_;
	}
	settings_.nativeDestination = observedDestination;
	const bool configured = hasManualYouTubeDestination(settings_);
	applyTransition(coordinator_.configure(settings_.nativeDestination, settings_.youtubeEnabled, configured));

	const SessionTransition starting = coordinator_.nativeStarting();
	applyTransition(starting);
	if (!starting.snapshot.nativeLease.has_value() || starting.snapshot.native != NativeStreamState::Starting) {
		return;
	}

	const NativeLease lease = *starting.snapshot.nativeLease;
	activeNativeLease_ = lease;
	pendingNativeStartCheck_ = lease;
	nativeOutputStartingObserved_ = false;

	const std::weak_ptr<CallbackState> weakState = callbackState_;
	try {
		post_([weakState, lease]() noexcept {
			const auto state = weakState.lock();
			if (!state) {
				return;
			}

			RuntimeController *owner = nullptr;
			{
				std::lock_guard lock(state->mutex);
				owner = state->owner;
			}
			if (owner != nullptr) {
				owner->reconcileNativeStart(lease);
			}
		});
	} catch (...) {
		// A dispatcher failure must not leave a native lease in Starting forever.
		reconcileNativeStart(lease);
	}
}

void RuntimeController::onStreamingStarted()
{
	if (activeNativeLease_) {
		onStreamingStarted(*activeNativeLease_);
	}
}

void RuntimeController::onStreamingStarted(NativeLease lease)
{
	if (exitSeen_ || !activeNativeLease_ || *activeNativeLease_ != lease) {
		return;
	}

	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	applyTransition(coordinator_.nativeStarted(*activeNativeLease_));
}

void RuntimeController::onStreamingStopping()
{
	if (exitSeen_) {
		return;
	}

	const std::optional<NativeLease> lease = activeNativeLease_.has_value()
									? activeNativeLease_
									: coordinator_.snapshot().nativeLease;
	if (!lease) {
		return;
	}
	onStreamingStopping(*lease);
}

void RuntimeController::onStreamingStopping(NativeLease lease)
{
	if (exitSeen_ || !activeNativeLease_ || *activeNativeLease_ != lease) {
		return;
	}
	stoppingNativeLease_ = lease;
	applyTransition(coordinator_.nativeStopping(lease));
}

void RuntimeController::onStreamingStopped()
{
	const std::optional<NativeLease> lease = stoppingNativeLease_.has_value()
									? stoppingNativeLease_
									: activeNativeLease_.has_value() ? activeNativeLease_ : coordinator_.snapshot().nativeLease;
	if (lease) {
		onStreamingStopped(*lease);
	}
}

void RuntimeController::onStreamingStopped(NativeLease lease)
{
	if (pendingNativeStartCheck_.has_value() && *pendingNativeStartCheck_ == lease) {
		pendingNativeStartCheck_.reset();
		nativeOutputStartingObserved_ = false;
	}
	applyTransition(coordinator_.nativeStopped(lease));
	clearNativeLeaseAfterStop(lease);
}

void RuntimeController::onNativeOutputStarting(NativeLease lease)
{
	if (pendingNativeStartCheck_.has_value() && *pendingNativeStartCheck_ == lease) {
		nativeOutputStartingObserved_ = true;
	}
}

void RuntimeController::onYouTubeOutputEvent(RuntimeOutputEvent event)
{
	if (event.kind == RuntimeOutputEventKind::Released) {
		if (pendingOutputStop_.has_value() && *pendingOutputStop_ == event.lease) {
			pendingOutputStop_.reset();
		}
		applyTransition(coordinator_.youtubeReleased(event.lease));
		maybeShutdownAdapter();
		return;
	}

	SessionTransition transition;
	switch (event.kind) {
	case RuntimeOutputEventKind::Started:
		transition = coordinator_.youtubeStarted(event.lease);
		break;
	case RuntimeOutputEventKind::Reconnecting:
		transition = coordinator_.youtubeReconnecting(event.lease);
		break;
	case RuntimeOutputEventKind::ReconnectSucceeded:
		transition = coordinator_.youtubeReconnectSucceeded(event.lease);
		break;
	case RuntimeOutputEventKind::Released:
		return;
	}
	applyTransition(transition);
}

void RuntimeController::retryYouTube()
{
	if (exitSeen_) {
		return;
	}
	applyTransition(coordinator_.retryYouTube());
}

void RuntimeController::onExit()
{
	if (exitSeen_) {
		maybeShutdownAdapter();
		return;
	}

	exitSeen_ = true;
	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	activeNativeLease_.reset();
	stoppingNativeLease_.reset();
	applyTransition(coordinator_.exit());
	maybeShutdownAdapter();
}

SessionSnapshot RuntimeController::snapshot() const noexcept
{
	return coordinator_.snapshot();
}

void RuntimeController::applyTransition(SessionTransition transition)
{
	const bool shouldPublish = transition.effect.has_value() || transition.snapshot.revision != publishedRevision_;
	if (shouldPublish) {
		publishedRevision_ = transition.snapshot.revision;
		publish(transition.snapshot);
	}

	if (transition.effect.has_value()) {
		applyEffect(*transition.effect);
	}
}

void RuntimeController::applyEffect(const SessionEffect &effect)
{
	switch (effect.kind) {
	case SessionEffectKind::StartYouTube:
		startYouTube(effect.lease);
		break;
	case SessionEffectKind::StopYouTube:
		pendingOutputStop_ = effect.lease;
		adapter_.requestStop(effect.lease);
		break;
	}
}

void RuntimeController::startYouTube(OutputLease lease)
{
	// The coordinator is the authority for the lease.  This check prevents a
	// stale effect from reaching the adapter if integration code re-enters the
	// bridge while processing a profile or output callback.
	const SessionSnapshot current = coordinator_.snapshot();
	if (current.phase != SessionPhase::Ready || !current.youtubeLease.has_value() ||
	    *current.youtubeLease != lease || current.youtube != YouTubeStreamState::Connecting ||
	    !settings_.youtubeEnabled || !hasManualYouTubeDestination(settings_) || pendingOutputStop_.has_value()) {
		completeRejectedStart(lease);
		return;
	}

	SecureBuffer key;
	try {
		if (readYouTubeKey_) {
			key = readYouTubeKey_();
		}
	} catch (...) {
		completeRejectedStart(lease);
		return;
	}

	if (key.empty() || validateYouTubeStreamKey(key.view()) != StreamKeyValidationError::None) {
		completeRejectedStart(lease);
		return;
	}

	// start() is noexcept by contract.  A synchronous rejection must still be
	// reported by the adapter as Released after it has completed its own cleanup.
	adapter_.start(YouTubeStartRequest{lease, settings_.youtubeServerUrl, std::move(key)});
}

void RuntimeController::reconcileNativeStart(NativeLease lease)
{
	if (!pendingNativeStartCheck_.has_value() || *pendingNativeStartCheck_ != lease) {
		return;
	}

	pendingNativeStartCheck_.reset();
	if (nativeOutputStartingObserved_) {
		nativeOutputStartingObserved_ = false;
		return;
	}

	nativeOutputStartingObserved_ = false;
	applyTransition(coordinator_.nativeStartFailed(lease));
	if (coordinator_.snapshot().native != NativeStreamState::Starting) {
		clearNativeLeaseAfterStop(lease);
	}
}

void RuntimeController::maybeShutdownAdapter() noexcept
{
	if (!exitSeen_ || adapterShutdownCalled_ || pendingOutputStop_.has_value() ||
	    coordinator_.snapshot().youtubeLease.has_value()) {
		return;
	}

	adapterShutdownCalled_ = true;
	adapter_.shutdown();
}

void RuntimeController::publish(SessionSnapshot snapshot)
{
	if (!snapshotSink_) {
		return;
	}
	try {
		snapshotSink_(snapshot);
	} catch (...) {
		// UI/presenter failures must not corrupt the runtime state machine.
	}
}

void RuntimeController::completeRejectedStart(OutputLease lease) noexcept
{
	// There is no adapter lease when the request was rejected before adapter
	// creation.  Feeding the same terminal event through the normal path keeps
	// SessionCoordinator's failure isolation and retry barrier intact.
	onYouTubeOutputEvent({RuntimeOutputEventKind::Released, lease});
}

void RuntimeController::clearNativeLeaseAfterStop(NativeLease lease) noexcept
{
	if (activeNativeLease_.has_value() && *activeNativeLease_ == lease) {
		activeNativeLease_.reset();
	}
	if (stoppingNativeLease_.has_value() && *stoppingNativeLease_ == lease) {
		stoppingNativeLease_.reset();
	}
}

} // namespace easy_multistream

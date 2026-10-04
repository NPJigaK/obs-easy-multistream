// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "runtime-controller.hpp"

#include "settings.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
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
	std::deque<RuntimeOutputEvent> postedOutputEvents;
};

RuntimeController::RuntimeController(IRuntimeYouTubeOutputAdapter &adapter, ReadYouTubeKey readYouTubeKey, Post post,
					     IRuntimeYouTubeDestinationProvider *destinationProvider)
	: adapter_(adapter),
	  readYouTubeKey_(std::move(readYouTubeKey)),
	  post_(std::move(post)),
	  destinationProvider_(destinationProvider),
	  callbackState_(std::make_shared<CallbackState>())
{
	if (!post_) {
		post_ = [](std::function<void()> callback) {
			if (callback) {
				callback();
			}
			return true;
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
		try {
			std::lock_guard lock(state->mutex);
			if (state->owner == nullptr) {
				return;
			}
			// Store the event before requesting a wake-up. If the dispatcher is
			// temporarily unable to queue one, the next owner-thread operation
			// drains this mailbox instead of losing a Released barrier.
			state->postedOutputEvents.push_back(event);
			post = state->post;
		} catch (...) {
			return;
		}

		if (!post) {
			return;
		}

		try {
			(void)post([weakState]() noexcept {
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
					owner->drainPostedOutputEvents();
				}
			});
		} catch (...) {
			// The mailbox remains available to the next owner-thread operation.
			// Adapter callbacks must never unwind through OBS's signal handler.
		}
	});
}

RuntimeController::~RuntimeController()
{
	if (!exitSeen_) {
		// Normal plugin teardown calls onExit(), but keeping the destructor safe
		// matters for tests and for an exceptional module-unload path.  This may
		// synchronously cancel an in-flight account preparation before the
		// callback guard is made inert below.
		onExit();
	}

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

	// If the adapter did not deliver a Released event before destruction, its
	// shutdown barrier is now complete and it is safe to relinquish any
	// account destination-use lease that was handed to it.  A normal Released
	// event clears this optional before reaching this path.
	if (activeDestinationLease_.has_value()) {
		releaseDestinationLease(*activeDestinationLease_);
	}
	if (destinationProvider_ && !destinationProviderShutdownCalled_) {
		destinationProvider_->shutdown();
		destinationProviderShutdownCalled_ = true;
	}
}

void RuntimeController::setSnapshotSink(SnapshotSink sink)
{
	drainPostedOutputEvents();
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
		drainPostedOutputEvents();
		return;
	}

	const bool modeChanged = settings_.youtubeConnectionMode != settings.youtubeConnectionMode;
	if (modeChanged && coordinator_.snapshot().youtubeLease.has_value()) {
		// A provider switch is an output boundary.  Force the old lease through
		// the normal stop/release barrier before the new provider can start; this
		// prevents a manual and account destination from sharing one output lease.
		const RuntimeSettings oldSettings = settings_;
		applyTransition(coordinator_.configure(oldSettings.nativeDestination, oldSettings.youtubeEnabled, false));
	}

	settings_ = std::move(settings);
	const bool configured = hasConfiguredYouTubeDestination(settings_);
	applyTransition(coordinator_.configure(settings_.nativeDestination, settings_.youtubeEnabled, configured));
	drainPostedOutputEvents();
}

void RuntimeController::onProfileChanging()
{
	if (exitSeen_) {
		drainPostedOutputEvents();
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
	drainPostedOutputEvents();
}

void RuntimeController::onProfileChanged(RuntimeSettings settings)
{
	if (exitSeen_) {
		drainPostedOutputEvents();
		return;
	}

	settings_ = std::move(settings);
	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	activeNativeLease_.reset();
	stoppingNativeLease_.reset();
	const bool configured = hasConfiguredYouTubeDestination(settings_);
	applyTransition(coordinator_.profileChanged(settings_.nativeDestination, settings_.youtubeEnabled, configured));
	drainPostedOutputEvents();
}

void RuntimeController::onStreamingStarting()
{
	onStreamingStarting(settings_.nativeDestination);
}

void RuntimeController::onStreamingStarting(NativeDestination observedDestination)
{
	if (exitSeen_) {
		drainPostedOutputEvents();
		return;
	}

	// If an old attempt is stopping, retain its lease only for its eventual
	// late STOPPED notification.  The coordinator itself will assign a fresh
	// generation/attempt to this new start.
	if (coordinator_.snapshot().native == NativeStreamState::Stopping && activeNativeLease_) {
		stoppingNativeLease_ = activeNativeLease_;
	}
	settings_.nativeDestination = observedDestination;
	const bool configured = hasConfiguredYouTubeDestination(settings_);
	applyTransition(coordinator_.configure(settings_.nativeDestination, settings_.youtubeEnabled, configured));

	const SessionTransition starting = coordinator_.nativeStarting();
	applyTransition(starting);
	if (!starting.snapshot.nativeLease.has_value() || starting.snapshot.native != NativeStreamState::Starting) {
		drainPostedOutputEvents();
		return;
	}

	const NativeLease lease = *starting.snapshot.nativeLease;
	activeNativeLease_ = lease;
	pendingNativeStartCheck_ = lease;
	nativeOutputStartingObserved_ = false;
	drainPostedOutputEvents();

	const std::weak_ptr<CallbackState> weakState = callbackState_;
	bool posted = false;
	try {
		posted = post_([weakState, lease]() noexcept {
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
		return;
	}
	if (!posted) {
		// A rejected dispatch has the same semantics as a failed dispatcher
		// invocation; no queued callback will reconcile this lease later.
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
		drainPostedOutputEvents();
		return;
	}

	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	applyTransition(coordinator_.nativeStarted(*activeNativeLease_));
	drainPostedOutputEvents();
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
		drainPostedOutputEvents();
		return;
	}
	stoppingNativeLease_ = lease;
	applyTransition(coordinator_.nativeStopping(lease));
	drainPostedOutputEvents();
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
	drainPostedOutputEvents();
}

void RuntimeController::onNativeOutputStarting(NativeLease lease)
{
	if (pendingNativeStartCheck_.has_value() && *pendingNativeStartCheck_ == lease) {
		nativeOutputStartingObserved_ = true;
	}
	drainPostedOutputEvents();
}

void RuntimeController::drainPostedOutputEvents() noexcept
{
	if (!callbackState_) {
		return;
	}

	for (;;) {
		std::deque<RuntimeOutputEvent> events;
		{
			std::lock_guard lock(callbackState_->mutex);
			events.swap(callbackState_->postedOutputEvents);
		}
		if (events.empty()) {
			return;
		}
		for (const RuntimeOutputEvent &event : events) {
			onYouTubeOutputEvent(event);
		}
	}
}

void RuntimeController::onYouTubeOutputEvent(RuntimeOutputEvent event)
{
	if (event.kind == RuntimeOutputEventKind::Released) {
		if (pendingOutputStop_.has_value() && *pendingOutputStop_ == event.lease) {
			pendingOutputStop_.reset();
		}
		if (activeDestinationLease_.has_value() && *activeDestinationLease_ == event.lease) {
			// The adapter has completed its full output/service/encoder teardown
			// barrier at this point.  Only now may the account provider release the
			// resolved destination-use lease.
			releaseDestinationLease(event.lease);
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
	drainPostedOutputEvents();
	if (exitSeen_) {
		return;
	}
	applyTransition(coordinator_.retryYouTube());
}

void RuntimeController::onExit()
{
	if (exitSeen_) {
		drainPostedOutputEvents();
		maybeShutdownAdapter();
		return;
	}

	exitSeen_ = true;
	pendingNativeStartCheck_.reset();
	nativeOutputStartingObserved_ = false;
	activeNativeLease_.reset();
	stoppingNativeLease_.reset();
	applyTransition(coordinator_.exit());
	drainPostedOutputEvents();
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
		if (pendingDestinationPreparation_.has_value() &&
		    pendingDestinationPreparation_->outputLease == effect.lease) {
			// No OBS output exists yet.  Cancelling the provider is the complete
			// stop operation; do not send a stop request to the adapter.
			cancelPendingDestinationPreparation(effect.lease);
			applyTransition(coordinator_.youtubeReleased(effect.lease));
			maybeShutdownAdapter();
			break;
		}
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
	    !settings_.youtubeEnabled || !hasConfiguredYouTubeDestination(settings_) || pendingOutputStop_.has_value() ||
	    pendingDestinationPreparation_.has_value()) {
		completeRejectedStart(lease);
		return;
	}

	if (settings_.youtubeConnectionMode == YouTubeConnectionMode::Account) {
		if (!destinationProvider_ || !settings_.youtubeAccountDestinationAvailable ||
		    !current.nativeLease.has_value() || current.native != NativeStreamState::Streaming) {
			applyTransition(coordinator_.youtubeSetupRequired(lease));
			return;
		}

		pendingDestinationPreparation_ = PendingDestinationPreparation{lease, *current.nativeLease};
		const std::weak_ptr<CallbackState> weakState = callbackState_;
		IRuntimeYouTubeDestinationProvider *provider = destinationProvider_;
		IRuntimeYouTubeDestinationProvider::CompletionHandler completionHandler =
			[weakState, provider](RuntimeYouTubeDestinationCompletion completion) noexcept {
				auto failUnconsumed = [weakState, provider](OutputLease failedLease, bool ownedUse) noexcept {
					if (ownedUse && provider) {
						provider->release(failedLease);
					}
					const auto failedState = weakState.lock();
					if (!failedState) {
						return;
					}
					RuntimeController *owner = nullptr;
					{
						std::lock_guard lock(failedState->mutex);
						owner = failedState->owner;
					}
					if (owner != nullptr) {
						owner->onDestinationDispatchFailed(failedLease);
					}
				};
				const OutputLease completionLease = completion.lease;
				const bool completionOwnsUse = completion.succeeded();
				const auto state = weakState.lock();
				if (!state) {
					failUnconsumed(completionLease, completionOwnsUse);
					return;
				}

				std::shared_ptr<RuntimeYouTubeDestinationCompletion> holder;
				try {
					holder = std::make_shared<RuntimeYouTubeDestinationCompletion>(std::move(completion));
				} catch (...) {
					failUnconsumed(completionLease, completionOwnsUse);
					return;
				}
				std::shared_ptr<std::atomic_bool> consumed;
				try {
					consumed = std::make_shared<std::atomic_bool>(false);
				} catch (...) {
					failUnconsumed(holder->lease, holder->succeeded());
					return;
				}

				auto failDispatch = [weakState, provider, holder, consumed]() noexcept {
					// A dispatcher must not both execute a callback and report failure,
					// but keep this ownership transfer exactly-once even for a faulty
					// replacement dispatcher.
					if (consumed->exchange(true)) {
						return;
					}
					if (holder->succeeded() && provider) {
						provider->release(holder->lease);
					}

					const auto failedState = weakState.lock();
					if (!failedState) {
						return;
					}
					RuntimeController *owner = nullptr;
					{
						std::lock_guard lock(failedState->mutex);
						owner = failedState->owner;
					}
					if (owner != nullptr) {
						owner->onDestinationDispatchFailed(holder->lease);
					}
				};

				RuntimeController::Post post;
				{
					std::lock_guard lock(state->mutex);
					if (state->owner == nullptr) {
						if (holder->succeeded() && provider) {
							provider->release(holder->lease);
						}
						return;
					}
					post = state->post;
				}
				if (!post) {
					failDispatch();
					return;
				}

				try {
					const bool posted = post([weakState, holder, consumed]() mutable noexcept {
						if (consumed->exchange(true)) {
							return;
						}
						const auto queuedState = weakState.lock();
						if (!queuedState) {
							return;
						}

						RuntimeController *owner = nullptr;
						{
							std::lock_guard lock(queuedState->mutex);
							owner = queuedState->owner;
						}
						if (owner != nullptr && holder) {
							owner->onDestinationCompletion(std::move(*holder));
						}
					});
					if (!posted) {
						failDispatch();
					}
				} catch (...) {
					// If dispatch cannot accept this completion, no output can consume
					// its destination lease. Release it and terminate the matching
					// runtime preparation synchronously on the provider owner thread.
					failDispatch();
				}
			};

		RuntimeYouTubeDestinationStartStatus startStatus = RuntimeYouTubeDestinationStartStatus::Failed;
		try {
			startStatus = destinationProvider_->start(lease, std::move(completionHandler));
		} catch (...) {
			startStatus = RuntimeYouTubeDestinationStartStatus::Failed;
		}

		// A synchronous provider completion may already have consumed the
		// pending marker and started the adapter.  Only handle a non-Started
		// return while this exact preparation is still pending.
		if (!pendingDestinationPreparation_.has_value() ||
		    pendingDestinationPreparation_->outputLease != lease) {
			return;
		}
		if (startStatus == RuntimeYouTubeDestinationStartStatus::Started) {
			return;
		}

		pendingDestinationPreparation_.reset();
		if (startStatus == RuntimeYouTubeDestinationStartStatus::SetupRequired) {
			applyTransition(coordinator_.youtubeSetupRequired(lease));
		} else {
			applyTransition(coordinator_.youtubeReleased(lease));
		}
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

void RuntimeController::onDestinationCompletion(RuntimeYouTubeDestinationCompletion completion) noexcept
{
	const OutputLease lease = completion.lease;
	if (!pendingDestinationPreparation_.has_value() || pendingDestinationPreparation_->outputLease != lease) {
		// A completion queued before cancel/profile change is stale.  It may
		// still carry a resolved key, so release the provider-use lease without
		// allowing it anywhere near the adapter.
		const bool isActiveDestination = activeDestinationLease_.has_value() && *activeDestinationLease_ == lease;
		if (!isActiveDestination) {
			if (completion.succeeded() && destinationProvider_) {
				destinationProvider_->release(lease);
			}
			if (coordinator_.snapshot().youtubeLease.has_value() && *coordinator_.snapshot().youtubeLease == lease) {
				applyTransition(coordinator_.youtubeReleased(lease));
			}
		}
		return;
	}

	const PendingDestinationPreparation preparation = *pendingDestinationPreparation_;
	pendingDestinationPreparation_.reset();
	const SessionSnapshot current = coordinator_.snapshot();
	const bool stillCurrent = current.phase == SessionPhase::Ready && current.youtubeLease.has_value() &&
					  *current.youtubeLease == lease && current.youtube == YouTubeStreamState::Connecting &&
					  settings_.youtubeEnabled && settings_.youtubeConnectionMode == YouTubeConnectionMode::Account &&
					  settings_.youtubeAccountDestinationAvailable && current.native == NativeStreamState::Streaming &&
					  current.nativeLease.has_value() && *current.nativeLease == preparation.nativeLease &&
					  activeNativeLease_.has_value() && *activeNativeLease_ == preparation.nativeLease &&
					  !pendingOutputStop_.has_value();

	if (!stillCurrent) {
		if (completion.succeeded() && destinationProvider_) {
			destinationProvider_->release(lease);
		}
		if (coordinator_.snapshot().youtubeLease.has_value() && *coordinator_.snapshot().youtubeLease == lease) {
			applyTransition(coordinator_.youtubeReleased(lease));
		}
		return;
	}

	if (completion.status == RuntimeYouTubeDestinationCompletionStatus::SetupRequired) {
		applyTransition(coordinator_.youtubeSetupRequired(lease));
		return;
	}
	if (completion.status != RuntimeYouTubeDestinationCompletionStatus::Success ||
	    !isValidRtmpsUrl(completion.serverUrl) || completion.streamKey.empty() ||
	    validateYouTubeStreamKey(completion.streamKey.view()) != StreamKeyValidationError::None) {
		if (completion.succeeded() && destinationProvider_) {
			destinationProvider_->release(lease);
		}
		applyTransition(coordinator_.youtubeReleased(lease));
		return;
	}

	activeDestinationLease_ = lease;
	adapter_.start(YouTubeStartRequest{lease, std::move(completion.serverUrl), std::move(completion.streamKey)});
}

void RuntimeController::onDestinationDispatchFailed(OutputLease lease) noexcept
{
	if (!pendingDestinationPreparation_.has_value() || pendingDestinationPreparation_->outputLease != lease) {
		return;
	}

	pendingDestinationPreparation_.reset();
	const SessionSnapshot current = coordinator_.snapshot();
	if (current.youtubeLease.has_value() && *current.youtubeLease == lease) {
		applyTransition(coordinator_.youtubeReleased(lease));
	}
	maybeShutdownAdapter();
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
	    pendingDestinationPreparation_.has_value() || coordinator_.snapshot().youtubeLease.has_value()) {
		return;
	}

	adapterShutdownCalled_ = true;
	adapter_.shutdown();
	maybeShutdownDestinationProvider();
}

void RuntimeController::maybeShutdownDestinationProvider() noexcept
{
	if (!exitSeen_ || destinationProviderShutdownCalled_ || !destinationProvider_ ||
	    pendingDestinationPreparation_.has_value() || activeDestinationLease_.has_value()) {
		return;
	}

	destinationProvider_->shutdown();
	destinationProviderShutdownCalled_ = true;
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

void RuntimeController::cancelPendingDestinationPreparation(OutputLease lease) noexcept
{
	if (!pendingDestinationPreparation_.has_value() || pendingDestinationPreparation_->outputLease != lease) {
		return;
	}

	if (destinationProvider_) {
		try {
			destinationProvider_->cancel(lease);
		} catch (...) {
			// A provider must implement cancel as a noexcept barrier.  Keep the
			// controller fail-closed if a test/dynamic implementation violates it.
		}
	}
	pendingDestinationPreparation_.reset();
}

void RuntimeController::releaseDestinationLease(OutputLease lease) noexcept
{
	if (!activeDestinationLease_.has_value() || *activeDestinationLease_ != lease) {
		return;
	}
	activeDestinationLease_.reset();
	if (destinationProvider_) {
		destinationProvider_->release(lease);
	}
}

bool RuntimeController::hasConfiguredYouTubeDestination(const RuntimeSettings &settings) const noexcept
{
	if (settings.youtubeConnectionMode == YouTubeConnectionMode::Manual) {
		return hasManualYouTubeDestination(settings);
	}
	return settings.youtubeConnectionMode == YouTubeConnectionMode::Account && destinationProvider_ != nullptr &&
	       settings.youtubeAccountDestinationAvailable;
}

} // namespace easy_multistream

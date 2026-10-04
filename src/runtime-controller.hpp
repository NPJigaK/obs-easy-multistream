// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"
#include "session-coordinator.hpp"
#include "youtube-account-selection.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace easy_multistream {

// This is deliberately a value-only input.  The plugin bridge obtains the
// provider from OBS and classifies it with classifyNativeDestination() before
// passing it here.  No OBS pointer is retained by RuntimeController.
struct RuntimeSettings {
	NativeDestination nativeDestination = NativeDestination::Unknown;
	bool youtubeEnabled = false;
	YouTubeConnectionMode youtubeConnectionMode = YouTubeConnectionMode::Manual;
	bool youtubeKeyAvailable = false;
	// Account mode is considered ready only when the concrete account bridge
	// says that a saved selection can be prepared.  This is deliberately a
	// value-only capability bit; RuntimeController never knows about OAuth,
	// profiles, or the account owner implementation.
	bool youtubeAccountDestinationAvailable = false;
};

// Account destinations remain dynamic and pass through this validator before
// reaching the output adapter.  The manual destination is a separate fixed
// invariant and is never accepted from RuntimeSettings.
bool isValidRtmpsUrl(std::string_view value) noexcept;

// RuntimeController's small port is intentionally named independently from
// any concrete adapter header.  A concrete adapter can translate its richer
// OBS signal set (Starting/Stopping/Failed, etc.) into these four state events.
enum class RuntimeOutputEventKind {
	Started,
	Reconnecting,
	ReconnectSucceeded,
	Released,
};

struct RuntimeOutputEvent {
	RuntimeOutputEventKind kind = RuntimeOutputEventKind::Released;
	OutputLease lease;
};

// The stream key is move-only and is zeroed by SecureBuffer on destruction.
// The adapter must consume or retain it only for the lifetime of its output
// lease; it must never put it into a snapshot, log, or profile configuration.
struct YouTubeStartRequest {
	OutputLease lease;
	std::string serverUrl;
	SecureBuffer streamKey;

	YouTubeStartRequest(OutputLease outputLease, std::string url, SecureBuffer key) noexcept
		: lease(outputLease), serverUrl(std::move(url)), streamKey(std::move(key))
	{
	}

	YouTubeStartRequest(const YouTubeStartRequest &) = delete;
	YouTubeStartRequest &operator=(const YouTubeStartRequest &) = delete;
	YouTubeStartRequest(YouTubeStartRequest &&) noexcept = default;
	YouTubeStartRequest &operator=(YouTubeStartRequest &&) noexcept = default;
};

// The concrete OBS output adapter is intentionally outside this slice.  Its
// implementation owns OBS output/service/encoder references and must emit
// Released only after the full teardown barrier in runtime-output-design.md.
// Calls are made on the RuntimeController owner thread; events may originate
// on OBS worker threads and are routed through the event sink's post function.
class IRuntimeYouTubeOutputAdapter {
public:
	using EventSink = std::function<void(RuntimeOutputEvent)>;

	virtual ~IRuntimeYouTubeOutputAdapter() = default;

	virtual void setEventSink(EventSink sink) noexcept = 0;
	virtual void start(YouTubeStartRequest request) noexcept = 0;
	virtual void requestStop(OutputLease lease) noexcept = 0;
	// shutdown() is called only after the controller has received the final
	// Released event for every lease.  It must make future callbacks inert.
	virtual void shutdown() noexcept = 0;
};

// Account destination preparation is an intentionally small runtime port.
// The implementation may use OAuth, a credential store, or another provider,
// but it must not make those details visible to RuntimeController.  A
// successful completion owns a destination-use lease until release() is
// called after the output adapter's Released barrier.
enum class RuntimeYouTubeDestinationStartStatus {
	Started,
	SetupRequired,
	Failed,
	Cancelled,
	Busy,
	Closed,
};

enum class RuntimeYouTubeDestinationCancelStatus {
	Cancelled,
	NoActiveAttempt,
	Closed,
	Failed,
};

enum class RuntimeYouTubeDestinationCompletionStatus {
	Success,
	SetupRequired,
	Failed,
	Cancelled,
};

struct RuntimeYouTubeDestinationCompletion final {
	OutputLease lease;
	RuntimeYouTubeDestinationCompletionStatus status = RuntimeYouTubeDestinationCompletionStatus::Failed;
	std::string serverUrl;
	SecureBuffer streamKey;

	RuntimeYouTubeDestinationCompletion() = default;
	RuntimeYouTubeDestinationCompletion(OutputLease outputLease,
						RuntimeYouTubeDestinationCompletionStatus completionStatus,
						std::string url = {}, SecureBuffer key = {}) noexcept
			: lease(outputLease), status(completionStatus), serverUrl(std::move(url)), streamKey(std::move(key))
	{
	}

	RuntimeYouTubeDestinationCompletion(const RuntimeYouTubeDestinationCompletion &) = delete;
	RuntimeYouTubeDestinationCompletion &operator=(const RuntimeYouTubeDestinationCompletion &) = delete;
	RuntimeYouTubeDestinationCompletion(RuntimeYouTubeDestinationCompletion &&) noexcept = default;
	RuntimeYouTubeDestinationCompletion &operator=(RuntimeYouTubeDestinationCompletion &&) noexcept = default;

	bool succeeded() const noexcept
	{
		return status == RuntimeYouTubeDestinationCompletionStatus::Success;
	}
};

class IRuntimeYouTubeDestinationProvider {
public:
	using CompletionHandler = std::function<void(RuntimeYouTubeDestinationCompletion)>;

	virtual ~IRuntimeYouTubeDestinationProvider() = default;

	// start() is called on the RuntimeController owner thread.  If Started is
	// returned, exactly one completion is expected on that same owner thread
	// (possibly synchronously).
	// cancel() is a synchronous barrier: after it returns, the provider must
	// not invoke the handler with usable secret material for that preparation.
	virtual RuntimeYouTubeDestinationStartStatus start(OutputLease lease,
								CompletionHandler completionHandler) noexcept = 0;
	virtual RuntimeYouTubeDestinationCancelStatus cancel(OutputLease lease) noexcept = 0;
	// release() relinquishes a successful destination-use lease.  It is called
	// only after the output adapter has emitted Released, or after adapter
	// shutdown has completed as a last-resort controller teardown path.
	virtual void release(OutputLease lease) noexcept = 0;
	virtual void shutdown() noexcept = 0;
};

class RuntimeController final {
public:
	// Every public event method is an owner-thread operation.  The post
	// callback is used only for adapter/OBS worker notifications and for the
	// guarded native-start reconciliation.  Production passes a queued UI
	// dispatcher (for example QMetaObject::invokeMethod); tests can provide a
	// deterministic queue.  An empty dispatcher runs inline and is suitable
	// only when the caller can prove that no queued ordering is required.
	// The dispatcher reports whether it accepted the callback.  A false/throw
	// result means the callback was not executed and will never run. A true
	// result guarantees exactly one invocation while its context remains alive;
	// context teardown is followed by RuntimeController destruction, whose
	// shutdown barrier reconciles unconsumed provider leases. Production uses a
	// queued UI dispatcher; tests use a deterministic queue.
	using Post = std::function<bool(std::function<void()>)>;
	using ReadYouTubeKey = std::function<SecureBuffer()>;
	using SnapshotSink = std::function<void(SessionSnapshot)>;

	RuntimeController(IRuntimeYouTubeOutputAdapter &adapter, ReadYouTubeKey readYouTubeKey, Post post = {},
				  IRuntimeYouTubeDestinationProvider *destinationProvider = nullptr);
	~RuntimeController();

	RuntimeController(const RuntimeController &) = delete;
	RuntimeController &operator=(const RuntimeController &) = delete;

	void setSnapshotSink(SnapshotSink sink);

	// The settings controller calls setSettings after a successful profile load
	// or after an enabled/destination change. A manual stream key is never passed
	// here; it is read just before its StartYouTube effect. Account-mode secrets
	// arrive only in the move-only destination completion.
	void setSettings(RuntimeSettings settings);

	// Profile transitions invalidate all old leases before accepting new values.
	void onProfileChanging();
	void onProfileChanged(RuntimeSettings settings);

	// Native frontend event bridge.  The overload taking a destination is the
	// preferred path: it lets the bridge classify the active OBS service at the
	// actual start boundary rather than trusting a stale profile cache.
	void onStreamingStarting();
	void onStreamingStarting(NativeDestination observedDestination);
	void onStreamingStarted();
	void onStreamingStarted(NativeLease lease);
	void onStreamingStopping();
	void onStreamingStopping(NativeLease lease);
	void onStreamingStopped();
	void onStreamingStopped(NativeLease lease);

	// The native output's synchronous "starting" signal is reported with the
	// NativeLease assigned by onStreamingStarting.  A queued reconciliation
	// treats absence of this marker as a synchronous native start rejection;
	// it never treats a slow network start as a failure.
	void onNativeOutputStarting(NativeLease lease);

	// Called on the owner thread by the adapter event sink.  The sink itself
	// posts worker-originated events through the guarded dispatcher installed in
	// the constructor.
	void onYouTubeOutputEvent(RuntimeOutputEvent event);
	void retryYouTube();

	// EXIT is terminal.  The controller requests secondary teardown, waits for
	// its Released event, then invokes adapter.shutdown().  It never calls an
	// OBS frontend start/stop API.
	void onExit();

	SessionSnapshot snapshot() const noexcept;
	bool exitSeen() const noexcept { return exitSeen_; }

private:
	struct CallbackState;

	void applyTransition(SessionTransition transition);
	void applyEffect(const SessionEffect &effect);
	void startYouTube(OutputLease lease);
	void onDestinationCompletion(RuntimeYouTubeDestinationCompletion completion) noexcept;
	void onDestinationDispatchFailed(OutputLease lease) noexcept;
	void drainPostedOutputEvents() noexcept;
	void reconcileNativeStart(NativeLease lease);
	void maybeShutdownAdapter() noexcept;
	void maybeShutdownDestinationProvider() noexcept;
	void publish(SessionSnapshot snapshot);
	void completeRejectedStart(OutputLease lease) noexcept;
	void clearNativeLeaseAfterStop(NativeLease lease) noexcept;
	void cancelPendingDestinationPreparation(OutputLease lease) noexcept;
	void releaseDestinationLease(OutputLease lease) noexcept;
	bool hasConfiguredYouTubeDestination(const RuntimeSettings &settings) const noexcept;

	IRuntimeYouTubeOutputAdapter &adapter_;
	ReadYouTubeKey readYouTubeKey_;
	Post post_;
	SnapshotSink snapshotSink_;
	IRuntimeYouTubeDestinationProvider *destinationProvider_ = nullptr;
	SessionCoordinator coordinator_;
	RuntimeSettings settings_;
	std::optional<NativeLease> activeNativeLease_;
	std::optional<NativeLease> stoppingNativeLease_;
	std::optional<NativeLease> pendingNativeStartCheck_;
	std::optional<OutputLease> pendingOutputStop_;
	struct PendingDestinationPreparation final {
		OutputLease outputLease;
		NativeLease nativeLease;
	};
	std::optional<PendingDestinationPreparation> pendingDestinationPreparation_;
	std::optional<OutputLease> activeDestinationLease_;
	std::shared_ptr<CallbackState> callbackState_;
	std::uint64_t publishedRevision_ = 0;
	bool nativeOutputStartingObserved_ = false;
	bool exitSeen_ = false;
	bool adapterShutdownCalled_ = false;
	bool destinationProviderShutdownCalled_ = false;
};

} // namespace easy_multistream

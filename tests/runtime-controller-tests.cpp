// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "runtime-controller.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                 \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using easy_multistream::IRuntimeYouTubeOutputAdapter;
using easy_multistream::IRuntimeYouTubeDestinationProvider;
using easy_multistream::NativeDestination;
using easy_multistream::NativeLease;
using easy_multistream::OutputLease;
using easy_multistream::RuntimeController;
using easy_multistream::RuntimeSettings;
using easy_multistream::SecureBuffer;
using easy_multistream::SessionPhase;
using easy_multistream::RuntimeOutputEventKind;
using easy_multistream::RuntimeYouTubeDestinationCancelStatus;
using easy_multistream::RuntimeYouTubeDestinationCompletion;
using easy_multistream::RuntimeYouTubeDestinationCompletionStatus;
using easy_multistream::RuntimeYouTubeDestinationStartStatus;
using easy_multistream::YouTubeConnectionMode;
using easy_multistream::YouTubeStreamState;

struct FakeAdapter final : IRuntimeYouTubeOutputAdapter {
	struct StartRecord {
		OutputLease lease;
		std::string serverUrl;
		std::size_t keySize = 0;
	};

	EventSink sink;
	std::vector<StartRecord> starts;
	std::vector<OutputLease> stops;
	bool shutdownCalled = false;

	void setEventSink(EventSink nextSink) noexcept override { sink = std::move(nextSink); }

	void start(easy_multistream::YouTubeStartRequest request) noexcept override
	{
		starts.push_back({request.lease, std::move(request.serverUrl), request.streamKey.size()});
		// The fake does not retain the key.  Destruction of request exercises the
		// same secure boundary used by the real adapter.
	}

	void requestStop(OutputLease lease) noexcept override { stops.push_back(lease); }

	void shutdown() noexcept override { shutdownCalled = true; }

	void emit(RuntimeOutputEventKind kind, OutputLease lease)
	{
		if (sink) {
			sink({kind, lease});
		}
	}
};

struct FakeDestinationProvider final : IRuntimeYouTubeDestinationProvider {
	struct StartRecord {
		OutputLease lease;
		CompletionHandler completion;
	};

	std::vector<StartRecord> starts;
	std::vector<OutputLease> cancels;
	std::vector<OutputLease> releases;
	RuntimeYouTubeDestinationStartStatus nextStartStatus = RuntimeYouTubeDestinationStartStatus::Started;
	bool completeSynchronously = false;
	bool shutdownCalled = false;

	RuntimeYouTubeDestinationStartStatus start(OutputLease lease, CompletionHandler completion) noexcept override
	{
		try {
			starts.push_back({lease, std::move(completion)});
		} catch (...) {
			return RuntimeYouTubeDestinationStartStatus::Failed;
		}
		if (completeSynchronously && starts.back().completion) {
			starts.back().completion(RuntimeYouTubeDestinationCompletion{
				lease, RuntimeYouTubeDestinationCompletionStatus::Success,
				"rtmps://a.rtmps.youtube.com/live2", SecureBuffer::copyOf("reentrant-account-key")});
		}
		return nextStartStatus;
	}

	RuntimeYouTubeDestinationCancelStatus cancel(OutputLease lease) noexcept override
	{
		cancels.push_back(lease);
		return RuntimeYouTubeDestinationCancelStatus::Cancelled;
	}

	void release(OutputLease lease) noexcept override { releases.push_back(lease); }

	void shutdown() noexcept override { shutdownCalled = true; }

	void complete(std::size_t index, RuntimeYouTubeDestinationCompletion completion)
	{
		if (index < starts.size() && starts[index].completion) {
			starts[index].completion(std::move(completion));
		}
	}
};

struct Harness {
	std::deque<std::function<void()>> queue;
	bool rejectNext = false;
	bool throwNext = false;

	bool post(std::function<void()> callback)
	{
		if (throwNext) {
			throwNext = false;
			throw std::runtime_error("dispatcher failure");
		}
		if (rejectNext) {
			rejectNext = false;
			return false;
		}
		queue.push_back(std::move(callback));
		return true;
	}

	void drain()
	{
		while (!queue.empty()) {
			auto callback = std::move(queue.front());
			queue.pop_front();
			callback();
		}
	}
};

RuntimeSettings twitchSettings(bool enabled = true, bool keyAvailable = true,
			       std::string server = "rtmps://a.rtmps.youtube.com/live2")
{
	RuntimeSettings settings;
	settings.nativeDestination = NativeDestination::Twitch;
	settings.youtubeEnabled = enabled;
	settings.youtubeKeyAvailable = keyAvailable;
	settings.youtubeServerUrl = std::move(server);
	return settings;
}

RuntimeSettings accountSettings(bool enabled = true, bool available = true)
{
	RuntimeSettings settings;
	settings.nativeDestination = NativeDestination::Twitch;
	settings.youtubeEnabled = enabled;
	settings.youtubeConnectionMode = YouTubeConnectionMode::Account;
	settings.youtubeAccountDestinationAvailable = available;
	return settings;
}

NativeLease nativeLease(const RuntimeController &controller)
{
	CHECK(controller.snapshot().nativeLease.has_value());
	return controller.snapshot().nativeLease.value_or(NativeLease{});
}

OutputLease startNativeAndSecondary(RuntimeController &controller, FakeAdapter &adapter, Harness &harness)
{
	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease lease = nativeLease(controller);
	controller.onNativeOutputStarting(lease);
	controller.onStreamingStarted();
	harness.drain();
	CHECK(adapter.starts.size() == 1);
	const OutputLease outputLease = adapter.starts.front().lease;
	adapter.emit(RuntimeOutputEventKind::Started, outputLease);
	harness.drain();
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Streaming);
	return outputLease;
}

void startNativeForAccount(RuntimeController &controller, FakeDestinationProvider &provider, Harness &harness)
{
	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease lease = nativeLease(controller);
	controller.onNativeOutputStarting(lease);
	controller.onStreamingStarted();
	harness.drain();
	CHECK(provider.starts.size() == 1);
}

RuntimeYouTubeDestinationCompletion successfulAccountCompletion(OutputLease lease,
									 std::string server = "rtmps://a.rtmps.youtube.com/live2",
									 std::string key = "account-stream-key")
{
	return RuntimeYouTubeDestinationCompletion{lease, RuntimeYouTubeDestinationCompletionStatus::Success,
									 std::move(server), SecureBuffer::copyOf(key)};
}

void testRtmpsValidation()
{
	using easy_multistream::isValidRtmpsUrl;

	CHECK(isValidRtmpsUrl("rtmps://a.rtmps.youtube.com/live2"));
	CHECK(isValidRtmpsUrl("rtmps://b.rtmps.youtube.com:443/live2"));
	CHECK(!isValidRtmpsUrl(""));
	CHECK(!isValidRtmpsUrl("rtmp://a.rtmps.youtube.com/live2"));
	CHECK(!isValidRtmpsUrl("https://a.rtmps.youtube.com/live2"));
	CHECK(!isValidRtmpsUrl("rtmps://"));
	CHECK(!isValidRtmpsUrl("rtmps://evil.example/live2"));
	CHECK(!isValidRtmpsUrl("rtmps://a.rtmps.youtube.com/live2?key=secret"));
	CHECK(!isValidRtmpsUrl("rtmps://a.rtmps.youtube.com/live2\n"));
}

void testStartBoundaryAndNativeFailureReconciliation()
{
	FakeAdapter adapter;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings());

	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease lease = nativeLease(controller);
	// STARTING without the synchronous native-output marker is treated as a
	// rejected native start only after the queued reconciliation runs.
	harness.drain();
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Stopped);
	CHECK(adapter.starts.empty());

	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease retryLease = nativeLease(controller);
	controller.onNativeOutputStarting(retryLease);
	controller.onStreamingStarted();
	harness.drain();
	CHECK(retryLease != lease);
	CHECK(adapter.starts.size() == 1);
	CHECK(adapter.starts.front().serverUrl == "rtmps://a.rtmps.youtube.com/live2");
	CHECK(adapter.starts.front().keySize == std::string("stream-key").size());
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Connecting);

	const OutputLease outputLease = adapter.starts.front().lease;
	adapter.emit(RuntimeOutputEventKind::Started, outputLease);
	harness.drain();
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Streaming);
}

void testSecondaryFailureDoesNotStopNative()
{
	FakeAdapter adapter;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings());
	const OutputLease lease = startNativeAndSecondary(controller, adapter, harness);

	adapter.emit(RuntimeOutputEventKind::Released, lease);
	harness.drain();
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Failed);
	CHECK(adapter.stops.empty());

	controller.retryYouTube();
	CHECK(adapter.starts.size() == 2);
	CHECK(adapter.starts.back().lease != lease);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Connecting);
	const OutputLease retryLease = adapter.starts.back().lease;
	adapter.emit(RuntimeOutputEventKind::Released, retryLease);
	harness.drain();
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Failed);

	controller.onStreamingStopping();
	controller.onStreamingStopped(nativeLease(controller));
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Stopped);
	CHECK(adapter.stops.empty());
	controller.onExit();
	CHECK(adapter.shutdownCalled);
}

void testReleaseBarrierBlocksRapidRestart()
{
	FakeAdapter adapter;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings());
	const OutputLease oldOutput = startNativeAndSecondary(controller, adapter, harness);
	const NativeLease oldNative = nativeLease(controller);

	controller.onStreamingStopping();
	CHECK(adapter.stops.size() == 1);
	CHECK(adapter.stops.front() == oldOutput);
	controller.onStreamingStopped(oldNative);

	// A new native stream can be live while the old YouTube output is still in
	// its teardown barrier, but no second output may be started yet.
	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease newNative = nativeLease(controller);
	controller.onNativeOutputStarting(newNative);
	controller.onStreamingStarted();
	// A late STOPPED notification for the previous native attempt must not
	// cancel the new attempt's start reconciliation or lease.
	controller.onStreamingStopped(oldNative);
	harness.drain();
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(adapter.starts.size() == 1);

	adapter.emit(RuntimeOutputEventKind::Released, oldOutput);
	harness.drain();
	CHECK(adapter.starts.size() == 2);
	CHECK(adapter.starts.back().lease != oldOutput);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Connecting);

	const auto revision = controller.snapshot().revision;
	adapter.emit(RuntimeOutputEventKind::Started, oldOutput);
	harness.drain();
	CHECK(controller.snapshot().revision == revision);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Connecting);

	controller.onExit();
	const OutputLease newOutput = adapter.starts.back().lease;
	CHECK(adapter.stops.size() == 2);
	CHECK(adapter.stops.back() == newOutput);
	adapter.emit(RuntimeOutputEventKind::Released, newOutput);
	harness.drain();
	CHECK(adapter.shutdownCalled);
}

void testProfileGenerationDropsOldCallbacks()
{
	FakeAdapter adapter;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings());
	const OutputLease oldOutput = startNativeAndSecondary(controller, adapter, harness);
	const auto oldGeneration = controller.snapshot().generation;

	controller.onProfileChanging();
	CHECK(controller.snapshot().phase == SessionPhase::ProfileChanging);
	CHECK(controller.snapshot().generation != oldGeneration);
	controller.onProfileChanged(twitchSettings());
	CHECK(controller.snapshot().phase == SessionPhase::Ready);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::NotStreaming);

	const auto revision = controller.snapshot().revision;
	adapter.emit(RuntimeOutputEventKind::Started, oldOutput);
	harness.drain();
	CHECK(controller.snapshot().revision == revision);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::NotStreaming);
	adapter.emit(RuntimeOutputEventKind::Released, oldOutput);
	harness.drain();
	CHECK(adapter.starts.size() == 1);

	controller.onExit();
	CHECK(adapter.shutdownCalled);
}

void testInvalidUrlAndCredentialReadFailureAreSecondaryOnly()
{
	FakeAdapter adapter;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer{}; },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings(true, true, "rtmp://not-rtmps.example/live"));
	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease lease = nativeLease(controller);
	controller.onNativeOutputStarting(lease);
	controller.onStreamingStarted();
	harness.drain();
	CHECK(adapter.starts.empty());
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::SetupRequired);

	controller.onExit();
	CHECK(adapter.shutdownCalled);
}

void testAccountModeCannotUseManualDestination()
{
	FakeAdapter adapter;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	RuntimeSettings settings = twitchSettings();
	settings.youtubeConnectionMode = YouTubeConnectionMode::Account;
	controller.setSettings(std::move(settings));

	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease lease = nativeLease(controller);
	controller.onNativeOutputStarting(lease);
	controller.onStreamingStarted();
	harness.drain();

	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::SetupRequired);
	CHECK(adapter.starts.empty());
	CHECK(manualKeyReads == 0);

	controller.onExit();
	CHECK(adapter.shutdownCalled);
}

void testSwitchingToAccountModeStopsManualOutputWithoutFallback()
{
	FakeAdapter adapter;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings());
	const OutputLease manualOutput = startNativeAndSecondary(controller, adapter, harness);
	CHECK(manualKeyReads == 1);

	RuntimeSettings accountSettings = twitchSettings();
	accountSettings.youtubeConnectionMode = YouTubeConnectionMode::Account;
	controller.setSettings(std::move(accountSettings));
	CHECK(adapter.stops.size() == 1);
	CHECK(adapter.stops.front() == manualOutput);

	adapter.emit(RuntimeOutputEventKind::Released, manualOutput);
	harness.drain();
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::SetupRequired);
	CHECK(adapter.starts.size() == 1);
	CHECK(manualKeyReads == 1);

	controller.retryYouTube();
	CHECK(adapter.starts.size() == 1);
	CHECK(manualKeyReads == 1);
	controller.onExit();
	CHECK(adapter.shutdownCalled);
}

void testAccountModeProfileChangeCannotRestartOldManualOutput()
{
	FakeAdapter adapter;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); });
	controller.setSettings(twitchSettings());
	const OutputLease oldOutput = startNativeAndSecondary(controller, adapter, harness);
	CHECK(manualKeyReads == 1);

	controller.onProfileChanging();
	CHECK(adapter.stops.size() == 1);
	CHECK(adapter.stops.front() == oldOutput);
	RuntimeSettings accountSettings = twitchSettings();
	accountSettings.youtubeConnectionMode = YouTubeConnectionMode::Account;
	controller.onProfileChanged(std::move(accountSettings));
	CHECK(controller.snapshot().youtube == YouTubeStreamState::SetupRequired);

	adapter.emit(RuntimeOutputEventKind::Started, oldOutput);
	adapter.emit(RuntimeOutputEventKind::Released, oldOutput);
	harness.drain();
	controller.retryYouTube();
	CHECK(adapter.starts.size() == 1);
	CHECK(manualKeyReads == 1);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::SetupRequired);

	controller.onExit();
	CHECK(adapter.shutdownCalled);
}

void testAccountDestinationSuccessUsesProviderAndReleasesAfterOutputBarrier()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);

	const OutputLease lease = provider.starts.front().lease;
	provider.complete(0, successfulAccountCompletion(lease));
	harness.drain();
	CHECK(adapter.starts.size() == 1);
	CHECK(adapter.starts.front().lease == lease);
	CHECK(adapter.starts.front().serverUrl == "rtmps://a.rtmps.youtube.com/live2");
	CHECK(adapter.starts.front().keySize == std::string("account-stream-key").size());
	CHECK(manualKeyReads == 0);
	CHECK(provider.releases.empty());

	adapter.emit(RuntimeOutputEventKind::Started, lease);
	harness.drain();
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Streaming);
	CHECK(provider.releases.empty());
	adapter.emit(RuntimeOutputEventKind::Released, lease);
	harness.drain();
	CHECK(provider.releases.size() == 1);
	CHECK(provider.releases.front() == lease);

	controller.onStreamingStopping();
	controller.onStreamingStopped(nativeLease(controller));
	controller.onExit();
	CHECK(adapter.shutdownCalled);
	CHECK(provider.shutdownCalled);
}

void testAccountDestinationReentrantAndDuplicateCompletion()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	provider.completeSynchronously = true;
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease lease = provider.starts.front().lease;
	CHECK(adapter.starts.size() == 1);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Connecting);

	// A duplicate success must not release the use lease or terminate the
	// output that already owns it.
	provider.complete(0, successfulAccountCompletion(lease));
	harness.drain();
	CHECK(adapter.starts.size() == 1);
	CHECK(provider.releases.empty());
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Connecting);

	adapter.emit(RuntimeOutputEventKind::Started, lease);
	harness.drain();
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Streaming);
	controller.onExit();
	CHECK(adapter.stops.size() == 1);
	adapter.emit(RuntimeOutputEventKind::Released, lease);
	harness.drain();
	CHECK(provider.releases.size() == 1);
	CHECK(provider.shutdownCalled);
}

void testAccountDestinationSetupRequiredAndFailureAreIsolated()
{
	{
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		RuntimeController controller(
			adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
		controller.setSettings(accountSettings());
		provider.nextStartStatus = RuntimeYouTubeDestinationStartStatus::SetupRequired;
		startNativeForAccount(controller, provider, harness);
		CHECK(controller.snapshot().youtube == YouTubeStreamState::SetupRequired);
		CHECK(adapter.starts.empty());
		CHECK(adapter.stops.empty());
		controller.onExit();
		CHECK(provider.shutdownCalled);
	}

	{
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		RuntimeController controller(
			adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
		controller.setSettings(accountSettings());
		startNativeForAccount(controller, provider, harness);
		const OutputLease lease = provider.starts.front().lease;
		provider.complete(0, {lease, RuntimeYouTubeDestinationCompletionStatus::Failed});
		harness.drain();
		CHECK(controller.snapshot().youtube == YouTubeStreamState::Failed);
		CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
		CHECK(adapter.starts.empty());
		CHECK(adapter.stops.empty());
		CHECK(provider.releases.empty());
		controller.onExit();
		CHECK(provider.shutdownCalled);
	}
}

void testAccountDestinationPendingStopCancelsAndDropsLateCompletion()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease lease = provider.starts.front().lease;
	const NativeLease native = nativeLease(controller);

	controller.onStreamingStopping();
	CHECK(provider.cancels.size() == 1);
	CHECK(provider.cancels.front() == lease);
	CHECK(adapter.stops.empty());
	CHECK(controller.snapshot().youtube == YouTubeStreamState::NotStreaming);
	controller.onStreamingStopped(native);

	provider.complete(0, successfulAccountCompletion(lease));
	harness.drain();
	CHECK(adapter.starts.empty());
	CHECK(provider.releases.size() == 1);
	CHECK(provider.releases.front() == lease);
	controller.onExit();
	CHECK(adapter.shutdownCalled);
	CHECK(provider.shutdownCalled);
}

void testAccountDestinationRejectedCompletionDispatchReleasesImmediately()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease lease = provider.starts.front().lease;
	harness.rejectNext = true;
	provider.complete(0, successfulAccountCompletion(lease));
	CHECK(adapter.starts.empty());
	CHECK(provider.releases.size() == 1);
	CHECK(provider.releases.front() == lease);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Failed);
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	controller.retryYouTube();
	CHECK(provider.starts.size() == 2);
	controller.onExit();
	CHECK(provider.cancels.size() == 1);
	CHECK(provider.shutdownCalled);
}

void testAccountDestinationThrowingCompletionDispatchTerminatesPreparation()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease lease = provider.starts.front().lease;
	harness.throwNext = true;
	provider.complete(0, successfulAccountCompletion(lease));
	CHECK(adapter.starts.empty());
	CHECK(provider.releases.size() == 1);
	CHECK(provider.releases.front() == lease);
	CHECK(controller.snapshot().youtube == YouTubeStreamState::Failed);
	CHECK(controller.snapshot().native == easy_multistream::NativeStreamState::Streaming);
	controller.onExit();
	CHECK(provider.shutdownCalled);
}

void testAccountDestinationProfileAndExitCancelPending()
{
	{
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		RuntimeController controller(
			adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
		controller.setSettings(accountSettings());
		startNativeForAccount(controller, provider, harness);
		const OutputLease lease = provider.starts.front().lease;
		controller.onProfileChanging();
		CHECK(provider.cancels.size() == 1);
		CHECK(provider.cancels.front() == lease);
		CHECK(adapter.stops.empty());
		controller.onProfileChanged(accountSettings());
		provider.complete(0, successfulAccountCompletion(lease));
		harness.drain();
		CHECK(adapter.starts.empty());
		CHECK(provider.releases.size() == 1);
		CHECK(controller.snapshot().youtube == YouTubeStreamState::NotStreaming);
		controller.onExit();
	}

	{
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		RuntimeController controller(
			adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
			[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
		controller.setSettings(accountSettings());
		startNativeForAccount(controller, provider, harness);
		const OutputLease lease = provider.starts.front().lease;
		controller.onExit();
		CHECK(provider.cancels.size() == 1);
		CHECK(provider.cancels.front() == lease);
		CHECK(adapter.stops.empty());
		CHECK(adapter.shutdownCalled);
		CHECK(provider.shutdownCalled);
		provider.complete(0, successfulAccountCompletion(lease));
		harness.drain();
		CHECK(adapter.starts.empty());
		CHECK(provider.releases.size() == 1);
	}
}

void testRejectedAdapterReleaseIsDrainedByNextOwnerAction()
{
	for (const bool throwDispatcher : {false, true}) {
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		RuntimeController controller(
			adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
			[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
		controller.setSettings(accountSettings());
		startNativeForAccount(controller, provider, harness);
		const OutputLease lease = provider.starts.front().lease;
		provider.complete(0, successfulAccountCompletion(lease));
		harness.drain();
		CHECK(adapter.starts.size() == 1);

		if (throwDispatcher) {
			harness.throwNext = true;
		} else {
			harness.rejectNext = true;
		}
		adapter.emit(RuntimeOutputEventKind::Released, lease);
		CHECK(provider.releases.empty());

		// The next owner-thread operation drains the event mailbox before it
		// acts, so a rejected wake-up cannot strand the Released barrier.
		controller.retryYouTube();
		CHECK(provider.releases.size() == 1);
		CHECK(provider.releases.front() == lease);
		CHECK(provider.starts.size() == 2);
		controller.onExit();
		CHECK(provider.cancels.size() == 1);
		CHECK(provider.shutdownCalled);
	}
}

void testExitBoundaryDrainsReleasedWithoutRestartingAnotherMode()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease lease = provider.starts.front().lease;
	provider.complete(0, successfulAccountCompletion(lease));
	harness.drain();
	CHECK(adapter.starts.size() == 1);

	RuntimeSettings manual = twitchSettings();
	manual.youtubeConnectionMode = YouTubeConnectionMode::Manual;
	controller.setSettings(std::move(manual));
	CHECK(adapter.stops.size() == 1);
	CHECK(adapter.stops.front() == lease);

	// Keep Released in the mailbox. EXIT must become authoritative before the
	// mailbox is drained, or the stop completion could start the new mode while
	// teardown is already in progress.
	adapter.emit(RuntimeOutputEventKind::Released, lease);
	CHECK(provider.releases.empty());
	controller.onExit();
	CHECK(adapter.starts.size() == 1);
	CHECK(manualKeyReads == 0);
	CHECK(provider.releases.size() == 1);
	CHECK(provider.releases.front() == lease);
	CHECK(adapter.shutdownCalled);
	CHECK(provider.shutdownCalled);
	harness.drain();
	CHECK(adapter.starts.size() == 1);
}

void testAccountManualModeSwitchUsesReleaseBarrier()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(twitchSettings());
	const OutputLease manualLease = startNativeAndSecondary(controller, adapter, harness);
	RuntimeSettings account = accountSettings();
	controller.setSettings(account);
	CHECK(adapter.stops.size() == 1);
	CHECK(adapter.stops.front() == manualLease);
	CHECK(provider.starts.empty());
	adapter.emit(RuntimeOutputEventKind::Released, manualLease);
	harness.drain();
	CHECK(provider.starts.size() == 1);
	CHECK(adapter.starts.size() == 1);
	const OutputLease accountLease = provider.starts.front().lease;
	provider.complete(0, successfulAccountCompletion(accountLease));
	harness.drain();
	CHECK(adapter.starts.size() == 2);
	CHECK(manualKeyReads == 1);
	CHECK(adapter.starts.back().lease == accountLease);
	controller.onExit();
	CHECK(adapter.stops.size() == 2);
	adapter.emit(RuntimeOutputEventKind::Released, accountLease);
	harness.drain();
	CHECK(provider.releases.size() == 1);
	CHECK(provider.shutdownCalled);
}

void testAccountRapidRestartWaitsForOutputRelease()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease oldLease = provider.starts.front().lease;
	provider.complete(0, successfulAccountCompletion(oldLease));
	harness.drain();
	adapter.emit(RuntimeOutputEventKind::Started, oldLease);
	harness.drain();
	const NativeLease oldNative = nativeLease(controller);

	controller.onStreamingStopping();
	controller.onStreamingStopped(oldNative);
	controller.onStreamingStarting(NativeDestination::Twitch);
	const NativeLease newNative = nativeLease(controller);
	controller.onNativeOutputStarting(newNative);
	controller.onStreamingStarted();
	harness.drain();
	CHECK(adapter.starts.size() == 1);
	CHECK(provider.starts.size() == 1);

	adapter.emit(RuntimeOutputEventKind::Released, oldLease);
	harness.drain();
	CHECK(provider.releases.size() == 1);
	CHECK(provider.releases.front() == oldLease);
	CHECK(provider.starts.size() == 2);
	const OutputLease newLease = provider.starts.back().lease;
	provider.complete(1, successfulAccountCompletion(newLease));
	harness.drain();
	CHECK(adapter.starts.size() == 2);

	controller.onExit();
	CHECK(adapter.stops.size() == 2);
	adapter.emit(RuntimeOutputEventKind::Released, newLease);
	harness.drain();
	CHECK(provider.releases.size() == 2);
	CHECK(provider.shutdownCalled);
}

void testAccountToManualModeSwitchUsesReleaseBarrier()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	int manualKeyReads = 0;
	RuntimeController controller(
		adapter,
		[&manualKeyReads] {
			++manualKeyReads;
			return SecureBuffer::copyOf("manual-stream-key");
		},
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease accountLease = provider.starts.front().lease;
	provider.complete(0, successfulAccountCompletion(accountLease));
	harness.drain();
	adapter.emit(RuntimeOutputEventKind::Started, accountLease);
	harness.drain();

	controller.setSettings(twitchSettings());
	CHECK(adapter.stops.size() == 1);
	CHECK(adapter.stops.front() == accountLease);
	CHECK(manualKeyReads == 0);
	adapter.emit(RuntimeOutputEventKind::Released, accountLease);
	harness.drain();
	CHECK(provider.releases.size() == 1);
	CHECK(adapter.starts.size() == 2);
	CHECK(manualKeyReads == 1);
	CHECK(adapter.starts.back().lease != accountLease);
	const OutputLease manualLease = adapter.starts.back().lease;
	adapter.emit(RuntimeOutputEventKind::Started, manualLease);
	harness.drain();
	controller.onExit();
	CHECK(adapter.stops.size() == 2);
	adapter.emit(RuntimeOutputEventKind::Released, manualLease);
	harness.drain();
	CHECK(provider.shutdownCalled);
}

void testAccountCompletionNeverExposesSecretInSnapshot()
{
	FakeAdapter adapter;
	FakeDestinationProvider provider;
	Harness harness;
	RuntimeController controller(
		adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
		[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
	controller.setSettings(accountSettings());
	startNativeForAccount(controller, provider, harness);
	const OutputLease lease = provider.starts.front().lease;
	provider.complete(0, successfulAccountCompletion(lease, "rtmps://a.rtmps.youtube.com/live2", "secret-key"));
	harness.drain();
	const auto snapshot = controller.snapshot();
	CHECK(snapshot.youtubeLease.has_value());
	CHECK(snapshot.youtube == YouTubeStreamState::Connecting);
	// SessionSnapshot contains only leases/status booleans; URL/key never enter
	// this value-only state boundary.
	controller.onExit();
	adapter.emit(RuntimeOutputEventKind::Released, lease);
	harness.drain();
}

void testAccountDestructorClosesPendingAndActiveLeases()
{
	{
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		{
			RuntimeController controller(
				adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
			[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
			controller.setSettings(accountSettings());
			startNativeForAccount(controller, provider, harness);
			provider.complete(0, successfulAccountCompletion(provider.starts.front().lease));
			// The completion is queued, then the controller is destroyed. The
			// queued callback must not dereference a destroyed provider bridge.
		}
		harness.drain();
		CHECK(provider.cancels.size() == 1);
		CHECK(adapter.stops.empty());
		CHECK(adapter.shutdownCalled);
		CHECK(provider.shutdownCalled);
		CHECK(provider.releases.empty());
	}

	{
		FakeAdapter adapter;
		FakeDestinationProvider provider;
		Harness harness;
		{
			RuntimeController controller(
				adapter, [] { return SecureBuffer::copyOf("manual-stream-key"); },
			[&harness](std::function<void()> callback) { return harness.post(std::move(callback)); }, &provider);
			controller.setSettings(accountSettings());
			startNativeForAccount(controller, provider, harness);
			const OutputLease lease = provider.starts.front().lease;
			provider.complete(0, successfulAccountCompletion(lease));
			harness.drain();
			CHECK(adapter.starts.size() == 1);
		}
		CHECK(adapter.stops.size() == 1);
		CHECK(adapter.shutdownCalled);
		CHECK(provider.releases.size() == 1);
		CHECK(provider.shutdownCalled);
	}
}

} // namespace

int main()
{
	testRtmpsValidation();
	testStartBoundaryAndNativeFailureReconciliation();
	testSecondaryFailureDoesNotStopNative();
	testReleaseBarrierBlocksRapidRestart();
	testProfileGenerationDropsOldCallbacks();
	testInvalidUrlAndCredentialReadFailureAreSecondaryOnly();
	testAccountModeCannotUseManualDestination();
	testSwitchingToAccountModeStopsManualOutputWithoutFallback();
	testAccountModeProfileChangeCannotRestartOldManualOutput();
	testAccountDestinationSuccessUsesProviderAndReleasesAfterOutputBarrier();
	testAccountDestinationReentrantAndDuplicateCompletion();
	testAccountDestinationSetupRequiredAndFailureAreIsolated();
	testAccountDestinationPendingStopCancelsAndDropsLateCompletion();
	testAccountDestinationRejectedCompletionDispatchReleasesImmediately();
	testAccountDestinationThrowingCompletionDispatchTerminatesPreparation();
	testAccountDestinationProfileAndExitCancelPending();
	testRejectedAdapterReleaseIsDrainedByNextOwnerAction();
	testExitBoundaryDrainsReleasedWithoutRestartingAnotherMode();
	testAccountManualModeSwitchUsesReleaseBarrier();
	testAccountRapidRestartWaitsForOutputRelease();
	testAccountToManualModeSwitchUsesReleaseBarrier();
	testAccountCompletionNeverExposesSecretInSnapshot();
	testAccountDestructorClosesPendingAndActiveLeases();

	if (failures != 0) {
		std::cerr << failures << " runtime-controller test(s) failed\n";
		return 1;
	}
	std::cout << "All runtime-controller tests passed\n";
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "runtime-controller.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
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
using easy_multistream::NativeDestination;
using easy_multistream::NativeLease;
using easy_multistream::OutputLease;
using easy_multistream::RuntimeController;
using easy_multistream::RuntimeSettings;
using easy_multistream::SecureBuffer;
using easy_multistream::SessionPhase;
using easy_multistream::RuntimeOutputEventKind;
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

struct Harness {
	std::deque<std::function<void()>> queue;

	void post(std::function<void()> callback) { queue.push_back(std::move(callback)); }

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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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
		[&harness](std::function<void()> callback) { harness.post(std::move(callback)); });
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

	if (failures != 0) {
		std::cerr << failures << " runtime-controller test(s) failed\n";
		return 1;
	}
	std::cout << "All runtime-controller tests passed\n";
	return 0;
}

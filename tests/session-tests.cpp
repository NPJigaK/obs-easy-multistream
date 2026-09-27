// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "session-coordinator.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                           \
		if (!(expression)) {                                                                                     \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';             \
			++failures;                                                                                         \
		}                                                                                                          \
	} while (false)

using easy_multistream::NativeDestination;
using easy_multistream::NativeLease;
using easy_multistream::NativeStreamState;
using easy_multistream::OutputLease;
using easy_multistream::SessionCoordinator;
using easy_multistream::SessionEffectKind;
using easy_multistream::SessionPhase;
using easy_multistream::SessionTransition;
using easy_multistream::YouTubeStreamState;

void checkNoEffect(const SessionTransition &transition)
{
	CHECK(!transition.effect.has_value());
}

OutputLease checkEffect(const SessionTransition &transition, SessionEffectKind kind)
{
	CHECK(transition.effect.has_value());
	if (!transition.effect.has_value()) {
		return {};
	}
	CHECK(transition.effect->kind == kind);
	return transition.effect->lease;
}

NativeLease currentNativeLease(const SessionCoordinator &coordinator)
{
	CHECK(coordinator.snapshot().nativeLease.has_value());
	return coordinator.snapshot().nativeLease.value_or(NativeLease{});
}

NativeLease startNative(SessionCoordinator &coordinator)
{
	const auto starting = coordinator.nativeStarting();
	checkNoEffect(starting);
	CHECK(starting.snapshot.native == NativeStreamState::Starting);
	return currentNativeLease(coordinator);
}

void configureTwitch(SessionCoordinator &coordinator, bool enabled = true, bool keyAvailable = true)
{
	const auto transition = coordinator.configure(NativeDestination::Twitch, enabled, keyAvailable);
	CHECK(transition.snapshot.nativeDestination == NativeDestination::Twitch);
	CHECK(transition.snapshot.youtubeEnabled == enabled);
	CHECK(transition.snapshot.youtubeKeyAvailable == keyAvailable);
}

OutputLease startTwitchYouTube(SessionCoordinator &coordinator)
{
	configureTwitch(coordinator);
	const NativeLease nativeLease = startNative(coordinator);
	const auto started = coordinator.nativeStarted(nativeLease);
	return checkEffect(started, SessionEffectKind::StartYouTube);
}

void testNativeClassification()
{
	using easy_multistream::classifyNativeDestination;

	CHECK(classifyNativeDestination("rtmp_common", "Twitch") == NativeDestination::Twitch);
	CHECK(classifyNativeDestination("rtmp_common", "YouTube - HLS") == NativeDestination::YouTube);
	CHECK(classifyNativeDestination("rtmp_common", "YouTube - RTMPS") == NativeDestination::YouTube);
	CHECK(classifyNativeDestination("rtmp_common", "YouTube / YouTube Gaming") == NativeDestination::YouTube);

	// OBS's current service list keeps these names as compatibility aliases.
	CHECK(classifyNativeDestination("rtmp_common", "YouTube - RTMP") == NativeDestination::YouTube);
	CHECK(classifyNativeDestination("rtmp_common", "YouTube - RTMPS (Beta)") == NativeDestination::YouTube);

	// A custom/unknown service or a provider name that merely contains a known name
	// must never be classified as a supported native destination.
	CHECK(classifyNativeDestination("rtmp_custom", "Twitch") == NativeDestination::Unknown);
	CHECK(classifyNativeDestination("rtmp_common", "Twitch Custom") == NativeDestination::Unknown);
	CHECK(classifyNativeDestination("rtmp_common", "My YouTube relay") == NativeDestination::Unknown);
	CHECK(classifyNativeDestination("rtmp_common_extra", "YouTube - RTMPS") == NativeDestination::Unknown);
	CHECK(classifyNativeDestination("", "Twitch") == NativeDestination::Unknown);
}

void testUnknownAndYouTubeNativeDoNotStartSecondary()
{
	SessionCoordinator unknown;
	configureTwitch(unknown, true, true);
	const auto unknownConfiguration = unknown.profileChanged(NativeDestination::Unknown, true, true);
	checkNoEffect(unknownConfiguration);
	const NativeLease unknownLease = startNative(unknown);
	const auto unknownStarted = unknown.nativeStarted(unknownLease);
	checkNoEffect(unknownStarted);
	CHECK(unknown.snapshot().nativeDestination == NativeDestination::Unknown);
	CHECK(unknown.snapshot().native == NativeStreamState::Streaming);
	CHECK(unknown.snapshot().youtube == YouTubeStreamState::NotStreaming);

	SessionCoordinator youtube;
	configureTwitch(youtube, true, true);
	const auto youtubeConfiguration = youtube.profileChanged(NativeDestination::YouTube, true, true);
	checkNoEffect(youtubeConfiguration);
	const NativeLease youtubeLease = startNative(youtube);
	const auto youtubeStarted = youtube.nativeStarted(youtubeLease);
	checkNoEffect(youtubeStarted);
	CHECK(youtube.snapshot().nativeDestination == NativeDestination::YouTube);
	CHECK(youtube.snapshot().native == NativeStreamState::Streaming);
	CHECK(youtube.snapshot().youtube == YouTubeStreamState::NotStreaming);
}

void testTwitchStartAndDuplicateStart()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);
	CHECK(lease.generation == coordinator.snapshot().generation);
	CHECK(lease.attempt != 0);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Connecting);

	const auto connected = coordinator.youtubeStarted(lease);
	checkNoEffect(connected);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Streaming);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.nativeStarted(currentNativeLease(coordinator)));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Streaming);
}

void testNativeStartFailureAndRetry()
{
	SessionCoordinator coordinator;
	configureTwitch(coordinator);
	const NativeLease failedLease = startNative(coordinator);

	const auto failed = coordinator.nativeStartFailed(failedLease);
	checkNoEffect(failed);
	CHECK(failed.snapshot.native == NativeStreamState::Stopped);
	CHECK(!failed.snapshot.nativeLease.has_value());
	CHECK(failed.snapshot.youtube == YouTubeStreamState::NotStreaming);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.nativeStarted(failedLease));
	checkNoEffect(coordinator.nativeStartFailed(failedLease));
	CHECK(coordinator.snapshot().revision == revision);

	const NativeLease retryLease = startNative(coordinator);
	CHECK(retryLease != failedLease);
	const auto retried = coordinator.nativeStarted(retryLease);
	checkEffect(retried, SessionEffectKind::StartYouTube);
	CHECK(retried.snapshot.native == NativeStreamState::Streaming);
	CHECK(retried.snapshot.youtube == YouTubeStreamState::Connecting);
}

void testDisabledAndMissingKeyNeverStart()
{
	SessionCoordinator coordinator;
	configureTwitch(coordinator, false, true);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Disabled);
	const NativeLease nativeLease = startNative(coordinator);
	checkNoEffect(coordinator.nativeStarted(nativeLease));
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Disabled);

	const auto missingKey = coordinator.configure(NativeDestination::Twitch, true, false);
	checkNoEffect(missingKey);
	CHECK(missingKey.snapshot.youtube == YouTubeStreamState::SetupRequired);

	const auto configured = coordinator.configure(NativeDestination::Twitch, true, true);
	checkEffect(configured, SessionEffectKind::StartYouTube);
	CHECK(configured.snapshot.youtube == YouTubeStreamState::Connecting);
}

void testConfigurationChangeStopsOnlyYouTube()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);
	checkNoEffect(coordinator.youtubeStarted(lease));

	const auto disabled = coordinator.configure(NativeDestination::Twitch, false, true);
	CHECK(checkEffect(disabled, SessionEffectKind::StopYouTube) == lease);
	CHECK(disabled.snapshot.native == NativeStreamState::Streaming);
	CHECK(disabled.snapshot.youtube == YouTubeStreamState::Stopping);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.configure(NativeDestination::Twitch, false, true));
	CHECK(coordinator.snapshot().revision == revision);

	const auto stopped = coordinator.youtubeReleased(lease);
	checkNoEffect(stopped);
	CHECK(stopped.snapshot.native == NativeStreamState::Streaming);
	CHECK(stopped.snapshot.youtube == YouTubeStreamState::Disabled);
}

void testDuplicateStopAndRapidRestart()
{
	SessionCoordinator coordinator;
	const OutputLease firstLease = startTwitchYouTube(coordinator);
	const NativeLease firstNativeLease = currentNativeLease(coordinator);
	checkNoEffect(coordinator.youtubeStarted(firstLease));

	const auto stopping = coordinator.nativeStopping(firstNativeLease);
	const OutputLease stopLease = checkEffect(stopping, SessionEffectKind::StopYouTube);
	CHECK(stopLease == firstLease);
	CHECK(coordinator.snapshot().native == NativeStreamState::Stopping);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Stopping);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.nativeStopping(firstNativeLease));
	CHECK(coordinator.snapshot().revision == revision);

	// A native restart can race the old YouTube stop acknowledgement. It must not
	// start a second output until the old lease reports stopped.
	checkNoEffect(coordinator.nativeStopped(firstNativeLease));
	const NativeLease secondNativeLease = startNative(coordinator);
	checkNoEffect(coordinator.nativeStarted(secondNativeLease));
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Stopping);

	const auto restarted = coordinator.youtubeReleased(stopLease);
	const OutputLease secondLease = checkEffect(restarted, SessionEffectKind::StartYouTube);
	CHECK(secondLease.generation == firstLease.generation);
	CHECK(secondLease.attempt != firstLease.attempt);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Connecting);

	// Completion for the old lease after the restart must not change the new
	// connection state.
	const std::uint64_t restartRevision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.youtubeStarted(firstLease));
	CHECK(coordinator.snapshot().revision == restartRevision);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Connecting);

	checkNoEffect(coordinator.youtubeStarted(secondLease));
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Streaming);
}

void testStaleNativeStopCannotStopRestartedSession()
{
	SessionCoordinator coordinator;
	const OutputLease firstYouTubeLease = startTwitchYouTube(coordinator);
	const NativeLease firstNativeLease = currentNativeLease(coordinator);
	checkNoEffect(coordinator.youtubeStarted(firstYouTubeLease));

	const auto stopping = coordinator.nativeStopping(firstNativeLease);
	CHECK(checkEffect(stopping, SessionEffectKind::StopYouTube) == firstYouTubeLease);

	// Model a new native attempt beginning before the old native Stopped event
	// has been delivered.
	const NativeLease secondNativeLease = startNative(coordinator);
	checkNoEffect(coordinator.nativeStarted(secondNativeLease));
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);

	const auto released = coordinator.youtubeReleased(firstYouTubeLease);
	const OutputLease secondYouTubeLease = checkEffect(released, SessionEffectKind::StartYouTube);
	CHECK(secondYouTubeLease != firstYouTubeLease);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Connecting);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.nativeStopped(firstNativeLease));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Connecting);

	checkNoEffect(coordinator.youtubeStarted(secondYouTubeLease));
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Streaming);
}

void testSecondaryFailureIsolatedFromNativeOutput()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);

	// A synchronously rejected start still reports the same full-release terminal
	// event as every other output outcome.
	const auto failed = coordinator.youtubeReleased(lease);
	checkNoEffect(failed);
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Failed);
	CHECK(!coordinator.snapshot().youtubeLease.has_value());

	// The old callback cannot resurrect an output after the failed attempt.
	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.youtubeStarted(lease));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Failed);
}

void testUnexpectedYouTubeStopDoesNotLoop()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);
	checkNoEffect(coordinator.youtubeStarted(lease));

	const auto stopped = coordinator.youtubeReleased(lease);
	checkNoEffect(stopped);
	CHECK(stopped.snapshot.native == NativeStreamState::Streaming);
	CHECK(stopped.snapshot.youtube == YouTubeStreamState::Failed);
	CHECK(!stopped.snapshot.youtubeLease.has_value());

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.nativeStarted(currentNativeLease(coordinator)));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Failed);
}

void testLateStartCannotEscapeStopping()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);
	const NativeLease nativeLease = currentNativeLease(coordinator);

	const auto stopping = coordinator.nativeStopping(nativeLease);
	CHECK(checkEffect(stopping, SessionEffectKind::StopYouTube) == lease);
	CHECK(stopping.snapshot.youtube == YouTubeStreamState::Stopping);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.youtubeStarted(lease));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().native == NativeStreamState::Stopping);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Stopping);
}

void testStartFailureCompletesARequestedStop()
{
	SessionCoordinator coordinator;
	const OutputLease firstYouTubeLease = startTwitchYouTube(coordinator);
	const NativeLease firstNativeLease = currentNativeLease(coordinator);

	const auto stopping = coordinator.nativeStopping(firstNativeLease);
	CHECK(checkEffect(stopping, SessionEffectKind::StopYouTube) == firstYouTubeLease);
	checkNoEffect(coordinator.nativeStopped(firstNativeLease));

	// A new native attempt can become live while the first YouTube output is
	// still cleaning up. A terminal start failure owns no resources, so it also
	// completes the requested stop and permits exactly one fresh start.
	const NativeLease secondNativeLease = startNative(coordinator);
	checkNoEffect(coordinator.nativeStarted(secondNativeLease));
	const auto failed = coordinator.youtubeReleased(firstYouTubeLease);
	const OutputLease secondYouTubeLease = checkEffect(failed, SessionEffectKind::StartYouTube);
	CHECK(secondYouTubeLease != firstYouTubeLease);
	CHECK(failed.snapshot.native == NativeStreamState::Streaming);
	CHECK(failed.snapshot.youtube == YouTubeStreamState::Connecting);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.youtubeReleased(firstYouTubeLease));
	CHECK(coordinator.snapshot().revision == revision);
}

void testReconnectAndStaleLease()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);
	checkNoEffect(coordinator.youtubeStarted(lease));

	checkNoEffect(coordinator.youtubeReconnecting(lease));
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Reconnecting);
	checkNoEffect(coordinator.youtubeReconnectSucceeded(lease));
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Streaming);

	// A different generation/attempt cannot mutate the active stream.
	const OutputLease staleGeneration{lease.generation + 1, lease.attempt};
	const OutputLease staleAttempt{lease.generation, lease.attempt + 1};
	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.youtubeReconnecting(staleGeneration));
	checkNoEffect(coordinator.youtubeReconnectSucceeded(staleAttempt));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Streaming);
}

void testProfileGenerationInvalidatesPendingCallbacks()
{
	SessionCoordinator coordinator;
	const OutputLease oldLease = startTwitchYouTube(coordinator);
	const std::uint64_t oldGeneration = coordinator.snapshot().generation;

	const auto changing = coordinator.profileChanging();
	const OutputLease stopLease = checkEffect(changing, SessionEffectKind::StopYouTube);
	CHECK(stopLease == oldLease);
	CHECK(changing.snapshot.generation != oldGeneration);
	CHECK(changing.snapshot.phase == SessionPhase::ProfileChanging);
	CHECK(changing.snapshot.nativeDestination == NativeDestination::Unknown);
	CHECK(changing.snapshot.youtube == YouTubeStreamState::Disabled);
	CHECK(!changing.snapshot.youtubeLease.has_value());

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.youtubeStarted(oldLease));
	checkNoEffect(coordinator.youtubeReconnecting(oldLease));
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().phase == SessionPhase::ProfileChanging);

	const auto changed = coordinator.profileChanged(NativeDestination::Twitch, true, true);
	checkNoEffect(changed);
	CHECK(changed.snapshot.phase == SessionPhase::Ready);
	CHECK(changed.snapshot.generation != oldGeneration);
	CHECK(changed.snapshot.native == NativeStreamState::Stopped);
	CHECK(changed.snapshot.youtube == YouTubeStreamState::NotStreaming);

	const NativeLease newNativeLease = startNative(coordinator);
	const auto started = coordinator.nativeStarted(newNativeLease);
	checkNoEffect(started);
	CHECK(started.snapshot.youtube == YouTubeStreamState::NotStreaming);

	const auto released = coordinator.youtubeReleased(oldLease);
	const OutputLease newLease = checkEffect(released, SessionEffectKind::StartYouTube);
	CHECK(newLease.generation == coordinator.snapshot().generation);
	CHECK(newLease.generation != oldLease.generation);
	CHECK(newLease.attempt != 0);
}

void testProfileChangeWaitsForOldYouTubeRelease()
{
	SessionCoordinator coordinator;
	const OutputLease oldYouTubeLease = startTwitchYouTube(coordinator);
	const NativeLease oldNativeLease = currentNativeLease(coordinator);
	checkNoEffect(coordinator.youtubeStarted(oldYouTubeLease));

	const auto changing = coordinator.profileChanging();
	CHECK(checkEffect(changing, SessionEffectKind::StopYouTube) == oldYouTubeLease);
	const auto changed = coordinator.profileChanged(NativeDestination::Twitch, true, true);
	checkNoEffect(changed);

	// A delayed native event from the old profile must not create or stop a new
	// YouTube output.
	const std::uint64_t changedRevision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.nativeStarted(oldNativeLease));
	checkNoEffect(coordinator.nativeStopped(oldNativeLease));
	CHECK(coordinator.snapshot().revision == changedRevision);

	const NativeLease newNativeLease = startNative(coordinator);
	checkNoEffect(coordinator.nativeStarted(newNativeLease));
	CHECK(coordinator.snapshot().native == NativeStreamState::Streaming);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::NotStreaming);
	CHECK(!coordinator.snapshot().youtubeLease.has_value());

	// Only adapter-level full release of the old output removes the teardown
	// barrier and permits a new start.
	const auto oldReleased = coordinator.youtubeReleased(oldYouTubeLease);
	const OutputLease newYouTubeLease = checkEffect(oldReleased, SessionEffectKind::StartYouTube);
	CHECK(newYouTubeLease.generation == coordinator.snapshot().generation);
	CHECK(newYouTubeLease.generation != oldYouTubeLease.generation);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Connecting);
}

void testExitIsTerminal()
{
	SessionCoordinator coordinator;
	const OutputLease lease = startTwitchYouTube(coordinator);
	const std::uint64_t oldGeneration = coordinator.snapshot().generation;

	const auto exited = coordinator.exit();
	const OutputLease stopLease = checkEffect(exited, SessionEffectKind::StopYouTube);
	CHECK(stopLease == lease);
	CHECK(exited.snapshot.phase == SessionPhase::Exited);
	CHECK(exited.snapshot.generation != oldGeneration);
	CHECK(exited.snapshot.native == NativeStreamState::Stopped);
	CHECK(exited.snapshot.youtube == YouTubeStreamState::Disabled);

	const std::uint64_t revision = coordinator.snapshot().revision;
	checkNoEffect(coordinator.exit());
	checkNoEffect(coordinator.nativeStarting());
	checkNoEffect(coordinator.nativeStarted({oldGeneration, 1}));
	checkNoEffect(coordinator.youtubeStarted(lease));
	checkNoEffect(coordinator.youtubeReleased(lease));
	const auto profile = coordinator.profileChanged(NativeDestination::Twitch, true, true);
	checkNoEffect(profile);
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.snapshot().phase == SessionPhase::Exited);
	CHECK(coordinator.snapshot().youtube == YouTubeStreamState::Disabled);
}

} // namespace

int main()
{
	testNativeClassification();
	testUnknownAndYouTubeNativeDoNotStartSecondary();
	testTwitchStartAndDuplicateStart();
	testNativeStartFailureAndRetry();
	testDisabledAndMissingKeyNeverStart();
	testConfigurationChangeStopsOnlyYouTube();
	testDuplicateStopAndRapidRestart();
	testStaleNativeStopCannotStopRestartedSession();
	testSecondaryFailureIsolatedFromNativeOutput();
	testUnexpectedYouTubeStopDoesNotLoop();
	testLateStartCannotEscapeStopping();
	testStartFailureCompletesARequestedStop();
	testReconnectAndStaleLease();
	testProfileGenerationInvalidatesPendingCallbacks();
	testProfileChangeWaitsForOldYouTubeRelease();
	testExitIsTerminal();

	if (failures != 0) {
		std::cerr << failures << " session test(s) failed\n";
		return 1;
	}

	std::cout << "All session tests passed\n";
	return 0;
}

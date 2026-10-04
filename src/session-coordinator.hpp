// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace easy_multistream {

enum class SessionPhase {
	Ready,
	ProfileChanging,
	Exited,
};

enum class NativeDestination {
	Unknown,
	Twitch,
	YouTube,
};

enum class NativeStreamState {
	Stopped,
	Starting,
	Streaming,
	Stopping,
};

enum class YouTubeStreamState {
	Disabled,
	SetupRequired,
	NotStreaming,
	Connecting,
	Streaming,
	Reconnecting,
	Stopping,
	Failed,
};

struct OutputLease {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;
};

struct NativeLease {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;
};

constexpr bool operator==(const OutputLease &left, const OutputLease &right) noexcept
{
	return left.generation == right.generation && left.attempt == right.attempt;
}

constexpr bool operator!=(const OutputLease &left, const OutputLease &right) noexcept
{
	return !(left == right);
}

constexpr bool operator==(const NativeLease &left, const NativeLease &right) noexcept
{
	return left.generation == right.generation && left.attempt == right.attempt;
}

constexpr bool operator!=(const NativeLease &left, const NativeLease &right) noexcept
{
	return !(left == right);
}

enum class SessionEffectKind {
	StartYouTube,
	StopYouTube,
};

struct SessionEffect {
	SessionEffectKind kind = SessionEffectKind::StartYouTube;
	OutputLease lease;
};

struct SessionSnapshot {
	std::uint64_t generation = 1;
	std::uint64_t revision = 0;
	SessionPhase phase = SessionPhase::Ready;
	NativeDestination nativeDestination = NativeDestination::Unknown;
	NativeStreamState native = NativeStreamState::Stopped;
	std::optional<NativeLease> nativeLease;
	YouTubeStreamState youtube = YouTubeStreamState::Disabled;
	bool youtubeEnabled = false;
	bool youtubeKeyAvailable = false;
	std::optional<OutputLease> youtubeLease;
};

struct SessionTransition {
	SessionSnapshot snapshot;
	std::optional<SessionEffect> effect;
};

NativeDestination classifyNativeDestination(std::string_view serviceId, std::string_view providerName) noexcept;

class SessionCoordinator final {
public:
	// This value-state machine is not thread-safe. A runtime bridge must serialize
	// every call on one owner thread and post only copied values from OBS callbacks.
	SessionCoordinator() = default;

	SessionSnapshot snapshot() const noexcept;

	SessionTransition configure(NativeDestination destination, bool youtubeEnabled, bool youtubeKeyAvailable);
	SessionTransition nativeStarting();
	SessionTransition nativeStartFailed(NativeLease lease);
	SessionTransition nativeStarted(NativeLease lease);
	SessionTransition nativeStopping(NativeLease lease);
	SessionTransition nativeStopped(NativeLease lease);

	SessionTransition youtubeStarted(OutputLease lease);
	SessionTransition youtubeReconnecting(OutputLease lease);
	SessionTransition youtubeReconnectSucceeded(OutputLease lease);
	SessionTransition retryYouTube();
	// Ends an account/manual preparation that never created an OBS output.  It
	// is intentionally distinct from youtubeReleased(): SetupRequired must not
	// be rendered as a transient transport failure, and it must not call the
	// output adapter's stop path.
	SessionTransition youtubeSetupRequired(OutputLease lease);

	// This is the single terminal event for every outcome, including a rejected
	// start. The adapter may call it only after the OBS output signal callback has
	// returned and all output, service, encoder, native-output, and callback-context
	// references for this lease have been fully released.
	SessionTransition youtubeReleased(OutputLease lease);

	SessionTransition profileChanging();
	SessionTransition profileChanged(NativeDestination destination, bool youtubeEnabled, bool youtubeKeyAvailable);
	SessionTransition exit();

private:
	SessionTransition result(std::optional<SessionEffect> effect = std::nullopt, bool changed = false);
	std::optional<SessionEffect> startYouTubeIfEligible();
	std::optional<SessionEffect> stopYouTubeIfActive();
	std::optional<SessionEffect> retireYouTube();
	SessionTransition completeRetiringYouTube(OutputLease lease);
	void setInactiveYouTubeState() noexcept;
	bool mayStartYouTube() const noexcept;
	bool accepts(OutputLease lease) const noexcept;
	bool accepts(NativeLease lease) const noexcept;
	void invalidateGeneration() noexcept;
	OutputLease nextLease() noexcept;
	NativeLease nextNativeLease() noexcept;

	SessionSnapshot snapshot_;
	std::uint64_t nextAttempt_ = 0;
	std::uint64_t nextNativeAttempt_ = 0;
	std::optional<OutputLease> retiringYouTubeLease_;
};

} // namespace easy_multistream

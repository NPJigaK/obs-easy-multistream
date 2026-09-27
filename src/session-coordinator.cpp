// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "session-coordinator.hpp"

#include <limits>

namespace easy_multistream {
namespace {

std::uint64_t nextNonZero(std::uint64_t value) noexcept
{
	return value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

bool isActiveYouTubeState(YouTubeStreamState state) noexcept
{
	return state == YouTubeStreamState::Connecting || state == YouTubeStreamState::Streaming ||
	       state == YouTubeStreamState::Reconnecting;
}

} // namespace

NativeDestination classifyNativeDestination(std::string_view serviceId, std::string_view providerName) noexcept
{
	if (serviceId != "rtmp_common") {
		return NativeDestination::Unknown;
	}
	if (providerName == "Twitch") {
		return NativeDestination::Twitch;
	}
	if (providerName == "YouTube - RTMP" || providerName == "YouTube - RTMPS" || providerName == "YouTube - HLS" ||
	    providerName == "YouTube / YouTube Gaming" || providerName == "YouTube - RTMPS (Beta)") {
		return NativeDestination::YouTube;
	}
	return NativeDestination::Unknown;
}

SessionSnapshot SessionCoordinator::snapshot() const noexcept
{
	return snapshot_;
}

SessionTransition SessionCoordinator::configure(NativeDestination destination, bool youtubeEnabled,
						bool youtubeKeyAvailable)
{
	if (snapshot_.phase != SessionPhase::Ready) {
		return result();
	}

	const bool configurationChanged = snapshot_.nativeDestination != destination ||
					  snapshot_.youtubeEnabled != youtubeEnabled ||
					  snapshot_.youtubeKeyAvailable != youtubeKeyAvailable;
	if (!configurationChanged) {
		return result();
	}

	snapshot_.nativeDestination = destination;
	snapshot_.youtubeEnabled = youtubeEnabled;
	snapshot_.youtubeKeyAvailable = youtubeKeyAvailable;

	if (!mayStartYouTube()) {
		if (const auto stop = stopYouTubeIfActive()) {
			return result(stop, true);
		}
		if (!snapshot_.youtubeLease.has_value()) {
			setInactiveYouTubeState();
		}
		return result(std::nullopt, true);
	}

	if (!snapshot_.youtubeLease.has_value() && snapshot_.youtube != YouTubeStreamState::Failed) {
		return result(startYouTubeIfEligible(), true);
	}
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::nativeStarting()
{
	if (snapshot_.phase != SessionPhase::Ready || snapshot_.native == NativeStreamState::Starting ||
	    snapshot_.native == NativeStreamState::Streaming) {
		return result();
	}
	snapshot_.nativeLease = nextNativeLease();
	snapshot_.native = NativeStreamState::Starting;
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::nativeStarted(NativeLease lease)
{
	if (!accepts(lease) || snapshot_.native != NativeStreamState::Starting) {
		return result();
	}
	snapshot_.native = NativeStreamState::Streaming;

	if (!snapshot_.youtubeLease.has_value() && snapshot_.youtube != YouTubeStreamState::Failed) {
		const auto start = startYouTubeIfEligible();
		return result(start, true);
	}
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::nativeStartFailed(NativeLease lease)
{
	if (!accepts(lease) || snapshot_.native != NativeStreamState::Starting) {
		return result();
	}
	snapshot_.native = NativeStreamState::Stopped;
	snapshot_.nativeLease.reset();
	if (!snapshot_.youtubeLease.has_value()) {
		setInactiveYouTubeState();
	}
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::nativeStopping(NativeLease lease)
{
	if (!accepts(lease) || snapshot_.native == NativeStreamState::Stopping ||
	    snapshot_.native == NativeStreamState::Stopped) {
		return result();
	}
	snapshot_.native = NativeStreamState::Stopping;
	if (const auto stop = stopYouTubeIfActive()) {
		return result(stop, true);
	}
	if (!snapshot_.youtubeLease.has_value()) {
		setInactiveYouTubeState();
	}
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::nativeStopped(NativeLease lease)
{
	if (!accepts(lease)) {
		return result();
	}
	snapshot_.native = NativeStreamState::Stopped;
	snapshot_.nativeLease.reset();
	if (const auto stop = stopYouTubeIfActive()) {
		return result(stop, true);
	}
	if (!snapshot_.youtubeLease.has_value()) {
		setInactiveYouTubeState();
	}
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::youtubeStarted(OutputLease lease)
{
	if (!accepts(lease) || snapshot_.youtube != YouTubeStreamState::Connecting) {
		return result();
	}
	if (!mayStartYouTube()) {
		snapshot_.youtube = YouTubeStreamState::Stopping;
		return result(SessionEffect{SessionEffectKind::StopYouTube, lease}, true);
	}
	snapshot_.youtube = YouTubeStreamState::Streaming;
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::youtubeReconnecting(OutputLease lease)
{
	if (!accepts(lease) || !isActiveYouTubeState(snapshot_.youtube)) {
		return result();
	}
	if (!mayStartYouTube()) {
		snapshot_.youtube = YouTubeStreamState::Stopping;
		return result(SessionEffect{SessionEffectKind::StopYouTube, lease}, true);
	}
	if (snapshot_.youtube == YouTubeStreamState::Reconnecting) {
		return result();
	}
	snapshot_.youtube = YouTubeStreamState::Reconnecting;
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::youtubeReconnectSucceeded(OutputLease lease)
{
	if (!accepts(lease) || snapshot_.youtube != YouTubeStreamState::Reconnecting) {
		return result();
	}
	if (!mayStartYouTube()) {
		snapshot_.youtube = YouTubeStreamState::Stopping;
		return result(SessionEffect{SessionEffectKind::StopYouTube, lease}, true);
	}
	snapshot_.youtube = YouTubeStreamState::Streaming;
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::youtubeReleased(OutputLease lease)
{
	if (retiringYouTubeLease_.has_value() && *retiringYouTubeLease_ == lease) {
		return completeRetiringYouTube(lease);
	}
	if (!accepts(lease)) {
		return result();
	}
	const bool requestedStop = snapshot_.youtube == YouTubeStreamState::Stopping;
	snapshot_.youtubeLease.reset();
	setInactiveYouTubeState();

	if (requestedStop) {
		const auto start = startYouTubeIfEligible();
		return result(start, true);
	}
	if (mayStartYouTube()) {
		snapshot_.youtube = YouTubeStreamState::Failed;
	}
	return result(std::nullopt, true);
}

SessionTransition SessionCoordinator::profileChanging()
{
	if (snapshot_.phase == SessionPhase::Exited || snapshot_.phase == SessionPhase::ProfileChanging) {
		return result();
	}

	const std::optional<SessionEffect> stop = retireYouTube();
	invalidateGeneration();
	snapshot_.phase = SessionPhase::ProfileChanging;
	snapshot_.nativeDestination = NativeDestination::Unknown;
	snapshot_.native = NativeStreamState::Stopped;
	snapshot_.nativeLease.reset();
	snapshot_.youtubeEnabled = false;
	snapshot_.youtubeKeyAvailable = false;
	snapshot_.youtube = YouTubeStreamState::Disabled;
	return result(stop, true);
}

SessionTransition SessionCoordinator::profileChanged(NativeDestination destination, bool youtubeEnabled,
						     bool youtubeKeyAvailable)
{
	if (snapshot_.phase == SessionPhase::Exited) {
		return result();
	}

	std::optional<SessionEffect> stop;
	if (snapshot_.phase != SessionPhase::ProfileChanging) {
		stop = retireYouTube();
		invalidateGeneration();
	}

	snapshot_.phase = SessionPhase::Ready;
	snapshot_.nativeDestination = destination;
	snapshot_.native = NativeStreamState::Stopped;
	snapshot_.nativeLease.reset();
	snapshot_.youtubeEnabled = youtubeEnabled;
	snapshot_.youtubeKeyAvailable = youtubeKeyAvailable;
	setInactiveYouTubeState();
	return result(stop, true);
}

SessionTransition SessionCoordinator::exit()
{
	if (snapshot_.phase == SessionPhase::Exited) {
		return result();
	}

	const std::optional<SessionEffect> stop = retireYouTube();
	invalidateGeneration();
	snapshot_.phase = SessionPhase::Exited;
	snapshot_.nativeDestination = NativeDestination::Unknown;
	snapshot_.native = NativeStreamState::Stopped;
	snapshot_.nativeLease.reset();
	snapshot_.youtubeEnabled = false;
	snapshot_.youtubeKeyAvailable = false;
	snapshot_.youtube = YouTubeStreamState::Disabled;
	return result(stop, true);
}

SessionTransition SessionCoordinator::result(std::optional<SessionEffect> effect, bool changed)
{
	if (changed || effect.has_value()) {
		snapshot_.revision = nextNonZero(snapshot_.revision);
	}
	return {snapshot_, effect};
}

std::optional<SessionEffect> SessionCoordinator::startYouTubeIfEligible()
{
	if (!mayStartYouTube() || snapshot_.youtubeLease.has_value() || retiringYouTubeLease_.has_value()) {
		return std::nullopt;
	}
	const OutputLease lease = nextLease();
	snapshot_.youtubeLease = lease;
	snapshot_.youtube = YouTubeStreamState::Connecting;
	return SessionEffect{SessionEffectKind::StartYouTube, lease};
}

std::optional<SessionEffect> SessionCoordinator::stopYouTubeIfActive()
{
	if (!snapshot_.youtubeLease.has_value() || !isActiveYouTubeState(snapshot_.youtube)) {
		return std::nullopt;
	}
	snapshot_.youtube = YouTubeStreamState::Stopping;
	return SessionEffect{SessionEffectKind::StopYouTube, *snapshot_.youtubeLease};
}

std::optional<SessionEffect> SessionCoordinator::retireYouTube()
{
	if (!snapshot_.youtubeLease.has_value()) {
		return std::nullopt;
	}
	const OutputLease lease = *snapshot_.youtubeLease;
	const bool stopAlreadyRequested = snapshot_.youtube == YouTubeStreamState::Stopping;
	retiringYouTubeLease_ = lease;
	snapshot_.youtubeLease.reset();
	return stopAlreadyRequested
		       ? std::nullopt
		       : std::optional<SessionEffect>(SessionEffect{SessionEffectKind::StopYouTube, lease});
}

SessionTransition SessionCoordinator::completeRetiringYouTube(OutputLease lease)
{
	if (!retiringYouTubeLease_.has_value() || *retiringYouTubeLease_ != lease) {
		return result();
	}
	retiringYouTubeLease_.reset();
	if (snapshot_.phase != SessionPhase::Ready) {
		return result();
	}
	setInactiveYouTubeState();
	const auto start = startYouTubeIfEligible();
	return result(start, start.has_value());
}

void SessionCoordinator::setInactiveYouTubeState() noexcept
{
	if (!snapshot_.youtubeEnabled) {
		snapshot_.youtube = YouTubeStreamState::Disabled;
	} else if (!snapshot_.youtubeKeyAvailable) {
		snapshot_.youtube = YouTubeStreamState::SetupRequired;
	} else {
		snapshot_.youtube = YouTubeStreamState::NotStreaming;
	}
}

bool SessionCoordinator::mayStartYouTube() const noexcept
{
	return snapshot_.phase == SessionPhase::Ready && snapshot_.nativeDestination == NativeDestination::Twitch &&
	       snapshot_.native == NativeStreamState::Streaming && snapshot_.nativeLease.has_value() &&
	       snapshot_.youtubeEnabled && snapshot_.youtubeKeyAvailable;
}

bool SessionCoordinator::accepts(OutputLease lease) const noexcept
{
	return snapshot_.phase == SessionPhase::Ready && snapshot_.youtubeLease.has_value() &&
	       *snapshot_.youtubeLease == lease;
}

bool SessionCoordinator::accepts(NativeLease lease) const noexcept
{
	return snapshot_.phase == SessionPhase::Ready && snapshot_.nativeLease.has_value() &&
	       *snapshot_.nativeLease == lease;
}

void SessionCoordinator::invalidateGeneration() noexcept
{
	snapshot_.generation = nextNonZero(snapshot_.generation);
	nextAttempt_ = 0;
	nextNativeAttempt_ = 0;
}

OutputLease SessionCoordinator::nextLease() noexcept
{
	nextAttempt_ = nextNonZero(nextAttempt_);
	return {snapshot_.generation, nextAttempt_};
}

NativeLease SessionCoordinator::nextNativeLease() noexcept
{
	nextNativeAttempt_ = nextNonZero(nextNativeAttempt_);
	return {snapshot_.generation, nextNativeAttempt_};
}

} // namespace easy_multistream

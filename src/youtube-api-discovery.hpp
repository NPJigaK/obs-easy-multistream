// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"

#include <QObject>
#include <QtGlobal>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QNetworkAccessManager;

namespace easy_multistream {

inline constexpr char kYouTubeChannelsEndpoint[] = "https://www.googleapis.com/youtube/v3/channels";
inline constexpr char kYouTubeLiveStreamsEndpoint[] = "https://www.googleapis.com/youtube/v3/liveStreams";
inline constexpr std::size_t kYouTubeApiMaxAccessTokenBytes = 2048U;
inline constexpr std::size_t kYouTubeApiMaxIdentifierBytes = 256U;
inline constexpr std::size_t kYouTubeApiMaxLabelBytes = 1024U;
inline constexpr qsizetype kYouTubeApiMaxResponseBytes = 256 * 1024;

struct YouTubeApiAttempt final {
	std::uint64_t generation = 0;
	std::uint64_t attempt = 0;

	bool isValid() const noexcept { return generation != 0 && attempt != 0; }
	bool operator==(const YouTubeApiAttempt &other) const noexcept
	{
		return generation == other.generation && attempt == other.attempt;
	}
};

struct YouTubeApiDiscoveryOptions final {
	std::chrono::milliseconds operationTimeout{std::chrono::seconds(30)};
	qsizetype maxResponseBytes = kYouTubeApiMaxResponseBytes;
};

enum class YouTubeApiOperation {
	ListOwnedChannels,
	ListReusableStreams,
};

enum class YouTubeApiTransportState {
	Idle,
	InFlight,
	Completed,
	Cancelled,
	Closed,
};

enum class YouTubeApiStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	InvalidOptions,
	InvalidAttempt,
	InvalidAccessToken,
	InvalidChannelId,
	InvalidPageToken,
	InvalidCompletionHandler,
	RequestCreationFailed,
};

enum class YouTubeApiCompletionStatus {
	Success,
	ProviderRejected,
	InvalidResponse,
	NetworkFailure,
	TlsFailure,
	RedirectRejected,
	ResponseTooLarge,
	TimedOut,
	HttpFailure,
};

enum class YouTubeApiProviderError {
	None,
	InvalidRequest,
	InvalidPageToken,
	InvalidToken,
	InsufficientPermissions,
	LiveStreamingNotEnabled,
	QuotaExceeded,
	RateLimited,
	TemporarilyUnavailable,
	PermissionDenied,
	NotFound,
	Unknown,
};

struct YouTubeOwnedChannel final {
	std::string id;
	std::string label;
};

// Discovery candidates deliberately contain no ingestion address or stream
// key. liveStreams.list(mine=true) omits non-reusable streams, and this request
// asks only for id/snippet. The selected stream is resolved by a later,
// separate secret-bearing operation.
struct YouTubeReusableStream final {
	std::string id;
	std::string channelId;
	std::string label;
};

struct YouTubeApiCompletion final {
	YouTubeApiAttempt attempt;
	YouTubeApiOperation operation = YouTubeApiOperation::ListOwnedChannels;
	YouTubeApiCompletionStatus status = YouTubeApiCompletionStatus::InvalidResponse;
	YouTubeApiProviderError providerError = YouTubeApiProviderError::None;
	int httpStatus = 0;
	std::vector<YouTubeOwnedChannel> channels;
	std::vector<YouTubeReusableStream> streams;
	// Opaque continuation value. It is never logged, displayed, or persisted;
	// the future provider owns pagination limits and cycle detection.
	std::string nextPageToken;

	YouTubeApiCompletion() = default;
	YouTubeApiCompletion(const YouTubeApiCompletion &) = delete;
	YouTubeApiCompletion &operator=(const YouTubeApiCompletion &) = delete;
	YouTubeApiCompletion(YouTubeApiCompletion &&) noexcept = default;
	YouTubeApiCompletion &operator=(YouTubeApiCompletion &&) noexcept = default;

	bool succeeded() const noexcept { return status == YouTubeApiCompletionStatus::Success; }
};

struct YouTubeListChannelsRequest final {
	YouTubeApiAttempt attempt;
	SecureBuffer accessToken;
	std::string pageToken;

	YouTubeListChannelsRequest() = default;
	YouTubeListChannelsRequest(const YouTubeListChannelsRequest &) = delete;
	YouTubeListChannelsRequest &operator=(const YouTubeListChannelsRequest &) = delete;
	YouTubeListChannelsRequest(YouTubeListChannelsRequest &&) noexcept = default;
	YouTubeListChannelsRequest &operator=(YouTubeListChannelsRequest &&) noexcept = default;
};

struct YouTubeListStreamsRequest final {
	YouTubeApiAttempt attempt;
	SecureBuffer accessToken;
	std::string channelId;
	std::string pageToken;

	YouTubeListStreamsRequest() = default;
	YouTubeListStreamsRequest(const YouTubeListStreamsRequest &) = delete;
	YouTubeListStreamsRequest &operator=(const YouTubeListStreamsRequest &) = delete;
	YouTubeListStreamsRequest(YouTubeListStreamsRequest &&) noexcept = default;
	YouTubeListStreamsRequest &operator=(YouTubeListStreamsRequest &&) noexcept = default;
};

// Owner-thread-only, fixed-origin HTTPS transport for YouTube's read-only
// channel and reusable-stream discovery APIs. Access tokens and resolved
// stream keys are move-only buffers and never enter URLs, settings, errors, or
// snapshots. Cancellation is silent and invalidates queued completion work.
//
// This library remains detached from the OBS module until the complete account
// provider is ready. Production callers cannot replace the network manager;
// the private friend exists solely for the standalone deterministic test.
class YouTubeApiDiscovery final : public QObject {
public:
	using CompletionHandler = std::function<void(YouTubeApiCompletion)>;

	explicit YouTubeApiDiscovery(YouTubeApiDiscoveryOptions options = {}, QObject *parent = nullptr);
	~YouTubeApiDiscovery() override;

	YouTubeApiDiscovery(const YouTubeApiDiscovery &) = delete;
	YouTubeApiDiscovery &operator=(const YouTubeApiDiscovery &) = delete;
	YouTubeApiDiscovery(YouTubeApiDiscovery &&) = delete;
	YouTubeApiDiscovery &operator=(YouTubeApiDiscovery &&) = delete;

	YouTubeApiStartStatus startListChannels(YouTubeListChannelsRequest request,
						CompletionHandler completionHandler) noexcept;
	YouTubeApiStartStatus startListStreams(YouTubeListStreamsRequest request,
					       CompletionHandler completionHandler) noexcept;

	bool cancel(YouTubeApiAttempt attempt) noexcept;
	bool shutdown() noexcept;

	YouTubeApiTransportState state() const noexcept;
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept;
	std::optional<YouTubeApiOperation> activeOperation() const noexcept;

private:
	using QObject::moveToThread;
	friend class YouTubeApiDiscoveryTestAccess;
	friend class YouTubeApiDiscoveryPagerTestAccess;

	YouTubeApiDiscovery(std::unique_ptr<QNetworkAccessManager> networkManager, YouTubeApiDiscoveryOptions options,
			    QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

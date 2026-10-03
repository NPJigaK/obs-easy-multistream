// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"
#include "youtube-api-discovery.hpp"

#include <QObject>
#include <QtGlobal>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class QNetworkAccessManager;

namespace easy_multistream {

// The resolver intentionally has a separate result type from discovery. A
// discovery result never contains an ingestion address or a stream key; only
// this selected-item request may return those values.
struct YouTubeStreamResolverOptions final {
	std::chrono::milliseconds operationTimeout{std::chrono::seconds(30)};
	qsizetype maxResponseBytes = kYouTubeApiMaxResponseBytes;
};

enum class YouTubeStreamResolverState {
	Idle,
	InFlight,
	Completed,
	Cancelled,
	Closed,
};

enum class YouTubeStreamResolverStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	InvalidOptions,
	InvalidAttempt,
	InvalidAccessToken,
	InvalidChannelId,
	InvalidStreamId,
	InvalidCompletionHandler,
	RequestCreationFailed,
};

enum class YouTubeStreamResolverCompletionStatus {
	Success,
	StreamNotFound,
	StreamMismatch,
	StreamAlreadyActive,
	StreamNotReady,
	ProviderRejected,
	InvalidResponse,
	TlsFailure,
	NetworkFailure,
	RedirectRejected,
	ResponseTooLarge,
	TimedOut,
	HttpFailure,
};

struct YouTubeResolvedIngestion final {
	std::string serverUrl;
	SecureBuffer streamKey;

	YouTubeResolvedIngestion() = default;
	YouTubeResolvedIngestion(const YouTubeResolvedIngestion &) = delete;
	YouTubeResolvedIngestion &operator=(const YouTubeResolvedIngestion &) = delete;
	YouTubeResolvedIngestion(YouTubeResolvedIngestion &&) noexcept = default;
	YouTubeResolvedIngestion &operator=(YouTubeResolvedIngestion &&) noexcept = default;
};

struct YouTubeStreamResolverCompletion final {
	YouTubeApiAttempt attempt;
	YouTubeStreamResolverCompletionStatus status = YouTubeStreamResolverCompletionStatus::InvalidResponse;
	YouTubeApiProviderError providerError = YouTubeApiProviderError::None;
	int httpStatus = 0;
	std::optional<YouTubeResolvedIngestion> ingestion;

	YouTubeStreamResolverCompletion() = default;
	YouTubeStreamResolverCompletion(const YouTubeStreamResolverCompletion &) = delete;
	YouTubeStreamResolverCompletion &operator=(const YouTubeStreamResolverCompletion &) = delete;
	YouTubeStreamResolverCompletion(YouTubeStreamResolverCompletion &&) noexcept = default;
	YouTubeStreamResolverCompletion &operator=(YouTubeStreamResolverCompletion &&) noexcept = default;

	bool succeeded() const noexcept { return status == YouTubeStreamResolverCompletionStatus::Success; }
};

struct YouTubeResolveStreamRequest final {
	YouTubeApiAttempt attempt;
	SecureBuffer accessToken;
	std::string channelId;
	std::string streamId;

	YouTubeResolveStreamRequest() = default;
	YouTubeResolveStreamRequest(const YouTubeResolveStreamRequest &) = delete;
	YouTubeResolveStreamRequest &operator=(const YouTubeResolveStreamRequest &) = delete;
	YouTubeResolveStreamRequest(YouTubeResolveStreamRequest &&) noexcept = default;
	YouTubeResolveStreamRequest &operator=(YouTubeResolveStreamRequest &&) noexcept = default;
};

// Owner-thread-only, fixed-origin HTTPS transport for resolving one selected
// reusable YouTube stream. It deliberately does not use mine=true: the caller
// supplies the stream and channel IDs selected by the preceding discovery
// operation. It returns only a validated RTMPS URL and a move-only key.
//
// Access tokens, stream keys, and raw response bytes never enter errors,
// settings, logs, or normal discovery candidates. The resolver is detached
// from the OBS plugin until the account provider and UI are complete.
class YouTubeApiStreamResolver final : public QObject {
public:
	using CompletionHandler = std::function<void(YouTubeStreamResolverCompletion)>;

	explicit YouTubeApiStreamResolver(YouTubeStreamResolverOptions options = {}, QObject *parent = nullptr);
	~YouTubeApiStreamResolver() override;

	YouTubeApiStreamResolver(const YouTubeApiStreamResolver &) = delete;
	YouTubeApiStreamResolver &operator=(const YouTubeApiStreamResolver &) = delete;
	YouTubeApiStreamResolver(YouTubeApiStreamResolver &&) = delete;
	YouTubeApiStreamResolver &operator=(YouTubeApiStreamResolver &&) = delete;

	YouTubeStreamResolverStartStatus startResolveStream(YouTubeResolveStreamRequest request,
							    CompletionHandler completionHandler) noexcept;

	bool cancel(YouTubeApiAttempt attempt) noexcept;
	bool shutdown() noexcept;

	YouTubeStreamResolverState state() const noexcept;
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept;

private:
	using QObject::moveToThread;
	friend class YouTubeApiStreamResolverTestAccess;

	YouTubeApiStreamResolver(std::unique_ptr<QNetworkAccessManager> networkManager,
				 YouTubeStreamResolverOptions options, QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

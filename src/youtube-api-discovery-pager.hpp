// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "youtube-api-discovery.hpp"

#include <QObject>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace easy_multistream {

inline constexpr std::size_t kYouTubeApiDefaultMaximumPages = 8U;
inline constexpr std::size_t kYouTubeApiDefaultMaximumItems = 400U;

struct YouTubeApiDiscoveryPagerOptions final {
	std::size_t maxPages = kYouTubeApiDefaultMaximumPages;
	std::size_t maxItems = kYouTubeApiDefaultMaximumItems;
	std::chrono::milliseconds totalTimeout{std::chrono::seconds(90)};
	YouTubeApiDiscoveryOptions pageRequestOptions;
};

enum class YouTubeApiPagerState {
	Idle,
	InFlight,
	Completed,
	Cancelled,
	Closed,
};

enum class YouTubeApiPagerStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	InvalidOptions,
	InvalidAttempt,
	InvalidAccessToken,
	InvalidChannelId,
	InvalidCompletionHandler,
	RequestCreationFailed,
};

// This classification is intentionally internal-facing. Page tokens, raw
// provider responses, counts, and partial candidate lists never leave the
// pager or become user-visible state.
enum class YouTubeApiPagingFailure {
	None,
	PageLimitExceeded,
	ItemLimitExceeded,
	PageTokenCycle,
	DuplicateItem,
	UnexpectedPageData,
	RequestStartFailed,
	ResourceFailure,
};

struct YouTubeApiPagedCompletion final {
	YouTubeApiAttempt attempt;
	YouTubeApiOperation operation = YouTubeApiOperation::ListOwnedChannels;
	YouTubeApiCompletionStatus status = YouTubeApiCompletionStatus::InvalidResponse;
	YouTubeApiProviderError providerError = YouTubeApiProviderError::None;
	YouTubeApiPagingFailure pagingFailure = YouTubeApiPagingFailure::None;
	int httpStatus = 0;
	std::vector<YouTubeOwnedChannel> channels;
	std::vector<YouTubeReusableStream> streams;

	YouTubeApiPagedCompletion() = default;
	YouTubeApiPagedCompletion(const YouTubeApiPagedCompletion &) = delete;
	YouTubeApiPagedCompletion &operator=(const YouTubeApiPagedCompletion &) = delete;
	YouTubeApiPagedCompletion(YouTubeApiPagedCompletion &&) noexcept = default;
	YouTubeApiPagedCompletion &operator=(YouTubeApiPagedCompletion &&) noexcept = default;

	bool succeeded() const noexcept
	{
		return status == YouTubeApiCompletionStatus::Success && pagingFailure == YouTubeApiPagingFailure::None;
	}
};

struct YouTubeListAllChannelsRequest final {
	YouTubeApiAttempt attempt;
	SecureBuffer accessToken;

	YouTubeListAllChannelsRequest() = default;
	YouTubeListAllChannelsRequest(const YouTubeListAllChannelsRequest &) = delete;
	YouTubeListAllChannelsRequest &operator=(const YouTubeListAllChannelsRequest &) = delete;
	YouTubeListAllChannelsRequest(YouTubeListAllChannelsRequest &&) noexcept = default;
	YouTubeListAllChannelsRequest &operator=(YouTubeListAllChannelsRequest &&) noexcept = default;
};

struct YouTubeListAllStreamsRequest final {
	YouTubeApiAttempt attempt;
	SecureBuffer accessToken;
	std::string channelId;

	YouTubeListAllStreamsRequest() = default;
	YouTubeListAllStreamsRequest(const YouTubeListAllStreamsRequest &) = delete;
	YouTubeListAllStreamsRequest &operator=(const YouTubeListAllStreamsRequest &) = delete;
	YouTubeListAllStreamsRequest(YouTubeListAllStreamsRequest &&) noexcept = default;
	YouTubeListAllStreamsRequest &operator=(YouTubeListAllStreamsRequest &&) noexcept = default;
};

// Owner-thread-only bounded pagination over YouTubeApiDiscovery. A completion
// is either the entire candidate set or a failure with empty candidates;
// partial pages are never exposed. Access and page tokens remain internal and
// are cleared on completion, cancellation, timeout, and shutdown.
class YouTubeApiDiscoveryPager final : public QObject {
public:
	using CompletionHandler = std::function<void(YouTubeApiPagedCompletion)>;

	explicit YouTubeApiDiscoveryPager(YouTubeApiDiscoveryPagerOptions options = {}, QObject *parent = nullptr);
	~YouTubeApiDiscoveryPager() override;

	YouTubeApiDiscoveryPager(const YouTubeApiDiscoveryPager &) = delete;
	YouTubeApiDiscoveryPager &operator=(const YouTubeApiDiscoveryPager &) = delete;
	YouTubeApiDiscoveryPager(YouTubeApiDiscoveryPager &&) = delete;
	YouTubeApiDiscoveryPager &operator=(YouTubeApiDiscoveryPager &&) = delete;

	YouTubeApiPagerStartStatus startListAllChannels(YouTubeListAllChannelsRequest request,
							CompletionHandler completionHandler) noexcept;
	YouTubeApiPagerStartStatus startListAllStreams(YouTubeListAllStreamsRequest request,
						       CompletionHandler completionHandler) noexcept;

	bool cancel(YouTubeApiAttempt attempt) noexcept;
	bool shutdown() noexcept;

	YouTubeApiPagerState state() const noexcept;
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept;
	std::optional<YouTubeApiOperation> activeOperation() const noexcept;

private:
	using QObject::moveToThread;
	friend class YouTubeApiDiscoveryPagerTestAccess;

	YouTubeApiDiscoveryPager(std::unique_ptr<YouTubeApiDiscovery> discovery,
				 YouTubeApiDiscoveryPagerOptions options, QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

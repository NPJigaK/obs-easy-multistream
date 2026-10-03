// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-api-discovery-pager.hpp"

#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <cstdint>
#include <unordered_set>
#include <utility>

namespace easy_multistream {
namespace {

constexpr std::size_t kMaximumConfiguredPages = 100U;
constexpr std::size_t kMaximumConfiguredItems = 5000U;

bool validOptions(const YouTubeApiDiscoveryPagerOptions &options) noexcept
{
	return options.maxPages > 0 && options.maxPages <= kMaximumConfiguredPages && options.maxItems > 0 &&
	       options.maxItems <= kMaximumConfiguredItems && options.totalTimeout >= std::chrono::milliseconds(10) &&
	       options.totalTimeout <= std::chrono::minutes(10);
}

YouTubeApiPagerStartStatus mapStartStatus(YouTubeApiStartStatus status) noexcept
{
	switch (status) {
	case YouTubeApiStartStatus::Started:
		return YouTubeApiPagerStartStatus::Started;
	case YouTubeApiStartStatus::WrongThread:
		return YouTubeApiPagerStartStatus::WrongThread;
	case YouTubeApiStartStatus::Busy:
		return YouTubeApiPagerStartStatus::Busy;
	case YouTubeApiStartStatus::Closed:
		return YouTubeApiPagerStartStatus::Closed;
	case YouTubeApiStartStatus::InvalidOptions:
		return YouTubeApiPagerStartStatus::InvalidOptions;
	case YouTubeApiStartStatus::InvalidAttempt:
		return YouTubeApiPagerStartStatus::InvalidAttempt;
	case YouTubeApiStartStatus::InvalidAccessToken:
		return YouTubeApiPagerStartStatus::InvalidAccessToken;
	case YouTubeApiStartStatus::InvalidChannelId:
		return YouTubeApiPagerStartStatus::InvalidChannelId;
	case YouTubeApiStartStatus::InvalidCompletionHandler:
		return YouTubeApiPagerStartStatus::InvalidCompletionHandler;
	case YouTubeApiStartStatus::InvalidPageToken:
	case YouTubeApiStartStatus::RequestCreationFailed:
		return YouTubeApiPagerStartStatus::RequestCreationFailed;
	}
	return YouTubeApiPagerStartStatus::RequestCreationFailed;
}

} // namespace

class YouTubeApiDiscoveryPager::Impl final {
public:
	Impl(YouTubeApiDiscoveryPager *owner, std::unique_ptr<YouTubeApiDiscovery> discovery,
	     YouTubeApiDiscoveryPagerOptions options)
		: owner_(owner),
		  discovery_(std::move(discovery)),
		  options_(options)
	{
		Q_ASSERT_X(discovery_ != nullptr, "YouTubeApiDiscoveryPager::Impl", "The page transport must exist");
		Q_ASSERT_X(discovery_ == nullptr || discovery_->parent() == nullptr, "YouTubeApiDiscoveryPager::Impl",
			   "The page transport must be unparented because ownership is transferred");
		Q_ASSERT_X(discovery_ == nullptr || discovery_->thread() == owner_->thread(),
			   "YouTubeApiDiscoveryPager::Impl", "The page transport must use the pager owner thread");
		totalTimer_.setSingleShot(true);
		QObject::connect(&totalTimer_, &QTimer::timeout, owner_, [this]() { handleTotalTimeout(); });
	}

	~Impl()
	{
		Q_ASSERT(!owner_ || QThread::currentThread() == owner_->thread());
		if (onOwnerThread()) {
			shutdown();
		}
	}

	YouTubeApiPagerStartStatus startListAllChannels(YouTubeListAllChannelsRequest request,
							YouTubeApiDiscoveryPager::CompletionHandler handler) noexcept
	{
		return begin(request.attempt, YouTubeApiOperation::ListOwnedChannels, std::move(request.accessToken),
			     {}, std::move(handler));
	}

	YouTubeApiPagerStartStatus startListAllStreams(YouTubeListAllStreamsRequest request,
						       YouTubeApiDiscoveryPager::CompletionHandler handler) noexcept
	{
		return begin(request.attempt, YouTubeApiOperation::ListReusableStreams, std::move(request.accessToken),
			     std::move(request.channelId), std::move(handler));
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept
	{
		if (!onOwnerThread() || !attempt.isValid()) {
			return false;
		}
		const bool matchesActive = activeAttempt_.has_value() && *activeAttempt_ == attempt;
		const bool matchesPending = pendingCompletion_.has_value() && pendingCompletion_->attempt == attempt;
		if (!matchesActive && !matchesPending) {
			return false;
		}
		cancelPage();
		advanceEpoch();
		clearOperation();
		state_ = YouTubeApiPagerState::Cancelled;
		return true;
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		if (state_ == YouTubeApiPagerState::Closed) {
			return true;
		}
		cancelPage();
		advanceEpoch();
		clearOperation();
		if (discovery_) {
			discovery_->shutdown();
		}
		state_ = YouTubeApiPagerState::Closed;
		return true;
	}

	YouTubeApiPagerState state() const noexcept { return state_; }
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept { return activeAttempt_; }
	std::optional<YouTubeApiOperation> activeOperation() const noexcept { return activeOperation_; }

private:
	bool onOwnerThread() const noexcept
	{
		QThread *current = QThread::currentThread();
		return owner_ != nullptr && discovery_ != nullptr && current == owner_->thread() &&
		       current == discovery_->thread() && current == totalTimer_.thread();
	}

	bool isBusy() const noexcept
	{
		return state_ == YouTubeApiPagerState::InFlight || activeAttempt_.has_value() ||
		       pendingCompletion_.has_value() || completionDeliveryQueued_;
	}

	YouTubeApiPagerStartStatus begin(YouTubeApiAttempt attempt, YouTubeApiOperation operation,
					 SecureBuffer accessToken, std::string channelId,
					 YouTubeApiDiscoveryPager::CompletionHandler handler) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeApiPagerStartStatus::WrongThread;
		}
		if (state_ == YouTubeApiPagerState::Closed) {
			return YouTubeApiPagerStartStatus::Closed;
		}
		if (isBusy()) {
			return YouTubeApiPagerStartStatus::Busy;
		}
		if (!validOptions(options_)) {
			return YouTubeApiPagerStartStatus::InvalidOptions;
		}
		if (!attempt.isValid()) {
			return YouTubeApiPagerStartStatus::InvalidAttempt;
		}
		if (!handler) {
			return YouTubeApiPagerStartStatus::InvalidCompletionHandler;
		}

		try {
			advanceEpoch();
			activeAttempt_ = attempt;
			activeOperation_ = operation;
			accessToken_ = std::move(accessToken);
			channelId_ = std::move(channelId);
			completionHandler_ = std::move(handler);
			state_ = YouTubeApiPagerState::InFlight;
			totalTimer_.start(options_.totalTimeout);
			const YouTubeApiStartStatus pageStatus = startPage({});
			if (pageStatus != YouTubeApiStartStatus::Started) {
				const YouTubeApiPagerStartStatus result = mapStartStatus(pageStatus);
				advanceEpoch();
				clearOperation();
				state_ = YouTubeApiPagerState::Idle;
				return result;
			}
			return YouTubeApiPagerStartStatus::Started;
		} catch (...) {
			cancelPage();
			advanceEpoch();
			clearOperation();
			state_ = YouTubeApiPagerState::Idle;
			return YouTubeApiPagerStartStatus::RequestCreationFailed;
		}
	}

	YouTubeApiStartStatus startPage(std::string pageToken)
	{
		if (!activeAttempt_.has_value() || !activeOperation_.has_value()) {
			return YouTubeApiStartStatus::RequestCreationFailed;
		}
		const std::uint64_t epoch = operationEpoch_;
		const std::uint64_t sequence = nextPageSequence();
		if (*activeOperation_ == YouTubeApiOperation::ListOwnedChannels) {
			YouTubeListChannelsRequest request;
			request.attempt = *activeAttempt_;
			request.accessToken = SecureBuffer::copyOf(accessToken_.view());
			request.pageToken = std::move(pageToken);
			return discovery_->startListChannels(std::move(request),
							     [this, epoch, sequence](YouTubeApiCompletion completion) {
								     handlePage(epoch, sequence, std::move(completion));
							     });
		}

		YouTubeListStreamsRequest request;
		request.attempt = *activeAttempt_;
		request.accessToken = SecureBuffer::copyOf(accessToken_.view());
		request.channelId = channelId_;
		request.pageToken = std::move(pageToken);
		return discovery_->startListStreams(std::move(request),
						    [this, epoch, sequence](YouTubeApiCompletion completion) {
							    handlePage(epoch, sequence, std::move(completion));
						    });
	}

	void handlePage(std::uint64_t epoch, std::uint64_t sequence, YouTubeApiCompletion completion) noexcept
	{
		if (!isCurrent(epoch, sequence, completion)) {
			return;
		}

		try {
			if (!completion.succeeded()) {
				finishFromPage(std::move(completion));
				return;
			}
			if (!appendPage(completion)) {
				return;
			}
			++pagesFetched_;
			if (completion.nextPageToken.empty()) {
				finishSuccess(completion.httpStatus);
				return;
			}
			if (pagesFetched_ >= options_.maxPages) {
				finishPagingFailure(YouTubeApiPagingFailure::PageLimitExceeded);
				return;
			}
			if (!seenPageTokens_.emplace(completion.nextPageToken).second) {
				finishPagingFailure(YouTubeApiPagingFailure::PageTokenCycle);
				return;
			}
			const YouTubeApiStartStatus nextStatus = startPage(std::move(completion.nextPageToken));
			if (nextStatus != YouTubeApiStartStatus::Started) {
				finishPagingFailure(YouTubeApiPagingFailure::RequestStartFailed,
						    YouTubeApiCompletionStatus::NetworkFailure);
			}
		} catch (...) {
			finishPagingFailure(YouTubeApiPagingFailure::ResourceFailure,
					    YouTubeApiCompletionStatus::InvalidResponse);
		}
	}

	bool isCurrent(std::uint64_t epoch, std::uint64_t sequence,
		       const YouTubeApiCompletion &completion) const noexcept
	{
		return state_ == YouTubeApiPagerState::InFlight && epoch == operationEpoch_ &&
		       sequence == pageSequence_ && activeAttempt_.has_value() && activeOperation_.has_value() &&
		       completion.attempt == *activeAttempt_ && completion.operation == *activeOperation_;
	}

	bool appendPage(const YouTubeApiCompletion &completion)
	{
		if (*activeOperation_ == YouTubeApiOperation::ListOwnedChannels) {
			if (!completion.streams.empty()) {
				finishPagingFailure(YouTubeApiPagingFailure::UnexpectedPageData);
				return false;
			}
			if (completion.channels.size() > options_.maxItems - channels_.size()) {
				finishPagingFailure(YouTubeApiPagingFailure::ItemLimitExceeded);
				return false;
			}
			for (const auto &channel : completion.channels) {
				if (!seenItemIds_.emplace(channel.id).second) {
					finishPagingFailure(YouTubeApiPagingFailure::DuplicateItem);
					return false;
				}
			}
			channels_.insert(channels_.end(), completion.channels.begin(), completion.channels.end());
			return true;
		}

		if (!completion.channels.empty()) {
			finishPagingFailure(YouTubeApiPagingFailure::UnexpectedPageData);
			return false;
		}
		if (completion.streams.size() > options_.maxItems - streams_.size()) {
			finishPagingFailure(YouTubeApiPagingFailure::ItemLimitExceeded);
			return false;
		}
		for (const auto &stream : completion.streams) {
			if (stream.channelId != channelId_) {
				finishPagingFailure(YouTubeApiPagingFailure::UnexpectedPageData);
				return false;
			}
			if (!seenItemIds_.emplace(stream.id).second) {
				finishPagingFailure(YouTubeApiPagingFailure::DuplicateItem);
				return false;
			}
		}
		streams_.insert(streams_.end(), completion.streams.begin(), completion.streams.end());
		return true;
	}

	void finishFromPage(YouTubeApiCompletion completion)
	{
		YouTubeApiPagedCompletion result;
		result.attempt = completion.attempt;
		result.operation = completion.operation;
		result.status = completion.status;
		result.providerError = completion.providerError;
		result.httpStatus = completion.httpStatus;
		finish(std::move(result));
	}

	void finishSuccess(int httpStatus)
	{
		YouTubeApiPagedCompletion result;
		result.attempt = *activeAttempt_;
		result.operation = *activeOperation_;
		result.status = YouTubeApiCompletionStatus::Success;
		result.httpStatus = httpStatus;
		result.channels = std::move(channels_);
		result.streams = std::move(streams_);
		finish(std::move(result));
	}

	void finishPagingFailure(YouTubeApiPagingFailure failure,
				 YouTubeApiCompletionStatus status = YouTubeApiCompletionStatus::InvalidResponse)
	{
		if (!activeAttempt_.has_value() || !activeOperation_.has_value()) {
			return;
		}
		YouTubeApiPagedCompletion result;
		result.attempt = *activeAttempt_;
		result.operation = *activeOperation_;
		result.status = status;
		result.pagingFailure = failure;
		finish(std::move(result));
	}

	void finish(YouTubeApiPagedCompletion completion)
	{
		if (state_ != YouTubeApiPagerState::InFlight) {
			return;
		}
		totalTimer_.stop();
		clearSensitiveAndAccumulated();
		activeAttempt_.reset();
		activeOperation_.reset();
		state_ = YouTubeApiPagerState::Completed;
		pendingCompletion_.emplace(std::move(completion));
		queueCompletion();
	}

	void queueCompletion()
	{
		if (!pendingCompletion_.has_value() || completionDeliveryQueued_) {
			return;
		}
		completionDeliveryQueued_ = true;
		const std::uint64_t epoch = operationEpoch_;
		const bool queued = QMetaObject::invokeMethod(
			owner_,
			[this, epoch]() {
				if (epoch != operationEpoch_) {
					return;
				}
				completionDeliveryQueued_ = false;
				deliverCompletion();
			},
			Qt::QueuedConnection);
		if (!queued) {
			completionDeliveryQueued_ = false;
			completionHandler_ = {};
			pendingCompletion_.reset();
		}
	}

	void deliverCompletion()
	{
		if (!pendingCompletion_.has_value()) {
			return;
		}
		YouTubeApiPagedCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		auto handler = std::move(completionHandler_);
		completionHandler_ = {};
		if (handler) {
			try {
				handler(std::move(completion));
			} catch (...) {
				// Never unwind an integration callback through Qt's event loop.
			}
		}
	}

	void handleTotalTimeout() noexcept
	{
		if (state_ != YouTubeApiPagerState::InFlight) {
			return;
		}
		try {
			cancelPage();
			finishPagingFailure(YouTubeApiPagingFailure::None, YouTubeApiCompletionStatus::TimedOut);
		} catch (...) {
			advanceEpoch();
			clearOperation();
			state_ = YouTubeApiPagerState::Completed;
		}
	}

	void cancelPage() noexcept
	{
		totalTimer_.stop();
		if (discovery_ && activeAttempt_.has_value()) {
			discovery_->cancel(*activeAttempt_);
		}
	}

	void clearSensitiveAndAccumulated() noexcept
	{
		accessToken_.clear();
		channelId_.clear();
		seenPageTokens_.clear();
		seenItemIds_.clear();
		channels_.clear();
		streams_.clear();
		pagesFetched_ = 0;
	}

	void clearOperation() noexcept
	{
		totalTimer_.stop();
		clearSensitiveAndAccumulated();
		activeAttempt_.reset();
		activeOperation_.reset();
		completionHandler_ = {};
		pendingCompletion_.reset();
		completionDeliveryQueued_ = false;
	}

	void advanceEpoch() noexcept
	{
		++operationEpoch_;
		if (operationEpoch_ == 0) {
			++operationEpoch_;
		}
		pageSequence_ = 0;
		completionDeliveryQueued_ = false;
	}

	std::uint64_t nextPageSequence() noexcept
	{
		++pageSequence_;
		if (pageSequence_ == 0) {
			++pageSequence_;
		}
		return pageSequence_;
	}

	YouTubeApiDiscoveryPager *owner_ = nullptr;
	std::unique_ptr<YouTubeApiDiscovery> discovery_;
	YouTubeApiDiscoveryPagerOptions options_;
	QTimer totalTimer_;
	YouTubeApiPagerState state_ = YouTubeApiPagerState::Idle;
	std::optional<YouTubeApiAttempt> activeAttempt_;
	std::optional<YouTubeApiOperation> activeOperation_;
	SecureBuffer accessToken_;
	std::string channelId_;
	std::unordered_set<std::string> seenPageTokens_;
	std::unordered_set<std::string> seenItemIds_;
	std::vector<YouTubeOwnedChannel> channels_;
	std::vector<YouTubeReusableStream> streams_;
	std::size_t pagesFetched_ = 0;
	YouTubeApiDiscoveryPager::CompletionHandler completionHandler_;
	std::optional<YouTubeApiPagedCompletion> pendingCompletion_;
	std::uint64_t operationEpoch_ = 1;
	std::uint64_t pageSequence_ = 0;
	bool completionDeliveryQueued_ = false;
};

YouTubeApiDiscoveryPager::YouTubeApiDiscoveryPager(YouTubeApiDiscoveryPagerOptions options, QObject *parent)
	: YouTubeApiDiscoveryPager(std::make_unique<YouTubeApiDiscovery>(options.pageRequestOptions), options, parent)
{
}

YouTubeApiDiscoveryPager::YouTubeApiDiscoveryPager(std::unique_ptr<YouTubeApiDiscovery> discovery,
						   YouTubeApiDiscoveryPagerOptions options, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(discovery), options))
{
}

YouTubeApiDiscoveryPager::~YouTubeApiDiscoveryPager()
{
	Q_ASSERT_X(thread() == QThread::currentThread(), "YouTubeApiDiscoveryPager::~YouTubeApiDiscoveryPager",
		   "Destroy the pager on its owner thread after shutdown");
	if (thread() == QThread::currentThread() && impl_) {
		impl_->shutdown();
	}
}

YouTubeApiPagerStartStatus YouTubeApiDiscoveryPager::startListAllChannels(YouTubeListAllChannelsRequest request,
									  CompletionHandler completionHandler) noexcept
{
	return impl_->startListAllChannels(std::move(request), std::move(completionHandler));
}

YouTubeApiPagerStartStatus YouTubeApiDiscoveryPager::startListAllStreams(YouTubeListAllStreamsRequest request,
									 CompletionHandler completionHandler) noexcept
{
	return impl_->startListAllStreams(std::move(request), std::move(completionHandler));
}

bool YouTubeApiDiscoveryPager::cancel(YouTubeApiAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool YouTubeApiDiscoveryPager::shutdown() noexcept
{
	return impl_->shutdown();
}

YouTubeApiPagerState YouTubeApiDiscoveryPager::state() const noexcept
{
	return impl_->state();
}

std::optional<YouTubeApiAttempt> YouTubeApiDiscoveryPager::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

std::optional<YouTubeApiOperation> YouTubeApiDiscoveryPager::activeOperation() const noexcept
{
	return impl_->activeOperation();
}

} // namespace easy_multistream

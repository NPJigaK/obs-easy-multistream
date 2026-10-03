// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-api-discovery-pager.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace easy_multistream {

class YouTubeApiDiscoveryPagerTestAccess final {
public:
	YouTubeApiDiscoveryPagerTestAccess(std::unique_ptr<QNetworkAccessManager> manager,
					   YouTubeApiDiscoveryPagerOptions pagerOptions = {},
					   YouTubeApiDiscoveryOptions pageOptions = {})
	{
		auto discovery = std::unique_ptr<YouTubeApiDiscovery>(
			new YouTubeApiDiscovery(std::move(manager), pageOptions, nullptr));
		pager_.reset(new YouTubeApiDiscoveryPager(std::move(discovery), pagerOptions, nullptr));
	}

	YouTubeApiPagerStartStatus startListAllChannels(YouTubeListAllChannelsRequest request,
							YouTubeApiDiscoveryPager::CompletionHandler handler)
	{
		return pager_->startListAllChannels(std::move(request), std::move(handler));
	}

	YouTubeApiPagerStartStatus startListAllStreams(YouTubeListAllStreamsRequest request,
						       YouTubeApiDiscoveryPager::CompletionHandler handler)
	{
		return pager_->startListAllStreams(std::move(request), std::move(handler));
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept { return pager_->cancel(attempt); }
	bool shutdown() noexcept { return pager_->shutdown(); }
	YouTubeApiPagerState state() const noexcept { return pager_->state(); }
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept { return pager_->activeAttempt(); }
	void installEventFilter(QObject *filter) { pager_->installEventFilter(filter); }
	YouTubeApiDiscoveryPager *get() noexcept { return pager_.get(); }

private:
	std::unique_ptr<YouTubeApiDiscoveryPager> pager_;
};

} // namespace easy_multistream

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                 \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using easy_multistream::SecureBuffer;
using easy_multistream::YouTubeApiAttempt;
using easy_multistream::YouTubeApiCompletionStatus;
using easy_multistream::YouTubeApiDiscoveryOptions;
using easy_multistream::YouTubeApiDiscoveryPagerOptions;
using easy_multistream::YouTubeApiPagedCompletion;
using easy_multistream::YouTubeApiPagerStartStatus;
using easy_multistream::YouTubeApiPagerState;
using easy_multistream::YouTubeApiPagingFailure;
using easy_multistream::YouTubeApiProviderError;
using easy_multistream::YouTubeListAllChannelsRequest;
using easy_multistream::YouTubeListAllStreamsRequest;
using TestPager = easy_multistream::YouTubeApiDiscoveryPagerTestAccess;

struct FakeResponse final {
	int httpStatus = 200;
	QByteArray body = R"({"items":[]})";
	QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
	bool neverFinish = false;
};

class FakeReply final : public QNetworkReply {
public:
	FakeReply(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, FakeResponse response,
		  QObject *parent)
		: QNetworkReply(parent),
		  response_(std::move(response))
	{
		setOperation(operation);
		setRequest(request);
		setUrl(request.url());
		open(QIODevice::ReadOnly | QIODevice::Unbuffered);
		QTimer::singleShot(0, this, [this]() { emitResponse(); });
	}

	void abort() override
	{
		aborted_ = true;
		if (isFinished()) {
			return;
		}
		setError(QNetworkReply::OperationCanceledError, QStringLiteral("cancelled"));
		setFinished(true);
		emit errorOccurred(QNetworkReply::OperationCanceledError);
		emit finished();
	}

	bool aborted() const noexcept { return aborted_; }

	void emitLateFailure()
	{
		setError(QNetworkReply::RemoteHostClosedError, QStringLiteral("late failure"));
		emit errorOccurred(QNetworkReply::RemoteHostClosedError);
		setFinished(true);
		emit finished();
	}

	qint64 bytesAvailable() const override
	{
		return static_cast<qint64>(response_.body.size() - offset_) + QNetworkReply::bytesAvailable();
	}

protected:
	qint64 readData(char *data, qint64 maximumSize) override
	{
		if (offset_ >= response_.body.size()) {
			return -1;
		}
		const qint64 count = std::min<qint64>(maximumSize, response_.body.size() - offset_);
		std::memcpy(data, response_.body.constData() + offset_, static_cast<std::size_t>(count));
		offset_ += count;
		return count;
	}

private:
	void emitResponse()
	{
		if (response_.neverFinish || isFinished()) {
			return;
		}
		setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.httpStatus);
		setAttribute(QNetworkRequest::ConnectionEncryptedAttribute, true);
		setRawHeader("Content-Type", "application/json");
		emit metaDataChanged();
		if (!response_.body.isEmpty()) {
			emit readyRead();
		}
		if (response_.networkError != QNetworkReply::NoError) {
			setError(response_.networkError, QStringLiteral("sanitized fake error"));
			emit errorOccurred(response_.networkError);
		}
		setFinished(true);
		emit finished();
	}

	FakeResponse response_;
	qsizetype offset_ = 0;
	bool aborted_ = false;
};

struct CapturedRequest final {
	QNetworkAccessManager::Operation operation = QNetworkAccessManager::UnknownOperation;
	QNetworkRequest request;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager {
public:
	std::deque<FakeResponse> responses;
	std::vector<CapturedRequest> requests;
	std::vector<QPointer<FakeReply>> replies;

protected:
	QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override
	{
		requests.push_back({operation, request});
		FakeResponse response;
		if (!responses.empty()) {
			response = std::move(responses.front());
			responses.pop_front();
		} else {
			response.httpStatus = 503;
			response.body = R"({"error":{"errors":[{"reason":"backendError"}]}})";
			response.networkError = QNetworkReply::ServiceUnavailableError;
		}
		auto *reply = new FakeReply(operation, request, std::move(response), this);
		replies.emplace_back(reply);
		return reply;
	}
};

bool pumpUntil(const std::function<bool()> &condition, int maximumMilliseconds = 1500)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maximumMilliseconds);
	while (!condition() && std::chrono::steady_clock::now() < deadline) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	}
	return condition();
}

SecureBuffer token(std::string_view value = "access-token")
{
	return SecureBuffer::copyOf(value);
}

YouTubeListAllChannelsRequest channelsRequest(YouTubeApiAttempt attempt = {1, 1})
{
	YouTubeListAllChannelsRequest request;
	request.attempt = attempt;
	request.accessToken = token();
	return request;
}

YouTubeListAllStreamsRequest streamsRequest(YouTubeApiAttempt attempt = {1, 1}, std::string channelId = "UC-channel")
{
	YouTubeListAllStreamsRequest request;
	request.attempt = attempt;
	request.accessToken = token();
	request.channelId = std::move(channelId);
	return request;
}

FakeResponse response(QByteArray body)
{
	FakeResponse result;
	result.body = std::move(body);
	return result;
}

void checkRequestSecurity(const CapturedRequest &request)
{
	CHECK(request.operation == QNetworkAccessManager::GetOperation);
	CHECK(request.request.rawHeader("Authorization") == QByteArray("Bearer access-token"));
	CHECK(!request.request.url().toEncoded().contains("access-token"));
	CHECK(request.request.url().scheme() == QStringLiteral("https"));
	CHECK(request.request.url().host() == QStringLiteral("www.googleapis.com"));
}

void testTwoPageChannelsAndEmptyContinuation()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *raw = manager.get();
	manager->responses.push_back(
		response(R"({"nextPageToken":"page-2","items":[{"id":"UC-one","snippet":{"title":"One"}}]})"));
	manager->responses.push_back(response(R"({"nextPageToken":"page-3","items":[]})"));
	manager->responses.push_back(response(R"({"items":[{"id":"UC-two","snippet":{"title":"Two"}}]})"));

	TestPager pager(std::move(manager));
	std::optional<YouTubeApiPagedCompletion> completion;
	CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiPagerStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
	CHECK(completion->channels.size() == 2);
	CHECK(completion->channels[0].id == "UC-one");
	CHECK(completion->channels[1].id == "UC-two");
	CHECK(completion->streams.empty());
	CHECK(raw->requests.size() == 3);
	for (const auto &request : raw->requests) {
		checkRequestSecurity(request);
	}
	CHECK(QUrlQuery(raw->requests[0].request.url()).queryItemValue(QStringLiteral("pageToken")).isEmpty());
	CHECK(QUrlQuery(raw->requests[1].request.url()).queryItemValue(QStringLiteral("pageToken")) ==
	      QStringLiteral("page-2"));
	CHECK(QUrlQuery(raw->requests[2].request.url()).queryItemValue(QStringLiteral("pageToken")) ==
	      QStringLiteral("page-3"));
	CHECK(pager.state() == YouTubeApiPagerState::Completed);
}

void testTwoPageStreams()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *raw = manager.get();
	manager->responses.push_back(response(
		R"({"nextPageToken":"more","items":[{"id":"stream-1","snippet":{"channelId":"UC-channel","title":"Primary"}},{"id":"ignored","snippet":{"channelId":"UC-other","title":"Other"}}]})"));
	manager->responses.push_back(
		response(R"({"items":[{"id":"stream-2","snippet":{"channelId":"UC-channel","title":"Backup"}}]})"));

	TestPager pager(std::move(manager));
	std::optional<YouTubeApiPagedCompletion> completion;
	CHECK(pager.startListAllStreams(streamsRequest(), [&](YouTubeApiPagedCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiPagerStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
	CHECK(completion->channels.empty());
	CHECK(completion->streams.size() == 2);
	CHECK(completion->streams[0].id == "stream-1");
	CHECK(completion->streams[1].id == "stream-2");
	CHECK(raw->requests.size() == 2);
	CHECK(QUrlQuery(raw->requests[1].request.url()).queryItemValue(QStringLiteral("pageToken")) ==
	      QStringLiteral("more"));
}

void testSinglePageAndInputValidation()
{
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *raw = manager.get();
		manager->responses.push_back(response(R"({"items":[]})"));
		TestPager pager(std::move(manager));
		std::optional<YouTubeApiPagedCompletion> completion;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->succeeded());
		CHECK(completion->channels.empty());
		CHECK(raw->requests.size() == 1);
	}
	{
		YouTubeApiDiscoveryPagerOptions options;
		options.maxPages = 0;
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		TestPager pager(std::move(manager), options);
		CHECK(pager.startListAllChannels(channelsRequest(), [](YouTubeApiPagedCompletion) {}) ==
		      YouTubeApiPagerStartStatus::InvalidOptions);
	}
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		TestPager pager(std::move(manager));
		auto request = channelsRequest({0, 1});
		CHECK(pager.startListAllChannels(std::move(request), [](YouTubeApiPagedCompletion) {}) ==
		      YouTubeApiPagerStartStatus::InvalidAttempt);
		CHECK(pager.startListAllChannels(channelsRequest({1, 2}), {}) ==
		      YouTubeApiPagerStartStatus::InvalidCompletionHandler);
		auto invalidToken = channelsRequest({1, 3});
		invalidToken.accessToken.clear();
		CHECK(pager.startListAllChannels(std::move(invalidToken), [](YouTubeApiPagedCompletion) {}) ==
		      YouTubeApiPagerStartStatus::InvalidAccessToken);
		CHECK(pager.startListAllStreams(streamsRequest({1, 4}, {}), [](YouTubeApiPagedCompletion) {}) ==
		      YouTubeApiPagerStartStatus::InvalidChannelId);
	}
}

void testPageAndItemLimitsFailWithoutPartialResults()
{
	{
		YouTubeApiDiscoveryPagerOptions options;
		options.maxPages = 1;
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *raw = manager.get();
		manager->responses.push_back(response(
			R"({"nextPageToken":"not-fetched","items":[{"id":"UC-one","snippet":{"title":"One"}}]})"));
		TestPager pager(std::move(manager), options);
		std::optional<YouTubeApiPagedCompletion> completion;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(!completion->succeeded());
		CHECK(completion->pagingFailure == YouTubeApiPagingFailure::PageLimitExceeded);
		CHECK(completion->channels.empty());
		CHECK(raw->requests.size() == 1);
	}
	{
		YouTubeApiDiscoveryPagerOptions options;
		options.maxItems = 1;
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		manager->responses.push_back(
			response(R"({"nextPageToken":"next","items":[{"id":"UC-one","snippet":{"title":"One"}}]})"));
		manager->responses.push_back(response(R"({"items":[{"id":"UC-two","snippet":{"title":"Two"}}]})"));
		TestPager pager(std::move(manager), options);
		std::optional<YouTubeApiPagedCompletion> completion;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->pagingFailure == YouTubeApiPagingFailure::ItemLimitExceeded);
		CHECK(completion->channels.empty());
	}
}

void testCycleAndCrossPageDuplicateFailClosed()
{
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *raw = manager.get();
		manager->responses.push_back(response(R"({"nextPageToken":"page-a","items":[]})"));
		manager->responses.push_back(response(R"({"nextPageToken":"page-b","items":[]})"));
		manager->responses.push_back(response(R"({"nextPageToken":"page-a","items":[]})"));
		TestPager pager(std::move(manager));
		std::optional<YouTubeApiPagedCompletion> completion;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->pagingFailure == YouTubeApiPagingFailure::PageTokenCycle);
		CHECK(completion->channels.empty());
		CHECK(raw->requests.size() == 3);
	}
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		manager->responses.push_back(response(
			R"({"nextPageToken":"next","items":[{"id":"UC-duplicate","snippet":{"title":"One"}}]})"));
		manager->responses.push_back(
			response(R"({"items":[{"id":"UC-duplicate","snippet":{"title":"Changed"}}]})"));
		TestPager pager(std::move(manager));
		std::optional<YouTubeApiPagedCompletion> completion;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->pagingFailure == YouTubeApiPagingFailure::DuplicateItem);
		CHECK(completion->channels.empty());
	}
}

void testSecondPageProviderFailureIsPreserved()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	manager->responses.push_back(response(R"({"nextPageToken":"next","items":[]})"));
	FakeResponse rejected;
	rejected.httpStatus = 403;
	rejected.networkError = QNetworkReply::ContentAccessDenied;
	rejected.body = R"({"error":{"errors":[{"reason":"quotaExceeded"}]}})";
	manager->responses.push_back(std::move(rejected));
	TestPager pager(std::move(manager));
	std::optional<YouTubeApiPagedCompletion> completion;
	CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiPagerStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->status == YouTubeApiCompletionStatus::ProviderRejected);
	CHECK(completion->providerError == YouTubeApiProviderError::QuotaExceeded);
	CHECK(completion->httpStatus == 403);
	CHECK(completion->pagingFailure == YouTubeApiPagingFailure::None);
	CHECK(completion->channels.empty());
}

void testSecondPageTransportFailureIsPreserved()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	manager->responses.push_back(response(R"({"nextPageToken":"next","items":[]})"));
	FakeResponse failed;
	failed.httpStatus = 0;
	failed.networkError = QNetworkReply::RemoteHostClosedError;
	failed.body.clear();
	manager->responses.push_back(std::move(failed));
	TestPager pager(std::move(manager));
	std::optional<YouTubeApiPagedCompletion> completion;
	CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiPagerStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->status == YouTubeApiCompletionStatus::NetworkFailure);
	CHECK(completion->providerError == YouTubeApiProviderError::None);
	CHECK(completion->pagingFailure == YouTubeApiPagingFailure::None);
	CHECK(completion->channels.empty());
}

void testCancelLateReplyAndNewAttemptIsolation()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *raw = manager.get();
	manager->responses.push_back(response(R"({"nextPageToken":"next","items":[]})"));
	FakeResponse blocked;
	blocked.neverFinish = true;
	manager->responses.push_back(std::move(blocked));
	TestPager pager(std::move(manager));
	int oldCompletions = 0;
	CHECK(pager.startListAllChannels(channelsRequest({7, 1}), [&](YouTubeApiPagedCompletion) {
		++oldCompletions;
	}) == YouTubeApiPagerStartStatus::Started);
	CHECK(pumpUntil([&]() { return raw->requests.size() == 2; }));
	CHECK(raw->replies.size() == 2);
	QPointer<FakeReply> oldReply = raw->replies[1];
	CHECK(!pager.cancel({7, 2}));
	CHECK(pager.cancel({7, 1}));
	CHECK(pager.state() == YouTubeApiPagerState::Cancelled);
	CHECK(oldReply != nullptr && oldReply->aborted());

	raw->responses.push_back(response(R"({"items":[{"id":"UC-new","snippet":{"title":"New"}}]})"));
	std::optional<YouTubeApiPagedCompletion> newCompletion;
	CHECK(pager.startListAllChannels(channelsRequest({7, 2}), [&](YouTubeApiPagedCompletion value) {
		newCompletion.emplace(std::move(value));
	}) == YouTubeApiPagerStartStatus::Started);
	if (oldReply != nullptr) {
		oldReply->emitLateFailure();
	}
	CHECK(pumpUntil([&]() { return newCompletion.has_value(); }));
	CHECK(oldCompletions == 0);
	CHECK(newCompletion->succeeded());
	CHECK(newCompletion->attempt == (YouTubeApiAttempt{7, 2}));
	CHECK(newCompletion->channels.size() == 1);
	CHECK(newCompletion->channels[0].id == "UC-new");
}

class RebindBeforePagerCompletion final : public QObject {
public:
	TestPager *pager = nullptr;
	std::optional<YouTubeApiPagedCompletion> *newCompletion = nullptr;
	int *newCompletionCount = nullptr;
	bool triggered = false;
	YouTubeApiPagerStartStatus startStatus = YouTubeApiPagerStartStatus::Busy;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!triggered && watched == pager->get() && event->type() == QEvent::MetaCall &&
		    pager->state() == YouTubeApiPagerState::Completed) {
			triggered = true;
			CHECK(pager->cancel({12, 1}));
			startStatus = pager->startListAllChannels(channelsRequest({12, 2}),
								  [this](YouTubeApiPagedCompletion value) {
									  ++*newCompletionCount;
									  newCompletion->emplace(std::move(value));
								  });
		}
		return QObject::eventFilter(watched, event);
	}
};

void testQueuedCompletionCannotCrossRebind()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	manager->responses.push_back(response(R"({"items":[{"id":"UC-old","snippet":{"title":"Old"}}]})"));
	manager->responses.push_back(response(R"({"items":[{"id":"UC-new","snippet":{"title":"New"}}]})"));
	TestPager pager(std::move(manager));
	int oldCompletionCount = 0;
	int newCompletionCount = 0;
	std::optional<YouTubeApiPagedCompletion> newCompletion;
	RebindBeforePagerCompletion filter;
	filter.pager = &pager;
	filter.newCompletion = &newCompletion;
	filter.newCompletionCount = &newCompletionCount;
	pager.installEventFilter(&filter);

	CHECK(pager.startListAllChannels(channelsRequest({12, 1}), [&](YouTubeApiPagedCompletion) {
		++oldCompletionCount;
	}) == YouTubeApiPagerStartStatus::Started);
	CHECK(pumpUntil([&]() { return newCompletion.has_value(); }));
	CHECK(filter.triggered);
	CHECK(filter.startStatus == YouTubeApiPagerStartStatus::Started);
	CHECK(oldCompletionCount == 0);
	CHECK(newCompletionCount == 1);
	CHECK(newCompletion->attempt == (YouTubeApiAttempt{12, 2}));
	CHECK(newCompletion->channels.size() == 1);
	CHECK(newCompletion->channels[0].id == "UC-new");
}

void testTotalTimeoutShutdownAndDestructionAreSilentWhereRequired()
{
	{
		YouTubeApiDiscoveryPagerOptions options;
		options.totalTimeout = std::chrono::milliseconds(20);
		YouTubeApiDiscoveryOptions pageOptions;
		pageOptions.operationTimeout = std::chrono::seconds(2);
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *raw = manager.get();
		FakeResponse blocked;
		blocked.neverFinish = true;
		manager->responses.push_back(std::move(blocked));
		TestPager pager(std::move(manager), options, pageOptions);
		std::optional<YouTubeApiPagedCompletion> completion;
		int completions = 0;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion value) {
			++completions;
			completion.emplace(std::move(value));
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->status == YouTubeApiCompletionStatus::TimedOut);
		CHECK(completion->channels.empty());
		CHECK(raw->replies.size() == 1);
		if (raw->replies[0] != nullptr) {
			raw->replies[0]->emitLateFailure();
		}
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		CHECK(completions == 1);
	}
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeResponse blocked;
		blocked.neverFinish = true;
		manager->responses.push_back(std::move(blocked));
		TestPager pager(std::move(manager));
		int completions = 0;
		CHECK(pager.startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion) {
			++completions;
		}) == YouTubeApiPagerStartStatus::Started);
		CHECK(pager.shutdown());
		CHECK(pager.shutdown());
		CHECK(pager.state() == YouTubeApiPagerState::Closed);
		CHECK(pager.startListAllChannels(channelsRequest({1, 2}), [](YouTubeApiPagedCompletion) {}) ==
		      YouTubeApiPagerStartStatus::Closed);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		CHECK(completions == 0);
	}
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeResponse blocked;
		blocked.neverFinish = true;
		manager->responses.push_back(std::move(blocked));
		int completions = 0;
		{
			auto pager = std::make_unique<TestPager>(std::move(manager));
			CHECK(pager->startListAllChannels(channelsRequest(), [&](YouTubeApiPagedCompletion) {
				++completions;
			}) == YouTubeApiPagerStartStatus::Started);
		}
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		CHECK(completions == 0);
	}
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testTwoPageChannelsAndEmptyContinuation();
	testTwoPageStreams();
	testSinglePageAndInputValidation();
	testPageAndItemLimitsFailWithoutPartialResults();
	testCycleAndCrossPageDuplicateFailClosed();
	testSecondPageProviderFailureIsPreserved();
	testSecondPageTransportFailureIsPreserved();
	testCancelLateReplyAndNewAttemptIsolation();
	testQueuedCompletionCannotCrossRebind();
	testTotalTimeoutShutdownAndDestructionAreSilentWhereRequired();

	if (failures != 0) {
		std::cerr << failures << " YouTube discovery pager test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube discovery pager tests passed\n";
	return 0;
}

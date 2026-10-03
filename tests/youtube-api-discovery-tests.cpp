// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-api-discovery.hpp"

#include <QAuthenticator>
#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace easy_multistream {

class YouTubeApiDiscoveryTestAccess final {
public:
	explicit YouTubeApiDiscoveryTestAccess(std::unique_ptr<QNetworkAccessManager> manager,
					       YouTubeApiDiscoveryOptions options = {})
		: transport_(new YouTubeApiDiscovery(std::move(manager), options, nullptr))
	{
	}

	YouTubeApiStartStatus startListChannels(YouTubeListChannelsRequest request,
						YouTubeApiDiscovery::CompletionHandler handler)
	{
		return transport_->startListChannels(std::move(request), std::move(handler));
	}

	YouTubeApiStartStatus startListStreams(YouTubeListStreamsRequest request,
					       YouTubeApiDiscovery::CompletionHandler handler)
	{
		return transport_->startListStreams(std::move(request), std::move(handler));
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept { return transport_->cancel(attempt); }
	bool shutdown() noexcept { return transport_->shutdown(); }
	YouTubeApiTransportState state() const noexcept { return transport_->state(); }
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept { return transport_->activeAttempt(); }
	void installEventFilter(QObject *filter) { transport_->installEventFilter(filter); }
	YouTubeApiDiscovery *get() noexcept { return transport_.get(); }

private:
	std::unique_ptr<YouTubeApiDiscovery> transport_;
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
using easy_multistream::YouTubeApiCompletion;
using easy_multistream::YouTubeApiCompletionStatus;
using easy_multistream::YouTubeApiDiscoveryOptions;
using easy_multistream::YouTubeApiOperation;
using easy_multistream::YouTubeApiProviderError;
using easy_multistream::YouTubeApiStartStatus;
using easy_multistream::YouTubeApiTransportState;
using easy_multistream::YouTubeListChannelsRequest;
using easy_multistream::YouTubeListStreamsRequest;
using TestDiscovery = easy_multistream::YouTubeApiDiscoveryTestAccess;

struct FakeResponse final {
	int httpStatus = 200;
	QByteArray contentType = "application/json";
	QByteArray contentEncoding;
	QByteArray body = R"({"items":[]})";
	QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
	bool sslFailure = false;
	bool neverFinish = false;
	QUrl redirectTarget;
	std::vector<std::pair<QByteArray, QByteArray>> extraHeaders;
	std::optional<bool> encrypted = true;
	QUrl finalUrl;
};

class FakeReply final : public QNetworkReply {
public:
	FakeReply(QNetworkAccessManager::Operation operation, const QNetworkRequest &request, FakeResponse response,
		  int *ignoreSslErrorsCount, QObject *parent)
		: QNetworkReply(parent),
		  response_(std::move(response)),
		  ignoreSslErrorsCount_(ignoreSslErrorsCount)
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

	void emitLateFailureAndFinish()
	{
		setError(QNetworkReply::RemoteHostClosedError, QStringLiteral("late fake failure"));
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

	void ignoreSslErrorsImplementation(const QList<QSslError> &) override
	{
		if (ignoreSslErrorsCount_ != nullptr) {
			++*ignoreSslErrorsCount_;
		}
	}

private:
	void emitResponse()
	{
		if (response_.neverFinish || isFinished()) {
			return;
		}
		if (response_.sslFailure) {
			emit sslErrors({QSslError(QSslError::SelfSignedCertificate)});
			return;
		}
		if (response_.httpStatus > 0) {
			setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.httpStatus);
		}
		if (response_.encrypted.has_value()) {
			setAttribute(QNetworkRequest::ConnectionEncryptedAttribute, *response_.encrypted);
		}
		if (!response_.finalUrl.isEmpty()) {
			setUrl(response_.finalUrl);
		}
		if (!response_.contentType.isNull()) {
			setRawHeader("Content-Type", response_.contentType);
		}
		if (!response_.contentEncoding.isEmpty()) {
			setRawHeader("Content-Encoding", response_.contentEncoding);
		}
		for (const auto &header : response_.extraHeaders) {
			setRawHeader(header.first, header.second);
		}
		if (!response_.redirectTarget.isEmpty()) {
			setAttribute(QNetworkRequest::RedirectionTargetAttribute, response_.redirectTarget);
		}
		emit metaDataChanged();
		if (!response_.redirectTarget.isEmpty()) {
			emit redirected(response_.redirectTarget);
			if (isFinished()) {
				return;
			}
		}
		if (!response_.body.isEmpty()) {
			emit readyRead();
			if (isFinished()) {
				return;
			}
		}
		if (response_.networkError != QNetworkReply::NoError) {
			setError(response_.networkError, QStringLiteral("sanitized fake failure"));
			emit errorOccurred(response_.networkError);
		}
		setFinished(true);
		emit finished();
	}

	FakeResponse response_;
	qsizetype offset_ = 0;
	int *ignoreSslErrorsCount_ = nullptr;
	bool aborted_ = false;
};

struct CapturedRequest final {
	QNetworkAccessManager::Operation operation = QNetworkAccessManager::UnknownOperation;
	QNetworkRequest request;
	QByteArray body;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager {
public:
	std::deque<FakeResponse> responses;
	std::vector<CapturedRequest> requests;
	std::vector<QPointer<FakeReply>> replies;
	int ignoreSslErrorsCount = 0;
	void emitAuthenticationChallenge(QNetworkReply *reply)
	{
		QAuthenticator authenticator;
		emit authenticationRequired(reply, &authenticator);
	}

protected:
	QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
				     QIODevice *outgoingData) override
	{
		CapturedRequest captured;
		captured.operation = operation;
		captured.request = request;
		if (outgoingData != nullptr) {
			captured.body = outgoingData->readAll();
		}
		requests.emplace_back(std::move(captured));

		FakeResponse response;
		if (!responses.empty()) {
			response = std::move(responses.front());
			responses.pop_front();
		} else {
			response.httpStatus = 503;
			response.body = R"({"error":{"errors":[{"reason":"backendError"}]}})";
			response.networkError = QNetworkReply::ServiceUnavailableError;
		}
		auto *reply = new FakeReply(operation, request, std::move(response), &ignoreSslErrorsCount, this);
		replies.emplace_back(reply);
		return reply;
	}
};

SecureBuffer secret(std::string_view value)
{
	return SecureBuffer::copyOf(value);
}

YouTubeListChannelsRequest channelRequest(YouTubeApiAttempt attempt = {1, 1},
					  std::string_view accessToken = "access-token", std::string pageToken = {})
{
	YouTubeListChannelsRequest request;
	request.attempt = attempt;
	request.accessToken = secret(accessToken);
	request.pageToken = std::move(pageToken);
	return request;
}

YouTubeListStreamsRequest streamRequest(YouTubeApiAttempt attempt = {1, 1},
					std::string_view accessToken = "access-token",
					std::string channelId = "UC-channel", std::string pageToken = {})
{
	YouTubeListStreamsRequest request;
	request.attempt = attempt;
	request.accessToken = secret(accessToken);
	request.channelId = std::move(channelId);
	request.pageToken = std::move(pageToken);
	return request;
}

bool pumpUntil(const std::function<bool()> &condition, int maximumMilliseconds = 1000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maximumMilliseconds);
	while (!condition() && std::chrono::steady_clock::now() < deadline) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	}
	return condition();
}

std::optional<YouTubeApiCompletion> runChannels(FakeNetworkAccessManager *manager, FakeResponse response,
						YouTubeApiDiscoveryOptions options = {},
						int *ignoreSslErrorsCount = nullptr)
{
	manager->responses.push_back(std::move(response));
	TestDiscovery discovery(std::unique_ptr<QNetworkAccessManager>(manager), options);
	std::optional<YouTubeApiCompletion> completion;
	CHECK(discovery.startListChannels(channelRequest(), [&](YouTubeApiCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }, 1500));
	if (ignoreSslErrorsCount != nullptr) {
		*ignoreSslErrorsCount = manager->ignoreSslErrorsCount;
	}
	return completion;
}

void checkSecureRequest(const CapturedRequest &captured, const QString &expectedPath)
{
	CHECK(captured.operation == QNetworkAccessManager::GetOperation);
	CHECK(captured.body.isEmpty());
	CHECK(captured.request.url().scheme() == QStringLiteral("https"));
	CHECK(captured.request.url().host() == QStringLiteral("www.googleapis.com"));
	CHECK(captured.request.url().port() == -1);
	CHECK(captured.request.url().path() == expectedPath);
	CHECK(captured.request.url().userInfo().isEmpty());
	CHECK(!captured.request.url().hasFragment());
	CHECK(captured.request.rawHeader("Authorization") == QByteArray("Bearer access-token"));
	CHECK(!captured.request.url().toEncoded().contains("access-token"));
	CHECK(captured.request.rawHeader("Accept") == QByteArray("application/json"));
	CHECK(captured.request.rawHeader("Accept-Encoding") == QByteArray("identity"));
	CHECK(captured.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() ==
	      QNetworkRequest::ManualRedirectPolicy);
	CHECK(captured.request.maximumRedirectsAllowed() == 0);
	CHECK(captured.request.sslConfiguration().peerVerifyMode() == QSslSocket::VerifyPeer);
	CHECK(captured.request.sslConfiguration().protocol() == QSsl::TlsV1_2OrLater);
}

void testChannelsSuccessAndRequestContract()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *rawManager = manager.get();
	FakeResponse response;
	response.body =
		R"({"nextPageToken":"page-2","items":[{"id":"UC-one","snippet":{"title":"One"}},{"id":"UC-two","snippet":{"title":"日本語 😀"}}]})";
	manager->responses.push_back(std::move(response));

	TestDiscovery discovery(std::move(manager));
	std::optional<YouTubeApiCompletion> completion;
	CHECK(discovery.startListChannels(channelRequest(), [&](YouTubeApiCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiStartStatus::Started);
	CHECK(!completion.has_value());
	CHECK(discovery.state() == YouTubeApiTransportState::InFlight);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
	CHECK(completion->operation == YouTubeApiOperation::ListOwnedChannels);
	CHECK(completion->channels.size() == 2);
	CHECK(completion->channels[0].id == "UC-one");
	CHECK(completion->channels[0].label == "One");
	CHECK(completion->channels[1].id == "UC-two");
	CHECK(completion->channels[1].label == u8"日本語 😀");
	CHECK(completion->nextPageToken == "page-2");
	CHECK(completion->streams.empty());
	CHECK(discovery.state() == YouTubeApiTransportState::Completed);
	CHECK(rawManager->requests.size() == 1);
	checkSecureRequest(rawManager->requests[0], QStringLiteral("/youtube/v3/channels"));
	const QUrlQuery query(rawManager->requests[0].request.url());
	CHECK(query.queryItemValue(QStringLiteral("part")) == QStringLiteral("id,snippet"));
	CHECK(query.queryItemValue(QStringLiteral("mine")) == QStringLiteral("true"));
	CHECK(query.queryItemValue(QStringLiteral("maxResults")) == QStringLiteral("50"));
	CHECK(query.queryItemValue(QStringLiteral("pageToken")).isEmpty());
	CHECK(query.queryItemValue(QStringLiteral("key")).isEmpty());
	CHECK(query.queryItemValue(QStringLiteral("fields")) ==
	      QStringLiteral("nextPageToken,items(id,snippet(title))"));
}

void testChannelContinuationAndEmptyPage()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *rawManager = manager.get();
	manager->responses.push_back(FakeResponse{});
	TestDiscovery discovery(std::move(manager));
	std::optional<YouTubeApiCompletion> completion;
	CHECK(discovery.startListChannels(channelRequest({3, 7}, "access-token", "opaque+/="),
					  [&](YouTubeApiCompletion value) { completion.emplace(std::move(value)); }) ==
	      YouTubeApiStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
	CHECK(completion->channels.empty());
	CHECK(completion->nextPageToken.empty());
	CHECK(QUrlQuery(rawManager->requests[0].request.url()).queryItemValue(QStringLiteral("pageToken")) ==
	      QStringLiteral("opaque+/="));
}

void testStreamsSuccessAndChannelFilter()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *rawManager = manager.get();
	FakeResponse response;
	response.body =
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Primary"}},{"id":"stream-b","snippet":{"channelId":"UC-other","title":"Other"}}]})";
	manager->responses.push_back(std::move(response));
	TestDiscovery discovery(std::move(manager));
	std::optional<YouTubeApiCompletion> completion;
	CHECK(discovery.startListStreams(streamRequest({2, 4}, "access-token", "UC-selected"),
					 [&](YouTubeApiCompletion value) { completion.emplace(std::move(value)); }) ==
	      YouTubeApiStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
	CHECK(completion->operation == YouTubeApiOperation::ListReusableStreams);
	CHECK(completion->streams.size() == 1);
	CHECK(completion->streams[0].id == "stream-a");
	CHECK(completion->streams[0].channelId == "UC-selected");
	CHECK(completion->streams[0].label == "Primary");
	CHECK(completion->channels.empty());
	checkSecureRequest(rawManager->requests[0], QStringLiteral("/youtube/v3/liveStreams"));
	const QUrlQuery query(rawManager->requests[0].request.url());
	CHECK(query.queryItemValue(QStringLiteral("part")) == QStringLiteral("id,snippet"));
	CHECK(query.queryItemValue(QStringLiteral("mine")) == QStringLiteral("true"));
	CHECK(query.queryItemValue(QStringLiteral("fields")) ==
	      QStringLiteral("nextPageToken,items(id,snippet(channelId,title))"));
	CHECK(!rawManager->requests[0].request.url().toEncoded().contains("streamName"));
	CHECK(!rawManager->requests[0].request.url().toEncoded().contains("cdn"));
}

void testInputValidationAndBusyState()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *rawManager = manager.get();
	FakeResponse pending;
	pending.neverFinish = true;
	manager->responses.push_back(std::move(pending));
	TestDiscovery discovery(std::move(manager));
	auto handler = [](YouTubeApiCompletion) {
	};

	CHECK(discovery.startListChannels(channelRequest({0, 1}), handler) == YouTubeApiStartStatus::InvalidAttempt);
	CHECK(discovery.startListChannels(channelRequest({1, 1}, "bad token"), handler) ==
	      YouTubeApiStartStatus::InvalidAccessToken);
	CHECK(discovery.startListChannels(
		      channelRequest({1, 1}, std::string(easy_multistream::kYouTubeApiMaxAccessTokenBytes + 1, 'x')),
		      handler) == YouTubeApiStartStatus::InvalidAccessToken);
	CHECK(discovery.startListChannels(channelRequest({1, 1}, "access-token", "bad page"), handler) ==
	      YouTubeApiStartStatus::InvalidPageToken);
	CHECK(discovery.startListStreams(streamRequest({1, 1}, "access-token", "bad channel"), handler) ==
	      YouTubeApiStartStatus::InvalidChannelId);
	CHECK(discovery.startListChannels(channelRequest(), {}) == YouTubeApiStartStatus::InvalidCompletionHandler);
	CHECK(rawManager->requests.empty());

	CHECK(discovery.startListChannels(channelRequest(), handler) == YouTubeApiStartStatus::Started);
	CHECK(discovery.startListChannels(channelRequest({1, 2}), handler) == YouTubeApiStartStatus::Busy);
	CHECK(discovery.cancel({1, 1}));
}

YouTubeApiCompletion runInvalidChannelsBody(const QByteArray &body)
{
	auto *manager = new FakeNetworkAccessManager();
	FakeResponse response;
	response.body = body;
	auto completion = runChannels(manager, std::move(response));
	CHECK(completion.has_value());
	return std::move(*completion);
}

void testStrictChannelParsing()
{
	const std::vector<QByteArray> invalidBodies = {
		R"({})",
		R"({"items":null})",
		R"({"items":["bad"]})",
		R"({"items":[{"snippet":{"title":"name"}}]})",
		R"({"items":[{"id":"","snippet":{"title":"name"}}]})",
		R"({"items":[{"id":"UC-a","snippet":null}]})",
		R"({"items":[{"id":"UC-a","snippet":{"title":7}}]})",
		R"({"items":[{"id":"UC-a","snippet":{"title":"line\nbreak"}}]})",
		R"({"items":[{"id":"UC-a","id":"UC-b","snippet":{"title":"name"}}]})",
		R"({"items":[],"items":[]})",
		R"({"items":[{"id":"UC-a","snippet":{"title":"one"}},{"id":"UC-a","snippet":{"title":"two"}}]})",
		R"({"items":[],"nextPageToken":7})",
		R"({"items":[],"nextPageToken":""})",
		R"({"items":[],"nextPageToken":"bad page"})",
		R"({"error":{},"items":[]})",
	};
	for (const QByteArray &body : invalidBodies) {
		YouTubeApiCompletion completion = runInvalidChannelsBody(body);
		CHECK(completion.status == YouTubeApiCompletionStatus::InvalidResponse);
	}

	QByteArray invalidUtf8 = R"({"items":[{"id":"UC-a","snippet":{"title":")";
	invalidUtf8.append(static_cast<char>(0xFF));
	invalidUtf8.append(R"("}}]})");
	CHECK(runInvalidChannelsBody(invalidUtf8).status == YouTubeApiCompletionStatus::InvalidResponse);

	QByteArray tooMany = R"({"items":[)";
	for (int index = 0; index < 51; ++index) {
		if (index != 0) {
			tooMany.append(',');
		}
		tooMany.append("{\"id\":\"UC-");
		tooMany.append(QByteArray::number(index));
		tooMany.append("\",\"snippet\":{\"title\":\"name\"}}");
	}
	tooMany.append("]}");
	CHECK(runInvalidChannelsBody(tooMany).status == YouTubeApiCompletionStatus::InvalidResponse);

	QByteArray tooDeep = R"({"items":[],"unknown":)";
	tooDeep.append(QByteArray(70, '['));
	tooDeep.append('0');
	tooDeep.append(QByteArray(70, ']'));
	tooDeep.append('}');
	CHECK(runInvalidChannelsBody(tooDeep).status == YouTubeApiCompletionStatus::InvalidResponse);
}

void testStrictStreamParsing()
{
	const std::vector<QByteArray> invalidBodies = {
		R"({"items":[{"id":"stream","snippet":{"title":"name"}}]})",
		R"({"items":[{"id":"stream","snippet":{"channelId":7,"title":"name"}}]})",
		R"({"items":[{"id":"stream","snippet":{"channelId":"UC-selected","title":null}}]})",
		R"({"items":[{"id":"stream","snippet":{"channelId":"UC-selected","title":"one"}},{"id":"stream","snippet":{"channelId":"UC-selected","title":"two"}}]})",
		R"({"items":[{"id":"stream","snippet":{"channelId":"UC-selected","channelId":"UC-other","title":"name"}}]})",
	};
	for (const QByteArray &body : invalidBodies) {
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeResponse response;
		response.body = body;
		manager->responses.push_back(std::move(response));
		TestDiscovery discovery(std::move(manager));
		std::optional<YouTubeApiCompletion> completion;
		CHECK(discovery.startListStreams(streamRequest({1, 1}, "access-token", "UC-selected"),
						 [&](YouTubeApiCompletion value) {
							 completion.emplace(std::move(value));
						 }) == YouTubeApiStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->status == YouTubeApiCompletionStatus::InvalidResponse);
	}
}

void testProviderErrorClassification()
{
	struct Case final {
		int httpStatus;
		QNetworkReply::NetworkError networkError;
		const char *reason;
		YouTubeApiProviderError expected;
	};
	const std::vector<Case> cases = {
		{401, QNetworkReply::AuthenticationRequiredError, "authError", YouTubeApiProviderError::InvalidToken},
		{400, QNetworkReply::ProtocolInvalidOperationError, "invalidPageToken",
		 YouTubeApiProviderError::InvalidPageToken},
		{403, QNetworkReply::ContentAccessDenied, "insufficientPermissions",
		 YouTubeApiProviderError::InsufficientPermissions},
		{403, QNetworkReply::ContentAccessDenied, "insufficientLivePermissions",
		 YouTubeApiProviderError::InsufficientPermissions},
		{403, QNetworkReply::ContentAccessDenied, "liveStreamingNotEnabled",
		 YouTubeApiProviderError::LiveStreamingNotEnabled},
		{403, QNetworkReply::ContentAccessDenied, "quotaExceeded", YouTubeApiProviderError::QuotaExceeded},
		{403, QNetworkReply::ContentAccessDenied, "rateLimitExceeded", YouTubeApiProviderError::RateLimited},
		{403, QNetworkReply::ContentAccessDenied, "userRequestsExceedRateLimit",
		 YouTubeApiProviderError::RateLimited},
		{403, QNetworkReply::ContentAccessDenied, "unknownReason", YouTubeApiProviderError::PermissionDenied},
		{404, QNetworkReply::ContentNotFoundError, "notFound", YouTubeApiProviderError::NotFound},
		{429, QNetworkReply::UnknownContentError, "rateLimitExceeded", YouTubeApiProviderError::RateLimited},
		{503, QNetworkReply::ServiceUnavailableError, "backendError",
		 YouTubeApiProviderError::TemporarilyUnavailable},
	};
	for (const Case &testCase : cases) {
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.httpStatus = testCase.httpStatus;
		response.networkError = testCase.networkError;
		response.body = QByteArray("{\"error\":{\"errors\":[{\"reason\":\"") + testCase.reason +
				"\",\"message\":\"must not escape\"}],\"message\":\"raw provider text\"}}";
		auto completion = runChannels(manager, std::move(response));
		CHECK(completion->status == YouTubeApiCompletionStatus::ProviderRejected);
		CHECK(completion->providerError == testCase.expected);
		CHECK(completion->channels.empty());
		CHECK(completion->nextPageToken.empty());
	}
}

void testAuthenticationChallengeStillClassifies401()
{
	for (const int httpStatus : {401, 0}) {
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *rawManager = manager.get();
		FakeResponse response;
		response.httpStatus = httpStatus;
		response.networkError = QNetworkReply::AuthenticationRequiredError;
		response.body = R"({"error":{"errors":[{"reason":"authError"}]}})";
		manager->responses.push_back(std::move(response));
		TestDiscovery discovery(std::move(manager));
		std::optional<YouTubeApiCompletion> completion;
		CHECK(discovery.startListChannels(channelRequest(), [&](YouTubeApiCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiStartStatus::Started);
		CHECK(rawManager->replies.size() == 1);
		rawManager->emitAuthenticationChallenge(rawManager->replies[0]);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->status == YouTubeApiCompletionStatus::ProviderRejected);
		CHECK(completion->providerError == YouTubeApiProviderError::InvalidToken);
	}
}

void testLateAuthenticationChallengeCannotAffectNewAttempt()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *rawManager = manager.get();
	FakeResponse oldResponse;
	oldResponse.neverFinish = true;
	manager->responses.push_back(std::move(oldResponse));
	FakeResponse newResponse;
	newResponse.httpStatus = 0;
	newResponse.networkError = QNetworkReply::AuthenticationRequiredError;
	newResponse.encrypted = true;
	manager->responses.push_back(std::move(newResponse));
	TestDiscovery discovery(std::move(manager));
	int oldCompletions = 0;
	CHECK(discovery.startListChannels(channelRequest({9, 1}), [&](YouTubeApiCompletion) { ++oldCompletions; }) ==
	      YouTubeApiStartStatus::Started);
	QPointer<FakeReply> oldReply = rawManager->replies[0];
	CHECK(discovery.cancel({9, 1}));
	std::optional<YouTubeApiCompletion> completion;
	CHECK(discovery.startListChannels(channelRequest({9, 2}), [&](YouTubeApiCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeApiStartStatus::Started);
	CHECK(oldReply != nullptr);
	if (oldReply != nullptr) {
		rawManager->emitAuthenticationChallenge(oldReply);
	}
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(oldCompletions == 0);
	CHECK(completion->status == YouTubeApiCompletionStatus::NetworkFailure);
}

void testNetworkTlsAndRedirectFailures()
{
	struct Case final {
		FakeResponse response;
		YouTubeApiCompletionStatus expected;
	};
	std::vector<Case> cases;
	{
		FakeResponse response;
		response.httpStatus = 0;
		response.networkError = QNetworkReply::HostNotFoundError;
		response.encrypted.reset();
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::NetworkFailure});
	}
	{
		FakeResponse response;
		response.httpStatus = 0;
		response.networkError = QNetworkReply::ProxyAuthenticationRequiredError;
		response.encrypted.reset();
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::NetworkFailure});
	}
	{
		FakeResponse response;
		response.encrypted = false;
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::TlsFailure});
	}
	{
		FakeResponse response;
		response.encrypted.reset();
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::TlsFailure});
	}
	{
		FakeResponse response;
		response.sslFailure = true;
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::TlsFailure});
	}
	{
		FakeResponse response;
		response.httpStatus = 302;
		response.redirectTarget = QUrl(QStringLiteral("https://evil.example/steal"));
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::RedirectRejected});
	}
	{
		FakeResponse response;
		response.finalUrl = QUrl(QStringLiteral("https://evil.example/youtube/v3/channels"));
		cases.push_back({std::move(response), YouTubeApiCompletionStatus::RedirectRejected});
	}
	for (Case &testCase : cases) {
		auto *manager = new FakeNetworkAccessManager();
		int ignoreSslErrorsCount = -1;
		auto completion = runChannels(manager, std::move(testCase.response), {}, &ignoreSslErrorsCount);
		CHECK(completion->status == testCase.expected);
		CHECK(ignoreSslErrorsCount == 0);
	}
}

void testResponseMetadataAndSizeFailures()
{
	{
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.contentType = "text/html";
		CHECK(runChannels(manager, std::move(response))->status == YouTubeApiCompletionStatus::InvalidResponse);
	}
	{
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.contentEncoding = "gzip";
		CHECK(runChannels(manager, std::move(response))->status == YouTubeApiCompletionStatus::InvalidResponse);
	}
	{
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.extraHeaders.emplace_back("Content-Length", "999999");
		YouTubeApiDiscoveryOptions options;
		options.maxResponseBytes = 1024;
		CHECK(runChannels(manager, std::move(response), options)->status ==
		      YouTubeApiCompletionStatus::ResponseTooLarge);
	}
	{
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.extraHeaders.emplace_back("Content-Length", "1");
		CHECK(runChannels(manager, std::move(response))->status == YouTubeApiCompletionStatus::InvalidResponse);
	}
	{
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.body = QByteArray(1025, 'x');
		YouTubeApiDiscoveryOptions options;
		options.maxResponseBytes = 1024;
		CHECK(runChannels(manager, std::move(response), options)->status ==
		      YouTubeApiCompletionStatus::ResponseTooLarge);
	}
}

void testTimeoutCancelLateReplyAndShutdown()
{
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeResponse response;
		response.neverFinish = true;
		manager->responses.push_back(std::move(response));
		YouTubeApiDiscoveryOptions options;
		options.operationTimeout = std::chrono::milliseconds(20);
		TestDiscovery discovery(std::move(manager), options);
		std::optional<YouTubeApiCompletion> completion;
		CHECK(discovery.startListChannels(channelRequest(), [&](YouTubeApiCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeApiStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->status == YouTubeApiCompletionStatus::TimedOut);
	}

	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *rawManager = manager.get();
		FakeResponse response;
		response.neverFinish = true;
		manager->responses.push_back(std::move(response));
		TestDiscovery discovery(std::move(manager));
		int completions = 0;
		CHECK(discovery.startListChannels(channelRequest({8, 9}), [&](YouTubeApiCompletion) {
			++completions;
		}) == YouTubeApiStartStatus::Started);
		QPointer<FakeReply> reply = rawManager->replies[0];
		CHECK(!discovery.cancel({8, 10}));
		CHECK(discovery.cancel({8, 9}));
		CHECK(discovery.state() == YouTubeApiTransportState::Cancelled);
		CHECK(reply != nullptr && reply->aborted());
		if (reply != nullptr) {
			reply->emitLateFailureAndFinish();
		}
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		CHECK(completions == 0);
	}

	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeResponse response;
		response.neverFinish = true;
		manager->responses.push_back(std::move(response));
		TestDiscovery discovery(std::move(manager));
		int completions = 0;
		CHECK(discovery.startListChannels(channelRequest(), [&](YouTubeApiCompletion) { ++completions; }) ==
		      YouTubeApiStartStatus::Started);
		CHECK(discovery.shutdown());
		CHECK(discovery.shutdown());
		CHECK(discovery.state() == YouTubeApiTransportState::Closed);
		CHECK(discovery.startListChannels(channelRequest({1, 2}), [](YouTubeApiCompletion) {}) ==
		      YouTubeApiStartStatus::Closed);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		CHECK(completions == 0);
	}
}

void testInvalidOptionsAndThrowingCompletion()
{
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		YouTubeApiDiscoveryOptions options;
		options.maxResponseBytes = 100;
		TestDiscovery discovery(std::move(manager), options);
		CHECK(discovery.startListChannels(channelRequest(), [](YouTubeApiCompletion) {}) ==
		      YouTubeApiStartStatus::InvalidOptions);
	}
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		manager->responses.push_back(FakeResponse{});
		TestDiscovery discovery(std::move(manager));
		bool invoked = false;
		CHECK(discovery.startListChannels(channelRequest(), [&](YouTubeApiCompletion) {
			invoked = true;
			throw std::runtime_error("test callback");
		}) == YouTubeApiStartStatus::Started);
		CHECK(pumpUntil([&]() { return invoked; }));
		CHECK(discovery.state() == YouTubeApiTransportState::Completed);
	}
}

class RebindBeforeOldCompletion final : public QObject {
public:
	TestDiscovery *discovery = nullptr;
	std::optional<YouTubeApiCompletion> *secondCompletion = nullptr;
	int *secondCount = nullptr;
	bool triggered = false;
	YouTubeApiStartStatus startStatus = YouTubeApiStartStatus::Busy;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!triggered && watched == discovery->get() && event->type() == QEvent::MetaCall &&
		    discovery->state() == YouTubeApiTransportState::Completed) {
			triggered = true;
			CHECK(discovery->cancel({22, 1}));
			startStatus = discovery->startListChannels(channelRequest({22, 2}),
								   [this](YouTubeApiCompletion value) {
									   ++*secondCount;
									   secondCompletion->emplace(std::move(value));
								   });
		}
		return QObject::eventFilter(watched, event);
	}
};

void testQueuedCompletionCannotCrossRebind()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeResponse first;
	first.body = R"({"items":[{"id":"UC-old","snippet":{"title":"old"}}]})";
	manager->responses.push_back(std::move(first));
	FakeResponse second;
	second.body = R"({"items":[{"id":"UC-new","snippet":{"title":"new"}}]})";
	manager->responses.push_back(std::move(second));
	TestDiscovery discovery(std::move(manager));
	int firstCount = 0;
	int secondCount = 0;
	std::optional<YouTubeApiCompletion> secondCompletion;
	RebindBeforeOldCompletion filter;
	filter.discovery = &discovery;
	filter.secondCompletion = &secondCompletion;
	filter.secondCount = &secondCount;
	discovery.installEventFilter(&filter);

	CHECK(discovery.startListChannels(channelRequest({22, 1}), [&](YouTubeApiCompletion) { ++firstCount; }) ==
	      YouTubeApiStartStatus::Started);
	CHECK(pumpUntil([&]() { return secondCompletion.has_value(); }));
	CHECK(filter.triggered);
	CHECK(filter.startStatus == YouTubeApiStartStatus::Started);
	CHECK(firstCount == 0);
	CHECK(secondCount == 1);
	CHECK(secondCompletion->attempt == (YouTubeApiAttempt{22, 2}));
	CHECK(secondCompletion->channels.size() == 1);
	CHECK(secondCompletion->channels[0].id == "UC-new");
}

void testDestructionWithActiveReplyIsSilent()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeResponse response;
	response.neverFinish = true;
	manager->responses.push_back(std::move(response));
	int completionCount = 0;
	{
		auto discovery = std::make_unique<TestDiscovery>(std::move(manager));
		CHECK(discovery->startListChannels(channelRequest(), [&](YouTubeApiCompletion) {
			++completionCount;
		}) == YouTubeApiStartStatus::Started);
	}
	QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	CHECK(completionCount == 0);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testChannelsSuccessAndRequestContract();
	testChannelContinuationAndEmptyPage();
	testStreamsSuccessAndChannelFilter();
	testInputValidationAndBusyState();
	testStrictChannelParsing();
	testStrictStreamParsing();
	testProviderErrorClassification();
	testAuthenticationChallengeStillClassifies401();
	testLateAuthenticationChallengeCannotAffectNewAttempt();
	testNetworkTlsAndRedirectFailures();
	testResponseMetadataAndSizeFailures();
	testTimeoutCancelLateReplyAndShutdown();
	testInvalidOptionsAndThrowingCompletion();
	testQueuedCompletionCannotCrossRebind();
	testDestructionWithActiveReplyIsSilent();

	if (failures != 0) {
		std::cerr << failures << " YouTube discovery test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube discovery tests passed\n";
	return 0;
}

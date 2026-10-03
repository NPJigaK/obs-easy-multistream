// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-api-stream-resolver.hpp"
#include "youtube-destination.hpp"

#include <QAuthenticator>
#include <QByteArray>
#include <QCoreApplication>
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
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace easy_multistream {

class YouTubeApiStreamResolverTestAccess final {
public:
	explicit YouTubeApiStreamResolverTestAccess(std::unique_ptr<QNetworkAccessManager> manager,
						    YouTubeStreamResolverOptions options = {})
		: resolver_(new YouTubeApiStreamResolver(std::move(manager), options, nullptr))
	{
	}

	YouTubeStreamResolverStartStatus startResolveStream(YouTubeResolveStreamRequest request,
							    YouTubeApiStreamResolver::CompletionHandler handler)
	{
		return resolver_->startResolveStream(std::move(request), std::move(handler));
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept { return resolver_->cancel(attempt); }
	bool shutdown() noexcept { return resolver_->shutdown(); }
	YouTubeStreamResolverState state() const noexcept { return resolver_->state(); }
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept { return resolver_->activeAttempt(); }
	YouTubeApiStreamResolver *get() noexcept { return resolver_.get(); }

private:
	std::unique_ptr<YouTubeApiStreamResolver> resolver_;
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
using easy_multistream::YouTubeApiProviderError;
using easy_multistream::YouTubeApiStreamResolver;
using easy_multistream::YouTubeApiStreamResolverTestAccess;
using easy_multistream::YouTubeResolveStreamRequest;
using easy_multistream::YouTubeResolvedIngestion;
using easy_multistream::YouTubeStreamResolverCompletion;
using easy_multistream::YouTubeStreamResolverCompletionStatus;
using easy_multistream::YouTubeStreamResolverOptions;
using easy_multistream::YouTubeStreamResolverStartStatus;
using easy_multistream::YouTubeStreamResolverState;

static_assert(!std::is_copy_constructible_v<YouTubeResolveStreamRequest>);
static_assert(!std::is_copy_constructible_v<YouTubeResolvedIngestion>);
static_assert(!std::is_copy_constructible_v<YouTubeStreamResolverCompletion>);

constexpr std::string_view kAccessToken = "access-token";
constexpr std::string_view kStreamKey = "stream-key-secret";
constexpr std::string_view kChannelId = "UC-selected";
constexpr std::string_view kStreamId = "stream-a";
constexpr std::string_view kServerUrl = "rtmps://a.rtmps.youtube.com/live2";

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

YouTubeResolveStreamRequest streamRequest(YouTubeApiAttempt attempt = {1, 1},
					  std::string_view accessToken = kAccessToken,
					  std::string channelId = std::string(kChannelId),
					  std::string streamId = std::string(kStreamId))
{
	YouTubeResolveStreamRequest request;
	request.attempt = attempt;
	request.accessToken = secret(accessToken);
	request.channelId = std::move(channelId);
	request.streamId = std::move(streamId);
	return request;
}

bool pumpUntil(const std::function<bool()> &condition, int maximumMilliseconds = 1500)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maximumMilliseconds);
	while (!condition() && std::chrono::steady_clock::now() < deadline) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	}
	return condition();
}

QByteArray resolverBody(std::string_view status = "ready", std::string_view id = kStreamId,
			std::string_view channelId = kChannelId, std::string_view serverUrl = kServerUrl,
			std::string_view streamKey = kStreamKey)
{
	QByteArray body = R"({"items":[{"id":")";
	body.append(QByteArray::fromStdString(std::string(id)));
	body.append(R"(","snippet":{"channelId":")");
	body.append(QByteArray::fromStdString(std::string(channelId)));
	body.append(
		R"(","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":")");
	body.append(QByteArray::fromStdString(std::string(serverUrl)));
	body.append(R"(","streamName":")");
	body.append(QByteArray::fromStdString(std::string(streamKey)));
	body.append(R"("}},"status":{"streamStatus":")");
	body.append(QByteArray::fromStdString(std::string(status)));
	body.append(R"("}}]})");
	return body;
}

std::optional<YouTubeStreamResolverCompletion> runResolver(FakeNetworkAccessManager *manager, FakeResponse response,
							   YouTubeStreamResolverOptions options = {},
							   int *ignoreSslErrorsCount = nullptr)
{
	manager->responses.push_back(std::move(response));
	YouTubeApiStreamResolverTestAccess resolver(std::unique_ptr<QNetworkAccessManager>(manager), options);
	std::optional<YouTubeStreamResolverCompletion> completion;
	CHECK(resolver.startResolveStream(streamRequest(), [&](YouTubeStreamResolverCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeStreamResolverStartStatus::Started);
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	if (ignoreSslErrorsCount != nullptr) {
		*ignoreSslErrorsCount = manager->ignoreSslErrorsCount;
	}
	return completion;
}

void checkSecureRequest(const CapturedRequest &captured)
{
	CHECK(captured.operation == QNetworkAccessManager::GetOperation);
	CHECK(captured.body.isEmpty());
	CHECK(captured.request.url().scheme() == QStringLiteral("https"));
	CHECK(captured.request.url().host() == QStringLiteral("www.googleapis.com"));
	CHECK(captured.request.url().port() == -1);
	CHECK(captured.request.url().path() == QStringLiteral("/youtube/v3/liveStreams"));
	CHECK(captured.request.url().userInfo().isEmpty());
	CHECK(!captured.request.url().hasFragment());
	CHECK(captured.request.rawHeader("Authorization") == QByteArray("Bearer access-token"));
	CHECK(captured.request.rawHeader("Cookie").isEmpty());
	CHECK(!captured.request.url().toEncoded().contains("access-token"));
	CHECK(!captured.request.url().toEncoded().contains(QByteArray::fromStdString(std::string(kStreamKey))));
	CHECK(!captured.body.contains(QByteArray::fromStdString(std::string(kAccessToken))));
	CHECK(!captured.body.contains(QByteArray::fromStdString(std::string(kStreamKey))));
	CHECK(captured.request.rawHeader("Accept") == QByteArray("application/json"));
	CHECK(captured.request.rawHeader("Accept-Encoding") == QByteArray("identity"));
	CHECK(captured.request.rawHeader("Cache-Control") == QByteArray("no-store"));
	CHECK(captured.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() ==
	      QNetworkRequest::ManualRedirectPolicy);
	CHECK(captured.request.maximumRedirectsAllowed() == 0);
	CHECK(captured.request.attribute(QNetworkRequest::CacheLoadControlAttribute).toInt() ==
	      QNetworkRequest::AlwaysNetwork);
	CHECK(!captured.request.attribute(QNetworkRequest::CacheSaveControlAttribute).toBool());
	CHECK(captured.request.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt() ==
	      QNetworkRequest::Manual);
	CHECK(captured.request.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt() ==
	      QNetworkRequest::Manual);
	CHECK(captured.request.attribute(QNetworkRequest::UseCredentialsAttribute).toBool() == false);
	CHECK(captured.request.sslConfiguration().peerVerifyMode() == QSslSocket::VerifyPeer);
	CHECK(captured.request.sslConfiguration().protocol() == QSsl::TlsV1_2OrLater);
}

void testReadyAndInactiveSuccessContracts()
{
	for (const std::string_view state : {"ready", "inactive"}) {
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *rawManager = manager.get();
		FakeResponse response;
		response.body = resolverBody(state);
		manager->responses.push_back(std::move(response));
		YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
		std::optional<YouTubeStreamResolverCompletion> completion;
		CHECK(resolver.startResolveStream(streamRequest({2, 3}), [&](YouTubeStreamResolverCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeStreamResolverStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->succeeded());
		CHECK(completion->status == YouTubeStreamResolverCompletionStatus::Success);
		CHECK(completion->attempt == (YouTubeApiAttempt{2, 3}));
		CHECK(completion->providerError == YouTubeApiProviderError::None);
		CHECK(completion->httpStatus == 200);
		CHECK(completion->ingestion.has_value());
		if (completion->ingestion.has_value()) {
			CHECK(completion->ingestion->serverUrl == kServerUrl);
			CHECK(completion->ingestion->streamKey.view() == kStreamKey);
		}
		CHECK(rawManager->requests.size() == 1);
		checkSecureRequest(rawManager->requests[0]);
		const QUrlQuery query(rawManager->requests[0].request.url());
		CHECK(query.queryItemValue(QStringLiteral("part")) == QStringLiteral("id,snippet,cdn,status"));
		CHECK(query.queryItemValue(QStringLiteral("id")) == QStringLiteral("stream-a"));
		CHECK(query.queryItemValue(QStringLiteral("maxResults")) == QStringLiteral("1"));
		CHECK(query.queryItemValue(QStringLiteral("fields")) ==
		      QStringLiteral(
			      "items(id,snippet(channelId),cdn(ingestionType,ingestionInfo(rtmpsIngestionAddress,streamName)),status(streamStatus))"));
		CHECK(query.queryItemValue(QStringLiteral("mine")).isEmpty());
		CHECK(query.queryItemValue(QStringLiteral("pageToken")).isEmpty());
		CHECK(query.queryItemValue(QStringLiteral("key")).isEmpty());
		CHECK(!rawManager->requests[0].request.url().toEncoded().contains("stream-key-secret"));
	}
}

void testInputValidationAndBusyState()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeNetworkAccessManager *rawManager = manager.get();
	FakeResponse pending;
	pending.neverFinish = true;
	manager->responses.push_back(std::move(pending));
	YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
	auto handler = [](YouTubeStreamResolverCompletion) {
	};
	CHECK(resolver.startResolveStream(streamRequest({0, 1}), handler) ==
	      YouTubeStreamResolverStartStatus::InvalidAttempt);
	CHECK(resolver.startResolveStream(streamRequest({1, 1}, "bad token"), handler) ==
	      YouTubeStreamResolverStartStatus::InvalidAccessToken);
	CHECK(resolver.startResolveStream(
		      streamRequest({1, 1}, std::string(easy_multistream::kYouTubeApiMaxAccessTokenBytes + 1, 'x')),
		      handler) == YouTubeStreamResolverStartStatus::InvalidAccessToken);
	CHECK(resolver.startResolveStream(streamRequest({1, 1}, kAccessToken, "bad channel!"), handler) ==
	      YouTubeStreamResolverStartStatus::InvalidChannelId);
	CHECK(resolver.startResolveStream(streamRequest({1, 1}, kAccessToken, std::string(kChannelId), "bad stream!"),
					  handler) == YouTubeStreamResolverStartStatus::InvalidStreamId);
	CHECK(resolver.startResolveStream(streamRequest(), {}) ==
	      YouTubeStreamResolverStartStatus::InvalidCompletionHandler);
	YouTubeStreamResolverOptions badOptions;
	badOptions.maxResponseBytes = 100;
	CHECK(YouTubeApiStreamResolverTestAccess(std::make_unique<FakeNetworkAccessManager>(), badOptions)
		      .startResolveStream(streamRequest(), handler) ==
	      YouTubeStreamResolverStartStatus::InvalidOptions);
	CHECK(rawManager->requests.empty());

	CHECK(resolver.startResolveStream(streamRequest(), handler) == YouTubeStreamResolverStartStatus::Started);
	CHECK(resolver.startResolveStream(streamRequest({1, 2}), handler) == YouTubeStreamResolverStartStatus::Busy);
	CHECK(resolver.cancel({1, 1}));
}

void testSelectionAndStatusRejections()
{
	struct Case final {
		QByteArray body;
		YouTubeStreamResolverCompletionStatus expected;
	};
	const std::vector<Case> cases = {
		{R"({"items":[]})", YouTubeStreamResolverCompletionStatus::StreamNotFound},
		{resolverBody("ready", "wrong-stream"), YouTubeStreamResolverCompletionStatus::StreamMismatch},
		{resolverBody("ready", kStreamId, "other-channel"),
		 YouTubeStreamResolverCompletionStatus::StreamMismatch},
		{[] {
			 const QByteArray one = resolverBody();
			 const qsizetype itemsStart = one.indexOf('[') + 1;
			 const qsizetype itemsEnd = one.lastIndexOf(']');
			 const QByteArray item = one.mid(itemsStart, itemsEnd - itemsStart);
			 QByteArray multiple = R"({"items":[)";
			 multiple += item;
			 multiple += ',';
			 multiple += item;
			 multiple += QByteArray("]})");
			 return multiple;
		 }(),
		 YouTubeStreamResolverCompletionStatus::InvalidResponse},
		{resolverBody("active"), YouTubeStreamResolverCompletionStatus::StreamAlreadyActive},
		{resolverBody("created"), YouTubeStreamResolverCompletionStatus::StreamNotReady},
		{resolverBody("error"), YouTubeStreamResolverCompletionStatus::StreamNotReady},
		{resolverBody("unknown"), YouTubeStreamResolverCompletionStatus::InvalidResponse},
	};
	for (const Case &testCase : cases) {
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.body = testCase.body;
		auto completion = runResolver(manager, std::move(response));
		CHECK(completion.has_value());
		CHECK(completion->status == testCase.expected);
		CHECK(!completion->ingestion.has_value());
	}
}

void testStrictDestinationAndStatusParsing()
{
	const std::vector<QByteArray> invalidBodies = {
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"ingestionAddress":"rtmp://a.rtmps.youtube.com/live2","streamName":"stream-key-secret"}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmps","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":"stream-key-secret"}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmp://a.rtmps.youtube.com/live2","streamName":"stream-key-secret"}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://evil.example/live2","streamName":"stream-key-secret"}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2?x=1","streamName":"stream-key-secret"}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":""}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":"bad key"}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":7}},"status":{"streamStatus":"ready"}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":"stream-key-secret"}},"status":{}}]})",
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":"stream-key-secret"}},"status":{"streamStatus":7}}]})",
	};
	for (const QByteArray &body : invalidBodies) {
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.body = body;
		auto completion = runResolver(manager, std::move(response));
		CHECK(completion->status == YouTubeStreamResolverCompletionStatus::InvalidResponse);
		CHECK(!completion->ingestion.has_value());
	}

	QByteArray invalidUtf8 = resolverBody();
	const qsizetype keyPosition = invalidUtf8.indexOf("stream-key-secret");
	CHECK(keyPosition >= 0);
	if (keyPosition >= 0) {
		invalidUtf8.replace(keyPosition, static_cast<qsizetype>(kStreamKey.size()), QByteArray("\xFF", 1));
	}
	auto *utfManager = new FakeNetworkAccessManager();
	FakeResponse utfResponse;
	utfResponse.body = invalidUtf8;
	CHECK(runResolver(utfManager, std::move(utfResponse))->status ==
	      YouTubeStreamResolverCompletionStatus::InvalidResponse);

	QByteArray duplicate =
		R"({"items":[{"id":"stream-a","id":"stream-b","snippet":{"channelId":"UC-selected","title":"Selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":"stream-key-secret"}},"status":{"streamStatus":"ready"}}]})";
	auto *duplicateManager = new FakeNetworkAccessManager();
	FakeResponse duplicateResponse;
	duplicateResponse.body = duplicate;
	CHECK(runResolver(duplicateManager, std::move(duplicateResponse))->status ==
	      YouTubeStreamResolverCompletionStatus::InvalidResponse);

	QByteArray duplicateSecret =
		R"({"items":[{"id":"stream-a","snippet":{"channelId":"UC-selected"},"cdn":{"ingestionType":"rtmp","ingestionInfo":{"rtmpsIngestionAddress":"rtmps://a.rtmps.youtube.com/live2","streamName":"first-secret","streamName":"second-secret"}},"status":{"streamStatus":"ready"}}]})";
	auto *duplicateSecretManager = new FakeNetworkAccessManager();
	FakeResponse duplicateSecretResponse;
	duplicateSecretResponse.body = duplicateSecret;
	CHECK(runResolver(duplicateSecretManager, std::move(duplicateSecretResponse))->status ==
	      YouTubeStreamResolverCompletionStatus::InvalidResponse);

	QByteArray tooDeep = R"({"items":[],"unknown":)";
	tooDeep.append(QByteArray(70, '['));
	tooDeep.append('0');
	tooDeep.append(QByteArray(70, ']'));
	tooDeep.append('}');
	auto *deepManager = new FakeNetworkAccessManager();
	FakeResponse deepResponse;
	deepResponse.body = tooDeep;
	CHECK(runResolver(deepManager, std::move(deepResponse))->status ==
	      YouTubeStreamResolverCompletionStatus::InvalidResponse);
}

void testInvalidKeysAndOversizedResponse()
{
	std::vector<QByteArray> invalidKeys;
	invalidKeys.push_back(resolverBody("ready", kStreamId, kChannelId, kServerUrl, ""));
	invalidKeys.push_back(resolverBody("ready", kStreamId, kChannelId, kServerUrl, "key\tvalue"));
	QByteArray controlKey = resolverBody();
	const qsizetype controlPosition = controlKey.indexOf("stream-key-secret");
	if (controlPosition >= 0) {
		controlKey.replace(controlPosition, static_cast<qsizetype>(kStreamKey.size()),
				   QByteArray("key\x01", 4));
	}
	invalidKeys.push_back(std::move(controlKey));
	std::string hugeKey(easy_multistream::kMaxCredentialSecretBytes + 1, 'x');
	invalidKeys.push_back(resolverBody("ready", kStreamId, kChannelId, kServerUrl, hugeKey));

	for (const QByteArray &body : invalidKeys) {
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.body = body;
		auto completion = runResolver(manager, std::move(response));
		CHECK(completion->status == YouTubeStreamResolverCompletionStatus::InvalidResponse);
		CHECK(!completion->ingestion.has_value());
	}

	auto *largeManager = new FakeNetworkAccessManager();
	FakeResponse largeResponse;
	largeResponse.body = resolverBody();
	largeResponse.extraHeaders.emplace_back("Content-Length", "999999");
	YouTubeStreamResolverOptions options;
	options.maxResponseBytes = 1024;
	CHECK(runResolver(largeManager, std::move(largeResponse), options)->status ==
	      YouTubeStreamResolverCompletionStatus::ResponseTooLarge);

	auto *largeBodyManager = new FakeNetworkAccessManager();
	FakeResponse largeBodyResponse;
	largeBodyResponse.body = QByteArray(1025, 'x');
	CHECK(runResolver(largeBodyManager, std::move(largeBodyResponse), options)->status ==
	      YouTubeStreamResolverCompletionStatus::ResponseTooLarge);
}

void testProviderErrorClassification()
{
	struct Case final {
		int httpStatus;
		QNetworkReply::NetworkError networkError;
		const char *reason;
		YouTubeStreamResolverCompletionStatus expectedStatus;
		YouTubeApiProviderError expected;
	};
	const std::vector<Case> cases = {
		{401, QNetworkReply::AuthenticationRequiredError, "authError",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected, YouTubeApiProviderError::InvalidToken},
		{403, QNetworkReply::ContentAccessDenied, "insufficientLivePermissions",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected,
		 YouTubeApiProviderError::InsufficientPermissions},
		{403, QNetworkReply::ContentAccessDenied, "liveStreamingNotEnabled",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected,
		 YouTubeApiProviderError::LiveStreamingNotEnabled},
		{403, QNetworkReply::ContentAccessDenied, "quotaExceeded",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected, YouTubeApiProviderError::QuotaExceeded},
		{403, QNetworkReply::ContentAccessDenied, "rateLimitExceeded",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected, YouTubeApiProviderError::RateLimited},
		{404, QNetworkReply::ContentNotFoundError, "notFound",
		 YouTubeStreamResolverCompletionStatus::StreamNotFound, YouTubeApiProviderError::NotFound},
		{503, QNetworkReply::ServiceUnavailableError, "backendError",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected,
		 YouTubeApiProviderError::TemporarilyUnavailable},
		{400, QNetworkReply::ProtocolInvalidOperationError, "invalidRequest",
		 YouTubeStreamResolverCompletionStatus::ProviderRejected, YouTubeApiProviderError::InvalidRequest},
	};
	for (const Case &testCase : cases) {
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.httpStatus = testCase.httpStatus;
		response.networkError = testCase.networkError;
		response.body = QByteArray("{\"error\":{\"errors\":[{\"reason\":\"") + testCase.reason +
				"\",\"message\":\"provider text must not escape\"}],\"message\":\"raw\"}}";
		auto completion = runResolver(manager, std::move(response));
		CHECK(completion->status == testCase.expectedStatus);
		CHECK(completion->providerError == testCase.expected);
		CHECK(completion->httpStatus == testCase.httpStatus);
		CHECK(!completion->ingestion.has_value());
	}
}

void testAuthenticationChallengeAndLateGeneration()
{
	for (const int httpStatus : {401, 0}) {
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *rawManager = manager.get();
		FakeResponse response;
		response.httpStatus = httpStatus;
		response.networkError = QNetworkReply::AuthenticationRequiredError;
		response.body = R"({"error":{"errors":[{"reason":"authError"}]}})";
		manager->responses.push_back(std::move(response));
		YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
		std::optional<YouTubeStreamResolverCompletion> completion;
		CHECK(resolver.startResolveStream(streamRequest(), [&](YouTubeStreamResolverCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeStreamResolverStartStatus::Started);
		CHECK(rawManager->replies.size() == 1);
		rawManager->emitAuthenticationChallenge(rawManager->replies[0]);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->status == YouTubeStreamResolverCompletionStatus::ProviderRejected);
		CHECK(completion->providerError == YouTubeApiProviderError::InvalidToken);
	}

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
	YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
	int oldCompletions = 0;
	CHECK(resolver.startResolveStream(streamRequest({9, 1}), [&](YouTubeStreamResolverCompletion) {
		++oldCompletions;
	}) == YouTubeStreamResolverStartStatus::Started);
	QPointer<FakeReply> oldReply = rawManager->replies[0];
	CHECK(resolver.cancel({9, 1}));
	std::optional<YouTubeStreamResolverCompletion> completion;
	CHECK(resolver.startResolveStream(streamRequest({9, 2}), [&](YouTubeStreamResolverCompletion value) {
		completion.emplace(std::move(value));
	}) == YouTubeStreamResolverStartStatus::Started);
	if (oldReply != nullptr) {
		rawManager->emitAuthenticationChallenge(oldReply);
	}
	CHECK(pumpUntil([&]() { return completion.has_value(); }));
	CHECK(oldCompletions == 0);
	CHECK(completion->status == YouTubeStreamResolverCompletionStatus::NetworkFailure);
}

void testNetworkTlsRedirectMetadataAndNoFallback()
{
	struct Case final {
		FakeResponse response;
		YouTubeStreamResolverCompletionStatus expected;
	};
	std::vector<Case> cases;
	{
		FakeResponse response;
		response.httpStatus = 0;
		response.networkError = QNetworkReply::HostNotFoundError;
		response.encrypted.reset();
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::NetworkFailure});
	}
	{
		FakeResponse response;
		response.httpStatus = 0;
		response.networkError = QNetworkReply::ProxyAuthenticationRequiredError;
		response.encrypted.reset();
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::NetworkFailure});
	}
	{
		FakeResponse response;
		response.encrypted = false;
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::TlsFailure});
	}
	{
		FakeResponse response;
		response.encrypted.reset();
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::TlsFailure});
	}
	{
		FakeResponse response;
		response.sslFailure = true;
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::TlsFailure});
	}
	{
		FakeResponse response;
		response.httpStatus = 302;
		response.redirectTarget = QUrl(QStringLiteral("https://evil.example/steal"));
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::RedirectRejected});
	}
	{
		FakeResponse response;
		response.finalUrl = QUrl(QStringLiteral("https://evil.example/youtube/v3/liveStreams"));
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::RedirectRejected});
	}
	{
		FakeResponse response;
		response.contentType = "text/html";
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::InvalidResponse});
	}
	{
		FakeResponse response;
		response.contentEncoding = "gzip";
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::InvalidResponse});
	}
	{
		FakeResponse response;
		response.extraHeaders.emplace_back("Content-Length", "1");
		cases.push_back({std::move(response), YouTubeStreamResolverCompletionStatus::InvalidResponse});
	}
	for (Case &testCase : cases) {
		auto *manager = new FakeNetworkAccessManager();
		int ignoreSslErrorsCount = -1;
		auto completion = runResolver(manager, std::move(testCase.response), {}, &ignoreSslErrorsCount);
		CHECK(completion->status == testCase.expected);
		CHECK(ignoreSslErrorsCount == 0);
	}

	for (const std::string_view badUrl : {"rtmp://a.rtmps.youtube.com/live2", "rtmps://evil.example/live2",
					      "rtmps://a.rtmps.youtube.com/live2?bad=1"}) {
		auto *manager = new FakeNetworkAccessManager();
		FakeResponse response;
		response.body = resolverBody("ready", kStreamId, kChannelId, badUrl);
		CHECK(runResolver(manager, std::move(response))->status ==
		      YouTubeStreamResolverCompletionStatus::InvalidResponse);
	}
}

void testTimeoutCancelLateReplyShutdownAndThrowingCallback()
{
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeResponse response;
		response.neverFinish = true;
		manager->responses.push_back(std::move(response));
		YouTubeStreamResolverOptions options;
		options.operationTimeout = std::chrono::milliseconds(20);
		YouTubeApiStreamResolverTestAccess resolver(std::move(manager), options);
		std::optional<YouTubeStreamResolverCompletion> completion;
		CHECK(resolver.startResolveStream(streamRequest(), [&](YouTubeStreamResolverCompletion value) {
			completion.emplace(std::move(value));
		}) == YouTubeStreamResolverStartStatus::Started);
		CHECK(pumpUntil([&]() { return completion.has_value(); }));
		CHECK(completion->status == YouTubeStreamResolverCompletionStatus::TimedOut);
	}

	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		FakeNetworkAccessManager *rawManager = manager.get();
		FakeResponse response;
		response.neverFinish = true;
		manager->responses.push_back(std::move(response));
		YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
		int completions = 0;
		CHECK(resolver.startResolveStream(streamRequest({8, 9}), [&](YouTubeStreamResolverCompletion) {
			++completions;
		}) == YouTubeStreamResolverStartStatus::Started);
		QPointer<FakeReply> reply = rawManager->replies[0];
		CHECK(!resolver.cancel({8, 10}));
		CHECK(resolver.cancel({8, 9}));
		CHECK(resolver.state() == YouTubeStreamResolverState::Cancelled);
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
		YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
		int completions = 0;
		CHECK(resolver.startResolveStream(streamRequest(), [&](YouTubeStreamResolverCompletion) {
			++completions;
		}) == YouTubeStreamResolverStartStatus::Started);
		CHECK(resolver.shutdown());
		CHECK(resolver.shutdown());
		CHECK(resolver.state() == YouTubeStreamResolverState::Closed);
		CHECK(resolver.startResolveStream(streamRequest({1, 2}), [](YouTubeStreamResolverCompletion) {}) ==
		      YouTubeStreamResolverStartStatus::Closed);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		CHECK(completions == 0);
	}

	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		manager->responses.push_back(FakeResponse{});
		YouTubeApiStreamResolverTestAccess resolver(std::move(manager));
		bool invoked = false;
		CHECK(resolver.startResolveStream(streamRequest(), [&](YouTubeStreamResolverCompletion) {
			invoked = true;
			throw std::runtime_error("test callback");
		}) == YouTubeStreamResolverStartStatus::Started);
		CHECK(pumpUntil([&]() { return invoked; }));
		CHECK(resolver.state() == YouTubeStreamResolverState::Completed);
	}
}

void testDestructionWithActiveReplyIsSilent()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeResponse response;
	response.neverFinish = true;
	manager->responses.push_back(std::move(response));
	int completionCount = 0;
	{
		auto resolver = std::make_unique<YouTubeApiStreamResolverTestAccess>(std::move(manager));
		CHECK(resolver->startResolveStream(streamRequest(), [&](YouTubeStreamResolverCompletion) {
			++completionCount;
		}) == YouTubeStreamResolverStartStatus::Started);
	}
	QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
	CHECK(completionCount == 0);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testReadyAndInactiveSuccessContracts();
	testInputValidationAndBusyState();
	testSelectionAndStatusRejections();
	testStrictDestinationAndStatusParsing();
	testInvalidKeysAndOversizedResponse();
	testProviderErrorClassification();
	testAuthenticationChallengeAndLateGeneration();
	testNetworkTlsRedirectMetadataAndNoFallback();
	testTimeoutCancelLateReplyShutdownAndThrowingCallback();
	testDestructionWithActiveReplyIsSilent();

	if (failures != 0) {
		std::cerr << failures << " YouTube stream resolver test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube stream resolver tests passed\n";
	return 0;
}

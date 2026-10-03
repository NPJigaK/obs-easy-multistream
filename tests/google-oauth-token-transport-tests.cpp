// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-token-transport.hpp"

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

// This friend exists only in the standalone test executable. The production
// header exposes no constructor that can replace the transport-owned manager.
class GoogleOAuthTokenTransportTestAccess final {
public:
	explicit GoogleOAuthTokenTransportTestAccess(std::unique_ptr<QNetworkAccessManager> manager,
						     GoogleOAuthTokenTransportOptions options = {})
		: transport_(new GoogleOAuthTokenTransport(std::move(manager), options, nullptr))
	{
	}

	GoogleOAuthTokenStartStatus startExchange(GoogleOAuthTokenExchangeRequest request,
						  GoogleOAuthTokenTransport::CompletionHandler handler)
	{
		return transport_->startExchange(std::move(request), std::move(handler));
	}

	GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
						 GoogleOAuthTokenTransport::CompletionHandler handler)
	{
		return transport_->startRefresh(std::move(request), std::move(handler));
	}

	GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
						GoogleOAuthTokenTransport::CompletionHandler handler)
	{
		return transport_->startRevoke(std::move(request), std::move(handler));
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept { return transport_->cancel(attempt); }
	bool shutdown() noexcept { return transport_->shutdown(); }
	GoogleOAuthTokenTransportState state() const noexcept { return transport_->state(); }
	std::optional<GoogleOAuthTokenAttempt> activeAttempt() const noexcept { return transport_->activeAttempt(); }
	void installEventFilter(QObject *filter) { transport_->installEventFilter(filter); }
	GoogleOAuthTokenTransport *get() noexcept { return transport_.get(); }

private:
	std::unique_ptr<GoogleOAuthTokenTransport> transport_;
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

using easy_multistream::GoogleOAuthTokenAttempt;
using easy_multistream::GoogleOAuthTokenCompletion;
using easy_multistream::GoogleOAuthTokenCompletionStatus;
using easy_multistream::GoogleOAuthTokenExchangeRequest;
using easy_multistream::GoogleOAuthTokenOperation;
using easy_multistream::GoogleOAuthTokenProviderError;
using easy_multistream::GoogleOAuthTokenRefreshRequest;
using easy_multistream::GoogleOAuthTokenRevokeRequest;
using easy_multistream::GoogleOAuthTokenStartStatus;
using easy_multistream::GoogleOAuthTokenTransport;
using easy_multistream::GoogleOAuthTokenTransportOptions;
using easy_multistream::GoogleOAuthTokenTransportState;
using TestTokenTransport = easy_multistream::GoogleOAuthTokenTransportTestAccess;
using easy_multistream::SecureBuffer;

struct FakeResponse final {
	int httpStatus = 200;
	QByteArray contentType = "application/json";
	QByteArray contentEncoding;
	QByteArray body;
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
			response.neverFinish = true;
		}
		auto *reply = new FakeReply(operation, request, std::move(response), &ignoreSslErrorsCount, this);
		replies.emplace_back(reply);
		return reply;
	}
};

bool waitUntil(const std::function<bool()> &predicate, int timeoutMilliseconds = 2000)
{
	if (predicate()) {
		return true;
	}
	QEventLoop loop;
	QTimer poll;
	QTimer deadline;
	poll.setInterval(1);
	deadline.setSingleShot(true);
	QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
		if (predicate()) {
			loop.quit();
		}
	});
	QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
	poll.start();
	deadline.start(timeoutMilliseconds);
	loop.exec();
	return predicate();
}

std::vector<std::pair<QByteArray, QByteArray>> parseForm(const QByteArray &body)
{
	std::vector<std::pair<QByteArray, QByteArray>> fields;
	for (const QByteArray &part : body.split('&')) {
		const qsizetype separator = part.indexOf('=');
		if (separator < 0) {
			fields.emplace_back(QByteArray::fromPercentEncoding(part), QByteArray());
		} else {
			fields.emplace_back(QByteArray::fromPercentEncoding(part.first(separator)),
					    QByteArray::fromPercentEncoding(part.sliced(separator + 1)));
		}
	}
	return fields;
}

std::optional<QByteArray> formValue(const CapturedRequest &request, const QByteArray &name)
{
	const auto fields = parseForm(request.body);
	const auto iterator =
		std::find_if(fields.begin(), fields.end(), [&name](const auto &field) { return field.first == name; });
	if (iterator == fields.end()) {
		return std::nullopt;
	}
	return iterator->second;
}

int formCount(const CapturedRequest &request, const QByteArray &name)
{
	const auto fields = parseForm(request.body);
	return static_cast<int>(std::count_if(fields.begin(), fields.end(),
					      [&name](const auto &field) { return field.first == name; }));
}

GoogleOAuthTokenExchangeRequest exchangeRequest(GoogleOAuthTokenAttempt attempt = {1, 1})
{
	GoogleOAuthTokenExchangeRequest request;
	request.attempt = attempt;
	request.clientId = QStringLiteral("client-id.apps.googleusercontent.com");
	request.redirectUri = QUrl(QStringLiteral("http://127.0.0.1:43123/callback"));
	request.authorizationCode = SecureBuffer::copyOf("code&=+%opaque");
	request.codeVerifier = SecureBuffer::copyOf("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
	return request;
}

GoogleOAuthTokenRefreshRequest refreshRequest(GoogleOAuthTokenAttempt attempt = {1, 2})
{
	GoogleOAuthTokenRefreshRequest request;
	request.attempt = attempt;
	request.clientId = QStringLiteral("client-id.apps.googleusercontent.com");
	request.refreshToken = SecureBuffer::copyOf("refresh&=+%opaque");
	return request;
}

GoogleOAuthTokenRevokeRequest revokeRequest(GoogleOAuthTokenAttempt attempt = {1, 3})
{
	GoogleOAuthTokenRevokeRequest request;
	request.attempt = attempt;
	request.token = SecureBuffer::copyOf("refresh-token-to-revoke");
	return request;
}

QByteArray successJson(bool includeRefreshToken = true, bool includeScope = true)
{
	QByteArray json = "{\"access_token\":\"access-token\",\"expires_in\":3599,\"token_type\":\"Bearer\"";
	if (includeScope) {
		json.append(",\"scope\":\"https://www.googleapis.com/auth/youtube.readonly\"");
	}
	if (includeRefreshToken) {
		json.append(",\"refresh_token\":\"refresh-token\"");
	}
	json.append('}');
	return json;
}

void testExchangeRequestAndSuccess()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	fake->responses.push_back({200, "application/json; charset=UTF-8", {}, successJson()});
	TestTokenTransport transport(std::move(manager));
	std::optional<GoogleOAuthTokenCompletion> completion;
	int completionCount = 0;

	CHECK(transport.startExchange(exchangeRequest(), [&](GoogleOAuthTokenCompletion value) {
		++completionCount;
		completion.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
	CHECK(completion->succeeded());
	CHECK(completion->attempt == (GoogleOAuthTokenAttempt{1, 1}));
	CHECK(completion->operation == GoogleOAuthTokenOperation::ExchangeAuthorizationCode);
	CHECK(completion->tokens.accessToken.view() == "access-token");
	CHECK(completion->tokens.refreshToken.view() == "refresh-token");
	CHECK(completion->tokens.expiresInSeconds == 3599U);
	CHECK(fake->requests.size() == 1U);
	const CapturedRequest &captured = fake->requests.front();
	CHECK(captured.operation == QNetworkAccessManager::PostOperation);
	CHECK(captured.request.url() == QUrl(QString::fromLatin1(easy_multistream::kGoogleOAuthTokenEndpoint)));
	CHECK(!captured.request.url().hasQuery());
	CHECK(captured.request.rawHeader("Accept") == "application/json");
	CHECK(captured.request.rawHeader("Accept-Encoding") == "identity");
	CHECK(captured.request.header(QNetworkRequest::ContentTypeHeader).toString() ==
	      QStringLiteral("application/x-www-form-urlencoded"));
	CHECK(captured.request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() ==
	      QNetworkRequest::ManualRedirectPolicy);
	CHECK(captured.request.maximumRedirectsAllowed() == 0);
	CHECK(captured.request.sslConfiguration().peerVerifyMode() == QSslSocket::VerifyPeer);
	CHECK(captured.request.sslConfiguration().protocol() == QSsl::TlsV1_2OrLater);
	CHECK(formCount(captured, "client_id") == 1);
	CHECK(formCount(captured, "code") == 1);
	CHECK(formCount(captured, "code_verifier") == 1);
	CHECK(formCount(captured, "grant_type") == 1);
	CHECK(formCount(captured, "redirect_uri") == 1);
	CHECK(formCount(captured, "client_secret") == 0);
	CHECK(formValue(captured, "code") == QByteArray("code&=+%opaque"));
	CHECK(formValue(captured, "code_verifier") == QByteArray("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
	CHECK(formValue(captured, "redirect_uri") == QByteArray("http://127.0.0.1:43123/callback"));
	CHECK(formValue(captured, "grant_type") == QByteArray("authorization_code"));
	CHECK(!captured.request.hasRawHeader("Authorization"));
	CHECK(!captured.request.hasRawHeader("Cookie"));
	CHECK(!captured.request.hasRawHeader("Referer"));
}

void testRefreshAndRotationRules()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	fake->responses.push_back({200, "application/json", {}, successJson(false, false)});
	FakeResponse rotated;
	rotated.body = successJson(true, true);
	fake->responses.emplace_back(std::move(rotated));
	TestTokenTransport transport(std::move(manager));
	std::optional<GoogleOAuthTokenCompletion> first;
	std::optional<GoogleOAuthTokenCompletion> second;

	CHECK(transport.startRefresh(refreshRequest(), [&](GoogleOAuthTokenCompletion value) {
		first.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return first.has_value(); }));
	CHECK(first->succeeded());
	CHECK(!first->tokens.hasRefreshToken());
	CHECK(fake->requests.size() == 1U);
	CHECK(formValue(fake->requests.at(0), "grant_type") == QByteArray("refresh_token"));
	CHECK(formValue(fake->requests.at(0), "refresh_token") == QByteArray("refresh&=+%opaque"));
	CHECK(formCount(fake->requests.at(0), "code") == 0);
	CHECK(formCount(fake->requests.at(0), "redirect_uri") == 0);

	CHECK(transport.startRefresh(refreshRequest({1, 3}), [&](GoogleOAuthTokenCompletion value) {
		second.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return second.has_value(); }));
	CHECK(second->succeeded());
	CHECK(second->tokens.refreshToken.view() == "refresh-token");
}

void testRevokeRequestAndSuccess()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse response;
	response.contentType.clear();
	fake->responses.emplace_back(std::move(response));
	TestTokenTransport transport(std::move(manager));
	std::optional<GoogleOAuthTokenCompletion> completion;
	CHECK(transport.startRevoke(revokeRequest(), [&](GoogleOAuthTokenCompletion value) {
		completion.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
	CHECK(completion->operation == GoogleOAuthTokenOperation::RevokeToken);
	CHECK(fake->requests.size() == 1U);
	CHECK(fake->requests.front().request.url() ==
	      QUrl(QString::fromLatin1(easy_multistream::kGoogleOAuthRevokeEndpoint)));
	CHECK(formCount(fake->requests.front(), "token") == 1);
	CHECK(formCount(fake->requests.front(), "client_id") == 0);
	CHECK(!fake->requests.front().request.url().hasQuery());
}

GoogleOAuthTokenCompletion runExchange(FakeResponse response, GoogleOAuthTokenTransportOptions options = {})
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	manager->responses.emplace_back(std::move(response));
	TestTokenTransport transport(std::move(manager), options);
	std::optional<GoogleOAuthTokenCompletion> completion;
	const auto started = transport.startExchange(exchangeRequest(), [&](GoogleOAuthTokenCompletion value) {
		completion.emplace(std::move(value));
	});
	CHECK(started == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return completion.has_value(); }, 3000));
	if (completion.has_value()) {
		return std::move(*completion);
	}
	return {};
}

void testProviderAndNetworkFailures()
{
	FakeResponse invalidGrant;
	invalidGrant.httpStatus = 400;
	invalidGrant.body = "{\"error\":\"invalid_grant\",\"error_description\":\"never expose me\"}";
	// A network backend can expose an HTTP error through both the status
	// attribute and error(). The completed provider body must remain usable.
	invalidGrant.networkError = QNetworkReply::ProtocolInvalidOperationError;
	auto completion = runExchange(std::move(invalidGrant));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::ProviderRejected);
	CHECK(completion.providerError == GoogleOAuthTokenProviderError::InvalidGrant);
	CHECK(completion.httpStatus == 400);

	FakeResponse rateLimited;
	rateLimited.httpStatus = 429;
	rateLimited.body = "not trusted";
	rateLimited.networkError = QNetworkReply::ContentAccessDenied;
	completion = runExchange(std::move(rateLimited));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::ProviderRejected);
	CHECK(completion.providerError == GoogleOAuthTokenProviderError::RateLimited);

	FakeResponse unavailable;
	unavailable.httpStatus = 503;
	unavailable.body = "<html>not trusted</html>";
	unavailable.networkError = QNetworkReply::ServiceUnavailableError;
	completion = runExchange(std::move(unavailable));
	CHECK(completion.providerError == GoogleOAuthTokenProviderError::TemporarilyUnavailable);

	FakeResponse networkFailure;
	networkFailure.httpStatus = 0;
	networkFailure.contentType.clear();
	networkFailure.networkError = QNetworkReply::HostNotFoundError;
	completion = runExchange(std::move(networkFailure));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::NetworkFailure);

	FakeResponse interruptedErrorResponse;
	interruptedErrorResponse.httpStatus = 400;
	interruptedErrorResponse.body = "{\"error\":\"invalid_grant\"}";
	interruptedErrorResponse.networkError = QNetworkReply::RemoteHostClosedError;
	completion = runExchange(std::move(interruptedErrorResponse));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::NetworkFailure);
}

void testRedirectTlsAndTimeout()
{
	FakeResponse redirect;
	redirect.httpStatus = 302;
	redirect.redirectTarget = QUrl(QStringLiteral("https://attacker.example/token"));
	auto completion = runExchange(std::move(redirect));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::RedirectRejected);

	FakeResponse changedFinalUrl;
	changedFinalUrl.body = successJson();
	changedFinalUrl.finalUrl = QUrl(QStringLiteral("https://attacker.example/token"));
	completion = runExchange(std::move(changedFinalUrl));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::RedirectRejected);

	FakeResponse unencrypted;
	unencrypted.body = successJson();
	unencrypted.encrypted = false;
	completion = runExchange(std::move(unencrypted));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::TlsFailure);

	FakeResponse missingEncryptionProof;
	missingEncryptionProof.body = successJson();
	missingEncryptionProof.encrypted.reset();
	completion = runExchange(std::move(missingEncryptionProof));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::TlsFailure);

	FakeResponse handshakeFailure;
	handshakeFailure.httpStatus = 0;
	handshakeFailure.networkError = QNetworkReply::SslHandshakeFailedError;
	handshakeFailure.encrypted = false;
	completion = runExchange(std::move(handshakeFailure));
	CHECK(completion.status == GoogleOAuthTokenCompletionStatus::TlsFailure);

	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse tlsFailure;
	tlsFailure.sslFailure = true;
	fake->responses.emplace_back(std::move(tlsFailure));
	TestTokenTransport tlsTransport(std::move(manager));
	std::optional<GoogleOAuthTokenCompletion> tlsCompletion;
	CHECK(tlsTransport.startExchange(exchangeRequest(), [&](GoogleOAuthTokenCompletion value) {
		tlsCompletion.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return tlsCompletion.has_value(); }));
	CHECK(tlsCompletion->status == GoogleOAuthTokenCompletionStatus::TlsFailure);
	CHECK(fake->ignoreSslErrorsCount == 0);

	manager = std::make_unique<FakeNetworkAccessManager>();
	FakeResponse never;
	never.neverFinish = true;
	manager->responses.emplace_back(std::move(never));
	GoogleOAuthTokenTransportOptions options;
	options.operationTimeout = std::chrono::milliseconds(20);
	TestTokenTransport timeoutTransport(std::move(manager), options);
	std::optional<GoogleOAuthTokenCompletion> timeoutCompletion;
	CHECK(timeoutTransport.startExchange(exchangeRequest(), [&](GoogleOAuthTokenCompletion value) {
		timeoutCompletion.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return timeoutCompletion.has_value(); }));
	CHECK(timeoutCompletion->status == GoogleOAuthTokenCompletionStatus::TimedOut);
}

void testStrictResponseValidation()
{
	FakeResponse malformed;
	malformed.body = "{not-json";
	CHECK(runExchange(std::move(malformed)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse wrongType;
	wrongType.contentType = "text/html";
	wrongType.body = successJson();
	CHECK(runExchange(std::move(wrongType)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse duplicate;
	duplicate.body = "{\"access_token\":\"one\",\"access_token\":\"two\",\"refresh_token\":\"refresh\","
			 "\"expires_in\":3600,\"token_type\":\"Bearer\","
			 "\"scope\":\"https://www.googleapis.com/auth/youtube.readonly\"}";
	CHECK(runExchange(std::move(duplicate)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse escapedDuplicate;
	escapedDuplicate.body =
		"{\"access_token\":\"one\",\"access\\u005ftoken\":\"two\",\"refresh_token\":\"refresh\","
		"\"expires_in\":3600,\"token_type\":\"Bearer\","
		"\"scope\":\"https://www.googleapis.com/auth/youtube.readonly\"}";
	CHECK(runExchange(std::move(escapedDuplicate)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse noRefresh;
	noRefresh.body = successJson(false, true);
	CHECK(runExchange(std::move(noRefresh)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse wrongScope;
	wrongScope.body = "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600,"
			  "\"token_type\":\"Bearer\",\"scope\":\"https://www.googleapis.com/auth/drive.readonly\"}";
	CHECK(runExchange(std::move(wrongScope)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse fractionalExpiry;
	fractionalExpiry.body = "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":1.5,"
				"\"token_type\":\"Bearer\","
				"\"scope\":\"https://www.googleapis.com/auth/youtube.readonly\"}";
	CHECK(runExchange(std::move(fractionalExpiry)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse excessiveExpiry;
	excessiveExpiry.body = "{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":2678401,"
			       "\"token_type\":\"Bearer\","
			       "\"scope\":\"https://www.googleapis.com/auth/youtube.readonly\"}";
	CHECK(runExchange(std::move(excessiveExpiry)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse compressed;
	compressed.contentEncoding = "gzip";
	compressed.body = successJson();
	CHECK(runExchange(std::move(compressed)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse oversized;
	oversized.body = QByteArray(easy_multistream::kGoogleOAuthMaxTokenResponseBytes + 1, 'x');
	CHECK(runExchange(std::move(oversized)).status == GoogleOAuthTokenCompletionStatus::ResponseTooLarge);

	FakeResponse declaredOversized;
	declaredOversized.extraHeaders.emplace_back("Content-Length", "999999");
	CHECK(runExchange(std::move(declaredOversized)).status == GoogleOAuthTokenCompletionStatus::ResponseTooLarge);

	FakeResponse lengthMismatch;
	lengthMismatch.body = successJson();
	lengthMismatch.extraHeaders.emplace_back("Content-Length", "1");
	CHECK(runExchange(std::move(lengthMismatch)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse malformedLength;
	malformedLength.extraHeaders.emplace_back("Content-Length", "not-a-number");
	CHECK(runExchange(std::move(malformedLength)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse mergedDuplicateLength;
	mergedDuplicateLength.extraHeaders.emplace_back("Content-Length", "1, 1");
	CHECK(runExchange(std::move(mergedDuplicateLength)).status ==
	      GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse tooManyHeaders;
	for (int index = 0; index < 65; ++index) {
		tooManyHeaders.extraHeaders.emplace_back("X-Test-" + QByteArray::number(index), "value");
	}
	CHECK(runExchange(std::move(tooManyHeaders)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse oversizedHeaders;
	oversizedHeaders.extraHeaders.emplace_back("X-Oversized", QByteArray(16 * 1024, 'x'));
	CHECK(runExchange(std::move(oversizedHeaders)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse successWithError;
	successWithError.body =
		"{\"access_token\":\"access\",\"refresh_token\":\"refresh\",\"expires_in\":3600,"
		"\"token_type\":\"Bearer\",\"scope\":\"https://www.googleapis.com/auth/youtube.readonly\","
		"\"error\":\"invalid_grant\"}";
	CHECK(runExchange(std::move(successWithError)).status == GoogleOAuthTokenCompletionStatus::InvalidResponse);

	FakeResponse partialNetworkFailure;
	partialNetworkFailure.body = successJson();
	partialNetworkFailure.networkError = QNetworkReply::RemoteHostClosedError;
	CHECK(runExchange(std::move(partialNetworkFailure)).status == GoogleOAuthTokenCompletionStatus::NetworkFailure);
}

void testValidationBusyCancelAndReuse()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse never;
	never.neverFinish = true;
	fake->responses.emplace_back(std::move(never));
	FakeResponse success;
	success.body = successJson();
	fake->responses.emplace_back(std::move(success));
	TestTokenTransport transport(std::move(manager));
	int callbackCount = 0;

	auto invalid = exchangeRequest({0, 1});
	CHECK(transport.startExchange(std::move(invalid), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::InvalidAttempt);
	CHECK(fake->requests.empty());

	auto invalidRedirect = exchangeRequest({7, 10});
	invalidRedirect.redirectUri = QUrl(QStringLiteral("http://localhost:43123/callback"));
	CHECK(transport.startExchange(std::move(invalidRedirect), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::InvalidRedirectUri);
	auto invalidVerifier = exchangeRequest({7, 11});
	invalidVerifier.codeVerifier = SecureBuffer::copyOf("too-short");
	CHECK(transport.startExchange(std::move(invalidVerifier), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::InvalidCodeVerifier);
	auto invalidCode = exchangeRequest({7, 12});
	invalidCode.authorizationCode = SecureBuffer::copyOf("code with spaces");
	CHECK(transport.startExchange(std::move(invalidCode), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::InvalidAuthorizationCode);
	CHECK(fake->requests.empty());

	CHECK(transport.startExchange(exchangeRequest({7, 1}), [&](GoogleOAuthTokenCompletion) { ++callbackCount; }) ==
	      GoogleOAuthTokenStartStatus::Started);
	CHECK(transport.state() == GoogleOAuthTokenTransportState::InFlight);
	CHECK(transport.startRefresh(refreshRequest({7, 2}), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::Busy);
	CHECK(!transport.cancel({7, 2}));
	CHECK(transport.cancel({7, 1}));
	CHECK(!transport.cancel({7, 1}));
	QCoreApplication::processEvents();
	CHECK(callbackCount == 0);
	CHECK(transport.state() == GoogleOAuthTokenTransportState::Cancelled);

	std::optional<GoogleOAuthTokenCompletion> completion;
	CHECK(transport.startExchange(exchangeRequest({7, 3}), [&](GoogleOAuthTokenCompletion value) {
		completion.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());
}

void testRevokeTokenBoundaries()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse response;
	response.contentType.clear();
	fake->responses.emplace_back(std::move(response));
	TestTokenTransport transport(std::move(manager));
	std::optional<GoogleOAuthTokenCompletion> completion;
	GoogleOAuthTokenRevokeRequest maximum;
	maximum.attempt = {30, 1};
	maximum.token = SecureBuffer::copyOf(std::string(easy_multistream::kGoogleOAuthMaxAccessTokenBytes, 'a'));
	CHECK(transport.startRevoke(std::move(maximum), [&](GoogleOAuthTokenCompletion value) {
		completion.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completion->succeeded());

	GoogleOAuthTokenRevokeRequest oversized;
	oversized.attempt = {30, 2};
	oversized.token = SecureBuffer::copyOf(std::string(easy_multistream::kGoogleOAuthMaxAccessTokenBytes + 1, 'b'));
	CHECK(transport.startRevoke(std::move(oversized), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::InvalidToken);
	CHECK(fake->requests.size() == 1U);
}

void testInvalidOptionsDoNotSendRequests()
{
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		auto *fake = manager.get();
		GoogleOAuthTokenTransportOptions options;
		options.maxResponseBytes = easy_multistream::kGoogleOAuthMaxTokenResponseBytes + 1;
		TestTokenTransport transport(std::move(manager), options);
		CHECK(transport.startExchange(exchangeRequest(), [](GoogleOAuthTokenCompletion) {}) ==
		      GoogleOAuthTokenStartStatus::InvalidOptions);
		CHECK(fake->requests.empty());
	}
	{
		auto manager = std::make_unique<FakeNetworkAccessManager>();
		auto *fake = manager.get();
		GoogleOAuthTokenTransportOptions options;
		options.operationTimeout = std::chrono::milliseconds(9);
		TestTokenTransport transport(std::move(manager), options);
		CHECK(transport.startExchange(exchangeRequest(), [](GoogleOAuthTokenCompletion) {}) ==
		      GoogleOAuthTokenStartStatus::InvalidOptions);
		CHECK(fake->requests.empty());
	}
}

void testDestroyedReplyCompletesAndCanBeReused()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse never;
	never.neverFinish = true;
	fake->responses.emplace_back(std::move(never));
	FakeResponse success;
	success.body = successJson();
	fake->responses.emplace_back(std::move(success));
	TestTokenTransport transport(std::move(manager));
	std::optional<GoogleOAuthTokenCompletion> first;
	CHECK(transport.startExchange(exchangeRequest({40, 1}), [&](GoogleOAuthTokenCompletion value) {
		first.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(fake->replies.size() == 1U);
	delete fake->replies.front().data();
	CHECK(waitUntil([&]() { return first.has_value(); }));
	CHECK(first->status == GoogleOAuthTokenCompletionStatus::NetworkFailure);
	CHECK(transport.state() == GoogleOAuthTokenTransportState::Completed);
	CHECK(!transport.activeAttempt().has_value());

	std::optional<GoogleOAuthTokenCompletion> second;
	CHECK(transport.startExchange(exchangeRequest({40, 2}), [&](GoogleOAuthTokenCompletion value) {
		second.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return second.has_value(); }));
	CHECK(second->succeeded());
}

void testLateReplyCannotAffectReplacement()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse never;
	never.neverFinish = true;
	fake->responses.emplace_back(std::move(never));
	FakeResponse success;
	success.body = successJson();
	fake->responses.emplace_back(std::move(success));
	TestTokenTransport transport(std::move(manager));
	int firstCount = 0;
	CHECK(transport.startExchange(exchangeRequest({41, 1}), [&](GoogleOAuthTokenCompletion) { ++firstCount; }) ==
	      GoogleOAuthTokenStartStatus::Started);
	CHECK(fake->replies.size() == 1U);
	QPointer<FakeReply> firstReply = fake->replies.front();
	CHECK(transport.cancel({41, 1}));

	std::optional<GoogleOAuthTokenCompletion> second;
	CHECK(transport.startExchange(exchangeRequest({41, 2}), [&](GoogleOAuthTokenCompletion value) {
		second.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(firstReply != nullptr);
	if (firstReply != nullptr) {
		firstReply->emitLateFailureAndFinish();
	}
	CHECK(waitUntil([&]() { return second.has_value(); }));
	CHECK(firstCount == 0);
	CHECK(second->succeeded());
	CHECK(second->attempt == (GoogleOAuthTokenAttempt{41, 2}));
}

void testShutdownIsSilentAndTerminal()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	auto *fake = manager.get();
	FakeResponse never;
	never.neverFinish = true;
	fake->responses.emplace_back(std::move(never));
	TestTokenTransport transport(std::move(manager));
	int completionCount = 0;
	CHECK(transport.startExchange(exchangeRequest({42, 1}), [&](GoogleOAuthTokenCompletion) {
		++completionCount;
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(transport.shutdown());
	QCoreApplication::processEvents();
	CHECK(completionCount == 0);
	CHECK(transport.state() == GoogleOAuthTokenTransportState::Closed);
	CHECK(!transport.activeAttempt().has_value());
	CHECK(transport.startExchange(exchangeRequest({42, 2}), [](GoogleOAuthTokenCompletion) {}) ==
	      GoogleOAuthTokenStartStatus::Closed);
	CHECK(fake->requests.size() == 1U);
}

void testThrowingCompletionHandlerDoesNotPoisonTransport()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeResponse firstResponse;
	firstResponse.body = successJson();
	manager->responses.emplace_back(std::move(firstResponse));
	FakeResponse secondResponse;
	secondResponse.body = successJson();
	manager->responses.emplace_back(std::move(secondResponse));
	TestTokenTransport transport(std::move(manager));
	bool firstInvoked = false;
	CHECK(transport.startExchange(exchangeRequest({43, 1}), [&](GoogleOAuthTokenCompletion) {
		firstInvoked = true;
		throw std::runtime_error("test callback failure");
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return firstInvoked; }));

	std::optional<GoogleOAuthTokenCompletion> second;
	CHECK(transport.startExchange(exchangeRequest({43, 2}), [&](GoogleOAuthTokenCompletion value) {
		second.emplace(std::move(value));
	}) == GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return second.has_value(); }));
	CHECK(second->succeeded());
}

class RebindBeforeOldCompletion final : public QObject {
public:
	GoogleOAuthTokenTransport *transport = nullptr;
	std::optional<GoogleOAuthTokenCompletion> *secondCompletion = nullptr;
	int *secondCount = nullptr;
	bool triggered = false;
	GoogleOAuthTokenStartStatus startStatus = GoogleOAuthTokenStartStatus::Busy;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!triggered && watched == transport && event->type() == QEvent::MetaCall &&
		    transport->state() == GoogleOAuthTokenTransportState::Completed) {
			triggered = true;
			CHECK(transport->cancel({22, 1}));
			startStatus = transport->startExchange(exchangeRequest({22, 2}),
							       [this](GoogleOAuthTokenCompletion value) {
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
	first.body = successJson();
	manager->responses.emplace_back(std::move(first));
	FakeResponse second;
	second.body = successJson();
	manager->responses.emplace_back(std::move(second));
	TestTokenTransport transport(std::move(manager));
	int firstCount = 0;
	int secondCount = 0;
	std::optional<GoogleOAuthTokenCompletion> secondCompletion;
	RebindBeforeOldCompletion filter;
	filter.transport = transport.get();
	filter.secondCompletion = &secondCompletion;
	filter.secondCount = &secondCount;
	transport.installEventFilter(&filter);

	CHECK(transport.startExchange(exchangeRequest({22, 1}), [&](GoogleOAuthTokenCompletion) { ++firstCount; }) ==
	      GoogleOAuthTokenStartStatus::Started);
	CHECK(waitUntil([&]() { return secondCompletion.has_value(); }));
	CHECK(filter.triggered);
	CHECK(filter.startStatus == GoogleOAuthTokenStartStatus::Started);
	CHECK(firstCount == 0);
	CHECK(secondCount == 1);
	CHECK(secondCompletion->attempt == (GoogleOAuthTokenAttempt{22, 2}));
}

void testDestructionWithActiveReplyIsSilent()
{
	auto manager = std::make_unique<FakeNetworkAccessManager>();
	FakeResponse never;
	never.neverFinish = true;
	manager->responses.emplace_back(std::move(never));
	int completionCount = 0;
	{
		auto transport = std::make_unique<TestTokenTransport>(std::move(manager));
		CHECK(transport->startExchange(exchangeRequest(), [&](GoogleOAuthTokenCompletion) {
			++completionCount;
		}) == GoogleOAuthTokenStartStatus::Started);
	}
	QCoreApplication::processEvents();
	CHECK(completionCount == 0);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testExchangeRequestAndSuccess();
	testRefreshAndRotationRules();
	testRevokeRequestAndSuccess();
	testProviderAndNetworkFailures();
	testRedirectTlsAndTimeout();
	testStrictResponseValidation();
	testValidationBusyCancelAndReuse();
	testRevokeTokenBoundaries();
	testInvalidOptionsDoNotSendRequests();
	testDestroyedReplyCompletesAndCanBeReused();
	testLateReplyCannotAffectReplacement();
	testShutdownIsSilentAndTerminal();
	testThrowingCompletionHandlerDoesNotPoisonTransport();
	testQueuedCompletionCannotCrossRebind();
	testDestructionWithActiveReplyIsSilent();
	return failures == 0 ? 0 : 1;
}

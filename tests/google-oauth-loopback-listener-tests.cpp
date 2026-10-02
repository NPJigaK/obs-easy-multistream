// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "google-oauth-loopback-listener.hpp"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                 \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using easy_multistream::GoogleOAuthCallbackStatus;
using easy_multistream::GoogleOAuthLoopbackArmStatus;
using easy_multistream::GoogleOAuthLoopbackAttempt;
using easy_multistream::GoogleOAuthLoopbackBindStatus;
using easy_multistream::GoogleOAuthLoopbackCompletion;
using easy_multistream::GoogleOAuthLoopbackCompletionStatus;
using easy_multistream::GoogleOAuthLoopbackListener;
using easy_multistream::GoogleOAuthLoopbackOptions;
using easy_multistream::GoogleOAuthLoopbackState;
using easy_multistream::GoogleOAuthProviderError;

struct HttpExchange final {
	QByteArray response;
	bool connected = false;
	bool timedOut = false;
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

HttpExchange exchangeHttp(quint16 port, QByteArray request, qsizetype splitAt = -1, int timeoutMilliseconds = 2000)
{
	QTcpSocket socket;
	QEventLoop loop;
	QTimer deadline;
	HttpExchange exchange;
	deadline.setSingleShot(true);

	QObject::connect(&deadline, &QTimer::timeout, &loop, [&]() {
		exchange.timedOut = true;
		socket.abort();
		loop.quit();
	});
	QObject::connect(&socket, &QTcpSocket::connected, &loop, [&]() {
		exchange.connected = true;
		if (splitAt > 0 && splitAt < request.size()) {
			socket.write(request.first(splitAt));
			QTimer::singleShot(10, &socket,
					   [&socket, &request, splitAt]() { socket.write(request.sliced(splitAt)); });
		} else {
			socket.write(request);
		}
	});
	QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&]() { exchange.response.append(socket.readAll()); });
	QObject::connect(&socket, &QTcpSocket::disconnected, &loop, [&]() {
		exchange.response.append(socket.readAll());
		loop.quit();
	});
	QObject::connect(&socket, &QTcpSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) {
		if (socket.state() == QAbstractSocket::UnconnectedState) {
			loop.quit();
		}
	});

	deadline.start(timeoutMilliseconds);
	socket.connectToHost(QStringLiteral("127.0.0.1"), port);
	loop.exec();
	return exchange;
}

QByteArray httpGet(quint16 port, const QByteArray &target, const QByteArray &additionalHeaders = {})
{
	QByteArray request("GET ");
	request.append(target);
	request.append(" HTTP/1.1\r\nHost: 127.0.0.1:");
	request.append(QByteArray::number(port));
	request.append("\r\n");
	request.append(additionalHeaders);
	request.append("Connection: close\r\n\r\n");
	return request;
}

QString authorizationState(const easy_multistream::GoogleOAuthAuthorizationRequest &request)
{
	return QUrlQuery(request.authorizationUrl).queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
}

struct ArmedAttempt final {
	easy_multistream::GoogleOAuthAuthorizationResult authorization;
	QString state;
};

class RebindBeforeOldCompletion final : public QObject {
public:
	GoogleOAuthLoopbackListener *listener = nullptr;
	std::optional<GoogleOAuthLoopbackCompletion> *secondCompletion = nullptr;
	int *secondCompletionCount = nullptr;
	bool triggered = false;
	GoogleOAuthLoopbackBindStatus bindStatus = GoogleOAuthLoopbackBindStatus::BindFailed;
	GoogleOAuthLoopbackArmStatus armStatus = GoogleOAuthLoopbackArmStatus::NotListening;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!triggered && watched == listener && event->type() == QEvent::MetaCall &&
		    listener->state() == GoogleOAuthLoopbackState::TimedOut) {
			triggered = true;
			listener->cancel();
			const auto rebound = listener->bind({22, 2});
			bindStatus = rebound.status;
			if (rebound.succeeded()) {
				auto authorization = easy_multistream::makeGoogleOAuthAuthorizationRequest(
					QStringLiteral("client-id.apps.googleusercontent.com"), rebound.port);
				if (authorization.succeeded()) {
					armStatus = listener->arm(authorization.request.redirectUri,
								  std::move(authorization.request.state),
								  [this](GoogleOAuthLoopbackCompletion value) {
									  ++*secondCompletionCount;
									  secondCompletion->emplace(std::move(value));
								  });
				}
			}
		}
		return QObject::eventFilter(watched, event);
	}
};

ArmedAttempt armListener(GoogleOAuthLoopbackListener &listener,
			 std::optional<GoogleOAuthLoopbackCompletion> &completion, int &completionCount,
			 GoogleOAuthLoopbackAttempt attempt = {7, 11})
{
	const auto bound = listener.bind(attempt);
	CHECK(bound.status == GoogleOAuthLoopbackBindStatus::Success);
	CHECK(bound.succeeded());

	ArmedAttempt armed;
	armed.authorization = easy_multistream::makeGoogleOAuthAuthorizationRequest(
		QStringLiteral("client-id.apps.googleusercontent.com"), bound.port);
	CHECK(armed.authorization.succeeded());
	armed.state = authorizationState(armed.authorization.request);
	const auto armStatus = listener.arm(armed.authorization.request.redirectUri,
					    std::move(armed.authorization.request.state),
					    [&completion, &completionCount](GoogleOAuthLoopbackCompletion value) {
						    ++completionCount;
						    completion.emplace(std::move(value));
					    });
	CHECK(armStatus == GoogleOAuthLoopbackArmStatus::Success);
	return armed;
}

void testExclusiveLoopbackBind()
{
	GoogleOAuthLoopbackListener listener;
	const GoogleOAuthLoopbackAttempt attempt{1, 1};
	const auto bound = listener.bind(attempt);
	CHECK(bound.succeeded());
	CHECK(bound.port != 0);
	CHECK(listener.port() == bound.port);
	CHECK(listener.state() == GoogleOAuthLoopbackState::Listening);
	CHECK(listener.attempt() == attempt);

	SOCKET competing = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
	CHECK(competing != INVALID_SOCKET);
	if (competing != INVALID_SOCKET) {
		const BOOL reuse = TRUE;
		CHECK(setsockopt(competing, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse),
				 sizeof(reuse)) == 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(bound.port);
		CHECK(::bind(competing, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == SOCKET_ERROR);
		closesocket(competing);
	}

	listener.cancel();
	CHECK(listener.state() == GoogleOAuthLoopbackState::Cancelled);
}

void testBindBeforeArmAndFragmentedSuccess()
{
	GoogleOAuthLoopbackListener listener;
	const auto bound = listener.bind({2, 3});
	CHECK(bound.succeeded());

	const auto early =
		exchangeHttp(bound.port, httpGet(bound.port, QByteArray("/callback?code=early&state=early")));
	CHECK(early.connected);
	CHECK(early.response.startsWith("HTTP/1.1 400"));
	CHECK(listener.state() == GoogleOAuthLoopbackState::Listening);

	std::optional<GoogleOAuthLoopbackCompletion> completion;
	int completionCount = 0;
	auto authorization = easy_multistream::makeGoogleOAuthAuthorizationRequest(
		QStringLiteral("client-id.apps.googleusercontent.com"), bound.port);
	CHECK(authorization.succeeded());
	const QString state = authorizationState(authorization.request);
	CHECK(listener.arm(authorization.request.redirectUri, std::move(authorization.request.state),
			   [&](GoogleOAuthLoopbackCompletion value) {
				   ++completionCount;
				   completion.emplace(std::move(value));
			   }) == GoogleOAuthLoopbackArmStatus::Success);

	const QByteArray code("one-time-code-for-test");
	const QByteArray target = QByteArray("/callback?code=") + code + "&state=" + state.toLatin1();
	const QByteArray request = httpGet(bound.port, target, QByteArray("Accept: text/html\r\n"));
	const auto success = exchangeHttp(bound.port, request, request.size() / 2);
	CHECK(success.response.startsWith("HTTP/1.1 200"));
	CHECK(success.response.contains("Cache-Control: no-store"));
	CHECK(success.response.contains("Content-Security-Policy:"));
	CHECK(!success.response.contains(code));
	CHECK(!success.response.contains(state.toLatin1()));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
	CHECK(listener.state() == GoogleOAuthLoopbackState::Completed);
	if (completion.has_value()) {
		CHECK(completion->status == GoogleOAuthLoopbackCompletionStatus::Callback);
		CHECK((completion->attempt == GoogleOAuthLoopbackAttempt{2, 3}));
		CHECK(completion->callback.status == GoogleOAuthCallbackStatus::Success);
		CHECK(completion->callback.authorizationCode.view() == "one-time-code-for-test");
	}
}

void testWrongStateAndMalformedRequestsDoNotWin()
{
	GoogleOAuthLoopbackListener listener;
	std::optional<GoogleOAuthLoopbackCompletion> completion;
	int completionCount = 0;
	ArmedAttempt armed = armListener(listener, completion, completionCount);
	const quint16 port = listener.port();

	const auto wrongState =
		exchangeHttp(port, httpGet(port, QByteArray("/callback?code=attacker-code&state=wrong")));
	CHECK(wrongState.response.startsWith("HTTP/1.1 400"));
	CHECK(!completion.has_value());
	CHECK(listener.state() == GoogleOAuthLoopbackState::Armed);

	std::vector<QByteArray> invalidRequests;
	invalidRequests.push_back(QByteArray("POST /callback HTTP/1.1\r\nHost: 127.0.0.1:") + QByteArray::number(port) +
				  "\r\n\r\n");
	invalidRequests.push_back(QByteArray("GET http://127.0.0.1:") + QByteArray::number(port) +
				  "/callback HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\n\r\n");
	invalidRequests.push_back(httpGet(port, QByteArray("/wrong?code=x&state=") + armed.state.toLatin1()));
	invalidRequests.push_back(QByteArray("GET /callback HTTP/1.1\r\nHost: localhost:") + QByteArray::number(port) +
				  "\r\n\r\n");
	invalidRequests.push_back(httpGet(port, QByteArray("/callback?code=x&state=") + armed.state.toLatin1(),
					  QByteArray("Host: 127.0.0.1:") + QByteArray::number(port) + "\r\n"));
	invalidRequests.push_back(httpGet(port, QByteArray("/callback?code=x&state=") + armed.state.toLatin1(),
					  QByteArray("Content-Length: 1\r\n")) +
				  "x");
	invalidRequests.push_back(httpGet(port, QByteArray("/callback?code=x&state=") + armed.state.toLatin1(),
					  QByteArray("Transfer-Encoding: chunked\r\n")));
	invalidRequests.push_back(
		httpGet(port, QByteArray("/callback?code=x&error=access_denied&state=") + armed.state.toLatin1()));

	for (const QByteArray &request : invalidRequests) {
		const auto rejected = exchangeHttp(port, request);
		CHECK(rejected.response.startsWith("HTTP/1.1 400"));
		CHECK(!completion.has_value());
		CHECK(listener.state() == GoogleOAuthLoopbackState::Armed);
	}

	const auto accepted = exchangeHttp(
		port, httpGet(port, QByteArray("/callback?code=real-code&state=") + armed.state.toLatin1()));
	CHECK(accepted.response.startsWith("HTTP/1.1 200"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
	if (completion.has_value()) {
		CHECK(completion->callback.succeeded());
		CHECK(completion->callback.authorizationCode.view() == "real-code");
	}
}

void testOversizedInputIsRejectedAndListenerSurvives()
{
	GoogleOAuthLoopbackListener listener;
	std::optional<GoogleOAuthLoopbackCompletion> completion;
	int completionCount = 0;
	ArmedAttempt armed = armListener(listener, completion, completionCount, {8, 13});
	const quint16 port = listener.port();

	const QByteArray oversizedTarget =
		QByteArray("/callback?code=") + QByteArray(easy_multistream::kGoogleOAuthMaxCallbackUrlBytes, 'a');
	const auto oversizedRequest = exchangeHttp(port, httpGet(port, oversizedTarget));
	CHECK(oversizedRequest.response.startsWith("HTTP/1.1 400"));
	CHECK(listener.state() == GoogleOAuthLoopbackState::Armed);
	CHECK(!completion.has_value());

	const QByteArray oversizedHeader =
		QByteArray("X-Oversized: ") +
		QByteArray(easy_multistream::kGoogleOAuthLoopbackMaxHeaderLineBytes + 1, 'b') + "\r\n";
	const auto oversizedHeaderRequest = exchangeHttp(
		port, httpGet(port, QByteArray("/callback?code=x&state=") + armed.state.toLatin1(), oversizedHeader));
	CHECK(oversizedHeaderRequest.response.startsWith("HTTP/1.1 400"));
	CHECK(listener.state() == GoogleOAuthLoopbackState::Armed);

	const auto accepted = exchangeHttp(
		port, httpGet(port, QByteArray("/callback?code=bounded-code&state=") + armed.state.toLatin1()));
	CHECK(accepted.response.startsWith("HTTP/1.1 200"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	if (completion.has_value()) {
		CHECK(completion->callback.succeeded());
	}
}

void testProviderDenialIsTerminalAndSanitized()
{
	GoogleOAuthLoopbackListener listener;
	std::optional<GoogleOAuthLoopbackCompletion> completion;
	int completionCount = 0;
	ArmedAttempt armed = armListener(listener, completion, completionCount, {10, 20});
	const auto denied = exchangeHttp(
		listener.port(),
		httpGet(listener.port(), QByteArray("/callback?error=access_denied&error_description=private&state=") +
						 armed.state.toLatin1()));
	CHECK(denied.response.startsWith("HTTP/1.1 200"));
	CHECK(!denied.response.contains("private"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
	if (completion.has_value()) {
		CHECK(completion->status == GoogleOAuthLoopbackCompletionStatus::Callback);
		CHECK(completion->callback.status == GoogleOAuthCallbackStatus::ProviderError);
		CHECK(completion->callback.providerError == GoogleOAuthProviderError::AccessDenied);
		CHECK(completion->callback.authorizationCode.empty());
	}
}

void testAuthorizationTimeoutAndSilentCancel()
{
	GoogleOAuthLoopbackOptions timeoutOptions;
	timeoutOptions.authorizationTimeout = std::chrono::milliseconds(30);
	timeoutOptions.clientReadTimeout = std::chrono::milliseconds(20);
	GoogleOAuthLoopbackListener timedListener(timeoutOptions);
	std::optional<GoogleOAuthLoopbackCompletion> timedCompletion;
	int timedCount = 0;
	armListener(timedListener, timedCompletion, timedCount, {12, 1});
	CHECK(waitUntil([&]() { return timedCompletion.has_value(); }));
	CHECK(timedCount == 1);
	CHECK(timedListener.state() == GoogleOAuthLoopbackState::TimedOut);
	if (timedCompletion.has_value()) {
		CHECK(timedCompletion->status == GoogleOAuthLoopbackCompletionStatus::AuthorizationTimedOut);
	}

	GoogleOAuthLoopbackListener cancelledListener;
	std::optional<GoogleOAuthLoopbackCompletion> cancelledCompletion;
	int cancelledCount = 0;
	armListener(cancelledListener, cancelledCompletion, cancelledCount, {12, 2});
	const quint16 cancelledPort = cancelledListener.port();
	cancelledListener.cancel();
	CHECK(cancelledListener.state() == GoogleOAuthLoopbackState::Cancelled);
	CHECK(cancelledListener.port() == 0);
	CHECK(!waitUntil([&]() { return cancelledCompletion.has_value(); }, 50));
	CHECK(cancelledCount == 0);
	const auto afterCancel = exchangeHttp(
		cancelledPort, httpGet(cancelledPort, QByteArray("/callback?code=late&state=late")), -1, 100);
	CHECK(!afterCancel.connected);
}

void testSlowClientTimesOutWithoutEndingAuthorization()
{
	GoogleOAuthLoopbackOptions options;
	options.authorizationTimeout = std::chrono::seconds(1);
	options.clientReadTimeout = std::chrono::milliseconds(30);
	GoogleOAuthLoopbackListener listener(options);
	std::optional<GoogleOAuthLoopbackCompletion> completion;
	int completionCount = 0;
	ArmedAttempt armed = armListener(listener, completion, completionCount, {14, 2});

	const auto slow = exchangeHttp(listener.port(), QByteArray("GET /callback HTTP/1.1\r\n"));
	CHECK(slow.response.startsWith("HTTP/1.1 408"));
	CHECK(listener.state() == GoogleOAuthLoopbackState::Armed);
	CHECK(!completion.has_value());

	const auto accepted = exchangeHttp(
		listener.port(),
		httpGet(listener.port(), QByteArray("/callback?code=after-timeout&state=") + armed.state.toLatin1()));
	CHECK(accepted.response.startsWith("HTTP/1.1 200"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
}

void testRequestLimitCompletesOnce()
{
	GoogleOAuthLoopbackOptions options;
	options.maxConcurrentConnections = 1;
	options.maxTotalConnections = 2;
	GoogleOAuthLoopbackListener listener(options);
	std::optional<GoogleOAuthLoopbackCompletion> completion;
	int completionCount = 0;
	armListener(listener, completion, completionCount, {15, 4});

	for (int index = 0; index < 2; ++index) {
		const auto rejected = exchangeHttp(
			listener.port(), httpGet(listener.port(), QByteArray("/callback?code=x&state=wrong")));
		CHECK(rejected.response.startsWith("HTTP/1.1 400"));
	}
	const auto limited =
		exchangeHttp(listener.port(), httpGet(listener.port(), QByteArray("/callback?code=x&state=wrong")));
	CHECK(limited.response.startsWith("HTTP/1.1 503"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
	CHECK(listener.state() == GoogleOAuthLoopbackState::RequestLimitReached);
	if (completion.has_value()) {
		CHECK(completion->status == GoogleOAuthLoopbackCompletionStatus::RequestLimitReached);
	}
}

void testArmValidationClosesInvalidAttempt()
{
	GoogleOAuthLoopbackListener listener;
	const auto bound = listener.bind({16, 9});
	CHECK(bound.succeeded());
	const easy_multistream::SecureBuffer state =
		easy_multistream::SecureBuffer::copyOf("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	CHECK(listener.arm(QUrl(QStringLiteral("http://127.0.0.1:1/callback")),
			   easy_multistream::SecureBuffer::copyOf(state.view()),
			   [](GoogleOAuthLoopbackCompletion) {}) == GoogleOAuthLoopbackArmStatus::InvalidRedirectUri);
	CHECK(listener.state() == GoogleOAuthLoopbackState::Cancelled);
	CHECK(listener.port() == 0);
	const auto rebound = listener.bind({16, 10});
	CHECK(rebound.succeeded());
	CHECK(rebound.port != 0);
}

void testDestructionCancelsActiveSocketsAndCallbacks()
{
	auto listener = std::make_unique<GoogleOAuthLoopbackListener>();
	const auto bound = listener->bind({18, 1});
	CHECK(bound.succeeded());
	auto authorization = easy_multistream::makeGoogleOAuthAuthorizationRequest(
		QStringLiteral("client-id.apps.googleusercontent.com"), bound.port);
	CHECK(authorization.succeeded());
	int completionCount = 0;
	CHECK(listener->arm(authorization.request.redirectUri, std::move(authorization.request.state),
			    [&](GoogleOAuthLoopbackCompletion) { ++completionCount; }) ==
	      GoogleOAuthLoopbackArmStatus::Success);

	QTcpSocket client;
	client.connectToHost(QStringLiteral("127.0.0.1"), bound.port);
	CHECK(waitUntil([&]() { return client.state() == QAbstractSocket::ConnectedState; }));
	client.write("GET /callback HTTP/1.1\r\n");
	QCoreApplication::processEvents();
	listener.reset();
	client.abort();
	QCoreApplication::processEvents();
	CHECK(completionCount == 0);
}

void testQueuedCompletionCannotCrossARebind()
{
	GoogleOAuthLoopbackOptions options;
	options.authorizationTimeout = std::chrono::milliseconds(20);
	GoogleOAuthLoopbackListener listener(options);
	int firstCompletionCount = 0;
	std::optional<GoogleOAuthLoopbackCompletion> secondCompletion;
	int secondCompletionCount = 0;

	const auto firstBound = listener.bind({22, 1});
	CHECK(firstBound.succeeded());
	auto firstAuthorization = easy_multistream::makeGoogleOAuthAuthorizationRequest(
		QStringLiteral("client-id.apps.googleusercontent.com"), firstBound.port);
	CHECK(firstAuthorization.succeeded());
	CHECK(listener.arm(firstAuthorization.request.redirectUri, std::move(firstAuthorization.request.state),
			   [&](GoogleOAuthLoopbackCompletion) { ++firstCompletionCount; }) ==
	      GoogleOAuthLoopbackArmStatus::Success);

	RebindBeforeOldCompletion filter;
	filter.listener = &listener;
	filter.secondCompletion = &secondCompletion;
	filter.secondCompletionCount = &secondCompletionCount;
	listener.installEventFilter(&filter);
	CHECK(waitUntil([&]() { return secondCompletion.has_value(); }));
	listener.removeEventFilter(&filter);

	CHECK(filter.triggered);
	CHECK(filter.bindStatus == GoogleOAuthLoopbackBindStatus::Success);
	CHECK(filter.armStatus == GoogleOAuthLoopbackArmStatus::Success);
	CHECK(firstCompletionCount == 0);
	CHECK(secondCompletionCount == 1);
	if (secondCompletion.has_value()) {
		CHECK((secondCompletion->attempt == GoogleOAuthLoopbackAttempt{22, 2}));
		CHECK(secondCompletion->status == GoogleOAuthLoopbackCompletionStatus::AuthorizationTimedOut);
	}
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testExclusiveLoopbackBind();
	testBindBeforeArmAndFragmentedSuccess();
	testWrongStateAndMalformedRequestsDoNotWin();
	testOversizedInputIsRejectedAndListenerSurvives();
	testProviderDenialIsTerminalAndSanitized();
	testAuthorizationTimeoutAndSilentCancel();
	testSlowClientTimesOutWithoutEndingAuthorization();
	testRequestLimitCompletesOnce();
	testArmValidationClosesInvalidAttempt();
	testDestructionCancelsActiveSocketsAndCallbacks();
	testQueuedCompletionCannotCrossARebind();
	return failures == 0 ? 0 : 1;
}

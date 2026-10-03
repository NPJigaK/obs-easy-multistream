// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-authorization-session.hpp"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
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

using easy_multistream::GoogleOAuthAuthorizationCompletion;
using easy_multistream::GoogleOAuthAuthorizationCompletionStatus;
using easy_multistream::GoogleOAuthAuthorizationSession;
using easy_multistream::GoogleOAuthAuthorizationSessionState;
using easy_multistream::GoogleOAuthAuthorizationStartStatus;
using easy_multistream::GoogleOAuthConsentMode;
using easy_multistream::GoogleOAuthLoopbackAttempt;
using easy_multistream::GoogleOAuthLoopbackOptions;
using easy_multistream::GoogleOAuthProviderError;

constexpr char kClientId[] = "client-id.apps.googleusercontent.com";

struct HttpExchange final {
	QByteArray response;
	bool connected = false;
	bool timedOut = false;
};

struct AuthorizationUrl final {
	QUrl redirectUri;
	QByteArray state;
	QByteArray challenge;
};

class CancelBeforeQueuedDelivery final : public QObject {
public:
	GoogleOAuthAuthorizationSession *session = nullptr;
	GoogleOAuthLoopbackAttempt attempt;
	bool triggered = false;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (!triggered && watched == session && event->type() == QEvent::MetaCall &&
		    session->state() == GoogleOAuthAuthorizationSessionState::DeliveringCompletion) {
			triggered = true;
			CHECK(session->cancel(attempt));
		}
		return QObject::eventFilter(watched, event);
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

HttpExchange exchangeHttp(quint16 port, QByteArray request, int timeoutMilliseconds = 2000)
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
		socket.write(request);
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

QByteArray httpGet(quint16 port, const QByteArray &target)
{
	QByteArray request("GET ");
	request.append(target);
	request.append(" HTTP/1.1\r\nHost: 127.0.0.1:");
	request.append(QByteArray::number(port));
	request.append("\r\nConnection: close\r\n\r\n");
	return request;
}

AuthorizationUrl inspectAuthorizationUrl(const QUrl &url)
{
	CHECK(url.scheme() == QStringLiteral("https"));
	CHECK(url.host() == QStringLiteral("accounts.google.com"));
	CHECK(url.path() == QStringLiteral("/o/oauth2/v2/auth"));
	const QUrlQuery query(url);
	CHECK(query.queryItemValue(QStringLiteral("client_id"), QUrl::FullyDecoded) == QString::fromLatin1(kClientId));
	CHECK(query.queryItemValue(QStringLiteral("response_type"), QUrl::FullyDecoded) == QStringLiteral("code"));
	CHECK(query.queryItemValue(QStringLiteral("code_challenge_method"), QUrl::FullyDecoded) ==
	      QStringLiteral("S256"));
	CHECK(!query.hasQueryItem(QStringLiteral("code_verifier")));

	AuthorizationUrl result;
	result.redirectUri = QUrl(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded));
	result.state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded).toLatin1();
	result.challenge = query.queryItemValue(QStringLiteral("code_challenge"), QUrl::FullyDecoded).toLatin1();
	CHECK(result.redirectUri.scheme() == QStringLiteral("http"));
	CHECK(result.redirectUri.host() == QStringLiteral("127.0.0.1"));
	CHECK(result.redirectUri.path() == QStringLiteral("/callback"));
	CHECK(result.redirectUri.port() > 0);
	CHECK(result.state.size() == easy_multistream::kGoogleOAuthEncodedRandomLength);
	CHECK(result.challenge.size() == easy_multistream::kGoogleOAuthEncodedRandomLength);
	return result;
}

QByteArray callbackTarget(const AuthorizationUrl &authorization, const QByteArray &code)
{
	return QByteArray("/callback?code=") + code + "&state=" + authorization.state;
}

void testSuccessArmsBeforeOpeningAndReturnsOnlyExchangeSecrets()
{
	GoogleOAuthAuthorizationSession *sessionPointer = nullptr;
	QUrl openedUrl;
	GoogleOAuthAuthorizationSession session(
		[&](const QUrl &url) {
			CHECK(sessionPointer != nullptr);
			CHECK(sessionPointer->state() == GoogleOAuthAuthorizationSessionState::Authorizing);
			CHECK(sessionPointer->activeAttempt() == std::optional<GoogleOAuthLoopbackAttempt>({1, 1}));
			openedUrl = url;
			const AuthorizationUrl authorization = inspectAuthorizationUrl(url);
			const auto probe = exchangeHttp(
				static_cast<quint16>(authorization.redirectUri.port()),
				httpGet(static_cast<quint16>(authorization.redirectUri.port()),
					QByteArray("/callback?code=probe&state=wrong")));
			CHECK(probe.connected);
			CHECK(probe.response.startsWith("HTTP/1.1 400"));
			CHECK(sessionPointer->state() == GoogleOAuthAuthorizationSessionState::Authorizing);
			return true;
		});
	sessionPointer = &session;

	std::optional<GoogleOAuthAuthorizationCompletion> completion;
	int completionCount = 0;
	CHECK(session.start({1, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [&](GoogleOAuthAuthorizationCompletion value) {
				    ++completionCount;
				    completion.emplace(std::move(value));
			    }) == GoogleOAuthAuthorizationStartStatus::Started);

	const AuthorizationUrl authorization = inspectAuthorizationUrl(openedUrl);
	const QByteArray code("one-time-code-for-session-test");
	const auto success = exchangeHttp(static_cast<quint16>(authorization.redirectUri.port()),
					  httpGet(static_cast<quint16>(authorization.redirectUri.port()),
						  callbackTarget(authorization, code)));
	CHECK(success.connected);
	CHECK(success.response.startsWith("HTTP/1.1 200"));
	CHECK(!success.response.contains(code));
	CHECK(!success.response.contains(authorization.state));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(completionCount == 1);
	CHECK(session.state() == GoogleOAuthAuthorizationSessionState::Completed);
	CHECK(!session.activeAttempt().has_value());
	if (completion.has_value()) {
		CHECK(completion->succeeded());
		CHECK((completion->attempt == GoogleOAuthLoopbackAttempt{1, 1}));
		CHECK(completion->authorizationCode.view() == "one-time-code-for-session-test");
		CHECK(completion->redirectUri == authorization.redirectUri);
		CHECK(completion->codeVerifier.size() ==
		      static_cast<std::size_t>(easy_multistream::kGoogleOAuthEncodedRandomLength));
		QByteArray verifier = QByteArray::fromRawData(completion->codeVerifier.view().data(),
							     static_cast<qsizetype>(completion->codeVerifier.size()));
		CHECK(easy_multistream::googleOAuthPkceS256Challenge(verifier) == authorization.challenge);
		verifier.clear();
	}
}

void testProviderDenialIsSanitized()
{
	QUrl openedUrl;
	GoogleOAuthAuthorizationSession session([&](const QUrl &url) {
		openedUrl = url;
		return true;
	});
	std::optional<GoogleOAuthAuthorizationCompletion> completion;
	CHECK(session.start({2, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [&](GoogleOAuthAuthorizationCompletion value) { completion.emplace(std::move(value)); }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);

	const AuthorizationUrl authorization = inspectAuthorizationUrl(openedUrl);
	const QByteArray target = QByteArray("/callback?error=access_denied&error_description=private-sentinel&state=") +
				  authorization.state;
	const auto denied = exchangeHttp(static_cast<quint16>(authorization.redirectUri.port()),
					 httpGet(static_cast<quint16>(authorization.redirectUri.port()), target));
	CHECK(denied.response.startsWith("HTTP/1.1 200"));
	CHECK(!denied.response.contains("private-sentinel"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	if (completion.has_value()) {
		CHECK(completion->status == GoogleOAuthAuthorizationCompletionStatus::ProviderRejected);
		CHECK(completion->providerError == GoogleOAuthProviderError::AccessDenied);
		CHECK(completion->authorizationCode.empty());
		CHECK(completion->codeVerifier.empty());
		CHECK(completion->redirectUri.isEmpty());
	}
}

void testBrowserFailureAndInvalidRequestCloseTheListener()
{
	for (const bool throwFromOpener : {false, true}) {
		QUrl openedUrl;
		int completionCount = 0;
		GoogleOAuthAuthorizationSession session([&](const QUrl &url) -> bool {
			openedUrl = url;
			if (throwFromOpener) {
				throw std::runtime_error("fake browser failure");
			}
			return false;
		});
		CHECK(session.start({3, throwFromOpener ? 2U : 1U}, QString::fromLatin1(kClientId),
				    GoogleOAuthConsentMode::Standard,
				    [&](GoogleOAuthAuthorizationCompletion) { ++completionCount; }) ==
		      GoogleOAuthAuthorizationStartStatus::BrowserOpenFailed);
		const AuthorizationUrl authorization = inspectAuthorizationUrl(openedUrl);
		CHECK(session.state() == GoogleOAuthAuthorizationSessionState::Idle);
		CHECK(!session.activeAttempt().has_value());
		CHECK(completionCount == 0);
		const auto afterFailure = exchangeHttp(
			static_cast<quint16>(authorization.redirectUri.port()),
			httpGet(static_cast<quint16>(authorization.redirectUri.port()), callbackTarget(authorization, "late")),
			100);
		CHECK(!afterFailure.connected);
	}

	bool opened = false;
	GoogleOAuthAuthorizationSession invalidSession([&](const QUrl &) {
		opened = true;
		return true;
	});
	CHECK(invalidSession.start({3, 3}, QStringLiteral("invalid client id"), GoogleOAuthConsentMode::Standard,
				   [](GoogleOAuthAuthorizationCompletion) {}) ==
	      GoogleOAuthAuthorizationStartStatus::AuthorizationRequestFailed);
	CHECK(!opened);
	CHECK(invalidSession.state() == GoogleOAuthAuthorizationSessionState::Idle);
	CHECK(invalidSession.start({3, 4}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
				   [](GoogleOAuthAuthorizationCompletion) {}) ==
	      GoogleOAuthAuthorizationStartStatus::Started);
	CHECK(invalidSession.cancel({3, 4}));
}

void testBusyAttemptScopedCancelRestartAndShutdown()
{
	std::vector<QUrl> openedUrls;
	int completionCount = 0;
	GoogleOAuthAuthorizationSession session([&](const QUrl &url) {
		openedUrls.push_back(url);
		return true;
	});
	CHECK(session.start({4, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [&](GoogleOAuthAuthorizationCompletion) { ++completionCount; }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);
	CHECK(session.start({4, 2}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [](GoogleOAuthAuthorizationCompletion) {}) == GoogleOAuthAuthorizationStartStatus::Busy);
	CHECK(!session.cancel({4, 2}));
	CHECK(session.cancel({4, 1}));
	CHECK(session.state() == GoogleOAuthAuthorizationSessionState::Cancelled);
	CHECK(completionCount == 0);
	const AuthorizationUrl oldAuthorization = inspectAuthorizationUrl(openedUrls.front());
	const auto oldCallback = exchangeHttp(
		static_cast<quint16>(oldAuthorization.redirectUri.port()),
		httpGet(static_cast<quint16>(oldAuthorization.redirectUri.port()),
			callbackTarget(oldAuthorization, "old-code")),
		100);
	CHECK(!oldCallback.connected);

	std::optional<GoogleOAuthAuthorizationCompletion> completion;
	CHECK(session.start({5, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::ForceConsent,
			    [&](GoogleOAuthAuthorizationCompletion value) { completion.emplace(std::move(value)); }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);
	CHECK(openedUrls.size() == 2);
	const AuthorizationUrl current = inspectAuthorizationUrl(openedUrls.back());
	CHECK(QUrlQuery(openedUrls.back()).queryItemValue(QStringLiteral("prompt"), QUrl::FullyDecoded) ==
	      QStringLiteral("consent"));
	const auto accepted = exchangeHttp(static_cast<quint16>(current.redirectUri.port()),
					   httpGet(static_cast<quint16>(current.redirectUri.port()),
						   callbackTarget(current, "current-code")));
	CHECK(accepted.response.startsWith("HTTP/1.1 200"));
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	CHECK(session.shutdown());
	CHECK(session.shutdown());
	CHECK(session.state() == GoogleOAuthAuthorizationSessionState::Closed);
	CHECK(session.start({6, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [](GoogleOAuthAuthorizationCompletion) {}) == GoogleOAuthAuthorizationStartStatus::Closed);
}

void testTimeoutAndDestructionAreSilentAndBounded()
{
	GoogleOAuthLoopbackOptions options;
	options.authorizationTimeout = std::chrono::milliseconds(30);
	options.clientReadTimeout = std::chrono::milliseconds(20);
	QUrl openedUrl;
	std::optional<GoogleOAuthAuthorizationCompletion> completion;
	GoogleOAuthAuthorizationSession timedSession(
		[&](const QUrl &url) {
			openedUrl = url;
			return true;
		},
		options);
	CHECK(timedSession.start({7, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
				 [&](GoogleOAuthAuthorizationCompletion value) { completion.emplace(std::move(value)); }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);
	CHECK(waitUntil([&]() { return completion.has_value(); }));
	if (completion.has_value()) {
		CHECK(completion->status == GoogleOAuthAuthorizationCompletionStatus::AuthorizationTimedOut);
		CHECK(completion->authorizationCode.empty());
		CHECK(completion->codeVerifier.empty());
		CHECK(completion->redirectUri.isEmpty());
	}

	QUrl destroyedUrl;
	int destroyedCompletionCount = 0;
	auto destroyedSession = std::make_unique<GoogleOAuthAuthorizationSession>([&](const QUrl &url) {
		destroyedUrl = url;
		return true;
	});
	CHECK(destroyedSession->start({7, 2}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
				      [&](GoogleOAuthAuthorizationCompletion) { ++destroyedCompletionCount; }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);
	const AuthorizationUrl destroyedAuthorization = inspectAuthorizationUrl(destroyedUrl);
	destroyedSession.reset();
	QCoreApplication::processEvents();
	CHECK(destroyedCompletionCount == 0);
	const auto afterDestruction = exchangeHttp(
		static_cast<quint16>(destroyedAuthorization.redirectUri.port()),
		httpGet(static_cast<quint16>(destroyedAuthorization.redirectUri.port()),
			callbackTarget(destroyedAuthorization, "late")),
		100);
	CHECK(!afterDestruction.connected);
}

void testReentrantSuccessfulCallbackWinsOverFalseBrowserReturn()
{
	GoogleOAuthAuthorizationSession *sessionPointer = nullptr;
	std::optional<GoogleOAuthAuthorizationCompletion> completion;
	GoogleOAuthAuthorizationSession session([&](const QUrl &url) {
		const AuthorizationUrl authorization = inspectAuthorizationUrl(url);
		const auto accepted = exchangeHttp(
			static_cast<quint16>(authorization.redirectUri.port()),
			httpGet(static_cast<quint16>(authorization.redirectUri.port()),
				callbackTarget(authorization, "reentrant-code")));
		CHECK(accepted.response.startsWith("HTTP/1.1 200"));
		CHECK(waitUntil([&]() { return completion.has_value(); }));
		CHECK(sessionPointer->state() == GoogleOAuthAuthorizationSessionState::Completed);
		return false;
	});
	sessionPointer = &session;
	CHECK(session.start({8, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [&](GoogleOAuthAuthorizationCompletion value) { completion.emplace(std::move(value)); }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);
	CHECK(completion.has_value());
	if (completion.has_value()) {
		CHECK(completion->succeeded());
		CHECK(completion->authorizationCode.view() == "reentrant-code");
	}
}

void testReentrantCancelAndShutdownHaveExplicitStartResults()
{
	GoogleOAuthAuthorizationSession *cancelSessionPointer = nullptr;
	const GoogleOAuthLoopbackAttempt cancelAttempt{9, 1};
	GoogleOAuthAuthorizationSession cancelSession([&](const QUrl &) {
		CHECK(cancelSessionPointer->cancel(cancelAttempt));
		return true;
	});
	cancelSessionPointer = &cancelSession;
	CHECK(cancelSession.start(cancelAttempt, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
				  [](GoogleOAuthAuthorizationCompletion) {}) ==
	      GoogleOAuthAuthorizationStartStatus::Cancelled);
	CHECK(cancelSession.state() == GoogleOAuthAuthorizationSessionState::Cancelled);

	GoogleOAuthAuthorizationSession *shutdownSessionPointer = nullptr;
	GoogleOAuthAuthorizationSession shutdownSession([&](const QUrl &) {
		CHECK(shutdownSessionPointer->shutdown());
		return true;
	});
	shutdownSessionPointer = &shutdownSession;
	CHECK(shutdownSession.start({9, 2}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
				    [](GoogleOAuthAuthorizationCompletion) {}) ==
	      GoogleOAuthAuthorizationStartStatus::Closed);
	CHECK(shutdownSession.state() == GoogleOAuthAuthorizationSessionState::Closed);
}

void testCancelSuppressesQueuedCompletionAndAllowsNewAttempt()
{
	std::vector<QUrl> openedUrls;
	int oldCompletionCount = 0;
	GoogleOAuthAuthorizationSession session([&](const QUrl &url) {
		openedUrls.push_back(url);
		return true;
	});
	const GoogleOAuthLoopbackAttempt oldAttempt{10, 1};
	CHECK(session.start(oldAttempt, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [&](GoogleOAuthAuthorizationCompletion) { ++oldCompletionCount; }) ==
	      GoogleOAuthAuthorizationStartStatus::Started);

	CancelBeforeQueuedDelivery filter;
	filter.session = &session;
	filter.attempt = oldAttempt;
	session.installEventFilter(&filter);
	const AuthorizationUrl oldAuthorization = inspectAuthorizationUrl(openedUrls.back());
	const auto oldAccepted = exchangeHttp(
		static_cast<quint16>(oldAuthorization.redirectUri.port()),
		httpGet(static_cast<quint16>(oldAuthorization.redirectUri.port()),
			callbackTarget(oldAuthorization, "cancelled-before-delivery")));
	CHECK(oldAccepted.response.startsWith("HTTP/1.1 200"));
	CHECK(waitUntil([&]() { return filter.triggered; }));
	session.removeEventFilter(&filter);
	QCoreApplication::processEvents();
	CHECK(oldCompletionCount == 0);
	CHECK(session.state() == GoogleOAuthAuthorizationSessionState::Cancelled);

	std::optional<GoogleOAuthAuthorizationCompletion> currentCompletion;
	CHECK(session.start({11, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [&](GoogleOAuthAuthorizationCompletion value) {
				    currentCompletion.emplace(std::move(value));
			    }) == GoogleOAuthAuthorizationStartStatus::Started);
	const AuthorizationUrl currentAuthorization = inspectAuthorizationUrl(openedUrls.back());
	const auto currentAccepted = exchangeHttp(
		static_cast<quint16>(currentAuthorization.redirectUri.port()),
		httpGet(static_cast<quint16>(currentAuthorization.redirectUri.port()),
			callbackTarget(currentAuthorization, "current-after-cancel")));
	CHECK(currentAccepted.response.startsWith("HTTP/1.1 200"));
	CHECK(waitUntil([&]() { return currentCompletion.has_value(); }));
	if (currentCompletion.has_value()) {
		CHECK((currentCompletion->attempt == GoogleOAuthLoopbackAttempt{11, 1}));
		CHECK(currentCompletion->authorizationCode.view() == "current-after-cancel");
	}
}

void testWrongThreadAndInputValidationDoNotMutateState()
{
	GoogleOAuthAuthorizationSession emptyOpener({});
	CHECK(emptyOpener.start({9, 1}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
				[](GoogleOAuthAuthorizationCompletion) {}) ==
	      GoogleOAuthAuthorizationStartStatus::InvalidBrowserOpener);

	GoogleOAuthAuthorizationSession session([](const QUrl &) { return true; });
	CHECK(session.start({}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard,
			    [](GoogleOAuthAuthorizationCompletion) {}) ==
	      GoogleOAuthAuthorizationStartStatus::InvalidAttempt);
	CHECK(session.start({9, 2}, QString::fromLatin1(kClientId), GoogleOAuthConsentMode::Standard, {}) ==
	      GoogleOAuthAuthorizationStartStatus::InvalidCompletionHandler);

	GoogleOAuthAuthorizationStartStatus crossThreadStatus = GoogleOAuthAuthorizationStartStatus::Started;
	bool crossThreadShutdown = true;
	std::thread other([&]() {
		crossThreadStatus = session.start({9, 3}, QString::fromLatin1(kClientId),
						  GoogleOAuthConsentMode::Standard,
						  [](GoogleOAuthAuthorizationCompletion) {});
		crossThreadShutdown = session.shutdown();
	});
	other.join();
	CHECK(crossThreadStatus == GoogleOAuthAuthorizationStartStatus::WrongThread);
	CHECK(!crossThreadShutdown);
	CHECK(session.state() == GoogleOAuthAuthorizationSessionState::Idle);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testSuccessArmsBeforeOpeningAndReturnsOnlyExchangeSecrets();
	testProviderDenialIsSanitized();
	testBrowserFailureAndInvalidRequestCloseTheListener();
	testBusyAttemptScopedCancelRestartAndShutdown();
	testTimeoutAndDestructionAreSilentAndBounded();
	testReentrantSuccessfulCallbackWinsOverFalseBrowserReturn();
	testReentrantCancelAndShutdownHaveExplicitStartResults();
	testCancelSuppressesQueuedCompletionAndAllowsNewAttempt();
	testWrongThreadAndInputValidationDoNotMutateState();
	if (failures != 0) {
		std::cerr << failures << " authorization-session test(s) failed\n";
		return 1;
	}
	std::cout << "All authorization-session tests passed\n";
	return 0;
}

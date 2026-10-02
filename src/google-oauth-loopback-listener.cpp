// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-loopback-listener.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <QAbstractSocket>
#include <QByteArray>
#include <QByteArrayView>
#include <QMetaObject>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace easy_multistream {
namespace {

constexpr std::chrono::milliseconds kMaximumAuthorizationTimeout = std::chrono::minutes(10);
constexpr std::chrono::milliseconds kMaximumClientReadTimeout = std::chrono::minutes(1);
constexpr std::chrono::milliseconds kResponseFlushTimeout = std::chrono::seconds(1);
constexpr std::size_t kMaximumConfiguredConnections = 128;

void wipe(QByteArray &bytes) noexcept
{
	volatile char *data = bytes.data();
	for (qsizetype index = 0; index < bytes.size(); ++index) {
		data[index] = 0;
	}
	bytes.clear();
}

void wipe(QList<QByteArray> &lines) noexcept
{
	for (QByteArray &line : lines) {
		wipe(line);
	}
	lines.clear();
}

bool isTokenCharacter(unsigned char byte) noexcept
{
	if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')) {
		return true;
	}

	switch (byte) {
	case '!':
	case '#':
	case '$':
	case '%':
	case '&':
	case '\'':
	case '*':
	case '+':
	case '-':
	case '.':
	case '^':
	case '_':
	case '`':
	case '|':
	case '~':
		return true;
	default:
		return false;
	}
}

bool isValidHeaderName(const QByteArray &name) noexcept
{
	return !name.isEmpty() && std::all_of(name.cbegin(), name.cend(), [](char value) {
		return isTokenCharacter(static_cast<unsigned char>(value));
	});
}

bool isValidHeaderValue(const QByteArray &value) noexcept
{
	return std::all_of(value.cbegin(), value.cend(), [](char character) {
		const unsigned char byte = static_cast<unsigned char>(character);
		return byte == '\t' || (byte >= 0x20U && byte < 0x7FU);
	});
}

bool isAsciiRequestTarget(const QByteArray &target) noexcept
{
	return std::all_of(target.cbegin(), target.cend(), [](char character) {
		const unsigned char byte = static_cast<unsigned char>(character);
		return byte >= 0x21U && byte < 0x7FU;
	});
}

bool isDecimal(const QByteArray &value) noexcept
{
	return !value.isEmpty() && std::all_of(value.cbegin(), value.cend(),
					       [](char character) { return character >= '0' && character <= '9'; });
}

bool isValidRandomState(const SecureBuffer &state) noexcept
{
	const std::string_view value = state.view();
	if (value.size() != static_cast<std::size_t>(kGoogleOAuthEncodedRandomLength)) {
		return false;
	}

	return std::all_of(value.cbegin(), value.cend(), [](char character) {
		const unsigned char byte = static_cast<unsigned char>(character);
		return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
		       byte == '-' || byte == '_';
	});
}

QUrl exactRedirectUri(quint16 port)
{
	QUrl result;
	result.setScheme(QStringLiteral("http"));
	result.setHost(QString::fromLatin1(kGoogleOAuthLoopbackHost));
	result.setPort(port);
	result.setPath(QString::fromLatin1(kGoogleOAuthCallbackPath));
	return result;
}

bool isExactRedirectUriForPort(const QUrl &value, quint16 port) noexcept
{
	const QUrl expected = exactRedirectUri(port);
	return value.isValid() && !value.isRelative() &&
	       value.toEncoded(QUrl::FullyEncoded) == expected.toEncoded(QUrl::FullyEncoded);
}

QByteArray makeHttpResponse(QByteArrayView status, QByteArrayView body)
{
	QByteArray response;
	response.reserve(512 + body.size());
	response.append("HTTP/1.1 ");
	response.append(status);
	response.append("\r\nContent-Type: text/html; charset=utf-8\r\n");
	response.append("Cache-Control: no-store\r\nPragma: no-cache\r\n");
	response.append(
		"Content-Security-Policy: default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'\r\n");
	response.append("Referrer-Policy: no-referrer\r\nX-Content-Type-Options: nosniff\r\n");
	response.append("Connection: close\r\nContent-Length: ");
	response.append(QByteArray::number(body.size()));
	response.append("\r\n\r\n");
	response.append(body);
	return response;
}

const QByteArray &acceptedResponse()
{
	static const QByteArray response = makeHttpResponse(
		QByteArrayView("200 OK"),
		QByteArrayView("<!doctype html><meta charset=utf-8><title>Easy Multistream</title>"
			       "<p>Authorization completed. You can close this page and return to OBS.</p>"));
	return response;
}

const QByteArray &declinedResponse()
{
	static const QByteArray response = makeHttpResponse(
		QByteArrayView("200 OK"),
		QByteArrayView(
			"<!doctype html><meta charset=utf-8><title>Easy Multistream</title>"
			"<p>Authorization was cancelled or declined. You can close this page and return to OBS.</p>"));
	return response;
}

const QByteArray &rejectedResponse()
{
	static const QByteArray response = makeHttpResponse(
		QByteArrayView("400 Bad Request"),
		QByteArrayView("<!doctype html><meta charset=utf-8><title>Easy Multistream</title>"
			       "<p>The authorization response was not accepted. Return to OBS and try again.</p>"));
	return response;
}

const QByteArray &timeoutResponse()
{
	static const QByteArray response = makeHttpResponse(
		QByteArrayView("408 Request Timeout"),
		QByteArrayView("<!doctype html><meta charset=utf-8><title>Easy Multistream</title>"
			       "<p>The local authorization request expired. Return to OBS and try again.</p>"));
	return response;
}

const QByteArray &busyResponse()
{
	static const QByteArray response = makeHttpResponse(
		QByteArrayView("503 Service Unavailable"),
		QByteArrayView("<!doctype html><meta charset=utf-8><title>Easy Multistream</title>"
			       "<p>The local authorization listener is busy. Return to OBS and try again.</p>"));
	return response;
}

bool optionsAreValid(const GoogleOAuthLoopbackOptions &options) noexcept
{
	return options.authorizationTimeout.count() > 0 &&
	       options.authorizationTimeout <= kMaximumAuthorizationTimeout && options.clientReadTimeout.count() > 0 &&
	       options.clientReadTimeout <= kMaximumClientReadTimeout && options.maxConcurrentConnections > 0 &&
	       options.maxConcurrentConnections <= kMaximumConfiguredConnections &&
	       options.maxTotalConnections >= options.maxConcurrentConnections &&
	       options.maxTotalConnections <= kMaximumConfiguredConnections;
}

} // namespace

class GoogleOAuthLoopbackListener::Impl final {
public:
	Impl(GoogleOAuthLoopbackListener *owner, GoogleOAuthLoopbackOptions options) : owner_(owner), options_(options)
	{
		authorizationTimer_.setSingleShot(true);
		responseTimer_.setSingleShot(true);
		QObject::connect(&server_, &QTcpServer::newConnection, owner_,
				 [this]() { acceptPendingConnections(); });
		QObject::connect(&server_, &QTcpServer::acceptError, owner_,
				 [this](QAbstractSocket::SocketError) { listenerFailed(); });
		QObject::connect(&authorizationTimer_, &QTimer::timeout, owner_, [this]() { authorizationTimedOut(); });
		QObject::connect(&responseTimer_, &QTimer::timeout, owner_, [this]() { responseFlushTimedOut(); });
	}

	~Impl() { destroy(); }

	GoogleOAuthLoopbackBindResult bind(GoogleOAuthLoopbackAttempt newAttempt) noexcept
	{
		GoogleOAuthLoopbackBindResult result;
		if (!onOwnerThread()) {
			result.status = GoogleOAuthLoopbackBindStatus::WrongThread;
			return result;
		}
		if (!newAttempt.isValid()) {
			result.status = GoogleOAuthLoopbackBindStatus::InvalidAttempt;
			return result;
		}
		if (!optionsAreValid(options_)) {
			result.status = GoogleOAuthLoopbackBindStatus::InvalidOptions;
			return result;
		}
		if (state_ == GoogleOAuthLoopbackState::Listening || state_ == GoogleOAuthLoopbackState::Armed ||
		    pendingCompletion_.has_value()) {
			result.status = GoogleOAuthLoopbackBindStatus::AlreadyActive;
			return result;
		}

		resetForBind();
		attempt_ = newAttempt;

		if (!winsockStarted_) {
			WSADATA data{};
			const int startupResult = WSAStartup(MAKEWORD(2, 2), &data);
			if (startupResult != 0 || LOBYTE(data.wVersion) != 2 || HIBYTE(data.wVersion) != 2) {
				if (startupResult == 0) {
					WSACleanup();
				}
				state_ = GoogleOAuthLoopbackState::NetworkFailed;
				result.status = GoogleOAuthLoopbackBindStatus::NetworkInitializationFailed;
				return result;
			}
			winsockStarted_ = true;
		}

		SOCKET nativeSocket = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (nativeSocket == INVALID_SOCKET) {
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::SocketCreationFailed;
			return result;
		}

		const BOOL exclusive = TRUE;
		if (setsockopt(nativeSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
			       reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::ExclusiveAddressFailed;
			return result;
		}

		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(0);
		if (::bind(nativeSocket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) ==
		    SOCKET_ERROR) {
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::BindFailed;
			return result;
		}

		const int backlog = static_cast<int>(options_.maxConcurrentConnections);
		if (::listen(nativeSocket, backlog) == SOCKET_ERROR) {
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::ListenFailed;
			return result;
		}

		int addressLength = sizeof(address);
		if (getsockname(nativeSocket, reinterpret_cast<sockaddr *>(&address), &addressLength) == SOCKET_ERROR) {
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::BindFailed;
			return result;
		}
		const quint16 assignedPort = ntohs(address.sin_port);
		if (assignedPort == 0) {
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::BindFailed;
			return result;
		}

		// QTcpServer's descriptor adoption makes the socket non-blocking. Do
		// that explicitly first so its only normal failure path is descriptor
		// inspection, where Qt leaves ownership with the caller. This avoids
		// the ambiguous failure case in which Qt closes a descriptor after a
		// failed non-blocking transition and the caller closes it again.
		u_long nonBlocking = 1;
		if (ioctlsocket(nativeSocket, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::SocketAdoptionFailed;
			return result;
		}

		server_.setMaxPendingConnections(backlog);
		if (!server_.setSocketDescriptor(static_cast<qintptr>(nativeSocket))) {
			// QTcpServer assumes ownership only when adoption succeeds.
			closesocket(nativeSocket);
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::SocketAdoptionFailed;
			return result;
		}

		port_ = assignedPort;
		if (!server_.isListening() || server_.serverAddress().toIPv4Address() != INADDR_LOOPBACK ||
		    server_.serverPort() != port_) {
			server_.close();
			port_ = 0;
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			result.status = GoogleOAuthLoopbackBindStatus::SocketAdoptionFailed;
			return result;
		}

		state_ = GoogleOAuthLoopbackState::Listening;
		result.status = GoogleOAuthLoopbackBindStatus::Success;
		result.port = port_;
		return result;
	}

	GoogleOAuthLoopbackArmStatus arm(const QUrl &expectedRedirectUri, SecureBuffer newExpectedState,
					 GoogleOAuthLoopbackListener::CompletionHandler newCompletionHandler) noexcept
	{
		if (!onOwnerThread()) {
			return GoogleOAuthLoopbackArmStatus::WrongThread;
		}
		if (state_ != GoogleOAuthLoopbackState::Listening || !server_.isListening() || port_ == 0) {
			return GoogleOAuthLoopbackArmStatus::NotListening;
		}
		if (!isExactRedirectUriForPort(expectedRedirectUri, port_)) {
			cancel();
			return GoogleOAuthLoopbackArmStatus::InvalidRedirectUri;
		}
		if (!isValidRandomState(newExpectedState)) {
			cancel();
			return GoogleOAuthLoopbackArmStatus::InvalidState;
		}
		if (!newCompletionHandler) {
			cancel();
			return GoogleOAuthLoopbackArmStatus::InvalidCompletionHandler;
		}

		expectedRedirectUri_ = expectedRedirectUri;
		expectedState_ = std::move(newExpectedState);
		completionHandler_ = std::move(newCompletionHandler);
		state_ = GoogleOAuthLoopbackState::Armed;
		authorizationTimer_.start(static_cast<int>(options_.authorizationTimeout.count()));
		return GoogleOAuthLoopbackArmStatus::Success;
	}

	void cancel() noexcept
	{
		if (!onOwnerThread()) {
			return;
		}
		const bool active = state_ == GoogleOAuthLoopbackState::Listening ||
				    state_ == GoogleOAuthLoopbackState::Armed || pendingCompletion_.has_value();
		if (!active) {
			return;
		}

		state_ = GoogleOAuthLoopbackState::Cancelled;
		shutdownActiveAttempt();
	}

	GoogleOAuthLoopbackState state() const noexcept { return state_; }
	quint16 port() const noexcept { return port_; }
	GoogleOAuthLoopbackAttempt attempt() const noexcept { return attempt_; }

private:
	struct Client final {
		QTcpSocket *socket = nullptr;
		QTimer *readTimer = nullptr;
		QByteArray request;
		bool responding = false;
	};

	bool onOwnerThread() const noexcept
	{
		QThread *current = QThread::currentThread();
		return owner_ != nullptr && current == owner_->thread() && current == server_.thread() &&
		       current == authorizationTimer_.thread() && current == responseTimer_.thread();
	}

	void resetForBind() noexcept
	{
		shutdownActiveAttempt();
		state_ = GoogleOAuthLoopbackState::Idle;
		attempt_ = {};
		port_ = 0;
		totalConnections_ = 0;
	}

	void shutdownActiveAttempt() noexcept
	{
		advanceCompletionEpoch();
		authorizationTimer_.stop();
		responseTimer_.stop();
		server_.close();
		closeAllClients();
		expectedState_.clear();
		expectedRedirectUri_.clear();
		completionHandler_ = {};
		pendingCompletion_.reset();
		terminalSocket_ = nullptr;
		port_ = 0;
	}

	void destroy() noexcept
	{
		authorizationTimer_.stop();
		responseTimer_.stop();
		QObject::disconnect(&server_, nullptr, owner_, nullptr);
		QObject::disconnect(&authorizationTimer_, nullptr, owner_, nullptr);
		QObject::disconnect(&responseTimer_, nullptr, owner_, nullptr);
		server_.close();

		while (!clients_.empty()) {
			auto iterator = clients_.begin();
			QTcpSocket *socket = iterator->first;
			wipe(iterator->second->request);
			if (iterator->second->readTimer != nullptr) {
				iterator->second->readTimer->stop();
			}
			QObject::disconnect(socket, nullptr, owner_, nullptr);
			socket->abort();
			clients_.erase(iterator);
			socket->setParent(nullptr);
			delete socket;
		}

		// Rejected over-capacity sockets are intentionally not part of the
		// parser map, but they are still QTcpServer children and must be gone
		// before the matching WSACleanup call.
		for (;;) {
			QTcpSocket *socket = nullptr;
			for (QObject *child : server_.children()) {
				socket = qobject_cast<QTcpSocket *>(child);
				if (socket != nullptr) {
					break;
				}
			}
			if (socket == nullptr) {
				break;
			}
			QObject::disconnect(socket, nullptr, nullptr, nullptr);
			socket->abort();
			socket->setParent(nullptr);
			delete socket;
		}
		expectedState_.clear();
		pendingCompletion_.reset();
		completionHandler_ = {};

		if (winsockStarted_) {
			WSACleanup();
			winsockStarted_ = false;
		}
	}

	void acceptPendingConnections()
	{
		if (state_ != GoogleOAuthLoopbackState::Listening && state_ != GoogleOAuthLoopbackState::Armed) {
			discardPendingConnections();
			return;
		}
		while (server_.hasPendingConnections()) {
			QTcpSocket *socket = server_.nextPendingConnection();
			if (socket == nullptr) {
				continue;
			}

			++totalConnections_;
			if (totalConnections_ > options_.maxTotalConnections) {
				replyAndDiscard(socket, busyResponse());
				requestLimitReached();
				return;
			}
			if (clients_.size() >= options_.maxConcurrentConnections) {
				replyAndDiscard(socket, busyResponse());
				continue;
			}

			auto client = std::make_unique<Client>();
			client->socket = socket;
			client->readTimer = new QTimer(socket);
			client->readTimer->setSingleShot(true);
			client->readTimer->setInterval(static_cast<int>(options_.clientReadTimeout.count()));
			socket->setReadBufferSize(maximumRequestBytes() + 1);

			clients_.emplace(socket, std::move(client));
			QObject::connect(socket, &QTcpSocket::readyRead, owner_,
					 [this, socket]() { readClient(socket); });
			QObject::connect(socket, &QTcpSocket::disconnected, owner_,
					 [this, socket]() { clientDisconnected(socket); });
			QObject::connect(clients_.at(socket)->readTimer, &QTimer::timeout, owner_,
					 [this, socket]() { clientReadTimedOut(socket); });
			clients_.at(socket)->readTimer->start();
		}
	}

	void discardPendingConnections() noexcept
	{
		while (server_.hasPendingConnections()) {
			QTcpSocket *socket = server_.nextPendingConnection();
			if (socket == nullptr) {
				continue;
			}
			socket->abort();
			socket->deleteLater();
		}
	}

	qint64 maximumRequestBytes() const noexcept
	{
		return static_cast<qint64>(kGoogleOAuthLoopbackMaxRequestLineBytes) +
		       static_cast<qint64>(kGoogleOAuthLoopbackMaxHeaderBytes) + 4;
	}

	void readClient(QTcpSocket *socket)
	{
		auto iterator = clients_.find(socket);
		if (iterator == clients_.end() || iterator->second->responding) {
			return;
		}
		Client &client = *iterator->second;

		const qint64 remaining = maximumRequestBytes() - client.request.size();
		if (remaining < 0 || socket->bytesAvailable() > remaining) {
			rejectClient(socket);
			return;
		}

		QByteArray chunk = socket->read(remaining + 1);
		if (chunk.size() > remaining) {
			wipe(chunk);
			rejectClient(socket);
			return;
		}
		client.request.append(chunk);
		wipe(chunk);

		const qsizetype headerEnd = client.request.indexOf("\r\n\r\n");
		if (headerEnd < 0) {
			const qsizetype requestLineEnd = client.request.indexOf("\r\n");
			if ((requestLineEnd < 0 && client.request.size() > kGoogleOAuthLoopbackMaxRequestLineBytes) ||
			    requestLineEnd > kGoogleOAuthLoopbackMaxRequestLineBytes) {
				rejectClient(socket);
			}
			return;
		}

		QUrl callbackUrl;
		if (!parseHttpRequest(client.request, headerEnd, callbackUrl)) {
			rejectClient(socket);
			return;
		}
		wipe(client.request);

		if (state_ != GoogleOAuthLoopbackState::Armed) {
			rejectClient(socket);
			return;
		}

		GoogleOAuthCallbackResult callback =
			parseGoogleOAuthCallback(expectedRedirectUri_, expectedState_, callbackUrl);
		if (callback.status == GoogleOAuthCallbackStatus::Success ||
		    callback.status == GoogleOAuthCallbackStatus::ProviderError) {
			completeCallback(socket, std::move(callback));
			return;
		}

		rejectClient(socket);
	}

	bool parseHttpRequest(const QByteArray &request, qsizetype headerEnd, QUrl &callbackUrl) const
	{
		if (headerEnd <= 0 || headerEnd > maximumRequestBytes() || request.size() != headerEnd + 4) {
			return false;
		}

		QByteArray headerSection = request.left(headerEnd);
		QList<QByteArray> lines = headerSection.split('\n');
		wipe(headerSection);
		if (lines.isEmpty() || lines.size() - 1 > kGoogleOAuthLoopbackMaxHeaderCount) {
			wipe(lines);
			return false;
		}
		for (qsizetype index = 0; index < lines.size(); ++index) {
			QByteArray &line = lines[index];
			const bool hasFollowingLine = index + 1 < lines.size();
			if (hasFollowingLine && !line.endsWith('\r')) {
				wipe(lines);
				return false;
			}
			if (hasFollowingLine) {
				line.chop(1);
			}
		}

		QByteArray requestLine = lines.takeFirst();
		if (requestLine.isEmpty() || requestLine.size() > kGoogleOAuthLoopbackMaxRequestLineBytes) {
			wipe(requestLine);
			wipe(lines);
			return false;
		}
		const qsizetype firstSpace = requestLine.indexOf(' ');
		const qsizetype secondSpace = firstSpace < 0 ? -1 : requestLine.indexOf(' ', firstSpace + 1);
		if (firstSpace <= 0 || secondSpace <= firstSpace + 1 ||
		    requestLine.indexOf(' ', secondSpace + 1) >= 0 || requestLine.left(firstSpace) != "GET" ||
		    requestLine.mid(secondSpace + 1) != "HTTP/1.1") {
			wipe(requestLine);
			wipe(lines);
			return false;
		}

		QByteArray requestTarget = requestLine.mid(firstSpace + 1, secondSpace - firstSpace - 1);
		wipe(requestLine);
		if (requestTarget.isEmpty() || requestTarget.size() > kGoogleOAuthMaxCallbackUrlBytes ||
		    !requestTarget.startsWith('/') || requestTarget.startsWith("//") || requestTarget.contains('#') ||
		    !isAsciiRequestTarget(requestTarget)) {
			wipe(requestTarget);
			wipe(lines);
			return false;
		}

		QSet<QByteArray> names;
		QByteArray host;
		bool hasContentLength = false;
		qulonglong contentLength = 0;
		bool invalidHeaders = false;
		qsizetype totalHeaderBytes = 0;
		for (QByteArray &line : lines) {
			totalHeaderBytes += line.size() + 2;
			if (line.isEmpty() || line.size() > kGoogleOAuthLoopbackMaxHeaderLineBytes ||
			    line.startsWith(' ') || line.startsWith('\t')) {
				invalidHeaders = true;
				break;
			}
			const qsizetype colon = line.indexOf(':');
			if (colon <= 0) {
				invalidHeaders = true;
				break;
			}
			QByteArray name = line.left(colon).toLower();
			QByteArray value = line.mid(colon + 1).trimmed();
			if (!isValidHeaderName(name) || !isValidHeaderValue(value) || names.contains(name)) {
				wipe(name);
				wipe(value);
				invalidHeaders = true;
				break;
			}
			names.insert(name);
			if (name == "host") {
				host = value;
			} else if (name == "transfer-encoding") {
				invalidHeaders = true;
			} else if (name == "content-length") {
				hasContentLength = true;
				bool converted = false;
				if (!isDecimal(value)) {
					invalidHeaders = true;
				} else {
					contentLength = value.toULongLong(&converted, 10);
					invalidHeaders = !converted;
				}
			}
			wipe(name);
			wipe(value);
			if (invalidHeaders) {
				break;
			}
		}

		const QByteArray expectedHost = QByteArray(kGoogleOAuthLoopbackHost) + ':' + QByteArray::number(port_);
		if (totalHeaderBytes > kGoogleOAuthLoopbackMaxHeaderBytes || invalidHeaders || host != expectedHost ||
		    (hasContentLength && contentLength != 0)) {
			wipe(host);
			wipe(requestTarget);
			wipe(lines);
			return false;
		}
		wipe(host);
		wipe(lines);

		QByteArray absoluteUrl("http://");
		absoluteUrl.append(kGoogleOAuthLoopbackHost);
		absoluteUrl.append(':');
		absoluteUrl.append(QByteArray::number(port_));
		absoluteUrl.append(requestTarget);
		wipe(requestTarget);
		if (absoluteUrl.size() > kGoogleOAuthMaxCallbackUrlBytes) {
			wipe(absoluteUrl);
			return false;
		}

		callbackUrl = QUrl::fromEncoded(absoluteUrl, QUrl::StrictMode);
		wipe(absoluteUrl);
		return callbackUrl.isValid() && !callbackUrl.isRelative();
	}

	void completeCallback(QTcpSocket *socket, GoogleOAuthCallbackResult callback)
	{
		state_ = GoogleOAuthLoopbackState::Completed;
		authorizationTimer_.stop();
		server_.close();
		port_ = 0;
		expectedState_.clear();
		expectedRedirectUri_.clear();

		GoogleOAuthLoopbackCompletion completion;
		completion.status = GoogleOAuthLoopbackCompletionStatus::Callback;
		completion.attempt = attempt_;
		completion.callback = std::move(callback);
		pendingCompletion_.emplace(std::move(completion));

		closeAllClientsExcept(socket);
		auto iterator = clients_.find(socket);
		if (iterator == clients_.end()) {
			finishPendingCompletion();
			return;
		}
		iterator->second->responding = true;
		iterator->second->readTimer->stop();
		terminalSocket_ = socket;
		responseTimer_.start(static_cast<int>(kResponseFlushTimeout.count()));

		const QByteArray &response = pendingCompletion_->callback.status == GoogleOAuthCallbackStatus::Success
						     ? acceptedResponse()
						     : declinedResponse();
		socket->write(response);
		socket->disconnectFromHost();
	}

	void rejectClient(QTcpSocket *socket)
	{
		auto iterator = clients_.find(socket);
		if (iterator == clients_.end()) {
			return;
		}
		wipe(iterator->second->request);
		iterator->second->responding = true;
		iterator->second->readTimer->stop();
		socket->write(rejectedResponse());
		socket->disconnectFromHost();
	}

	void clientReadTimedOut(QTcpSocket *socket)
	{
		auto iterator = clients_.find(socket);
		if (iterator == clients_.end() || iterator->second->responding) {
			return;
		}
		wipe(iterator->second->request);
		iterator->second->responding = true;
		socket->write(timeoutResponse());
		socket->disconnectFromHost();
	}

	void clientDisconnected(QTcpSocket *socket)
	{
		const bool terminal = socket == terminalSocket_;
		if (terminal) {
			terminalSocket_ = nullptr;
			responseTimer_.stop();
		}
		removeClient(socket, true);
		if (terminal) {
			queuePendingCompletion();
		}
	}

	void replyAndDiscard(QTcpSocket *socket, const QByteArray &response)
	{
		socket->setReadBufferSize(1);
		QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
		socket->write(response);
		socket->disconnectFromHost();
	}

	void removeClient(QTcpSocket *socket, bool deferredDelete) noexcept
	{
		auto iterator = clients_.find(socket);
		if (iterator == clients_.end()) {
			return;
		}
		wipe(iterator->second->request);
		iterator->second->readTimer->stop();
		QObject::disconnect(socket, nullptr, owner_, nullptr);
		clients_.erase(iterator);
		if (deferredDelete) {
			socket->deleteLater();
		}
	}

	void closeAllClientsExcept(QTcpSocket *exception) noexcept
	{
		for (auto iterator = clients_.begin(); iterator != clients_.end();) {
			if (iterator->first == exception) {
				++iterator;
				continue;
			}
			QTcpSocket *socket = iterator->first;
			wipe(iterator->second->request);
			iterator->second->readTimer->stop();
			QObject::disconnect(socket, nullptr, owner_, nullptr);
			socket->abort();
			iterator = clients_.erase(iterator);
			socket->deleteLater();
		}
	}

	void closeAllClients() noexcept { closeAllClientsExcept(nullptr); }

	void authorizationTimedOut()
	{
		if (state_ != GoogleOAuthLoopbackState::Armed) {
			return;
		}
		completeWithoutResponse(GoogleOAuthLoopbackState::TimedOut,
					GoogleOAuthLoopbackCompletionStatus::AuthorizationTimedOut);
	}

	void requestLimitReached()
	{
		if (state_ == GoogleOAuthLoopbackState::Armed) {
			completeWithoutResponse(GoogleOAuthLoopbackState::RequestLimitReached,
						GoogleOAuthLoopbackCompletionStatus::RequestLimitReached);
			return;
		}
		state_ = GoogleOAuthLoopbackState::RequestLimitReached;
		shutdownActiveAttempt();
	}

	void listenerFailed()
	{
		if (state_ == GoogleOAuthLoopbackState::Armed) {
			completeWithoutResponse(GoogleOAuthLoopbackState::NetworkFailed,
						GoogleOAuthLoopbackCompletionStatus::ListenerFailure);
			return;
		}
		if (state_ == GoogleOAuthLoopbackState::Listening) {
			state_ = GoogleOAuthLoopbackState::NetworkFailed;
			shutdownActiveAttempt();
		}
	}

	void completeWithoutResponse(GoogleOAuthLoopbackState terminalState,
				     GoogleOAuthLoopbackCompletionStatus completionStatus)
	{
		state_ = terminalState;
		authorizationTimer_.stop();
		responseTimer_.stop();
		server_.close();
		port_ = 0;
		closeAllClients();
		expectedState_.clear();
		expectedRedirectUri_.clear();
		pendingCompletion_.reset();
		terminalSocket_ = nullptr;

		GoogleOAuthLoopbackCompletion completion;
		completion.status = completionStatus;
		completion.attempt = attempt_;
		pendingCompletion_.emplace(std::move(completion));
		queuePendingCompletion();
	}

	void responseFlushTimedOut()
	{
		QTcpSocket *socket = terminalSocket_;
		terminalSocket_ = nullptr;
		if (socket != nullptr) {
			QObject::disconnect(socket, nullptr, owner_, nullptr);
			socket->abort();
			removeClient(socket, true);
		}
		finishPendingCompletion();
	}

	void finishPendingCompletion() { queuePendingCompletion(); }

	void queuePendingCompletion()
	{
		if (!pendingCompletion_.has_value() || completionDeliveryQueued_) {
			return;
		}
		completionDeliveryQueued_ = true;
		const std::uint64_t epoch = completionEpoch_;
		QMetaObject::invokeMethod(
			owner_,
			[this, epoch]() {
				if (epoch != completionEpoch_) {
					return;
				}
				completionDeliveryQueued_ = false;
				deliverPendingCompletion();
			},
			Qt::QueuedConnection);
	}

	void advanceCompletionEpoch() noexcept
	{
		++completionEpoch_;
		if (completionEpoch_ == 0) {
			++completionEpoch_;
		}
		completionDeliveryQueued_ = false;
	}

	void deliverPendingCompletion()
	{
		if (!pendingCompletion_.has_value()) {
			return;
		}
		GoogleOAuthLoopbackCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		GoogleOAuthLoopbackListener::CompletionHandler handler = std::move(completionHandler_);
		completionHandler_ = {};
		if (!handler) {
			return;
		}
		try {
			handler(std::move(completion));
		} catch (...) {
			// Never let an integration callback unwind through Qt's event loop.
		}
	}

	GoogleOAuthLoopbackListener *owner_ = nullptr;
	GoogleOAuthLoopbackOptions options_;
	QTcpServer server_;
	QTimer authorizationTimer_;
	QTimer responseTimer_;
	std::unordered_map<QTcpSocket *, std::unique_ptr<Client>> clients_;
	GoogleOAuthLoopbackState state_ = GoogleOAuthLoopbackState::Idle;
	GoogleOAuthLoopbackAttempt attempt_;
	quint16 port_ = 0;
	std::size_t totalConnections_ = 0;
	bool winsockStarted_ = false;
	QUrl expectedRedirectUri_;
	SecureBuffer expectedState_;
	GoogleOAuthLoopbackListener::CompletionHandler completionHandler_;
	std::optional<GoogleOAuthLoopbackCompletion> pendingCompletion_;
	std::uint64_t completionEpoch_ = 1;
	bool completionDeliveryQueued_ = false;
	QTcpSocket *terminalSocket_ = nullptr;
};

GoogleOAuthLoopbackListener::GoogleOAuthLoopbackListener(GoogleOAuthLoopbackOptions options, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, options))
{
}

GoogleOAuthLoopbackListener::~GoogleOAuthLoopbackListener() = default;

GoogleOAuthLoopbackBindResult GoogleOAuthLoopbackListener::bind(GoogleOAuthLoopbackAttempt attempt) noexcept
{
	return impl_->bind(attempt);
}

GoogleOAuthLoopbackArmStatus GoogleOAuthLoopbackListener::arm(const QUrl &expectedRedirectUri,
							      SecureBuffer expectedState,
							      CompletionHandler completionHandler) noexcept
{
	return impl_->arm(expectedRedirectUri, std::move(expectedState), std::move(completionHandler));
}

void GoogleOAuthLoopbackListener::cancel() noexcept
{
	impl_->cancel();
}

GoogleOAuthLoopbackState GoogleOAuthLoopbackListener::state() const noexcept
{
	return impl_->state();
}

quint16 GoogleOAuthLoopbackListener::port() const noexcept
{
	return impl_->port();
}

GoogleOAuthLoopbackAttempt GoogleOAuthLoopbackListener::attempt() const noexcept
{
	return impl_->attempt();
}

} // namespace easy_multistream

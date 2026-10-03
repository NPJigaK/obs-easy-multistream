// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-token-transport.hpp"

#include "google-oauth-protocol.hpp"

#include <QAuthenticator>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QThread>
#include <QTimer>
#include <QVariant>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace easy_multistream {
namespace {

constexpr qsizetype kMaxRequestBodyBytes = 8 * 1024;
constexpr qsizetype kMaxResponseHeaderBytes = 16 * 1024;
constexpr qsizetype kMaxResponseHeaderCount = 64;

void wipe(QByteArray &bytes) noexcept
{
	volatile char *data = bytes.data();
	for (qsizetype index = 0; index < bytes.size(); ++index) {
		data[index] = 0;
	}
	bytes.clear();
}

bool hasWhitespaceOrControlCharacter(const QString &value) noexcept
{
	for (const QChar character : value) {
		const ushort codePoint = character.unicode();
		if (character.isSpace() || codePoint <= 0x1FU || (codePoint >= 0x7FU && codePoint <= 0x9FU)) {
			return true;
		}
	}
	return false;
}

bool isExactLoopbackRedirectUri(const QUrl &redirectUri) noexcept
{
	return redirectUri.isValid() && !redirectUri.isRelative() && redirectUri.scheme() == QStringLiteral("http") &&
	       redirectUri.host(QUrl::FullyEncoded) == QString::fromLatin1(kGoogleOAuthLoopbackHost) &&
	       redirectUri.port() >= 1 && redirectUri.port() <= std::numeric_limits<quint16>::max() &&
	       redirectUri.path(QUrl::FullyEncoded) == QString::fromLatin1(kGoogleOAuthCallbackPath) &&
	       redirectUri.userInfo().isEmpty() && !redirectUri.hasFragment() && !redirectUri.hasQuery();
}

bool isVisibleAscii(std::string_view value, std::size_t maximumBytes) noexcept
{
	if (value.empty() || value.size() > maximumBytes) {
		return false;
	}
	for (const unsigned char byte : value) {
		if (byte <= 0x20U || byte >= 0x7FU) {
			return false;
		}
	}
	return true;
}

bool isPkceVerifier(std::string_view value) noexcept
{
	if (value.size() < kGoogleOAuthMinPkceVerifierBytes || value.size() > kGoogleOAuthMaxPkceVerifierBytes) {
		return false;
	}
	for (const unsigned char byte : value) {
		const bool alphaNumeric = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
					  (byte >= '0' && byte <= '9');
		if (!alphaNumeric && byte != '-' && byte != '.' && byte != '_' && byte != '~') {
			return false;
		}
	}
	return true;
}

bool isUnreserved(unsigned char byte) noexcept
{
	return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
	       byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

void appendFormEncoded(QByteArray &target, std::string_view value)
{
	static constexpr std::array<char, 16> kHex = {'0', '1', '2', '3', '4', '5', '6', '7',
						      '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
	for (const unsigned char byte : value) {
		if (isUnreserved(byte)) {
			target.append(static_cast<char>(byte));
		} else {
			target.append('%');
			target.append(kHex[(byte >> 4U) & 0x0FU]);
			target.append(kHex[byte & 0x0FU]);
		}
	}
}

void appendFormField(QByteArray &target, QByteArrayView name, std::string_view value)
{
	if (!target.isEmpty()) {
		target.append('&');
	}
	target.append(name.data(), name.size());
	target.append('=');
	appendFormEncoded(target, value);
}

QByteArray makeExchangeBody(const GoogleOAuthTokenExchangeRequest &request)
{
	QByteArray clientId = request.clientId.toUtf8();
	QByteArray redirectUri = request.redirectUri.toEncoded(QUrl::FullyEncoded);
	QByteArray body;
	body.reserve(clientId.size() + redirectUri.size() +
		     static_cast<qsizetype>(request.authorizationCode.size() + request.codeVerifier.size()) + 160);
	appendFormField(body, "client_id",
			std::string_view(clientId.constData(), static_cast<std::size_t>(clientId.size())));
	appendFormField(body, "code", request.authorizationCode.view());
	appendFormField(body, "code_verifier", request.codeVerifier.view());
	appendFormField(body, "grant_type", "authorization_code");
	appendFormField(body, "redirect_uri",
			std::string_view(redirectUri.constData(), static_cast<std::size_t>(redirectUri.size())));
	return body;
}

QByteArray makeRefreshBody(const GoogleOAuthTokenRefreshRequest &request)
{
	QByteArray clientId = request.clientId.toUtf8();
	QByteArray body;
	body.reserve(clientId.size() + static_cast<qsizetype>(request.refreshToken.size()) + 80);
	appendFormField(body, "client_id",
			std::string_view(clientId.constData(), static_cast<std::size_t>(clientId.size())));
	appendFormField(body, "grant_type", "refresh_token");
	appendFormField(body, "refresh_token", request.refreshToken.view());
	return body;
}

QByteArray makeRevokeBody(const GoogleOAuthTokenRevokeRequest &request)
{
	QByteArray body;
	body.reserve(static_cast<qsizetype>(request.token.size()) + 16);
	appendFormField(body, "token", request.token.view());
	return body;
}

bool isValidOptions(const GoogleOAuthTokenTransportOptions &options) noexcept
{
	return options.operationTimeout >= std::chrono::milliseconds(10) &&
	       options.operationTimeout <= std::chrono::minutes(5) && options.maxResponseBytes >= 1024 &&
	       options.maxResponseBytes <= kGoogleOAuthMaxTokenResponseBytes;
}

QUrl endpointFor(GoogleOAuthTokenOperation operation)
{
	if (operation == GoogleOAuthTokenOperation::RevokeToken) {
		return QUrl(QString::fromLatin1(kGoogleOAuthRevokeEndpoint));
	}
	return QUrl(QString::fromLatin1(kGoogleOAuthTokenEndpoint));
}

QNetworkRequest makeNetworkRequest(GoogleOAuthTokenOperation operation, const GoogleOAuthTokenTransportOptions &options)
{
	QNetworkRequest request(endpointFor(operation));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
	request.setRawHeader("Accept", "application/json");
	request.setRawHeader("Accept-Encoding", "identity");
	request.setRawHeader("Cache-Control", "no-store");
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
	request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
	request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
	request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
	request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
	request.setAttribute(QNetworkRequest::UseCredentialsAttribute, false);
	request.setMaximumRedirectsAllowed(0);
	request.setTransferTimeout(options.operationTimeout);
	request.setDecompressedSafetyCheckThreshold(options.maxResponseBytes);

	QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
	ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
	ssl.setProtocol(QSsl::TlsV1_2OrLater);
	request.setSslConfiguration(ssl);
	return request;
}

bool isJsonContentType(const QByteArray &rawContentType)
{
	const QList<QByteArray> parts = rawContentType.split(';');
	if (parts.isEmpty() || parts.first().trimmed().compare("application/json", Qt::CaseInsensitive) != 0) {
		return false;
	}
	bool sawCharset = false;
	for (qsizetype index = 1; index < parts.size(); ++index) {
		const QByteArray parameter = parts.at(index).trimmed();
		if (parameter.isEmpty() || sawCharset) {
			return false;
		}
		const qsizetype separator = parameter.indexOf('=');
		if (separator <= 0) {
			return false;
		}
		const QByteArray name = parameter.first(separator).trimmed();
		QByteArray value = parameter.sliced(separator + 1).trimmed();
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
			value = value.sliced(1, value.size() - 2);
		}
		if (name.compare("charset", Qt::CaseInsensitive) != 0 ||
		    value.compare("utf-8", Qt::CaseInsensitive) != 0) {
			return false;
		}
		sawCharset = true;
	}
	return true;
}

bool canClassifyCompletedHttpResponse(QNetworkReply::NetworkError error) noexcept
{
	if (error == QNetworkReply::NoError) {
		return true;
	}
	const int code = static_cast<int>(error);
	return (code >= static_cast<int>(QNetworkReply::ContentAccessDenied) &&
		code <= static_cast<int>(QNetworkReply::UnknownContentError)) ||
	       error == QNetworkReply::ProtocolInvalidOperationError ||
	       (code >= static_cast<int>(QNetworkReply::InternalServerError) &&
		code <= static_cast<int>(QNetworkReply::UnknownServerError));
}

int hexDigit(unsigned char byte) noexcept
{
	if (byte >= '0' && byte <= '9') {
		return byte - '0';
	}
	if (byte >= 'a' && byte <= 'f') {
		return byte - 'a' + 10;
	}
	if (byte >= 'A' && byte <= 'F') {
		return byte - 'A' + 10;
	}
	return -1;
}

// Decode only enough of a JSON string key to compare known ASCII field names.
// Full JSON validity and UTF-8 validation are still delegated to QJsonDocument.
QByteArray decodeJsonKey(QByteArrayView raw)
{
	QByteArray decoded;
	decoded.reserve(raw.size());
	for (qsizetype index = 0; index < raw.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(raw.at(index));
		if (byte != '\\') {
			if (byte >= 0x80U) {
				return {};
			}
			decoded.append(static_cast<char>(byte));
			continue;
		}
		if (++index >= raw.size()) {
			return {};
		}
		const unsigned char escaped = static_cast<unsigned char>(raw.at(index));
		if (escaped == '"' || escaped == '\\' || escaped == '/') {
			decoded.append(static_cast<char>(escaped));
			continue;
		}
		if (escaped != 'u' || index + 4 >= raw.size()) {
			return {};
		}
		int value = 0;
		for (int digitIndex = 0; digitIndex < 4; ++digitIndex) {
			const int digit = hexDigit(static_cast<unsigned char>(raw.at(++index)));
			if (digit < 0) {
				return {};
			}
			value = (value << 4) | digit;
		}
		if (value > 0x7F) {
			return {};
		}
		decoded.append(static_cast<char>(value));
	}
	return decoded;
}

bool isKnownTokenField(const QByteArray &key) noexcept
{
	return key == "access_token" || key == "refresh_token" || key == "token_type" || key == "expires_in" ||
	       key == "scope" || key == "refresh_token_expires_in" || key == "error";
}

bool hasDuplicateKnownTopLevelField(const QByteArray &json)
{
	QList<QByteArray> seen;
	int depth = 0;
	bool expectingTopLevelKey = false;
	for (qsizetype index = 0; index < json.size(); ++index) {
		const unsigned char byte = static_cast<unsigned char>(json.at(index));
		if (byte == '"') {
			const qsizetype start = index + 1;
			bool escaped = false;
			for (++index; index < json.size(); ++index) {
				const char current = json.at(index);
				if (!escaped && current == '"') {
					break;
				}
				if (!escaped && current == '\\') {
					escaped = true;
				} else {
					escaped = false;
				}
			}
			if (index >= json.size()) {
				return false;
			}
			if (depth == 1 && expectingTopLevelKey) {
				const QByteArray key = decodeJsonKey(QByteArrayView(json).sliced(start, index - start));
				if (isKnownTokenField(key)) {
					if (seen.contains(key)) {
						return true;
					}
					seen.append(key);
				}
				expectingTopLevelKey = false;
			}
			continue;
		}
		if (byte == '{' || byte == '[') {
			++depth;
			if (depth == 1 && byte == '{') {
				expectingTopLevelKey = true;
			}
		} else if (byte == '}' || byte == ']') {
			--depth;
		} else if (byte == ',' && depth == 1) {
			expectingTopLevelKey = true;
		}
	}
	return false;
}

GoogleOAuthTokenProviderError classifyProviderError(const QString &error) noexcept
{
	if (error == QStringLiteral("invalid_request")) {
		return GoogleOAuthTokenProviderError::InvalidRequest;
	}
	if (error == QStringLiteral("invalid_grant")) {
		return GoogleOAuthTokenProviderError::InvalidGrant;
	}
	if (error == QStringLiteral("invalid_client")) {
		return GoogleOAuthTokenProviderError::InvalidClient;
	}
	if (error == QStringLiteral("unauthorized_client")) {
		return GoogleOAuthTokenProviderError::UnauthorizedClient;
	}
	if (error == QStringLiteral("invalid_scope")) {
		return GoogleOAuthTokenProviderError::InvalidScope;
	}
	if (error == QStringLiteral("access_denied")) {
		return GoogleOAuthTokenProviderError::AccessDenied;
	}
	if (error == QStringLiteral("invalid_token")) {
		return GoogleOAuthTokenProviderError::InvalidToken;
	}
	if (error == QStringLiteral("temporarily_unavailable") || error == QStringLiteral("server_error")) {
		return GoogleOAuthTokenProviderError::TemporarilyUnavailable;
	}
	return GoogleOAuthTokenProviderError::Unknown;
}

bool hasRequiredScope(const QString &scope) noexcept
{
	if (scope.isEmpty() || scope.size() > 4096 || scope.startsWith(QLatin1Char(' ')) ||
	    scope.endsWith(QLatin1Char(' ')) || scope.contains(QStringLiteral("  "))) {
		return false;
	}
	for (const QChar character : scope) {
		const ushort value = character.unicode();
		if (value == ' ') {
			continue;
		}
		if (value <= 0x20U || value >= 0x7FU) {
			return false;
		}
	}
	const QStringList scopes = scope.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	return scopes.contains(QString::fromLatin1(kGoogleOAuthYouTubeReadonlyScope));
}

std::optional<GoogleOAuthTokenProviderError> parseProviderError(const QByteArray &body)
{
	if (!body.isValidUtf8() || hasDuplicateKnownTopLevelField(body)) {
		return std::nullopt;
	}
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const QJsonValue errorValue = document.object().value(QStringLiteral("error"));
	if (!errorValue.isString()) {
		return std::nullopt;
	}
	const QString error = errorValue.toString();
	if (error.isEmpty() || error.size() > 128 || hasWhitespaceOrControlCharacter(error)) {
		return std::nullopt;
	}
	return classifyProviderError(error);
}

std::optional<GoogleOAuthTokenSet> parseTokenSet(const QByteArray &body, GoogleOAuthTokenOperation operation)
{
	if (!body.isValidUtf8() || hasDuplicateKnownTopLevelField(body)) {
		return std::nullopt;
	}
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	const QJsonObject object = document.object();
	const QJsonValue accessValue = object.value(QStringLiteral("access_token"));
	const QJsonValue refreshValue = object.value(QStringLiteral("refresh_token"));
	const QJsonValue typeValue = object.value(QStringLiteral("token_type"));
	const QJsonValue expiresValue = object.value(QStringLiteral("expires_in"));
	const QJsonValue scopeValue = object.value(QStringLiteral("scope"));
	if (!object.value(QStringLiteral("error")).isUndefined()) {
		return std::nullopt;
	}
	if (!accessValue.isString() || !typeValue.isString() || !expiresValue.isDouble() ||
	    typeValue.toString() != QStringLiteral("Bearer")) {
		return std::nullopt;
	}
	if (operation == GoogleOAuthTokenOperation::ExchangeAuthorizationCode && !refreshValue.isString()) {
		return std::nullopt;
	}
	if (!refreshValue.isUndefined() && !refreshValue.isString()) {
		return std::nullopt;
	}
	if (refreshValue.isString() && refreshValue.toString().isEmpty()) {
		return std::nullopt;
	}
	if (operation == GoogleOAuthTokenOperation::ExchangeAuthorizationCode && !scopeValue.isString()) {
		return std::nullopt;
	}
	if (!scopeValue.isUndefined() && !scopeValue.isString()) {
		return std::nullopt;
	}
	if (scopeValue.isString() && !hasRequiredScope(scopeValue.toString())) {
		return std::nullopt;
	}

	const double expiresDouble = expiresValue.toDouble();
	if (!std::isfinite(expiresDouble) || expiresDouble != std::floor(expiresDouble) || expiresDouble <= 0.0 ||
	    expiresDouble > static_cast<double>(kGoogleOAuthMaxExpiresInSeconds)) {
		return std::nullopt;
	}

	QByteArray accessToken = accessValue.toString().toUtf8();
	QByteArray refreshToken;
	if (refreshValue.isString()) {
		refreshToken = refreshValue.toString().toUtf8();
	}
	const bool validAccess =
		isVisibleAscii(std::string_view(accessToken.constData(), static_cast<std::size_t>(accessToken.size())),
			       kGoogleOAuthMaxAccessTokenBytes);
	const bool validRefresh = refreshToken.isEmpty() ||
				  isVisibleAscii(std::string_view(refreshToken.constData(),
								  static_cast<std::size_t>(refreshToken.size())),
						 kGoogleOAuthMaxRefreshTokenBytes);
	if (!validAccess || !validRefresh ||
	    (operation == GoogleOAuthTokenOperation::ExchangeAuthorizationCode && refreshToken.isEmpty())) {
		wipe(accessToken);
		wipe(refreshToken);
		return std::nullopt;
	}

	GoogleOAuthTokenSet tokens;
	tokens.accessToken = SecureBuffer::copyOf(
		std::string_view(accessToken.constData(), static_cast<std::size_t>(accessToken.size())));
	if (!refreshToken.isEmpty()) {
		tokens.refreshToken = SecureBuffer::copyOf(
			std::string_view(refreshToken.constData(), static_cast<std::size_t>(refreshToken.size())));
	}
	tokens.expiresInSeconds = static_cast<std::uint32_t>(expiresDouble);
	wipe(accessToken);
	wipe(refreshToken);
	return tokens;
}

} // namespace

class GoogleOAuthTokenTransport::Impl final {
public:
	Impl(GoogleOAuthTokenTransport *owner, std::unique_ptr<QNetworkAccessManager> manager,
	     GoogleOAuthTokenTransportOptions options)
		: owner_(owner),
		  manager_(std::move(manager)),
		  options_(options)
	{
		Q_ASSERT_X(manager_ == nullptr || manager_->parent() == nullptr, "GoogleOAuthTokenTransport::Impl",
			   "The injected test network manager must be unparented because ownership is transferred");
		Q_ASSERT_X(manager_ == nullptr || manager_->thread() == owner_->thread(),
			   "GoogleOAuthTokenTransport::Impl",
			   "The network manager must be created on the transport owner thread");
		if (!manager_) {
			manager_ = std::make_unique<QNetworkAccessManager>();
		}
		overallTimer_.setSingleShot(true);
		QObject::connect(&overallTimer_, &QTimer::timeout, owner_, [this]() {
			if (state_ == GoogleOAuthTokenTransportState::InFlight) {
				finishForced(GoogleOAuthTokenCompletionStatus::TimedOut);
			}
		});
		QObject::connect(manager_.get(), &QNetworkAccessManager::authenticationRequired, owner_,
				 [this](QNetworkReply *reply, QAuthenticator *) {
					 if (state_ == GoogleOAuthTokenTransportState::InFlight &&
					     reply_.data() == reply) {
						 finishForced(GoogleOAuthTokenCompletionStatus::NetworkFailure);
					 }
				 });
		QObject::connect(manager_.get(), &QNetworkAccessManager::proxyAuthenticationRequired, owner_,
				 [this](const QNetworkProxy &, QAuthenticator *) {
					 if (state_ == GoogleOAuthTokenTransportState::InFlight) {
						 finishForced(GoogleOAuthTokenCompletionStatus::NetworkFailure);
					 }
				 });
	}

	~Impl()
	{
		shutdown();
		wipe(response_);
	}

	GoogleOAuthTokenStartStatus startExchange(GoogleOAuthTokenExchangeRequest request,
						  GoogleOAuthTokenTransport::CompletionHandler handler) noexcept
	{
		if (!onOwnerThread()) {
			return GoogleOAuthTokenStartStatus::WrongThread;
		}
		if (state_ == GoogleOAuthTokenTransportState::Closed) {
			return GoogleOAuthTokenStartStatus::Closed;
		}
		if (!canStart()) {
			return GoogleOAuthTokenStartStatus::Busy;
		}
		if (!isValidOptions(options_)) {
			return GoogleOAuthTokenStartStatus::InvalidOptions;
		}
		if (!request.attempt.isValid()) {
			return GoogleOAuthTokenStartStatus::InvalidAttempt;
		}
		if (!isValidGoogleOAuthClientId(request.clientId)) {
			return GoogleOAuthTokenStartStatus::InvalidClientId;
		}
		if (!isExactLoopbackRedirectUri(request.redirectUri)) {
			return GoogleOAuthTokenStartStatus::InvalidRedirectUri;
		}
		if (!isVisibleAscii(request.authorizationCode.view(), kGoogleOAuthMaxAuthorizationCodeBytes)) {
			return GoogleOAuthTokenStartStatus::InvalidAuthorizationCode;
		}
		if (!isPkceVerifier(request.codeVerifier.view())) {
			return GoogleOAuthTokenStartStatus::InvalidCodeVerifier;
		}
		if (!handler) {
			return GoogleOAuthTokenStartStatus::InvalidCompletionHandler;
		}
		QByteArray body = makeExchangeBody(request);
		request.authorizationCode.clear();
		request.codeVerifier.clear();
		return begin(request.attempt, GoogleOAuthTokenOperation::ExchangeAuthorizationCode, std::move(body),
			     std::move(handler));
	}

	GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
						 GoogleOAuthTokenTransport::CompletionHandler handler) noexcept
	{
		if (!onOwnerThread()) {
			return GoogleOAuthTokenStartStatus::WrongThread;
		}
		if (state_ == GoogleOAuthTokenTransportState::Closed) {
			return GoogleOAuthTokenStartStatus::Closed;
		}
		if (!canStart()) {
			return GoogleOAuthTokenStartStatus::Busy;
		}
		if (!isValidOptions(options_)) {
			return GoogleOAuthTokenStartStatus::InvalidOptions;
		}
		if (!request.attempt.isValid()) {
			return GoogleOAuthTokenStartStatus::InvalidAttempt;
		}
		if (!isValidGoogleOAuthClientId(request.clientId)) {
			return GoogleOAuthTokenStartStatus::InvalidClientId;
		}
		if (!isVisibleAscii(request.refreshToken.view(), kGoogleOAuthMaxRefreshTokenBytes)) {
			return GoogleOAuthTokenStartStatus::InvalidRefreshToken;
		}
		if (!handler) {
			return GoogleOAuthTokenStartStatus::InvalidCompletionHandler;
		}
		QByteArray body = makeRefreshBody(request);
		request.refreshToken.clear();
		return begin(request.attempt, GoogleOAuthTokenOperation::RefreshAccessToken, std::move(body),
			     std::move(handler));
	}

	GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
						GoogleOAuthTokenTransport::CompletionHandler handler) noexcept
	{
		if (!onOwnerThread()) {
			return GoogleOAuthTokenStartStatus::WrongThread;
		}
		if (state_ == GoogleOAuthTokenTransportState::Closed) {
			return GoogleOAuthTokenStartStatus::Closed;
		}
		if (!canStart()) {
			return GoogleOAuthTokenStartStatus::Busy;
		}
		if (!isValidOptions(options_)) {
			return GoogleOAuthTokenStartStatus::InvalidOptions;
		}
		if (!request.attempt.isValid()) {
			return GoogleOAuthTokenStartStatus::InvalidAttempt;
		}
		if (!isVisibleAscii(request.token.view(), kGoogleOAuthMaxAccessTokenBytes)) {
			return GoogleOAuthTokenStartStatus::InvalidToken;
		}
		if (!handler) {
			return GoogleOAuthTokenStartStatus::InvalidCompletionHandler;
		}
		QByteArray body = makeRevokeBody(request);
		request.token.clear();
		return begin(request.attempt, GoogleOAuthTokenOperation::RevokeToken, std::move(body),
			     std::move(handler));
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept
	{
		if (!onOwnerThread() || !attempt.isValid()) {
			return false;
		}
		const bool matchesActive = activeAttempt_.has_value() && *activeAttempt_ == attempt;
		const bool matchesPending = pendingCompletion_.has_value() && pendingCompletion_->attempt == attempt;
		if (!matchesActive && !matchesPending) {
			return false;
		}
		shutdownOperation(GoogleOAuthTokenTransportState::Cancelled);
		return true;
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		shutdownOperation(GoogleOAuthTokenTransportState::Closed);
		return true;
	}

	GoogleOAuthTokenTransportState state() const noexcept { return state_; }

	std::optional<GoogleOAuthTokenAttempt> activeAttempt() const noexcept { return activeAttempt_; }

	std::optional<GoogleOAuthTokenOperation> activeOperation() const noexcept { return activeOperation_; }

private:
	bool onOwnerThread() const noexcept
	{
		return owner_ != nullptr && owner_->thread() == QThread::currentThread() && manager_ != nullptr &&
		       manager_->thread() == QThread::currentThread() &&
		       overallTimer_.thread() == QThread::currentThread();
	}

	bool canStart() const noexcept
	{
		return state_ != GoogleOAuthTokenTransportState::InFlight && reply_.isNull() &&
		       !pendingCompletion_.has_value() && !completionDeliveryQueued_;
	}

	GoogleOAuthTokenStartStatus begin(GoogleOAuthTokenAttempt attempt, GoogleOAuthTokenOperation operation,
					  QByteArray body,
					  GoogleOAuthTokenTransport::CompletionHandler handler) noexcept
	{
		if (body.isEmpty() || body.size() > kMaxRequestBodyBytes) {
			wipe(body);
			return GoogleOAuthTokenStartStatus::RequestCreationFailed;
		}

		advanceCompletionEpoch();
		activeAttempt_ = attempt;
		activeOperation_ = operation;
		completionHandler_ = std::move(handler);
		state_ = GoogleOAuthTokenTransportState::InFlight;
		endpoint_ = endpointFor(operation);
		wipe(response_);
		sawTlsError_ = false;
		declaredContentLength_.reset();

		QNetworkRequest networkRequest = makeNetworkRequest(operation, options_);
		QNetworkReply *reply = manager_->post(networkRequest, body);
		wipe(body);
		if (reply == nullptr) {
			resetActiveState(GoogleOAuthTokenTransportState::Idle);
			return GoogleOAuthTokenStartStatus::RequestCreationFailed;
		}

		reply_ = reply;
		reply->setReadBufferSize(options_.maxResponseBytes + 1);
		const std::uint64_t epoch = completionEpoch_;
		QObject::connect(reply, &QNetworkReply::readyRead, owner_,
				 [this, epoch, reply]() { consumeReply(epoch, reply); });
		QObject::connect(reply, &QNetworkReply::metaDataChanged, owner_,
				 [this, epoch, reply]() { validateResponseMetadata(epoch, reply); });
		QObject::connect(reply, &QNetworkReply::redirected, owner_, [this, epoch, reply](const QUrl &) {
			if (isCurrent(epoch, reply)) {
				finishForced(GoogleOAuthTokenCompletionStatus::RedirectRejected);
			}
		});
		QObject::connect(reply, &QNetworkReply::sslErrors, owner_,
				 [this, epoch, reply](const QList<QSslError> &) {
					 if (isCurrent(epoch, reply)) {
						 sawTlsError_ = true;
						 finishForced(GoogleOAuthTokenCompletionStatus::TlsFailure);
					 }
				 });
		QObject::connect(reply, &QNetworkReply::downloadProgress, owner_,
				 [this, epoch, reply](qint64 received, qint64) {
					 if (isCurrent(epoch, reply) && received > options_.maxResponseBytes) {
						 finishForced(GoogleOAuthTokenCompletionStatus::ResponseTooLarge);
					 }
				 });
		QObject::connect(reply, &QNetworkReply::finished, owner_, [this, epoch, reply]() {
			if (isCurrent(epoch, reply)) {
				finishFromReply(reply);
			}
		});
		QObject::connect(reply, &QObject::destroyed, owner_, [this, epoch]() {
			if (completionEpoch_ == epoch && state_ == GoogleOAuthTokenTransportState::InFlight) {
				reply_ = nullptr;
				finishWithoutReply(GoogleOAuthTokenCompletionStatus::NetworkFailure);
			}
		});
		overallTimer_.start(options_.operationTimeout);
		return GoogleOAuthTokenStartStatus::Started;
	}

	bool isCurrent(std::uint64_t epoch, QNetworkReply *reply) const noexcept
	{
		return epoch == completionEpoch_ && state_ == GoogleOAuthTokenTransportState::InFlight &&
		       reply_.data() == reply;
	}

	void consumeReply(std::uint64_t epoch, QNetworkReply *reply)
	{
		if (!isCurrent(epoch, reply)) {
			return;
		}
		while (reply->bytesAvailable() > 0) {
			const qsizetype remaining = options_.maxResponseBytes - response_.size();
			if (remaining < 0) {
				finishForced(GoogleOAuthTokenCompletionStatus::ResponseTooLarge);
				return;
			}
			QByteArray chunk = reply->read(std::min<qint64>(reply->bytesAvailable(), remaining + 1));
			if (chunk.isEmpty()) {
				break;
			}
			response_.append(chunk);
			wipe(chunk);
			if (response_.size() > options_.maxResponseBytes) {
				finishForced(GoogleOAuthTokenCompletionStatus::ResponseTooLarge);
				return;
			}
		}
	}

	void validateResponseMetadata(std::uint64_t epoch, QNetworkReply *reply)
	{
		if (!isCurrent(epoch, reply)) {
			return;
		}
		qsizetype totalHeaderBytes = 0;
		qsizetype headerCount = 0;
		int contentLengthCount = 0;
		int contentTypeCount = 0;
		int contentEncodingCount = 0;
		for (const auto &header : reply->rawHeaderPairs()) {
			++headerCount;
			totalHeaderBytes += header.first.size() + header.second.size();
			if (header.first.compare("Content-Length", Qt::CaseInsensitive) == 0) {
				++contentLengthCount;
			} else if (header.first.compare("Content-Type", Qt::CaseInsensitive) == 0) {
				++contentTypeCount;
			} else if (header.first.compare("Content-Encoding", Qt::CaseInsensitive) == 0) {
				++contentEncodingCount;
			}
		}
		if (headerCount > kMaxResponseHeaderCount || totalHeaderBytes > kMaxResponseHeaderBytes ||
		    contentLengthCount > 1 || contentTypeCount > 1 || contentEncodingCount > 1) {
			finishForced(GoogleOAuthTokenCompletionStatus::InvalidResponse);
			return;
		}

		if (contentLengthCount == 1) {
			bool ok = false;
			const qlonglong contentLength = reply->rawHeader("Content-Length").trimmed().toLongLong(&ok);
			if (!ok || contentLength < 0) {
				finishForced(GoogleOAuthTokenCompletionStatus::InvalidResponse);
				return;
			}
			if (contentLength > options_.maxResponseBytes) {
				finishForced(GoogleOAuthTokenCompletionStatus::ResponseTooLarge);
				return;
			}
			declaredContentLength_ = contentLength;
		}
		if (contentEncodingCount == 1 &&
		    reply->rawHeader("Content-Encoding").trimmed().compare("identity", Qt::CaseInsensitive) != 0) {
			finishForced(GoogleOAuthTokenCompletionStatus::InvalidResponse);
			return;
		}
		if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
			finishForced(GoogleOAuthTokenCompletionStatus::RedirectRejected);
		}
	}

	void finishFromReply(QNetworkReply *reply)
	{
		consumeReply(completionEpoch_, reply);
		if (!isCurrent(completionEpoch_, reply)) {
			return;
		}
		validateResponseMetadata(completionEpoch_, reply);
		if (!isCurrent(completionEpoch_, reply)) {
			return;
		}

		const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
		const int httpStatus = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
		const QNetworkReply::NetworkError networkError = reply->error();
		if (sawTlsError_ || networkError == QNetworkReply::SslHandshakeFailedError) {
			finishForced(GoogleOAuthTokenCompletionStatus::TlsFailure, httpStatus);
			return;
		}
		if (networkError == QNetworkReply::TimeoutError) {
			finishForced(GoogleOAuthTokenCompletionStatus::TimedOut, httpStatus);
			return;
		}
		if (httpStatus == 0 && networkError != QNetworkReply::NoError) {
			finishForced(GoogleOAuthTokenCompletionStatus::NetworkFailure);
			return;
		}
		if (!reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool()) {
			finishForced(GoogleOAuthTokenCompletionStatus::TlsFailure, httpStatus);
			return;
		}
		if (reply->url() != endpoint_ || (httpStatus >= 300 && httpStatus < 400) ||
		    reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
			finishForced(GoogleOAuthTokenCompletionStatus::RedirectRejected, httpStatus);
			return;
		}
		if (declaredContentLength_.has_value() && *declaredContentLength_ != response_.size()) {
			finishForced(GoogleOAuthTokenCompletionStatus::InvalidResponse, httpStatus);
			return;
		}
		// Preserve bounded, completed HTTP error replies for provider-error
		// classification. A socket/proxy/protocol failure can carry a status but
		// still leave a truncated body, so it must never be treated as a complete
		// Google response.
		if ((httpStatus < 400 || !canClassifyCompletedHttpResponse(networkError)) &&
		    networkError != QNetworkReply::NoError) {
			finishForced(GoogleOAuthTokenCompletionStatus::NetworkFailure, httpStatus);
			return;
		}
		GoogleOAuthTokenCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.operation = *activeOperation_;
		completion.httpStatus = httpStatus;

		if (*activeOperation_ == GoogleOAuthTokenOperation::RevokeToken && httpStatus == 200) {
			completion.status = GoogleOAuthTokenCompletionStatus::Success;
			finish(std::move(completion));
			return;
		}

		if (httpStatus == 429) {
			completion.status = GoogleOAuthTokenCompletionStatus::ProviderRejected;
			completion.providerError = GoogleOAuthTokenProviderError::RateLimited;
			finish(std::move(completion));
			return;
		}
		if (httpStatus >= 500 && httpStatus <= 599) {
			completion.status = GoogleOAuthTokenCompletionStatus::ProviderRejected;
			completion.providerError = GoogleOAuthTokenProviderError::TemporarilyUnavailable;
			finish(std::move(completion));
			return;
		}

		const bool jsonContentType = isJsonContentType(reply->rawHeader("Content-Type"));
		if (httpStatus == 200 && *activeOperation_ != GoogleOAuthTokenOperation::RevokeToken) {
			if (!jsonContentType) {
				completion.status = GoogleOAuthTokenCompletionStatus::InvalidResponse;
				finish(std::move(completion));
				return;
			}
			auto tokens = parseTokenSet(response_, *activeOperation_);
			if (!tokens.has_value()) {
				completion.status = GoogleOAuthTokenCompletionStatus::InvalidResponse;
				finish(std::move(completion));
				return;
			}
			completion.status = GoogleOAuthTokenCompletionStatus::Success;
			completion.tokens = std::move(*tokens);
			finish(std::move(completion));
			return;
		}

		if (jsonContentType) {
			const auto providerError = parseProviderError(response_);
			if (providerError.has_value()) {
				completion.status = GoogleOAuthTokenCompletionStatus::ProviderRejected;
				completion.providerError = *providerError;
				finish(std::move(completion));
				return;
			}
		}
		completion.status = httpStatus > 0 ? GoogleOAuthTokenCompletionStatus::HttpFailure
						   : GoogleOAuthTokenCompletionStatus::NetworkFailure;
		finish(std::move(completion));
	}

	void finishForced(GoogleOAuthTokenCompletionStatus status, int httpStatus = 0)
	{
		if (!activeAttempt_.has_value() || !activeOperation_.has_value()) {
			return;
		}
		GoogleOAuthTokenCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.operation = *activeOperation_;
		completion.status = status;
		completion.httpStatus = httpStatus;
		finish(std::move(completion));
	}

	void finishWithoutReply(GoogleOAuthTokenCompletionStatus status)
	{
		if (!activeAttempt_.has_value() || !activeOperation_.has_value()) {
			return;
		}
		GoogleOAuthTokenCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.operation = *activeOperation_;
		completion.status = status;
		finish(std::move(completion));
	}

	void finish(GoogleOAuthTokenCompletion completion)
	{
		if (state_ != GoogleOAuthTokenTransportState::InFlight) {
			return;
		}
		overallTimer_.stop();
		QPointer<QNetworkReply> reply = reply_;
		reply_ = nullptr;
		if (reply != nullptr) {
			QObject::disconnect(reply, nullptr, owner_, nullptr);
			if (reply->isRunning()) {
				reply->abort();
			}
			if (reply != nullptr) {
				reply->deleteLater();
			}
		}
		wipe(response_);
		endpoint_.clear();
		declaredContentLength_.reset();
		activeAttempt_.reset();
		activeOperation_.reset();
		state_ = GoogleOAuthTokenTransportState::Completed;
		pendingCompletion_.emplace(std::move(completion));
		queuePendingCompletion();
	}

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

	void deliverPendingCompletion()
	{
		if (!pendingCompletion_.has_value()) {
			return;
		}
		auto handler = std::move(completionHandler_);
		GoogleOAuthTokenCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		completionHandler_ = {};
		if (handler) {
			try {
				handler(std::move(completion));
			} catch (...) {
				// Never let an integration callback unwind through Qt's event loop.
			}
		}
	}

	void shutdownOperation(GoogleOAuthTokenTransportState terminalState) noexcept
	{
		advanceCompletionEpoch();
		overallTimer_.stop();
		QPointer<QNetworkReply> reply = reply_;
		reply_ = nullptr;
		if (reply != nullptr) {
			QObject::disconnect(reply, nullptr, owner_, nullptr);
			if (reply->isRunning()) {
				reply->abort();
			}
			if (reply != nullptr) {
				reply->deleteLater();
			}
		}
		wipe(response_);
		endpoint_.clear();
		declaredContentLength_.reset();
		activeAttempt_.reset();
		activeOperation_.reset();
		completionHandler_ = {};
		pendingCompletion_.reset();
		state_ = terminalState;
	}

	void resetActiveState(GoogleOAuthTokenTransportState state) noexcept
	{
		wipe(response_);
		endpoint_.clear();
		declaredContentLength_.reset();
		activeAttempt_.reset();
		activeOperation_.reset();
		completionHandler_ = {};
		state_ = state;
	}

	void advanceCompletionEpoch() noexcept
	{
		++completionEpoch_;
		if (completionEpoch_ == 0) {
			++completionEpoch_;
		}
		completionDeliveryQueued_ = false;
	}

	GoogleOAuthTokenTransport *owner_ = nullptr;
	std::unique_ptr<QNetworkAccessManager> manager_;
	GoogleOAuthTokenTransportOptions options_;
	QTimer overallTimer_;
	QPointer<QNetworkReply> reply_;
	GoogleOAuthTokenTransportState state_ = GoogleOAuthTokenTransportState::Idle;
	std::optional<GoogleOAuthTokenAttempt> activeAttempt_;
	std::optional<GoogleOAuthTokenOperation> activeOperation_;
	QUrl endpoint_;
	QByteArray response_;
	std::optional<qint64> declaredContentLength_;
	bool sawTlsError_ = false;
	GoogleOAuthTokenTransport::CompletionHandler completionHandler_;
	std::optional<GoogleOAuthTokenCompletion> pendingCompletion_;
	std::uint64_t completionEpoch_ = 1;
	bool completionDeliveryQueued_ = false;
};

GoogleOAuthTokenTransport::GoogleOAuthTokenTransport(GoogleOAuthTokenTransportOptions options, QObject *parent)
	: GoogleOAuthTokenTransport(std::make_unique<QNetworkAccessManager>(), options, parent)
{
}

GoogleOAuthTokenTransport::GoogleOAuthTokenTransport(std::unique_ptr<QNetworkAccessManager> networkManager,
						     GoogleOAuthTokenTransportOptions options, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(networkManager), options))
{
}

GoogleOAuthTokenTransport::~GoogleOAuthTokenTransport()
{
	Q_ASSERT_X(thread() == QThread::currentThread(), "GoogleOAuthTokenTransport::~GoogleOAuthTokenTransport",
		   "Destroy the transport on its owner thread after shutdown");
	if (thread() == QThread::currentThread() && impl_) {
		impl_->shutdown();
	}
}

GoogleOAuthTokenStartStatus GoogleOAuthTokenTransport::startExchange(GoogleOAuthTokenExchangeRequest request,
								     CompletionHandler completionHandler) noexcept
{
	return impl_->startExchange(std::move(request), std::move(completionHandler));
}

GoogleOAuthTokenStartStatus GoogleOAuthTokenTransport::startRefresh(GoogleOAuthTokenRefreshRequest request,
								    CompletionHandler completionHandler) noexcept
{
	return impl_->startRefresh(std::move(request), std::move(completionHandler));
}

GoogleOAuthTokenStartStatus GoogleOAuthTokenTransport::startRevoke(GoogleOAuthTokenRevokeRequest request,
								   CompletionHandler completionHandler) noexcept
{
	return impl_->startRevoke(std::move(request), std::move(completionHandler));
}

bool GoogleOAuthTokenTransport::cancel(GoogleOAuthTokenAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool GoogleOAuthTokenTransport::shutdown() noexcept
{
	return impl_->shutdown();
}

GoogleOAuthTokenTransportState GoogleOAuthTokenTransport::state() const noexcept
{
	return impl_->state();
}

std::optional<GoogleOAuthTokenAttempt> GoogleOAuthTokenTransport::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

std::optional<GoogleOAuthTokenOperation> GoogleOAuthTokenTransport::activeOperation() const noexcept
{
	return impl_->activeOperation();
}

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-api-discovery.hpp"

#include <QAuthenticator>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace easy_multistream {
namespace {

constexpr qsizetype kMaxResponseHeaderBytes = 16 * 1024;
constexpr qsizetype kMaxResponseHeaderCount = 64;
// Google documents page tokens as opaque strings without a size guarantee.
// This is a deliberately generous local defense bound, not a protocol limit.
constexpr qsizetype kMaxPageTokenBytes = 2048;
constexpr qsizetype kMaxItemsPerPage = 50;
constexpr std::size_t kMaxJsonNestingDepth = 64;
constexpr auto kChannelsFields = "nextPageToken,items(id,snippet(title))";
constexpr auto kStreamsFields = "nextPageToken,items(id,snippet(channelId,title))";

void wipe(QByteArray &bytes) noexcept
{
	volatile char *data = bytes.data();
	for (qsizetype index = 0; index < bytes.size(); ++index) {
		data[index] = 0;
	}
	bytes.clear();
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

bool isValidIdentifier(std::string_view value) noexcept
{
	return isVisibleAscii(value, kYouTubeApiMaxIdentifierBytes);
}

bool isValidOptions(const YouTubeApiDiscoveryOptions &options) noexcept
{
	return options.operationTimeout >= std::chrono::milliseconds(10) &&
	       options.operationTimeout <= std::chrono::minutes(5) && options.maxResponseBytes >= 1024 &&
	       options.maxResponseBytes <= kYouTubeApiMaxResponseBytes;
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

// Decode enough of a JSON object key to compare ASCII field names, including
// their \u00XX escaped forms. Full syntax and UTF-8 validation are handled by
// QJsonDocument after this duplicate-key pass.
QByteArray decodeAsciiJsonKey(QByteArrayView raw)
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

class JsonDuplicateKeyScanner final {
public:
	explicit JsonDuplicateKeyScanner(QByteArrayView input) : input_(input) {}

	bool isValidAndUnique()
	{
		skipWhitespace();
		if (!parseValue(0)) {
			return false;
		}
		skipWhitespace();
		return position_ == input_.size();
	}

private:
	void skipWhitespace() noexcept
	{
		while (position_ < input_.size()) {
			const char value = input_.at(position_);
			if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
				break;
			}
			++position_;
		}
	}

	bool consume(char value) noexcept
	{
		if (position_ >= input_.size() || input_.at(position_) != value) {
			return false;
		}
		++position_;
		return true;
	}

	bool parseString(QByteArray *decodedKey = nullptr)
	{
		if (!consume('"')) {
			return false;
		}
		const qsizetype start = position_;
		bool escaped = false;
		while (position_ < input_.size()) {
			const unsigned char current = static_cast<unsigned char>(input_.at(position_));
			if (!escaped && current == '"') {
				if (decodedKey != nullptr) {
					*decodedKey = decodeAsciiJsonKey(input_.sliced(start, position_ - start));
				}
				++position_;
				return true;
			}
			if (!escaped && current < 0x20U) {
				return false;
			}
			if (!escaped && current == '\\') {
				escaped = true;
			} else {
				escaped = false;
			}
			++position_;
		}
		return false;
	}

	bool parseObject(std::size_t depth)
	{
		if (!consume('{')) {
			return false;
		}
		skipWhitespace();
		if (consume('}')) {
			return true;
		}

		QSet<QByteArray> keys;
		while (position_ < input_.size()) {
			QByteArray key;
			if (!parseString(&key)) {
				return false;
			}
			if (!key.isEmpty()) {
				if (keys.contains(key)) {
					return false;
				}
				keys.insert(key);
			}
			skipWhitespace();
			if (!consume(':')) {
				return false;
			}
			skipWhitespace();
			if (!parseValue(depth)) {
				return false;
			}
			skipWhitespace();
			if (consume('}')) {
				return true;
			}
			if (!consume(',')) {
				return false;
			}
			skipWhitespace();
		}
		return false;
	}

	bool parseArray(std::size_t depth)
	{
		if (!consume('[')) {
			return false;
		}
		skipWhitespace();
		if (consume(']')) {
			return true;
		}
		while (position_ < input_.size()) {
			if (!parseValue(depth)) {
				return false;
			}
			skipWhitespace();
			if (consume(']')) {
				return true;
			}
			if (!consume(',')) {
				return false;
			}
			skipWhitespace();
		}
		return false;
	}

	bool parsePrimitive()
	{
		const qsizetype start = position_;
		while (position_ < input_.size()) {
			const char value = input_.at(position_);
			if (value == ',' || value == ']' || value == '}' || value == ' ' || value == '\t' ||
			    value == '\r' || value == '\n') {
				break;
			}
			if (value == '"' || value == '{' || value == '[' || value == ':') {
				return false;
			}
			++position_;
		}
		return position_ > start;
	}

	bool parseValue(std::size_t depth)
	{
		if (position_ >= input_.size()) {
			return false;
		}
		if (input_.at(position_) == '{') {
			return depth < kMaxJsonNestingDepth && parseObject(depth + 1);
		}
		if (input_.at(position_) == '[') {
			return depth < kMaxJsonNestingDepth && parseArray(depth + 1);
		}
		if (input_.at(position_) == '"') {
			return parseString();
		}
		return parsePrimitive();
	}

	QByteArrayView input_;
	qsizetype position_ = 0;
};

std::optional<QJsonObject> parseRootObject(const QByteArray &body)
{
	if (!body.isValidUtf8() || !JsonDuplicateKeyScanner(QByteArrayView(body)).isValidAndUnique()) {
		return std::nullopt;
	}
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		return std::nullopt;
	}
	return document.object();
}

std::optional<std::string> parseIdentifier(const QJsonValue &value)
{
	if (!value.isString()) {
		return std::nullopt;
	}
	QByteArray encoded = value.toString().toUtf8();
	const std::string_view view(encoded.constData(), static_cast<std::size_t>(encoded.size()));
	if (!isValidIdentifier(view)) {
		return std::nullopt;
	}
	return std::string(view);
}

std::optional<std::string> parseLabel(const QJsonValue &value)
{
	if (!value.isString()) {
		return std::nullopt;
	}
	const QString label = value.toString();
	if (label.isEmpty()) {
		return std::nullopt;
	}
	for (qsizetype index = 0; index < label.size(); ++index) {
		const ushort codePoint = label.at(index).unicode();
		if (codePoint <= 0x1FU || (codePoint >= 0x7FU && codePoint <= 0x9FU) || codePoint == 0x2028U ||
		    codePoint == 0x2029U) {
			return std::nullopt;
		}
		if (QChar::isHighSurrogate(codePoint)) {
			if (++index >= label.size() || !QChar::isLowSurrogate(label.at(index).unicode())) {
				return std::nullopt;
			}
		} else if (QChar::isLowSurrogate(codePoint)) {
			return std::nullopt;
		}
	}
	const QByteArray encoded = label.toUtf8();
	if (encoded.isEmpty() || static_cast<std::size_t>(encoded.size()) > kYouTubeApiMaxLabelBytes) {
		return std::nullopt;
	}
	return std::string(encoded.constData(), static_cast<std::size_t>(encoded.size()));
}

std::optional<std::string> parsePageToken(const QJsonValue &value)
{
	if (value.isUndefined()) {
		return std::string{};
	}
	if (!value.isString()) {
		return std::nullopt;
	}
	const QString token = value.toString();
	if (token.isEmpty() || token.size() > kMaxPageTokenBytes) {
		return std::nullopt;
	}
	for (const QChar character : token) {
		const ushort codePoint = character.unicode();
		if (codePoint <= 0x20U || codePoint >= 0x7FU) {
			return std::nullopt;
		}
	}
	const QByteArray encoded = token.toLatin1();
	const std::string_view view(encoded.constData(), static_cast<std::size_t>(encoded.size()));
	if (!isVisibleAscii(view, static_cast<std::size_t>(kMaxPageTokenBytes))) {
		return std::nullopt;
	}
	return std::string(view);
}

struct ParsedChannelPage final {
	std::vector<YouTubeOwnedChannel> items;
	std::string nextPageToken;
};

struct ParsedStreamPage final {
	std::vector<YouTubeReusableStream> items;
	std::string nextPageToken;
};

std::optional<ParsedChannelPage> parseChannelPage(const QByteArray &body)
{
	const auto root = parseRootObject(body);
	if (!root.has_value() || !root->value(QStringLiteral("error")).isUndefined()) {
		return std::nullopt;
	}
	const QJsonValue itemsValue = root->value(QStringLiteral("items"));
	if (!itemsValue.isArray()) {
		return std::nullopt;
	}
	const auto nextPageToken = parsePageToken(root->value(QStringLiteral("nextPageToken")));
	if (!nextPageToken.has_value()) {
		return std::nullopt;
	}

	ParsedChannelPage page;
	page.nextPageToken = *nextPageToken;
	const QJsonArray items = itemsValue.toArray();
	if (items.size() > kMaxItemsPerPage) {
		return std::nullopt;
	}
	page.items.reserve(static_cast<std::size_t>(items.size()));
	QSet<QString> ids;
	for (const QJsonValue &itemValue : items) {
		if (!itemValue.isObject()) {
			return std::nullopt;
		}
		const QJsonObject item = itemValue.toObject();
		const auto id = parseIdentifier(item.value(QStringLiteral("id")));
		const QJsonValue snippetValue = item.value(QStringLiteral("snippet"));
		if (!id.has_value() || !snippetValue.isObject()) {
			return std::nullopt;
		}
		const auto label = parseLabel(snippetValue.toObject().value(QStringLiteral("title")));
		if (!label.has_value()) {
			return std::nullopt;
		}
		const QString qId = QString::fromLatin1(id->data(), static_cast<qsizetype>(id->size()));
		if (ids.contains(qId)) {
			return std::nullopt;
		}
		ids.insert(qId);
		page.items.push_back({*id, *label});
	}
	return page;
}

std::optional<ParsedStreamPage> parseStreamPage(const QByteArray &body, std::string_view expectedChannelId)
{
	const auto root = parseRootObject(body);
	if (!root.has_value() || !root->value(QStringLiteral("error")).isUndefined()) {
		return std::nullopt;
	}
	const QJsonValue itemsValue = root->value(QStringLiteral("items"));
	if (!itemsValue.isArray()) {
		return std::nullopt;
	}
	const auto nextPageToken = parsePageToken(root->value(QStringLiteral("nextPageToken")));
	if (!nextPageToken.has_value()) {
		return std::nullopt;
	}

	ParsedStreamPage page;
	page.nextPageToken = *nextPageToken;
	const QJsonArray items = itemsValue.toArray();
	if (items.size() > kMaxItemsPerPage) {
		return std::nullopt;
	}
	page.items.reserve(static_cast<std::size_t>(items.size()));
	QSet<QString> ids;
	for (const QJsonValue &itemValue : items) {
		if (!itemValue.isObject()) {
			return std::nullopt;
		}
		const QJsonObject item = itemValue.toObject();
		const auto id = parseIdentifier(item.value(QStringLiteral("id")));
		const QJsonValue snippetValue = item.value(QStringLiteral("snippet"));
		if (!id.has_value() || !snippetValue.isObject()) {
			return std::nullopt;
		}
		const QJsonObject snippet = snippetValue.toObject();
		const auto channelId = parseIdentifier(snippet.value(QStringLiteral("channelId")));
		const auto label = parseLabel(snippet.value(QStringLiteral("title")));
		if (!channelId.has_value() || !label.has_value()) {
			return std::nullopt;
		}
		const QString qId = QString::fromLatin1(id->data(), static_cast<qsizetype>(id->size()));
		if (ids.contains(qId)) {
			return std::nullopt;
		}
		ids.insert(qId);

		if (*channelId != expectedChannelId) {
			continue;
		}
		page.items.push_back({*id, *channelId, *label});
	}
	return page;
}

QString parseProviderReason(const QByteArray &body)
{
	const auto root = parseRootObject(body);
	if (!root.has_value()) {
		return {};
	}
	const QJsonValue errorValue = root->value(QStringLiteral("error"));
	if (!errorValue.isObject()) {
		return {};
	}
	const QJsonValue errorsValue = errorValue.toObject().value(QStringLiteral("errors"));
	if (!errorsValue.isArray() || errorsValue.toArray().isEmpty() || !errorsValue.toArray().first().isObject()) {
		return {};
	}
	const QJsonValue reasonValue = errorsValue.toArray().first().toObject().value(QStringLiteral("reason"));
	if (!reasonValue.isString()) {
		return {};
	}
	const QString reason = reasonValue.toString();
	if (reason.isEmpty() || reason.size() > 128) {
		return {};
	}
	for (const QChar character : reason) {
		const ushort codePoint = character.unicode();
		if (codePoint <= 0x20U || codePoint >= 0x7FU) {
			return {};
		}
	}
	return reason;
}

YouTubeApiProviderError classifyProviderError(int httpStatus, const QByteArray &body)
{
	if (httpStatus == 401) {
		return YouTubeApiProviderError::InvalidToken;
	}
	if (httpStatus == 429) {
		return YouTubeApiProviderError::RateLimited;
	}
	if (httpStatus >= 500 && httpStatus <= 599) {
		return YouTubeApiProviderError::TemporarilyUnavailable;
	}
	if (httpStatus == 404) {
		return YouTubeApiProviderError::NotFound;
	}

	const QString reason = parseProviderReason(body);
	if (reason == QStringLiteral("invalidPageToken")) {
		return YouTubeApiProviderError::InvalidPageToken;
	}
	if (reason == QStringLiteral("insufficientPermissions") ||
	    reason == QStringLiteral("insufficientLivePermissions")) {
		return YouTubeApiProviderError::InsufficientPermissions;
	}
	if (reason == QStringLiteral("liveStreamingNotEnabled")) {
		return YouTubeApiProviderError::LiveStreamingNotEnabled;
	}
	if (reason == QStringLiteral("quotaExceeded") || reason == QStringLiteral("dailyLimitExceeded")) {
		return YouTubeApiProviderError::QuotaExceeded;
	}
	if (reason == QStringLiteral("rateLimitExceeded") || reason == QStringLiteral("userRateLimitExceeded") ||
	    reason == QStringLiteral("userRequestsExceedRateLimit")) {
		return YouTubeApiProviderError::RateLimited;
	}
	if (httpStatus == 400) {
		return YouTubeApiProviderError::InvalidRequest;
	}
	if (httpStatus == 403) {
		return YouTubeApiProviderError::PermissionDenied;
	}
	return YouTubeApiProviderError::Unknown;
}

QUrl makeEndpoint(YouTubeApiOperation operation, std::string_view pageToken)
{
	QUrl url(QString::fromLatin1(operation == YouTubeApiOperation::ListOwnedChannels
					     ? kYouTubeChannelsEndpoint
					     : kYouTubeLiveStreamsEndpoint));
	QUrlQuery query;
	if (operation == YouTubeApiOperation::ListOwnedChannels) {
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("id,snippet"));
		query.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
		query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
		query.addQueryItem(QStringLiteral("fields"), QString::fromLatin1(kChannelsFields));
	} else {
		query.addQueryItem(QStringLiteral("part"), QStringLiteral("id,snippet"));
		query.addQueryItem(QStringLiteral("mine"), QStringLiteral("true"));
		query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("50"));
		query.addQueryItem(QStringLiteral("fields"), QString::fromLatin1(kStreamsFields));
	}
	if (!pageToken.empty()) {
		query.addQueryItem(QStringLiteral("pageToken"),
				   QString::fromLatin1(pageToken.data(), static_cast<qsizetype>(pageToken.size())));
	}
	url.setQuery(query);
	return url;
}

QNetworkRequest makeNetworkRequest(const QUrl &endpoint, const SecureBuffer &accessToken,
				   const YouTubeApiDiscoveryOptions &options)
{
	QNetworkRequest request(endpoint);
	QByteArray authorization("Bearer ");
	authorization.append(accessToken.view().data(), static_cast<qsizetype>(accessToken.size()));
	request.setRawHeader("Authorization", authorization);
	wipe(authorization);
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

} // namespace

class YouTubeApiDiscovery::Impl final {
public:
	Impl(YouTubeApiDiscovery *owner, std::unique_ptr<QNetworkAccessManager> manager,
	     YouTubeApiDiscoveryOptions options)
		: owner_(owner),
		  manager_(std::move(manager)),
		  options_(options)
	{
		Q_ASSERT_X(manager_ == nullptr || manager_->parent() == nullptr, "YouTubeApiDiscovery::Impl",
			   "The injected test network manager must be unparented because ownership is transferred");
		Q_ASSERT_X(manager_ == nullptr || manager_->thread() == owner_->thread(), "YouTubeApiDiscovery::Impl",
			   "The network manager must be created on the transport owner thread");
		if (!manager_) {
			manager_ = std::make_unique<QNetworkAccessManager>();
		}
		overallTimer_.setSingleShot(true);
		QObject::connect(&overallTimer_, &QTimer::timeout, owner_, [this]() {
			if (state_ == YouTubeApiTransportState::InFlight) {
				finishForced(YouTubeApiCompletionStatus::TimedOut);
			}
		});
		QObject::connect(manager_.get(), &QNetworkAccessManager::authenticationRequired, owner_,
				 [this](QNetworkReply *reply, QAuthenticator *) {
					 // Never supply origin credentials. Record only a challenge for
					 // the exact active reply so a completed Bearer rejection can
					 // become InvalidToken even if Qt omits the HTTP status.
					 if (isCurrent(completionEpoch_, reply)) {
						 sawOriginAuthChallenge_ = true;
					 }
				 });
		// Proxy credentials are also never supplied. Unlike the origin signal,
		// proxyAuthenticationRequired carries no reply identity; leaving it
		// unconnected prevents a late challenge from an aborted request from
		// terminating a newer operation. The reply completes as NetworkFailure.
	}

	~Impl()
	{
		shutdown();
		wipe(response_);
	}

	YouTubeApiStartStatus startListChannels(YouTubeListChannelsRequest request,
						YouTubeApiDiscovery::CompletionHandler handler) noexcept
	{
		const YouTubeApiStartStatus common =
			validateStart(request.attempt, request.accessToken, request.pageToken, handler);
		if (common != YouTubeApiStartStatus::Started) {
			return common;
		}
		return begin(request.attempt, YouTubeApiOperation::ListOwnedChannels, std::move(request.accessToken),
			     {}, std::move(request.pageToken), std::move(handler));
	}

	YouTubeApiStartStatus startListStreams(YouTubeListStreamsRequest request,
					       YouTubeApiDiscovery::CompletionHandler handler) noexcept
	{
		const YouTubeApiStartStatus common =
			validateStart(request.attempt, request.accessToken, request.pageToken, handler);
		if (common != YouTubeApiStartStatus::Started) {
			return common;
		}
		if (!isValidIdentifier(request.channelId)) {
			return YouTubeApiStartStatus::InvalidChannelId;
		}
		return begin(request.attempt, YouTubeApiOperation::ListReusableStreams, std::move(request.accessToken),
			     std::move(request.channelId), std::move(request.pageToken), std::move(handler));
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
		shutdownOperation(YouTubeApiTransportState::Cancelled);
		return true;
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		shutdownOperation(YouTubeApiTransportState::Closed);
		return true;
	}

	YouTubeApiTransportState state() const noexcept { return state_; }
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept { return activeAttempt_; }
	std::optional<YouTubeApiOperation> activeOperation() const noexcept { return activeOperation_; }

private:
	bool onOwnerThread() const noexcept
	{
		return owner_ != nullptr && owner_->thread() == QThread::currentThread() && manager_ != nullptr &&
		       manager_->thread() == QThread::currentThread() &&
		       overallTimer_.thread() == QThread::currentThread();
	}

	bool canStart() const noexcept
	{
		return state_ != YouTubeApiTransportState::InFlight && reply_.isNull() &&
		       !pendingCompletion_.has_value() && !completionDeliveryQueued_;
	}

	YouTubeApiStartStatus validateStart(YouTubeApiAttempt attempt, const SecureBuffer &accessToken,
					    std::string_view pageToken,
					    const YouTubeApiDiscovery::CompletionHandler &handler) const noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeApiStartStatus::WrongThread;
		}
		if (state_ == YouTubeApiTransportState::Closed) {
			return YouTubeApiStartStatus::Closed;
		}
		if (!canStart()) {
			return YouTubeApiStartStatus::Busy;
		}
		if (!isValidOptions(options_)) {
			return YouTubeApiStartStatus::InvalidOptions;
		}
		if (!attempt.isValid()) {
			return YouTubeApiStartStatus::InvalidAttempt;
		}
		if (!isVisibleAscii(accessToken.view(), kYouTubeApiMaxAccessTokenBytes)) {
			return YouTubeApiStartStatus::InvalidAccessToken;
		}
		if (!pageToken.empty() && !isVisibleAscii(pageToken, static_cast<std::size_t>(kMaxPageTokenBytes))) {
			return YouTubeApiStartStatus::InvalidPageToken;
		}
		if (!handler) {
			return YouTubeApiStartStatus::InvalidCompletionHandler;
		}
		return YouTubeApiStartStatus::Started;
	}

	YouTubeApiStartStatus begin(YouTubeApiAttempt attempt, YouTubeApiOperation operation, SecureBuffer accessToken,
				    std::string channelId, std::string pageToken,
				    YouTubeApiDiscovery::CompletionHandler handler) noexcept
	{
		try {
			advanceCompletionEpoch();
			activeAttempt_ = attempt;
			activeOperation_ = operation;
			accessToken_ = std::move(accessToken);
			channelId_ = std::move(channelId);
			completionHandler_ = std::move(handler);
			state_ = YouTubeApiTransportState::InFlight;
			endpoint_ = makeEndpoint(operation, pageToken);
			wipe(response_);
			sawTlsError_ = false;
			sawOriginAuthChallenge_ = false;
			declaredContentLength_.reset();

			QNetworkRequest networkRequest = makeNetworkRequest(endpoint_, accessToken_, options_);
			QNetworkReply *reply = manager_->get(networkRequest);
			if (reply == nullptr) {
				resetActiveState(YouTubeApiTransportState::Idle);
				return YouTubeApiStartStatus::RequestCreationFailed;
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
					finishForced(YouTubeApiCompletionStatus::RedirectRejected);
				}
			});
			QObject::connect(reply, &QNetworkReply::sslErrors, owner_,
					 [this, epoch, reply](const QList<QSslError> &) {
						 if (isCurrent(epoch, reply)) {
							 sawTlsError_ = true;
							 finishForced(YouTubeApiCompletionStatus::TlsFailure);
						 }
					 });
			QObject::connect(reply, &QNetworkReply::downloadProgress, owner_,
					 [this, epoch, reply](qint64 received, qint64) {
						 if (isCurrent(epoch, reply) && received > options_.maxResponseBytes) {
							 finishForced(YouTubeApiCompletionStatus::ResponseTooLarge);
						 }
					 });
			QObject::connect(reply, &QNetworkReply::finished, owner_, [this, epoch, reply]() {
				if (isCurrent(epoch, reply)) {
					finishFromReply(reply);
				}
			});
			QObject::connect(reply, &QObject::destroyed, owner_, [this, epoch]() {
				if (completionEpoch_ == epoch && state_ == YouTubeApiTransportState::InFlight) {
					reply_ = nullptr;
					finishWithoutReply(YouTubeApiCompletionStatus::NetworkFailure);
				}
			});
			overallTimer_.start(options_.operationTimeout);
			return YouTubeApiStartStatus::Started;
		} catch (...) {
			shutdownOperation(YouTubeApiTransportState::Idle);
			return YouTubeApiStartStatus::RequestCreationFailed;
		}
	}

	bool isCurrent(std::uint64_t epoch, QNetworkReply *reply) const noexcept
	{
		return epoch == completionEpoch_ && state_ == YouTubeApiTransportState::InFlight &&
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
				finishForced(YouTubeApiCompletionStatus::ResponseTooLarge);
				return;
			}
			QByteArray chunk = reply->read(std::min<qint64>(reply->bytesAvailable(), remaining + 1));
			if (chunk.isEmpty()) {
				break;
			}
			response_.append(chunk);
			wipe(chunk);
			if (response_.size() > options_.maxResponseBytes) {
				finishForced(YouTubeApiCompletionStatus::ResponseTooLarge);
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
			finishForced(YouTubeApiCompletionStatus::InvalidResponse);
			return;
		}

		if (contentLengthCount == 1) {
			bool ok = false;
			const qlonglong contentLength = reply->rawHeader("Content-Length").trimmed().toLongLong(&ok);
			if (!ok || contentLength < 0) {
				finishForced(YouTubeApiCompletionStatus::InvalidResponse);
				return;
			}
			if (contentLength > options_.maxResponseBytes) {
				finishForced(YouTubeApiCompletionStatus::ResponseTooLarge);
				return;
			}
			declaredContentLength_ = contentLength;
		}
		if (contentEncodingCount == 1 &&
		    reply->rawHeader("Content-Encoding").trimmed().compare("identity", Qt::CaseInsensitive) != 0) {
			finishForced(YouTubeApiCompletionStatus::InvalidResponse);
			return;
		}
		if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
			finishForced(YouTubeApiCompletionStatus::RedirectRejected);
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
		const bool originAuthenticationRejected =
			sawOriginAuthChallenge_ &&
			(httpStatus == 401 ||
			 (httpStatus == 0 && networkError == QNetworkReply::AuthenticationRequiredError));
		if (sawTlsError_ || networkError == QNetworkReply::SslHandshakeFailedError) {
			finishForced(YouTubeApiCompletionStatus::TlsFailure, httpStatus);
			return;
		}
		if (networkError == QNetworkReply::TimeoutError) {
			finishForced(YouTubeApiCompletionStatus::TimedOut, httpStatus);
			return;
		}
		if (!originAuthenticationRejected && httpStatus == 0 && networkError != QNetworkReply::NoError) {
			finishForced(YouTubeApiCompletionStatus::NetworkFailure);
			return;
		}
		if (!reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool()) {
			finishForced(YouTubeApiCompletionStatus::TlsFailure, httpStatus);
			return;
		}
		if (reply->url() != endpoint_ || (httpStatus >= 300 && httpStatus < 400) ||
		    reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
			finishForced(YouTubeApiCompletionStatus::RedirectRejected, httpStatus);
			return;
		}
		if (declaredContentLength_.has_value() && *declaredContentLength_ != response_.size()) {
			finishForced(YouTubeApiCompletionStatus::InvalidResponse, httpStatus);
			return;
		}
		if (!originAuthenticationRejected &&
		    (httpStatus < 400 || !canClassifyCompletedHttpResponse(networkError)) &&
		    networkError != QNetworkReply::NoError) {
			finishForced(YouTubeApiCompletionStatus::NetworkFailure, httpStatus);
			return;
		}

		YouTubeApiCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.operation = *activeOperation_;
		completion.httpStatus = httpStatus;
		if (originAuthenticationRejected) {
			completion.status = YouTubeApiCompletionStatus::ProviderRejected;
			completion.providerError = YouTubeApiProviderError::InvalidToken;
			finish(std::move(completion));
			return;
		}
		if (httpStatus == 200) {
			if (!isJsonContentType(reply->rawHeader("Content-Type"))) {
				completion.status = YouTubeApiCompletionStatus::InvalidResponse;
				finish(std::move(completion));
				return;
			}
			if (*activeOperation_ == YouTubeApiOperation::ListOwnedChannels) {
				auto page = parseChannelPage(response_);
				if (!page.has_value()) {
					completion.status = YouTubeApiCompletionStatus::InvalidResponse;
					finish(std::move(completion));
					return;
				}
				completion.channels = std::move(page->items);
				completion.nextPageToken = std::move(page->nextPageToken);
			} else {
				auto page = parseStreamPage(response_, channelId_);
				if (!page.has_value()) {
					completion.status = YouTubeApiCompletionStatus::InvalidResponse;
					finish(std::move(completion));
					return;
				}
				completion.streams = std::move(page->items);
				completion.nextPageToken = std::move(page->nextPageToken);
			}
			completion.status = YouTubeApiCompletionStatus::Success;
			finish(std::move(completion));
			return;
		}

		const YouTubeApiProviderError providerError = classifyProviderError(httpStatus, response_);
		if (providerError != YouTubeApiProviderError::Unknown) {
			completion.status = YouTubeApiCompletionStatus::ProviderRejected;
			completion.providerError = providerError;
			finish(std::move(completion));
			return;
		}
		completion.status = httpStatus > 0 ? YouTubeApiCompletionStatus::HttpFailure
						   : YouTubeApiCompletionStatus::NetworkFailure;
		finish(std::move(completion));
	}

	void finishForced(YouTubeApiCompletionStatus status, int httpStatus = 0)
	{
		if (!activeAttempt_.has_value() || !activeOperation_.has_value()) {
			return;
		}
		YouTubeApiCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.operation = *activeOperation_;
		completion.status = status;
		completion.httpStatus = httpStatus;
		finish(std::move(completion));
	}

	void finishWithoutReply(YouTubeApiCompletionStatus status)
	{
		if (!activeAttempt_.has_value() || !activeOperation_.has_value()) {
			return;
		}
		YouTubeApiCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.operation = *activeOperation_;
		completion.status = status;
		finish(std::move(completion));
	}

	void finish(YouTubeApiCompletion completion)
	{
		if (state_ != YouTubeApiTransportState::InFlight) {
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
		accessToken_.clear();
		channelId_.clear();
		endpoint_.clear();
		declaredContentLength_.reset();
		sawOriginAuthChallenge_ = false;
		activeAttempt_.reset();
		activeOperation_.reset();
		state_ = YouTubeApiTransportState::Completed;
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
		YouTubeApiCompletion completion = std::move(*pendingCompletion_);
		pendingCompletion_.reset();
		completionHandler_ = {};
		if (handler) {
			try {
				handler(std::move(completion));
			} catch (...) {
				// Never unwind an integration callback through Qt's event loop.
			}
		}
	}

	void shutdownOperation(YouTubeApiTransportState terminalState) noexcept
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
		accessToken_.clear();
		channelId_.clear();
		endpoint_.clear();
		declaredContentLength_.reset();
		sawOriginAuthChallenge_ = false;
		activeAttempt_.reset();
		activeOperation_.reset();
		completionHandler_ = {};
		pendingCompletion_.reset();
		state_ = terminalState;
	}

	void resetActiveState(YouTubeApiTransportState state) noexcept
	{
		wipe(response_);
		accessToken_.clear();
		channelId_.clear();
		endpoint_.clear();
		declaredContentLength_.reset();
		sawOriginAuthChallenge_ = false;
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

	YouTubeApiDiscovery *owner_ = nullptr;
	std::unique_ptr<QNetworkAccessManager> manager_;
	YouTubeApiDiscoveryOptions options_;
	QTimer overallTimer_;
	QPointer<QNetworkReply> reply_;
	YouTubeApiTransportState state_ = YouTubeApiTransportState::Idle;
	std::optional<YouTubeApiAttempt> activeAttempt_;
	std::optional<YouTubeApiOperation> activeOperation_;
	SecureBuffer accessToken_;
	std::string channelId_;
	QUrl endpoint_;
	QByteArray response_;
	std::optional<qint64> declaredContentLength_;
	bool sawTlsError_ = false;
	bool sawOriginAuthChallenge_ = false;
	YouTubeApiDiscovery::CompletionHandler completionHandler_;
	std::optional<YouTubeApiCompletion> pendingCompletion_;
	std::uint64_t completionEpoch_ = 1;
	bool completionDeliveryQueued_ = false;
};

YouTubeApiDiscovery::YouTubeApiDiscovery(YouTubeApiDiscoveryOptions options, QObject *parent)
	: YouTubeApiDiscovery(std::make_unique<QNetworkAccessManager>(), options, parent)
{
}

YouTubeApiDiscovery::YouTubeApiDiscovery(std::unique_ptr<QNetworkAccessManager> networkManager,
					 YouTubeApiDiscoveryOptions options, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(networkManager), options))
{
}

YouTubeApiDiscovery::~YouTubeApiDiscovery()
{
	Q_ASSERT_X(thread() == QThread::currentThread(), "YouTubeApiDiscovery::~YouTubeApiDiscovery",
		   "Destroy the transport on its owner thread after shutdown");
	if (thread() == QThread::currentThread() && impl_) {
		impl_->shutdown();
	}
}

YouTubeApiStartStatus YouTubeApiDiscovery::startListChannels(YouTubeListChannelsRequest request,
							     CompletionHandler completionHandler) noexcept
{
	return impl_->startListChannels(std::move(request), std::move(completionHandler));
}

YouTubeApiStartStatus YouTubeApiDiscovery::startListStreams(YouTubeListStreamsRequest request,
							    CompletionHandler completionHandler) noexcept
{
	return impl_->startListStreams(std::move(request), std::move(completionHandler));
}

bool YouTubeApiDiscovery::cancel(YouTubeApiAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool YouTubeApiDiscovery::shutdown() noexcept
{
	return impl_->shutdown();
}

YouTubeApiTransportState YouTubeApiDiscovery::state() const noexcept
{
	return impl_->state();
}

std::optional<YouTubeApiAttempt> YouTubeApiDiscovery::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

std::optional<YouTubeApiOperation> YouTubeApiDiscovery::activeOperation() const noexcept
{
	return impl_->activeOperation();
}

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-api-stream-resolver.hpp"

#include "youtube-destination.hpp"

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
constexpr std::size_t kMaxJsonNestingDepth = 64;
constexpr auto kResolverFields =
	"items(id,snippet(channelId),cdn(ingestionType,ingestionInfo(rtmpsIngestionAddress,streamName)),status(streamStatus))";

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

bool isValidOptions(const YouTubeStreamResolverOptions &options) noexcept
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

// Decode enough of a JSON object key to compare ASCII names, including their
// \u00XX escaped forms. QJsonDocument does the complete syntax/UTF-8 parse
// after this duplicate-key pass.
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
	const QByteArray encoded = value.toString().toUtf8();
	const std::string_view view(encoded.constData(), static_cast<std::size_t>(encoded.size()));
	if (!isValidIdentifier(view)) {
		return std::nullopt;
	}
	return std::string(view);
}

std::optional<std::string> parseAsciiValue(const QJsonValue &value, std::size_t maximumBytes)
{
	if (!value.isString()) {
		return std::nullopt;
	}
	const QByteArray encoded = value.toString().toUtf8();
	const std::string_view view(encoded.constData(), static_cast<std::size_t>(encoded.size()));
	if (!isVisibleAscii(view, maximumBytes)) {
		return std::nullopt;
	}
	return std::string(view);
}

std::optional<SecureBuffer> parseStreamKey(const QJsonValue &value)
{
	if (!value.isString()) {
		return std::nullopt;
	}
	QByteArray encoded = value.toString().toUtf8();
	const std::string_view view(encoded.constData(), static_cast<std::size_t>(encoded.size()));
	if (validateYouTubeStreamKey(view) != StreamKeyValidationError::None) {
		wipe(encoded);
		return std::nullopt;
	}
	try {
		SecureBuffer result = SecureBuffer::copyOf(view);
		wipe(encoded);
		return result;
	} catch (...) {
		wipe(encoded);
		throw;
	}
}

struct ParsedResolvedStream final {
	std::string serverUrl;
	SecureBuffer streamKey;
};

enum class ParseResolvedStreamStatus {
	Success,
	NotFound,
	Mismatch,
	AlreadyActive,
	NotReady,
	Invalid,
};

struct ParseResolvedStreamResult final {
	ParseResolvedStreamStatus status = ParseResolvedStreamStatus::Invalid;
	std::optional<ParsedResolvedStream> value;
};

ParseResolvedStreamResult parseResolvedStream(const QByteArray &body, std::string_view expectedChannelId,
					      std::string_view expectedStreamId)
{
	const auto root = parseRootObject(body);
	if (!root.has_value() || !root->value(QStringLiteral("error")).isUndefined()) {
		return {};
	}
	const QJsonValue itemsValue = root->value(QStringLiteral("items"));
	if (!itemsValue.isArray()) {
		return {};
	}
	const QJsonArray items = itemsValue.toArray();
	if (items.isEmpty()) {
		return {ParseResolvedStreamStatus::NotFound, std::nullopt};
	}
	if (items.size() != 1 || !items.first().isObject()) {
		return {};
	}

	const QJsonObject item = items.first().toObject();
	const auto id = parseIdentifier(item.value(QStringLiteral("id")));
	const QJsonValue snippetValue = item.value(QStringLiteral("snippet"));
	const QJsonValue cdnValue = item.value(QStringLiteral("cdn"));
	const QJsonValue statusValue = item.value(QStringLiteral("status"));
	if (!id.has_value() || !snippetValue.isObject() || !cdnValue.isObject() || !statusValue.isObject()) {
		return {};
	}
	if (*id != expectedStreamId) {
		return {ParseResolvedStreamStatus::Mismatch, std::nullopt};
	}

	const auto channelId = parseIdentifier(snippetValue.toObject().value(QStringLiteral("channelId")));
	if (!channelId.has_value() || *channelId != expectedChannelId) {
		return {ParseResolvedStreamStatus::Mismatch, std::nullopt};
	}

	const QJsonObject cdn = cdnValue.toObject();
	const auto ingestionType = parseAsciiValue(cdn.value(QStringLiteral("ingestionType")), 32);
	const QJsonValue ingestionInfoValue = cdn.value(QStringLiteral("ingestionInfo"));
	if (!ingestionType.has_value() || *ingestionType != "rtmp" || !ingestionInfoValue.isObject()) {
		return {};
	}

	const auto streamStatus = parseAsciiValue(statusValue.toObject().value(QStringLiteral("streamStatus")), 32);
	if (!streamStatus.has_value()) {
		return {};
	}
	if (*streamStatus == "active") {
		return {ParseResolvedStreamStatus::AlreadyActive, std::nullopt};
	}
	if (*streamStatus == "created" || *streamStatus == "error") {
		return {ParseResolvedStreamStatus::NotReady, std::nullopt};
	}
	if (*streamStatus != "ready" && *streamStatus != "inactive") {
		return {};
	}

	const QJsonObject ingestionInfo = ingestionInfoValue.toObject();
	const auto serverUrl = parseAsciiValue(ingestionInfo.value(QStringLiteral("rtmpsIngestionAddress")),
					       kMaxYouTubeServerUrlBytes);
	if (!serverUrl.has_value() || validateYouTubeServerUrl(*serverUrl) != YouTubeServerUrlValidationError::None) {
		return {};
	}
	auto streamKey = parseStreamKey(ingestionInfo.value(QStringLiteral("streamName")));
	if (!streamKey.has_value() || streamKey->empty()) {
		return {};
	}

	// The only point at which the secret enters the result boundary. The
	// QJson/QString copies are provider-library temporaries that are not
	// retained after this function returns.
	ParsedResolvedStream parsed;
	parsed.serverUrl = *serverUrl;
	parsed.streamKey = std::move(*streamKey);
	return {ParseResolvedStreamStatus::Success, std::move(parsed)};
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

QUrl makeEndpoint(std::string_view streamId)
{
	QUrl url(QString::fromLatin1(kYouTubeLiveStreamsEndpoint));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("part"), QStringLiteral("id,snippet,cdn,status"));
	query.addQueryItem(QStringLiteral("id"),
			   QString::fromLatin1(streamId.data(), static_cast<qsizetype>(streamId.size())));
	query.addQueryItem(QStringLiteral("maxResults"), QStringLiteral("1"));
	query.addQueryItem(QStringLiteral("fields"), QString::fromLatin1(kResolverFields));
	url.setQuery(query);
	return url;
}

QNetworkRequest makeNetworkRequest(const QUrl &endpoint, const SecureBuffer &accessToken,
				   const YouTubeStreamResolverOptions &options)
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

class YouTubeApiStreamResolver::Impl final {
public:
	Impl(YouTubeApiStreamResolver *owner, std::unique_ptr<QNetworkAccessManager> manager,
	     YouTubeStreamResolverOptions options)
		: owner_(owner),
		  manager_(std::move(manager)),
		  options_(options)
	{
		Q_ASSERT_X(manager_ == nullptr || manager_->parent() == nullptr, "YouTubeApiStreamResolver::Impl",
			   "The injected test network manager must be unparented because ownership is transferred");
		Q_ASSERT_X(manager_ == nullptr || manager_->thread() == owner_->thread(),
			   "YouTubeApiStreamResolver::Impl",
			   "The network manager must be created on the transport owner thread");
		if (!manager_) {
			manager_ = std::make_unique<QNetworkAccessManager>();
		}
		overallTimer_.setSingleShot(true);
		QObject::connect(&overallTimer_, &QTimer::timeout, owner_, [this]() {
			if (state_ == YouTubeStreamResolverState::InFlight) {
				finishForced(YouTubeStreamResolverCompletionStatus::TimedOut);
			}
		});
		QObject::connect(manager_.get(), &QNetworkAccessManager::authenticationRequired, owner_,
				 [this](QNetworkReply *reply, QAuthenticator *) {
					 // Never supply origin credentials. Record only a challenge for
					 // the exact current reply; a late old reply cannot affect a new
					 // attempt.
					 if (isCurrent(completionEpoch_, reply)) {
						 sawOriginAuthChallenge_ = true;
					 }
				 });
		// The proxy challenge carries no reply identity. Do not connect it:
		// supplying credentials is out of scope, and a late challenge must not
		// terminate a newer request.
	}

	~Impl()
	{
		shutdown();
		wipe(response_);
	}

	YouTubeStreamResolverStartStatus
	startResolveStream(YouTubeResolveStreamRequest request,
			   YouTubeApiStreamResolver::CompletionHandler handler) noexcept
	{
		if (!onOwnerThread()) {
			return YouTubeStreamResolverStartStatus::WrongThread;
		}
		if (state_ == YouTubeStreamResolverState::Closed) {
			return YouTubeStreamResolverStartStatus::Closed;
		}
		if (!canStart()) {
			return YouTubeStreamResolverStartStatus::Busy;
		}
		if (!isValidOptions(options_)) {
			return YouTubeStreamResolverStartStatus::InvalidOptions;
		}
		if (!request.attempt.isValid()) {
			return YouTubeStreamResolverStartStatus::InvalidAttempt;
		}
		if (!isVisibleAscii(request.accessToken.view(), kYouTubeApiMaxAccessTokenBytes)) {
			return YouTubeStreamResolverStartStatus::InvalidAccessToken;
		}
		if (!isValidIdentifier(request.channelId)) {
			return YouTubeStreamResolverStartStatus::InvalidChannelId;
		}
		if (!isValidIdentifier(request.streamId)) {
			return YouTubeStreamResolverStartStatus::InvalidStreamId;
		}
		if (!handler) {
			return YouTubeStreamResolverStartStatus::InvalidCompletionHandler;
		}

		try {
			advanceCompletionEpoch();
			activeAttempt_ = request.attempt;
			accessToken_ = std::move(request.accessToken);
			channelId_ = std::move(request.channelId);
			streamId_ = std::move(request.streamId);
			completionHandler_ = std::move(handler);
			state_ = YouTubeStreamResolverState::InFlight;
			endpoint_ = makeEndpoint(streamId_);
			wipe(response_);
			sawTlsError_ = false;
			sawOriginAuthChallenge_ = false;
			declaredContentLength_.reset();

			QNetworkRequest networkRequest = makeNetworkRequest(endpoint_, accessToken_, options_);
			QNetworkReply *reply = manager_->get(networkRequest);
			if (reply == nullptr) {
				resetActiveState(YouTubeStreamResolverState::Idle);
				return YouTubeStreamResolverStartStatus::RequestCreationFailed;
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
					finishForced(YouTubeStreamResolverCompletionStatus::RedirectRejected);
				}
			});
			QObject::connect(reply, &QNetworkReply::sslErrors, owner_,
					 [this, epoch, reply](const QList<QSslError> &) {
						 if (isCurrent(epoch, reply)) {
							 sawTlsError_ = true;
							 finishForced(
								 YouTubeStreamResolverCompletionStatus::TlsFailure);
						 }
					 });
			QObject::connect(
				reply, &QNetworkReply::downloadProgress, owner_,
				[this, epoch, reply](qint64 received, qint64) {
					if (isCurrent(epoch, reply) && received > options_.maxResponseBytes) {
						finishForced(YouTubeStreamResolverCompletionStatus::ResponseTooLarge);
					}
				});
			QObject::connect(reply, &QNetworkReply::finished, owner_, [this, epoch, reply]() {
				if (isCurrent(epoch, reply)) {
					finishFromReply(reply);
				}
			});
			QObject::connect(reply, &QObject::destroyed, owner_, [this, epoch]() {
				if (completionEpoch_ == epoch && state_ == YouTubeStreamResolverState::InFlight) {
					reply_ = nullptr;
					finishWithoutReply(YouTubeStreamResolverCompletionStatus::NetworkFailure);
				}
			});
			overallTimer_.start(options_.operationTimeout);
			return YouTubeStreamResolverStartStatus::Started;
		} catch (...) {
			shutdownOperation(YouTubeStreamResolverState::Idle);
			return YouTubeStreamResolverStartStatus::RequestCreationFailed;
		}
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
		shutdownOperation(YouTubeStreamResolverState::Cancelled);
		return true;
	}

	bool shutdown() noexcept
	{
		if (!onOwnerThread()) {
			return false;
		}
		shutdownOperation(YouTubeStreamResolverState::Closed);
		return true;
	}

	YouTubeStreamResolverState state() const noexcept { return state_; }
	std::optional<YouTubeApiAttempt> activeAttempt() const noexcept { return activeAttempt_; }

private:
	bool onOwnerThread() const noexcept
	{
		return owner_ != nullptr && owner_->thread() == QThread::currentThread() && manager_ != nullptr &&
		       manager_->thread() == QThread::currentThread() &&
		       overallTimer_.thread() == QThread::currentThread();
	}

	bool canStart() const noexcept
	{
		return state_ != YouTubeStreamResolverState::InFlight && reply_.isNull() &&
		       !pendingCompletion_.has_value() && !completionDeliveryQueued_;
	}

	bool isCurrent(std::uint64_t epoch, QNetworkReply *reply) const noexcept
	{
		return epoch == completionEpoch_ && state_ == YouTubeStreamResolverState::InFlight &&
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
				finishForced(YouTubeStreamResolverCompletionStatus::ResponseTooLarge);
				return;
			}
			QByteArray chunk = reply->read(std::min<qint64>(reply->bytesAvailable(), remaining + 1));
			if (chunk.isEmpty()) {
				break;
			}
			response_.append(chunk);
			wipe(chunk);
			if (response_.size() > options_.maxResponseBytes) {
				finishForced(YouTubeStreamResolverCompletionStatus::ResponseTooLarge);
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
			finishForced(YouTubeStreamResolverCompletionStatus::InvalidResponse);
			return;
		}
		if (contentLengthCount == 1) {
			bool ok = false;
			const qlonglong contentLength = reply->rawHeader("Content-Length").trimmed().toLongLong(&ok);
			if (!ok || contentLength < 0) {
				finishForced(YouTubeStreamResolverCompletionStatus::InvalidResponse);
				return;
			}
			if (contentLength > options_.maxResponseBytes) {
				finishForced(YouTubeStreamResolverCompletionStatus::ResponseTooLarge);
				return;
			}
			declaredContentLength_ = contentLength;
		}
		if (contentEncodingCount == 1 &&
		    reply->rawHeader("Content-Encoding").trimmed().compare("identity", Qt::CaseInsensitive) != 0) {
			finishForced(YouTubeStreamResolverCompletionStatus::InvalidResponse);
			return;
		}
		if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
			finishForced(YouTubeStreamResolverCompletionStatus::RedirectRejected);
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
			finishForced(YouTubeStreamResolverCompletionStatus::TlsFailure, httpStatus);
			return;
		}
		if (networkError == QNetworkReply::TimeoutError) {
			finishForced(YouTubeStreamResolverCompletionStatus::TimedOut, httpStatus);
			return;
		}
		if (!originAuthenticationRejected && httpStatus == 0 && networkError != QNetworkReply::NoError) {
			finishForced(YouTubeStreamResolverCompletionStatus::NetworkFailure);
			return;
		}
		if (!reply->attribute(QNetworkRequest::ConnectionEncryptedAttribute).toBool()) {
			finishForced(YouTubeStreamResolverCompletionStatus::TlsFailure, httpStatus);
			return;
		}
		if (reply->url() != endpoint_ || (httpStatus >= 300 && httpStatus < 400) ||
		    reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
			finishForced(YouTubeStreamResolverCompletionStatus::RedirectRejected, httpStatus);
			return;
		}
		if (declaredContentLength_.has_value() && *declaredContentLength_ != response_.size()) {
			finishForced(YouTubeStreamResolverCompletionStatus::InvalidResponse, httpStatus);
			return;
		}
		if (!originAuthenticationRejected &&
		    (httpStatus < 400 || !canClassifyCompletedHttpResponse(networkError)) &&
		    networkError != QNetworkReply::NoError) {
			finishForced(YouTubeStreamResolverCompletionStatus::NetworkFailure, httpStatus);
			return;
		}

		YouTubeStreamResolverCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.httpStatus = httpStatus;
		if (originAuthenticationRejected) {
			completion.status = YouTubeStreamResolverCompletionStatus::ProviderRejected;
			completion.providerError = YouTubeApiProviderError::InvalidToken;
			finish(std::move(completion));
			return;
		}
		if (httpStatus == 200) {
			if (!isJsonContentType(reply->rawHeader("Content-Type"))) {
				completion.status = YouTubeStreamResolverCompletionStatus::InvalidResponse;
				finish(std::move(completion));
				return;
			}
			ParseResolvedStreamResult parsed = parseResolvedStream(response_, channelId_, streamId_);
			switch (parsed.status) {
			case ParseResolvedStreamStatus::Success:
				if (!parsed.value.has_value()) {
					completion.status = YouTubeStreamResolverCompletionStatus::InvalidResponse;
					break;
				}
				completion.status = YouTubeStreamResolverCompletionStatus::Success;
				completion.ingestion.emplace();
				completion.ingestion->serverUrl = parsed.value->serverUrl;
				completion.ingestion->streamKey = std::move(parsed.value->streamKey);
				break;
			case ParseResolvedStreamStatus::NotFound:
				completion.status = YouTubeStreamResolverCompletionStatus::StreamNotFound;
				break;
			case ParseResolvedStreamStatus::Mismatch:
				completion.status = YouTubeStreamResolverCompletionStatus::StreamMismatch;
				break;
			case ParseResolvedStreamStatus::AlreadyActive:
				completion.status = YouTubeStreamResolverCompletionStatus::StreamAlreadyActive;
				break;
			case ParseResolvedStreamStatus::NotReady:
				completion.status = YouTubeStreamResolverCompletionStatus::StreamNotReady;
				break;
			case ParseResolvedStreamStatus::Invalid:
				completion.status = YouTubeStreamResolverCompletionStatus::InvalidResponse;
				break;
			}
			finish(std::move(completion));
			return;
		}

		completion.providerError = classifyProviderError(httpStatus, response_);
		if (httpStatus == 404 || completion.providerError == YouTubeApiProviderError::NotFound) {
			completion.status = YouTubeStreamResolverCompletionStatus::StreamNotFound;
		} else if (completion.providerError != YouTubeApiProviderError::Unknown) {
			completion.status = YouTubeStreamResolverCompletionStatus::ProviderRejected;
		} else {
			completion.status = httpStatus > 0 ? YouTubeStreamResolverCompletionStatus::HttpFailure
							   : YouTubeStreamResolverCompletionStatus::NetworkFailure;
		}
		finish(std::move(completion));
	}

	void finishForced(YouTubeStreamResolverCompletionStatus status, int httpStatus = 0)
	{
		if (!activeAttempt_.has_value()) {
			return;
		}
		YouTubeStreamResolverCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.status = status;
		completion.httpStatus = httpStatus;
		finish(std::move(completion));
	}

	void finishWithoutReply(YouTubeStreamResolverCompletionStatus status)
	{
		if (!activeAttempt_.has_value()) {
			return;
		}
		YouTubeStreamResolverCompletion completion;
		completion.attempt = *activeAttempt_;
		completion.status = status;
		finish(std::move(completion));
	}

	void finish(YouTubeStreamResolverCompletion completion)
	{
		if (state_ != YouTubeStreamResolverState::InFlight) {
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
		streamId_.clear();
		endpoint_.clear();
		declaredContentLength_.reset();
		sawTlsError_ = false;
		sawOriginAuthChallenge_ = false;
		activeAttempt_.reset();
		state_ = YouTubeStreamResolverState::Completed;
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
		YouTubeStreamResolverCompletion completion = std::move(*pendingCompletion_);
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

	void shutdownOperation(YouTubeStreamResolverState terminalState) noexcept
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
		streamId_.clear();
		endpoint_.clear();
		declaredContentLength_.reset();
		sawTlsError_ = false;
		sawOriginAuthChallenge_ = false;
		activeAttempt_.reset();
		completionHandler_ = {};
		pendingCompletion_.reset();
		state_ = terminalState;
	}

	void resetActiveState(YouTubeStreamResolverState state) noexcept
	{
		wipe(response_);
		accessToken_.clear();
		channelId_.clear();
		streamId_.clear();
		endpoint_.clear();
		declaredContentLength_.reset();
		sawTlsError_ = false;
		sawOriginAuthChallenge_ = false;
		activeAttempt_.reset();
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

	YouTubeApiStreamResolver *owner_ = nullptr;
	std::unique_ptr<QNetworkAccessManager> manager_;
	YouTubeStreamResolverOptions options_;
	QTimer overallTimer_;
	QPointer<QNetworkReply> reply_;
	YouTubeStreamResolverState state_ = YouTubeStreamResolverState::Idle;
	std::optional<YouTubeApiAttempt> activeAttempt_;
	SecureBuffer accessToken_;
	std::string channelId_;
	std::string streamId_;
	QUrl endpoint_;
	QByteArray response_;
	std::optional<qint64> declaredContentLength_;
	bool sawTlsError_ = false;
	bool sawOriginAuthChallenge_ = false;
	YouTubeApiStreamResolver::CompletionHandler completionHandler_;
	std::optional<YouTubeStreamResolverCompletion> pendingCompletion_;
	std::uint64_t completionEpoch_ = 1;
	bool completionDeliveryQueued_ = false;
};

YouTubeApiStreamResolver::YouTubeApiStreamResolver(YouTubeStreamResolverOptions options, QObject *parent)
	: YouTubeApiStreamResolver(std::make_unique<QNetworkAccessManager>(), options, parent)
{
}

YouTubeApiStreamResolver::YouTubeApiStreamResolver(std::unique_ptr<QNetworkAccessManager> networkManager,
						   YouTubeStreamResolverOptions options, QObject *parent)
	: QObject(parent),
	  impl_(std::make_unique<Impl>(this, std::move(networkManager), options))
{
}

YouTubeApiStreamResolver::~YouTubeApiStreamResolver()
{
	Q_ASSERT_X(thread() == QThread::currentThread(), "YouTubeApiStreamResolver::~YouTubeApiStreamResolver",
		   "Destroy the resolver on its owner thread after shutdown");
	if (thread() == QThread::currentThread() && impl_) {
		impl_->shutdown();
	}
}

YouTubeStreamResolverStartStatus
YouTubeApiStreamResolver::startResolveStream(YouTubeResolveStreamRequest request,
					     CompletionHandler completionHandler) noexcept
{
	return impl_->startResolveStream(std::move(request), std::move(completionHandler));
}

bool YouTubeApiStreamResolver::cancel(YouTubeApiAttempt attempt) noexcept
{
	return impl_->cancel(attempt);
}

bool YouTubeApiStreamResolver::shutdown() noexcept
{
	return impl_->shutdown();
}

YouTubeStreamResolverState YouTubeApiStreamResolver::state() const noexcept
{
	return impl_->state();
}

std::optional<YouTubeApiAttempt> YouTubeApiStreamResolver::activeAttempt() const noexcept
{
	return impl_->activeAttempt();
}

} // namespace easy_multistream

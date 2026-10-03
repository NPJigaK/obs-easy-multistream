// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-protocol.hpp"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSet>
#include <QString>
#include <QUrlQuery>

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

namespace easy_multistream {
namespace {

constexpr std::size_t kRandomWordCount = kGoogleOAuthRandomBytes / sizeof(quint32);

static_assert(kGoogleOAuthRandomBytes % sizeof(quint32) == 0U,
		      "the random token size must be a whole number of QRandomGenerator words");

// A volatile wipe is used for temporary QByteArrays because a normal memset
// may be removed by an optimizing compiler.  SecureBuffer itself uses the
// platform secure wipe for its long-lived/move-only storage.
void wipe(QByteArray &bytes) noexcept
{
	volatile char *data = bytes.data();
	for (qsizetype index = 0; index < bytes.size(); ++index) {
		data[index] = 0;
	}
	bytes.clear();
}

template<std::size_t N> void wipe(std::array<quint32, N> &words) noexcept
{
	volatile quint32 *data = words.data();
	for (std::size_t index = 0; index < N; ++index) {
		data[index] = 0U;
	}
}

bool hasWhitespaceOrControlCharacter(const QString &value) noexcept
{
	for (const QChar character : value) {
		const ushort codePoint = character.unicode();
		// Include C0, DEL, and C1 controls.  In particular this rejects embedded
		// NUL without relying on QString's C-string conversion semantics.
		if (character.isSpace() || codePoint <= 0x1FU || (codePoint >= 0x7FU && codePoint <= 0x9FU)) {
			return true;
		}
	}

	return false;
}

bool isBase64UrlToken(std::string_view value) noexcept
{
	if (value.size() != static_cast<std::size_t>(kGoogleOAuthEncodedRandomLength)) {
		return false;
	}
	for (const unsigned char byte : value) {
		const bool alphaNumeric = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
					  (byte >= '0' && byte <= '9');
		if (!alphaNumeric && byte != '-' && byte != '_') {
			return false;
		}
	}
	return true;
}

QUrl makeLoopbackRedirectUri(int port)
{
	QUrl redirectUri;
	redirectUri.setScheme(QStringLiteral("http"));
	redirectUri.setHost(QString::fromLatin1(kGoogleOAuthLoopbackHost));
	redirectUri.setPort(port);
	redirectUri.setPath(QString::fromLatin1(kGoogleOAuthCallbackPath));
	return redirectUri;
}

bool isExactLoopbackRedirectUri(const QUrl &redirectUri) noexcept
{
	return redirectUri.isValid() && !redirectUri.isRelative() && redirectUri.scheme() == QStringLiteral("http") &&
	       redirectUri.host(QUrl::FullyEncoded) == QString::fromLatin1(kGoogleOAuthLoopbackHost) &&
	       redirectUri.port() >= 1 &&
	       redirectUri.port() <= std::numeric_limits<quint16>::max() &&
	       redirectUri.path(QUrl::FullyEncoded) == QString::fromLatin1(kGoogleOAuthCallbackPath) &&
	       redirectUri.userInfo().isEmpty() && !redirectUri.hasFragment() && !redirectUri.hasQuery();
}

bool constantTimeEquals(const QByteArray &actual, std::string_view expected) noexcept
{
	if (actual.size() != static_cast<qsizetype>(expected.size())) {
		return false;
	}

	unsigned char difference = 0U;
	for (qsizetype index = 0; index < actual.size(); ++index) {
		difference |= static_cast<unsigned char>(actual.at(index)) ^
			       static_cast<unsigned char>(expected[static_cast<std::size_t>(index)]);
	}

	return difference == 0U;
}

bool isSafeAuthorizationCode(const QByteArray &code) noexcept
{
	if (code.isEmpty()) {
		return false;
	}

	for (const unsigned char byte : code) {
		// Google authorization codes are opaque, visible ASCII values. Reject
		// whitespace, controls, DEL, and non-ASCII input before it reaches the
		// token endpoint or a diagnostic boundary.
		if (byte <= 0x20U || byte >= 0x7FU) {
			return false;
		}
	}

	return true;
}

GoogleOAuthProviderError classifyProviderError(const QString &error) noexcept
{
	if (error == QStringLiteral("invalid_request")) {
		return GoogleOAuthProviderError::InvalidRequest;
	}
	if (error == QStringLiteral("access_denied")) {
		return GoogleOAuthProviderError::AccessDenied;
	}
	if (error == QStringLiteral("server_error")) {
		return GoogleOAuthProviderError::ServerError;
	}
	if (error == QStringLiteral("temporarily_unavailable")) {
		return GoogleOAuthProviderError::TemporarilyUnavailable;
	}
	return GoogleOAuthProviderError::Unknown;
}

GoogleOAuthCallbackResult callbackFailure(GoogleOAuthCallbackStatus status) noexcept
{
	GoogleOAuthCallbackResult result;
	result.status = status;
	return result;
}

} // namespace

bool isValidGoogleOAuthClientId(const QString &clientId) noexcept
{
	return !clientId.isEmpty() && clientId.size() <= kGoogleOAuthMaxClientIdCharacters &&
	       !hasWhitespaceOrControlCharacter(clientId);
}

QByteArray googleOAuthBase64UrlNoPadding(const QByteArray &bytes)
{
	return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QByteArray googleOAuthPkceS256Challenge(const QByteArray &codeVerifier)
{
	if (codeVerifier.isEmpty()) {
		return {};
	}

	QByteArray digest = QCryptographicHash::hash(codeVerifier, QCryptographicHash::Sha256);
	QByteArray challenge = googleOAuthBase64UrlNoPadding(digest);
	wipe(digest);
	return challenge;
}

SecureBuffer generateGoogleOAuthRandomToken()
{
	std::array<quint32, kRandomWordCount> randomWords{};
	QRandomGenerator *generator = QRandomGenerator::system();
	if (generator == nullptr) {
		return {};
	}

	// QRandomGenerator::system() is the cryptographic system source.  Eight
	// words are exactly 32 bytes, which become a 43-character unpadded token.
	generator->generate(randomWords.data(), randomWords.data() + randomWords.size());
	QByteArray randomBytes(reinterpret_cast<const char *>(randomWords.data()),
			       static_cast<qsizetype>(kGoogleOAuthRandomBytes));
	QByteArray encoded = googleOAuthBase64UrlNoPadding(randomBytes);

	SecureBuffer result = SecureBuffer::copyOf(std::string_view(encoded.constData(),
									 static_cast<std::size_t>(encoded.size())));
	wipe(encoded);
	wipe(randomBytes);
	wipe(randomWords);
	return result;
}

SecureBuffer generateGoogleOAuthPkceVerifier()
{
	return generateGoogleOAuthRandomToken();
}

SecureBuffer generateGoogleOAuthState()
{
	return generateGoogleOAuthRandomToken();
}

GoogleOAuthAuthorizationResult makeGoogleOAuthAuthorizationRequest(
	const QString &clientId, int port, GoogleOAuthConsentMode consentMode)
{
	GoogleOAuthAuthorizationResult result;
	if (!isValidGoogleOAuthClientId(clientId)) {
		result.status = GoogleOAuthRequestStatus::InvalidClientId;
		return result;
	}

	if (port < 1 || port > std::numeric_limits<quint16>::max()) {
		result.status = GoogleOAuthRequestStatus::InvalidRedirectPort;
		return result;
	}

	SecureBuffer verifier = generateGoogleOAuthPkceVerifier();
	SecureBuffer state = generateGoogleOAuthState();
	if (verifier.empty() || state.empty()) {
		result.status = GoogleOAuthRequestStatus::RandomGenerationFailed;
		return result;
	}

	QByteArray verifierBytes = QByteArray::fromRawData(reinterpret_cast<const char *>(verifier.data()),
									static_cast<qsizetype>(verifier.size()));
	QByteArray challenge = googleOAuthPkceS256Challenge(verifierBytes);
	verifierBytes.clear();
	if (challenge.isEmpty()) {
		result.status = GoogleOAuthRequestStatus::RandomGenerationFailed;
		return result;
	}

	const QUrl redirectUri = makeLoopbackRedirectUri(port);
	if (!isExactLoopbackRedirectUri(redirectUri)) {
		wipe(challenge);
		result.status = GoogleOAuthRequestStatus::InvalidRedirectPort;
		return result;
	}

	QUrl authorizationUrl(QString::fromLatin1(kGoogleOAuthAuthorizationEndpoint));
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("client_id"), clientId);
	query.addQueryItem(QStringLiteral("redirect_uri"), redirectUri.toString(QUrl::FullyDecoded));
	query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	query.addQueryItem(QStringLiteral("scope"), QString::fromLatin1(kGoogleOAuthYouTubeReadonlyScope));
	query.addQueryItem(QStringLiteral("access_type"), QStringLiteral("offline"));
	if (consentMode == GoogleOAuthConsentMode::ForceConsent) {
		// Reconnection after a locally lost/replaced refresh token must be able
		// to request a newly issued token. Normal first-time authorization does
		// not force the consent screen.
		query.addQueryItem(QStringLiteral("prompt"), QStringLiteral("consent"));
	}
	query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
	query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
	query.addQueryItem(QStringLiteral("state"),
				  QString::fromLatin1(reinterpret_cast<const char *>(state.data()),
							     static_cast<qsizetype>(state.size())));
	authorizationUrl.setQuery(query);
	wipe(challenge);

	if (!authorizationUrl.isValid()) {
		result.status = GoogleOAuthRequestStatus::RandomGenerationFailed;
		return result;
	}

	result.status = GoogleOAuthRequestStatus::Success;
	result.request.authorizationUrl = std::move(authorizationUrl);
	result.request.redirectUri = redirectUri;
	result.request.codeVerifier = std::move(verifier);
	result.request.state = std::move(state);
	return result;
}

GoogleOAuthCallbackResult parseGoogleOAuthCallback(
	const GoogleOAuthAuthorizationRequest &request, const QUrl &callbackUrl)
{
	if (!request.isValid()) {
		return callbackFailure(GoogleOAuthCallbackStatus::InvalidCallbackUrl);
	}
	return parseGoogleOAuthCallback(request.redirectUri, request.state, callbackUrl);
}

GoogleOAuthCallbackResult parseGoogleOAuthCallback(
	const QUrl &expectedRedirectUri, const SecureBuffer &expectedState, const QUrl &callbackUrl)
{
	if (!isExactLoopbackRedirectUri(expectedRedirectUri) || !isBase64UrlToken(expectedState.view())) {
		return callbackFailure(GoogleOAuthCallbackStatus::InvalidCallbackUrl);
	}
	if (!callbackUrl.isValid() || callbackUrl.isRelative()) {
		return callbackFailure(GoogleOAuthCallbackStatus::InvalidCallbackUrl);
	}
	QByteArray encodedCallback = callbackUrl.toEncoded(QUrl::FullyEncoded);
	const bool callbackTooLarge = encodedCallback.size() > kGoogleOAuthMaxCallbackUrlBytes;
	wipe(encodedCallback);
	if (callbackTooLarge) {
		return callbackFailure(GoogleOAuthCallbackStatus::InvalidQuery);
	}
	if (callbackUrl.hasFragment()) {
		return callbackFailure(GoogleOAuthCallbackStatus::FragmentNotAllowed);
	}
	if (!callbackUrl.userInfo().isEmpty()) {
		return callbackFailure(GoogleOAuthCallbackStatus::UserInfoNotAllowed);
	}
	if (callbackUrl.scheme() != expectedRedirectUri.scheme() ||
	    callbackUrl.host(QUrl::FullyEncoded) != expectedRedirectUri.host(QUrl::FullyEncoded) ||
	    callbackUrl.port() != expectedRedirectUri.port()) {
		return callbackFailure(GoogleOAuthCallbackStatus::OriginMismatch);
	}
	if (callbackUrl.path(QUrl::FullyEncoded) != expectedRedirectUri.path(QUrl::FullyEncoded)) {
		return callbackFailure(GoogleOAuthCallbackStatus::PathMismatch);
	}

	const QUrlQuery query(callbackUrl);
	const QList<QPair<QString, QString>> items = query.queryItems(QUrl::FullyDecoded);
	if (items.size() > kGoogleOAuthMaxCallbackParameters) {
		return callbackFailure(GoogleOAuthCallbackStatus::InvalidQuery);
	}
	QSet<QString> seenNames;
	QString state;
	QString code;
	QString error;
	bool hasState = false;
	bool hasCode = false;
	bool hasError = false;

	for (const auto &item : items) {
		if (item.first.isEmpty() || item.first.size() > kGoogleOAuthMaxCallbackNameCharacters ||
		    item.second.size() > kGoogleOAuthMaxCallbackValueCharacters) {
			return callbackFailure(GoogleOAuthCallbackStatus::InvalidQuery);
		}
		if (seenNames.contains(item.first)) {
			return callbackFailure(GoogleOAuthCallbackStatus::DuplicateQueryParameter);
		}
		seenNames.insert(item.first);

		if (item.first == QStringLiteral("state")) {
			hasState = true;
			state = item.second;
		} else if (item.first == QStringLiteral("code")) {
			hasCode = true;
			code = item.second;
		} else if (item.first == QStringLiteral("error")) {
			hasError = true;
			error = item.second;
		}
	}

	if (!hasState) {
		return callbackFailure(GoogleOAuthCallbackStatus::MissingState);
	}

	QByteArray stateBytes = state.toUtf8();
	const bool stateMatches = constantTimeEquals(stateBytes, expectedState.view());
	wipe(stateBytes);
	if (!stateMatches) {
		return callbackFailure(GoogleOAuthCallbackStatus::StateMismatch);
	}

	if (hasError) {
		if (hasCode) {
			return callbackFailure(GoogleOAuthCallbackStatus::CodeAndError);
		}
		if (error.isEmpty()) {
			return callbackFailure(GoogleOAuthCallbackStatus::InvalidQuery);
		}

		GoogleOAuthCallbackResult result = callbackFailure(GoogleOAuthCallbackStatus::ProviderError);
		result.providerError = classifyProviderError(error);
		return result;
	}

	if (!hasCode || code.isEmpty()) {
		return callbackFailure(GoogleOAuthCallbackStatus::MissingCode);
	}

	QByteArray codeBytes = code.toUtf8();
	if (!isSafeAuthorizationCode(codeBytes)) {
		wipe(codeBytes);
		return callbackFailure(GoogleOAuthCallbackStatus::InvalidCode);
	}

	GoogleOAuthCallbackResult result = callbackFailure(GoogleOAuthCallbackStatus::Success);
	result.authorizationCode = SecureBuffer::copyOf(
		std::string_view(codeBytes.constData(), static_cast<std::size_t>(codeBytes.size())));
	wipe(codeBytes);
	return result;
}

} // namespace easy_multistream

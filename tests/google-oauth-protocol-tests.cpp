// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "google-oauth-protocol.hpp"

#include <QCoreApplication>
#include <QUrlQuery>

#include <algorithm>
#include <iostream>

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
using easy_multistream::GoogleOAuthConsentMode;
using easy_multistream::GoogleOAuthProviderError;
using easy_multistream::GoogleOAuthRequestStatus;
using easy_multistream::SecureBuffer;

QString expectedStateQuery(const QUrl &url)
{
	const QUrlQuery query(url);
	return query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
}

void testRfc7636KnownVector()
{
	// RFC 7636 section 4.2/4.6 verifier and S256 challenge.
	const QByteArray verifier("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
	CHECK(easy_multistream::googleOAuthPkceS256Challenge(verifier) ==
	      QByteArray("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
}

void testAuthorizationRequest()
{
	const auto result = easy_multistream::makeGoogleOAuthAuthorizationRequest(
		QStringLiteral("client-id.apps.googleusercontent.com"), 43123);
	CHECK(result.status == GoogleOAuthRequestStatus::Success);
	CHECK(result.succeeded());
	CHECK(result.request.redirectUri == QUrl(QStringLiteral("http://127.0.0.1:43123/callback")));
	CHECK(result.request.codeVerifier.size() == 43U);
	CHECK(result.request.state.size() == 43U);
	const auto isBase64Url = [](unsigned char value) {
		return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
		       (value >= '0' && value <= '9') || value == '-' || value == '_';
	};
	CHECK(std::all_of(result.request.codeVerifier.view().begin(), result.request.codeVerifier.view().end(),
			  isBase64Url));
	CHECK(std::all_of(result.request.state.view().begin(), result.request.state.view().end(), isBase64Url));
	CHECK(result.request.authorizationUrl.scheme() == QStringLiteral("https"));
	CHECK(result.request.authorizationUrl.host() == QStringLiteral("accounts.google.com"));
	CHECK(result.request.authorizationUrl.path() == QStringLiteral("/o/oauth2/v2/auth"));

	const QUrlQuery query(result.request.authorizationUrl);
	CHECK(query.queryItemValue(QStringLiteral("client_id"), QUrl::FullyDecoded) ==
	      QStringLiteral("client-id.apps.googleusercontent.com"));
	CHECK(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded) ==
	      QStringLiteral("http://127.0.0.1:43123/callback"));
	CHECK(query.queryItemValue(QStringLiteral("response_type"), QUrl::FullyDecoded) == QStringLiteral("code"));
	CHECK(query.queryItemValue(QStringLiteral("scope"), QUrl::FullyDecoded) ==
	      QStringLiteral("https://www.googleapis.com/auth/youtube.readonly"));
	CHECK(query.queryItemValue(QStringLiteral("access_type"), QUrl::FullyDecoded) == QStringLiteral("offline"));
	CHECK(!query.hasQueryItem(QStringLiteral("prompt")));
	CHECK(query.queryItemValue(QStringLiteral("code_challenge_method"), QUrl::FullyDecoded) == QStringLiteral("S256"));

	const QByteArray verifierBytes(reinterpret_cast<const char *>(result.request.codeVerifier.data()),
					      static_cast<qsizetype>(result.request.codeVerifier.size()));
	CHECK(query.queryItemValue(QStringLiteral("code_challenge"), QUrl::FullyDecoded) ==
	      QString::fromLatin1(easy_multistream::googleOAuthPkceS256Challenge(verifierBytes)));
	CHECK(query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded).toUtf8() ==
	      QByteArray(reinterpret_cast<const char *>(result.request.state.data()),
			 static_cast<qsizetype>(result.request.state.size())));
}

void testReconnectAuthorizationForcesConsent()
{
	const auto result = easy_multistream::makeGoogleOAuthAuthorizationRequest(
		QStringLiteral("client-id.apps.googleusercontent.com"), 43123, GoogleOAuthConsentMode::ForceConsent);
	CHECK(result.succeeded());
	CHECK(QUrlQuery(result.request.authorizationUrl).queryItemValue(QStringLiteral("prompt"), QUrl::FullyDecoded) ==
	      QStringLiteral("consent"));
}

void testRequestInputValidation()
{
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(
		      QString(easy_multistream::kGoogleOAuthMaxClientIdCharacters, QLatin1Char('a')), 43123)
		      .status == GoogleOAuthRequestStatus::Success);
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(QString(), 43123).status ==
	      GoogleOAuthRequestStatus::InvalidClientId);
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(QStringLiteral("client\n-id"), 43123).status ==
	      GoogleOAuthRequestStatus::InvalidClientId);
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(QStringLiteral("client id"), 43123).status ==
	      GoogleOAuthRequestStatus::InvalidClientId);
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(
		      QString(easy_multistream::kGoogleOAuthMaxClientIdCharacters + 1, QLatin1Char('a')), 43123)
		      .status == GoogleOAuthRequestStatus::InvalidClientId);
	QString nulClient(QStringLiteral("client"));
	nulClient.append(QChar::Null);
	nulClient.append(QStringLiteral("-id"));
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(nulClient, 43123).status ==
	      GoogleOAuthRequestStatus::InvalidClientId);
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(QStringLiteral("client-id"), 0).status ==
	      GoogleOAuthRequestStatus::InvalidRedirectPort);
	CHECK(easy_multistream::makeGoogleOAuthAuthorizationRequest(QStringLiteral("client-id"), 65536).status ==
	      GoogleOAuthRequestStatus::InvalidRedirectPort);
}

QUrl callbackFor(const easy_multistream::GoogleOAuthAuthorizationRequest &request, const QString &queryString)
{
	QUrl callback = request.redirectUri;
	callback.setQuery(queryString);
	return callback;
}

void testCallbackSuccessAndSecretBoundary()
{
	const auto requestResult = easy_multistream::makeGoogleOAuthAuthorizationRequest(QStringLiteral("client-id"), 12345);
	const auto &request = requestResult.request;
	const QString state = expectedStateQuery(request.authorizationUrl);
	const auto callback = easy_multistream::parseGoogleOAuthCallback(
		request, callbackFor(request, QStringLiteral("code=one-time-code&state=") + state));
	CHECK(callback.status == GoogleOAuthCallbackStatus::Success);
	CHECK(callback.succeeded());
	CHECK(callback.authorizationCode.view() == "one-time-code");
	CHECK(callback.providerError == GoogleOAuthProviderError::None);
}

void testCallbackValidationFailures()
{
	const auto requestResult = easy_multistream::makeGoogleOAuthAuthorizationRequest(QStringLiteral("client-id"), 12345);
	const auto &request = requestResult.request;
	const QString state = expectedStateQuery(request.authorizationUrl);
	const QString validQuery = QStringLiteral("code=one-time-code&state=") + state;

	QUrl wrongOrigin = callbackFor(request, validQuery);
	wrongOrigin.setHost(QStringLiteral("localhost"));
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, wrongOrigin).status ==
	      GoogleOAuthCallbackStatus::OriginMismatch);

	QUrl wrongScheme = callbackFor(request, validQuery);
	wrongScheme.setScheme(QStringLiteral("https"));
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, wrongScheme).status ==
	      GoogleOAuthCallbackStatus::OriginMismatch);

	QUrl wrongPort = callbackFor(request, validQuery);
	wrongPort.setPort(12346);
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, wrongPort).status ==
	      GoogleOAuthCallbackStatus::OriginMismatch);

	QUrl wrongPath = callbackFor(request, validQuery);
	wrongPath.setPath(QStringLiteral("/wrong"));
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, wrongPath).status ==
	      GoogleOAuthCallbackStatus::PathMismatch);

	QUrl fragment = callbackFor(request, validQuery);
	fragment.setFragment(QStringLiteral("fragment"));
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, fragment).status ==
	      GoogleOAuthCallbackStatus::FragmentNotAllowed);

	QUrl userInfo = callbackFor(request, validQuery);
	userInfo.setUserName(QStringLiteral("attacker"));
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, userInfo).status ==
	      GoogleOAuthCallbackStatus::UserInfoNotAllowed);

	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=one&code=two&state=") + state))
		      .status == GoogleOAuthCallbackStatus::DuplicateQueryParameter);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=one&state=") + state + QStringLiteral("&state=other")))
		      .status == GoogleOAuthCallbackStatus::DuplicateQueryParameter);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("state=") + state))
		      .status == GoogleOAuthCallbackStatus::MissingCode);
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, callbackFor(request, QStringLiteral("code=one"))).status ==
	      GoogleOAuthCallbackStatus::MissingState);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("error=access_denied&state=") + state))
		      .status == GoogleOAuthCallbackStatus::ProviderError);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("error=access_denied&state=") + state))
		      .providerError == GoogleOAuthProviderError::AccessDenied);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=one&error=access_denied&state=") + state))
		      .status == GoogleOAuthCallbackStatus::CodeAndError);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("error=&state=") + state))
		      .status == GoogleOAuthCallbackStatus::InvalidQuery);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=one%20two&state=") + state))
		      .status == GoogleOAuthCallbackStatus::InvalidCode);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("=value&code=one&state=") + state))
		      .status == GoogleOAuthCallbackStatus::InvalidQuery);

	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=one&state=wrong")))
		      .status == GoogleOAuthCallbackStatus::StateMismatch);

	easy_multistream::GoogleOAuthAuthorizationRequest incompleteRequest;
	incompleteRequest.redirectUri = request.redirectUri;
	CHECK(easy_multistream::parseGoogleOAuthCallback(incompleteRequest, callbackFor(request, validQuery)).status ==
	      GoogleOAuthCallbackStatus::InvalidCallbackUrl);

	const SecureBuffer malformedState = SecureBuffer::copyOf("not-a-complete-state");
	CHECK(easy_multistream::parseGoogleOAuthCallback(request.redirectUri, malformedState,
							 callbackFor(request, validQuery))
		      .status == GoogleOAuthCallbackStatus::InvalidCallbackUrl);
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=one&state=") + state +
						    QStringLiteral("&extra=one&extra=two")))
		      .status == GoogleOAuthCallbackStatus::DuplicateQueryParameter);

	QString tooManyParameters = QStringLiteral("code=one&state=") + state;
	for (int index = 0; index < easy_multistream::kGoogleOAuthMaxCallbackParameters; ++index) {
		tooManyParameters += QStringLiteral("&p%1=x").arg(index);
	}
	CHECK(easy_multistream::parseGoogleOAuthCallback(request, callbackFor(request, tooManyParameters)).status ==
	      GoogleOAuthCallbackStatus::InvalidQuery);

	const QString oversizedCode(easy_multistream::kGoogleOAuthMaxCallbackValueCharacters + 1, QLatin1Char('a'));
	CHECK(easy_multistream::parseGoogleOAuthCallback(
		       request, callbackFor(request, QStringLiteral("code=") + oversizedCode + QStringLiteral("&state=") + state))
		      .status == GoogleOAuthCallbackStatus::InvalidQuery);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testRfc7636KnownVector();
	testAuthorizationRequest();
	testReconnectAuthorizationForcesConsent();
	testRequestInputValidation();
	testCallbackSuccessAndSecretBoundary();
	testCallbackValidationFailures();
	return failures == 0 ? 0 : 1;
}

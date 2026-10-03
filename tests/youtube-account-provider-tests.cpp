// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-provider.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace easy_multistream {

class YouTubeAccountProviderTestAccess final {
public:
	YouTubeAccountProviderTestAccess(std::unique_ptr<YouTubeAccountAuthorizationPort> authorization,
					 std::unique_ptr<YouTubeAccountTokenPort> token,
					 std::unique_ptr<YouTubeAccountDiscoveryPort> discovery,
					 YouTubeAccountRefreshTokenVault &vault,
					 YouTubeAccountSelectionCommitter committer,
					 QString clientId = QStringLiteral("test-client-id"))
		: provider_(new YouTubeAccountProvider(std::move(clientId), std::move(authorization), std::move(token),
						       std::move(discovery), vault, std::move(committer), nullptr))
	{
	}

	YouTubeAccountProviderStartStatus
	startConnection(GoogleOAuthConsentMode mode = GoogleOAuthConsentMode::Standard)
	{
		return provider_->startConnection(mode);
	}
	YouTubeAccountProviderSelectionStatus selectChannel(YouTubeAccountLease lease, std::string_view id)
	{
		return provider_->selectChannel(lease, id);
	}
	YouTubeAccountProviderSelectionStatus selectStream(YouTubeAccountLease lease, std::string_view id)
	{
		return provider_->selectStream(lease, id);
	}
	bool cancel(YouTubeAccountLease lease) noexcept { return provider_->cancel(lease); }
	bool invalidateContext() noexcept { return provider_->invalidateContext(); }
	bool shutdown() noexcept { return provider_->shutdown(); }
	YouTubeAccountProviderSnapshot snapshot() const { return provider_->snapshot(); }

private:
	std::unique_ptr<YouTubeAccountProvider> provider_;
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

using easy_multistream::CredentialError;
using easy_multistream::CredentialReadResult;
using easy_multistream::CredentialResult;
using easy_multistream::CredentialState;
using easy_multistream::CredentialStatus;
using easy_multistream::CredentialVault;
using easy_multistream::GoogleOAuthAuthorizationCompletion;
using easy_multistream::GoogleOAuthAuthorizationCompletionStatus;
using easy_multistream::GoogleOAuthAuthorizationStartStatus;
using easy_multistream::GoogleOAuthConsentMode;
using easy_multistream::GoogleOAuthLoopbackAttempt;
using easy_multistream::GoogleOAuthProviderError;
using easy_multistream::GoogleOAuthTokenAttempt;
using easy_multistream::GoogleOAuthTokenCompletion;
using easy_multistream::GoogleOAuthTokenCompletionStatus;
using easy_multistream::GoogleOAuthTokenExchangeRequest;
using easy_multistream::GoogleOAuthTokenOperation;
using easy_multistream::GoogleOAuthTokenProviderError;
using easy_multistream::GoogleOAuthTokenStartStatus;
using easy_multistream::SecureBuffer;
using easy_multistream::YouTubeAccountDiscoveryPort;
using easy_multistream::YouTubeAccountLease;
using easy_multistream::YouTubeAccountProviderSelectionStatus;
using easy_multistream::YouTubeAccountProviderSnapshot;
using easy_multistream::YouTubeAccountProviderStage;
using easy_multistream::YouTubeAccountProviderStartStatus;
using easy_multistream::YouTubeAccountRefreshTokenVault;
using easy_multistream::YouTubeApiPagedCompletion;
using easy_multistream::YouTubeApiPagerStartStatus;
using easy_multistream::YouTubeApiPagingFailure;
using easy_multistream::YouTubeApiProviderError;
using easy_multistream::YouTubeApiAttempt;
using easy_multistream::YouTubeApiCompletionStatus;
using easy_multistream::YouTubeListAllChannelsRequest;
using easy_multistream::YouTubeListAllStreamsRequest;
using easy_multistream::YouTubeOwnedChannel;
using easy_multistream::YouTubeReusableStream;
using easy_multistream::YouTubeAccountAuthorizationPort;
using easy_multistream::YouTubeAccountTokenPort;
using easy_multistream::YouTubeAccountProviderTestAccess;

constexpr char kAccessToken[] = "access-token-sentinel";
constexpr char kRefreshToken[] = "refresh-token-sentinel";
constexpr char kAuthorizationCode[] = "authorization-code-sentinel";
constexpr char kVerifier[] = "verifier-sentinel-012345678901234567890123456789012345";

bool waitUntil(const std::function<bool()> &predicate, int timeoutMilliseconds = 1000)
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

GoogleOAuthAuthorizationCompletion authorizationSuccess(GoogleOAuthLoopbackAttempt attempt)
{
	GoogleOAuthAuthorizationCompletion completion;
	completion.attempt = attempt;
	completion.status = GoogleOAuthAuthorizationCompletionStatus::Success;
	completion.redirectUri = QUrl(QStringLiteral("http://127.0.0.1:43123/callback"));
	completion.authorizationCode = SecureBuffer::copyOf(kAuthorizationCode);
	completion.codeVerifier = SecureBuffer::copyOf(kVerifier);
	return completion;
}

GoogleOAuthAuthorizationCompletion authorizationFailure(GoogleOAuthLoopbackAttempt attempt,
							GoogleOAuthProviderError error)
{
	GoogleOAuthAuthorizationCompletion completion;
	completion.attempt = attempt;
	completion.status = GoogleOAuthAuthorizationCompletionStatus::ProviderRejected;
	completion.providerError = error;
	return completion;
}

GoogleOAuthTokenCompletion tokenSuccess(GoogleOAuthTokenAttempt attempt, bool includeRefresh = true,
					std::string_view refreshToken = kRefreshToken)
{
	GoogleOAuthTokenCompletion completion;
	completion.attempt = attempt;
	completion.operation = GoogleOAuthTokenOperation::ExchangeAuthorizationCode;
	completion.status = GoogleOAuthTokenCompletionStatus::Success;
	completion.tokens.accessToken = SecureBuffer::copyOf(kAccessToken);
	if (includeRefresh) {
		completion.tokens.refreshToken = SecureBuffer::copyOf(refreshToken);
	}
	completion.tokens.expiresInSeconds = 3600;
	return completion;
}

GoogleOAuthTokenCompletion tokenFailure(GoogleOAuthTokenAttempt attempt, GoogleOAuthTokenProviderError error)
{
	GoogleOAuthTokenCompletion completion;
	completion.attempt = attempt;
	completion.operation = GoogleOAuthTokenOperation::ExchangeAuthorizationCode;
	completion.status = GoogleOAuthTokenCompletionStatus::ProviderRejected;
	completion.providerError = error;
	return completion;
}

YouTubeApiPagedCompletion channelPage(std::vector<YouTubeOwnedChannel> channels)
{
	YouTubeApiPagedCompletion completion;
	completion.operation = easy_multistream::YouTubeApiOperation::ListOwnedChannels;
	completion.status = YouTubeApiCompletionStatus::Success;
	completion.channels = std::move(channels);
	return completion;
}

YouTubeApiPagedCompletion streamPage(std::vector<YouTubeReusableStream> streams)
{
	YouTubeApiPagedCompletion completion;
	completion.operation = easy_multistream::YouTubeApiOperation::ListReusableStreams;
	completion.status = YouTubeApiCompletionStatus::Success;
	completion.streams = std::move(streams);
	return completion;
}

YouTubeApiPagedCompletion discoveryFailure(YouTubeApiAttempt attempt, YouTubeApiProviderError error)
{
	YouTubeApiPagedCompletion completion;
	completion.attempt = attempt;
	completion.operation = easy_multistream::YouTubeApiOperation::ListOwnedChannels;
	completion.status = YouTubeApiCompletionStatus::ProviderRejected;
	completion.providerError = error;
	return completion;
}

class FakeAuthorizationPort final : public YouTubeAccountAuthorizationPort {
public:
	GoogleOAuthAuthorizationStartStatus start(GoogleOAuthLoopbackAttempt attempt, const QString &clientId,
						  GoogleOAuthConsentMode consentMode,
						  CompletionHandler completionHandler) noexcept override
	{
		++startCount;
		lastAttempt = attempt;
		lastClientId = clientId;
		lastConsentMode = consentMode;
		handler = std::move(completionHandler);
		if (forcedStartStatus.has_value()) {
			return *forcedStartStatus;
		}
		return GoogleOAuthAuthorizationStartStatus::Started;
	}

	bool cancel(GoogleOAuthLoopbackAttempt attempt) noexcept override
	{
		++cancelCount;
		lastCancelled = attempt;
		handler = {};
		return true;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		handler = {};
		return true;
	}

	void complete(GoogleOAuthAuthorizationCompletion completion)
	{
		auto callback = std::move(handler);
		handler = {};
		if (callback) {
			callback(std::move(completion));
		}
	}

	int startCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	std::optional<GoogleOAuthLoopbackAttempt> lastAttempt;
	std::optional<GoogleOAuthLoopbackAttempt> lastCancelled;
	QString lastClientId;
	GoogleOAuthConsentMode lastConsentMode = GoogleOAuthConsentMode::Standard;
	std::optional<GoogleOAuthAuthorizationStartStatus> forcedStartStatus;
	CompletionHandler handler;
};

class FakeTokenPort final : public YouTubeAccountTokenPort {
public:
	GoogleOAuthTokenStartStatus startExchange(GoogleOAuthTokenExchangeRequest request,
						  CompletionHandler completionHandler) noexcept override
	{
		++startCount;
		lastAttempt = request.attempt;
		lastClientId = request.clientId;
		lastCode = request.authorizationCode.view().data() == nullptr
				   ? std::string{}
				   : std::string(request.authorizationCode.view());
		lastVerifier = request.codeVerifier.view().data() == nullptr ? std::string{}
									     : std::string(request.codeVerifier.view());
		handler = std::move(completionHandler);
		if (forcedStartStatus.has_value()) {
			return *forcedStartStatus;
		}
		return GoogleOAuthTokenStartStatus::Started;
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override
	{
		++cancelCount;
		lastCancelled = attempt;
		handler = {};
		return true;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		handler = {};
		return true;
	}

	void complete(GoogleOAuthTokenCompletion completion)
	{
		auto callback = std::move(handler);
		handler = {};
		if (callback) {
			callback(std::move(completion));
		}
	}

	int startCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	std::optional<GoogleOAuthTokenAttempt> lastAttempt;
	std::optional<GoogleOAuthTokenAttempt> lastCancelled;
	QString lastClientId;
	std::string lastCode;
	std::string lastVerifier;
	std::optional<GoogleOAuthTokenStartStatus> forcedStartStatus;
	CompletionHandler handler;
};

class FakeDiscoveryPort final : public YouTubeAccountDiscoveryPort {
public:
	YouTubeApiPagerStartStatus startListAllChannels(YouTubeListAllChannelsRequest request,
							CompletionHandler completionHandler) noexcept override
	{
		++channelStartCount;
		lastChannelAttempt = request.attempt;
		lastChannelAccessToken = std::string(request.accessToken.view());
		channelHandler = std::move(completionHandler);
		if (forcedChannelStartStatus.has_value()) {
			return *forcedChannelStartStatus;
		}
		return YouTubeApiPagerStartStatus::Started;
	}

	YouTubeApiPagerStartStatus startListAllStreams(YouTubeListAllStreamsRequest request,
						       CompletionHandler completionHandler) noexcept override
	{
		++streamStartCount;
		lastStreamAttempt = request.attempt;
		lastStreamAccessToken = std::string(request.accessToken.view());
		lastStreamChannelId = request.channelId;
		streamHandler = std::move(completionHandler);
		if (forcedStreamStartStatus.has_value()) {
			return *forcedStreamStartStatus;
		}
		return YouTubeApiPagerStartStatus::Started;
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept override
	{
		++cancelCount;
		lastCancelled = attempt;
		channelHandler = {};
		streamHandler = {};
		return true;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		channelHandler = {};
		streamHandler = {};
		return true;
	}

	void completeChannels(YouTubeApiPagedCompletion completion)
	{
		if (!completion.attempt.isValid() && lastChannelAttempt.has_value()) {
			completion.attempt = *lastChannelAttempt;
		}
		auto callback = std::move(channelHandler);
		channelHandler = {};
		if (callback) {
			callback(std::move(completion));
		}
	}

	void completeStreams(YouTubeApiPagedCompletion completion)
	{
		if (!completion.attempt.isValid() && lastStreamAttempt.has_value()) {
			completion.attempt = *lastStreamAttempt;
		}
		auto callback = std::move(streamHandler);
		streamHandler = {};
		if (callback) {
			callback(std::move(completion));
		}
	}

	int channelStartCount = 0;
	int streamStartCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	std::optional<YouTubeApiAttempt> lastChannelAttempt;
	std::optional<YouTubeApiAttempt> lastStreamAttempt;
	std::optional<YouTubeApiAttempt> lastCancelled;
	std::string lastChannelAccessToken;
	std::string lastStreamAccessToken;
	std::string lastStreamChannelId;
	std::optional<YouTubeApiPagerStartStatus> forcedChannelStartStatus;
	std::optional<YouTubeApiPagerStartStatus> forcedStreamStartStatus;
	CompletionHandler channelHandler;
	CompletionHandler streamHandler;
};

class FakeCredentialVault final : public YouTubeAccountRefreshTokenVault {
public:
	CredentialResult write(std::string_view value) noexcept override
	{
		++writeCount;
		lastWritten = SecureBuffer::copyOf(value);
		if (writeError != CredentialError::None ||
		    (failWriteOnCall.has_value() && writeCount == *failWriteOnCall)) {
			return {writeError == CredentialError::None ? CredentialError::Unavailable : writeError, 1};
		}
		stored = SecureBuffer::copyOf(value);
		return {};
	}

	CredentialReadResult read() noexcept override
	{
		++readCount;
		if (readError != CredentialError::None) {
			return {{readError, 1}, {}};
		}
		if (stored.empty()) {
			return {{CredentialError::NotFound, 0}, {}};
		}
		return {{}, SecureBuffer::copyOf(stored.view())};
	}

	CredentialResult erase() noexcept override
	{
		++eraseCount;
		stored.clear();
		if (eraseError != CredentialError::None) {
			return {eraseError, 1};
		}
		return {};
	}

	CredentialStatus status() noexcept override
	{
		if (statusState == CredentialState::Present) {
			return {CredentialState::Present, {}};
		}
		if (statusState == CredentialState::Missing) {
			return {CredentialState::Missing, {CredentialError::NotFound, 0}};
		}
		return {CredentialState::Unavailable, {CredentialError::Unavailable, 1}};
	}

	int writeCount = 0;
	int readCount = 0;
	int eraseCount = 0;
	SecureBuffer stored;
	SecureBuffer lastWritten;
	CredentialError writeError = CredentialError::None;
	std::optional<int> failWriteOnCall;
	CredentialError readError = CredentialError::None;
	CredentialError eraseError = CredentialError::None;
	CredentialState statusState = CredentialState::Missing;
};

struct Fixture final {
	std::unique_ptr<FakeAuthorizationPort> authorization;
	std::unique_ptr<FakeTokenPort> token;
	std::unique_ptr<FakeDiscoveryPort> discovery;
	FakeCredentialVault vault;
	bool commitResult = true;
	int commitCount = 0;
	std::optional<easy_multistream::YouTubeAccountSelection> committedSelection;
	std::unique_ptr<YouTubeAccountProviderTestAccess> provider;

	Fixture()
	{
		authorization = std::make_unique<FakeAuthorizationPort>();
		token = std::make_unique<FakeTokenPort>();
		discovery = std::make_unique<FakeDiscoveryPort>();
		FakeAuthorizationPort *authorizationPointer = authorization.get();
		FakeTokenPort *tokenPointer = token.get();
		FakeDiscoveryPort *discoveryPointer = discovery.get();
		provider = std::make_unique<YouTubeAccountProviderTestAccess>(
			std::move(authorization), std::move(token), std::move(discovery), vault,
			[this](const easy_multistream::YouTubeAccountSelection &selection) {
				++commitCount;
				if (commitResult) {
					committedSelection = selection;
				}
				return commitResult;
			});
		// Keep the fake pointers accessible through the provider's port ownership
		// only in tests. They remain valid until provider destruction.
		this->authorizationPointer = authorizationPointer;
		this->tokenPointer = tokenPointer;
		this->discoveryPointer = discoveryPointer;
	}

	FakeAuthorizationPort *authorizationPointer = nullptr;
	FakeTokenPort *tokenPointer = nullptr;
	FakeDiscoveryPort *discoveryPointer = nullptr;
};

void completeAuthorization(Fixture &fixture)
{
	CHECK(fixture.authorizationPointer != nullptr);
	const auto attempt = fixture.authorizationPointer->lastAttempt;
	CHECK(attempt.has_value());
	if (attempt.has_value()) {
		fixture.authorizationPointer->complete(authorizationSuccess(*attempt));
	}
}

void completeToken(Fixture &fixture, bool includeRefresh = true, std::string_view refreshToken = kRefreshToken)
{
	CHECK(fixture.tokenPointer != nullptr);
	const auto attempt = fixture.tokenPointer->lastAttempt;
	CHECK(attempt.has_value());
	if (attempt.has_value()) {
		fixture.tokenPointer->complete(tokenSuccess(*attempt, includeRefresh, refreshToken));
	}
}

YouTubeAccountLease activeLease(const Fixture &fixture)
{
	return fixture.provider->snapshot().account.lease.value_or(YouTubeAccountLease{});
}

void startAndReachChannelListing(Fixture &fixture)
{
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Authorizing);
	completeAuthorization(fixture);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::ExchangingCode);
	completeToken(fixture);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::ListingChannels);
}

void testSingleCandidateAutoSelectionAndSecretFreeSnapshot()
{
	Fixture fixture;
	CHECK(fixture.provider->startConnection(GoogleOAuthConsentMode::ForceConsent) ==
	      YouTubeAccountProviderStartStatus::Started);
	CHECK(fixture.authorizationPointer->lastConsentMode == GoogleOAuthConsentMode::ForceConsent);
	completeAuthorization(fixture);
	CHECK(fixture.tokenPointer->startCount == 1);
	CHECK(fixture.tokenPointer->lastCode == kAuthorizationCode);
	CHECK(fixture.tokenPointer->lastVerifier == kVerifier);
	completeToken(fixture);
	CHECK(fixture.discoveryPointer->channelStartCount == 1);
	CHECK(fixture.discoveryPointer->lastChannelAccessToken == kAccessToken);

	const YouTubeAccountLease lease = activeLease(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({YouTubeOwnedChannel{"channel-one", "Channel One"}}));
	CHECK(fixture.discoveryPointer->streamStartCount == 1);
	CHECK(fixture.discoveryPointer->lastStreamChannelId == "channel-one");
	fixture.discoveryPointer->completeStreams(
		streamPage({YouTubeReusableStream{"stream-one", "channel-one", "Everyday"}}));

	const YouTubeAccountProviderSnapshot snapshot = fixture.provider->snapshot();
	CHECK(snapshot.stage == YouTubeAccountProviderStage::Connected);
	CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Connected);
	CHECK(snapshot.account.channelId == "channel-one");
	CHECK(snapshot.account.streamId == "stream-one");
	CHECK(!snapshot.selectedChannelId.has_value());
	CHECK(!snapshot.selectedStreamId.has_value());
	CHECK(fixture.vault.writeCount == 1);
	CHECK(fixture.vault.stored.view() == kRefreshToken);
	CHECK(fixture.commitCount == 1);
	CHECK(fixture.committedSelection.has_value());
	CHECK(fixture.committedSelection->channelId == "channel-one");
	CHECK(fixture.committedSelection->streamId == "stream-one");
	CHECK(snapshot.account.channelId.find(kAccessToken) == std::string::npos);
	CHECK(snapshot.account.streamId.find(kRefreshToken) == std::string::npos);
	CHECK(snapshot.account.channelLabel.find(kAuthorizationCode) == std::string::npos);
	CHECK(snapshot.account.streamLabel.find(kVerifier) == std::string::npos);
	CHECK(lease == snapshot.account.connectionLease.value_or(YouTubeAccountLease{}));
}

void testMultipleCandidatesRequireExactIdSelection()
{
	Fixture fixture;
	startAndReachChannelListing(fixture);
	const YouTubeAccountLease lease = activeLease(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({
		{"channel-a", "Same title"},
		{"channel-b", "Same title"},
	}));
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::AwaitingChannelSelection);
	CHECK(fixture.provider->selectChannel(lease, "missing") ==
	      YouTubeAccountProviderSelectionStatus::UnknownCandidate);
	CHECK(fixture.discoveryPointer->streamStartCount == 0);
	CHECK(fixture.provider->selectChannel(lease, "channel-b") == YouTubeAccountProviderSelectionStatus::Accepted);
	CHECK(fixture.discoveryPointer->lastStreamChannelId == "channel-b");
	fixture.discoveryPointer->completeStreams(streamPage({
		{"stream-a", "channel-b", "Same title"},
		{"stream-b", "channel-b", "Same title"},
	}));
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::AwaitingStreamSelection);
	CHECK(fixture.provider->selectStream(lease, "missing") ==
	      YouTubeAccountProviderSelectionStatus::UnknownCandidate);
	CHECK(fixture.vault.writeCount == 0);
	CHECK(fixture.provider->selectStream(lease, "stream-b") == YouTubeAccountProviderSelectionStatus::Accepted);
	CHECK(fixture.provider->snapshot().account.streamId == "stream-b");
	CHECK(fixture.provider->snapshot().account.channelId == "channel-b");
	CHECK(fixture.vault.writeCount == 1);
}

void testEmptyCandidatesAndInvalidSelectionDoNotPersist()
{
	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		const YouTubeAccountLease lease = activeLease(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::DiscoveryFailed);
		CHECK(fixture.vault.writeCount == 0);
		CHECK(fixture.provider->selectChannel(lease, "anything") ==
		      YouTubeAccountProviderSelectionStatus::StaleLease);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		const YouTubeAccountLease lease = activeLease(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::ListingStreams);
		fixture.discoveryPointer->completeStreams(streamPage({}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::NoCompatibleStream);
		CHECK(fixture.vault.writeCount == 0);
	}
}

void testMalformedCandidatesFailClosed()
{
	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", ""}}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::InvalidResponse);
		CHECK(fixture.vault.writeCount == 0);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({
			{"channel", "Channel"},
			{"channel", "Duplicate"},
		}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::InvalidResponse);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({
			{"stream", "other-channel", "Wrong channel"},
		}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::InvalidResponse);
		CHECK(fixture.vault.writeCount == 0);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({
			{"stream", "channel", "One"},
			{"stream", "channel", "Duplicate"},
		}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::InvalidResponse);
		CHECK(fixture.vault.writeCount == 0);
	}
}

void testAuthorizationTokenAndDiscoveryFailuresAreSanitized()
{
	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		const auto attempt = fixture.authorizationPointer->lastAttempt;
		CHECK(attempt.has_value());
		fixture.authorizationPointer->complete(
			authorizationFailure(*attempt, GoogleOAuthProviderError::AccessDenied));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::AuthorizationRejected);
		CHECK(fixture.tokenPointer->startCount == 0);
		CHECK(fixture.vault.writeCount == 0);
	}

	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		const auto attempt = fixture.tokenPointer->lastAttempt;
		CHECK(attempt.has_value());
		fixture.tokenPointer->complete(tokenFailure(*attempt, GoogleOAuthTokenProviderError::InvalidGrant));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::TokenExchangeFailed);
		CHECK(fixture.discoveryPointer->channelStartCount == 0);
		CHECK(fixture.vault.writeCount == 0);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		const YouTubeAccountLease lease = activeLease(fixture);
		fixture.discoveryPointer->completeChannels(discoveryFailure(
			*fixture.discoveryPointer->lastChannelAttempt, YouTubeApiProviderError::RateLimited));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::ServiceUnavailable);
		CHECK(fixture.provider->selectChannel(lease, "channel") ==
		      YouTubeAccountProviderSelectionStatus::StaleLease);
		CHECK(fixture.vault.writeCount == 0);
	}
}

void connectFixture(Fixture &fixture, std::string_view channelId = "old-channel",
		    std::string_view streamId = "old-stream")
{
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	completeAuthorization(fixture);
	completeToken(fixture);
	const YouTubeAccountLease lease = activeLease(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({
		{std::string(channelId), "Old channel"},
	}));
	fixture.discoveryPointer->completeStreams(streamPage({
		{std::string(streamId), std::string(channelId), "Old stream"},
	}));
	CHECK(fixture.provider->snapshot().account.state == easy_multistream::YouTubeAccountState::Connected);
	CHECK(fixture.provider->snapshot().account.channelId == channelId);
	CHECK(lease == fixture.provider->snapshot().account.connectionLease.value_or(YouTubeAccountLease{}));
}

void testVaultAndSelectionCommitFailuresDoNotPublishConnection()
{
	{
		Fixture fixture;
		fixture.vault.writeError = CredentialError::Unavailable;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({{"stream", "channel", "Stream"}}));
		CHECK(fixture.provider->snapshot().stage != YouTubeAccountProviderStage::Connected);
		CHECK(fixture.provider->snapshot().account.state != easy_multistream::YouTubeAccountState::Connected);
		CHECK(fixture.commitCount == 0);
	}

	{
		Fixture fixture;
		fixture.commitResult = false;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({{"stream", "channel", "Stream"}}));
		CHECK(fixture.commitCount == 1);
		CHECK(fixture.provider->snapshot().stage != YouTubeAccountProviderStage::Connected);
		CHECK(fixture.provider->snapshot().account.state != easy_multistream::YouTubeAccountState::Connected);
		CHECK(fixture.vault.writeCount == 1);
		CHECK(fixture.vault.eraseCount == 1);
		CHECK(fixture.vault.stored.empty());
	}
}

void testReplacementFailurePreservesOldConnection()
{
	Fixture fixture;
	connectFixture(fixture);
	const auto oldSnapshot = fixture.provider->snapshot();
	fixture.commitResult = false;
	CHECK(fixture.provider->startConnection(GoogleOAuthConsentMode::ForceConsent) ==
	      YouTubeAccountProviderStartStatus::Started);
	completeAuthorization(fixture);
	completeToken(fixture);
	const YouTubeAccountLease replacement = activeLease(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({{"new-channel", "New channel"}}));
	fixture.discoveryPointer->completeStreams(streamPage({{"new-stream", "new-channel", "New stream"}}));
	const auto after = fixture.provider->snapshot();
	CHECK(after.account.channelId == oldSnapshot.account.channelId);
	CHECK(after.account.streamId == oldSnapshot.account.streamId);
	CHECK(after.account.state == easy_multistream::YouTubeAccountState::Connected ||
	      after.account.state == easy_multistream::YouTubeAccountState::Unavailable);
	CHECK(after.account.connectionLease == oldSnapshot.account.connectionLease);
	CHECK(replacement != oldSnapshot.account.connectionLease.value_or(YouTubeAccountLease{}));
}

void testReplacementRollbackRestoresOldCredentialAndFailsClosedWhenItCannot()
{
	{
		Fixture fixture;
		connectFixture(fixture);
		const std::string previousToken(fixture.vault.stored.view());
		const auto previousConnection = fixture.provider->snapshot().account.connectionLease;
		fixture.commitResult = false;

		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		completeToken(fixture, true, "replacement-refresh-token");
		fixture.discoveryPointer->completeChannels(channelPage({{"new-channel", "New channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({{"new-stream", "new-channel", "New stream"}}));

		CHECK(fixture.vault.stored.view() == previousToken);
		CHECK(fixture.provider->snapshot().account.state == easy_multistream::YouTubeAccountState::Connected);
		CHECK(fixture.provider->snapshot().account.connectionLease == previousConnection);
		CHECK(fixture.provider->snapshot().account.channelId == "old-channel");
	}

	{
		Fixture fixture;
		connectFixture(fixture);
		fixture.commitResult = false;
		fixture.vault.failWriteOnCall = fixture.vault.writeCount + 2;

		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		completeToken(fixture, true, "replacement-refresh-token");
		fixture.discoveryPointer->completeChannels(channelPage({{"new-channel", "New channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({{"new-stream", "new-channel", "New stream"}}));

		const auto snapshot = fixture.provider->snapshot();
		CHECK(snapshot.stage == YouTubeAccountProviderStage::Failed);
		CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Unavailable);
		CHECK(snapshot.account.failure == easy_multistream::YouTubeAccountFailure::CredentialUnavailable);
		CHECK(snapshot.account.channelId.empty());
		CHECK(!snapshot.account.connectionLease.has_value());
	}
}

void testSynchronousStartFailuresAndMissingRefreshTokenTerminate()
{
	{
		Fixture fixture;
		fixture.authorizationPointer->forcedStartStatus =
			easy_multistream::GoogleOAuthAuthorizationStartStatus::BrowserOpenFailed;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::OperationFailed);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(!fixture.provider->snapshot().account.lease.has_value());
	}

	{
		Fixture fixture;
		fixture.tokenPointer->forcedStartStatus = GoogleOAuthTokenStartStatus::RequestCreationFailed;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::TokenExchangeFailed);
	}

	{
		Fixture fixture;
		fixture.discoveryPointer->forcedChannelStartStatus = YouTubeApiPagerStartStatus::RequestCreationFailed;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		completeToken(fixture);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(!fixture.provider->snapshot().account.lease.has_value());
	}

	{
		Fixture fixture;
		fixture.discoveryPointer->forcedStreamStartStatus = YouTubeApiPagerStartStatus::RequestCreationFailed;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.vault.writeCount == 0);
	}

	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		completeToken(fixture, false);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::InvalidResponse);
		CHECK(fixture.discoveryPointer->channelStartCount == 0);
	}
}

void testCredentialReadFailureAndContextInvalidationDoNotPersist()
{
	{
		Fixture fixture;
		fixture.vault.readError = CredentialError::AccessDenied;
		startAndReachChannelListing(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		fixture.discoveryPointer->completeStreams(streamPage({{"stream", "channel", "Stream"}}));
		CHECK(fixture.vault.writeCount == 0);
		CHECK(fixture.commitCount == 0);
		CHECK(fixture.provider->snapshot().account.failure ==
		      easy_multistream::YouTubeAccountFailure::CredentialUnavailable);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		const auto oldHandler = fixture.discoveryPointer->channelHandler;
		const auto oldAttempt = fixture.discoveryPointer->lastChannelAttempt;
		CHECK(fixture.provider->invalidateContext());
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Idle);
		if (oldHandler && oldAttempt.has_value()) {
			auto stale = channelPage({{"stale-channel", "Stale"}});
			stale.attempt = *oldAttempt;
			oldHandler(std::move(stale));
		}
		CHECK(fixture.discoveryPointer->streamStartCount == 0);
		CHECK(fixture.vault.writeCount == 0);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Idle);
	}
}

void testCancelAtEachActiveStageSuppressesLateCallbacks()
{
	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		const YouTubeAccountLease lease = activeLease(fixture);
		CHECK(fixture.provider->cancel(lease));
		CHECK(fixture.authorizationPointer->cancelCount == 1);
		fixture.authorizationPointer->complete(authorizationSuccess({lease.generation, lease.attempt}));
		CHECK(fixture.provider->snapshot().stage != YouTubeAccountProviderStage::Connected);
	}

	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		const YouTubeAccountLease lease = activeLease(fixture);
		CHECK(fixture.provider->cancel(lease));
		CHECK(fixture.tokenPointer->cancelCount == 1);
		fixture.tokenPointer->complete(tokenSuccess({lease.generation, lease.attempt}));
		CHECK(fixture.discoveryPointer->channelStartCount == 0);
		CHECK(fixture.provider->snapshot().stage != YouTubeAccountProviderStage::Connected);
	}

	{
		Fixture fixture;
		startAndReachChannelListing(fixture);
		const YouTubeAccountLease lease = activeLease(fixture);
		CHECK(fixture.provider->cancel(lease));
		CHECK(fixture.discoveryPointer->cancelCount == 1);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel", "Channel"}}));
		CHECK(fixture.provider->snapshot().stage != YouTubeAccountProviderStage::Connected);
	}
}

void testStaleOldAttemptCannotReplaceNewAttempt()
{
	Fixture fixture;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	const YouTubeAccountLease oldLease = activeLease(fixture);
	const auto oldAuthorizationHandler = fixture.authorizationPointer->handler;
	CHECK(fixture.provider->cancel(oldLease));
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	const YouTubeAccountLease currentLease = activeLease(fixture);
	CHECK(currentLease != oldLease);
	if (oldAuthorizationHandler) {
		oldAuthorizationHandler(authorizationSuccess({oldLease.generation, oldLease.attempt}));
	}
	CHECK(fixture.tokenPointer->startCount == 0);
	completeAuthorization(fixture);
	completeToken(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({{"new-channel", "New"}}));
	fixture.discoveryPointer->completeStreams(streamPage({{"new-stream", "new-channel", "New"}}));
	CHECK(fixture.provider->snapshot().account.channelId == "new-channel");
	CHECK(fixture.provider->snapshot().account.connectionLease == currentLease);
}

void testInvalidSelectionLeaseAndShutdownAreSafe()
{
	Fixture fixture;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	const YouTubeAccountLease lease = activeLease(fixture);
	CHECK(fixture.provider->selectChannel({lease.generation, lease.attempt + 1}, "anything") ==
	      YouTubeAccountProviderSelectionStatus::StaleLease);
	CHECK(fixture.provider->selectStream(lease, "anything") == YouTubeAccountProviderSelectionStatus::WrongStage);
	CHECK(fixture.provider->shutdown());
	CHECK(fixture.provider->shutdown());
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Closed);
	CHECK(fixture.authorizationPointer->shutdownCount == 1);
	CHECK(fixture.tokenPointer->shutdownCount == 1);
	CHECK(fixture.discoveryPointer->shutdownCount == 1);
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Closed);
	fixture.authorizationPointer->complete(authorizationSuccess({lease.generation, lease.attempt}));
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Closed);

	{
		Fixture destroyed;
		CHECK(destroyed.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		// The fake drops its handler on provider destruction; processing events
		// afterwards must not access the destroyed provider.
	}
	QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

void testInvalidStartAndCommitterFailureDoNotMutateState()
{
	FakeCredentialVault vault;
	int commits = 0;
	YouTubeAccountProviderTestAccess provider(
		std::make_unique<FakeAuthorizationPort>(), std::make_unique<FakeTokenPort>(),
		std::make_unique<FakeDiscoveryPort>(), vault,
		[&](const easy_multistream::YouTubeAccountSelection &) {
			++commits;
			return true;
		},
		QString{});
	CHECK(provider.startConnection() == YouTubeAccountProviderStartStatus::InvalidClientId);
	CHECK(provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	(void)commits;
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testSingleCandidateAutoSelectionAndSecretFreeSnapshot();
	testMultipleCandidatesRequireExactIdSelection();
	testEmptyCandidatesAndInvalidSelectionDoNotPersist();
	testMalformedCandidatesFailClosed();
	testAuthorizationTokenAndDiscoveryFailuresAreSanitized();
	testVaultAndSelectionCommitFailuresDoNotPublishConnection();
	testReplacementFailurePreservesOldConnection();
	testReplacementRollbackRestoresOldCredentialAndFailsClosedWhenItCannot();
	testSynchronousStartFailuresAndMissingRefreshTokenTerminate();
	testCredentialReadFailureAndContextInvalidationDoNotPersist();
	testCancelAtEachActiveStageSuppressesLateCallbacks();
	testStaleOldAttemptCannotReplaceNewAttempt();
	testInvalidSelectionLeaseAndShutdownAreSafe();
	testInvalidStartAndCommitterFailureDoNotMutateState();

	if (failures != 0) {
		std::cerr << failures << " YouTube account provider test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube account provider tests passed\n";
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-destination-preparer.hpp"

#include <QCoreApplication>
#include <QEventLoop>

#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace easy_multistream {

// The production constructor deliberately accepts only production adapters.
// This friend keeps this suite detached from network and OBS code while still
// exercising the real orchestration and lifetime rules.
class YouTubeAccountDestinationPreparerTestAccess final {
public:
	YouTubeAccountDestinationPreparerTestAccess(std::unique_ptr<YouTubeDestinationRefreshPort> refresh,
						    std::unique_ptr<YouTubeDestinationResolverPort> resolver,
						    YouTubeAccountRefreshTokenVault &vault,
						    QString clientId = QStringLiteral("test-client-id"))
		: preparer_(new YouTubeAccountDestinationPreparer(std::move(clientId), std::move(refresh),
								  std::move(resolver), vault, nullptr))
	{
	}

	YouTubeDestinationPrepareStartStatus
	start(YouTubeDestinationPrepareRequest request,
	      YouTubeAccountDestinationPreparer::CompletionHandler handler) noexcept
	{
		return preparer_->start(std::move(request), std::move(handler));
	}
	bool cancel(YouTubeDestinationPrepareAttempt attempt) noexcept { return preparer_->cancel(attempt); }
	bool invalidateContext() noexcept { return preparer_->invalidateContext(); }
	bool shutdown() noexcept { return preparer_->shutdown(); }
	YouTubeDestinationPreparerState state() const noexcept { return preparer_->state(); }
	std::optional<YouTubeDestinationPrepareAttempt> activeAttempt() const noexcept
	{
		return preparer_->activeAttempt();
	}

private:
	std::unique_ptr<YouTubeAccountDestinationPreparer> preparer_;
};

} // namespace easy_multistream

namespace {

using namespace easy_multistream;

int failures = 0;

#define CHECK(expression)                                                                                             \
	do {                                                                                                                \
		if (!(expression)) {                                                                                          \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';              \
			++failures;                                                                                                \
		}                                                                                                             \
	} while (false)

constexpr std::string_view kOldRefreshToken = "refresh-token-old";
constexpr std::string_view kRotatedRefreshToken = "refresh-token-rotated";
constexpr std::string_view kAccessToken = "access-token";
constexpr std::string_view kStreamKey = "stream-key";
constexpr std::string_view kServerUrl = "rtmps://a.rtmps.youtube.com/live2";

struct RefreshState final {
	using Handler = YouTubeDestinationRefreshPort::CompletionHandler;
	std::vector<Handler> handlers;
	std::vector<GoogleOAuthTokenAttempt> cancelled;
	std::optional<GoogleOAuthTokenAttempt> lastAttempt;
	std::string lastRefreshToken;
	QString lastClientId;
	std::optional<GoogleOAuthTokenStartStatus> forcedStartStatus;
	std::function<void()> onCancel;
	std::function<void()> onShutdown;
	int shutdownCount = 0;
};

class FakeRefreshPort final : public YouTubeDestinationRefreshPort {
public:
	explicit FakeRefreshPort(std::shared_ptr<RefreshState> state) : state_(std::move(state)) {}

	GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
						 CompletionHandler completionHandler) noexcept override
	{
		state_->lastAttempt = request.attempt;
		state_->lastRefreshToken = std::string(request.refreshToken.view());
		state_->lastClientId = request.clientId;
		state_->handlers.push_back(std::move(completionHandler));
		if (state_->forcedStartStatus.has_value()) {
			return *state_->forcedStartStatus;
		}
		return GoogleOAuthTokenStartStatus::Started;
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override
	{
		state_->cancelled.push_back(attempt);
		if (state_->onCancel) {
			state_->onCancel();
		}
		return true;
	}

	bool shutdown() noexcept override
	{
		++state_->shutdownCount;
		if (state_->onShutdown) {
			state_->onShutdown();
		}
		return true;
	}

private:
	std::shared_ptr<RefreshState> state_;
};

struct ResolverState final {
	using Handler = YouTubeDestinationResolverPort::CompletionHandler;
	std::vector<Handler> handlers;
	std::vector<YouTubeApiAttempt> cancelled;
	std::optional<YouTubeApiAttempt> lastAttempt;
	std::string lastAccessToken;
	std::string lastChannelId;
	std::string lastStreamId;
	std::optional<YouTubeStreamResolverStartStatus> forcedStartStatus;
	std::function<void()> onShutdown;
	int shutdownCount = 0;
};

class FakeResolverPort final : public YouTubeDestinationResolverPort {
public:
	explicit FakeResolverPort(std::shared_ptr<ResolverState> state) : state_(std::move(state)) {}

	YouTubeStreamResolverStartStatus startResolveStream(YouTubeResolveStreamRequest request,
							    CompletionHandler completionHandler) noexcept override
	{
		state_->lastAttempt = request.attempt;
		state_->lastAccessToken = std::string(request.accessToken.view());
		state_->lastChannelId = std::move(request.channelId);
		state_->lastStreamId = std::move(request.streamId);
		state_->handlers.push_back(std::move(completionHandler));
		if (state_->forcedStartStatus.has_value()) {
			return *state_->forcedStartStatus;
		}
		return YouTubeStreamResolverStartStatus::Started;
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept override
	{
		state_->cancelled.push_back(attempt);
		return true;
	}

	bool shutdown() noexcept override
	{
		++state_->shutdownCount;
		if (state_->onShutdown) {
			state_->onShutdown();
		}
		return true;
	}

private:
	std::shared_ptr<ResolverState> state_;
};

class FakeCredentialVault final : public YouTubeAccountRefreshTokenVault {
public:
	CredentialResult write(std::string_view value) noexcept override
	{
		++writeCount;
		lastWritten = SecureBuffer::copyOf(value);
		if (writeError != CredentialError::None) {
			return {writeError, 1};
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
		stored.clear();
		return {};
	}

	CredentialStatus status() noexcept override
	{
		if (readError == CredentialError::None && !stored.empty()) {
			return {CredentialState::Present, {}};
		}
		if (readError == CredentialError::NotFound || stored.empty()) {
			return {CredentialState::Missing, {CredentialError::NotFound, 0}};
		}
		return {CredentialState::Unavailable, {readError, 1}};
	}

	SecureBuffer stored;
	SecureBuffer lastWritten;
	CredentialError readError = CredentialError::None;
	CredentialError writeError = CredentialError::None;
	int readCount = 0;
	int writeCount = 0;
};

// Keep only non-secret completion data. The key is copied transiently so the
// test can verify the successful hand-off, then the move-only completion dies.
struct Result final {
	YouTubeDestinationPrepareAttempt attempt;
	YouTubeDestinationPrepareStatus status = YouTubeDestinationPrepareStatus::InvalidResponse;
	bool succeeded = false;
	std::string serverUrl;
	std::string streamKey;
};

struct Fixture final {
	std::shared_ptr<RefreshState> refreshState = std::make_shared<RefreshState>();
	std::shared_ptr<ResolverState> resolverState = std::make_shared<ResolverState>();
	FakeCredentialVault vault;
	std::unique_ptr<YouTubeAccountDestinationPreparerTestAccess> preparer;

	Fixture()
	{
		preparer = std::make_unique<YouTubeAccountDestinationPreparerTestAccess>(
			std::make_unique<FakeRefreshPort>(refreshState),
			std::make_unique<FakeResolverPort>(resolverState), vault);
	}
};

YouTubeAccountSelection validSelection()
{
	YouTubeAccountSelection selection;
	selection.channelId = "channel-1";
	selection.channelLabel = "My channel";
	selection.streamId = "stream-1";
	selection.streamLabel = "Reusable stream";
	return selection;
}

YouTubeDestinationPrepareRequest requestFor(YouTubeDestinationPrepareAttempt attempt)
{
	YouTubeDestinationPrepareRequest request;
	request.attempt = attempt;
	request.selection = validSelection();
	return request;
}

void processQueuedEvents()
{
	for (int i = 0; i < 8; ++i) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	}
}

std::function<void(YouTubeDestinationPrepareCompletion)> recorder(std::vector<Result> &results)
{
	return [&results](YouTubeDestinationPrepareCompletion completion) {
		Result result;
		result.attempt = completion.attempt;
		result.status = completion.status;
		result.succeeded = completion.succeeded();
		if (completion.ingestion.has_value()) {
			result.serverUrl = completion.ingestion->serverUrl;
			result.streamKey = std::string(completion.ingestion->streamKey.view());
		}
		results.push_back(std::move(result));
	};
}

GoogleOAuthTokenCompletion refreshSuccess(GoogleOAuthTokenAttempt attempt, bool rotate = false)
{
	GoogleOAuthTokenCompletion completion;
	completion.attempt = attempt;
	completion.operation = GoogleOAuthTokenOperation::RefreshAccessToken;
	completion.status = GoogleOAuthTokenCompletionStatus::Success;
	completion.tokens.accessToken = SecureBuffer::copyOf(kAccessToken);
	if (rotate) {
		completion.tokens.refreshToken = SecureBuffer::copyOf(kRotatedRefreshToken);
	}
	completion.tokens.expiresInSeconds = 3600;
	return completion;
}

GoogleOAuthTokenCompletion refreshFailure(GoogleOAuthTokenAttempt attempt, GoogleOAuthTokenCompletionStatus status,
					  GoogleOAuthTokenProviderError error = GoogleOAuthTokenProviderError::None)
{
	GoogleOAuthTokenCompletion completion;
	completion.attempt = attempt;
	completion.operation = GoogleOAuthTokenOperation::RefreshAccessToken;
	completion.status = status;
	completion.providerError = error;
	return completion;
}

YouTubeStreamResolverCompletion resolveSuccess(YouTubeApiAttempt attempt)
{
	YouTubeStreamResolverCompletion completion;
	completion.attempt = attempt;
	completion.status = YouTubeStreamResolverCompletionStatus::Success;
	YouTubeResolvedIngestion ingestion;
	ingestion.serverUrl = std::string(kServerUrl);
	ingestion.streamKey = SecureBuffer::copyOf(kStreamKey);
	completion.ingestion.emplace(std::move(ingestion));
	return completion;
}

YouTubeStreamResolverCompletion resolveFailure(YouTubeApiAttempt attempt, YouTubeStreamResolverCompletionStatus status,
					       YouTubeApiProviderError error = YouTubeApiProviderError::None)
{
	YouTubeStreamResolverCompletion completion;
	completion.attempt = attempt;
	completion.status = status;
	completion.providerError = error;
	return completion;
}

void beginWithStoredCredential(Fixture &fixture, YouTubeDestinationPrepareAttempt attempt, std::vector<Result> &results)
{
	fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	CHECK(fixture.preparer->start(requestFor(attempt), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.refreshState->handlers.size() == 1);
	CHECK(fixture.refreshState->lastRefreshToken == kOldRefreshToken);
}

void completeRefreshAndReachResolver(Fixture &fixture, bool rotate = false)
{
	CHECK(fixture.refreshState->lastAttempt.has_value());
	if (!fixture.refreshState->lastAttempt.has_value()) {
		return;
	}
	fixture.refreshState->handlers.front()(refreshSuccess(*fixture.refreshState->lastAttempt, rotate));
	processQueuedEvents();
	CHECK(fixture.resolverState->handlers.size() == 1);
}

void testSuccessAndExistingRefreshTokenIsRetained()
{
	Fixture fixture;
	std::vector<Result> results;
	const YouTubeDestinationPrepareAttempt attempt{1, 1};
	beginWithStoredCredential(fixture, attempt, results);
	completeRefreshAndReachResolver(fixture, false);
	CHECK(fixture.vault.writeCount == 0);
	CHECK(fixture.resolverState->lastAccessToken == kAccessToken);
	CHECK(fixture.resolverState->lastChannelId == "channel-1");
	CHECK(fixture.resolverState->lastStreamId == "stream-1");
	CHECK(fixture.resolverState->lastAttempt.has_value());
	if (fixture.resolverState->lastAttempt.has_value()) {
		fixture.resolverState->handlers.front()(resolveSuccess(*fixture.resolverState->lastAttempt));
	}
	CHECK(results.empty());
	processQueuedEvents();
	CHECK(results.size() == 1);
	if (results.size() == 1) {
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::Success);
		CHECK(results[0].succeeded);
		CHECK(results[0].serverUrl == kServerUrl);
		CHECK(results[0].streamKey == kStreamKey);
		CHECK(results[0].attempt == attempt);
	}
	CHECK(fixture.preparer->state() == YouTubeDestinationPreparerState::Completed);
	CHECK(!fixture.preparer->activeAttempt().has_value());
}

void testRefreshTokenRotationIsPersistedBeforeResolve()
{
	Fixture fixture;
	std::vector<Result> results;
	const YouTubeDestinationPrepareAttempt attempt{2, 1};
	beginWithStoredCredential(fixture, attempt, results);
	completeRefreshAndReachResolver(fixture, true);
	CHECK(fixture.vault.writeCount == 1);
	CHECK(std::string(fixture.vault.stored.view()) == kRotatedRefreshToken);
	CHECK(std::string(fixture.vault.lastWritten.view()) == kRotatedRefreshToken);
	CHECK(fixture.resolverState->handlers.size() == 1);
	CHECK(fixture.resolverState->lastAccessToken == kAccessToken);

	fixture.resolverState->handlers.front()(resolveSuccess(*fixture.resolverState->lastAttempt));
	processQueuedEvents();
	CHECK(results.size() == 1);
	CHECK(results[0].status == YouTubeDestinationPrepareStatus::Success);
}

void testRefreshTokenRotationWriteFailureStopsBeforeResolver()
{
	Fixture fixture;
	fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	fixture.vault.writeError = CredentialError::Unavailable;
	std::vector<Result> results;
	const YouTubeDestinationPrepareAttempt attempt{3, 1};
	CHECK(fixture.preparer->start(requestFor(attempt), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.refreshState->lastAttempt.has_value());
	if (fixture.refreshState->lastAttempt.has_value()) {
		fixture.refreshState->handlers.front()(refreshSuccess(*fixture.refreshState->lastAttempt, true));
	}
	processQueuedEvents();
	CHECK(results.size() == 1);
	CHECK(results[0].status == YouTubeDestinationPrepareStatus::CredentialUnavailable);
	CHECK(fixture.resolverState->handlers.empty());
	CHECK(std::string(fixture.vault.stored.view()) == kOldRefreshToken);
}

void testPersistedRotationIsNotRolledBackAfterResolveFailureOrCancel()
{
	{
		Fixture fixture;
		std::vector<Result> results;
		beginWithStoredCredential(fixture, {3, 2}, results);
		completeRefreshAndReachResolver(fixture, true);
		CHECK(std::string(fixture.vault.stored.view()) == kRotatedRefreshToken);
		CHECK(fixture.resolverState->lastAttempt.has_value());
		if (fixture.resolverState->lastAttempt.has_value()) {
			fixture.resolverState->handlers.front()(
				resolveFailure(*fixture.resolverState->lastAttempt,
					       YouTubeStreamResolverCompletionStatus::StreamNotReady));
		}
		processQueuedEvents();
		CHECK(results.size() == 1);
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::DestinationUnavailable);
		CHECK(std::string(fixture.vault.stored.view()) == kRotatedRefreshToken);
	}

	{
		Fixture fixture;
		std::vector<Result> results;
		const YouTubeDestinationPrepareAttempt attempt{3, 3};
		beginWithStoredCredential(fixture, attempt, results);
		completeRefreshAndReachResolver(fixture, true);
		CHECK(std::string(fixture.vault.stored.view()) == kRotatedRefreshToken);
		CHECK(fixture.preparer->cancel(attempt));
		processQueuedEvents();
		CHECK(results.size() == 1);
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::Cancelled);
		CHECK(std::string(fixture.vault.stored.view()) == kRotatedRefreshToken);
	}
}

void testCredentialReadFailuresAreSanitized()
{
	const std::vector<std::pair<CredentialError, YouTubeDestinationPrepareStatus>> cases = {
		{CredentialError::NotFound, YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{CredentialError::CorruptData, YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{CredentialError::AccessDenied, YouTubeDestinationPrepareStatus::CredentialUnavailable},
		{CredentialError::Unavailable, YouTubeDestinationPrepareStatus::CredentialUnavailable},
	};
	std::uint64_t attemptNumber = 1;
	for (const auto &[error, expected] : cases) {
		Fixture fixture;
		fixture.vault.readError = error;
		std::vector<Result> results;
		CHECK(fixture.preparer->start(requestFor({4, attemptNumber++}), recorder(results)) ==
		      YouTubeDestinationPrepareStartStatus::Started);
		processQueuedEvents();
		CHECK(results.size() == 1);
		if (results.size() == 1) {
			CHECK(results[0].status == expected);
			CHECK(!results[0].succeeded);
		}
		CHECK(fixture.refreshState->handlers.empty());
	}
}

void testTokenProviderFailuresMapWithoutLeakingDetails()
{
	struct Case final {
		GoogleOAuthTokenCompletionStatus status;
		GoogleOAuthTokenProviderError error;
		YouTubeDestinationPrepareStatus expected;
	};
	const std::vector<Case> cases = {
		{GoogleOAuthTokenCompletionStatus::ProviderRejected, GoogleOAuthTokenProviderError::InvalidGrant,
		 YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{GoogleOAuthTokenCompletionStatus::ProviderRejected, GoogleOAuthTokenProviderError::InvalidToken,
		 YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{GoogleOAuthTokenCompletionStatus::ProviderRejected, GoogleOAuthTokenProviderError::AccessDenied,
		 YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{GoogleOAuthTokenCompletionStatus::NetworkFailure, GoogleOAuthTokenProviderError::None,
		 YouTubeDestinationPrepareStatus::NetworkFailure},
		{GoogleOAuthTokenCompletionStatus::TlsFailure, GoogleOAuthTokenProviderError::None,
		 YouTubeDestinationPrepareStatus::NetworkFailure},
		{GoogleOAuthTokenCompletionStatus::TimedOut, GoogleOAuthTokenProviderError::None,
		 YouTubeDestinationPrepareStatus::NetworkFailure},
		{GoogleOAuthTokenCompletionStatus::HttpFailure, GoogleOAuthTokenProviderError::None,
		 YouTubeDestinationPrepareStatus::ServiceUnavailable},
		{GoogleOAuthTokenCompletionStatus::InvalidResponse, GoogleOAuthTokenProviderError::None,
		 YouTubeDestinationPrepareStatus::InvalidResponse},
	};
	std::uint64_t attemptNumber = 1;
	for (const auto &testCase : cases) {
		Fixture fixture;
		std::vector<Result> results;
		beginWithStoredCredential(fixture, {5, attemptNumber++}, results);
		CHECK(fixture.refreshState->lastAttempt.has_value());
		if (fixture.refreshState->lastAttempt.has_value()) {
			fixture.refreshState->handlers.front()(
				refreshFailure(*fixture.refreshState->lastAttempt, testCase.status, testCase.error));
		}
		processQueuedEvents();
		CHECK(results.size() == 1);
		if (results.size() == 1) {
			CHECK(results[0].status == testCase.expected);
			CHECK(results[0].serverUrl.empty());
			CHECK(results[0].streamKey.empty());
		}
		CHECK(fixture.resolverState->handlers.empty());
	}
}

void testUnexpectedTokenOperationFailsClosed()
{
	Fixture fixture;
	std::vector<Result> results;
	beginWithStoredCredential(fixture, {5, 99}, results);
	CHECK(fixture.refreshState->lastAttempt.has_value());
	if (fixture.refreshState->lastAttempt.has_value()) {
		auto completion = refreshSuccess(*fixture.refreshState->lastAttempt);
		completion.operation = GoogleOAuthTokenOperation::ExchangeAuthorizationCode;
		fixture.refreshState->handlers.front()(std::move(completion));
	}
	processQueuedEvents();
	CHECK(results.size() == 1);
	if (results.size() == 1) {
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::InvalidResponse);
	}
	CHECK(fixture.resolverState->handlers.empty());
}

void reachResolverFor(Fixture &fixture, std::vector<Result> &results, YouTubeDestinationPrepareAttempt attempt)
{
	beginWithStoredCredential(fixture, attempt, results);
	completeRefreshAndReachResolver(fixture);
}

void testResolverFailuresMapWithoutLeakingDestination()
{
	struct Case final {
		YouTubeStreamResolverCompletionStatus status;
		YouTubeApiProviderError error;
		YouTubeDestinationPrepareStatus expected;
	};
	const std::vector<Case> cases = {
		{YouTubeStreamResolverCompletionStatus::StreamNotFound, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::DestinationUnavailable},
		{YouTubeStreamResolverCompletionStatus::StreamMismatch, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::DestinationUnavailable},
		{YouTubeStreamResolverCompletionStatus::StreamNotReady, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::DestinationUnavailable},
		{YouTubeStreamResolverCompletionStatus::StreamAlreadyActive, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::DestinationAlreadyActive},
		{YouTubeStreamResolverCompletionStatus::ProviderRejected, YouTubeApiProviderError::InvalidToken,
		 YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{YouTubeStreamResolverCompletionStatus::ProviderRejected, YouTubeApiProviderError::PermissionDenied,
		 YouTubeDestinationPrepareStatus::ReauthorizationRequired},
		{YouTubeStreamResolverCompletionStatus::ProviderRejected,
		 YouTubeApiProviderError::TemporarilyUnavailable, YouTubeDestinationPrepareStatus::ServiceUnavailable},
		{YouTubeStreamResolverCompletionStatus::NetworkFailure, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::NetworkFailure},
		{YouTubeStreamResolverCompletionStatus::TlsFailure, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::NetworkFailure},
		{YouTubeStreamResolverCompletionStatus::TimedOut, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::NetworkFailure},
		{YouTubeStreamResolverCompletionStatus::HttpFailure, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::ServiceUnavailable},
		{YouTubeStreamResolverCompletionStatus::InvalidResponse, YouTubeApiProviderError::None,
		 YouTubeDestinationPrepareStatus::InvalidResponse},
	};
	std::uint64_t attemptNumber = 1;
	for (const auto &testCase : cases) {
		Fixture fixture;
		std::vector<Result> results;
		reachResolverFor(fixture, results, {6, attemptNumber++});
		CHECK(fixture.resolverState->lastAttempt.has_value());
		if (fixture.resolverState->lastAttempt.has_value()) {
			fixture.resolverState->handlers.front()(
				resolveFailure(*fixture.resolverState->lastAttempt, testCase.status, testCase.error));
		}
		processQueuedEvents();
		CHECK(results.size() == 1);
		if (results.size() == 1) {
			CHECK(results[0].status == testCase.expected);
			CHECK(!results[0].succeeded);
			CHECK(results[0].serverUrl.empty());
			CHECK(results[0].streamKey.empty());
		}
	}
}

void testSuccessfulResolverWithInvalidIngestionIsRejected()
{
	Fixture fixture;
	std::vector<Result> results;
	reachResolverFor(fixture, results, {6, 99});
	CHECK(fixture.resolverState->lastAttempt.has_value());
	if (!fixture.resolverState->lastAttempt.has_value()) {
		return;
	}
	YouTubeStreamResolverCompletion completion;
	completion.attempt = *fixture.resolverState->lastAttempt;
	completion.status = YouTubeStreamResolverCompletionStatus::Success;
	YouTubeResolvedIngestion ingestion;
	ingestion.serverUrl = "https://not-an-rtmps-endpoint.example/live";
	ingestion.streamKey = SecureBuffer::copyOf(kStreamKey);
	completion.ingestion.emplace(std::move(ingestion));
	fixture.resolverState->handlers.back()(std::move(completion));
	processQueuedEvents();
	CHECK(results.size() == 1);
	if (results.size() == 1) {
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::InvalidResponse);
		CHECK(!results[0].succeeded);
		CHECK(results[0].serverUrl.empty());
		CHECK(results[0].streamKey.empty());
	}
}

void testSynchronousStartFailuresAreStable()
{
	{
		Fixture fixture;
		fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
		fixture.refreshState->forcedStartStatus = GoogleOAuthTokenStartStatus::RequestCreationFailed;
		std::vector<Result> results;
		const auto status = fixture.preparer->start(requestFor({7, 1}), recorder(results));
		CHECK(status == YouTubeDestinationPrepareStartStatus::Started);
		processQueuedEvents();
		CHECK(results.size() == 1);
		if (results.size() == 1) {
			CHECK(results[0].status == YouTubeDestinationPrepareStatus::ServiceUnavailable);
		}
		CHECK(fixture.resolverState->handlers.empty());
	}
	Fixture resolverFailure;
	resolverFailure.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	resolverFailure.resolverState->forcedStartStatus = YouTubeStreamResolverStartStatus::RequestCreationFailed;
	std::vector<Result> resolverResults;
	CHECK(resolverFailure.preparer->start(requestFor({7, 3}), recorder(resolverResults)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(resolverFailure.refreshState->lastAttempt.has_value());
	if (resolverFailure.refreshState->lastAttempt.has_value()) {
		resolverFailure.refreshState->handlers.front()(
			refreshSuccess(*resolverFailure.refreshState->lastAttempt));
	}
	processQueuedEvents();
	CHECK(resolverResults.size() == 1);
	if (resolverResults.size() == 1) {
		CHECK(resolverResults[0].status == YouTubeDestinationPrepareStatus::ServiceUnavailable);
	}
	CHECK(resolverFailure.preparer->state() == YouTubeDestinationPreparerState::Completed);
}

void testCancelAtRefreshAndLateCallback()
{
	Fixture fixture;
	fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	std::vector<Result> results;
	const YouTubeDestinationPrepareAttempt attempt{8, 1};
	CHECK(fixture.preparer->start(requestFor(attempt), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.preparer->cancel(attempt));
	CHECK(fixture.refreshState->cancelled.size() == 1);
	CHECK(fixture.resolverState->cancelled.size() == 1);
	processQueuedEvents();
	CHECK(results.size() == 1);
	CHECK(results[0].status == YouTubeDestinationPrepareStatus::Cancelled);
	// A late transport callback is ignored and cannot publish a second result.
	CHECK(fixture.refreshState->lastAttempt.has_value());
	if (fixture.refreshState->lastAttempt.has_value()) {
		fixture.refreshState->handlers.front()(refreshSuccess(*fixture.refreshState->lastAttempt));
	}
	processQueuedEvents();
	CHECK(results.size() == 1);
}

void testCancelAtResolverAndLateCallback()
{
	Fixture fixture;
	std::vector<Result> results;
	reachResolverFor(fixture, results, {9, 1});
	CHECK(fixture.preparer->activeAttempt().has_value());
	const auto attempt = fixture.preparer->activeAttempt().value_or(YouTubeDestinationPrepareAttempt{});
	CHECK(fixture.preparer->cancel(attempt));
	CHECK(fixture.resolverState->cancelled.size() == 1);
	processQueuedEvents();
	CHECK(results.size() == 1);
	CHECK(results[0].status == YouTubeDestinationPrepareStatus::Cancelled);
	CHECK(fixture.resolverState->lastAttempt.has_value());
	if (fixture.resolverState->lastAttempt.has_value()) {
		fixture.resolverState->handlers.front()(resolveSuccess(*fixture.resolverState->lastAttempt));
	}
	processQueuedEvents();
	CHECK(results.size() == 1);
}

void testCancelDuringQueuedDeliveryProducesOneCancelledCompletion()
{
	Fixture fixture;
	std::vector<Result> results;
	reachResolverFor(fixture, results, {10, 1});
	CHECK(fixture.resolverState->lastAttempt.has_value());
	if (fixture.resolverState->lastAttempt.has_value()) {
		fixture.resolverState->handlers.front()(resolveSuccess(*fixture.resolverState->lastAttempt));
	}
	// The success is queued. Cancelling before the event loop runs must replace
	// it with exactly one cancellation rather than delivering stale success.
	const auto active = fixture.preparer->activeAttempt();
	CHECK(active.has_value());
	if (active.has_value()) {
		CHECK(fixture.preparer->cancel(*active));
	}
	processQueuedEvents();
	CHECK(results.size() == 1);
	if (results.size() == 1) {
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::Cancelled);
	}
}

void testLifecycleMutationRejectsSynchronousPortReentry()
{
	{
		Fixture fixture;
		std::vector<Result> results;
		const YouTubeDestinationPrepareAttempt attempt{10, 2};
		beginWithStoredCredential(fixture, attempt, results);
		bool nestedCancel = true;
		bool nestedInvalidation = true;
		bool nestedShutdown = true;
		fixture.refreshState->onCancel = [&] {
			nestedCancel = fixture.preparer->cancel(attempt);
			nestedInvalidation = fixture.preparer->invalidateContext();
			nestedShutdown = fixture.preparer->shutdown();
		};
		CHECK(fixture.preparer->cancel(attempt));
		CHECK(!nestedCancel);
		CHECK(!nestedInvalidation);
		CHECK(!nestedShutdown);
		CHECK(fixture.refreshState->cancelled.size() == 1);
		processQueuedEvents();
		CHECK(results.size() == 1);
		CHECK(results[0].status == YouTubeDestinationPrepareStatus::Cancelled);
	}

	{
		Fixture fixture;
		std::vector<Result> results;
		beginWithStoredCredential(fixture, {10, 3}, results);
		std::vector<YouTubeDestinationPrepareStartStatus> nestedStarts;
		auto tryStart = [&] {
			nestedStarts.push_back(fixture.preparer->start(requestFor({10, 4}), recorder(results)));
		};
		fixture.refreshState->onShutdown = tryStart;
		fixture.resolverState->onShutdown = tryStart;
		CHECK(fixture.preparer->shutdown());
		CHECK(nestedStarts.size() == 2);
		for (const auto status : nestedStarts) {
			CHECK(status == YouTubeDestinationPrepareStartStatus::Closed);
		}
		CHECK(fixture.preparer->state() == YouTubeDestinationPreparerState::Closed);
		processQueuedEvents();
		CHECK(results.empty());
	}
}

void testStaleAttemptCannotReplaceNewAttempt()
{
	Fixture fixture;
	std::vector<Result> firstResults;
	std::vector<Result> secondResults;
	const YouTubeDestinationPrepareAttempt first{11, 1};
	const YouTubeDestinationPrepareAttempt second{11, 2};
	beginWithStoredCredential(fixture, first, firstResults);
	CHECK(fixture.preparer->cancel(first));
	processQueuedEvents();
	CHECK(firstResults.size() == 1);
	CHECK(firstResults[0].status == YouTubeDestinationPrepareStatus::Cancelled);
	CHECK(fixture.preparer->start(requestFor(second), recorder(secondResults)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.refreshState->handlers.size() >= 2);
	// Complete the old callback after the new attempt is active.
	fixture.refreshState->handlers.front()(refreshSuccess({first.generation, 99}));
	processQueuedEvents();
	CHECK(secondResults.empty());
	CHECK(fixture.preparer->activeAttempt().has_value());
	if (fixture.refreshState->lastAttempt.has_value()) {
		fixture.refreshState->handlers.back()(refreshSuccess(*fixture.refreshState->lastAttempt));
	}
	processQueuedEvents();
	CHECK(fixture.resolverState->handlers.size() >= 1);
	if (!fixture.resolverState->handlers.empty() && fixture.resolverState->lastAttempt.has_value()) {
		fixture.resolverState->handlers.back()(resolveSuccess(*fixture.resolverState->lastAttempt));
	}
	processQueuedEvents();
	CHECK(secondResults.size() == 1);
	CHECK(secondResults[0].status == YouTubeDestinationPrepareStatus::Success);
	CHECK(secondResults[0].attempt == second);
}

void testInvalidRequestAndBusyState()
{
	{
		FakeCredentialVault vault;
		const auto refreshState = std::make_shared<RefreshState>();
		const auto resolverState = std::make_shared<ResolverState>();
		YouTubeAccountDestinationPreparerTestAccess invalidClient(
			std::make_unique<FakeRefreshPort>(refreshState),
			std::make_unique<FakeResolverPort>(resolverState), vault, QStringLiteral("client id"));
		std::vector<Result> ignored;
		CHECK(invalidClient.start(requestFor({12, 99}), recorder(ignored)) ==
		      YouTubeDestinationPrepareStartStatus::InvalidClientId);
		CHECK(vault.readCount == 0);
		CHECK(refreshState->handlers.empty());
	}

	Fixture fixture;
	std::vector<Result> results;
	CHECK(fixture.preparer->start({}, recorder(results)) == YouTubeDestinationPrepareStartStatus::InvalidAttempt);
	YouTubeDestinationPrepareRequest invalidSelection;
	invalidSelection.attempt = {12, 1};
	CHECK(fixture.preparer->start(std::move(invalidSelection), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::InvalidSelection);
	fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	CHECK(fixture.preparer->start(requestFor({12, 1}), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.preparer->start(requestFor({12, 2}), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Busy);
	CHECK(fixture.preparer->cancel({12, 1}));
	processQueuedEvents();
	CHECK(results.size() == 1);
}

void testInvalidationSuppressesOldContext()
{
	Fixture fixture;
	std::vector<Result> results;
	fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	const YouTubeDestinationPrepareAttempt attempt{13, 1};
	CHECK(fixture.preparer->start(requestFor(attempt), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.preparer->invalidateContext());
	CHECK(fixture.refreshState->cancelled.size() == 1);
	processQueuedEvents();
	CHECK(results.empty());
	CHECK(!fixture.preparer->activeAttempt().has_value());
	CHECK(fixture.preparer->state() == YouTubeDestinationPreparerState::Idle);
	if (fixture.refreshState->lastAttempt.has_value()) {
		fixture.refreshState->handlers.front()(refreshSuccess(*fixture.refreshState->lastAttempt));
	}
	processQueuedEvents();
	CHECK(results.empty());
}

void testShutdownSuppressesCompletionAndCloses()
{
	Fixture fixture;
	std::vector<Result> results;
	fixture.vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
	CHECK(fixture.preparer->start(requestFor({14, 1}), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.preparer->shutdown());
	CHECK(fixture.preparer->state() == YouTubeDestinationPreparerState::Closed);
	CHECK(fixture.refreshState->shutdownCount == 1);
	CHECK(fixture.resolverState->shutdownCount == 1);
	processQueuedEvents();
	CHECK(results.empty());
	CHECK(fixture.preparer->start(requestFor({14, 2}), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Closed);
	CHECK(!fixture.preparer->cancel({14, 1}));
}

void testDestructionWithLateCallbacksIsSilent()
{
	FakeCredentialVault vault;
	const auto refreshState = std::make_shared<RefreshState>();
	const auto resolverState = std::make_shared<ResolverState>();
	{
		auto preparer = std::make_unique<YouTubeAccountDestinationPreparerTestAccess>(
			std::make_unique<FakeRefreshPort>(refreshState),
			std::make_unique<FakeResolverPort>(resolverState), vault);
		vault.stored = SecureBuffer::copyOf(kOldRefreshToken);
		std::vector<Result> ignored;
		CHECK(preparer->start(requestFor({15, 1}), recorder(ignored)) ==
		      YouTubeDestinationPrepareStartStatus::Started);
	}
	// Both fake ports deliberately retain their handlers. Invoking them after
	// QObject destruction must not call into freed storage.
	if (!refreshState->handlers.empty() && refreshState->lastAttempt.has_value()) {
		refreshState->handlers.front()(refreshSuccess(*refreshState->lastAttempt));
	}
	processQueuedEvents();
}

void testSecretFreePublicState()
{
	Fixture fixture;
	fixture.vault.stored = SecureBuffer::copyOf("refresh-secret-public-state");
	std::vector<Result> results;
	const auto attempt = YouTubeDestinationPrepareAttempt{16, 1};
	CHECK(fixture.preparer->start(requestFor(attempt), recorder(results)) ==
	      YouTubeDestinationPrepareStartStatus::Started);
	CHECK(fixture.preparer->state() == YouTubeDestinationPreparerState::ReadingCredential ||
	      fixture.preparer->state() == YouTubeDestinationPreparerState::RefreshingAccessToken);
	CHECK(fixture.preparer->activeAttempt().has_value());
	if (fixture.preparer->activeAttempt().has_value()) {
		CHECK(fixture.preparer->activeAttempt()->attempt == attempt.attempt);
	}
	// No public state object contains a credential, access token, URL, or key;
	// the only secret-bearing values are in the move-only port requests.
	CHECK(fixture.preparer->state() != YouTubeDestinationPreparerState::Completed);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testSuccessAndExistingRefreshTokenIsRetained();
	testRefreshTokenRotationIsPersistedBeforeResolve();
	testRefreshTokenRotationWriteFailureStopsBeforeResolver();
	testPersistedRotationIsNotRolledBackAfterResolveFailureOrCancel();
	testCredentialReadFailuresAreSanitized();
	testTokenProviderFailuresMapWithoutLeakingDetails();
	testUnexpectedTokenOperationFailsClosed();
	testResolverFailuresMapWithoutLeakingDestination();
	testSuccessfulResolverWithInvalidIngestionIsRejected();
	testSynchronousStartFailuresAreStable();
	testCancelAtRefreshAndLateCallback();
	testCancelAtResolverAndLateCallback();
	testCancelDuringQueuedDeliveryProducesOneCancelledCompletion();
	testLifecycleMutationRejectsSynchronousPortReentry();
	testStaleAttemptCannotReplaceNewAttempt();
	testInvalidRequestAndBusyState();
	testInvalidationSuppressesOldContext();
	testShutdownSuppressesCompletionAndCloses();
	testDestructionWithLateCallbacksIsSilent();
	testSecretFreePublicState();

	if (failures != 0) {
		std::cerr << failures << " YouTube account destination preparer test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube account destination preparer tests passed\n";
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-provider.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace easy_multistream {

class FakeYouTubeAccountProfileOperationLockApi final : public YouTubeAccountProfileOperationLockApi {
public:
	HANDLE createMutex(LPCWSTR, DWORD &error) noexcept override
	{
		++createCount;
		if (nextCreateError.has_value()) {
			error = *nextCreateError;
			nextCreateError.reset();
			return nullptr;
		}
		const HANDLE handle = reinterpret_cast<HANDLE>(nextHandle++);
		openHandles.insert(handle);
		error = ERROR_SUCCESS;
		return handle;
	}

	DWORD wait(HANDLE handle, DWORD, DWORD &error) noexcept override
	{
		++waitCount;
		lastWaitHandle = handle;
		if (onWait) {
			onWait();
		}
		const DWORD result = nextWaitResult.value_or(WAIT_OBJECT_0);
		nextWaitResult.reset();
		error = nextWaitError.value_or(result == WAIT_FAILED ? ERROR_GEN_FAILURE : ERROR_SUCCESS);
		nextWaitError.reset();
		return result;
	}

	bool releaseMutex(HANDLE handle, DWORD &error) noexcept override
	{
		++releaseCount;
		lastReleaseHandle = handle;
		if (onRelease) {
			onRelease();
		}
		if (!releaseResult) {
			error = releaseError;
			return false;
		}
		error = ERROR_SUCCESS;
		return true;
	}

	bool closeHandle(HANDLE handle, DWORD &error) noexcept override
	{
		++closeCount;
		lastCloseHandle = handle;
		if (!closeResult) {
			error = closeError;
			return false;
		}
		openHandles.erase(handle);
		error = ERROR_SUCCESS;
		return true;
	}

	std::optional<DWORD> nextCreateError;
	std::optional<DWORD> nextWaitResult;
	std::optional<DWORD> nextWaitError;
	std::function<void()> onWait;
	std::function<void()> onRelease;
	bool releaseResult = true;
	DWORD releaseError = ERROR_ACCESS_DENIED;
	bool closeResult = true;
	DWORD closeError = ERROR_INVALID_HANDLE;
	int createCount = 0;
	int waitCount = 0;
	int releaseCount = 0;
	int closeCount = 0;
	HANDLE lastWaitHandle = nullptr;
	HANDLE lastReleaseHandle = nullptr;
	HANDLE lastCloseHandle = nullptr;
	std::unordered_set<HANDLE> openHandles;

private:
	std::uintptr_t nextHandle = 1;
};

class YouTubeAccountProviderTestAccess final {
public:
	YouTubeAccountProviderTestAccess(std::unique_ptr<YouTubeAccountAuthorizationPort> authorization,
					 std::unique_ptr<YouTubeAccountTokenPort> token,
					 std::unique_ptr<YouTubeAccountDiscoveryPort> discovery,
					 YouTubeAccountRefreshTokenStore &store,
					 YouTubeAccountProfileOperationLockProvider &profileOperationLockProvider,
					 std::string profileBinding, YouTubeAccountSelectionCommitter committer,
					 QString clientId = QStringLiteral("test-client-id"))
		: provider_(new YouTubeAccountProvider(std::move(clientId), std::move(authorization), std::move(token),
						       std::move(discovery), store, profileOperationLockProvider,
						       profileBinding, std::move(committer), nullptr)),
		  profileBinding_(std::move(profileBinding))
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
	YouTubeAccountProviderRestoreStatus restoreSavedState(std::optional<YouTubeAccountSelection> selection) noexcept
	{
		return provider_->restoreSavedState(profileBinding_, std::move(selection));
	}
	YouTubeAccountProviderRestoreStatus restoreSavedState(std::string profileBinding,
							      std::optional<YouTubeAccountSelection> selection) noexcept
	{
		return provider_->restoreSavedState(std::move(profileBinding), std::move(selection));
	}
	YouTubeAccountProviderRestoreStatus restoreSavedStateUnderHeldOperationLock(
		std::string profileBinding, std::optional<YouTubeAccountSelection> selection,
		const YouTubeAccountProfileOperationLock &heldLock) noexcept
	{
		return provider_->restoreSavedStateUnderHeldOperationLock(std::move(profileBinding), std::move(selection),
												 heldLock);
	}
	void markExternalOperationReleaseFailed() noexcept { provider_->markExternalOperationReleaseFailed(); }
	void externalOperationLockReleased() noexcept { provider_->externalOperationLockReleased(); }
	bool cancel(YouTubeAccountLease lease) noexcept { return provider_->cancel(lease); }
	bool invalidateContext() noexcept { return provider_->invalidateContext(); }
	bool shutdown() noexcept { return provider_->shutdown(); }
	YouTubeAccountProviderSnapshot snapshot() const { return provider_->snapshot(); }

private:
	std::unique_ptr<YouTubeAccountProvider> provider_;
	std::string profileBinding_;
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
using easy_multistream::YouTubeAccountCredentialScope;
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
using easy_multistream::YouTubeAccountProviderRestoreStatus;
using easy_multistream::YouTubeAccountProviderSnapshot;
using easy_multistream::YouTubeAccountProviderStage;
using easy_multistream::YouTubeAccountProviderStartStatus;
using easy_multistream::YouTubeAccountRefreshTokenStore;
using easy_multistream::YouTubeAccountSelection;
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
using easy_multistream::FakeYouTubeAccountProfileOperationLockApi;
using easy_multistream::YouTubeAccountProfileOperationLock;
using easy_multistream::YouTubeAccountProfileOperationLockProvider;

constexpr char kAccessToken[] = "access-token-sentinel";
constexpr char kRefreshToken[] = "refresh-token-sentinel";
constexpr char kAuthorizationCode[] = "authorization-code-sentinel";
constexpr char kVerifier[] = "verifier-sentinel-012345678901234567890123456789012345";
constexpr char kProfileA[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kProfileB[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

YouTubeAccountSelection savedSelection(const char *suffix)
{
	return {std::string("channel-") + suffix, std::string("Channel ") + suffix, std::string("stream-") + suffix,
		std::string("Stream ") + suffix};
}

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
		if (onCancel) {
			onCancel();
		}
		return true;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		handler = {};
		return shutdownResult;
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
	bool shutdownResult = true;
	std::optional<GoogleOAuthLoopbackAttempt> lastAttempt;
	std::optional<GoogleOAuthLoopbackAttempt> lastCancelled;
	QString lastClientId;
	GoogleOAuthConsentMode lastConsentMode = GoogleOAuthConsentMode::Standard;
	std::optional<GoogleOAuthAuthorizationStartStatus> forcedStartStatus;
	CompletionHandler handler;
	std::function<void()> onCancel;
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
		return shutdownResult;
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
	bool shutdownResult = true;
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
		return shutdownResult;
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
	bool shutdownResult = true;
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

class FakeCredentialStore final : public YouTubeAccountRefreshTokenStore {
public:
	struct Record final {
		YouTubeAccountCredentialScope scope;
		SecureBuffer secret;
	};

	CredentialResult write(const YouTubeAccountCredentialScope &scope, std::string_view value) noexcept override
	{
		++writeCount;
		writeScopes.push_back(scope);
		lastWritten = SecureBuffer::copyOf(value);
		if (writeError != CredentialError::None ||
		    (failWriteOnCall.has_value() && writeCount == *failWriteOnCall)) {
			return {writeError == CredentialError::None ? CredentialError::Unavailable : writeError, 1};
		}
		Record *record = findByProfile(scope.profileBinding);
		if (record == nullptr) {
			records.push_back({scope, SecureBuffer{}});
			record = &records.back();
		}
		record->scope = scope;
		record->secret = SecureBuffer::copyOf(value);
		stored = SecureBuffer::copyOf(value);
		return {};
	}

	CredentialReadResult read(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++readCount;
		readScopes.push_back(scope);
		if (readError != CredentialError::None) {
			return {{readError, 1}, {}};
		}
		const Record *record = findByProfile(scope.profileBinding);
		if (record == nullptr || record->secret.empty()) {
			return {{CredentialError::NotFound, 0}, {}};
		}
		if (record->scope.channelId != scope.channelId) {
			return {{CredentialError::ScopeMismatch, 1}, {}};
		}
		return {{}, SecureBuffer::copyOf(record->secret.view())};
	}

	CredentialResult erase(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++eraseCount;
		eraseScopes.push_back(scope);
		if (eraseError != CredentialError::None) {
			return {eraseError, 1};
		}
		Record *record = findByProfile(scope.profileBinding);
		if (record == nullptr || record->secret.empty()) {
			return {};
		}
		if (record->scope.channelId != scope.channelId) {
			return {CredentialError::ScopeMismatch, 1};
		}
		record->secret.clear();
		stored.clear();
		return {};
	}

	CredentialStatus status(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++statusCount;
		statusScopes.push_back(scope);
		if (onStatus) {
			onStatus();
		}
		if (forcedStatus.has_value()) {
			return *forcedStatus;
		}
		const Record *record = findByProfile(scope.profileBinding);
		if (record == nullptr || record->secret.empty()) {
			return {CredentialState::Missing, {CredentialError::NotFound, 0}};
		}
		if (record->scope.channelId != scope.channelId) {
			return {CredentialState::NeedsReauthorization, {CredentialError::ScopeMismatch, 1}};
		}
		return {CredentialState::Present, {}};
	}

	void forceStatus(CredentialState state)
	{
		if (state == CredentialState::Present) {
			forcedStatus = CredentialStatus{CredentialState::Present, {}};
		} else if (state == CredentialState::Missing) {
			forcedStatus = CredentialStatus{CredentialState::Missing, {CredentialError::NotFound, 0}};
		} else if (state == CredentialState::NeedsReauthorization) {
			forcedStatus = CredentialStatus{CredentialState::NeedsReauthorization,
							{CredentialError::ScopeMismatch, 1}};
		} else {
			forcedStatus =
				CredentialStatus{CredentialState::Unavailable, {CredentialError::Unavailable, 1}};
		}
	}

	/*
	 * Records model the production Windows store: one target per profile and
	 * the current channel binding in credential metadata.
	 */
	Record *findByProfile(std::string_view profileBinding) noexcept
	{
		for (Record &record : records) {
			if (record.scope.profileBinding == profileBinding) {
				return &record;
			}
		}
		return nullptr;
	}

	const Record *findByProfile(std::string_view profileBinding) const noexcept
	{
		for (const Record &record : records) {
			if (record.scope.profileBinding == profileBinding) {
				return &record;
			}
		}
		return nullptr;
	}

	int writeCount = 0;
	int readCount = 0;
	int eraseCount = 0;
	int statusCount = 0;
	SecureBuffer stored;
	SecureBuffer lastWritten;
	CredentialError writeError = CredentialError::None;
	std::optional<int> failWriteOnCall;
	CredentialError readError = CredentialError::None;
	CredentialError eraseError = CredentialError::None;
	std::optional<CredentialStatus> forcedStatus;
	std::function<void()> onStatus;
	std::vector<Record> records;
	std::vector<YouTubeAccountCredentialScope> writeScopes;
	std::vector<YouTubeAccountCredentialScope> readScopes;
	std::vector<YouTubeAccountCredentialScope> eraseScopes;
	std::vector<YouTubeAccountCredentialScope> statusScopes;
};

struct Fixture final {
	std::unique_ptr<FakeAuthorizationPort> authorization;
	std::unique_ptr<FakeTokenPort> token;
	std::unique_ptr<FakeDiscoveryPort> discovery;
	FakeCredentialStore vault;
	FakeYouTubeAccountProfileOperationLockApi operationLockApi;
	std::unique_ptr<YouTubeAccountProfileOperationLockProvider> operationLockProvider;
	std::string profileBinding = kProfileA;
	bool commitResult = true;
	int commitCount = 0;
	std::optional<int> failCommitOnCall;
	std::optional<easy_multistream::YouTubeAccountSelection> committedSelection;
	std::vector<std::optional<easy_multistream::YouTubeAccountSelection>> commitHistory;
	std::unique_ptr<YouTubeAccountProviderTestAccess> provider;

	Fixture()
	{
		operationLockProvider = std::make_unique<YouTubeAccountProfileOperationLockProvider>(operationLockApi);
		authorization = std::make_unique<FakeAuthorizationPort>();
		token = std::make_unique<FakeTokenPort>();
		discovery = std::make_unique<FakeDiscoveryPort>();
		FakeAuthorizationPort *authorizationPointer = authorization.get();
		FakeTokenPort *tokenPointer = token.get();
		FakeDiscoveryPort *discoveryPointer = discovery.get();
		provider = std::make_unique<YouTubeAccountProviderTestAccess>(
			std::move(authorization), std::move(token), std::move(discovery), vault, *operationLockProvider,
			profileBinding,
			[this](const std::optional<easy_multistream::YouTubeAccountSelection> &selection) {
				++commitCount;
				commitHistory.push_back(selection);
				if (failCommitOnCall.has_value() && commitCount == *failCommitOnCall) {
					return false;
				}
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
		CHECK(fixture.commitCount == 2);
		CHECK(!fixture.committedSelection.has_value());
		CHECK(fixture.commitHistory.size() == 2);
		CHECK(fixture.commitHistory.front().has_value());
		CHECK(!fixture.commitHistory.back().has_value());
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
		CHECK(fixture.vault.writeCount == 0);
		CHECK(fixture.vault.eraseCount == 0);
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
		fixture.vault.failWriteOnCall = fixture.vault.writeCount + 1;
		fixture.failCommitOnCall = fixture.commitCount + 2;

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
		fixture.vault.forceStatus(CredentialState::Present);
		CHECK(fixture.provider->restoreSavedState(kProfileA, savedSelection("prior")) ==
		      YouTubeAccountProviderRestoreStatus::Configured);
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

void testExplicitReauthorizationReplacesCorruptCredential()
{
	Fixture fixture;
	const auto prior = savedSelection("corrupt");
	fixture.vault.forcedStatus =
		CredentialStatus{CredentialState::NeedsReauthorization, {CredentialError::CorruptData, 1}};
	CHECK(fixture.provider->restoreSavedState(kProfileA, prior) ==
	      YouTubeAccountProviderRestoreStatus::ReauthorizationRequired);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::NeedsReauthorization);

	fixture.vault.forcedStatus.reset();
	fixture.vault.readError = CredentialError::CorruptData;
	startAndReachChannelListing(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({{"replacement-channel", "Replacement channel"}}));
	fixture.discoveryPointer->completeStreams(
		streamPage({{"replacement-stream", "replacement-channel", "Replacement stream"}}));

	const auto snapshot = fixture.provider->snapshot();
	CHECK(snapshot.stage == YouTubeAccountProviderStage::Connected);
	CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Connected);
	CHECK(snapshot.account.channelId == "replacement-channel");
	CHECK(snapshot.account.streamId == "replacement-stream");
	CHECK(fixture.vault.writeCount == 1);
	CHECK(!fixture.vault.writeScopes.empty());
	if (!fixture.vault.writeScopes.empty()) {
		CHECK(fixture.vault.writeScopes.back().profileBinding == kProfileA);
		CHECK(fixture.vault.writeScopes.back().channelId == "replacement-channel");
	}
	CHECK(fixture.committedSelection.has_value());
	CHECK(fixture.committedSelection->channelId == "replacement-channel");
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

void checkNoConnectionPortsStarted(const Fixture &fixture)
{
	CHECK(fixture.authorizationPointer->startCount == 0);
	CHECK(fixture.tokenPointer->startCount == 0);
	CHECK(fixture.discoveryPointer->channelStartCount == 0);
	CHECK(fixture.discoveryPointer->streamStartCount == 0);
	CHECK(fixture.commitCount == 0);
}

void testSavedStateRestoreUsesOnlyCredentialStatus()
{
	{
		Fixture fixture;
		fixture.vault.forceStatus(CredentialState::Present);
		const YouTubeAccountSelection selection = savedSelection("saved");
		CHECK(fixture.provider->restoreSavedState(selection) ==
		      YouTubeAccountProviderRestoreStatus::Configured);
		const auto snapshot = fixture.provider->snapshot();
		CHECK(snapshot.stage == YouTubeAccountProviderStage::Configured);
		CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Configured);
		CHECK(snapshot.account.channelId == selection.channelId);
		CHECK(snapshot.account.channelLabel == selection.channelLabel);
		CHECK(snapshot.account.streamId == selection.streamId);
		CHECK(snapshot.account.streamLabel == selection.streamLabel);
		CHECK(snapshot.account.connectionLease.has_value());
		CHECK(fixture.vault.statusCount == 1);
		CHECK(fixture.vault.readCount == 0);
		CHECK(fixture.vault.writeCount == 0);
		CHECK(fixture.vault.eraseCount == 0);
		checkNoConnectionPortsStarted(fixture);
	}

	{
		Fixture fixture;
		fixture.vault.forceStatus(CredentialState::Present);
		CHECK(fixture.provider->restoreSavedState(std::nullopt) ==
		      YouTubeAccountProviderRestoreStatus::SetupRequired);
		const auto snapshot = fixture.provider->snapshot();
		CHECK(snapshot.stage == YouTubeAccountProviderStage::Idle);
		CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Disconnected);
		CHECK(!snapshot.account.connectionLease.has_value());
		CHECK(fixture.vault.statusCount == 0);
		CHECK(fixture.vault.eraseCount == 0);
		checkNoConnectionPortsStarted(fixture);
	}

	{
		Fixture fixture;
		fixture.vault.forceStatus(CredentialState::Missing);
		const YouTubeAccountSelection selection = savedSelection("missing");
		CHECK(fixture.provider->restoreSavedState(selection) ==
		      YouTubeAccountProviderRestoreStatus::ReauthorizationRequired);
		const auto snapshot = fixture.provider->snapshot();
		CHECK(snapshot.stage == YouTubeAccountProviderStage::NeedsReauthorization);
		CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::NeedsReauthorization);
		CHECK(snapshot.account.failure == easy_multistream::YouTubeAccountFailure::ReauthorizationRequired);
		CHECK(snapshot.account.channelId == selection.channelId);
		CHECK(snapshot.account.streamId == selection.streamId);
		CHECK(!snapshot.account.connectionLease.has_value());
		CHECK(fixture.vault.statusCount == 1);
		checkNoConnectionPortsStarted(fixture);
	}

	{
		Fixture fixture;
		fixture.vault.forceStatus(CredentialState::Unavailable);
		const YouTubeAccountSelection selection = savedSelection("unavailable");
		CHECK(fixture.provider->restoreSavedState(selection) ==
		      YouTubeAccountProviderRestoreStatus::CredentialUnavailable);
		const auto snapshot = fixture.provider->snapshot();
		CHECK(snapshot.stage == YouTubeAccountProviderStage::Unavailable);
		CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Unavailable);
		CHECK(snapshot.account.failure == easy_multistream::YouTubeAccountFailure::CredentialUnavailable);
		CHECK(snapshot.account.channelId == selection.channelId);
		CHECK(snapshot.account.streamId == selection.streamId);
		CHECK(!snapshot.account.connectionLease.has_value());
		CHECK(fixture.vault.statusCount == 1);
		checkNoConnectionPortsStarted(fixture);
	}
}

void testSavedStateRestoreRejectsInvalidInputsAndStatusCombinations()
{
	{
		Fixture fixture;
		YouTubeAccountSelection invalid = savedSelection("invalid");
		invalid.streamId.clear();
		CHECK(fixture.provider->restoreSavedState(invalid) ==
		      YouTubeAccountProviderRestoreStatus::InvalidSelection);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
		CHECK(fixture.provider->snapshot().account.state == easy_multistream::YouTubeAccountState::Failed);
		CHECK(fixture.vault.statusCount == 0);
		checkNoConnectionPortsStarted(fixture);
	}

	for (const CredentialStatus inconsistent : {
		     CredentialStatus{CredentialState::Present, {CredentialError::AccessDenied, 1}},
		     CredentialStatus{CredentialState::Missing, {CredentialError::None, 0}},
		     CredentialStatus{CredentialState::Unavailable, {CredentialError::None, 0}},
		     CredentialStatus{CredentialState::Missing, {CredentialError::Unavailable, 1}},
	     }) {
		Fixture fixture;
		fixture.vault.forcedStatus = inconsistent;
		CHECK(fixture.provider->restoreSavedState(savedSelection("inconsistent")) ==
		      YouTubeAccountProviderRestoreStatus::CredentialUnavailable);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Unavailable);
		CHECK(fixture.provider->snapshot().account.state == easy_multistream::YouTubeAccountState::Unavailable);
		CHECK(fixture.vault.statusCount == 1);
		checkNoConnectionPortsStarted(fixture);
	}
}

void testSavedStateRestoreInvalidatesOldProfileWork()
{
	Fixture fixture;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	const YouTubeAccountLease oldLease = activeLease(fixture);
	const auto oldHandler = fixture.authorizationPointer->handler;
	CHECK(fixture.provider->restoreSavedState(savedSelection("busy")) == YouTubeAccountProviderRestoreStatus::Busy);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(fixture.provider->invalidateContext());

	fixture.vault.forceStatus(CredentialState::Present);
	const auto restoredSelection = savedSelection("new-profile");
	CHECK(fixture.provider->restoreSavedState(kProfileB, restoredSelection) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	const auto restored = fixture.provider->snapshot();
	CHECK(restored.account.connectionLease.has_value());
	CHECK(restored.account.connectionLease->generation != oldLease.generation);

	if (oldHandler) {
		oldHandler(authorizationSuccess({oldLease.generation, oldLease.attempt}));
	}
	CHECK(fixture.tokenPointer->startCount == 0);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Configured);
	CHECK(fixture.provider->snapshot().account.channelId == restoredSelection.channelId);
	CHECK(fixture.provider->snapshot().account.connectionLease == restored.account.connectionLease);
}

void testConfiguredReauthorizationPreservesSavedConnectionUntilSuccess()
{
	Fixture fixture;
	fixture.vault.forceStatus(CredentialState::Present);
	const YouTubeAccountSelection saved = savedSelection("saved");
	CHECK(fixture.provider->restoreSavedState(saved) == YouTubeAccountProviderRestoreStatus::Configured);
	const YouTubeAccountLease savedLease =
		fixture.provider->snapshot().account.connectionLease.value_or(YouTubeAccountLease{});

	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	CHECK(fixture.provider->cancel(activeLease(fixture)));
	auto snapshot = fixture.provider->snapshot();
	CHECK(snapshot.stage == YouTubeAccountProviderStage::Configured);
	CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Configured);
	CHECK(snapshot.account.channelId == saved.channelId);
	CHECK(snapshot.account.streamId == saved.streamId);
	CHECK(snapshot.account.connectionLease == savedLease);

	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	const YouTubeAccountLease failedLease = activeLease(fixture);
	fixture.authorizationPointer->complete(authorizationFailure({failedLease.generation, failedLease.attempt},
								    GoogleOAuthProviderError::AccessDenied));
	snapshot = fixture.provider->snapshot();
	CHECK(snapshot.stage == YouTubeAccountProviderStage::Configured);
	CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Configured);
	CHECK(snapshot.account.channelId == saved.channelId);
	CHECK(snapshot.account.streamId == saved.streamId);
	CHECK(snapshot.account.connectionLease == savedLease);

	startAndReachChannelListing(fixture);
	const YouTubeAccountLease replacement = activeLease(fixture);
	fixture.discoveryPointer->completeChannels(channelPage({{"new-channel", "New channel"}}));
	fixture.discoveryPointer->completeStreams(streamPage({{"new-stream", "new-channel", "New stream"}}));
	snapshot = fixture.provider->snapshot();
	CHECK(snapshot.stage == YouTubeAccountProviderStage::Connected);
	CHECK(snapshot.account.state == easy_multistream::YouTubeAccountState::Connected);
	CHECK(snapshot.account.channelId == "new-channel");
	CHECK(snapshot.account.streamId == "new-stream");
	CHECK(snapshot.account.connectionLease == replacement);
}

void testSavedStateRestoreReevaluatesCredentialPresence()
{
	Fixture fixture;
	const YouTubeAccountSelection selection = savedSelection("repeat");
	fixture.vault.forceStatus(CredentialState::Missing);
	CHECK(fixture.provider->restoreSavedState(selection) ==
	      YouTubeAccountProviderRestoreStatus::ReauthorizationRequired);
	const std::uint64_t missingGeneration = fixture.provider->snapshot().account.generation;

	fixture.vault.forceStatus(CredentialState::Present);
	CHECK(fixture.provider->restoreSavedState(selection) == YouTubeAccountProviderRestoreStatus::Configured);
	const auto connected = fixture.provider->snapshot();
	CHECK(connected.account.generation != missingGeneration);
	CHECK(connected.account.connectionLease.has_value());
	const YouTubeAccountLease connectedLease = connected.account.connectionLease.value_or(YouTubeAccountLease{});

	fixture.vault.forceStatus(CredentialState::Missing);
	CHECK(fixture.provider->restoreSavedState(selection) ==
	      YouTubeAccountProviderRestoreStatus::ReauthorizationRequired);
	const auto missingAgain = fixture.provider->snapshot();
	CHECK(missingAgain.account.generation != connected.account.generation);
	CHECK(!missingAgain.account.connectionLease.has_value());
	CHECK(!fixture.provider->snapshot().account.lease.has_value());
	CHECK(fixture.vault.statusCount == 3);
	CHECK(fixture.provider->snapshot().account.channelId == selection.channelId);

	CHECK(fixture.provider->restoreSavedState(std::nullopt) == YouTubeAccountProviderRestoreStatus::SetupRequired);
	const auto unconfigured = fixture.provider->snapshot();
	CHECK(unconfigured.account.generation != missingAgain.account.generation);
	CHECK(unconfigured.account.state == easy_multistream::YouTubeAccountState::Disconnected);
	CHECK(unconfigured.account.channelId.empty());
	CHECK(unconfigured.account.streamId.empty());
	CHECK(fixture.vault.statusCount == 3);
	CHECK(fixture.vault.eraseCount == 0);
	CHECK(!fixture.provider->snapshot().account.connectionLease.has_value());
	CHECK(connectedLease.generation != 0 && connectedLease.attempt != 0);
	checkNoConnectionPortsStarted(fixture);
}

void testSavedStateRestoreIsThreadBoundAndReentrySafe()
{
	Fixture fixture;
	std::optional<YouTubeAccountProviderRestoreStatus> wrongThreadResult;
	std::thread worker(
		[&]() { wrongThreadResult = fixture.provider->restoreSavedState(savedSelection("thread")); });
	worker.join();
	CHECK(wrongThreadResult == YouTubeAccountProviderRestoreStatus::WrongThread);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Idle);

	std::optional<YouTubeAccountProviderStartStatus> nestedStart;
	std::optional<YouTubeAccountProviderRestoreStatus> nestedRestore;
	std::optional<bool> nestedShutdown;
	fixture.vault.forceStatus(CredentialState::Present);
	fixture.vault.onStatus = [&]() {
		nestedStart = fixture.provider->startConnection();
		nestedRestore = fixture.provider->restoreSavedState(savedSelection("nested"));
		nestedShutdown = fixture.provider->shutdown();
	};
	CHECK(fixture.provider->restoreSavedState(savedSelection("outer")) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(nestedStart == YouTubeAccountProviderStartStatus::Busy);
	CHECK(nestedRestore == YouTubeAccountProviderRestoreStatus::Busy);
	CHECK(nestedShutdown.has_value() && !*nestedShutdown);
	CHECK(fixture.provider->snapshot().account.channelId == "channel-outer");

	fixture.vault.onStatus = {};
	CHECK(fixture.provider->shutdown());
	CHECK(fixture.provider->restoreSavedState(savedSelection("closed")) ==
	      YouTubeAccountProviderRestoreStatus::Closed);
	CHECK(fixture.vault.statusCount == 1);
}

void testProfileAndChannelScopeRoutingAndMismatch()
{
	Fixture fixture;
	connectFixture(fixture, "channel-a", "stream-a");
	CHECK(!fixture.vault.writeScopes.empty());
	if (!fixture.vault.writeScopes.empty()) {
		const auto &scope = fixture.vault.writeScopes.back();
		CHECK(scope.profileBinding == kProfileA);
		CHECK(scope.channelId == "channel-a");
	}

	const YouTubeAccountSelection restored{"channel-a", "Channel A", "stream-a", "Stream A"};
	CHECK(fixture.provider->restoreSavedState(kProfileB, restored) ==
	      YouTubeAccountProviderRestoreStatus::ReauthorizationRequired);
	CHECK(!fixture.vault.statusScopes.empty());
	if (!fixture.vault.statusScopes.empty()) {
		const auto &scope = fixture.vault.statusScopes.back();
		CHECK(scope.profileBinding == kProfileB);
		CHECK(scope.channelId == restored.channelId);
	}
	CHECK(fixture.provider->restoreSavedState(kProfileA, restored) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(fixture.vault.statusScopes.back().profileBinding == kProfileA);
	CHECK(fixture.vault.statusScopes.back().channelId == "channel-a");

	fixture.vault.forcedStatus =
		CredentialStatus{CredentialState::NeedsReauthorization, {CredentialError::ScopeMismatch, 1}};
	const auto mismatched = savedSelection("mismatch");
	CHECK(fixture.provider->restoreSavedState(kProfileB, mismatched) ==
	      YouTubeAccountProviderRestoreStatus::ReauthorizationRequired);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::NeedsReauthorization);
	CHECK(fixture.vault.statusScopes.back().profileBinding == kProfileB);
	CHECK(fixture.vault.statusScopes.back().channelId == mismatched.channelId);

	CHECK(fixture.provider->restoreSavedState(kProfileA, std::nullopt) ==
	      YouTubeAccountProviderRestoreStatus::SetupRequired);
	const std::size_t statusCount = fixture.vault.statusScopes.size();
	CHECK(fixture.provider->restoreSavedState(kProfileB, std::nullopt) ==
	      YouTubeAccountProviderRestoreStatus::SetupRequired);
	CHECK(fixture.vault.statusScopes.size() == statusCount);
}

void testInvalidProfileBindingFailsClosedWithoutCredentialAccess()
{
	Fixture fixture;
	const std::string invalid(64U, 'A');
	CHECK(fixture.provider->restoreSavedState(invalid, savedSelection("invalid-profile")) ==
	      YouTubeAccountProviderRestoreStatus::InvalidProfileBinding);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::InvalidProfileBinding);
	checkNoConnectionPortsStarted(fixture);

	FakeCredentialStore store;
	FakeYouTubeAccountProfileOperationLockApi operationLockApi;
	YouTubeAccountProfileOperationLockProvider operationLockProvider(operationLockApi);
	YouTubeAccountProviderTestAccess provider(std::make_unique<FakeAuthorizationPort>(),
						  std::make_unique<FakeTokenPort>(),
						  std::make_unique<FakeDiscoveryPort>(), store, operationLockProvider,
						  invalid,
						  [](const std::optional<YouTubeAccountSelection> &) { return true; });
	CHECK(provider.startConnection() == YouTubeAccountProviderStartStatus::InvalidProfileBinding);
	CHECK(store.statusCount == 0);
}

void testFailureCleanupRejectsSynchronousLifecycleReentry()
{
	Fixture fixture;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	const YouTubeAccountLease lease = activeLease(fixture);
	std::optional<bool> nestedInvalidate;
	std::optional<bool> nestedShutdown;
	fixture.authorizationPointer->onCancel = [&]() {
		nestedInvalidate = fixture.provider->invalidateContext();
		nestedShutdown = fixture.provider->shutdown();
	};

	fixture.authorizationPointer->complete(
		authorizationFailure({lease.generation, lease.attempt}, GoogleOAuthProviderError::AccessDenied));

	CHECK(nestedInvalidate.has_value() && !*nestedInvalidate);
	CHECK(nestedShutdown.has_value() && !*nestedShutdown);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Failed);
	CHECK(fixture.provider->snapshot().account.state == easy_multistream::YouTubeAccountState::Failed);
}

void testInvalidStartAndCommitterFailureDoNotMutateState()
{
	FakeCredentialStore vault;
	FakeYouTubeAccountProfileOperationLockApi operationLockApi;
	YouTubeAccountProfileOperationLockProvider operationLockProvider(operationLockApi);
	int commits = 0;
	YouTubeAccountProviderTestAccess provider(
		std::make_unique<FakeAuthorizationPort>(), std::make_unique<FakeTokenPort>(),
		std::make_unique<FakeDiscoveryPort>(), vault, operationLockProvider, kProfileA,
		[&](const std::optional<easy_multistream::YouTubeAccountSelection> &) {
			++commits;
			return true;
		},
		QString{});
	CHECK(provider.startConnection() == YouTubeAccountProviderStartStatus::InvalidClientId);
	CHECK(provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	(void)commits;
}

void testProfileOperationLockSpansInteractiveTransaction()
{
	Fixture fixture;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	CHECK(fixture.operationLockApi.createCount == 1);
	CHECK(fixture.operationLockApi.waitCount == 1);
	CHECK(fixture.operationLockApi.releaseCount == 0);
	CHECK(fixture.operationLockApi.closeCount == 0);

	completeAuthorization(fixture);
	CHECK(fixture.operationLockApi.releaseCount == 0);
	completeToken(fixture);
	CHECK(fixture.operationLockApi.releaseCount == 0);
	fixture.discoveryPointer->completeChannels(channelPage({{"channel-one", "Channel One"}}));
	CHECK(fixture.operationLockApi.releaseCount == 0);
	fixture.discoveryPointer->completeStreams(streamPage({{"stream-one", "channel-one", "Everyday"}}));
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Connected);
	CHECK(fixture.vault.writeCount == 1);
	CHECK(fixture.commitCount == 1);
	CHECK(fixture.operationLockApi.releaseCount == 1);
	CHECK(fixture.operationLockApi.closeCount == 1);
	CHECK(fixture.operationLockApi.openHandles.empty());
}

void testProfileOperationLockFailureDoesNotMutateStart()
{
	Fixture fixture;
	const auto before = fixture.provider->snapshot();
	fixture.operationLockApi.nextWaitResult = WAIT_TIMEOUT;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Busy);
	CHECK(fixture.provider->snapshot().stage == before.stage);
	CHECK(fixture.provider->snapshot().revision == before.revision);
	CHECK(fixture.provider->snapshot().account.state == before.account.state);
	CHECK(fixture.authorizationPointer->startCount == 0);
	CHECK(fixture.operationLockApi.releaseCount == 0);
	CHECK(fixture.operationLockApi.closeCount == 1);

	fixture.operationLockApi.nextCreateError = ERROR_ACCESS_DENIED;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::OperationFailed);
	CHECK(fixture.provider->snapshot().stage == before.stage);
	CHECK(fixture.provider->snapshot().revision == before.revision);
	CHECK(fixture.authorizationPointer->startCount == 0);
	CHECK(fixture.operationLockApi.releaseCount == 0);
}

void testRecoveredProfileOperationLockContinuesNormally()
{
	Fixture fixture;
	fixture.operationLockApi.nextWaitResult = WAIT_ABANDONED;
	CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
	CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Authorizing);
	CHECK(fixture.operationLockApi.releaseCount == 0);
	CHECK(fixture.provider->cancel(activeLease(fixture)));
	CHECK(fixture.operationLockApi.releaseCount == 1);
}

void testProfileOperationLockSpansRestoreStatusAndPreservesRejectedRestore()
{
	Fixture fixture;
	const YouTubeAccountSelection selection = savedSelection("restore-lock");
	fixture.vault.forcedStatus = CredentialStatus{CredentialState::Present, {CredentialError::None, 0}};
	bool lockHeldDuringStatus = false;
	fixture.vault.onStatus = [&]() {
		lockHeldDuringStatus = fixture.operationLockApi.releaseCount == 0;
	};
	CHECK(fixture.provider->restoreSavedState(kProfileA, selection) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(lockHeldDuringStatus);
	CHECK(fixture.operationLockApi.createCount == 1);
	CHECK(fixture.operationLockApi.waitCount == 1);
	CHECK(fixture.operationLockApi.releaseCount == 1);
	CHECK(fixture.operationLockApi.closeCount == 1);

	const auto beforeBusy = fixture.provider->snapshot();
	fixture.operationLockApi.nextWaitResult = WAIT_TIMEOUT;
	CHECK(fixture.provider->restoreSavedState(kProfileB, savedSelection("other")) ==
	      YouTubeAccountProviderRestoreStatus::Busy);
	const auto afterBusy = fixture.provider->snapshot();
	CHECK(afterBusy.revision == beforeBusy.revision);
	CHECK(afterBusy.stage == beforeBusy.stage);
	CHECK(afterBusy.account.state == beforeBusy.account.state);
	CHECK(afterBusy.account.channelId == beforeBusy.account.channelId);
	CHECK(afterBusy.account.streamId == beforeBusy.account.streamId);
	CHECK(fixture.vault.statusScopes.size() == 1);

	fixture.operationLockApi.nextCreateError = ERROR_ACCESS_DENIED;
	CHECK(fixture.provider->restoreSavedState(kProfileB, savedSelection("unavailable")) ==
	      YouTubeAccountProviderRestoreStatus::OperationFailed);
	const auto afterUnavailable = fixture.provider->snapshot();
	CHECK(afterUnavailable.revision == beforeBusy.revision);
	CHECK(afterUnavailable.stage == beforeBusy.stage);
	CHECK(afterUnavailable.account.state == beforeBusy.account.state);
	CHECK(afterUnavailable.account.channelId == beforeBusy.account.channelId);
	CHECK(afterUnavailable.account.streamId == beforeBusy.account.streamId);
	CHECK(fixture.vault.statusScopes.size() == 1);

	fixture.vault.forcedStatus = CredentialStatus{CredentialState::Present, {CredentialError::None, 0}};
	CHECK(fixture.provider->restoreSavedState(kProfileA, savedSelection("current")) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(fixture.vault.statusScopes.back().profileBinding == kProfileA);
}

void testHeldProfileOperationLockRestoreDoesNotReacquireOrRelease()
{
	Fixture fixture;
	YouTubeAccountProfileOperationLock heldLock;
	const auto lockResult = fixture.operationLockProvider->acquire(kProfileA, heldLock);
	CHECK(lockResult.acquired());
	CHECK(heldLock.acquiredFor(kProfileA));

	fixture.vault.forcedStatus = CredentialStatus{CredentialState::Present, {CredentialError::None, 0}};
	bool lockHeldDuringStatus = false;
	fixture.vault.onStatus = [&]() { lockHeldDuringStatus = heldLock.acquiredFor(kProfileA); };
	const auto before = fixture.provider->snapshot();
	CHECK(fixture.provider->restoreSavedStateUnderHeldOperationLock(kProfileA, savedSelection("borrowed"), heldLock) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(lockHeldDuringStatus);
	CHECK(heldLock.acquired());
	CHECK(fixture.operationLockApi.createCount == 1);
	CHECK(fixture.operationLockApi.waitCount == 1);
	// The provider borrowed the caller's lock. Only the explicit cleanup below
	// is allowed to release it.
	CHECK(fixture.operationLockApi.releaseCount == 0);
	CHECK(fixture.operationLockApi.closeCount == 0);
	CHECK(fixture.vault.statusCount == 1);
	CHECK(fixture.provider->snapshot().revision != before.revision);
	CHECK(heldLock.release());
	fixture.provider->externalOperationLockReleased();
	CHECK(fixture.operationLockApi.releaseCount == 1);
	CHECK(fixture.operationLockApi.closeCount == 1);
}

void testHeldProfileOperationLockRestoreSkipsCredentialForMissingSelection()
{
	Fixture fixture;
	YouTubeAccountProfileOperationLock heldLock;
	CHECK(fixture.operationLockProvider->acquire(kProfileA, heldLock).acquired());

	const auto before = fixture.provider->snapshot();
	CHECK(fixture.provider->restoreSavedStateUnderHeldOperationLock(kProfileA, std::nullopt, heldLock) ==
	      YouTubeAccountProviderRestoreStatus::SetupRequired);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(heldLock.acquired());
	CHECK(fixture.operationLockApi.releaseCount == 0);
	CHECK(fixture.provider->snapshot().revision != before.revision);
	CHECK(heldLock.release());
	fixture.provider->externalOperationLockReleased();
}

void testHeldProfileOperationLockRestoreRejectsWrongOrUnheldLockWithoutMutation()
{
	Fixture fixture;
	const auto before = fixture.provider->snapshot();
	YouTubeAccountProfileOperationLock heldLock;
	CHECK(fixture.operationLockProvider->acquire(kProfileA, heldLock).acquired());

	CHECK(fixture.provider->restoreSavedStateUnderHeldOperationLock(kProfileB, savedSelection("wrong-binding"), heldLock) ==
	      YouTubeAccountProviderRestoreStatus::OperationFailed);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(fixture.provider->snapshot().revision == before.revision);
	CHECK(fixture.provider->snapshot().stage == before.stage);
	CHECK(heldLock.acquired());

	YouTubeAccountProfileOperationLock unheldLock;
	CHECK(fixture.provider->restoreSavedStateUnderHeldOperationLock(kProfileA, savedSelection("unheld"), unheldLock) ==
	      YouTubeAccountProviderRestoreStatus::OperationFailed);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(fixture.provider->snapshot().revision == before.revision);
	CHECK(heldLock.acquired());

	std::optional<YouTubeAccountProviderRestoreStatus> wrongThreadResult;
	std::thread wrongThread([&]() {
		wrongThreadResult = fixture.provider->restoreSavedStateUnderHeldOperationLock(
			kProfileA, savedSelection("wrong-thread"), heldLock);
	});
	wrongThread.join();
	CHECK(wrongThreadResult == YouTubeAccountProviderRestoreStatus::WrongThread);
	CHECK(fixture.vault.statusCount == 0);
	CHECK(fixture.provider->snapshot().revision == before.revision);
	CHECK(heldLock.acquired());
	CHECK(heldLock.release());
}

void testExternalOperationReleaseFailureCanFailProviderClosed()
{
	Fixture fixture;
	YouTubeAccountProfileOperationLock heldLock;
	CHECK(fixture.operationLockProvider->acquire(kProfileA, heldLock).acquired());
	fixture.vault.forcedStatus = CredentialStatus{CredentialState::Present, {CredentialError::None, 0}};
	CHECK(fixture.provider->restoreSavedStateUnderHeldOperationLock(kProfileA, savedSelection("external-failure"), heldLock) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(heldLock.acquired());

	fixture.provider->markExternalOperationReleaseFailed();
	const auto failed = fixture.provider->snapshot();
	CHECK(failed.stage == YouTubeAccountProviderStage::Unavailable);
	CHECK(failed.account.state == easy_multistream::YouTubeAccountState::Unavailable);
	CHECK(!failed.account.connectionLease.has_value());
	CHECK(failed.account.failure == easy_multistream::YouTubeAccountFailure::CredentialUnavailable);
	CHECK(heldLock.acquired());
	CHECK(fixture.operationLockApi.releaseCount == 0);
	CHECK(heldLock.release());
	fixture.provider->externalOperationLockReleased();
	CHECK(fixture.provider->invalidateContext());
}

void testProfileOperationLockIsNotNeededForNonCredentialRestore()
{
	Fixture fixture;
	CHECK(fixture.provider->restoreSavedState(kProfileA, std::nullopt) ==
	      YouTubeAccountProviderRestoreStatus::SetupRequired);
	CHECK(fixture.operationLockApi.createCount == 0);
	CHECK(fixture.operationLockApi.waitCount == 0);
	CHECK(fixture.vault.statusCount == 0);

	CHECK(fixture.provider->restoreSavedState(kProfileA, YouTubeAccountSelection{"", "bad", "stream", "label"}) ==
	      YouTubeAccountProviderRestoreStatus::InvalidSelection);
	CHECK(fixture.operationLockApi.createCount == 0);
	CHECK(fixture.operationLockApi.waitCount == 0);
	CHECK(fixture.vault.statusCount == 0);
}

void testProfileOperationLockReleasesOnLifecycleTermination()
{
	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		CHECK(fixture.provider->cancel(activeLease(fixture)));
		CHECK(fixture.operationLockApi.releaseCount == 1);
	}
	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		CHECK(fixture.provider->invalidateContext());
		CHECK(fixture.operationLockApi.releaseCount == 1);
	}
	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		CHECK(fixture.provider->shutdown());
		CHECK(fixture.operationLockApi.releaseCount == 1);
	}
	FakeYouTubeAccountProfileOperationLockApi operationLockApi;
	YouTubeAccountProfileOperationLockProvider operationLockProvider(operationLockApi);
	FakeCredentialStore vault;
	{
		YouTubeAccountProviderTestAccess provider(
			std::make_unique<FakeAuthorizationPort>(), std::make_unique<FakeTokenPort>(),
			std::make_unique<FakeDiscoveryPort>(), vault, operationLockProvider, kProfileA,
			[](const std::optional<YouTubeAccountSelection> &) { return true; });
		CHECK(provider.startConnection() == YouTubeAccountProviderStartStatus::Started);
		CHECK(operationLockApi.releaseCount == 0);
	}
	CHECK(operationLockApi.releaseCount == 1);
	CHECK(operationLockApi.closeCount == 1);
}

void testProfileOperationLockReleaseFailuresFailClosedAndRetry()
{
	{
		Fixture fixture;
		fixture.operationLockApi.releaseResult = false;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		completeAuthorization(fixture);
		completeToken(fixture);
		fixture.discoveryPointer->completeChannels(channelPage({{"channel-one", "Channel One"}}));
		fixture.discoveryPointer->completeStreams(streamPage({{"stream-one", "channel-one", "Everyday"}}));

		const auto failedRelease = fixture.provider->snapshot();
		CHECK(failedRelease.stage == YouTubeAccountProviderStage::Unavailable);
		CHECK(failedRelease.account.state == easy_multistream::YouTubeAccountState::Unavailable);
		CHECK(!failedRelease.account.connectionLease.has_value());
		CHECK(failedRelease.account.failure == easy_multistream::YouTubeAccountFailure::CredentialUnavailable);
		CHECK(fixture.operationLockApi.releaseCount == 1);
		CHECK(fixture.operationLockApi.closeCount == 0);
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Busy);

		fixture.operationLockApi.releaseResult = true;
		CHECK(fixture.provider->shutdown());
		CHECK(fixture.operationLockApi.releaseCount == 2);
		CHECK(fixture.operationLockApi.closeCount == 1);
	}

	{
		Fixture fixture;
		fixture.vault.forcedStatus = CredentialStatus{CredentialState::Present, {CredentialError::None, 0}};
		fixture.operationLockApi.releaseResult = false;
		CHECK(fixture.provider->restoreSavedState(kProfileA, savedSelection("release-failure")) ==
		      YouTubeAccountProviderRestoreStatus::OperationFailed);
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Unavailable);
		CHECK(fixture.operationLockApi.releaseCount == 1);
		const auto beforeRejectedRestore = fixture.provider->snapshot();
		CHECK(fixture.provider->restoreSavedState(kProfileB, std::nullopt) ==
		      YouTubeAccountProviderRestoreStatus::Busy);
		const auto afterRejectedRestore = fixture.provider->snapshot();
		CHECK(afterRejectedRestore.revision == beforeRejectedRestore.revision);
		CHECK(afterRejectedRestore.stage == beforeRejectedRestore.stage);
		CHECK(afterRejectedRestore.account.state == beforeRejectedRestore.account.state);

		fixture.operationLockApi.releaseResult = true;
		CHECK(fixture.provider->invalidateContext());
		CHECK(fixture.operationLockApi.releaseCount == 2);
		CHECK(fixture.operationLockApi.closeCount == 1);
	}

	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		fixture.operationLockApi.releaseResult = false;
		CHECK(!fixture.provider->shutdown());
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Closed);
		CHECK(fixture.operationLockApi.releaseCount == 1);
		fixture.operationLockApi.releaseResult = true;
		CHECK(fixture.provider->shutdown());
		CHECK(fixture.operationLockApi.releaseCount == 2);
		CHECK(fixture.operationLockApi.closeCount == 1);
	}

	{
		Fixture fixture;
		CHECK(fixture.provider->startConnection() == YouTubeAccountProviderStartStatus::Started);
		fixture.authorizationPointer->shutdownResult = false;
		fixture.tokenPointer->shutdownResult = false;
		fixture.discoveryPointer->shutdownResult = false;
		CHECK(!fixture.provider->shutdown());
		CHECK(fixture.provider->snapshot().stage == YouTubeAccountProviderStage::Closed);
		CHECK(fixture.authorizationPointer->shutdownCount == 1);
		CHECK(fixture.tokenPointer->shutdownCount == 1);
		CHECK(fixture.discoveryPointer->shutdownCount == 1);

		fixture.authorizationPointer->shutdownResult = true;
		fixture.tokenPointer->shutdownResult = true;
		fixture.discoveryPointer->shutdownResult = true;
		CHECK(fixture.provider->shutdown());
		CHECK(fixture.authorizationPointer->shutdownCount == 2);
		CHECK(fixture.tokenPointer->shutdownCount == 2);
		CHECK(fixture.discoveryPointer->shutdownCount == 2);
	}
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
	testExplicitReauthorizationReplacesCorruptCredential();
	testCancelAtEachActiveStageSuppressesLateCallbacks();
	testStaleOldAttemptCannotReplaceNewAttempt();
	testInvalidSelectionLeaseAndShutdownAreSafe();
	testSavedStateRestoreUsesOnlyCredentialStatus();
	testSavedStateRestoreRejectsInvalidInputsAndStatusCombinations();
	testSavedStateRestoreInvalidatesOldProfileWork();
	testConfiguredReauthorizationPreservesSavedConnectionUntilSuccess();
	testSavedStateRestoreReevaluatesCredentialPresence();
	testSavedStateRestoreIsThreadBoundAndReentrySafe();
	testProfileAndChannelScopeRoutingAndMismatch();
	testInvalidProfileBindingFailsClosedWithoutCredentialAccess();
	testFailureCleanupRejectsSynchronousLifecycleReentry();
	testInvalidStartAndCommitterFailureDoNotMutateState();
	testProfileOperationLockSpansInteractiveTransaction();
	testProfileOperationLockFailureDoesNotMutateStart();
	testRecoveredProfileOperationLockContinuesNormally();
	testProfileOperationLockSpansRestoreStatusAndPreservesRejectedRestore();
	testHeldProfileOperationLockRestoreDoesNotReacquireOrRelease();
	testHeldProfileOperationLockRestoreSkipsCredentialForMissingSelection();
	testHeldProfileOperationLockRestoreRejectsWrongOrUnheldLockWithoutMutation();
	testExternalOperationReleaseFailureCanFailProviderClosed();
	testProfileOperationLockIsNotNeededForNonCredentialRestore();
	testProfileOperationLockReleasesOnLifecycleTermination();
	testProfileOperationLockReleaseFailuresFailClosedAndRetry();

	if (failures != 0) {
		std::cerr << failures << " YouTube account provider test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube account provider tests passed\n";
	return 0;
}

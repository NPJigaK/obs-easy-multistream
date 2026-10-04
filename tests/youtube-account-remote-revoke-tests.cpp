// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings.hpp"
#include "youtube-account-remote-revoke.hpp"
#include "windows-credential-vault.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QUrl>

#include <util/config-file.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace easy_multistream {

// This is intentionally test-only. The production coordinator exposes only
// the real Google transport constructor; tests replace it with a deterministic
// owner-thread port without opening a network connection.
class YouTubeAccountRemoteRevokeCoordinatorTestAccess final {
public:
	static std::unique_ptr<YouTubeAccountRemoteRevokeCoordinator> create(
		YouTubeAccountProfileContext &context, YouTubeAccountProvider &provider,
		YouTubeAccountRefreshTokenStore &store, YouTubeAccountProfileOperationLockProvider &lockProvider,
		std::unique_ptr<YouTubeAccountRemoteRevokePort> port)
	{
		return std::unique_ptr<YouTubeAccountRemoteRevokeCoordinator>(new YouTubeAccountRemoteRevokeCoordinator(
			context, provider, store, lockProvider, std::move(port), nullptr));
	}
};

} // namespace easy_multistream

namespace {

using namespace easy_multistream;

int failures = 0;

#define CHECK(expression)                                                                                             \
	do {                                                                                                               \
		if (!(expression)) {                                                                                         \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                 \
			++failures;                                                                                               \
		}                                                                                                            \
	} while (false)

constexpr char kProfileA[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kProfileB[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr char kRefreshToken[] = "refresh-token-sentinel";

YouTubeAccountSelection selectionA()
{
	return {"UC-alpha", "Alpha channel", "stream-alpha", "Alpha stream"};
}

YouTubeAccountSelection selectionB()
{
	return {"UC-beta", "Beta channel", "stream-beta", "Beta stream"};
}

bool sameSelection(const YouTubeAccountSelection &left, const YouTubeAccountSelection &right)
{
	return left.channelId == right.channelId && left.channelLabel == right.channelLabel &&
	       left.streamId == right.streamId && left.streamLabel == right.streamLabel;
}

class TestLockApi final : public YouTubeAccountProfileOperationLockApi {
public:
	HANDLE createMutex(LPCWSTR, DWORD &error) noexcept override
	{
		++createCount;
		const HANDLE handle = reinterpret_cast<HANDLE>(nextHandle++);
		openHandles.insert(handle);
		events.push_back("lock.create");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("lock.create");
		}
		error = ERROR_SUCCESS;
		return handle;
	}

	DWORD wait(HANDLE, DWORD, DWORD &error) noexcept override
	{
		++waitCount;
		events.push_back("lock.wait");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("lock.wait");
		}
		error = ERROR_SUCCESS;
		return WAIT_OBJECT_0;
	}

	bool releaseMutex(HANDLE, DWORD &error) noexcept override
	{
		++releaseCount;
		events.push_back("lock.release");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("lock.release");
		}
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
		events.push_back("lock.close");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("lock.close");
		}
		if (!closeResult) {
			error = closeError;
			return false;
		}
		openHandles.erase(handle);
		error = ERROR_SUCCESS;
		return true;
	}

	bool releaseResult = true;
	DWORD releaseError = ERROR_ACCESS_DENIED;
	bool closeResult = true;
	DWORD closeError = ERROR_INVALID_HANDLE;
	int createCount = 0;
	int waitCount = 0;
	int releaseCount = 0;
	int closeCount = 0;
	std::vector<std::string> events;
	std::vector<std::string> *sharedEvents = nullptr;
	std::function<void()> onRelease;

private:
	std::uintptr_t nextHandle = 1;
	std::unordered_set<HANDLE> openHandles;
};

class TestCredentialStore final : public YouTubeAccountRefreshTokenStore {
public:
	CredentialResult write(const YouTubeAccountCredentialScope &, std::string_view) noexcept override
	{
		events.push_back("credential.write");
		return {};
	}

	CredentialReadResult read(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++readCount;
		lastScope = scope;
		events.push_back("credential.read");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("credential.read");
		}
		if (onRead) {
			onRead();
		}
		CredentialReadResult result;
		result.result = readResult;
		if (readResult.succeeded()) {
			result.secret = SecureBuffer::copyOf(secret);
		}
		return result;
	}

	CredentialResult erase(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++eraseCount;
		lastScope = scope;
		events.push_back("credential.erase");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("credential.erase");
		}
		if (onErase) {
			onErase();
		}
		return eraseResult;
	}

	CredentialStatus status(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		lastScope = scope;
		events.push_back("credential.status");
		return statusValue;
	}

	std::string secret = kRefreshToken;
	CredentialResult readResult;
	CredentialResult eraseResult;
	CredentialStatus statusValue{CredentialState::Present, {}};
	std::function<void()> onRead;
	std::function<void()> onErase;
	std::optional<YouTubeAccountCredentialScope> lastScope;
	std::vector<std::string> events;
	int readCount = 0;
	int eraseCount = 0;
	std::vector<std::string> *sharedEvents = nullptr;
};

class TestRevokePort final : public YouTubeAccountRemoteRevokePort {
public:
	GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
						CompletionHandler completionHandler) noexcept override
	{
		++startCount;
		lastAttempt = request.attempt;
		lastToken = request.token.view();
		events.push_back("revoke.start");
		if (sharedEvents != nullptr) {
			sharedEvents->push_back("revoke.start");
		}
		handler = std::move(completionHandler);
		if (synchronous && handler) {
			pendingCompletion.attempt = lastAttempt;
			complete(std::move(pendingCompletion));
		}
		return startStatus;
	}

	bool cancel(GoogleOAuthTokenAttempt) noexcept override
	{
		++cancelCount;
		events.push_back("revoke.cancel");
		return true;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		events.push_back("revoke.shutdown");
		closed = true;
		return shutdownResult;
	}

	void complete(GoogleOAuthTokenCompletion completion)
	{
		if (handler) {
			events.push_back("revoke.complete");
			if (sharedEvents != nullptr) {
				sharedEvents->push_back("revoke.complete");
			}
			auto callback = handler;
			callback(std::move(completion));
		}
	}

	GoogleOAuthTokenCompletion successfulCompletion() const
	{
		GoogleOAuthTokenCompletion completion;
		completion.attempt = lastAttempt;
		completion.operation = GoogleOAuthTokenOperation::RevokeToken;
		completion.status = GoogleOAuthTokenCompletionStatus::Success;
		completion.providerError = GoogleOAuthTokenProviderError::None;
		return completion;
	}

	GoogleOAuthTokenCompletion invalidTokenCompletion() const
	{
		auto completion = successfulCompletion();
		completion.status = GoogleOAuthTokenCompletionStatus::ProviderRejected;
		completion.providerError = GoogleOAuthTokenProviderError::InvalidToken;
		completion.httpStatus = 400;
		return completion;
	}

	GoogleOAuthTokenCompletion networkFailureCompletion() const
	{
		auto completion = successfulCompletion();
		completion.status = GoogleOAuthTokenCompletionStatus::NetworkFailure;
		return completion;
	}

	GoogleOAuthTokenStartStatus startStatus = GoogleOAuthTokenStartStatus::Started;
	GoogleOAuthTokenCompletion pendingCompletion;
	bool synchronous = false;
	bool shutdownResult = true;
	bool closed = false;
	int startCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	GoogleOAuthTokenAttempt lastAttempt;
	std::string lastToken;
	CompletionHandler handler;
	std::vector<std::string> events;
	std::vector<std::string> *sharedEvents = nullptr;
};

class ConfigHandle final {
public:
	~ConfigHandle()
	{
		if (config_ != nullptr) {
			config_close(config_);
		}
		std::error_code error;
		std::filesystem::remove(path_, error);
		std::filesystem::remove(path_.string() + ".tmp", error);
	}

	bool open(const char *name)
	{
		path_ = std::filesystem::current_path() / name;
		std::error_code error;
		std::filesystem::remove(path_, error);
		std::filesystem::remove(path_.string() + ".tmp", error);
		return config_open(&config_, path_.string().c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS;
	}

	config_t *get() const noexcept { return config_; }

	bool blockSafeSave()
	{
		std::error_code error;
		std::filesystem::remove(path_, error);
		error.clear();
		return std::filesystem::create_directory(path_, error);
	}

private:
	config_t *config_ = nullptr;
	std::filesystem::path path_;
};

struct Fixture final {
	ConfigHandle config;
	std::string currentPath = kProfileA;
	std::vector<std::string> events;
	std::function<void()> onConfigRead;
	int configReadCount = 0;
	TestCredentialStore store;
	TestLockApi lockApi;
	YouTubeAccountProfileOperationLockProvider lockProvider;
	YouTubeAccountProfileContext context;
	YouTubeAccountProvider provider;
	std::unique_ptr<TestRevokePort> port;
	std::unique_ptr<YouTubeAccountRemoteRevokeCoordinator> coordinator;
	std::optional<std::string> binding;

	Fixture()
		: lockProvider(lockApi),
		  context(
			[ this ]() { return currentPath; },
			[ this ]() {
				++configReadCount;
				if (onConfigRead) {
					onConfigRead();
				}
				return config.get();
			}),
		  provider(QStringLiteral("test-client-id"), [](const QUrl &) { return true; }, store, lockProvider,
			   makeBindingOrEmpty(), [](const auto &) { return true; })
	{
		CHECK(config.open("easy-multistream-remote-revoke-test.ini"));
		setAccountSettings(selectionA());
		const auto loaded = context.load();
		CHECK(loaded.status == YouTubeAccountProfileContext::LoadStatus::Loaded);
		binding = loaded.snapshot.profileBinding;
		CHECK(binding.has_value());
		CHECK(provider.restoreSavedState(*binding, selectionA()) == YouTubeAccountProviderRestoreStatus::Configured);
		port = std::make_unique<TestRevokePort>();
		rawPort = port.get();
		coordinator = YouTubeAccountRemoteRevokeCoordinatorTestAccess::create(context, provider, store,
										   lockProvider, std::move(port));
		store.sharedEvents = &events;
		lockApi.sharedEvents = &events;
		rawPort->sharedEvents = &events;
	}

	std::string makeBindingOrEmpty() const
	{
		const auto value = makeYouTubeAccountProfileBinding(kProfileA);
		return value.value_or(std::string());
	}

	void setAccountSettings(std::optional<YouTubeAccountSelection> selection)
	{
		Settings settings;
		settings.youtubeEnabled = true;
		settings.youtubeConnectionMode = YouTubeConnectionMode::Account;
		settings.youtubeAccountSelection = std::move(selection);
		writeProfileSettings(config.get(), settings);
	}

	YouTubeAccountRemoteRevokeRequest request() const
	{
		return {context.snapshot().generation, binding.value_or(std::string())};
	}

	TestRevokePort *revokePort() const noexcept
	{
		// The unique_ptr is moved into the port-owning coordinator. Test access
		// retains a raw pointer before construction through this helper instead.
		return rawPort;
	}

	TestRevokePort *rawPort = nullptr;
};

void processEvents()
{
	QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void testSuccessfulRevokeOrdersCleanupBeforeCompletion()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		fixture.events.push_back("completion");
		received.emplace(std::move(completion));
		CHECK(!fixture.coordinator->lockHeld());
	});
	CHECK(fixture.coordinator->activeAttempt().has_value());
	CHECK(fixture.rawPort->startCount == 1);
	CHECK(fixture.rawPort->lastToken == kRefreshToken);
	CHECK(fixture.store.lastScope.has_value());
	if (fixture.store.lastScope.has_value()) {
		CHECK(fixture.store.lastScope->channelId == selectionA().channelId);
		CHECK(fixture.store.lastScope->profileBinding == fixture.binding.value());
	}
	auto completion = fixture.rawPort->successfulCompletion();
	fixture.rawPort->complete(std::move(completion));
	processEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Revoked);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
		CHECK(received->selectionCleared);
		CHECK(!received->lockCleanupPending);
		CHECK(received->profile.status == YouTubeAccountProfileContext::LoadStatus::SetupRequired);
	}
	CHECK(fixture.store.eraseCount == 1);
	CHECK(fixture.lockApi.releaseCount == 2);
	CHECK(!fixture.coordinator->lockHeld());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(!fixture.context.snapshot().selection.has_value());

	const std::vector<std::string> expected{
		"lock.create", "lock.wait", "credential.read", "revoke.start", "revoke.complete", "credential.erase",
		"lock.release", "lock.close", "completion",
	};
	CHECK(fixture.events == expected);
}

void testInvalidTokenConvergesToLocalDisconnect()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	CHECK(fixture.coordinator->activeAttempt().has_value());
	fixture.rawPort->complete(fixture.rawPort->invalidTokenCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::AlreadyRevoked);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
	}
	CHECK(fixture.store.eraseCount == 1);
	CHECK(!fixture.context.snapshot().selection.has_value());
}

void testNetworkFailurePreservesCredentialAndSelection()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	fixture.rawPort->complete(fixture.rawPort->networkFailureCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::NetworkFailure);
		CHECK(!received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
	}
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.context.snapshot().selection.has_value());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Configured);
}

void testCredentialReadFailureDoesNotStartRemoteRevoke()
{
	Fixture fixture;
	fixture.store.readResult = {CredentialError::AccessDenied, ERROR_ACCESS_DENIED};
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::CredentialUnavailable);
	}
	CHECK(fixture.rawPort->startCount == 0);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.context.snapshot().selection.has_value());
}

void testCredentialEraseFailureReportsRemoteAcceptedButDoesNotClaimCleanup()
{
	Fixture fixture;
	fixture.store.eraseResult = {CredentialError::AccessDenied, ERROR_ACCESS_DENIED};
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::CredentialEraseFailed);
		CHECK(received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(fixture.context.snapshot().selection.has_value());
}

void testProviderSelectionMismatchIsRejectedBeforeRemoteSideEffect()
{
	Fixture fixture;
	// The context has selection A while the restored provider state is replaced
	// with selection B. A correct coordinator must reject before contacting
	// Google; currently this is a regression guard for the core implementation.
	CHECK(fixture.provider.invalidateContext());
	CHECK(fixture.provider.restoreSavedState(*fixture.binding, selectionB()) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	const auto result = fixture.coordinator->start(fixture.request(), [](YouTubeAccountRemoteRevokeCompletion) {});
	CHECK(result == YouTubeAccountRemoteRevokeStartStatus::InvalidSelection);
	CHECK(fixture.rawPort->startCount == 0);
}

void testProviderProfileMismatchIsRejectedBeforeRemoteSideEffect()
{
	Fixture fixture;
	CHECK(fixture.provider.invalidateContext());
	CHECK(fixture.provider.restoreSavedState(kProfileB, selectionA()) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	const auto result = fixture.coordinator->start(fixture.request(), [](YouTubeAccountRemoteRevokeCompletion) {});
	CHECK(result == YouTubeAccountRemoteRevokeStartStatus::InvalidSelection);
	CHECK(fixture.rawPort->startCount == 0);
	CHECK(fixture.store.readCount == 0);
}

void testReentrantInvalidationDuringLockedPreflightNeverRevokesWithoutTheLock()
{
	Fixture fixture;
	const int baselineReads = fixture.configReadCount;
	bool invalidatedImmediately = true;
	fixture.onConfigRead = [&]() {
		if (fixture.configReadCount == baselineReads + 2) {
			fixture.onConfigRead = {};
			invalidatedImmediately = fixture.coordinator->invalidateContext();
		}
	};
	const auto result = fixture.coordinator->start(fixture.request(), [](YouTubeAccountRemoteRevokeCompletion) {});
	CHECK(result == YouTubeAccountRemoteRevokeStartStatus::ProfileChanged);
	CHECK(!invalidatedImmediately);
	CHECK(fixture.rawPort->startCount == 0);
	CHECK(fixture.store.readCount == 0);
	CHECK(!fixture.coordinator->lockHeld());
}

void testProfileDriftBeforeRemoteCompletionDoesNotEraseCredential()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	fixture.currentPath = kProfileB;
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileChanged);
		CHECK(received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
	}
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.context.snapshot().selection.has_value());
}

void testCancelIgnoresLateRemoteCompletion()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	const auto attempt = fixture.coordinator->activeAttempt();
	CHECK(attempt.has_value());
	CHECK(fixture.coordinator->cancel(*attempt));
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Cancelled);
		CHECK(!received->remoteRevokeAccepted);
	}
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.context.snapshot().selection.has_value());
	CHECK(!fixture.coordinator->lockHeld());
}

void testSynchronousRemoteCompletionIsDeferredSafely()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.rawPort->synchronous = true;
	fixture.rawPort->pendingCompletion.operation = GoogleOAuthTokenOperation::RevokeToken;
	fixture.rawPort->pendingCompletion.status = GoogleOAuthTokenCompletionStatus::Success;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	CHECK(!received.has_value());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Revoked);
	}
	CHECK(fixture.store.eraseCount == 1);
}

void testShutdownCancelsRequestAndClosesPort()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	CHECK(fixture.coordinator->shutdown());
	processEvents();
	CHECK(fixture.rawPort->cancelCount == 1);
	CHECK(fixture.rawPort->shutdownCount == 1);
	CHECK(fixture.rawPort->closed);
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Closed);
	}
	CHECK(fixture.store.eraseCount == 0);
	CHECK(!fixture.coordinator->lockHeld());
	CHECK(fixture.coordinator->start(fixture.request(), [](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Closed);
}

void testRemoteCompletionIsDeliveredOnlyAfterLockRelease()
{
	Fixture fixture;
	bool completionSawReleasedLock = false;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion) {
		completionSawReleasedLock = !fixture.coordinator->lockHeld();
	});
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(completionSawReleasedLock);
	CHECK(fixture.lockApi.releaseCount == 2);
}

void testCancelCannotReplaceAnAlreadyCommittedResult()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	const auto attempt = fixture.coordinator->activeAttempt();
	CHECK(attempt.has_value());
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	CHECK(!received.has_value());
	CHECK(attempt.has_value() && !fixture.coordinator->cancel(*attempt));
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Revoked);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
		CHECK(received->selectionCleared);
	}
}

void testLockReleaseFailureIsExplicitAndRetriedDuringShutdown()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
		CHECK(fixture.coordinator->lockHeld());
	});
	fixture.lockApi.releaseResult = false;
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::OperationFailed);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
		CHECK(received->selectionCleared);
		CHECK(received->lockCleanupPending);
	}
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);
	CHECK(fixture.coordinator->lockHeld());
	fixture.lockApi.releaseResult = true;
	CHECK(fixture.coordinator->shutdown());
	CHECK(!fixture.coordinator->lockHeld());
}

void testResultFlagsDoNotLeakIntoANewAttempt()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> first;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		first.emplace(std::move(completion));
	});
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(first.has_value() && first->remoteRevokeAccepted && first->credentialErased && first->selectionCleared);

	fixture.setAccountSettings(selectionA());
	const auto loaded = fixture.context.load();
	CHECK(loaded.status == YouTubeAccountProfileContext::LoadStatus::Loaded);
	CHECK(fixture.provider.restoreSavedState(*fixture.binding, selectionA()) ==
	      YouTubeAccountProviderRestoreStatus::Configured);
	std::optional<YouTubeAccountRemoteRevokeCompletion> second;
	fixture.coordinator->start({loaded.snapshot.generation, *fixture.binding},
				   [&](YouTubeAccountRemoteRevokeCompletion completion) {
					   second.emplace(std::move(completion));
				   });
	fixture.rawPort->complete(fixture.rawPort->networkFailureCompletion());
	processEvents();
	CHECK(second.has_value());
	if (second.has_value()) {
		CHECK(second->status == YouTubeAccountRemoteRevokeStatus::NetworkFailure);
		CHECK(!second->remoteRevokeAccepted);
		CHECK(!second->credentialErased);
		CHECK(!second->selectionCleared);
	}
}

void testReentrantInvalidationDuringCredentialReadDefersCleanup()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	bool invalidatedImmediately = true;
	fixture.store.onRead = [&]() {
		fixture.store.onRead = {};
		invalidatedImmediately = fixture.coordinator->invalidateContext();
	};
	CHECK(fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		      received.emplace(std::move(completion));
	      }) == YouTubeAccountRemoteRevokeStartStatus::Started);
	processEvents();
	CHECK(!invalidatedImmediately);
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileChanged);
	}
	CHECK(fixture.rawPort->startCount == 0);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(!fixture.coordinator->lockHeld());
}

void testReentrantShutdownDuringLockReleaseIsTerminal()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	bool shutdownCompletedImmediately = true;
	fixture.lockApi.onRelease = [&]() {
		fixture.lockApi.onRelease = {};
		shutdownCompletedImmediately = fixture.coordinator->shutdown();
	};
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	fixture.rawPort->complete(fixture.rawPort->networkFailureCompletion());
	processEvents();
	CHECK(!shutdownCompletedImmediately);
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Closed);
	}
	CHECK(fixture.rawPort->shutdownCount == 1);
	CHECK(!fixture.coordinator->lockHeld());
	CHECK(fixture.coordinator->start(fixture.request(), [](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Closed);
}

void testProfileSaveFailureNeverRestoresRevokedCredential()
{
	Fixture fixture;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	fixture.coordinator->start(fixture.request(), [&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	});
	CHECK(fixture.config.blockSafeSave());
	fixture.rawPort->complete(fixture.rawPort->successfulCompletion());
	processEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileSaveFailed);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(fixture.store.eraseCount == 1);
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);
	CHECK(fixture.context.snapshot().selection.has_value());
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testSuccessfulRevokeOrdersCleanupBeforeCompletion();
	testInvalidTokenConvergesToLocalDisconnect();
	testNetworkFailurePreservesCredentialAndSelection();
	testCredentialReadFailureDoesNotStartRemoteRevoke();
	testCredentialEraseFailureReportsRemoteAcceptedButDoesNotClaimCleanup();
	testProviderSelectionMismatchIsRejectedBeforeRemoteSideEffect();
	testProviderProfileMismatchIsRejectedBeforeRemoteSideEffect();
	testReentrantInvalidationDuringLockedPreflightNeverRevokesWithoutTheLock();
	testProfileDriftBeforeRemoteCompletionDoesNotEraseCredential();
	testCancelIgnoresLateRemoteCompletion();
	testSynchronousRemoteCompletionIsDeferredSafely();
	testShutdownCancelsRequestAndClosesPort();
	testRemoteCompletionIsDeliveredOnlyAfterLockRelease();
	testCancelCannotReplaceAnAlreadyCommittedResult();
	testLockReleaseFailureIsExplicitAndRetriedDuringShutdown();
	testResultFlagsDoNotLeakIntoANewAttempt();
	testReentrantInvalidationDuringCredentialReadDefersCleanup();
	testReentrantShutdownDuringLockReleaseIsTerminal();
	testProfileSaveFailureNeverRestoresRevokedCredential();
	if (failures != 0) {
		std::cerr << failures << " youtube-account-remote-revoke test(s) failed\n";
		return 1;
	}
	std::cout << "All youtube-account-remote-revoke tests passed\n";
	return 0;
}

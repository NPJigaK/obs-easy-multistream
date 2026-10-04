// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings.hpp"
#include "youtube-account-profile-restore.hpp"
#include "windows-credential-vault.hpp"

#include <QCoreApplication>
#include <QUrl>

#include <util/config-file.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>

namespace easy_multistream {

class RestoreTestLockApi final : public YouTubeAccountProfileOperationLockApi {
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

	DWORD wait(HANDLE, DWORD, DWORD &error) noexcept override
	{
		++waitCount;
		if (onWait) {
			onWait();
		}
		const DWORD result = nextWaitResult.value_or(WAIT_OBJECT_0);
		nextWaitResult.reset();
		error = result == WAIT_FAILED ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
		return result;
	}

	bool releaseMutex(HANDLE handle, DWORD &error) noexcept override
	{
		++releaseCount;
		lastReleaseHandle = handle;
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
	std::function<void()> onWait;
	bool releaseResult = true;
	DWORD releaseError = ERROR_ACCESS_DENIED;
	bool closeResult = true;
	DWORD closeError = ERROR_INVALID_HANDLE;
	int createCount = 0;
	int waitCount = 0;
	int releaseCount = 0;
	int closeCount = 0;
	HANDLE lastReleaseHandle = nullptr;
	HANDLE lastCloseHandle = nullptr;
	std::unordered_set<HANDLE> openHandles;

private:
	std::uintptr_t nextHandle = 1;
};

class RestoreTestCredentialStore final : public YouTubeAccountRefreshTokenStore {
public:
	CredentialResult write(const YouTubeAccountCredentialScope &, std::string_view) noexcept override
	{
		++writeCount;
		return {};
	}
	CredentialReadResult read(const YouTubeAccountCredentialScope &) noexcept override
	{
		++readCount;
		return {};
	}
	CredentialStatus status(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++statusCount;
		lastScope = scope;
		if (onStatus) {
			onStatus();
		}
		return statusValue;
	}

	CredentialResult erase(const YouTubeAccountCredentialScope &scope) noexcept override
	{
		++eraseCount;
		lastScope = scope;
		if (onErase) {
			onErase();
		}
		return eraseValue;
	}

	CredentialStatus statusValue{CredentialState::Missing, {CredentialError::NotFound, 0}};
	CredentialResult eraseValue;
	std::function<void()> onStatus;
	std::function<void()> onErase;
	int writeCount = 0;
	int readCount = 0;
	int eraseCount = 0;
	int statusCount = 0;
	std::optional<YouTubeAccountCredentialScope> lastScope;
};

} // namespace easy_multistream

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using namespace easy_multistream;

constexpr std::string_view kProfileA = "C:/obs/profiles/alpha";
constexpr std::string_view kProfileB = "C:/obs/profiles/beta";
constexpr char kProfileBindingA[] =
	"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

YouTubeAccountSelection selectionA()
{
	return {"UC-alpha", "Alpha channel", "stream-alpha", "Alpha stream"};
}

YouTubeAccountSelection selectionB()
{
	return {"UC-beta", "Beta channel", "stream-beta", "Beta stream"};
}

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
		return config_open(&config_, path_.string().c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS;
	}

	config_t *get() const noexcept { return config_; }

private:
	config_t *config_ = nullptr;
	std::filesystem::path path_;
};

void setSettings(config_t *config, YouTubeConnectionMode mode, std::optional<YouTubeAccountSelection> selection)
{
	Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = mode;
	settings.youtubeServerUrl = "rtmps://a.rtmps.youtube.com/live2";
	settings.youtubeAccountSelection = std::move(selection);
	writeProfileSettings(config, settings);
}

struct Fixture final {
	ConfigHandle config;
	std::string currentPath = std::string(kProfileA);
	bool throwConfigReader = false;
	std::function<void()> onConfigRead;
	int configReadCount = 0;
	RestoreTestCredentialStore store;
	RestoreTestLockApi lockApi;
	YouTubeAccountProfileOperationLockProvider lockProvider;
	YouTubeAccountProfileContext context;
	YouTubeAccountProvider provider;
	YouTubeAccountProfileRestoreCoordinator coordinator;

	Fixture()
		: lockProvider(lockApi),
		  context(
			[ this ]() { return currentPath; },
			[ this ]() -> config_t * {
				++configReadCount;
				if (onConfigRead) {
					onConfigRead();
				}
				if (throwConfigReader) {
					throw std::runtime_error("config reader failure");
				}
				return config.get();
			}),
		  provider(QStringLiteral("test-client-id"), [](const QUrl &) { return true; }, store, lockProvider,
				   *makeYouTubeAccountProfileBinding(kProfileA), [](const auto &) { return true; }),
		  coordinator(context, provider, lockProvider)
	{
		CHECK(config.open("easy-multistream-profile-restore-test.ini"));
	}

	void account(std::optional<YouTubeAccountSelection> selection = selectionA())
	{
		setSettings(config.get(), YouTubeConnectionMode::Account, std::move(selection));
	}
};

void testSuccessfulRestoreAndNoRecursiveAcquisition()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(result.profile.status == YouTubeAccountProfileContext::LoadStatus::Loaded);
	CHECK(fixture.store.statusCount == 1);
	CHECK(fixture.store.readCount == 0);
	CHECK(fixture.store.writeCount == 0);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.store.lastScope.has_value());
	if (fixture.store.lastScope.has_value()) {
		CHECK(fixture.store.lastScope->channelId == "UC-alpha");
	}
	CHECK(fixture.lockApi.createCount == 1);
	CHECK(fixture.lockApi.waitCount == 1);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 1);
	CHECK(!fixture.coordinator.lockHeld());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Configured);
}

void testReentrantInvalidationIsDeferredUntilRestoreCompletes()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	bool callbackCalled = false;
	bool lockHeldBefore = false;
	bool lockHeldAfter = false;
	bool invalidatedImmediately = true;
	fixture.store.onStatus = [&]() {
		callbackCalled = true;
		lockHeldBefore = fixture.coordinator.lockHeld();
		invalidatedImmediately = fixture.coordinator.invalidate();
		lockHeldAfter = fixture.coordinator.lockHeld();
		CHECK(fixture.lockApi.releaseCount == 0);
	};

	const auto result = fixture.coordinator.restore();
	CHECK(callbackCalled);
	CHECK(lockHeldBefore);
	CHECK(lockHeldAfter);
	CHECK(!invalidatedImmediately);
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::ProfileChanged);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::OperationFailed);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.context.snapshot().profileBinding.empty());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 1);
	CHECK(!fixture.coordinator.lockHeld());
}

void testReentrantShutdownIsDeferredAndTerminal()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	bool callbackCalled = false;
	bool lockHeldBefore = false;
	bool lockHeldAfter = false;
	bool shutdownCompletedImmediately = true;
	fixture.store.onStatus = [&]() {
		callbackCalled = true;
		lockHeldBefore = fixture.coordinator.lockHeld();
		shutdownCompletedImmediately = fixture.coordinator.shutdown();
		lockHeldAfter = fixture.coordinator.lockHeld();
		CHECK(fixture.lockApi.releaseCount == 0);
	};

	const auto result = fixture.coordinator.restore();
	CHECK(callbackCalled);
	CHECK(lockHeldBefore);
	CHECK(lockHeldAfter);
	CHECK(!shutdownCompletedImmediately);
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Closed);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::Closed);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 1);
	CHECK(!fixture.coordinator.lockHeld());

	const int configReads = fixture.configReadCount;
	const int credentialReads = fixture.store.statusCount;
	const int lockCreates = fixture.lockApi.createCount;
	const auto afterShutdown = fixture.coordinator.restore();
	CHECK(afterShutdown.status == YouTubeAccountProfileRestoreStatus::Closed);
	CHECK(fixture.configReadCount == configReads);
	CHECK(fixture.store.statusCount == credentialReads);
	CHECK(fixture.lockApi.createCount == lockCreates);
}

void testReentrantRestoreIsBusyAndKeepsOuterLock()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	bool callbackCalled = false;
	bool lockHeldAfterNestedRestore = false;
	YouTubeAccountProfileRestoreResult nested;
	fixture.store.onStatus = [&]() {
		callbackCalled = true;
		nested = fixture.coordinator.restore();
		lockHeldAfterNestedRestore = fixture.coordinator.lockHeld();
		CHECK(fixture.lockApi.releaseCount == 0);
	};

	const auto outer = fixture.coordinator.restore();
	CHECK(callbackCalled);
	CHECK(nested.status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(nested.providerStatus == YouTubeAccountProviderRestoreStatus::Busy);
	CHECK(lockHeldAfterNestedRestore);
	CHECK(outer.status == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(outer.providerStatus == YouTubeAccountProviderRestoreStatus::Configured);
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Configured);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 1);
	CHECK(!fixture.coordinator.lockHeld());
}

void testProfileChangeDuringCredentialStatusRejectsRestoredState()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	fixture.store.onStatus = [&]() { fixture.currentPath = std::string(kProfileB); };

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::ProfileChanged);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::OperationFailed);
	CHECK(fixture.store.statusCount == 1);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.context.snapshot().profileBinding.empty());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 1);
	CHECK(!fixture.coordinator.lockHeld());
}

void testConfigIsReadAgainInsideLock()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	fixture.lockApi.onWait = [&fixture]() { fixture.account(selectionB()); };

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(fixture.store.statusCount == 1);
	CHECK(fixture.store.lastScope.has_value());
	if (fixture.store.lastScope.has_value()) {
		CHECK(fixture.store.lastScope->channelId == "UC-beta");
	}
	CHECK(result.profile.snapshot.selection.has_value());
	if (result.profile.snapshot.selection.has_value()) {
		CHECK(result.profile.snapshot.selection->channelId == "UC-beta");
	}
}

void testProfileBindingRaceClearsWithoutCredentialRead()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	const int configReadsBefore = fixture.configReadCount;
	fixture.lockApi.onWait = [&fixture]() {
		fixture.currentPath = std::string(kProfileB);
		fixture.account(selectionB());
	};

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::ProfileChanged);
	CHECK(fixture.store.statusCount == 0);
	CHECK(fixture.configReadCount == configReadsBefore);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.context.snapshot().profileBinding.empty());
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
}

void testProfileBindingChangeDuringConfigReadIsRejectedBeforeCredentialRead()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	fixture.onConfigRead = [&]() { fixture.currentPath = std::string(kProfileB); };

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::ProfileChanged);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::OperationFailed);
	CHECK(fixture.store.statusCount == 0);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.context.snapshot().profileBinding.empty());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 1);
	CHECK(!fixture.coordinator.lockHeld());
}

void testBusyAndUnavailableDoNotMutateContext()
{
	Fixture fixture;
	fixture.account(selectionA());
	const auto loaded = fixture.context.load();
	const auto before = fixture.context.snapshot();
	const auto providerBefore = fixture.provider.snapshot();

	YouTubeAccountProfileOperationLock held;
	CHECK(fixture.lockProvider.acquire(*fixture.context.currentProfileBinding(), held).acquired());
	const auto busy = fixture.coordinator.restore();
	CHECK(busy.status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(fixture.store.statusCount == 0);
	CHECK(fixture.context.snapshot().generation == before.generation);
	CHECK(fixture.context.snapshot().profileBinding == before.profileBinding);
	CHECK(fixture.provider.snapshot().revision == providerBefore.revision);
	CHECK(fixture.provider.snapshot().stage == providerBefore.stage);
	CHECK(held.release());

	fixture.lockApi.nextCreateError = ERROR_NOT_ENOUGH_MEMORY;
	const auto unavailable = fixture.coordinator.restore();
	CHECK(unavailable.status == YouTubeAccountProfileRestoreStatus::Unavailable);
	CHECK(fixture.store.statusCount == 0);
	CHECK(fixture.context.snapshot().generation == before.generation);
	CHECK(fixture.context.snapshot().profileBinding == before.profileBinding);
	CHECK(fixture.provider.snapshot().revision == providerBefore.revision);
	CHECK(fixture.provider.snapshot().stage == providerBefore.stage);
	(void)loaded;
}

void testRecoveredLockIsReported()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Missing, {CredentialError::NotFound, 0}};
	fixture.lockApi.nextWaitResult = WAIT_ABANDONED;
	const auto result = fixture.coordinator.restore();
	CHECK(result.recovered);
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::ReauthorizationRequired);
	CHECK(fixture.store.statusCount == 1);
}

void testProviderBusyAfterLockedLoadInvalidatesBothProfiles()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	const auto bindingA = makeYouTubeAccountProfileBinding(kProfileA);
	CHECK(bindingA.has_value());
	if (!bindingA.has_value()) {
		return;
	}

	// Retain the provider's profile-A lock through a simulated native release
	// failure, then switch the active context to B. The outer B lock can be
	// acquired, but the held-lock provider restore must reject the old A state.
	fixture.lockApi.releaseResult = false;
	CHECK(fixture.provider.restoreSavedState(*bindingA, selectionA()) ==
	      YouTubeAccountProviderRestoreStatus::OperationFailed);
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);

	fixture.store.statusCount = 0;
	fixture.currentPath = std::string(kProfileB);
	fixture.account(selectionB());
	fixture.lockApi.releaseResult = true;
	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::Busy);
	CHECK(fixture.store.statusCount == 0);
	CHECK(fixture.context.snapshot().profileBinding.empty());
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(!fixture.coordinator.lockHeld());
}

void testSetupInvalidAndFutureSettingsNeverReadCredential()
{
	Fixture fixture;
	setSettings(fixture.config.get(), YouTubeConnectionMode::Manual,
			std::nullopt);
	const auto manual = fixture.coordinator.restore();
	CHECK(manual.status == YouTubeAccountProfileRestoreStatus::NotAccountMode);
	CHECK(fixture.store.statusCount == 0);

	fixture.account(std::nullopt);
	const auto setup = fixture.coordinator.restore();
	CHECK(setup.status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(fixture.store.statusCount == 0);

	config_set_uint(fixture.config.get(), "EasyMultistream", "SchemaVersion", 0);
	const auto invalid = fixture.coordinator.restore();
	CHECK(invalid.status == YouTubeAccountProfileRestoreStatus::InvalidSettings);
	CHECK(fixture.store.statusCount == 0);

	config_set_uint(fixture.config.get(), "EasyMultistream", "SchemaVersion", kSettingsSchemaVersion + 1);
	const auto future = fixture.coordinator.restore();
	CHECK(future.status == YouTubeAccountProfileRestoreStatus::UnsupportedFutureSettings);
	CHECK(fixture.store.statusCount == 0);
	CHECK(fixture.store.readCount == 0);
	CHECK(fixture.store.writeCount == 0);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
}

void testReaderFailureAndReleaseFailureFailClosed()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	fixture.throwConfigReader = true;
	const auto readerFailure = fixture.coordinator.restore();
	CHECK(readerFailure.status == YouTubeAccountProfileRestoreStatus::ProfileUnavailable);
	CHECK(fixture.store.statusCount == 0);

	fixture.throwConfigReader = false;
	fixture.account(selectionA());
	fixture.lockApi.releaseResult = false;
	const auto releaseFailure = fixture.coordinator.restore();
	CHECK(releaseFailure.status == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.store.statusCount == 1);
	CHECK(fixture.coordinator.lockHeld());
	CHECK(releaseFailure.profile.snapshot.profileBinding.empty());
	CHECK(!releaseFailure.profile.snapshot.selection.has_value());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);
	const auto providerBeforeBypass = fixture.provider.snapshot();
	CHECK(fixture.provider.restoreSavedState(*fixture.context.currentProfileBinding(), std::nullopt) ==
	      YouTubeAccountProviderRestoreStatus::Busy);
	CHECK(fixture.provider.snapshot().revision == providerBeforeBypass.revision);
	CHECK(fixture.provider.snapshot().stage == providerBeforeBypass.stage);
	CHECK(fixture.provider.startConnection() == YouTubeAccountProviderStartStatus::Busy);
	CHECK(!fixture.provider.invalidateContext());
	CHECK(!fixture.provider.shutdown());

	fixture.lockApi.releaseResult = true;
	CHECK(fixture.coordinator.shutdown());
	CHECK(!fixture.coordinator.lockHeld());
}

void testCloseFailureAlsoFailsClosed()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	fixture.lockApi.closeResult = false;

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.store.statusCount == 1);
	CHECK(fixture.coordinator.lockHeld());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);
	fixture.lockApi.closeResult = true;
	CHECK(fixture.coordinator.shutdown());
	CHECK(!fixture.coordinator.lockHeld());
	CHECK(fixture.lockApi.releaseCount == 1);
	CHECK(fixture.lockApi.closeCount == 2);
}

void testDestructorClosesProviderWhenNativeCleanupNeverCompletes()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-profile-restore-destructor-test.ini"));
	setSettings(config.get(), YouTubeConnectionMode::Account, selectionA());
	RestoreTestCredentialStore store;
	store.statusValue = {CredentialState::Present, {}};
	RestoreTestLockApi lockApi;
	lockApi.closeResult = false;
	YouTubeAccountProfileOperationLockProvider lockProvider(lockApi);
	YouTubeAccountProfileContext context([]() { return std::string(kProfileA); },
					     [&config]() { return config.get(); });
	const auto binding = makeYouTubeAccountProfileBinding(kProfileA);
	CHECK(binding.has_value());
	if (!binding.has_value()) {
		return;
	}
	YouTubeAccountProvider provider(QStringLiteral("test-client-id"), [](const QUrl &) { return true; }, store,
					lockProvider, *binding, [](const auto &) { return true; });
	{
		YouTubeAccountProfileRestoreCoordinator coordinator(context, provider, lockProvider);
		const auto result = coordinator.restore();
		CHECK(result.status == YouTubeAccountProfileRestoreStatus::OperationFailed);
		CHECK(coordinator.lockHeld());
		CHECK(provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);
	}

	CHECK(provider.snapshot().stage == YouTubeAccountProviderStage::Closed);
	CHECK(lockApi.releaseCount == 1);
	CHECK(lockApi.closeCount >= 3);
}

void testWrongThreadDoesNotReadProfile()
{
	Fixture fixture;
	fixture.account(selectionA());
	bool pathRead = false;
	// The context reader is replaced only for this independent context; the
	// coordinator itself must reject before invoking it.
	YouTubeAccountProfileContext context(
		[&pathRead]() {
			pathRead = true;
			return std::string(kProfileA);
		},
		[]() { return static_cast<config_t *>(nullptr); });
	YouTubeAccountProfileRestoreCoordinator coordinator(context, fixture.provider, fixture.lockProvider);
	YouTubeAccountProfileRestoreResult result;
	std::thread worker([&]() { result = coordinator.restore(); });
	worker.join();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::WrongThread);
	CHECK(!pathRead);
}

void testShutdownIsTerminalAndDoesNotReadProfileAgain()
{
	Fixture fixture;
	fixture.account(selectionA());
	CHECK(fixture.coordinator.shutdown());
	const int createCount = fixture.lockApi.createCount;
	const int statusCount = fixture.store.statusCount;
	const auto contextBefore = fixture.context.snapshot();

	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Closed);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::Closed);
	CHECK(fixture.lockApi.createCount == createCount);
	CHECK(fixture.store.statusCount == statusCount);
	CHECK(fixture.context.snapshot().generation == contextBefore.generation);
	CHECK(fixture.context.snapshot().profileBinding == contextBefore.profileBinding);
}

void testClosedProviderIsSideEffectFree()
{
	Fixture fixture;
	fixture.account(selectionA());
	CHECK(fixture.provider.shutdown());
	const auto result = fixture.coordinator.restore();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Closed);
	CHECK(result.providerStatus == YouTubeAccountProviderRestoreStatus::Closed);
	CHECK(fixture.lockApi.createCount == 0);
	CHECK(fixture.store.statusCount == 0);
}

void testLocalDisconnectClearsSelectionAndCredential()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::Restored);
	const int releaseCountBefore = fixture.lockApi.releaseCount;
	const int closeCountBefore = fixture.lockApi.closeCount;

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::Disconnected);
	CHECK(result.providerStatus == YouTubeAccountProviderDisconnectStatus::Disconnected);
	CHECK(fixture.store.eraseCount == 1);
	CHECK(fixture.store.lastScope.has_value());
	CHECK(result.profile.status == YouTubeAccountProfileContext::LoadStatus::SetupRequired);
	CHECK(result.profile.snapshot.connectionMode == YouTubeConnectionMode::Account);
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(!fixture.context.snapshot().selection.has_value());
	CHECK(!fixture.coordinator.lockHeld());
	CHECK(fixture.lockApi.releaseCount == releaseCountBefore + 1);
	CHECK(fixture.lockApi.closeCount == closeCountBefore + 1);
}

void testLocalDisconnectWithoutSelectionDoesNotEraseCredential()
{
	Fixture fixture;
	fixture.account(std::nullopt);
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::SetupRequired);

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::AlreadyDisconnected);
	CHECK(result.providerStatus == YouTubeAccountProviderDisconnectStatus::AlreadyDisconnected);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(result.profile.snapshot.connectionMode == YouTubeConnectionMode::Account);
	CHECK(!fixture.coordinator.lockHeld());
}

void testLocalDisconnectManualProfileDoesNotEraseCredential()
{
	Fixture fixture;
	setSettings(fixture.config.get(), YouTubeConnectionMode::Manual, std::nullopt);

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::NotAccountMode);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(result.profile.snapshot.connectionMode == YouTubeConnectionMode::Manual);
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(!fixture.coordinator.lockHeld());
}

void testLocalDisconnectDoesNotEraseOnBusyLock()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto before = fixture.context.snapshot();
	fixture.lockApi.nextWaitResult = WAIT_TIMEOUT;

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::Busy);
	CHECK(result.providerStatus == YouTubeAccountProviderDisconnectStatus::Busy);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(fixture.context.snapshot().generation == before.generation);
	CHECK(fixture.context.snapshot().profileBinding == before.profileBinding);
	CHECK(!fixture.coordinator.lockHeld());
}

void testLocalDisconnectRejectsProfileRaceBeforeCredentialErase()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::Restored);
	fixture.lockApi.onWait = [&fixture]() {
		fixture.currentPath = std::string(kProfileB);
		fixture.account(selectionB());
	};

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::ProfileChanged);
	CHECK(result.providerStatus == YouTubeAccountProviderDisconnectStatus::OperationFailed);
	CHECK(fixture.store.eraseCount == 0);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.context.snapshot().profileBinding.empty());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(!fixture.coordinator.lockHeld());
}

void testLocalDisconnectReleaseFailureFailsClosedAndRetries()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::Restored);
	fixture.lockApi.releaseResult = false;
	const int releaseCountBefore = fixture.lockApi.releaseCount;

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::OperationFailed);
	CHECK(result.providerStatus == YouTubeAccountProviderDisconnectStatus::OperationFailed);
	CHECK(fixture.store.eraseCount == 1);
	CHECK(fixture.coordinator.lockHeld());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Unavailable);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());

	fixture.lockApi.releaseResult = true;
	CHECK(fixture.coordinator.shutdown());
	CHECK(!fixture.coordinator.lockHeld());
	CHECK(fixture.lockApi.releaseCount == releaseCountBefore + 2);
}

void testLocalDisconnectCredentialFailurePreservesStateAndCanRetry()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto before = fixture.context.snapshot();
	const auto providerBefore = fixture.provider.snapshot();
	fixture.store.eraseValue = {CredentialError::Unavailable, ERROR_ACCESS_DENIED};

	const auto failed = fixture.coordinator.disconnectLocal();
	CHECK(failed.status == YouTubeAccountProfileDisconnectStatus::CredentialUnavailable);
	CHECK(failed.providerStatus == YouTubeAccountProviderDisconnectStatus::CredentialUnavailable);
	CHECK(failed.profile.snapshot.generation > before.generation);
	CHECK(failed.profile.snapshot.profileBinding == before.profileBinding);
	CHECK(failed.profile.snapshot.selection.has_value());
	CHECK(fixture.context.snapshot().selection.has_value());
	CHECK(fixture.provider.snapshot().stage == providerBefore.stage);
	CHECK(fixture.provider.snapshot().account.state == providerBefore.account.state);
	CHECK(!fixture.coordinator.lockHeld());

	fixture.store.eraseValue = {};
	const auto retried = fixture.coordinator.disconnectLocal();
	CHECK(retried.status == YouTubeAccountProfileDisconnectStatus::Disconnected);
	CHECK(!retried.profile.snapshot.selection.has_value());
}

void testReentrantLocalDisconnectInvalidationIsDeferred()
{
	Fixture fixture;
	fixture.account(selectionA());
	fixture.store.statusValue = {CredentialState::Present, {}};
	CHECK(fixture.coordinator.restore().status == YouTubeAccountProfileRestoreStatus::Restored);
	fixture.store.onErase = [&]() {
		CHECK(!fixture.coordinator.invalidate());
		CHECK(fixture.coordinator.lockHeld());
	};

	const auto result = fixture.coordinator.disconnectLocal();
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::ProfileChanged);
	CHECK(result.providerStatus == YouTubeAccountProviderDisconnectStatus::OperationFailed);
	CHECK(fixture.store.eraseCount == 1);
	CHECK(result.profile.snapshot.profileBinding.empty());
	CHECK(!result.profile.snapshot.selection.has_value());
	CHECK(fixture.provider.snapshot().stage == YouTubeAccountProviderStage::Idle);
	CHECK(!fixture.coordinator.lockHeld());
}

void testLocalDisconnectWrongThreadAndClosedAreSideEffectFree()
{
	Fixture fixture;
	fixture.account(selectionA());
	YouTubeAccountProfileDisconnectResult wrongThread;
	std::thread worker([&]() { wrongThread = fixture.coordinator.disconnectLocal(); });
	worker.join();
	CHECK(wrongThread.status == YouTubeAccountProfileDisconnectStatus::WrongThread);
	CHECK(wrongThread.providerStatus == YouTubeAccountProviderDisconnectStatus::WrongThread);
	CHECK(fixture.store.eraseCount == 0);

	CHECK(fixture.coordinator.shutdown());
	const auto closed = fixture.coordinator.disconnectLocal();
	CHECK(closed.status == YouTubeAccountProfileDisconnectStatus::Closed);
	CHECK(closed.providerStatus == YouTubeAccountProviderDisconnectStatus::Closed);
	CHECK(fixture.store.eraseCount == 0);
}

void testLocalDisconnectPreservesPreciseProfileValidationFailures()
{
	Fixture fixture;
	fixture.account(selectionA());
	config_set_uint(fixture.config.get(), "EasyMultistream", "SchemaVersion", 0);
	const auto invalid = fixture.coordinator.disconnectLocal();
	CHECK(invalid.status == YouTubeAccountProfileDisconnectStatus::InvalidSettings);
	CHECK(fixture.store.eraseCount == 0);

	fixture.account(selectionA());
	config_set_uint(fixture.config.get(), "EasyMultistream", "SchemaVersion", kSettingsSchemaVersion + 1);
	const auto future = fixture.coordinator.disconnectLocal();
	CHECK(future.status == YouTubeAccountProfileDisconnectStatus::UnsupportedFutureSettings);
	CHECK(fixture.store.eraseCount == 0);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testSuccessfulRestoreAndNoRecursiveAcquisition();
	testReentrantInvalidationIsDeferredUntilRestoreCompletes();
	testReentrantShutdownIsDeferredAndTerminal();
	testReentrantRestoreIsBusyAndKeepsOuterLock();
	testProfileChangeDuringCredentialStatusRejectsRestoredState();
	testConfigIsReadAgainInsideLock();
	testProfileBindingRaceClearsWithoutCredentialRead();
	testProfileBindingChangeDuringConfigReadIsRejectedBeforeCredentialRead();
	testBusyAndUnavailableDoNotMutateContext();
	testRecoveredLockIsReported();
	testProviderBusyAfterLockedLoadInvalidatesBothProfiles();
	testSetupInvalidAndFutureSettingsNeverReadCredential();
	testReaderFailureAndReleaseFailureFailClosed();
	testCloseFailureAlsoFailsClosed();
	testDestructorClosesProviderWhenNativeCleanupNeverCompletes();
	testWrongThreadDoesNotReadProfile();
	testShutdownIsTerminalAndDoesNotReadProfileAgain();
	testClosedProviderIsSideEffectFree();
	testLocalDisconnectClearsSelectionAndCredential();
	testLocalDisconnectWithoutSelectionDoesNotEraseCredential();
	testLocalDisconnectManualProfileDoesNotEraseCredential();
	testLocalDisconnectDoesNotEraseOnBusyLock();
	testLocalDisconnectRejectsProfileRaceBeforeCredentialErase();
	testLocalDisconnectReleaseFailureFailsClosedAndRetries();
	testLocalDisconnectCredentialFailurePreservesStateAndCanRetry();
	testReentrantLocalDisconnectInvalidationIsDeferred();
	testLocalDisconnectWrongThreadAndClosedAreSideEffectFree();
	testLocalDisconnectPreservesPreciseProfileValidationFailures();
	if (failures != 0) {
		std::cerr << failures << " youtube-account-profile-restore test(s) failed\n";
		return 1;
	}
	std::cout << "All youtube-account-profile-restore tests passed\n";
	return 0;
}

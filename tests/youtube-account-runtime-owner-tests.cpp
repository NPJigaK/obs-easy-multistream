// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "settings.hpp"
#include "youtube-account-runtime-owner.hpp"
#include "windows-credential-vault.hpp"

#include <QCoreApplication>

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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace easy_multistream {

class YouTubeAccountRuntimeOwnerTestAccess final {
public:
	static bool commitSelection(YouTubeAccountRuntimeOwner &owner,
				    const std::optional<YouTubeAccountSelection> &selection) noexcept
	{
		return owner.commitSelection(selection);
	}

	static YouTubeAccountProfileDisconnectResult disconnect(YouTubeAccountRuntimeOwner &owner) noexcept
	{
		return owner.disconnectActiveProfile();
	}
};

class OwnerTestLockApi final : public YouTubeAccountProfileOperationLockApi {
public:
	HANDLE createMutex(LPCWSTR, DWORD &error) noexcept override
	{
		const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(nextHandle_++));
		openHandles_.insert(handle);
		error = ERROR_SUCCESS;
		return handle;
	}

	DWORD wait(HANDLE, DWORD, DWORD &error) noexcept override
	{
		if (nextWaitResult != WAIT_OBJECT_0) {
			const DWORD result = nextWaitResult;
			nextWaitResult = WAIT_OBJECT_0;
			error = result == WAIT_FAILED ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
			return result;
		}
		error = ERROR_SUCCESS;
		return WAIT_OBJECT_0;
	}

	bool releaseMutex(HANDLE, DWORD &error) noexcept override
	{
		++releaseCount;
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
		if (!closeResult) {
			error = closeError;
			return false;
		}
		openHandles_.erase(handle);
		error = ERROR_SUCCESS;
		return true;
	}

	bool releaseResult = true;
	bool closeResult = true;
	DWORD releaseError = ERROR_ACCESS_DENIED;
	DWORD closeError = ERROR_INVALID_HANDLE;
	DWORD nextWaitResult = WAIT_OBJECT_0;
	int releaseCount = 0;
	int closeCount = 0;

private:
	std::uintptr_t nextHandle_ = 1;
	std::unordered_set<HANDLE> openHandles_;
};

class OwnerTestCredentialApi final : public WinCredentialApi {
public:
	struct Entry final {
		std::wstring targetName;
		std::wstring userName;
		std::vector<unsigned char> blob;
	};

	struct Allocation final {
		CREDENTIALW credential{};
		std::wstring targetName;
		std::wstring userName;
		std::vector<unsigned char> blob;
	};

	bool write(PCREDENTIALW credential, DWORD, DWORD &error) noexcept override
	{
		++writeCount;
		if (credential == nullptr || credential->TargetName == nullptr || credential->UserName == nullptr ||
		    credential->CredentialBlob == nullptr || credential->CredentialBlobSize == 0) {
			error = ERROR_INVALID_PARAMETER;
			return false;
		}
		try {
			Entry entry;
			entry.targetName = credential->TargetName;
			entry.userName = credential->UserName;
			entry.blob.assign(credential->CredentialBlob,
					  credential->CredentialBlob + credential->CredentialBlobSize);
			entries[credential->TargetName] = std::move(entry);
			error = ERROR_SUCCESS;
			return true;
		} catch (...) {
			error = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
	}

	bool read(LPCWSTR targetName, DWORD, DWORD, PCREDENTIALW &credential, DWORD &error) noexcept override
	{
		credential = nullptr;
		if (targetName == nullptr) {
			error = ERROR_INVALID_PARAMETER;
			return false;
		}
		++readCount;
		if (onRead) {
			onRead();
		}
		const auto found = entries.find(targetName);
		if (found == entries.end()) {
			error = ERROR_NOT_FOUND;
			return false;
		}
		try {
			auto allocation = std::make_unique<Allocation>();
			allocation->targetName = found->second.targetName;
			allocation->userName = found->second.userName;
			allocation->blob = found->second.blob;
			allocation->credential.Type = CRED_TYPE_GENERIC;
			allocation->credential.TargetName = allocation->targetName.data();
			allocation->credential.UserName = allocation->userName.data();
			allocation->credential.CredentialBlob = allocation->blob.data();
			allocation->credential.CredentialBlobSize = static_cast<DWORD>(allocation->blob.size());
			credential = &allocation->credential;
			allocations.emplace(credential, std::move(allocation));
			error = ERROR_SUCCESS;
			return true;
		} catch (...) {
			error = ERROR_NOT_ENOUGH_MEMORY;
			return false;
		}
	}

	bool erase(LPCWSTR targetName, DWORD, DWORD, DWORD &error) noexcept override
	{
		if (targetName == nullptr) {
			error = ERROR_INVALID_PARAMETER;
			return false;
		}
		++eraseCount;
		if (!eraseResult) {
			error = eraseError;
			return false;
		}
		const auto erased = entries.erase(targetName);
		if (erased == 0) {
			error = ERROR_NOT_FOUND;
			return false;
		}
		error = ERROR_SUCCESS;
		return true;
	}

	void freeCredential(PVOID credential) noexcept override
	{
		allocations.erase(static_cast<CREDENTIALW *>(credential));
	}

	std::unordered_map<std::wstring, Entry> entries;
	int writeCount = 0;
	int readCount = 0;
	int eraseCount = 0;
	bool eraseResult = true;
	DWORD eraseError = ERROR_ACCESS_DENIED;
	std::function<void()> onRead;

private:
	std::unordered_map<CREDENTIALW *, std::unique_ptr<Allocation>> allocations;
};

class OwnerTestRemoteRevokePort final : public YouTubeAccountRemoteRevokePort {
public:
	GoogleOAuthTokenStartStatus startRevoke(GoogleOAuthTokenRevokeRequest request,
						CompletionHandler completionHandler) noexcept override
	{
		++startCount;
		lastAttempt = request.attempt;
		lastToken = request.token.view();
		handler = std::move(completionHandler);
		return startStatus;
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override
	{
		++cancelCount;
		lastCancelledAttempt = attempt;
		return cancelResult;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		closed = true;
		return shutdownResult;
	}

	void complete(GoogleOAuthTokenCompletion completion)
	{
		if (handler) {
			auto callback = handler;
			callback(std::move(completion));
		}
	}

	GoogleOAuthTokenCompletion success() const
	{
		GoogleOAuthTokenCompletion completion;
		completion.attempt = lastAttempt;
		completion.operation = GoogleOAuthTokenOperation::RevokeToken;
		completion.status = GoogleOAuthTokenCompletionStatus::Success;
		completion.providerError = GoogleOAuthTokenProviderError::None;
		return completion;
	}

	GoogleOAuthTokenCompletion networkFailure() const
	{
		auto completion = success();
		completion.status = GoogleOAuthTokenCompletionStatus::NetworkFailure;
		return completion;
	}

	GoogleOAuthTokenStartStatus startStatus = GoogleOAuthTokenStartStatus::Started;
	bool cancelResult = true;
	bool shutdownResult = true;
	bool closed = false;
	int startCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	GoogleOAuthTokenAttempt lastAttempt;
	GoogleOAuthTokenAttempt lastCancelledAttempt;
	std::string lastToken;
	CompletionHandler handler;
};

class OwnerTestDestinationRefreshPort final : public YouTubeDestinationRefreshPort {
public:
	GoogleOAuthTokenStartStatus startRefresh(GoogleOAuthTokenRefreshRequest request,
						 CompletionHandler completionHandler) noexcept override
	{
		++startCount;
		lastAttempt = request.attempt;
		lastRefreshToken = std::string(request.refreshToken.view());
		handler = std::move(completionHandler);
		if (onStart) {
			onStart();
		}
		return startStatus;
	}

	bool cancel(GoogleOAuthTokenAttempt attempt) noexcept override
	{
		++cancelCount;
		lastCancelledAttempt = attempt;
		if (onCancel) {
			onCancel();
		}
		return cancelResult;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		closed = true;
		if (onShutdown) {
			onShutdown();
		}
		return shutdownResult;
	}

	void complete(GoogleOAuthTokenCompletion completion)
	{
		if (handler) {
			auto callback = handler;
			callback(std::move(completion));
		}
	}

	GoogleOAuthTokenStartStatus startStatus = GoogleOAuthTokenStartStatus::Started;
	bool cancelResult = true;
	bool shutdownResult = true;
	bool closed = false;
	int startCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	std::optional<GoogleOAuthTokenAttempt> lastAttempt;
	GoogleOAuthTokenAttempt lastCancelledAttempt;
	std::string lastRefreshToken;
	CompletionHandler handler;
	std::function<void()> onStart;
	std::function<void()> onCancel;
	std::function<void()> onShutdown;
};

class OwnerTestDestinationResolverPort final : public YouTubeDestinationResolverPort {
public:
	YouTubeStreamResolverStartStatus startResolveStream(YouTubeResolveStreamRequest request,
							    CompletionHandler completionHandler) noexcept override
	{
		++startCount;
		lastAttempt = request.attempt;
		lastChannelId = std::move(request.channelId);
		lastStreamId = std::move(request.streamId);
		handler = std::move(completionHandler);
		if (onStart) {
			onStart();
		}
		return startStatus;
	}

	bool cancel(YouTubeApiAttempt attempt) noexcept override
	{
		++cancelCount;
		lastCancelledAttempt = attempt;
		if (onCancel) {
			onCancel();
		}
		return cancelResult;
	}

	bool shutdown() noexcept override
	{
		++shutdownCount;
		closed = true;
		if (onShutdown) {
			onShutdown();
		}
		return shutdownResult;
	}

	void complete(YouTubeStreamResolverCompletion completion)
	{
		if (handler) {
			auto callback = handler;
			callback(std::move(completion));
		}
	}

	YouTubeStreamResolverStartStatus startStatus = YouTubeStreamResolverStartStatus::Started;
	bool cancelResult = true;
	bool shutdownResult = true;
	bool closed = false;
	int startCount = 0;
	int cancelCount = 0;
	int shutdownCount = 0;
	std::optional<YouTubeApiAttempt> lastAttempt;
	YouTubeApiAttempt lastCancelledAttempt;
	std::string lastChannelId;
	std::string lastStreamId;
	CompletionHandler handler;
	std::function<void()> onStart;
	std::function<void()> onCancel;
	std::function<void()> onShutdown;
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

constexpr std::string_view kProfilePathA = "C:/obs/profiles/easy-owner-a";
constexpr std::string_view kProfilePathB = "C:/obs/profiles/easy-owner-b";

YouTubeAccountSelection selectionA()
{
	return {"UC-owner-a", "Owner A", "stream-owner-a", "Owner stream A"};
}

YouTubeAccountSelection selectionB()
{
	return {"UC-owner-b", "Owner B", "stream-owner-b", "Owner stream B"};
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

void setAccountSettings(config_t *config, const std::optional<YouTubeAccountSelection> &selection)
{
	Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = YouTubeConnectionMode::Account;
	settings.youtubeAccountSelection = selection;
	writeProfileSettings(config, settings);
}

void setManualSettings(config_t *config)
{
	Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = YouTubeConnectionMode::Manual;
	settings.youtubeServerUrl = "rtmps://a.rtmps.youtube.com/live2";
	writeProfileSettings(config, settings);
}

std::unique_ptr<YouTubeAccountRuntimeOwner>
makeOwner(std::string &currentPath, ConfigHandle &config, std::unique_ptr<WinCredentialApi> credentialApi,
	  std::unique_ptr<YouTubeAccountProfileOperationLockApi> lockApi,
	  GoogleOAuthAuthorizationSession::BrowserOpener browserOpener = {},
	  YouTubeAccountRuntimeOwner::ConfigReader configReaderOverride = {}, QString clientId = {},
	  std::unique_ptr<YouTubeAccountRemoteRevokePort> remoteRevokePort = {},
	  std::unique_ptr<YouTubeDestinationRefreshPort> destinationRefreshPort = {},
	  std::unique_ptr<YouTubeDestinationResolverPort> destinationResolverPort = {})
{
	if (!configReaderOverride) {
		configReaderOverride = [&config]() {
			return config.get();
		};
	}
	return std::make_unique<YouTubeAccountRuntimeOwner>(
		[&currentPath]() { return currentPath; }, std::move(configReaderOverride), std::move(clientId),
		std::move(browserOpener), std::move(credentialApi), std::move(lockApi), std::move(remoteRevokePort),
		std::move(destinationRefreshPort), std::move(destinationResolverPort));
}

void saveCredential(OwnerTestCredentialApi &api, std::string_view profilePath, const YouTubeAccountSelection &selection)
{
	const auto binding = makeYouTubeAccountProfileBinding(profilePath);
	CHECK(binding.has_value());
	if (!binding.has_value()) {
		return;
	}
	WindowsYouTubeAccountRefreshTokenStore store(api);
	CHECK(store.write({*binding, selection.channelId}, "owner-refresh-token").succeeded());
}

class RemoteOwnerFixture final {
public:
	RemoteOwnerFixture(const char *configName)
	{
		CHECK(config.open(configName));
		setAccountSettings(config.get(), selectionA());

		auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
		credentialApiRaw = credentialApi.get();
		saveCredential(*credentialApiRaw, currentPath, selectionA());

		auto lockApi = std::make_unique<OwnerTestLockApi>();
		lockApiRaw = lockApi.get();
		auto remoteRevokePort = std::make_unique<OwnerTestRemoteRevokePort>();
		remoteRevokePortRaw = remoteRevokePort.get();
		owner = makeOwner(currentPath, config, std::move(credentialApi), std::move(lockApi), {}, {}, {},
				  std::move(remoteRevokePort));
		CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Restored);
	}

	ConfigHandle config;
	std::string currentPath{kProfilePathA};
	OwnerTestCredentialApi *credentialApiRaw = nullptr;
	OwnerTestLockApi *lockApiRaw = nullptr;
	OwnerTestRemoteRevokePort *remoteRevokePortRaw = nullptr;
	std::unique_ptr<YouTubeAccountRuntimeOwner> owner;
};

constexpr std::string_view kOwnerDestinationServerUrl = "rtmps://a.rtmps.youtube.com/live2";
constexpr std::string_view kOwnerDestinationStreamKey = "owner-destination-stream-key";
constexpr std::string_view kOwnerDestinationAccessToken = "owner-destination-access-token";
constexpr std::string_view kOwnerDestinationRotatedRefreshToken = "owner-destination-rotated-refresh-token";

GoogleOAuthTokenCompletion ownerDestinationRefreshSuccess(GoogleOAuthTokenAttempt attempt, bool rotate = false)
{
	GoogleOAuthTokenCompletion completion;
	completion.attempt = attempt;
	completion.operation = GoogleOAuthTokenOperation::RefreshAccessToken;
	completion.status = GoogleOAuthTokenCompletionStatus::Success;
	completion.providerError = GoogleOAuthTokenProviderError::None;
	completion.tokens.accessToken = SecureBuffer::copyOf(kOwnerDestinationAccessToken);
	if (rotate) {
		completion.tokens.refreshToken = SecureBuffer::copyOf(kOwnerDestinationRotatedRefreshToken);
	}
	completion.tokens.expiresInSeconds = 3600;
	return completion;
}

YouTubeStreamResolverCompletion ownerDestinationResolveSuccess(YouTubeApiAttempt attempt)
{
	YouTubeStreamResolverCompletion completion;
	completion.attempt = attempt;
	completion.status = YouTubeStreamResolverCompletionStatus::Success;
	YouTubeResolvedIngestion ingestion;
	ingestion.serverUrl = std::string(kOwnerDestinationServerUrl);
	ingestion.streamKey = SecureBuffer::copyOf(kOwnerDestinationStreamKey);
	completion.ingestion.emplace(std::move(ingestion));
	return completion;
}

YouTubeStreamResolverCompletion ownerDestinationResolveFailure(YouTubeApiAttempt attempt)
{
	YouTubeStreamResolverCompletion completion;
	completion.attempt = attempt;
	completion.status = YouTubeStreamResolverCompletionStatus::StreamNotReady;
	completion.providerError = YouTubeApiProviderError::None;
	return completion;
}

class DestinationOwnerFixture final {
public:
	DestinationOwnerFixture(const char *configName,
				GoogleOAuthAuthorizationSession::BrowserOpener browserOpener = {})
	{
		CHECK(config.open(configName));
		setAccountSettings(config.get(), selectionA());

		auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
		credentialApiRaw = credentialApi.get();
		saveCredential(*credentialApiRaw, currentPath, selectionA());

		auto lockApi = std::make_unique<OwnerTestLockApi>();
		lockApiRaw = lockApi.get();
		auto remoteRevokePort = std::make_unique<OwnerTestRemoteRevokePort>();
		remoteRevokePortRaw = remoteRevokePort.get();
		auto destinationRefreshPort = std::make_unique<OwnerTestDestinationRefreshPort>();
		destinationRefreshPortRaw = destinationRefreshPort.get();
		auto destinationResolverPort = std::make_unique<OwnerTestDestinationResolverPort>();
		destinationResolverPortRaw = destinationResolverPort.get();
		owner = makeOwner(currentPath, config, std::move(credentialApi), std::move(lockApi),
				  std::move(browserOpener), {},
				  QStringLiteral("runtime-owner-destination-client.apps.googleusercontent.com"),
				  std::move(remoteRevokePort), std::move(destinationRefreshPort),
				  std::move(destinationResolverPort));
		CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Restored);
	}

	ConfigHandle config;
	std::string currentPath{kProfilePathA};
	OwnerTestCredentialApi *credentialApiRaw = nullptr;
	OwnerTestLockApi *lockApiRaw = nullptr;
	OwnerTestRemoteRevokePort *remoteRevokePortRaw = nullptr;
	OwnerTestDestinationRefreshPort *destinationRefreshPortRaw = nullptr;
	OwnerTestDestinationResolverPort *destinationResolverPortRaw = nullptr;
	std::unique_ptr<YouTubeAccountRuntimeOwner> owner;
};

void processOwnerEvents()
{
	QCoreApplication::processEvents();
}

void testRuntimeOwnerDestinationPreparationSuccessReleasesLockBeforeHandler()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-success.ini");
	const int releasesBeforePreparation = fixture.lockApiRaw->releaseCount;
	bool handlerCalled = false;
	bool handlerSawReleasedLock = false;
	std::optional<YouTubeDestinationPrepareAttempt> handlerUseAttempt;
	YouTubeDestinationPrepareStatus receivedStatus = YouTubeDestinationPrepareStatus::InvalidResponse;
	bool receivedIngestion = false;
	std::string receivedServerUrl;
	std::string receivedStreamKey;

	CHECK(fixture.owner->startDestinationPreparation([&](YouTubeDestinationPrepareCompletion completion) {
		handlerCalled = true;
		handlerSawReleasedLock = fixture.lockApiRaw->releaseCount == releasesBeforePreparation + 1;
		handlerUseAttempt = fixture.owner->destinationSnapshot().activeUseAttempt;
		receivedStatus = completion.status;
		receivedIngestion = completion.ingestion.has_value();
		if (completion.ingestion.has_value()) {
			receivedServerUrl = completion.ingestion->serverUrl;
			receivedStreamKey = std::string(completion.ingestion->streamKey.view());
		}
	}) == YouTubeAccountDestinationOperationStatus::Started);
	const auto active = fixture.owner->destinationSnapshot().activeAttempt;
	CHECK(active.has_value());
	CHECK(fixture.destinationRefreshPortRaw->startCount == 1);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt.has_value());
	if (!active.has_value() || !fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}

	CHECK(fixture.destinationRefreshPortRaw->lastAttempt->generation == active->generation);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt->attempt == active->attempt);
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->startCount == 1);
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	CHECK(fixture.lockApiRaw->releaseCount == releasesBeforePreparation);
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveSuccess(*fixture.destinationResolverPortRaw->lastAttempt));
	CHECK(!handlerCalled);
	processOwnerEvents();

	CHECK(handlerCalled);
	CHECK(handlerSawReleasedLock);
	CHECK(receivedStatus == YouTubeDestinationPrepareStatus::Success);
	CHECK(receivedIngestion);
	CHECK(receivedServerUrl == kOwnerDestinationServerUrl);
	CHECK(receivedStreamKey == kOwnerDestinationStreamKey);
	CHECK(handlerUseAttempt.has_value());
	CHECK(handlerUseAttempt == active);
	const auto destination = fixture.owner->destinationSnapshot();
	CHECK(destination.state == YouTubeDestinationPreparerState::Completed);
	CHECK(!destination.activeAttempt.has_value());
	CHECK(destination.activeUseAttempt == active);
	CHECK(!destination.lockCleanupPending);
	CHECK(destination.lastStatus.has_value());
	CHECK(destination.lastStatus == YouTubeDestinationPrepareStatus::Success);
	CHECK(destination.restored);
}

void testRuntimeOwnerDestinationUseLeaseGuardsAccountMutationsAndReleasesExactly()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-use-lease.ini");
	std::optional<YouTubeDestinationPrepareAttempt> handlerUseAttempt;
	const auto start = fixture.owner->startDestinationPreparationWithAttempt(
		[&](YouTubeDestinationPrepareCompletion completion) {
			if (completion.succeeded()) {
				handlerUseAttempt = fixture.owner->destinationSnapshot().activeUseAttempt;
			}
		});
	CHECK(start.status == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(start.attempt.has_value());
	if (!start.attempt.has_value() || !fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	const auto attempt = *start.attempt;
	CHECK(attempt.generation == fixture.destinationRefreshPortRaw->lastAttempt->generation);
	CHECK(attempt.attempt == fixture.destinationRefreshPortRaw->lastAttempt->attempt);

	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveSuccess(*fixture.destinationResolverPortRaw->lastAttempt));
	processOwnerEvents();

	CHECK(handlerUseAttempt == start.attempt);
	CHECK(fixture.owner->destinationSnapshot().activeUseAttempt == start.attempt);
	CHECK(fixture.owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(YouTubeAccountRuntimeOwnerTestAccess::disconnect(*fixture.owner).status ==
	      YouTubeAccountProfileDisconnectStatus::Busy);
	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Busy);
	CHECK(fixture.owner->startConnection() == YouTubeAccountConnectionOperationStatus::Busy);
	CHECK(!YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*fixture.owner, selectionB()));
	CHECK(fixture.owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {}) ==
	      YouTubeAccountDestinationOperationStatus::Busy);

	const YouTubeDestinationPrepareAttempt stale{attempt.generation, attempt.attempt + 1};
	CHECK(fixture.owner->releaseDestinationUse(stale) ==
	      YouTubeAccountDestinationUseReleaseStatus::StaleAttempt);
	CHECK(fixture.owner->destinationSnapshot().activeUseAttempt == start.attempt);
	CHECK(fixture.owner->releaseDestinationUse(attempt) ==
	      YouTubeAccountDestinationUseReleaseStatus::Released);
	CHECK(!fixture.owner->destinationSnapshot().activeUseAttempt.has_value());
	CHECK(fixture.owner->releaseDestinationUse(attempt) ==
	      YouTubeAccountDestinationUseReleaseStatus::NoActiveUse);
}

void testRuntimeOwnerDestinationPreparationFailureDoesNotCreateUseLease()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-use-failure.ini");
	YouTubeDestinationPrepareStatus receivedStatus = YouTubeDestinationPrepareStatus::InvalidResponse;
	bool receivedIngestion = true;
	const auto start = fixture.owner->startDestinationPreparationWithAttempt(
		[&](YouTubeDestinationPrepareCompletion completion) {
			receivedStatus = completion.status;
			receivedIngestion = completion.ingestion.has_value();
		});
	CHECK(start.status == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(start.attempt.has_value());
	if (!start.attempt.has_value() || !fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveFailure(*fixture.destinationResolverPortRaw->lastAttempt));
	processOwnerEvents();

	CHECK(receivedStatus == YouTubeDestinationPrepareStatus::DestinationUnavailable);
	CHECK(!receivedIngestion);
	CHECK(!fixture.owner->destinationSnapshot().activeUseAttempt.has_value());
	CHECK(fixture.owner->releaseDestinationUse(*start.attempt) ==
	      YouTubeAccountDestinationUseReleaseStatus::NoActiveUse);
}

void testRuntimeOwnerDestinationUseReleaseRestoresChangedProfile()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-use-profile-change.ini");
	const auto start = fixture.owner->startDestinationPreparationWithAttempt(
		[](YouTubeDestinationPrepareCompletion) {});
	CHECK(start.status == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(start.attempt.has_value());
	if (!start.attempt.has_value() || !fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveSuccess(*fixture.destinationResolverPortRaw->lastAttempt));
	processOwnerEvents();
	CHECK(fixture.owner->destinationSnapshot().activeUseAttempt == start.attempt);

	saveCredential(*fixture.credentialApiRaw, kProfilePathB, selectionB());
	fixture.currentPath = std::string(kProfilePathB);
	setAccountSettings(fixture.config.get(), selectionB());
	CHECK(fixture.owner->invalidateForProfileChange());
	CHECK(fixture.owner->destinationSnapshot().activeUseAttempt == start.attempt);
	CHECK(fixture.owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Busy);

	CHECK(fixture.owner->releaseDestinationUse(*start.attempt) ==
	      YouTubeAccountDestinationUseReleaseStatus::Released);
	const auto bindingB = makeYouTubeAccountProfileBinding(kProfilePathB);
	CHECK(bindingB.has_value());
	const auto restored = fixture.owner->snapshot();
	CHECK(restored.restored);
	CHECK(restored.restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(bindingB.has_value() && restored.profileBinding == *bindingB);
}

void testRuntimeOwnerDestinationUseShutdownRetiresLease()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-use-shutdown.ini");
	const auto start = fixture.owner->startDestinationPreparationWithAttempt(
		[](YouTubeDestinationPrepareCompletion) {});
	CHECK(start.status == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(start.attempt.has_value());
	if (!start.attempt.has_value() || !fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveSuccess(*fixture.destinationResolverPortRaw->lastAttempt));
	processOwnerEvents();
	CHECK(fixture.owner->destinationSnapshot().activeUseAttempt == start.attempt);

	CHECK(fixture.owner->shutdown());
	const auto destination = fixture.owner->destinationSnapshot();
	CHECK(destination.closed);
	CHECK(!destination.activeUseAttempt.has_value());
	CHECK(fixture.owner->releaseDestinationUse(*start.attempt) ==
	      YouTubeAccountDestinationUseReleaseStatus::Closed);
}

void testRuntimeOwnerDestinationPreparationBlocksOtherAccountOperations()
{
	int browserOpenCount = 0;
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-busy.ini",
					[&browserOpenCount](const QUrl &) {
						++browserOpenCount;
						return true;
					});
	const auto before = fixture.owner->snapshot();
	CHECK(fixture.owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {}) ==
	      YouTubeAccountDestinationOperationStatus::Started);
	const auto active = fixture.owner->destinationSnapshot().activeAttempt;
	CHECK(active.has_value());
	const int readsAfterStart = fixture.credentialApiRaw->readCount;
	const int destinationStartsAfterStart = fixture.destinationRefreshPortRaw->startCount;

	CHECK(fixture.owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(YouTubeAccountRuntimeOwnerTestAccess::disconnect(*fixture.owner).status ==
	      YouTubeAccountProfileDisconnectStatus::Busy);
	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Busy);
	CHECK(fixture.owner->startConnection() == YouTubeAccountConnectionOperationStatus::Busy);
	CHECK(fixture.owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {}) ==
	      YouTubeAccountDestinationOperationStatus::Busy);

	CHECK(fixture.remoteRevokePortRaw->startCount == 0);
	CHECK(browserOpenCount == 0);
	CHECK(fixture.credentialApiRaw->readCount == readsAfterStart);
	CHECK(fixture.destinationRefreshPortRaw->startCount == destinationStartsAfterStart);
	const auto after = fixture.owner->snapshot();
	CHECK(after.restored == before.restored);
	CHECK(after.restoreStatus == before.restoreStatus);
	CHECK(after.generation == before.generation);
	CHECK(after.profileBinding == before.profileBinding);
	CHECK(after.closed == before.closed);
	const auto destination = fixture.owner->destinationSnapshot();
	CHECK(destination.activeAttempt == active);
	CHECK(destination.lockCleanupPending);
	CHECK(destination.restored);

	if (active.has_value()) {
		CHECK(fixture.owner->cancelDestinationPreparation(*active) ==
		      YouTubeAccountDestinationOperationStatus::Cancelled);
	}
	processOwnerEvents();
}

void testRuntimeOwnerDestinationCompletionRejectsChangedProfileAndRestoresNewOne()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-profile-change.ini");
	bool handlerCalled = false;
	YouTubeDestinationPrepareStatus receivedStatus = YouTubeDestinationPrepareStatus::InvalidResponse;
	bool receivedIngestion = true;
	CHECK(fixture.owner->startDestinationPreparation([&](YouTubeDestinationPrepareCompletion completion) {
		handlerCalled = true;
		receivedStatus = completion.status;
		receivedIngestion = completion.ingestion.has_value();
	}) == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt.has_value());
	if (!fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveSuccess(*fixture.destinationResolverPortRaw->lastAttempt));

	const auto old = fixture.owner->snapshot();
	saveCredential(*fixture.credentialApiRaw, kProfilePathB, selectionB());
	fixture.currentPath = std::string(kProfilePathB);
	setAccountSettings(fixture.config.get(), selectionB());
	const auto pendingRestore = fixture.owner->restoreActiveProfile();
	CHECK(pendingRestore.status == YouTubeAccountProfileRestoreStatus::Busy);
	processOwnerEvents();

	CHECK(handlerCalled);
	CHECK(receivedStatus == YouTubeDestinationPrepareStatus::ProfileChanged);
	CHECK(!receivedIngestion);
	CHECK(fixture.owner->destinationSnapshot().lastStatus.has_value());
	CHECK(fixture.owner->destinationSnapshot().lastStatus == YouTubeDestinationPrepareStatus::ProfileChanged);

	const auto expectedBinding = makeYouTubeAccountProfileBinding(kProfilePathB);
	CHECK(expectedBinding.has_value());
	const auto after = fixture.owner->snapshot();
	CHECK(after.restored);
	CHECK(after.restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(after.generation > old.generation);
	CHECK(expectedBinding.has_value() && after.profileBinding == *expectedBinding);
}

void testRuntimeOwnerDestinationProfileChangeRetriesNativeCleanupBeforeRestore()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-profile-cleanup-retry.ini");
	int handlerCount = 0;
	CHECK(fixture.owner->startDestinationPreparation([&](YouTubeDestinationPrepareCompletion) {
		++handlerCount;
	}) == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt.has_value());
	if (!fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	const auto obsoleteAttempt = *fixture.destinationRefreshPortRaw->lastAttempt;

	fixture.lockApiRaw->releaseResult = false;
	CHECK(!fixture.owner->invalidateForProfileChange());
	CHECK(fixture.owner->destinationSnapshot().lockCleanupPending);
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.destinationRefreshPortRaw->cancelCount == 1);

	saveCredential(*fixture.credentialApiRaw, kProfilePathB, selectionB());
	fixture.currentPath = std::string(kProfilePathB);
	setAccountSettings(fixture.config.get(), selectionB());
	fixture.lockApiRaw->releaseResult = true;
	CHECK(fixture.owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Busy);
	processOwnerEvents();

	const auto expectedBinding = makeYouTubeAccountProfileBinding(kProfilePathB);
	CHECK(expectedBinding.has_value());
	const auto restored = fixture.owner->snapshot();
	CHECK(restored.restored);
	CHECK(restored.restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(expectedBinding.has_value() && restored.profileBinding == *expectedBinding);
	CHECK(!fixture.owner->destinationSnapshot().lockCleanupPending);
	CHECK(!fixture.owner->destinationSnapshot().activeAttempt.has_value());
	CHECK(handlerCount == 0);

	fixture.destinationRefreshPortRaw->complete(ownerDestinationRefreshSuccess(obsoleteAttempt));
	processOwnerEvents();
	CHECK(fixture.destinationResolverPortRaw->startCount == 0);
	CHECK(handlerCount == 0);
}

void testRuntimeOwnerDestinationShutdownSuppressesLateCallbacks()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-shutdown.ini");
	int handlerCount = 0;
	CHECK(fixture.owner->startDestinationPreparation([&](YouTubeDestinationPrepareCompletion) {
		++handlerCount;
	}) == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt.has_value());
	if (!fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	const auto resolverAttempt = *fixture.destinationResolverPortRaw->lastAttempt;
	CHECK(fixture.owner->destinationSnapshot().activeAttempt.has_value());
	CHECK(fixture.owner->shutdown());
	CHECK(fixture.destinationRefreshPortRaw->cancelCount == 1);
	CHECK(fixture.destinationResolverPortRaw->cancelCount == 1);
	CHECK(fixture.destinationRefreshPortRaw->shutdownCount == 1);
	CHECK(fixture.destinationResolverPortRaw->shutdownCount == 1);
	CHECK(fixture.destinationRefreshPortRaw->closed);
	CHECK(fixture.destinationResolverPortRaw->closed);

	// Both transport callbacks remain callable in this fake, modelling a late
	// provider completion after the owner has invalidated the preparer's epoch.
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	fixture.destinationResolverPortRaw->complete(ownerDestinationResolveSuccess(resolverAttempt));
	processOwnerEvents();
	CHECK(handlerCount == 0);
	const auto destination = fixture.owner->destinationSnapshot();
	CHECK(destination.closed);
	CHECK(destination.state == YouTubeDestinationPreparerState::Closed);
	CHECK(!destination.activeAttempt.has_value());
	CHECK(!destination.lockCleanupPending);
	CHECK(fixture.owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {}) ==
	      YouTubeAccountDestinationOperationStatus::Closed);
}

void testRuntimeOwnerDestinationCompletionLockFailureFailsClosedAndRetriesOnShutdown()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-lock-failure.ini");
	YouTubeDestinationPrepareStatus receivedStatus = YouTubeDestinationPrepareStatus::InvalidResponse;
	bool receivedIngestion = true;
	CHECK(fixture.owner->startDestinationPreparation([&](YouTubeDestinationPrepareCompletion completion) {
		receivedStatus = completion.status;
		receivedIngestion = completion.ingestion.has_value();
	}) == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt.has_value());
	if (!fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt));
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.lockApiRaw->releaseResult = false;
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveSuccess(*fixture.destinationResolverPortRaw->lastAttempt));
	processOwnerEvents();

	CHECK(receivedStatus == YouTubeDestinationPrepareStatus::ServiceUnavailable);
	CHECK(!receivedIngestion);
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.owner->destinationSnapshot().lockCleanupPending);
	CHECK(fixture.owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {}) ==
	      YouTubeAccountDestinationOperationStatus::Busy);

	fixture.lockApiRaw->releaseResult = true;
	CHECK(fixture.owner->shutdown());
	CHECK(!fixture.owner->destinationSnapshot().lockCleanupPending);
	CHECK(fixture.owner->snapshot().closed);
}

void testRuntimeOwnerDestinationRotationSurvivesResolverFailureForRemoteRevoke()
{
	DestinationOwnerFixture fixture("easy-multistream-runtime-owner-destination-rotation.ini");
	YouTubeDestinationPrepareStatus receivedStatus = YouTubeDestinationPrepareStatus::InvalidResponse;
	bool receivedIngestion = true;
	CHECK(fixture.owner->startDestinationPreparation([&](YouTubeDestinationPrepareCompletion completion) {
		receivedStatus = completion.status;
		receivedIngestion = completion.ingestion.has_value();
	}) == YouTubeAccountDestinationOperationStatus::Started);
	CHECK(fixture.destinationRefreshPortRaw->lastAttempt.has_value());
	if (!fixture.destinationRefreshPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationRefreshPortRaw->complete(
		ownerDestinationRefreshSuccess(*fixture.destinationRefreshPortRaw->lastAttempt, true));
	CHECK(fixture.credentialApiRaw->writeCount == 2);
	CHECK(fixture.destinationResolverPortRaw->lastAttempt.has_value());
	if (!fixture.destinationResolverPortRaw->lastAttempt.has_value()) {
		return;
	}
	fixture.destinationResolverPortRaw->complete(
		ownerDestinationResolveFailure(*fixture.destinationResolverPortRaw->lastAttempt));
	processOwnerEvents();

	CHECK(receivedStatus == YouTubeDestinationPrepareStatus::DestinationUnavailable);
	CHECK(!receivedIngestion);
	const auto binding = makeYouTubeAccountProfileBinding(kProfilePathA);
	CHECK(binding.has_value());
	if (binding.has_value()) {
		WindowsYouTubeAccountRefreshTokenStore store(*fixture.credentialApiRaw);
		const auto rotated = store.read({*binding, selectionA().channelId});
		CHECK(rotated.result.succeeded());
		CHECK(std::string(rotated.secret.view()) == kOwnerDestinationRotatedRefreshToken);
	}

	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Started);
	CHECK(fixture.remoteRevokePortRaw->startCount == 1);
	CHECK(fixture.remoteRevokePortRaw->lastToken == kOwnerDestinationRotatedRefreshToken);
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->networkFailure());
	processOwnerEvents();
}

void testRuntimeOwnerRemoteRevokeSuccessPublishesSetupRequiredBeforeHandler()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-success.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	YouTubeAccountRuntimeOwnerSnapshot handlerSnapshot;
	YouTubeAccountProfileContext::LoadResult handlerProfile;

	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		handlerSnapshot = fixture.owner->snapshot();
		handlerProfile = completion.profile;
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	CHECK(fixture.remoteRevokePortRaw->startCount == 1);
	CHECK(fixture.remoteRevokePortRaw->lastToken == "owner-refresh-token");
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Revoked);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
		CHECK(received->selectionCleared);
		CHECK(received->profile.status == YouTubeAccountProfileContext::LoadStatus::SetupRequired);
		CHECK(!received->profile.snapshot.selection.has_value());
	}
	CHECK(handlerSnapshot.restored);
	CHECK(handlerSnapshot.restoreStatus == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(handlerSnapshot.providerStage == YouTubeAccountProviderStage::Idle);
	CHECK(handlerSnapshot.generation != 0);
	CHECK(!handlerSnapshot.profileBinding.empty());
	CHECK(handlerProfile.status == YouTubeAccountProfileContext::LoadStatus::SetupRequired);
	CHECK(!handlerProfile.snapshot.selection.has_value());
	CHECK(fixture.credentialApiRaw->entries.empty());
	CHECK(!loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
	CHECK(fixture.owner->remoteRevokeSnapshot().state == YouTubeAccountRemoteRevokeState::Completed);
}

void testRuntimeOwnerBlocksRestoreConnectionAndDisconnectWhileRemoteRevokeRuns()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-busy.ini");
	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Started);
	CHECK(fixture.owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(fixture.owner->startConnection() == YouTubeAccountConnectionOperationStatus::Busy);
	CHECK(YouTubeAccountRuntimeOwnerTestAccess::disconnect(*fixture.owner).status ==
	      YouTubeAccountProfileDisconnectStatus::Busy);
	CHECK(fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(fixture.owner->remoteRevokeSnapshot().activeAttempt.has_value());

	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->networkFailure());
	processOwnerEvents();
}

void testRuntimeOwnerRemoteRevokeNetworkFailurePreservesRestoredState()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-network-failure.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	const auto before = fixture.owner->snapshot();
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->networkFailure());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::NetworkFailure);
		CHECK(!received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	const auto after = fixture.owner->snapshot();
	CHECK(after.restored);
	CHECK(after.restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(after.generation == before.generation);
	CHECK(after.profileBinding == before.profileBinding);
	CHECK(after.providerStage == YouTubeAccountProviderStage::Configured);
	CHECK(!fixture.credentialApiRaw->entries.empty());
	CHECK(loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
}

void testRuntimeOwnerRemoteRevokeEraseFailureRemainsRetryable()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-erase-failure.ini");
	fixture.credentialApiRaw->eraseResult = false;
	fixture.credentialApiRaw->eraseError = ERROR_ACCESS_DENIED;
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::CredentialEraseFailed);
		CHECK(received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().providerStage == YouTubeAccountProviderStage::Configured);
	CHECK(fixture.credentialApiRaw->entries.size() == 1);
	CHECK(loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
}

void testRuntimeOwnerRemoteRevokeProfileSaveFailureFailsClosed()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-save-failure.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	CHECK(fixture.config.blockSafeSave());
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileSaveFailed);
		CHECK(received->remoteRevokeAccepted);
		CHECK(received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.owner->snapshot().providerStage == YouTubeAccountProviderStage::Unavailable);
	CHECK(fixture.credentialApiRaw->entries.empty());
	CHECK(loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
}

void testRuntimeOwnerRemoteRevokeLockCleanupFailureFailsClosedAndRetriesOnShutdown()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-lock-failure.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	fixture.lockApiRaw->releaseResult = false;
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::OperationFailed);
		CHECK(received->lockCleanupPending);
	}
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.owner->remoteRevokeSnapshot().lockCleanupPending);
	fixture.lockApiRaw->releaseResult = true;
	CHECK(fixture.owner->shutdown());
	CHECK(!fixture.owner->remoteRevokeSnapshot().lockCleanupPending);
}

void testRuntimeOwnerRemoteRevokeImmediateLockCleanupFailureFailsClosed()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-start-lock-failure.ini");
	// Keep the active profile valid while deliberately making the provider's
	// restored selection stale. The coordinator discovers that mismatch only
	// after acquiring its operation lock.
	CHECK(YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*fixture.owner, selectionB()));
	fixture.lockApiRaw->releaseResult = false;

	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::OperationFailed);
	CHECK(fixture.remoteRevokePortRaw->startCount == 0);
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(fixture.owner->remoteRevokeSnapshot().lockCleanupPending);

	fixture.lockApiRaw->releaseResult = true;
	CHECK(fixture.owner->shutdown());
	CHECK(!fixture.owner->remoteRevokeSnapshot().lockCleanupPending);
}

void testRuntimeOwnerRestoresChangedProfileAfterQueuedRemoteCompletion()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-profile-restore-retry.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);

	CHECK(fixture.owner->invalidateForProfileChange());
	saveCredential(*fixture.credentialApiRaw, kProfilePathB, selectionB());
	fixture.currentPath = kProfilePathB;
	setAccountSettings(fixture.config.get(), selectionB());
	CHECK(fixture.owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Busy);

	processOwnerEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileChanged);
	}
	const auto owner = fixture.owner->snapshot();
	const auto expectedBinding = makeYouTubeAccountProfileBinding(kProfilePathB);
	CHECK(expectedBinding.has_value());
	CHECK(owner.restored);
	CHECK(owner.restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(owner.providerStage == YouTubeAccountProviderStage::Configured);
	CHECK(expectedBinding.has_value() && owner.profileBinding == *expectedBinding);
	const auto savedSelection = loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection;
	CHECK(savedSelection.has_value());
	if (savedSelection.has_value()) {
		const auto expectedSelection = selectionB();
		CHECK(savedSelection->channelId == expectedSelection.channelId);
		CHECK(savedSelection->channelLabel == expectedSelection.channelLabel);
		CHECK(savedSelection->streamId == expectedSelection.streamId);
		CHECK(savedSelection->streamLabel == expectedSelection.streamLabel);
	}
}

void testRuntimeOwnerSecondRemoteRevokeKeepsSetupRequiredState()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-idempotent-state.ini");
	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::Started);
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	const auto before = fixture.owner->snapshot();
	CHECK(before.restored);
	CHECK(before.restoreStatus == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(before.providerStage == YouTubeAccountProviderStage::Idle);
	CHECK(fixture.owner->startRemoteRevoke([](YouTubeAccountRemoteRevokeCompletion) {}) ==
	      YouTubeAccountRemoteRevokeStartStatus::InvalidSelection);
	const auto after = fixture.owner->snapshot();
	CHECK(after.restored);
	CHECK(after.restoreStatus == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(after.providerStage == YouTubeAccountProviderStage::Idle);
	CHECK(after.generation == before.generation);
	CHECK(after.profileBinding == before.profileBinding);
	CHECK(fixture.remoteRevokePortRaw->startCount == 1);
}

void testRuntimeOwnerRemoteStartReportsReentrantProfileInvalidation()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-reentrant-profile-change.ini");
	bool invalidationCalled = false;
	bool invalidationResult = true;
	fixture.credentialApiRaw->onRead = [&]() {
		if (!invalidationCalled) {
			invalidationCalled = true;
			invalidationResult = fixture.owner->invalidateForProfileChange();
		}
	};
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::ProfileChanged);
	CHECK(invalidationCalled);
	CHECK(!invalidationResult);
	CHECK(fixture.remoteRevokePortRaw->startCount == 0);

	processOwnerEvents();
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileChanged);
	}
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::ProfileChanged);
}

void testRuntimeOwnerCancelIgnoresLateRemoteCompletion()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-cancel.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	const auto attempt = fixture.owner->remoteRevokeSnapshot().activeAttempt;
	CHECK(attempt.has_value());
	if (!attempt.has_value()) {
		return;
	}
	CHECK(fixture.owner->cancelRemoteRevoke(*attempt));
	CHECK(fixture.remoteRevokePortRaw->cancelCount == 1);
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Cancelled);
		CHECK(!received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(fixture.credentialApiRaw->entries.size() == 1);
	CHECK(loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
}

void testRuntimeOwnerProfileInvalidationCancelsRemoteRevoke()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-profile-change.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	CHECK(fixture.owner->invalidateForProfileChange());
	CHECK(fixture.remoteRevokePortRaw->cancelCount == 1);
	fixture.remoteRevokePortRaw->complete(fixture.remoteRevokePortRaw->success());
	processOwnerEvents();

	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::ProfileChanged);
		CHECK(!received->remoteRevokeAccepted);
		CHECK(!received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(!fixture.owner->snapshot().restored);
	CHECK(fixture.owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::ProfileChanged);
	CHECK(fixture.owner->snapshot().generation == 0);
	CHECK(fixture.credentialApiRaw->entries.size() == 1);
	CHECK(loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
}

void testRuntimeOwnerShutdownCancelsAndClosesRemotePort()
{
	RemoteOwnerFixture fixture("easy-multistream-runtime-owner-remote-revoke-shutdown.ini");
	std::optional<YouTubeAccountRemoteRevokeCompletion> received;
	CHECK(fixture.owner->startRemoteRevoke([&](YouTubeAccountRemoteRevokeCompletion completion) {
		received.emplace(std::move(completion));
	}) == YouTubeAccountRemoteRevokeStartStatus::Started);
	CHECK(fixture.owner->shutdown());
	processOwnerEvents();

	CHECK(fixture.remoteRevokePortRaw->cancelCount == 1);
	CHECK(fixture.remoteRevokePortRaw->shutdownCount == 1);
	CHECK(fixture.remoteRevokePortRaw->closed);
	CHECK(fixture.owner->snapshot().closed);
	CHECK(fixture.owner->remoteRevokeSnapshot().closed);
	CHECK(!fixture.owner->remoteRevokeSnapshot().activeAttempt.has_value());
	CHECK(received.has_value());
	if (received.has_value()) {
		CHECK(received->status == YouTubeAccountRemoteRevokeStatus::Closed);
		CHECK(!received->credentialErased);
		CHECK(!received->selectionCleared);
	}
	CHECK(fixture.credentialApiRaw->entries.size() == 1);
	CHECK(loadProfileSettings(fixture.config.get()).settings.youtubeAccountSelection.has_value());
}

void testRestoreOnlyOwnerBindsSuccessfulGeneration()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-restore.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	saveCredential(*credentialApiRaw, currentPath, selectionA());
	auto lockApi = std::make_unique<OwnerTestLockApi>();
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::move(lockApi));

	CHECK(!owner->snapshot().restored);
	CHECK(!YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*owner, selectionA()));
	const auto result = owner->restoreActiveProfile();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto snapshot = owner->snapshot();
	CHECK(snapshot.restored);
	CHECK(snapshot.providerStage == YouTubeAccountProviderStage::Configured);
	CHECK(snapshot.generation == result.profile.snapshot.generation);
	CHECK(snapshot.profileBinding == result.profile.snapshot.profileBinding);
	CHECK(YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*owner, selectionA()));
	CHECK(loadProfileSettings(config.get()).settings.youtubeAccountSelection.has_value());

	const auto repeated = owner->restoreActiveProfile();
	CHECK(repeated.status == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(repeated.profile.snapshot.generation > snapshot.generation);
	CHECK(owner->snapshot().generation == repeated.profile.snapshot.generation);
	CHECK(owner->snapshot().profileBinding == snapshot.profileBinding);
	CHECK(owner->snapshot().providerStage == YouTubeAccountProviderStage::Configured);
	CHECK(YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*owner, selectionA()));

	const int readsBeforeDestination = credentialApiRaw->readCount;
	CHECK(owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {}) ==
	      YouTubeAccountDestinationOperationStatus::NotConfigured);
	CHECK(credentialApiRaw->readCount == readsBeforeDestination);
	CHECK(!owner->destinationSnapshot().activeAttempt.has_value());
}

void testConnectionSeamFailsClosedWhenProductionOAuthIsUnconfigured()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-unconfigured.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto lockApi = std::make_unique<OwnerTestLockApi>();
	auto *lockApiRaw = lockApi.get();
	int browserOpenCount = 0;
	auto owner = makeOwner(currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::move(lockApi),
			       [&browserOpenCount](const QUrl &) {
				       ++browserOpenCount;
				       return true;
			       });

	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::NotRestored);
	CHECK(browserOpenCount == 0);
	CHECK(lockApiRaw->releaseCount == 0);
	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	const int releasesAfterRestore = lockApiRaw->releaseCount;
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::NotConfigured);
	CHECK(browserOpenCount == 0);
	CHECK(lockApiRaw->releaseCount == releasesAfterRestore);
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
	CHECK(owner->selectChannel({1, 1}, {1, 0}) == YouTubeAccountConnectionOperationStatus::StaleAttempt);
	CHECK(owner->selectStream({1, 1}, {1, 0}) == YouTubeAccountConnectionOperationStatus::StaleAttempt);
	CHECK(owner->cancelConnection({1, 1}) == YouTubeAccountConnectionOperationStatus::NoActiveAttempt);
}

void testConnectionSeamStartsAndCancelsForCurrentRestoredProfile()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto lockApi = std::make_unique<OwnerTestLockApi>();
	auto *lockApiRaw = lockApi.get();
	int browserOpenCount = 0;
	QUrl authorizationUrl;
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::move(lockApi),
		[&browserOpenCount, &authorizationUrl](const QUrl &url) {
			++browserOpenCount;
			authorizationUrl = url;
			return true;
		},
		{}, QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	const int releasesAfterRestore = lockApiRaw->releaseCount;
	CHECK(owner->startConnection(GoogleOAuthConsentMode::ForceConsent) ==
	      YouTubeAccountConnectionOperationStatus::Started);
	CHECK(browserOpenCount == 1);
	CHECK(authorizationUrl.scheme() == QStringLiteral("https"));
	CHECK(authorizationUrl.host() == QStringLiteral("accounts.google.com"));
	CHECK(authorizationUrl.query().contains(QStringLiteral("prompt=consent")));
	CHECK(lockApiRaw->releaseCount == releasesAfterRestore);

	const auto connection = owner->connectionSnapshot();
	CHECK(connection.stage == YouTubeAccountConnectionStage::Connecting);
	CHECK(connection.activeAttempt.has_value());
	CHECK(connection.channels.empty());
	CHECK(connection.streams.empty());
	if (!connection.activeAttempt.has_value()) {
		return;
	}
	const YouTubeAccountConnectionAttempt attempt = *connection.activeAttempt;
	CHECK(owner->selectChannel(attempt, {connection.revision, 0}) ==
	      YouTubeAccountConnectionOperationStatus::WrongPhase);
	CHECK(owner->selectStream(attempt, {connection.revision, 0}) ==
	      YouTubeAccountConnectionOperationStatus::WrongPhase);
	CHECK(owner->selectChannel({attempt.generation, attempt.attempt + 1}, {connection.revision, 0}) ==
	      YouTubeAccountConnectionOperationStatus::StaleAttempt);
	CHECK(owner->cancelConnection({attempt.generation, attempt.attempt + 1}) ==
	      YouTubeAccountConnectionOperationStatus::StaleAttempt);
	const auto stillActive = owner->connectionSnapshot().activeAttempt;
	CHECK(stillActive.has_value() && *stillActive == attempt);
	CHECK(owner->cancelConnection(attempt) == YouTubeAccountConnectionOperationStatus::Cancelled);
	CHECK(lockApiRaw->releaseCount == releasesAfterRestore + 1);
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
	CHECK(owner->cancelConnection(attempt) == YouTubeAccountConnectionOperationStatus::NoActiveAttempt);
}

void testRepeatedRestoreDoesNotOrphanAnActiveConnectionAttempt()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-repeat-restore.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	const auto before = owner->snapshot();
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	const auto attempt = owner->connectionSnapshot().activeAttempt;
	CHECK(attempt.has_value());

	const auto repeated = owner->restoreActiveProfile();
	CHECK(repeated.status == YouTubeAccountProfileRestoreStatus::Busy);
	CHECK(repeated.providerStatus == YouTubeAccountProviderRestoreStatus::Busy);
	CHECK(owner->snapshot().restored);
	CHECK(owner->snapshot().generation == before.generation);
	CHECK(owner->snapshot().profileBinding == before.profileBinding);
	CHECK(owner->connectionSnapshot().activeAttempt == attempt);
	if (attempt.has_value()) {
		CHECK(owner->cancelConnection(*attempt) == YouTubeAccountConnectionOperationStatus::Cancelled);
	}
}

void testRepeatedRestoreCancelsAnAttemptFromAChangedProfile()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-restore-profile-change.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	const auto old = owner->snapshot();
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	CHECK(owner->connectionSnapshot().activeAttempt.has_value());
	currentPath = std::string(kProfilePathB);

	const auto restored = owner->restoreActiveProfile();
	CHECK(restored.status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->snapshot().restored);
	CHECK(owner->snapshot().generation > old.generation);
	CHECK(owner->snapshot().profileBinding != old.profileBinding);
	CHECK(!owner->connectionSnapshot().activeAttempt.has_value());
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
}

void testReentrantProfileInvalidationCannotReportAStartedConnection()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-reentrant-invalidate.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	YouTubeAccountRuntimeOwner *ownerRaw = nullptr;
	bool invalidated = false;
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[&ownerRaw, &invalidated](const QUrl &) {
			CHECK(ownerRaw != nullptr);
			if (ownerRaw != nullptr) {
				invalidated = ownerRaw->invalidateForProfileChange();
			}
			return true;
		},
		{}, QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));
	ownerRaw = owner.get();

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::ProfileChanged);
	CHECK(invalidated);
	CHECK(!owner->snapshot().restored);
	CHECK(!owner->connectionSnapshot().activeAttempt.has_value());
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
}

void testReentrantShutdownCannotReportAStartedConnection()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-reentrant-shutdown.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	YouTubeAccountRuntimeOwner *ownerRaw = nullptr;
	bool shutdownComplete = false;
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[&ownerRaw, &shutdownComplete](const QUrl &) {
			CHECK(ownerRaw != nullptr);
			if (ownerRaw != nullptr) {
				shutdownComplete = ownerRaw->shutdown();
			}
			return true;
		},
		{}, QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));
	ownerRaw = owner.get();

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Closed);
	CHECK(shutdownComplete);
	CHECK(owner->snapshot().closed);
	CHECK(!owner->connectionSnapshot().activeAttempt.has_value());
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Closed);
}

void testConnectionSeamRejectsAProfileSwitchBeforeOpeningBrowser()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-profile-switch.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	int browserOpenCount = 0;
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[&browserOpenCount](const QUrl &) {
			++browserOpenCount;
			return true;
		},
		{}, QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	currentPath = std::string(kProfilePathB);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::ProfileChanged);
	CHECK(browserOpenCount == 0);
	const auto snapshot = owner->snapshot();
	CHECK(!snapshot.restored);
	CHECK(snapshot.restoreStatus == YouTubeAccountProfileRestoreStatus::ProfileChanged);
	CHECK(snapshot.providerStage == YouTubeAccountProviderStage::Idle);
}

void testConnectionSeamRejectsAChangedConnectionModeBeforeOpeningBrowser()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-mode-change.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	int browserOpenCount = 0;
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[&browserOpenCount](const QUrl &) {
			++browserOpenCount;
			return true;
		},
		{}, QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	setManualSettings(config.get());
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::NotAccountMode);
	CHECK(browserOpenCount == 0);
	const auto snapshot = owner->snapshot();
	CHECK(!snapshot.restored);
	CHECK(snapshot.restoreStatus == YouTubeAccountProfileRestoreStatus::NotAccountMode);
	CHECK(snapshot.providerStage == YouTubeAccountProviderStage::Idle);
}

void testActiveConnectionAttemptIsInvalidatedByAProfileSwitch()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-active-profile-switch.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	const auto attempt = owner->connectionSnapshot().activeAttempt;
	CHECK(attempt.has_value());
	currentPath = std::string(kProfilePathB);
	if (attempt.has_value()) {
		CHECK(owner->cancelConnection(*attempt) == YouTubeAccountConnectionOperationStatus::ProfileChanged);
	}
	CHECK(!owner->snapshot().restored);
	CHECK(!owner->connectionSnapshot().activeAttempt.has_value());
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
}

void testActiveConnectionAttemptIsInvalidatedByAConnectionModeChange()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-active-mode-change.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	const auto attempt = owner->connectionSnapshot().activeAttempt;
	CHECK(attempt.has_value());
	setManualSettings(config.get());
	if (attempt.has_value()) {
		CHECK(owner->selectChannel(*attempt, {1, 0}) ==
		      YouTubeAccountConnectionOperationStatus::NotAccountMode);
	}
	CHECK(!owner->snapshot().restored);
	CHECK(!owner->connectionSnapshot().activeAttempt.has_value());
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
}

void testProfileInvalidationCancelsAnActiveConnectionAttempt()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-invalidate.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	const auto active = owner->connectionSnapshot().activeAttempt;
	CHECK(active.has_value());
	CHECK(owner->invalidateForProfileChange());
	CHECK(!owner->snapshot().restored);
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Idle);
	if (active.has_value()) {
		CHECK(owner->cancelConnection(*active) == YouTubeAccountConnectionOperationStatus::NotRestored);
		CHECK(owner->selectChannel(*active, {1, 0}) == YouTubeAccountConnectionOperationStatus::NotRestored);
	}
}

void testConnectionCancelFailsClosedWhenNativeLockCleanupFails()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-cancel-lock-failure.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto lockApi = std::make_unique<OwnerTestLockApi>();
	auto *lockApiRaw = lockApi.get();
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::move(lockApi),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	const auto attempt = owner->connectionSnapshot().activeAttempt;
	CHECK(attempt.has_value());
	if (!attempt.has_value()) {
		return;
	}

	lockApiRaw->releaseResult = false;
	CHECK(owner->cancelConnection(*attempt) == YouTubeAccountConnectionOperationStatus::OperationFailed);
	CHECK(!owner->snapshot().restored);
	CHECK(owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Unavailable);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::NotRestored);

	// Cleanup remains terminal and retryable without reopening the failed
	// connection context.
	CHECK(!owner->shutdown());
	lockApiRaw->releaseResult = true;
	CHECK(owner->shutdown());
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Closed);
}

void testShutdownCancelsAnActiveConnectionAttemptAndStaysTerminal()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-connect-active-shutdown.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(
		currentPath, config, std::make_unique<OwnerTestCredentialApi>(), std::make_unique<OwnerTestLockApi>(),
		[](const QUrl &) { return true; }, {},
		QStringLiteral("runtime-owner-client.apps.googleusercontent.com"));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Started);
	const auto attempt = owner->connectionSnapshot().activeAttempt;
	CHECK(attempt.has_value());
	CHECK(owner->shutdown());
	const auto closed = owner->connectionSnapshot();
	CHECK(closed.closed);
	CHECK(closed.stage == YouTubeAccountConnectionStage::Closed);
	CHECK(!closed.activeAttempt.has_value());
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Closed);
	if (attempt.has_value()) {
		CHECK(owner->cancelConnection(*attempt) == YouTubeAccountConnectionOperationStatus::Closed);
	}
}

void testLocalDisconnectDeletesCredentialAndKeepsAccountMode()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	saveCredential(*credentialApiRaw, currentPath, selectionA());
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>());

	const auto restored = owner->restoreActiveProfile();
	CHECK(restored.status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto before = owner->snapshot();
	CHECK(before.restored);

	const auto disconnected = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(disconnected.status == YouTubeAccountProfileDisconnectStatus::Disconnected);
	CHECK(disconnected.profile.status == YouTubeAccountProfileContext::LoadStatus::SetupRequired);
	CHECK(disconnected.profile.snapshot.connectionMode == YouTubeConnectionMode::Account);
	CHECK(!disconnected.profile.snapshot.selection.has_value());
	CHECK(disconnected.profile.snapshot.generation > before.generation);
	CHECK(disconnected.profile.snapshot.profileBinding == before.profileBinding);
	CHECK(credentialApiRaw->entries.empty());

	const auto settings = loadProfileSettings(config.get());
	CHECK(settings.status == SettingsLoadStatus::SetupRequired);
	CHECK(settings.settings.youtubeConnectionMode == YouTubeConnectionMode::Account);
	CHECK(!settings.settings.youtubeAccountSelection.has_value());
	const auto after = owner->snapshot();
	CHECK(after.restored);
	CHECK(after.restoreStatus == YouTubeAccountProfileRestoreStatus::SetupRequired);
	CHECK(after.generation == disconnected.profile.snapshot.generation);
	CHECK(after.profileBinding == disconnected.profile.snapshot.profileBinding);
	CHECK(after.providerStage == YouTubeAccountProviderStage::Idle);
}

void testLocalDisconnectIsIdempotentWhenCredentialIsMissing()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect-missing.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>());

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::ReauthorizationRequired);
	const int readsBeforeDisconnect = credentialApiRaw->readCount;
	const auto first = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(first.status == YouTubeAccountProfileDisconnectStatus::Disconnected);
	CHECK(credentialApiRaw->readCount == readsBeforeDisconnect + 1);
	CHECK(credentialApiRaw->eraseCount == 0);
	CHECK(owner->snapshot().restored);

	const auto second = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(second.status == YouTubeAccountProfileDisconnectStatus::AlreadyDisconnected);
	CHECK(credentialApiRaw->readCount == readsBeforeDisconnect + 1);
	CHECK(credentialApiRaw->eraseCount == 0);
	CHECK(owner->snapshot().restored);
}

void testBusyDisconnectLeavesRestoredOwnerMetadataUntouched()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect-busy.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	saveCredential(*credentialApi, currentPath, selectionA());
	auto lockApi = std::make_unique<OwnerTestLockApi>();
	auto *lockApiRaw = lockApi.get();
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::move(lockApi));
	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto before = owner->snapshot();

	lockApiRaw->nextWaitResult = WAIT_TIMEOUT;
	const auto busy = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(busy.status == YouTubeAccountProfileDisconnectStatus::Busy);
	const auto after = owner->snapshot();
	CHECK(after.restoreStatus == before.restoreStatus);
	CHECK(after.generation == before.generation);
	CHECK(after.profileBinding == before.profileBinding);
	CHECK(after.restored == before.restored);
	CHECK(after.closed == before.closed);
}

void testLocalDisconnectWithNoSelectionNeverTouchesCredentialStore()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect-setup.ini"));
	setAccountSettings(config.get(), std::nullopt);
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>());

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::SetupRequired);
	const auto result = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::AlreadyDisconnected);
	CHECK(credentialApiRaw->readCount == 0);
	CHECK(credentialApiRaw->eraseCount == 0);
	CHECK(owner->snapshot().restored);
	CHECK(owner->snapshot().restoreStatus == YouTubeAccountProfileRestoreStatus::SetupRequired);
}

void testLocalDisconnectManualProfileNeverTouchesCredentialStore()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect-manual.ini"));
	setManualSettings(config.get());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>());

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::NotAccountMode);
	const auto result = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::NotAccountMode);
	CHECK(credentialApiRaw->readCount == 0);
	CHECK(credentialApiRaw->eraseCount == 0);
	CHECK(!owner->snapshot().restored);
}

void testLocalDisconnectSaveFailureDoesNotRestoreErasedCredential()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect-save-failure.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	int configReadCount = 0;
	int failFromRead = 0;
	YouTubeAccountRuntimeOwner::ConfigReader configReader = [&]() -> config_t * {
		++configReadCount;
		if (failFromRead != 0 && configReadCount >= failFromRead) {
			return nullptr;
		}
		return config.get();
	};
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	saveCredential(*credentialApiRaw, currentPath, selectionA());
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>(), {},
			       std::move(configReader));

	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Restored);
	// The disconnect preflight is the next config read; make only its commit
	// read fail, after the credential has already been erased.
	failFromRead = configReadCount + 2;
	const auto result = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(result.status == YouTubeAccountProfileDisconnectStatus::Unavailable);
	CHECK(credentialApiRaw->entries.empty());
	CHECK(credentialApiRaw->eraseCount == 1);
	CHECK(!owner->snapshot().restored);
	CHECK(loadProfileSettings(config.get()).settings.youtubeAccountSelection.has_value());
}

void testLocalDisconnectCredentialFailureCanRetryWithoutRestore()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-disconnect-credential-failure.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	saveCredential(*credentialApiRaw, currentPath, selectionA());
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>());
	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto before = owner->snapshot();
	credentialApiRaw->eraseResult = false;
	credentialApiRaw->eraseError = ERROR_ACCESS_DENIED;

	const auto failed = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(failed.status == YouTubeAccountProfileDisconnectStatus::CredentialUnavailable);
	CHECK(owner->snapshot().restored);
	CHECK(owner->snapshot().generation > before.generation);
	CHECK(owner->snapshot().profileBinding == before.profileBinding);
	CHECK(owner->snapshot().providerStage == YouTubeAccountProviderStage::Configured);
	CHECK(loadProfileSettings(config.get()).settings.youtubeAccountSelection.has_value());
	CHECK(!credentialApiRaw->entries.empty());

	credentialApiRaw->eraseResult = true;
	const auto retried = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(retried.status == YouTubeAccountProfileDisconnectStatus::Disconnected);
	CHECK(!loadProfileSettings(config.get()).settings.youtubeAccountSelection.has_value());
	CHECK(credentialApiRaw->entries.empty());
}

void testMissingCredentialRestoresReauthorizationBoundaryWithoutOpeningBrowser()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-missing-credential.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	int browserOpenCount = 0;
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>(),
			       [&browserOpenCount](const QUrl &) {
				       ++browserOpenCount;
				       return false;
			       });

	const auto result = owner->restoreActiveProfile();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::ReauthorizationRequired);
	CHECK(owner->snapshot().restored);
	CHECK(owner->snapshot().providerStage == YouTubeAccountProviderStage::NeedsReauthorization);
	CHECK(credentialApiRaw->readCount == 1);
	CHECK(credentialApiRaw->writeCount == 0);
	CHECK(browserOpenCount == 0);
}

void testManualProfileNeverReadsAccountCredential()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-manual.ini"));
	setManualSettings(config.get());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	int browserOpenCount = 0;
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>(),
			       [&browserOpenCount](const QUrl &) {
				       ++browserOpenCount;
				       return false;
			       });

	const auto result = owner->restoreActiveProfile();
	CHECK(result.status == YouTubeAccountProfileRestoreStatus::NotAccountMode);
	CHECK(!owner->snapshot().restored);
	CHECK(credentialApiRaw->readCount == 0);
	CHECK(credentialApiRaw->writeCount == 0);
	CHECK(browserOpenCount == 0);
	CHECK(!YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*owner, selectionA()));
}

void testOwnerLifecycleIsThreadBoundWithoutSideEffects()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-thread.ini"));
	setManualSettings(config.get());
	std::string currentPath(kProfilePathA);
	auto owner = makeOwner(currentPath, config, std::make_unique<OwnerTestCredentialApi>(),
			       std::make_unique<OwnerTestLockApi>());
	YouTubeAccountProfileRestoreResult restoreResult;
	YouTubeAccountProfileDisconnectResult disconnectResult;
	YouTubeAccountConnectionOperationStatus startStatus = YouTubeAccountConnectionOperationStatus::OperationFailed;
	YouTubeAccountConnectionOperationStatus channelStatus =
		YouTubeAccountConnectionOperationStatus::OperationFailed;
	YouTubeAccountConnectionOperationStatus streamStatus = YouTubeAccountConnectionOperationStatus::OperationFailed;
	YouTubeAccountConnectionOperationStatus cancelStatus = YouTubeAccountConnectionOperationStatus::OperationFailed;
	YouTubeAccountConnectionSnapshot connectionSnapshot;
	YouTubeAccountDestinationOperationStatus destinationStartStatus =
		YouTubeAccountDestinationOperationStatus::OperationFailed;
	YouTubeAccountDestinationOperationStatus destinationCancelStatus =
		YouTubeAccountDestinationOperationStatus::OperationFailed;
	YouTubeAccountDestinationSnapshot destinationSnapshot;
	bool invalidateResult = true;
	bool shutdownResult = true;
	std::thread worker([&]() {
		restoreResult = owner->restoreActiveProfile();
		disconnectResult = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
		startStatus = owner->startConnection();
		channelStatus = owner->selectChannel({1, 1}, {1, 0});
		streamStatus = owner->selectStream({1, 1}, {1, 0});
		cancelStatus = owner->cancelConnection({1, 1});
		connectionSnapshot = owner->connectionSnapshot();
		destinationStartStatus = owner->startDestinationPreparation([](YouTubeDestinationPrepareCompletion) {});
		destinationCancelStatus = owner->cancelDestinationPreparation({1, 1});
		destinationSnapshot = owner->destinationSnapshot();
		invalidateResult = owner->invalidateForProfileChange();
		shutdownResult = owner->shutdown();
	});
	worker.join();
	CHECK(restoreResult.status == YouTubeAccountProfileRestoreStatus::WrongThread);
	CHECK(!invalidateResult);
	CHECK(!shutdownResult);
	CHECK(!owner->snapshot().closed);
	CHECK(!owner->snapshot().restored);
	CHECK(disconnectResult.status == YouTubeAccountProfileDisconnectStatus::WrongThread);
	CHECK(startStatus == YouTubeAccountConnectionOperationStatus::WrongThread);
	CHECK(channelStatus == YouTubeAccountConnectionOperationStatus::WrongThread);
	CHECK(streamStatus == YouTubeAccountConnectionOperationStatus::WrongThread);
	CHECK(cancelStatus == YouTubeAccountConnectionOperationStatus::WrongThread);
	CHECK(destinationStartStatus == YouTubeAccountDestinationOperationStatus::WrongThread);
	CHECK(destinationCancelStatus == YouTubeAccountDestinationOperationStatus::WrongThread);
	CHECK(!connectionSnapshot.restored);
	CHECK(!connectionSnapshot.closed);
	CHECK(!connectionSnapshot.activeAttempt.has_value());
	CHECK(!destinationSnapshot.restored);
	CHECK(!destinationSnapshot.closed);
	CHECK(!destinationSnapshot.activeAttempt.has_value());
}

void testProfileChangeClearsOldBindingAndRestoresNewOne()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-profile-change.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	saveCredential(*credentialApiRaw, currentPath, selectionA());
	CHECK(makeYouTubeAccountProfileBinding(kProfilePathB).has_value());
	const auto bindingB = makeYouTubeAccountProfileBinding(kProfilePathB);
	if (bindingB.has_value()) {
		WindowsYouTubeAccountRefreshTokenStore store(*credentialApiRaw);
		CHECK(store.write({*bindingB, selectionB().channelId}, "owner-refresh-token-b").succeeded());
	}
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>());
	CHECK(owner->restoreActiveProfile().status == YouTubeAccountProfileRestoreStatus::Restored);
	const auto old = owner->snapshot();
	currentPath = std::string(kProfilePathB);
	setAccountSettings(config.get(), selectionB());
	CHECK(owner->invalidateForProfileChange());
	CHECK(!owner->snapshot().restored);
	CHECK(!YouTubeAccountRuntimeOwnerTestAccess::commitSelection(*owner, selectionA()));
	const auto restored = owner->restoreActiveProfile();
	CHECK(restored.status == YouTubeAccountProfileRestoreStatus::Restored);
	CHECK(owner->snapshot().restored);
	CHECK(owner->snapshot().generation > old.generation);
	CHECK(owner->snapshot().profileBinding != old.profileBinding);
}

void testShutdownRetriesIncompleteNativeCleanup()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-shutdown.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	const auto binding = makeYouTubeAccountProfileBinding(currentPath);
	CHECK(binding.has_value());
	if (!binding.has_value()) {
		return;
	}
	auto *credentialApiRaw = credentialApi.get();
	WindowsYouTubeAccountRefreshTokenStore store(*credentialApiRaw);
	CHECK(store.write({*binding, selectionA().channelId}, "owner-refresh-token").succeeded());
	auto lockApi = std::make_unique<OwnerTestLockApi>();
	auto *lockApiRaw = lockApi.get();
	lockApiRaw->closeResult = false;
	auto owner = makeOwner(currentPath, config, std::move(credentialApi), std::move(lockApi));
	const auto restore = owner->restoreActiveProfile();
	CHECK(restore.status == YouTubeAccountProfileRestoreStatus::OperationFailed);
	CHECK(!owner->shutdown());
	CHECK(owner->snapshot().closed);
	lockApiRaw->closeResult = true;
	CHECK(owner->shutdown());
	const auto closedDisconnect = YouTubeAccountRuntimeOwnerTestAccess::disconnect(*owner);
	CHECK(closedDisconnect.status == YouTubeAccountProfileDisconnectStatus::Closed);
	CHECK(owner->startConnection() == YouTubeAccountConnectionOperationStatus::Closed);
	CHECK(owner->selectChannel({1, 1}, {1, 0}) == YouTubeAccountConnectionOperationStatus::Closed);
	CHECK(owner->selectStream({1, 1}, {1, 0}) == YouTubeAccountConnectionOperationStatus::Closed);
	CHECK(owner->cancelConnection({1, 1}) == YouTubeAccountConnectionOperationStatus::Closed);
	CHECK(owner->connectionSnapshot().stage == YouTubeAccountConnectionStage::Closed);
	CHECK(owner->snapshot().closed);
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testRuntimeOwnerDestinationPreparationSuccessReleasesLockBeforeHandler();
	testRuntimeOwnerDestinationUseLeaseGuardsAccountMutationsAndReleasesExactly();
	testRuntimeOwnerDestinationPreparationFailureDoesNotCreateUseLease();
	testRuntimeOwnerDestinationUseReleaseRestoresChangedProfile();
	testRuntimeOwnerDestinationUseShutdownRetiresLease();
	testRuntimeOwnerDestinationPreparationBlocksOtherAccountOperations();
	testRuntimeOwnerDestinationCompletionRejectsChangedProfileAndRestoresNewOne();
	testRuntimeOwnerDestinationProfileChangeRetriesNativeCleanupBeforeRestore();
	testRuntimeOwnerDestinationShutdownSuppressesLateCallbacks();
	testRuntimeOwnerDestinationCompletionLockFailureFailsClosedAndRetriesOnShutdown();
	testRuntimeOwnerDestinationRotationSurvivesResolverFailureForRemoteRevoke();
	testRuntimeOwnerRemoteRevokeSuccessPublishesSetupRequiredBeforeHandler();
	testRuntimeOwnerBlocksRestoreConnectionAndDisconnectWhileRemoteRevokeRuns();
	testRuntimeOwnerRemoteRevokeNetworkFailurePreservesRestoredState();
	testRuntimeOwnerRemoteRevokeEraseFailureRemainsRetryable();
	testRuntimeOwnerRemoteRevokeProfileSaveFailureFailsClosed();
	testRuntimeOwnerRemoteRevokeLockCleanupFailureFailsClosedAndRetriesOnShutdown();
	testRuntimeOwnerRemoteRevokeImmediateLockCleanupFailureFailsClosed();
	testRuntimeOwnerRestoresChangedProfileAfterQueuedRemoteCompletion();
	testRuntimeOwnerSecondRemoteRevokeKeepsSetupRequiredState();
	testRuntimeOwnerRemoteStartReportsReentrantProfileInvalidation();
	testRuntimeOwnerCancelIgnoresLateRemoteCompletion();
	testRuntimeOwnerProfileInvalidationCancelsRemoteRevoke();
	testRuntimeOwnerShutdownCancelsAndClosesRemotePort();
	testRestoreOnlyOwnerBindsSuccessfulGeneration();
	testConnectionSeamFailsClosedWhenProductionOAuthIsUnconfigured();
	testConnectionSeamStartsAndCancelsForCurrentRestoredProfile();
	testRepeatedRestoreDoesNotOrphanAnActiveConnectionAttempt();
	testRepeatedRestoreCancelsAnAttemptFromAChangedProfile();
	testReentrantProfileInvalidationCannotReportAStartedConnection();
	testReentrantShutdownCannotReportAStartedConnection();
	testConnectionSeamRejectsAProfileSwitchBeforeOpeningBrowser();
	testConnectionSeamRejectsAChangedConnectionModeBeforeOpeningBrowser();
	testActiveConnectionAttemptIsInvalidatedByAProfileSwitch();
	testActiveConnectionAttemptIsInvalidatedByAConnectionModeChange();
	testProfileInvalidationCancelsAnActiveConnectionAttempt();
	testConnectionCancelFailsClosedWhenNativeLockCleanupFails();
	testShutdownCancelsAnActiveConnectionAttemptAndStaysTerminal();
	testLocalDisconnectDeletesCredentialAndKeepsAccountMode();
	testLocalDisconnectIsIdempotentWhenCredentialIsMissing();
	testBusyDisconnectLeavesRestoredOwnerMetadataUntouched();
	testLocalDisconnectWithNoSelectionNeverTouchesCredentialStore();
	testLocalDisconnectManualProfileNeverTouchesCredentialStore();
	testLocalDisconnectSaveFailureDoesNotRestoreErasedCredential();
	testLocalDisconnectCredentialFailureCanRetryWithoutRestore();
	testMissingCredentialRestoresReauthorizationBoundaryWithoutOpeningBrowser();
	testManualProfileNeverReadsAccountCredential();
	testOwnerLifecycleIsThreadBoundWithoutSideEffects();
	testProfileChangeClearsOldBindingAndRestoresNewOne();
	testShutdownRetriesIncompleteNativeCleanup();
	if (failures != 0) {
		std::cerr << failures << " YouTube account runtime owner test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube account runtime owner tests passed\n";
	return 0;
}

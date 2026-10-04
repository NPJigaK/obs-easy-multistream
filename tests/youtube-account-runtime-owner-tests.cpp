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

private:
	std::unordered_map<CREDENTIALW *, std::unique_ptr<Allocation>> allocations;
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
	  YouTubeAccountRuntimeOwner::ConfigReader configReaderOverride = {}, QString clientId = {})
{
	if (!configReaderOverride) {
		configReaderOverride = [&config]() {
			return config.get();
		};
	}
	return std::make_unique<YouTubeAccountRuntimeOwner>([&currentPath]() { return currentPath; },
							    std::move(configReaderOverride), std::move(clientId),
							    std::move(browserOpener), std::move(credentialApi),
							    std::move(lockApi));
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
	CHECK(!connectionSnapshot.restored);
	CHECK(!connectionSnapshot.closed);
	CHECK(!connectionSnapshot.activeAttempt.has_value());
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

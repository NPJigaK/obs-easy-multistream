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

std::unique_ptr<YouTubeAccountRuntimeOwner> makeOwner(
	std::string &currentPath, ConfigHandle &config, std::unique_ptr<WinCredentialApi> credentialApi,
	std::unique_ptr<YouTubeAccountProfileOperationLockApi> lockApi,
	GoogleOAuthAuthorizationSession::BrowserOpener browserOpener = {})
{
	return std::make_unique<YouTubeAccountRuntimeOwner>(
		[&currentPath]() { return currentPath; }, [&config]() { return config.get(); }, QString{},
		std::move(browserOpener),
		std::move(credentialApi), std::move(lockApi));
}

void saveCredential(OwnerTestCredentialApi &api, std::string_view profilePath,
			    const YouTubeAccountSelection &selection)
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

void testMissingCredentialRestoresReauthorizationBoundaryWithoutOpeningBrowser()
{
	ConfigHandle config;
	CHECK(config.open("easy-multistream-runtime-owner-missing-credential.ini"));
	setAccountSettings(config.get(), selectionA());
	std::string currentPath(kProfilePathA);
	auto credentialApi = std::make_unique<OwnerTestCredentialApi>();
	auto *credentialApiRaw = credentialApi.get();
	int browserOpenCount = 0;
	auto owner = makeOwner(
		currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>(),
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
	auto owner = makeOwner(
		currentPath, config, std::move(credentialApi), std::make_unique<OwnerTestLockApi>(),
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
	bool invalidateResult = true;
	bool shutdownResult = true;
	std::thread worker([&]() {
		restoreResult = owner->restoreActiveProfile();
		invalidateResult = owner->invalidateForProfileChange();
		shutdownResult = owner->shutdown();
	});
	worker.join();
	CHECK(restoreResult.status == YouTubeAccountProfileRestoreStatus::WrongThread);
	CHECK(!invalidateResult);
	CHECK(!shutdownResult);
	CHECK(!owner->snapshot().closed);
	CHECK(!owner->snapshot().restored);
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
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	testRestoreOnlyOwnerBindsSuccessfulGeneration();
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

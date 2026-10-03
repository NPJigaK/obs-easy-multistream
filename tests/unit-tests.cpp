// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "onboarding-state.hpp"
#include "settings.hpp"
#include "windows-credential-vault.hpp"

#include <util/config-file.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
constexpr std::string_view kValidYouTubeServerUrl = "rtmps://a.rtmps.youtube.com/live2";

easy_multistream::YouTubeAccountSelection accountSelection()
{
	return {"UC-account-channel", u8"配信チャンネル", "stream-reusable", u8"いつもの配信"};
}

#define CHECK(expression)                                                                                              \
	do {                                                                                                           \
		if (!(expression)) {                                                                                     \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';             \
			++failures;                                                                                         \
		}                                                                                                          \
	} while (false)

class Config final {
public:
	explicit Config(const char *contents)
	{
		const int result = config_open_string(&config_, contents);
		CHECK(result == CONFIG_SUCCESS);
	}

	~Config() { config_close(config_); }

	Config(const Config &) = delete;
	Config &operator=(const Config &) = delete;

	config_t *get() const noexcept { return config_; }

private:
	config_t *config_ = nullptr;
};

class FakeWinCredentialApi final : public easy_multistream::WinCredentialApi {
public:
	bool write(PCREDENTIALW credential, DWORD flags, DWORD &error) noexcept override
	{
		++writeCalls;
		writeFlags = flags;
		writeType = credential->Type;
		writePersist = credential->Persist;
		writeTarget = credential->TargetName != nullptr ? credential->TargetName : L"";
		if (credential->CredentialBlob != nullptr && credential->CredentialBlobSize != 0) {
			writeBlob.assign(credential->CredentialBlob,
					 credential->CredentialBlob + credential->CredentialBlobSize);
		} else {
			writeBlob.clear();
		}
		error = writeError;
		return writeSucceeds;
	}

	bool read(LPCWSTR targetName, DWORD type, DWORD flags, PCREDENTIALW &credential, DWORD &error) noexcept override
	{
		++readCalls;
		readTarget = targetName != nullptr ? targetName : L"";
		readType = type;
		readFlags = flags;
		error = readError;
		if (!readSucceeds) {
			credential = nullptr;
			return false;
		}

		readCredential = {};
		readCredential.Type = credentialType;
		readCredential.CredentialBlobSize = credentialBlobSizeOverride == 0
							    ? static_cast<DWORD>(readBlob.size())
							    : credentialBlobSizeOverride;
		readCredential.CredentialBlob = readBlob.empty() ? nullptr : readBlob.data();
		credential = &readCredential;
		return true;
	}

	bool erase(LPCWSTR targetName, DWORD type, DWORD flags, DWORD &error) noexcept override
	{
		++eraseCalls;
		eraseTarget = targetName != nullptr ? targetName : L"";
		eraseType = type;
		eraseFlags = flags;
		error = eraseError;
		return eraseSucceeds;
	}

	void freeCredential(PVOID credential) noexcept override
	{
		CHECK(credential == &readCredential);
		++freeCalls;
	}

	void setReadBlob(std::string_view value)
	{
		readBlob.assign(reinterpret_cast<const BYTE *>(value.data()),
				reinterpret_cast<const BYTE *>(value.data()) + value.size());
		credentialBlobSizeOverride = 0;
		credentialType = CRED_TYPE_GENERIC;
		readSucceeds = true;
		readError = ERROR_SUCCESS;
	}

	bool writeSucceeds = true;
	DWORD writeError = ERROR_SUCCESS;
	int writeCalls = 0;
	DWORD writeFlags = 0;
	DWORD writeType = 0;
	DWORD writePersist = 0;
	std::wstring writeTarget;
	std::vector<BYTE> writeBlob;

	bool readSucceeds = false;
	DWORD readError = ERROR_NOT_FOUND;
	int readCalls = 0;
	DWORD readFlags = 0;
	DWORD readType = 0;
	DWORD credentialType = CRED_TYPE_GENERIC;
	DWORD credentialBlobSizeOverride = 0;
	std::wstring readTarget;
	std::vector<BYTE> readBlob;
	CREDENTIALW readCredential{};
	int freeCalls = 0;

	bool eraseSucceeds = true;
	DWORD eraseError = ERROR_SUCCESS;
	int eraseCalls = 0;
	DWORD eraseFlags = 0;
	DWORD eraseType = 0;
	std::wstring eraseTarget;
};

void testSettingsDefaults()
{
	Config config("");
	const auto loaded = easy_multistream::loadProfileSettings(config.get());
	CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Defaults);
	CHECK(!loaded.settings.youtubeEnabled);
	CHECK(loaded.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Manual);
	CHECK(loaded.settings.youtubeServerUrl.empty());
	CHECK(!loaded.settings.youtubeAccountSelection.has_value());
	CHECK(loaded.sourceSchemaVersion == easy_multistream::kSettingsSchemaVersion);
}

void testOnboardingPreference()
{
	CHECK(!easy_multistream::shouldAutoShowDock(nullptr));
	CHECK(easy_multistream::markDockAutoShowHandled(nullptr) == CONFIG_ERROR);

	Config fresh("");
	CHECK(easy_multistream::shouldAutoShowDock(fresh.get()));

	// The marker is intentionally presence-based.  A malformed or manually
	// edited value must not make the plugin repeatedly take over the UI.
	Config alreadyHandled("[EasyMultistream]\nDockAutoShowHandled=false\n");
	CHECK(!easy_multistream::shouldAutoShowDock(alreadyHandled.get()));

	const std::filesystem::path path = std::filesystem::current_path() / "easy-multistream-onboarding-test.ini";
	std::error_code removeError;
	std::filesystem::remove(path, removeError);
	std::filesystem::remove(path.string() + ".tmp", removeError);

	config_t *config = nullptr;
	const std::string utf8Path = path.u8string();
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		config_set_string(config, "OtherPlugin", "Preserved", "yes");
		CHECK(easy_multistream::shouldAutoShowDock(config));
		CHECK(easy_multistream::markDockAutoShowHandled(config) == CONFIG_SUCCESS);
		CHECK(!easy_multistream::shouldAutoShowDock(config));
		config_close(config);
	}

	config = nullptr;
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		CHECK(!easy_multistream::shouldAutoShowDock(config));
		CHECK(std::string(config_get_string(config, "OtherPlugin", "Preserved")) == "yes");
		config_close(config);
	}
	CHECK(std::filesystem::remove(path, removeError));

	const std::filesystem::path failureDirectory =
		std::filesystem::current_path() / "easy-multistream-onboarding-save-failure-test";
	const std::filesystem::path failurePath = failureDirectory / "user.ini";
	std::filesystem::remove_all(failureDirectory, removeError);
	CHECK(std::filesystem::create_directory(failureDirectory, removeError));
	config = nullptr;
	const std::string failureUtf8Path = failurePath.u8string();
	CHECK(config_open(&config, failureUtf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		config_set_string(config, "OtherPlugin", "Preserved", "yes");
		CHECK(config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS);
		CHECK(std::filesystem::remove(failurePath, removeError));
		CHECK(std::filesystem::remove(failureDirectory, removeError));
		CHECK(easy_multistream::markDockAutoShowHandled(config) != CONFIG_SUCCESS);
		CHECK(easy_multistream::shouldAutoShowDock(config));
		config_close(config);
	}
	CHECK(!std::filesystem::exists(failurePath));
	CHECK(!std::filesystem::exists(failurePath.string() + ".tmp"));
}

void testSettingsRoundTripAndPlaintextRemoval()
{
	Config config("[EasyMultistream]\nStreamKey=must-not-survive\n");
	easy_multistream::Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeServerUrl = kValidYouTubeServerUrl;
	easy_multistream::writeProfileSettings(config.get(), settings);

	const auto loaded = easy_multistream::loadProfileSettings(config.get());
	CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(loaded.settings.youtubeEnabled);
	CHECK(loaded.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Manual);
	CHECK(loaded.settings.youtubeServerUrl == kValidYouTubeServerUrl);
	CHECK(!loaded.settings.youtubeAccountSelection.has_value());
	CHECK(!config_has_user_value(config.get(), "EasyMultistream", "StreamKey"));
}

void testSettingsConnectionModesAndMigration()
{
	for (const char *schema : {"1", "2"}) {
		const std::string contents =
			std::string("[EasyMultistream]\nSchemaVersion=") + schema +
			"\nYouTubeEnabled=true\nYouTubeServerUrl=" + std::string(kValidYouTubeServerUrl) + "\n";
		Config legacy(contents.c_str());
		const auto loaded = easy_multistream::loadProfileSettings(legacy.get());
		CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
		CHECK(loaded.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Manual);
		CHECK(!loaded.settings.youtubeAccountSelection.has_value());
	}

	Config account("[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=account\n"
		       "YouTubeChannelId=UC-account-channel\nYouTubeChannelLabel=Channel\n"
		       "YouTubeStreamId=stream-reusable\nYouTubeStreamLabel=Stream\n");
	const auto connected = easy_multistream::loadProfileSettings(account.get());
	CHECK(connected.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(connected.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Account);
	CHECK(connected.settings.youtubeServerUrl.empty());
	CHECK(connected.settings.youtubeAccountSelection.has_value());
	if (connected.settings.youtubeAccountSelection.has_value()) {
		CHECK(connected.settings.youtubeAccountSelection->channelId == "UC-account-channel");
		CHECK(connected.settings.youtubeAccountSelection->streamId == "stream-reusable");
	}

	Config incompleteAccount("[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=account\n");
	const auto setupRequired = easy_multistream::loadProfileSettings(incompleteAccount.get());
	CHECK(setupRequired.status == easy_multistream::SettingsLoadStatus::SetupRequired);
	CHECK(setupRequired.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Account);

	Config unknownMode("[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=automatic\n");
	CHECK(easy_multistream::loadProfileSettings(unknownMode.get()).status ==
	      easy_multistream::SettingsLoadStatus::InvalidSchema);

	Config partialSelection("[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=account\n"
				"YouTubeChannelId=UC-account-channel\n");
	CHECK(easy_multistream::loadProfileSettings(partialSelection.get()).status ==
	      easy_multistream::SettingsLoadStatus::InvalidSchema);

	Config emptyLabel("[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=account\n"
			  "YouTubeChannelId=UC-account-channel\nYouTubeChannelLabel=\n"
			  "YouTubeStreamId=stream-reusable\nYouTubeStreamLabel=Stream\n");
	CHECK(easy_multistream::loadProfileSettings(emptyLabel.get()).status ==
	      easy_multistream::SettingsLoadStatus::InvalidSchema);

	Config manualWithSelection("[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=false\nConnectionMode=manual\n"
				   "YouTubeServerUrl=rtmps://a.rtmps.youtube.com/live2\n"
				   "YouTubeChannelId=UC-account-channel\nYouTubeChannelLabel=Channel\n"
				   "YouTubeStreamId=stream-reusable\nYouTubeStreamLabel=Stream\n");
	const auto preserved = easy_multistream::loadProfileSettings(manualWithSelection.get());
	CHECK(preserved.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(preserved.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Manual);
	CHECK(preserved.settings.youtubeAccountSelection.has_value());

	Config manualWithDamagedSelection(
		"[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=manual\n"
		"YouTubeServerUrl=rtmps://a.rtmps.youtube.com/live2\nYouTubeChannelId=partial-only\n");
	const auto recoveredManual = easy_multistream::loadProfileSettings(manualWithDamagedSelection.get());
	CHECK(recoveredManual.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(recoveredManual.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Manual);
	CHECK(!recoveredManual.settings.youtubeAccountSelection.has_value());

	Config accountWithDamagedManualUrl(
		"[EasyMultistream]\nSchemaVersion=3\nYouTubeEnabled=true\nConnectionMode=account\n"
		"YouTubeServerUrl=https://unsafe.example/live\n"
		"YouTubeChannelId=UC-account-channel\nYouTubeChannelLabel=Channel\n"
		"YouTubeStreamId=stream-reusable\nYouTubeStreamLabel=Stream\n");
	const auto recoveredAccount = easy_multistream::loadProfileSettings(accountWithDamagedManualUrl.get());
	CHECK(recoveredAccount.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(recoveredAccount.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Account);
	CHECK(recoveredAccount.settings.youtubeServerUrl.empty());
	CHECK(recoveredAccount.settings.youtubeAccountSelection.has_value());
}

void testAccountSettingsRoundTripAndSecretExclusion()
{
	Config config("[EasyMultistream]\nRefreshToken=refresh-token-must-not-survive\n"
		      "AccessToken=access-token-must-not-survive\nAuthorizationCode=code-must-not-survive\n"
		      "CodeVerifier=verifier-must-not-survive\nStreamName=stream-name-must-not-survive\n");
	easy_multistream::Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = easy_multistream::YouTubeConnectionMode::Account;
	settings.youtubeServerUrl = std::string(kValidYouTubeServerUrl);
	settings.youtubeAccountSelection = accountSelection();
	easy_multistream::writeProfileSettings(config.get(), settings);

	const auto loaded = easy_multistream::loadProfileSettings(config.get());
	CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(loaded.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Account);
	CHECK(loaded.settings.youtubeAccountSelection.has_value());
	if (loaded.settings.youtubeAccountSelection.has_value()) {
		CHECK(loaded.settings.youtubeAccountSelection->channelId ==
		      settings.youtubeAccountSelection->channelId);
		CHECK(loaded.settings.youtubeAccountSelection->channelLabel ==
		      settings.youtubeAccountSelection->channelLabel);
		CHECK(loaded.settings.youtubeAccountSelection->streamId == settings.youtubeAccountSelection->streamId);
		CHECK(loaded.settings.youtubeAccountSelection->streamLabel ==
		      settings.youtubeAccountSelection->streamLabel);
	}
	for (const char *forbidden :
	     {"RefreshToken", "AccessToken", "AuthorizationCode", "CodeVerifier", "StreamName"}) {
		CHECK(!config_has_user_value(config.get(), "EasyMultistream", forbidden));
	}

	easy_multistream::Settings invalid = settings;
	invalid.youtubeAccountSelection->streamLabel.clear();
	config_set_string(config.get(), "EasyMultistream", "RefreshToken", "remove-even-on-invalid-write");
	easy_multistream::writeProfileSettings(config.get(), invalid);
	CHECK(!config_has_user_value(config.get(), "EasyMultistream", "RefreshToken"));
	CHECK(easy_multistream::saveProfileSettings(config.get(), invalid) == CONFIG_ERROR);
	const auto afterRejectedSave = easy_multistream::loadProfileSettings(config.get());
	CHECK(afterRejectedSave.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(afterRejectedSave.settings.youtubeAccountSelection.has_value());

	easy_multistream::Settings invalidMode = settings;
	invalidMode.youtubeConnectionMode = static_cast<easy_multistream::YouTubeConnectionMode>(99);
	CHECK(easy_multistream::saveProfileSettings(config.get(), invalidMode) == CONFIG_ERROR);
}

void testAccountSettingsSafeSaveAndReload()
{
	const std::filesystem::path path =
		std::filesystem::current_path() / "easy-multistream-account-settings-test.ini";
	std::error_code removeError;
	std::filesystem::remove(path, removeError);
	std::filesystem::remove(path.string() + ".tmp", removeError);

	config_t *config = nullptr;
	const std::string utf8Path = path.u8string();
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		easy_multistream::Settings settings;
		settings.youtubeEnabled = true;
		settings.youtubeConnectionMode = easy_multistream::YouTubeConnectionMode::Account;
		settings.youtubeAccountSelection = accountSelection();
		CHECK(easy_multistream::saveProfileSettings(config, settings) == CONFIG_SUCCESS);
		config_close(config);
	}

	config = nullptr;
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		const auto loaded = easy_multistream::loadProfileSettings(config);
		CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
		CHECK(loaded.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Account);
		CHECK(loaded.settings.youtubeServerUrl.empty());
		CHECK(loaded.settings.youtubeAccountSelection.has_value());
		if (loaded.settings.youtubeAccountSelection.has_value()) {
			CHECK(loaded.settings.youtubeAccountSelection->channelLabel == u8"配信チャンネル");
			CHECK(loaded.settings.youtubeAccountSelection->streamLabel == u8"いつもの配信");
		}
		config_close(config);
	}

	std::ifstream savedFile(path, std::ios::binary);
	const std::string contents{std::istreambuf_iterator<char>(savedFile), std::istreambuf_iterator<char>()};
	savedFile.close();
	CHECK(contents.find("ConnectionMode=account") != std::string::npos);
	CHECK(contents.find("RefreshToken") == std::string::npos);
	CHECK(contents.find("AccessToken") == std::string::npos);
	CHECK(contents.find("StreamKey") == std::string::npos);
	CHECK(contents.find("StreamName") == std::string::npos);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	CHECK(std::filesystem::remove(path, removeError));
}

void testInvalidSettingsStillScrubPlaintextSecretsFromDisk()
{
	const std::filesystem::path path =
		std::filesystem::current_path() / "easy-multistream-invalid-settings-scrub-test.ini";
	std::error_code removeError;
	std::filesystem::remove(path, removeError);
	std::filesystem::remove(path.string() + ".tmp", removeError);

	config_t *config = nullptr;
	const std::string utf8Path = path.u8string();
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config == nullptr) {
		return;
	}
	config_set_uint(config, "EasyMultistream", "SchemaVersion", easy_multistream::kSettingsSchemaVersion);
	config_set_bool(config, "EasyMultistream", "YouTubeEnabled", false);
	config_set_string(config, "EasyMultistream", "ConnectionMode", "manual");
	config_set_string(config, "EasyMultistream", "YouTubeServerUrl", kValidYouTubeServerUrl.data());
	config_set_string(config, "EasyMultistream", "RefreshToken", "invalid-save-secret-sentinel");
	CHECK(config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS);

	easy_multistream::Settings invalid;
	invalid.youtubeConnectionMode = easy_multistream::YouTubeConnectionMode::Account;
	CHECK(easy_multistream::saveProfileSettings(config, invalid) == CONFIG_ERROR);
	CHECK(!config_has_user_value(config, "EasyMultistream", "RefreshToken"));
	config_close(config);

	std::ifstream savedFile(path, std::ios::binary);
	const std::string contents{std::istreambuf_iterator<char>(savedFile), std::istreambuf_iterator<char>()};
	savedFile.close();
	CHECK(contents.find("invalid-save-secret-sentinel") == std::string::npos);
	CHECK(contents.find("RefreshToken") == std::string::npos);
	CHECK(contents.find("ConnectionMode=manual") != std::string::npos);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	CHECK(std::filesystem::remove(path, removeError));
}

void testPlaintextScrubSaveFailureIsRecoverable()
{
	const std::filesystem::path directory =
		std::filesystem::current_path() / "easy-multistream-secret-scrub-failure-test";
	const std::filesystem::path movedDirectory =
		std::filesystem::current_path() / "easy-multistream-secret-scrub-failure-moved";
	const std::filesystem::path path = directory / "basic.ini";
	const std::filesystem::path movedPath = movedDirectory / "basic.ini";
	std::error_code filesystemError;
	std::filesystem::remove_all(directory, filesystemError);
	std::filesystem::remove_all(movedDirectory, filesystemError);
	CHECK(std::filesystem::create_directory(directory, filesystemError));

	config_t *config = nullptr;
	const std::string utf8Path = path.u8string();
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config == nullptr) {
		std::filesystem::remove_all(directory, filesystemError);
		return;
	}
	config_set_uint(config, "EasyMultistream", "SchemaVersion", easy_multistream::kSettingsSchemaVersion);
	config_set_bool(config, "EasyMultistream", "YouTubeEnabled", false);
	config_set_string(config, "EasyMultistream", "ConnectionMode", "manual");
	config_set_string(config, "EasyMultistream", "YouTubeServerUrl", kValidYouTubeServerUrl.data());
	config_set_string(config, "EasyMultistream", "RefreshToken", "scrub-retry-secret-sentinel");
	CHECK(config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS);
	std::filesystem::rename(directory, movedDirectory, filesystemError);
	CHECK(!filesystemError);

	easy_multistream::Settings invalid;
	invalid.youtubeConnectionMode = easy_multistream::YouTubeConnectionMode::Account;
	CHECK(easy_multistream::saveProfileSettings(config, invalid) != CONFIG_SUCCESS);
	CHECK(!config_has_user_value(config, "EasyMultistream", "RefreshToken"));

	std::ifstream unchangedFile(movedPath, std::ios::binary);
	const std::string unchangedContents{std::istreambuf_iterator<char>(unchangedFile),
					    std::istreambuf_iterator<char>()};
	unchangedFile.close();
	CHECK(unchangedContents.find("scrub-retry-secret-sentinel") != std::string::npos);

	filesystemError.clear();
	std::filesystem::rename(movedDirectory, directory, filesystemError);
	CHECK(!filesystemError);
	easy_multistream::Settings valid;
	valid.youtubeServerUrl = std::string(kValidYouTubeServerUrl);
	CHECK(easy_multistream::saveProfileSettings(config, valid) == CONFIG_SUCCESS);
	config_close(config);

	std::ifstream repairedFile(path, std::ios::binary);
	const std::string repairedContents{std::istreambuf_iterator<char>(repairedFile),
					   std::istreambuf_iterator<char>()};
	repairedFile.close();
	CHECK(repairedContents.find("scrub-retry-secret-sentinel") == std::string::npos);
	CHECK(repairedContents.find("RefreshToken") == std::string::npos);
	CHECK(std::filesystem::remove_all(directory, filesystemError) != 0);
}

void testSettingsSafeSaveAndReload()
{
	const std::filesystem::path path = std::filesystem::current_path() / "easy-multistream-settings-test.ini";
	std::error_code removeError;
	std::filesystem::remove(path, removeError);
	std::filesystem::remove(path.string() + ".tmp", removeError);

	config_t *config = nullptr;
	const std::string utf8Path = path.u8string();
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		config_set_string(config, "OtherPlugin", "Preserved", "yes");
		config_set_string(config, "EasyMultistream", "YouTubeStreamKey", "plaintext-must-not-survive");
		config_set_string(config, "EasyMultistream", "RefreshToken", "refresh-token-must-not-survive");
		config_set_string(config, "EasyMultistream", "AccessToken", "access-token-must-not-survive");
		config_set_string(config, "EasyMultistream", "AuthorizationCode", "code-must-not-survive");
		config_set_string(config, "EasyMultistream", "CodeVerifier", "verifier-must-not-survive");
		config_set_string(config, "EasyMultistream", "StreamName", "stream-name-must-not-survive");
		easy_multistream::Settings settings;
		settings.youtubeEnabled = true;
		settings.youtubeServerUrl = kValidYouTubeServerUrl;
		CHECK(easy_multistream::saveProfileSettings(config, settings) == CONFIG_SUCCESS);
		config_close(config);
	}

	config = nullptr;
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_EXISTING) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config != nullptr) {
		const auto loaded = easy_multistream::loadProfileSettings(config);
		CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
		CHECK(loaded.settings.youtubeEnabled);
		CHECK(loaded.settings.youtubeServerUrl == kValidYouTubeServerUrl);
		CHECK(std::string(config_get_string(config, "OtherPlugin", "Preserved")) == "yes");
		CHECK(!config_has_user_value(config, "EasyMultistream", "YouTubeStreamKey"));
		config_close(config);
	}

	std::ifstream savedFile(path, std::ios::binary);
	const std::string contents{std::istreambuf_iterator<char>(savedFile), std::istreambuf_iterator<char>()};
	savedFile.close();
	CHECK(contents.find("plaintext-must-not-survive") == std::string::npos);
	CHECK(contents.find("refresh-token-must-not-survive") == std::string::npos);
	CHECK(contents.find("access-token-must-not-survive") == std::string::npos);
	CHECK(contents.find("code-must-not-survive") == std::string::npos);
	CHECK(contents.find("verifier-must-not-survive") == std::string::npos);
	CHECK(contents.find("stream-name-must-not-survive") == std::string::npos);
	CHECK(contents.find("StreamKey") == std::string::npos);
	CHECK(contents.find("RefreshToken") == std::string::npos);
	CHECK(contents.find("AccessToken") == std::string::npos);
	CHECK(contents.find("AuthorizationCode") == std::string::npos);
	CHECK(contents.find("CodeVerifier") == std::string::npos);
	CHECK(contents.find("StreamName") == std::string::npos);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	CHECK(std::filesystem::remove(path, removeError));
}

void testSettingsSaveFailureRollsBackInMemoryValues()
{
	const std::filesystem::path directory = std::filesystem::current_path() / "easy-multistream-save-failure-test";
	const std::filesystem::path path = directory / "basic.ini";
	std::error_code filesystemError;
	std::filesystem::remove_all(directory, filesystemError);
	CHECK(std::filesystem::create_directory(directory, filesystemError));

	config_t *config = nullptr;
	const std::string utf8Path = path.u8string();
	CHECK(config_open(&config, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS);
	CHECK(config != nullptr);
	if (config == nullptr) {
		std::filesystem::remove_all(directory, filesystemError);
		return;
	}

	config_set_uint(config, "EasyMultistream", "SchemaVersion", easy_multistream::kSettingsSchemaVersion);
	config_set_bool(config, "EasyMultistream", "YouTubeEnabled", false);
	config_set_string(config, "EasyMultistream", "ConnectionMode", "account");
	config_set_string(config, "EasyMultistream", "YouTubeServerUrl", kValidYouTubeServerUrl.data());
	config_set_string(config, "EasyMultistream", "YouTubeChannelId", "UC-original");
	config_set_string(config, "EasyMultistream", "YouTubeChannelLabel", "Original channel");
	config_set_string(config, "EasyMultistream", "YouTubeStreamId", "stream-original");
	config_set_string(config, "EasyMultistream", "YouTubeStreamLabel", "Original stream");
	CHECK(config_save_safe(config, "tmp", nullptr) == CONFIG_SUCCESS);
	CHECK(std::filesystem::remove(path, filesystemError));
	CHECK(std::filesystem::remove(directory, filesystemError));

	easy_multistream::Settings desired;
	desired.youtubeEnabled = true;
	desired.youtubeServerUrl = "rtmps://b.rtmps.youtube.com/live2";
	CHECK(easy_multistream::saveProfileSettings(config, desired) != CONFIG_SUCCESS);
	const auto afterFailure = easy_multistream::loadProfileSettings(config);
	CHECK(afterFailure.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(!afterFailure.settings.youtubeEnabled);
	CHECK(afterFailure.settings.youtubeConnectionMode == easy_multistream::YouTubeConnectionMode::Account);
	CHECK(afterFailure.settings.youtubeServerUrl == kValidYouTubeServerUrl);
	CHECK(afterFailure.settings.youtubeAccountSelection.has_value());
	if (afterFailure.settings.youtubeAccountSelection.has_value()) {
		CHECK(afterFailure.settings.youtubeAccountSelection->channelId == "UC-original");
		CHECK(afterFailure.settings.youtubeAccountSelection->streamId == "stream-original");
	}
	config_close(config);

	CHECK(!std::filesystem::exists(path));
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
}

void testFutureSchemaIsDisabled()
{
	Config config("[EasyMultistream]\nSchemaVersion=99\nYouTubeEnabled=true\n");
	const auto loaded = easy_multistream::loadProfileSettings(config.get());
	CHECK(loaded.status == easy_multistream::SettingsLoadStatus::UnsupportedFutureSchema);
	CHECK(!loaded.settings.youtubeEnabled);
	CHECK(loaded.sourceSchemaVersion == 99);
}

void testInvalidSchemaAndUnavailableConfig()
{
	Config config(
		"[EasyMultistream]\nSchemaVersion=0\nYouTubeEnabled=true\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto invalid = easy_multistream::loadProfileSettings(config.get());
	CHECK(invalid.status == easy_multistream::SettingsLoadStatus::InvalidSchema);
	CHECK(!invalid.settings.youtubeEnabled);
	CHECK(invalid.sourceSchemaVersion == 0);

	Config trailing(
		"[EasyMultistream]\nSchemaVersion=1garbage\nYouTubeEnabled=true\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto trailingResult = easy_multistream::loadProfileSettings(trailing.get());
	CHECK(trailingResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);
	CHECK(!trailingResult.settings.youtubeEnabled);

	Config overflow(
		"[EasyMultistream]\nSchemaVersion=18446744073709551616\nYouTubeEnabled=true\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto overflowResult = easy_multistream::loadProfileSettings(overflow.get());
	CHECK(overflowResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);
	CHECK(!overflowResult.settings.youtubeEnabled);

	Config trailingBool(
		"[EasyMultistream]\nSchemaVersion=1\nYouTubeEnabled=1garbage\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto trailingBoolResult = easy_multistream::loadProfileSettings(trailingBool.get());
	CHECK(trailingBoolResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);
	CHECK(!trailingBoolResult.settings.youtubeEnabled);

	Config textBool(
		"[EasyMultistream]\nSchemaVersion=1\nYouTubeEnabled=garbage\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto textBoolResult = easy_multistream::loadProfileSettings(textBool.get());
	CHECK(textBoolResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);
	CHECK(!textBoolResult.settings.youtubeEnabled);

	Config missingEnabled("[EasyMultistream]\nSchemaVersion=1\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto missingEnabledResult = easy_multistream::loadProfileSettings(missingEnabled.get());
	CHECK(missingEnabledResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);

	Config missingSchema("[EasyMultistream]\nYouTubeEnabled=true\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto missingSchemaResult = easy_multistream::loadProfileSettings(missingSchema.get());
	CHECK(missingSchemaResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);

	Config orphanServerUrl("[EasyMultistream]\nYouTubeServerUrl=rtmps://a.example/live2\n");
	const auto orphanServerUrlResult = easy_multistream::loadProfileSettings(orphanServerUrl.get());
	CHECK(orphanServerUrlResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);

	Config missingServerUrl("[EasyMultistream]\nSchemaVersion=1\nYouTubeEnabled=true\n");
	const auto missingServerUrlResult = easy_multistream::loadProfileSettings(missingServerUrl.get());
	CHECK(missingServerUrlResult.status == easy_multistream::SettingsLoadStatus::SetupRequired);
	CHECK(missingServerUrlResult.settings.youtubeEnabled);
	CHECK(missingServerUrlResult.sourceSchemaVersion == 1);

	Config invalidServerUrl(
		"[EasyMultistream]\nSchemaVersion=2\nYouTubeEnabled=true\nYouTubeServerUrl=https://a.example/live2\n");
	const auto invalidServerUrlResult = easy_multistream::loadProfileSettings(invalidServerUrl.get());
	CHECK(invalidServerUrlResult.status == easy_multistream::SettingsLoadStatus::InvalidSchema);

	const auto unavailable = easy_multistream::loadProfileSettings(nullptr);
	CHECK(unavailable.status == easy_multistream::SettingsLoadStatus::Unavailable);
	CHECK(easy_multistream::saveProfileSettings(nullptr, {}) == CONFIG_ERROR);
}

void testStreamKeyValidation()
{
	using ValidationError = easy_multistream::StreamKeyValidationError;
	CHECK(easy_multistream::validateYouTubeStreamKey("") == ValidationError::Empty);
	CHECK(easy_multistream::validateYouTubeStreamKey("valid-key") == ValidationError::None);
	CHECK(easy_multistream::validateYouTubeStreamKey(" leading") == ValidationError::WhitespaceOrControlCharacter);
	CHECK(easy_multistream::validateYouTubeStreamKey("line\nbreak") ==
	      ValidationError::WhitespaceOrControlCharacter);
	CHECK(easy_multistream::validateYouTubeStreamKey(
		      std::string(easy_multistream::kMaxCredentialSecretBytes + 1, 'a')) == ValidationError::TooLong);
	CHECK(easy_multistream::validateYouTubeStreamKey(
		      std::string(easy_multistream::kMaxCredentialSecretBytes, 'a')) == ValidationError::None);
	CHECK(easy_multistream::validateYouTubeStreamKey(u8"日本語-key") == ValidationError::None);
	const std::string embeddedNull("abc\0def", 7);
	CHECK(easy_multistream::validateYouTubeStreamKey(embeddedNull) ==
	      ValidationError::WhitespaceOrControlCharacter);
	const char invalidUtf8[] = {static_cast<char>(0xc3), static_cast<char>(0x28)};
	CHECK(easy_multistream::validateYouTubeStreamKey({invalidUtf8, sizeof(invalidUtf8)}) ==
	      ValidationError::InvalidUtf8);
}

void testYouTubeServerUrlValidation()
{
	using ValidationError = easy_multistream::YouTubeServerUrlValidationError;
	CHECK(easy_multistream::validateYouTubeServerUrl(kValidYouTubeServerUrl) == ValidationError::None);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com:443/live2") ==
	      ValidationError::None);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://B.RTMPS.YOUTUBE.COM/live2") == ValidationError::None);
	CHECK(easy_multistream::validateYouTubeServerUrl("RTMPS://a.rtmps.youtube.com/live2") ==
	      ValidationError::InvalidScheme);

	CHECK(easy_multistream::validateYouTubeServerUrl("") == ValidationError::Empty);
	CHECK(easy_multistream::validateYouTubeServerUrl("https://a.rtmps.youtube.com/live2") ==
	      ValidationError::InvalidScheme);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmp://a.rtmps.youtube.com/live2") ==
	      ValidationError::InvalidScheme);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps:///live2") == ValidationError::MissingHostname);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://:443/live2") == ValidationError::MissingHostname);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://user@a.rtmps.youtube.com/live2") ==
	      ValidationError::UserInfoNotAllowed);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com:8443/live2") ==
	      ValidationError::InvalidPort);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com:abc/live2") ==
	      ValidationError::InvalidPort);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com/live2?key=secret") ==
	      ValidationError::QueryNotAllowed);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com/live2#fragment") ==
	      ValidationError::FragmentNotAllowed);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com/live2/secret-key") ==
	      ValidationError::InvalidPath);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com") ==
	      ValidationError::InvalidPath);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://a.rtmps.youtube.com/live%32") ==
	      ValidationError::InvalidPath);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://evil.example/live2") ==
	      ValidationError::UnsupportedHostname);
	CHECK(easy_multistream::validateYouTubeServerUrl("rtmps://rtmps.youtube.com/live2") ==
	      ValidationError::UnsupportedHostname);
	CHECK(easy_multistream::validateYouTubeServerUrl(" rtmps://a.rtmps.youtube.com/live2") ==
	      ValidationError::WhitespaceOrControlCharacter);
	CHECK(easy_multistream::validateYouTubeServerUrl(
		      std::string(easy_multistream::kMaxYouTubeServerUrlBytes + 1, 'a')) == ValidationError::TooLong);
	const std::string embeddedNull("rtmps://a.rtmps.youtube.com/live\0", 34);
	CHECK(easy_multistream::validateYouTubeServerUrl(embeddedNull) == ValidationError::EmbeddedNull);
	const char invalidUtf8[] = {
		'r', 't', 'm', 'p', 's', ':', '/', '/', 'a', '.', static_cast<char>(0xc3), static_cast<char>(0x28),
		'/', 'l', 'i', 'v', 'e', '2'};
	CHECK(easy_multistream::validateYouTubeServerUrl({invalidUtf8, sizeof(invalidUtf8)}) ==
	      ValidationError::InvalidUtf8);
}

void testSettingsInvalidServerUrlIsRejectedBeforeSave()
{
	Config config("[EasyMultistream]\nSchemaVersion=2\nYouTubeEnabled=false\n"
		      "YouTubeServerUrl=rtmps://a.rtmps.youtube.com/live2\n");
	easy_multistream::Settings invalid;
	invalid.youtubeServerUrl = "rtmp://a.example/live2?key=secret";
	CHECK(easy_multistream::saveProfileSettings(config.get(), invalid) == CONFIG_ERROR);
	const auto loaded = easy_multistream::loadProfileSettings(config.get());
	CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(loaded.settings.youtubeServerUrl == "rtmps://a.rtmps.youtube.com/live2");
}

void testCredentialWriteContract()
{
	FakeWinCredentialApi api;
	easy_multistream::WindowsCredentialVault vault(api);
	const auto result = vault.write("valid-key");
	CHECK(result.succeeded());
	CHECK(api.writeCalls == 1);
	CHECK(api.writeFlags == 0);
	CHECK(api.writeType == CRED_TYPE_GENERIC);
	CHECK(api.writePersist == CRED_PERSIST_LOCAL_MACHINE);
	CHECK(api.writeTarget == easy_multistream::defaultYouTubeCredentialTarget());
	CHECK(std::string(api.writeBlob.begin(), api.writeBlob.end()) == "valid-key");
}

void testAccountCredentialTargetIsIsolated()
{
	const std::wstring streamKeyTarget = easy_multistream::defaultYouTubeCredentialTarget();
	const std::wstring refreshTokenTarget = easy_multistream::defaultYouTubeAccountRefreshTokenTarget();
	CHECK(!streamKeyTarget.empty());
	CHECK(!refreshTokenTarget.empty());
	CHECK(streamKeyTarget != refreshTokenTarget);

	FakeWinCredentialApi api;
	easy_multistream::WindowsCredentialVault refreshTokenVault(api, refreshTokenTarget);
	CHECK(refreshTokenVault.write("test-refresh-token").succeeded());
	CHECK(api.writeTarget == refreshTokenTarget);
	CHECK(api.writeTarget != streamKeyTarget);
	CHECK(std::string(api.writeBlob.begin(), api.writeBlob.end()) == "test-refresh-token");
}

void testCredentialInputValidation()
{
	FakeWinCredentialApi api;
	easy_multistream::WindowsCredentialVault vault(api);
	CHECK(vault.write("").error == easy_multistream::CredentialError::InvalidInput);
	CHECK(vault.write(std::string(easy_multistream::kMaxCredentialSecretBytes + 1, 'a')).error ==
	      easy_multistream::CredentialError::InvalidInput);
	const std::string embeddedNull("abc\0def", 7);
	CHECK(vault.write(embeddedNull).error == easy_multistream::CredentialError::InvalidInput);
	CHECK(api.writeCalls == 0);
}

void testCredentialWriteFailureMapping()
{
	FakeWinCredentialApi api;
	api.writeSucceeds = false;
	api.writeError = ERROR_ACCESS_DENIED;
	easy_multistream::WindowsCredentialVault vault(api);
	const auto result = vault.write("valid-key");
	CHECK(result.error == easy_multistream::CredentialError::AccessDenied);
	CHECK(result.nativeError == ERROR_ACCESS_DENIED);
	CHECK(api.writeCalls == 1);

	api.writeError = ERROR_SUCCESS;
	const auto missingError = vault.write("valid-key");
	CHECK(missingError.error == easy_multistream::CredentialError::OperatingSystemError);
	CHECK(missingError.nativeError == ERROR_GEN_FAILURE);
}

void testCredentialReadAndFree()
{
	FakeWinCredentialApi api;
	api.setReadBlob("stored-key");
	easy_multistream::WindowsCredentialVault vault(api);
	auto result = vault.read();
	CHECK(result.result.succeeded());
	CHECK(result.secret.view() == "stored-key");
	CHECK(api.freeCalls == 1);
	CHECK(api.readFlags == 0);
	CHECK(api.readType == CRED_TYPE_GENERIC);
	CHECK(api.readTarget == easy_multistream::defaultYouTubeCredentialTarget());
	CHECK(std::all_of(api.readBlob.begin(), api.readBlob.end(), [](BYTE value) { return value == 0; }));
	result.secret.clear();
	CHECK(result.secret.empty());
}

void testCredentialStatusAndDeleteMapping()
{
	FakeWinCredentialApi api;
	easy_multistream::WindowsCredentialVault vault(api);

	const auto missing = vault.status();
	CHECK(missing.state == easy_multistream::CredentialState::Missing);
	CHECK(missing.result.error == easy_multistream::CredentialError::NotFound);

	api.readError = ERROR_ACCESS_DENIED;
	const auto denied = vault.status();
	CHECK(denied.state == easy_multistream::CredentialState::Unavailable);
	CHECK(denied.result.error == easy_multistream::CredentialError::AccessDenied);

	api.setReadBlob("present-key");
	const auto present = vault.status();
	CHECK(present.state == easy_multistream::CredentialState::Present);
	CHECK(present.result.succeeded());
	CHECK(std::all_of(api.readBlob.begin(), api.readBlob.end(), [](BYTE value) { return value == 0; }));

	api.eraseSucceeds = false;
	api.eraseError = ERROR_NOT_FOUND;
	CHECK(vault.erase().succeeded());
	CHECK(api.eraseCalls == 1);

	api.eraseError = ERROR_ACCESS_DENIED;
	CHECK(vault.erase().error == easy_multistream::CredentialError::AccessDenied);
	CHECK(api.eraseCalls == 2);
}

void testCredentialCorruptionIsRejected()
{
	FakeWinCredentialApi api;
	api.setReadBlob("x");
	api.credentialBlobSizeOverride = CRED_MAX_CREDENTIAL_BLOB_SIZE + 1;
	easy_multistream::WindowsCredentialVault vault(api);
	const auto result = vault.read();
	CHECK(result.result.error == easy_multistream::CredentialError::CorruptData);
	CHECK(api.freeCalls == 1);
}

void testCredentialInvalidTargetIsRejected()
{
	FakeWinCredentialApi api;
	const std::wstring embeddedNull(L"valid\0suffix", 12);
	easy_multistream::WindowsCredentialVault vault(api, embeddedNull);
	CHECK(vault.write("valid-key").error == easy_multistream::CredentialError::InvalidInput);
	CHECK(vault.read().result.error == easy_multistream::CredentialError::InvalidInput);
	CHECK(vault.erase().error == easy_multistream::CredentialError::InvalidInput);
	CHECK(api.writeCalls == 0);
	CHECK(api.readCalls == 0);
	CHECK(api.eraseCalls == 0);

	easy_multistream::WindowsCredentialVault oversizedTarget(
		api, std::wstring(CRED_MAX_GENERIC_TARGET_NAME_LENGTH + 1, L'a'));
	CHECK(oversizedTarget.write("valid-key").error == easy_multistream::CredentialError::InvalidInput);
}

void testCredentialMalformedReadIsRejectedAndFreed()
{
	FakeWinCredentialApi api;
	api.readSucceeds = true;
	api.readError = ERROR_SUCCESS;
	easy_multistream::WindowsCredentialVault vault(api);

	const auto empty = vault.read();
	CHECK(empty.result.error == easy_multistream::CredentialError::CorruptData);
	CHECK(api.freeCalls == 1);

	api.setReadBlob("wrong-type");
	api.credentialType = CRED_TYPE_DOMAIN_PASSWORD;
	const auto wrongType = vault.read();
	CHECK(wrongType.result.error == easy_multistream::CredentialError::CorruptData);
	CHECK(api.freeCalls == 2);
	CHECK(std::all_of(api.readBlob.begin(), api.readBlob.end(), [](BYTE value) { return value == 0; }));
}

void testSecureBufferBasics()
{
	auto source = easy_multistream::SecureBuffer::copyOf("secret");
	CHECK(source.view() == "secret");

	easy_multistream::SecureBuffer moved(std::move(source));
	CHECK(source.empty());
	CHECK(source.view().empty());
	CHECK(moved.view() == "secret");

	easy_multistream::SecureBuffer assigned;
	assigned = std::move(moved);
	CHECK(moved.empty());
	CHECK(assigned.view() == "secret");
	assigned.clear();
	CHECK(assigned.empty());
	CHECK(assigned.data() == nullptr);
	CHECK(assigned.view().empty());
}

} // namespace

int main()
{
	testSettingsDefaults();
	testOnboardingPreference();
	testSettingsRoundTripAndPlaintextRemoval();
	testSettingsConnectionModesAndMigration();
	testAccountSettingsRoundTripAndSecretExclusion();
	testAccountSettingsSafeSaveAndReload();
	testInvalidSettingsStillScrubPlaintextSecretsFromDisk();
	testPlaintextScrubSaveFailureIsRecoverable();
	testSettingsSafeSaveAndReload();
	testSettingsSaveFailureRollsBackInMemoryValues();
	testFutureSchemaIsDisabled();
	testInvalidSchemaAndUnavailableConfig();
	testStreamKeyValidation();
	testYouTubeServerUrlValidation();
	testSettingsInvalidServerUrlIsRejectedBeforeSave();
	testCredentialWriteContract();
	testAccountCredentialTargetIsIsolated();
	testCredentialInputValidation();
	testCredentialWriteFailureMapping();
	testCredentialReadAndFree();
	testCredentialStatusAndDeleteMapping();
	testCredentialCorruptionIsRejected();
	testCredentialInvalidTargetIsRejected();
	testCredentialMalformedReadIsRejectedAndFreed();
	testSecureBufferBasics();

	if (failures != 0) {
		std::cerr << failures << " test(s) failed\n";
		return 1;
	}

	std::cout << "All unit tests passed\n";
	return 0;
}

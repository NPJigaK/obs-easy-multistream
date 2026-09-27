// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

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
	CHECK(loaded.sourceSchemaVersion == easy_multistream::kSettingsSchemaVersion);
}

void testSettingsRoundTripAndPlaintextRemoval()
{
	Config config("[EasyMultistream]\nStreamKey=must-not-survive\n");
	easy_multistream::Settings settings;
	settings.youtubeEnabled = true;
	easy_multistream::writeProfileSettings(config.get(), settings);

	const auto loaded = easy_multistream::loadProfileSettings(config.get());
	CHECK(loaded.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(loaded.settings.youtubeEnabled);
	CHECK(!config_has_user_value(config.get(), "EasyMultistream", "StreamKey"));
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
		easy_multistream::Settings settings;
		settings.youtubeEnabled = true;
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
		CHECK(std::string(config_get_string(config, "OtherPlugin", "Preserved")) == "yes");
		CHECK(!config_has_user_value(config, "EasyMultistream", "YouTubeStreamKey"));
		config_close(config);
	}

	std::ifstream savedFile(path, std::ios::binary);
	const std::string contents{std::istreambuf_iterator<char>(savedFile), std::istreambuf_iterator<char>()};
	savedFile.close();
	CHECK(contents.find("plaintext-must-not-survive") == std::string::npos);
	CHECK(contents.find("StreamKey") == std::string::npos);
	CHECK(contents.find("Credential") == std::string::npos);
	CHECK(!std::filesystem::exists(path.string() + ".tmp"));
	CHECK(std::filesystem::remove(path, removeError));
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
	Config config("[EasyMultistream]\nSchemaVersion=0\nYouTubeEnabled=true\n");
	const auto invalid = easy_multistream::loadProfileSettings(config.get());
	CHECK(invalid.status == easy_multistream::SettingsLoadStatus::InvalidSchema);
	CHECK(!invalid.settings.youtubeEnabled);
	CHECK(invalid.sourceSchemaVersion == 0);

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
	testSettingsRoundTripAndPlaintextRemoval();
	testSettingsSafeSaveAndReload();
	testFutureSchemaIsDisabled();
	testInvalidSchemaAndUnavailableConfig();
	testStreamKeyValidation();
	testCredentialWriteContract();
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

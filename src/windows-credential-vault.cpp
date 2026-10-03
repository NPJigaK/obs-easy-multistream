// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "windows-credential-vault.hpp"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace easy_multistream {
namespace {

constexpr wchar_t kYouTubeCredentialTarget[] = L"NPJigaK/obs-easy-multistream/v1/youtube-stream-key";
constexpr wchar_t kYouTubeAccountRefreshTokenTargetPrefix[] =
	L"NPJigaK/obs-easy-multistream/v2/youtube-account-refresh-token/profile-";
constexpr wchar_t kYouTubeAccountCredentialUserNamePrefix[] = L"NPJigaK/obs-easy-multistream/youtube-account/channel-";
wchar_t kYouTubeUserName[] = L"YouTube";

constexpr std::size_t kSha256Bytes = 32U;
constexpr std::size_t kSha256HexBytes = kSha256Bytes * 2U;

class BCryptAlgorithmGuard final {
public:
	BCryptAlgorithmGuard() = default;
	BCRYPT_ALG_HANDLE handle = nullptr;

	~BCryptAlgorithmGuard()
	{
		if (handle != nullptr) {
			BCryptCloseAlgorithmProvider(handle, 0);
		}
	}

	BCryptAlgorithmGuard(const BCryptAlgorithmGuard &) = delete;
	BCryptAlgorithmGuard &operator=(const BCryptAlgorithmGuard &) = delete;
};

class BCryptHashGuard final {
public:
	BCryptHashGuard() = default;
	BCRYPT_HASH_HANDLE handle = nullptr;

	~BCryptHashGuard()
	{
		if (handle != nullptr) {
			BCryptDestroyHash(handle);
		}
	}

	BCryptHashGuard(const BCryptHashGuard &) = delete;
	BCryptHashGuard &operator=(const BCryptHashGuard &) = delete;
};

class ZeroedBytesGuard final {
public:
	explicit ZeroedBytesGuard(std::vector<BYTE> &bytes) noexcept : bytes_(bytes) {}

	~ZeroedBytesGuard()
	{
		if (!bytes_.empty()) {
			SecureZeroMemory(bytes_.data(), bytes_.size());
		}
	}

	ZeroedBytesGuard(const ZeroedBytesGuard &) = delete;
	ZeroedBytesGuard &operator=(const ZeroedBytesGuard &) = delete;

private:
	std::vector<BYTE> &bytes_;
};

std::optional<std::array<BYTE, kSha256Bytes>> sha256(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kMaxYouTubeAccountProfilePathBytes ||
	    std::find(value.begin(), value.end(), '\0') != value.end()) {
		return std::nullopt;
	}

	try {
		BCryptAlgorithmGuard algorithm;
		if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
			return std::nullopt;
		}

		ULONG objectLength = 0;
		ULONG propertyLength = 0;
		if (BCryptGetProperty(algorithm.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
				      sizeof(objectLength), &propertyLength, 0) != 0 ||
		    propertyLength != sizeof(objectLength) || objectLength == 0 || objectLength > 1024U) {
			return std::nullopt;
		}

		ULONG hashLength = 0;
		propertyLength = 0;
		if (BCryptGetProperty(algorithm.handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
				      sizeof(hashLength), &propertyLength, 0) != 0 ||
		    propertyLength != sizeof(hashLength) || hashLength != kSha256Bytes) {
			return std::nullopt;
		}

		std::vector<BYTE> hashObject(objectLength);
		ZeroedBytesGuard hashObjectGuard(hashObject);
		std::vector<BYTE> mutableValue(value.begin(), value.end());
		ZeroedBytesGuard mutableValueGuard(mutableValue);

		BCryptHashGuard hash;
		if (BCryptCreateHash(algorithm.handle, &hash.handle, hashObject.data(), objectLength, nullptr, 0, 0) !=
		    0) {
			return std::nullopt;
		}
		if (BCryptHashData(hash.handle, mutableValue.data(), static_cast<ULONG>(mutableValue.size()), 0) != 0) {
			return std::nullopt;
		}

		std::array<BYTE, kSha256Bytes> digest{};
		if (BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
			return std::nullopt;
		}
		return digest;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<std::string> sha256Hex(std::string_view value) noexcept
{
	const auto digest = sha256(value);
	if (!digest.has_value()) {
		return std::nullopt;
	}

	try {
		constexpr char kHex[] = "0123456789abcdef";
		std::string result;
		result.reserve(kSha256HexBytes);
		for (const BYTE byte : *digest) {
			result.push_back(kHex[(byte >> 4U) & 0x0FU]);
			result.push_back(kHex[byte & 0x0FU]);
		}
		return result;
	} catch (...) {
		return std::nullopt;
	}
}

CredentialResult mapCredentialError(DWORD error) noexcept
{
	switch (error) {
	case ERROR_SUCCESS:
		return {CredentialError::OperatingSystemError, ERROR_GEN_FAILURE};
	case ERROR_NOT_FOUND:
		return {CredentialError::NotFound, error};
	case ERROR_ACCESS_DENIED:
		return {CredentialError::AccessDenied, error};
	case ERROR_NO_SUCH_LOGON_SESSION:
	case ERROR_NOT_SUPPORTED:
	case ERROR_CALL_NOT_IMPLEMENTED:
		return {CredentialError::Unavailable, error};
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:
		return {CredentialError::ResourceExhausted, error};
	case ERROR_INVALID_PARAMETER:
	case ERROR_INVALID_FLAGS:
	case ERROR_BAD_USERNAME:
		return {CredentialError::InvalidInput, error};
	default:
		return {CredentialError::OperatingSystemError, error};
	}
}

std::optional<std::wstring> makeTarget(std::string_view profileBinding) noexcept
{
	if (!isValidYouTubeAccountProfileBinding(profileBinding)) {
		return std::nullopt;
	}
	try {
		std::wstring target(kYouTubeAccountRefreshTokenTargetPrefix);
		target.reserve(target.size() + profileBinding.size());
		for (const char byte : profileBinding) {
			target.push_back(static_cast<wchar_t>(static_cast<unsigned char>(byte)));
		}
		if (target.empty() || target.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH ||
		    target.find(L'\0') != std::wstring::npos) {
			return std::nullopt;
		}
		return target;
	} catch (...) {
		return std::nullopt;
	}
}

std::optional<std::wstring> makeUserName(std::string_view channelId) noexcept
{
	if (!isValidYouTubeAccountIdentifier(channelId)) {
		return std::nullopt;
	}
	const auto digest = sha256Hex(channelId);
	if (!digest.has_value()) {
		return std::nullopt;
	}
	try {
		std::wstring userName(kYouTubeAccountCredentialUserNamePrefix);
		userName.reserve(userName.size() + digest->size());
		for (const char byte : *digest) {
			userName.push_back(static_cast<wchar_t>(static_cast<unsigned char>(byte)));
		}
		return userName;
	} catch (...) {
		return std::nullopt;
	}
}

struct DerivedCredentialScope final {
	std::wstring target;
	std::wstring userName;
};

std::optional<DerivedCredentialScope> deriveScope(const YouTubeAccountCredentialScope &scope) noexcept
{
	const auto target = makeTarget(scope.profileBinding);
	const auto userName = makeUserName(scope.channelId);
	if (!target.has_value() || !userName.has_value()) {
		return std::nullopt;
	}
	try {
		return DerivedCredentialScope{*target, *userName};
	} catch (...) {
		return std::nullopt;
	}
}

CredentialResult validateScopedIdentity(const CREDENTIALW *credential, const DerivedCredentialScope &scope) noexcept
{
	if (credential == nullptr) {
		return {CredentialError::CorruptData, ERROR_INVALID_DATA};
	}
	if (credential->TargetName == nullptr || std::wcscmp(credential->TargetName, scope.target.c_str()) != 0 ||
	    credential->UserName == nullptr ||
	    std::wcscmp(credential->UserName, scope.userName.c_str()) != 0) {
		return {CredentialError::ScopeMismatch, ERROR_INVALID_DATA};
	}
	return {};
}

CredentialResult validateScopedCredential(const CREDENTIALW *credential, const DerivedCredentialScope &scope) noexcept
{
	const CredentialResult identity = validateScopedIdentity(credential, scope);
	if (!identity.succeeded()) {
		return identity;
	}
	if (credential->Type != CRED_TYPE_GENERIC) {
		return {CredentialError::CorruptData, ERROR_INVALID_DATA};
	}
	if (credential->CredentialBlob == nullptr || credential->CredentialBlobSize == 0 ||
	    credential->CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
		return {CredentialError::CorruptData, ERROR_INVALID_DATA};
	}
	return {};
}

CredentialStatus statusForValidation(CredentialResult result) noexcept
{
	if (result.succeeded()) {
		return {CredentialState::Present, result};
	}
	if (result.error == CredentialError::NotFound) {
		return {CredentialState::Missing, result};
	}
	if (result.error == CredentialError::ScopeMismatch || result.error == CredentialError::CorruptData) {
		return {CredentialState::NeedsReauthorization, result};
	}
	return {CredentialState::Unavailable, result};
}

class ReadCredentialGuard final {
public:
	ReadCredentialGuard(WinCredentialApi &api, PCREDENTIALW credential) noexcept
		: api_(api),
		  credential_(credential)
	{
	}

	~ReadCredentialGuard()
	{
		if (credential_ == nullptr) {
			return;
		}

		if (credential_->CredentialBlob != nullptr &&
		    credential_->CredentialBlobSize <= CRED_MAX_CREDENTIAL_BLOB_SIZE) {
			SecureZeroMemory(credential_->CredentialBlob, credential_->CredentialBlobSize);
		}
		api_.freeCredential(credential_);
	}

	ReadCredentialGuard(const ReadCredentialGuard &) = delete;
	ReadCredentialGuard &operator=(const ReadCredentialGuard &) = delete;

private:
	WinCredentialApi &api_;
	PCREDENTIALW credential_;
};

} // namespace

static_assert(kMaxCredentialSecretBytes == CRED_MAX_CREDENTIAL_BLOB_SIZE);

bool NativeWinCredentialApi::write(PCREDENTIALW credential, DWORD flags, DWORD &error) noexcept
{
	if (CredWriteW(credential, flags)) {
		error = ERROR_SUCCESS;
		return true;
	}

	error = GetLastError();
	return false;
}

bool NativeWinCredentialApi::read(LPCWSTR targetName, DWORD type, DWORD flags, PCREDENTIALW &credential,
				  DWORD &error) noexcept
{
	if (CredReadW(targetName, type, flags, &credential)) {
		error = ERROR_SUCCESS;
		return true;
	}

	error = GetLastError();
	credential = nullptr;
	return false;
}

bool NativeWinCredentialApi::erase(LPCWSTR targetName, DWORD type, DWORD flags, DWORD &error) noexcept
{
	if (CredDeleteW(targetName, type, flags)) {
		error = ERROR_SUCCESS;
		return true;
	}

	error = GetLastError();
	return false;
}

void NativeWinCredentialApi::freeCredential(PVOID credential) noexcept
{
	CredFree(credential);
}

WindowsCredentialVault::WindowsCredentialVault(WinCredentialApi &api)
	: WindowsCredentialVault(api, defaultYouTubeCredentialTarget())
{
}

WindowsCredentialVault::WindowsCredentialVault(WinCredentialApi &api, std::wstring targetName)
	: api_(api),
	  targetName_(std::move(targetName))
{
}

CredentialResult WindowsCredentialVault::write(std::string_view secret) noexcept
{
	if (!targetIsValid() || secret.empty() || secret.size() > kMaxCredentialSecretBytes ||
	    std::find(secret.begin(), secret.end(), '\0') != secret.end()) {
		return {CredentialError::InvalidInput, ERROR_INVALID_PARAMETER};
	}

	SecureBuffer mutableSecret;
	try {
		mutableSecret = SecureBuffer::copyOf(secret);
	} catch (const std::bad_alloc &) {
		return {CredentialError::ResourceExhausted, ERROR_NOT_ENOUGH_MEMORY};
	} catch (...) {
		return {CredentialError::OperatingSystemError, ERROR_GEN_FAILURE};
	}

	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = targetName_.data();
	credential.CredentialBlobSize = static_cast<DWORD>(mutableSecret.size());
	credential.CredentialBlob = mutableSecret.data();
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = kYouTubeUserName;

	DWORD error = ERROR_SUCCESS;
	if (!api_.write(&credential, 0, error)) {
		return mapError(error);
	}

	return {};
}

CredentialReadResult WindowsCredentialVault::read() noexcept
{
	if (!targetIsValid()) {
		return {{CredentialError::InvalidInput, ERROR_INVALID_PARAMETER}, {}};
	}

	PCREDENTIALW credential = nullptr;
	DWORD error = ERROR_SUCCESS;
	if (!api_.read(targetName_.c_str(), CRED_TYPE_GENERIC, 0, credential, error)) {
		return {mapError(error), {}};
	}

	ReadCredentialGuard guard(api_, credential);
	if (credential == nullptr || credential->Type != CRED_TYPE_GENERIC || credential->CredentialBlobSize == 0 ||
	    credential->CredentialBlobSize > CRED_MAX_CREDENTIAL_BLOB_SIZE || credential->CredentialBlob == nullptr) {
		return {{CredentialError::CorruptData, ERROR_INVALID_DATA}, {}};
	}

	try {
		SecureBuffer secret(credential->CredentialBlobSize);
		std::memcpy(secret.data(), credential->CredentialBlob, credential->CredentialBlobSize);
		return {{}, std::move(secret)};
	} catch (const std::bad_alloc &) {
		return {{CredentialError::ResourceExhausted, ERROR_NOT_ENOUGH_MEMORY}, {}};
	} catch (...) {
		return {{CredentialError::OperatingSystemError, ERROR_GEN_FAILURE}, {}};
	}
}

CredentialResult WindowsCredentialVault::erase() noexcept
{
	if (!targetIsValid()) {
		return {CredentialError::InvalidInput, ERROR_INVALID_PARAMETER};
	}

	DWORD error = ERROR_SUCCESS;
	if (!api_.erase(targetName_.c_str(), CRED_TYPE_GENERIC, 0, error) && error != ERROR_NOT_FOUND) {
		return mapError(error);
	}

	return {};
}

CredentialStatus WindowsCredentialVault::status() noexcept
{
	CredentialReadResult readResult = read();
	if (readResult.result.succeeded()) {
		return {CredentialState::Present, {}};
	}
	if (readResult.result.error == CredentialError::NotFound) {
		return {CredentialState::Missing, readResult.result};
	}
	return {CredentialState::Unavailable, readResult.result};
}

const std::wstring &WindowsCredentialVault::targetName() const noexcept
{
	return targetName_;
}

std::optional<std::string> makeYouTubeAccountProfileBinding(std::string_view profilePath) noexcept
{
	return sha256Hex(profilePath);
}

std::optional<std::wstring> makeYouTubeAccountCredentialTarget(std::string_view profileBinding) noexcept
{
	return makeTarget(profileBinding);
}

std::optional<std::wstring> makeYouTubeAccountCredentialUserName(std::string_view channelId) noexcept
{
	return makeUserName(channelId);
}

WindowsYouTubeAccountRefreshTokenStore::WindowsYouTubeAccountRefreshTokenStore(WinCredentialApi &api) : api_(api) {}

CredentialResult WindowsYouTubeAccountRefreshTokenStore::write(const YouTubeAccountCredentialScope &scope,
							       std::string_view secret) noexcept
{
	auto derived = deriveScope(scope);
	if (!derived.has_value() || secret.empty() || secret.size() > kMaxCredentialSecretBytes ||
	    std::find(secret.begin(), secret.end(), '\0') != secret.end()) {
		return {CredentialError::InvalidInput, ERROR_INVALID_PARAMETER};
	}

	SecureBuffer mutableSecret;
	try {
		mutableSecret = SecureBuffer::copyOf(secret);
	} catch (const std::bad_alloc &) {
		return {CredentialError::ResourceExhausted, ERROR_NOT_ENOUGH_MEMORY};
	} catch (...) {
		return {CredentialError::OperatingSystemError, ERROR_GEN_FAILURE};
	}

	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = derived->target.data();
	credential.CredentialBlobSize = static_cast<DWORD>(mutableSecret.size());
	credential.CredentialBlob = mutableSecret.data();
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = derived->userName.data();

	DWORD error = ERROR_SUCCESS;
	if (!api_.write(&credential, 0, error)) {
		return mapCredentialError(error);
	}
	return {};
}

CredentialReadResult WindowsYouTubeAccountRefreshTokenStore::read(const YouTubeAccountCredentialScope &scope) noexcept
{
	const auto derived = deriveScope(scope);
	if (!derived.has_value()) {
		return {{CredentialError::InvalidInput, ERROR_INVALID_PARAMETER}, {}};
	}

	PCREDENTIALW credential = nullptr;
	DWORD error = ERROR_SUCCESS;
	if (!api_.read(derived->target.c_str(), CRED_TYPE_GENERIC, 0, credential, error)) {
		return {mapCredentialError(error), {}};
	}

	ReadCredentialGuard guard(api_, credential);
	const CredentialResult validation = validateScopedCredential(credential, *derived);
	if (!validation.succeeded()) {
		return {validation, {}};
	}

	try {
		SecureBuffer secret(credential->CredentialBlobSize);
		std::memcpy(secret.data(), credential->CredentialBlob, credential->CredentialBlobSize);
		return {{}, std::move(secret)};
	} catch (const std::bad_alloc &) {
		return {{CredentialError::ResourceExhausted, ERROR_NOT_ENOUGH_MEMORY}, {}};
	} catch (...) {
		return {{CredentialError::OperatingSystemError, ERROR_GEN_FAILURE}, {}};
	}
}

CredentialResult WindowsYouTubeAccountRefreshTokenStore::erase(const YouTubeAccountCredentialScope &scope) noexcept
{
	const auto derived = deriveScope(scope);
	if (!derived.has_value()) {
		return {CredentialError::InvalidInput, ERROR_INVALID_PARAMETER};
	}

	// Inspect the credential before deletion. A target can contain a different
	// channel binding after a profile is restored or a credential is rotated;
	// never delete that credential merely because the target name matches.
	PCREDENTIALW credential = nullptr;
	DWORD error = ERROR_SUCCESS;
	if (!api_.read(derived->target.c_str(), CRED_TYPE_GENERIC, 0, credential, error)) {
		if (error == ERROR_NOT_FOUND) {
			return {};
		}
		return mapCredentialError(error);
	}
	ReadCredentialGuard guard(api_, credential);
	// Deletion needs only the exact target/channel identity. A corrupt blob or
	// type must remain removable so the user can recover without opening the
	// Windows Credential Manager manually.
	const CredentialResult validation = validateScopedIdentity(credential, *derived);
	if (!validation.succeeded()) {
		return validation;
	}

	error = ERROR_SUCCESS;
	if (!api_.erase(derived->target.c_str(), CRED_TYPE_GENERIC, 0, error) && error != ERROR_NOT_FOUND) {
		return mapCredentialError(error);
	}
	return {};
}

CredentialStatus WindowsYouTubeAccountRefreshTokenStore::status(const YouTubeAccountCredentialScope &scope) noexcept
{
	const auto derived = deriveScope(scope);
	if (!derived.has_value()) {
		return {CredentialState::Unavailable, {CredentialError::InvalidInput, ERROR_INVALID_PARAMETER}};
	}

	PCREDENTIALW credential = nullptr;
	DWORD error = ERROR_SUCCESS;
	if (!api_.read(derived->target.c_str(), CRED_TYPE_GENERIC, 0, credential, error)) {
		return statusForValidation(mapCredentialError(error));
	}
	ReadCredentialGuard guard(api_, credential);
	// Validation intentionally inspects metadata only. status() must not copy a
	// refresh token into a second SecureBuffer just to report presence.
	return statusForValidation(validateScopedCredential(credential, *derived));
}

CredentialResult WindowsCredentialVault::mapError(DWORD error) const noexcept
{
	return mapCredentialError(error);
}

bool WindowsCredentialVault::targetIsValid() const noexcept
{
	return !targetName_.empty() && targetName_.size() <= CRED_MAX_GENERIC_TARGET_NAME_LENGTH &&
	       targetName_.find(L'\0') == std::wstring::npos;
}

const wchar_t *defaultYouTubeCredentialTarget() noexcept
{
	return kYouTubeCredentialTarget;
}

} // namespace easy_multistream

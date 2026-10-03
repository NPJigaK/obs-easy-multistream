// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "windows-credential-vault.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace easy_multistream {
namespace {

constexpr wchar_t kYouTubeCredentialTarget[] = L"NPJigaK/obs-easy-multistream/v1/youtube-stream-key";
constexpr wchar_t kYouTubeAccountRefreshTokenTarget[] =
	L"NPJigaK/obs-easy-multistream/v1/youtube-account-refresh-token";
wchar_t kYouTubeUserName[] = L"YouTube";

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

WindowsYouTubeAccountRefreshTokenVault::WindowsYouTubeAccountRefreshTokenVault(WinCredentialApi &api)
	: vault_(api, defaultYouTubeAccountRefreshTokenTarget())
{
}

CredentialResult WindowsYouTubeAccountRefreshTokenVault::write(std::string_view secret) noexcept
{
	return vault_.write(secret);
}

CredentialReadResult WindowsYouTubeAccountRefreshTokenVault::read() noexcept
{
	return vault_.read();
}

CredentialResult WindowsYouTubeAccountRefreshTokenVault::erase() noexcept
{
	return vault_.erase();
}

CredentialStatus WindowsYouTubeAccountRefreshTokenVault::status() noexcept
{
	return vault_.status();
}

const std::wstring &WindowsYouTubeAccountRefreshTokenVault::targetName() const noexcept
{
	return vault_.targetName();
}

CredentialResult WindowsCredentialVault::mapError(DWORD error) const noexcept
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

bool WindowsCredentialVault::targetIsValid() const noexcept
{
	return !targetName_.empty() && targetName_.size() <= CRED_MAX_GENERIC_TARGET_NAME_LENGTH &&
	       targetName_.find(L'\0') == std::wstring::npos;
}

const wchar_t *defaultYouTubeCredentialTarget() noexcept
{
	return kYouTubeCredentialTarget;
}

const wchar_t *defaultYouTubeAccountRefreshTokenTarget() noexcept
{
	return kYouTubeAccountRefreshTokenTarget;
}

} // namespace easy_multistream

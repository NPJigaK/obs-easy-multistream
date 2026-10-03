// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "credential-vault.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincred.h>

#include <string>
#include <string_view>

namespace easy_multistream {

class WinCredentialApi {
public:
	virtual ~WinCredentialApi() = default;

	virtual bool write(PCREDENTIALW credential, DWORD flags, DWORD &error) noexcept = 0;
	virtual bool read(LPCWSTR targetName, DWORD type, DWORD flags, PCREDENTIALW &credential,
			  DWORD &error) noexcept = 0;
	virtual bool erase(LPCWSTR targetName, DWORD type, DWORD flags, DWORD &error) noexcept = 0;
	virtual void freeCredential(PVOID credential) noexcept = 0;
};

class NativeWinCredentialApi final : public WinCredentialApi {
public:
	bool write(PCREDENTIALW credential, DWORD flags, DWORD &error) noexcept override;
	bool read(LPCWSTR targetName, DWORD type, DWORD flags, PCREDENTIALW &credential,
		  DWORD &error) noexcept override;
	bool erase(LPCWSTR targetName, DWORD type, DWORD flags, DWORD &error) noexcept override;
	void freeCredential(PVOID credential) noexcept override;
};

class WindowsCredentialVault final : public CredentialVault {
public:
	explicit WindowsCredentialVault(WinCredentialApi &api);
	WindowsCredentialVault(WinCredentialApi &api, std::wstring targetName);

	CredentialResult write(std::string_view secret) noexcept override;
	CredentialReadResult read() noexcept override;
	CredentialResult erase() noexcept override;
	CredentialStatus status() noexcept override;

	const std::wstring &targetName() const noexcept;

private:
	CredentialResult mapError(DWORD error) const noexcept;
	bool targetIsValid() const noexcept;

	WinCredentialApi &api_;
	std::wstring targetName_;
};

// The target is intentionally fixed. Production account components accept
// this semantic vault type rather than the generic manual stream-key vault.
class WindowsYouTubeAccountRefreshTokenVault final : public YouTubeAccountRefreshTokenVault {
public:
	explicit WindowsYouTubeAccountRefreshTokenVault(WinCredentialApi &api);

	CredentialResult write(std::string_view secret) noexcept override;
	CredentialReadResult read() noexcept override;
	CredentialResult erase() noexcept override;
	CredentialStatus status() noexcept override;

	const std::wstring &targetName() const noexcept;

private:
	WindowsCredentialVault vault_;
};

const wchar_t *defaultYouTubeCredentialTarget() noexcept;
const wchar_t *defaultYouTubeAccountRefreshTokenTarget() noexcept;

} // namespace easy_multistream

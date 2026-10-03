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

#include <cstddef>
#include <optional>
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

inline constexpr std::size_t kMaxYouTubeAccountProfilePathBytes = 32767U;

// The profile path is hashed with the platform crypto provider before it is
// used as a credential-store namespace. The returned value is always exactly
// 64 lower-case hexadecimal characters when present.
std::optional<std::string> makeYouTubeAccountProfileBinding(std::string_view profilePath) noexcept;

// These helpers are public so tests and platform adapters can inspect the
// deterministic namespace without ever handling a raw profile path or token.
std::optional<std::wstring> makeYouTubeAccountCredentialTarget(std::string_view profileBinding) noexcept;
std::optional<std::wstring> makeYouTubeAccountCredentialUserName(std::string_view channelId) noexcept;

// Unlike the manual stream-key vault, the account credential is never stored
// under one process-wide name. Each profile owns one current credential. Its
// target is scoped by profileBinding, and read/status/erase verify the selected
// channel digest carried in UserName before acting on the record.
class WindowsYouTubeAccountRefreshTokenStore final : public YouTubeAccountRefreshTokenStore {
public:
	explicit WindowsYouTubeAccountRefreshTokenStore(WinCredentialApi &api);

	CredentialResult write(const YouTubeAccountCredentialScope &scope, std::string_view secret) noexcept override;
	CredentialReadResult read(const YouTubeAccountCredentialScope &scope) noexcept override;
	CredentialResult erase(const YouTubeAccountCredentialScope &scope) noexcept override;
	CredentialStatus status(const YouTubeAccountCredentialScope &scope) noexcept override;

private:
	WinCredentialApi &api_;
};

const wchar_t *defaultYouTubeCredentialTarget() noexcept;

} // namespace easy_multistream

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace easy_multistream {

// The mutex name is derived only from the canonical SHA-256 binding of an
// OBS profile path. A raw path, channel identifier, token, or other user data
// must never be placed in the global object namespace.
std::optional<std::wstring> makeYouTubeAccountProfileOperationLockName(std::string_view profileBinding) noexcept;

enum class YouTubeAccountProfileOperationLockStatus {
	Acquired,
	Recovered,
	Busy,
	InvalidProfileBinding,
	Unavailable,
};

struct YouTubeAccountProfileOperationLockResult final {
	YouTubeAccountProfileOperationLockStatus status = YouTubeAccountProfileOperationLockStatus::Unavailable;
	DWORD nativeError = ERROR_SUCCESS;

	bool acquired() const noexcept
	{
		return status == YouTubeAccountProfileOperationLockStatus::Acquired ||
		       status == YouTubeAccountProfileOperationLockStatus::Recovered;
	}

	bool recovered() const noexcept { return status == YouTubeAccountProfileOperationLockStatus::Recovered; }
};

// The API boundary keeps Win32 calls injectable for deterministic tests. An
// implementation and all lock operations must be used on one owner thread.
// The API instance must also outlive every lock acquired through it.
class YouTubeAccountProfileOperationLockApi {
public:
	virtual ~YouTubeAccountProfileOperationLockApi() = default;

	virtual HANDLE createMutex(LPCWSTR name, DWORD &error) noexcept = 0;
	virtual DWORD wait(HANDLE handle, DWORD timeoutMilliseconds, DWORD &error) noexcept = 0;
	virtual bool releaseMutex(HANDLE handle, DWORD &error) noexcept = 0;
	virtual bool closeHandle(HANDLE handle, DWORD &error) noexcept = 0;
};

class NativeYouTubeAccountProfileOperationLockApi final : public YouTubeAccountProfileOperationLockApi {
public:
	HANDLE createMutex(LPCWSTR name, DWORD &error) noexcept override;
	DWORD wait(HANDLE handle, DWORD timeoutMilliseconds, DWORD &error) noexcept override;
	bool releaseMutex(HANDLE handle, DWORD &error) noexcept override;
	bool closeHandle(HANDLE handle, DWORD &error) noexcept override;
};

class YouTubeAccountProfileOperationLock final {
public:
	YouTubeAccountProfileOperationLock() noexcept = default;
	~YouTubeAccountProfileOperationLock();

	YouTubeAccountProfileOperationLock(const YouTubeAccountProfileOperationLock &) = delete;
	YouTubeAccountProfileOperationLock &operator=(const YouTubeAccountProfileOperationLock &) = delete;
	YouTubeAccountProfileOperationLock(YouTubeAccountProfileOperationLock &&) = delete;
	YouTubeAccountProfileOperationLock &operator=(YouTubeAccountProfileOperationLock &&) = delete;

	// Releases the held mutex and closes the native handle. This is idempotent.
	// It fails closed without changing ownership when called from another
	// thread or when ReleaseMutex fails.
	bool release() noexcept;

	// acquired() reports native mutex ownership. cleanupPending() also remains
	// true after ownership was released if closing the handle failed and must be
	// retried on the owner thread.
	bool acquired() const noexcept { return handle_ != nullptr && mutexOwned_; }
	bool cleanupPending() const noexcept { return handle_ != nullptr; }
	bool recovered() const noexcept { return recovered_; }

	// Verifies that this lock is currently held by the calling owner thread
	// for exactly the mutex derived from profileBinding. This is intentionally
	// stricter than acquired(): borrowed-lock users must not accidentally use a
	// lock for another OBS profile or from a different thread.
	bool acquiredFor(std::string_view profileBinding) const noexcept;

private:
	friend class YouTubeAccountProfileOperationLockProvider;

	YouTubeAccountProfileOperationLockApi *api_ = nullptr;
	HANDLE handle_ = nullptr;
	std::wstring name_;
	DWORD ownerThreadId_ = 0;
	bool claimed_ = false;
	bool mutexOwned_ = false;
	bool recovered_ = false;
};

class YouTubeAccountProfileOperationLockProvider final {
public:
	explicit YouTubeAccountProfileOperationLockProvider(YouTubeAccountProfileOperationLockApi &api) noexcept
		: api_(api)
	{
	}

	YouTubeAccountProfileOperationLockProvider(const YouTubeAccountProfileOperationLockProvider &) = delete;
	YouTubeAccountProfileOperationLockProvider &
	operator=(const YouTubeAccountProfileOperationLockProvider &) = delete;

	YouTubeAccountProfileOperationLockResult acquire(std::string_view profileBinding,
							 YouTubeAccountProfileOperationLock &lock) noexcept;

private:
	YouTubeAccountProfileOperationLockApi &api_;
};

} // namespace easy_multistream

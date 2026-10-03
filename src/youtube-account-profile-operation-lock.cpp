// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-profile-operation-lock.hpp"

#include "credential-vault.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace easy_multistream {
namespace {

constexpr wchar_t kLockNamePrefix[] = L"Local\\NPJigaK.obs-easy-multistream.youtube-account.profile-";
constexpr DWORD kWaitMilliseconds = 0;

struct ProcessClaimRegistry final {
	std::mutex mutex;
	std::unordered_set<std::wstring> names;
};

struct ProcessClaimRegistryStorage final {
	std::once_flag initialization;
	ProcessClaimRegistry *registry = nullptr;
};

ProcessClaimRegistryStorage &processClaimRegistryStorage() noexcept
{
	// The storage itself is trivially destructible. The registry intentionally
	// lives in a heap allocation for the process lifetime, avoiding static
	// destruction ordering while the plugin DLL is being unloaded.
	static ProcessClaimRegistryStorage storage;
	return storage;
}

ProcessClaimRegistry *processClaimRegistry() noexcept
{
	ProcessClaimRegistryStorage &storage = processClaimRegistryStorage();
	try {
		std::call_once(storage.initialization, [&storage]() noexcept {
			try {
				storage.registry = new ProcessClaimRegistry();
			} catch (...) {
				storage.registry = nullptr;
			}
		});
	} catch (...) {
		return nullptr;
	}
	return storage.registry;
}

enum class ClaimResult {
	Claimed,
	AlreadyClaimed,
	Unavailable,
};

ClaimResult claimName(const std::wstring &name) noexcept
{
	ProcessClaimRegistry *registry = processClaimRegistry();
	if (registry == nullptr) {
		return ClaimResult::Unavailable;
	}

	try {
		std::lock_guard<std::mutex> guard(registry->mutex);
		const auto [iterator, inserted] = registry->names.insert(name);
		(void)iterator;
		return inserted ? ClaimResult::Claimed : ClaimResult::AlreadyClaimed;
	} catch (...) {
		return ClaimResult::Unavailable;
	}
}

void releaseName(const std::wstring &name) noexcept
{
	ProcessClaimRegistry *registry = processClaimRegistry();
	if (registry == nullptr) {
		return;
	}

	try {
		std::lock_guard<std::mutex> guard(registry->mutex);
		registry->names.erase(name);
	} catch (...) {
		// The registry is only an in-process duplicate guard. The native mutex
		// has already been released/closed; no exception can escape teardown.
	}
}

} // namespace

std::optional<std::wstring> makeYouTubeAccountProfileOperationLockName(std::string_view profileBinding) noexcept
{
	if (!isValidYouTubeAccountProfileBinding(profileBinding)) {
		return std::nullopt;
	}

	try {
		std::wstring name(kLockNamePrefix);
		name.reserve(name.size() + profileBinding.size());
		for (const char byte : profileBinding) {
			name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(byte)));
		}
		if (name.find(L'\0') != std::wstring::npos) {
			return std::nullopt;
		}
		return name;
	} catch (...) {
		return std::nullopt;
	}
}

HANDLE NativeYouTubeAccountProfileOperationLockApi::createMutex(LPCWSTR name, DWORD &error) noexcept
{
	HANDLE handle = CreateMutexW(nullptr, FALSE, name);
	if (handle == nullptr) {
		error = GetLastError();
		if (error == ERROR_SUCCESS) {
			error = ERROR_GEN_FAILURE;
		}
		return nullptr;
	}

	error = ERROR_SUCCESS;
	return handle;
}

DWORD NativeYouTubeAccountProfileOperationLockApi::wait(HANDLE handle, DWORD timeoutMilliseconds, DWORD &error) noexcept
{
	const DWORD result = WaitForSingleObject(handle, timeoutMilliseconds);
	error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
	if (result == WAIT_FAILED && error == ERROR_SUCCESS) {
		error = ERROR_GEN_FAILURE;
	}
	return result;
}

bool NativeYouTubeAccountProfileOperationLockApi::releaseMutex(HANDLE handle, DWORD &error) noexcept
{
	if (ReleaseMutex(handle)) {
		error = ERROR_SUCCESS;
		return true;
	}

	error = GetLastError();
	if (error == ERROR_SUCCESS) {
		error = ERROR_GEN_FAILURE;
	}
	return false;
}

bool NativeYouTubeAccountProfileOperationLockApi::closeHandle(HANDLE handle, DWORD &error) noexcept
{
	if (CloseHandle(handle)) {
		error = ERROR_SUCCESS;
		return true;
	}

	error = GetLastError();
	if (error == ERROR_SUCCESS) {
		error = ERROR_GEN_FAILURE;
	}
	return false;
}

YouTubeAccountProfileOperationLock::~YouTubeAccountProfileOperationLock()
{
	(void)release();
}

bool YouTubeAccountProfileOperationLock::release() noexcept
{
	if (handle_ == nullptr) {
		return true;
	}
	if (api_ == nullptr || ownerThreadId_ == 0 || ownerThreadId_ != GetCurrentThreadId()) {
		return false;
	}

	DWORD releaseError = ERROR_SUCCESS;
	if (!api_->releaseMutex(handle_, releaseError)) {
		// Ownership is now uncertain. Retain both the native handle and the
		// process claim so another local operation cannot enter incorrectly.
		return false;
	}

	DWORD closeError = ERROR_SUCCESS;
	const bool closed = api_->closeHandle(handle_, closeError);

	if (claimed_) {
		releaseName(name_);
	}

	api_ = nullptr;
	handle_ = nullptr;
	name_.clear();
	ownerThreadId_ = 0;
	claimed_ = false;
	recovered_ = false;
	return closed;
}

YouTubeAccountProfileOperationLockResult
YouTubeAccountProfileOperationLockProvider::acquire(std::string_view profileBinding,
						    YouTubeAccountProfileOperationLock &lock) noexcept
{
	// Never replace a currently held lock. This keeps a failed/rejected
	// acquisition from accidentally releasing an active operation.
	if (lock.acquired()) {
		return {YouTubeAccountProfileOperationLockStatus::Busy, ERROR_BUSY};
	}

	auto name = makeYouTubeAccountProfileOperationLockName(profileBinding);
	if (!name.has_value()) {
		return {YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding, ERROR_INVALID_PARAMETER};
	}

	const ClaimResult claim = claimName(*name);
	if (claim == ClaimResult::AlreadyClaimed) {
		return {YouTubeAccountProfileOperationLockStatus::Busy, ERROR_BUSY};
	}
	if (claim == ClaimResult::Unavailable) {
		return {YouTubeAccountProfileOperationLockStatus::Unavailable, ERROR_NOT_ENOUGH_MEMORY};
	}

	DWORD error = ERROR_SUCCESS;
	HANDLE handle = api_.createMutex(name->c_str(), error);
	if (handle == nullptr) {
		releaseName(*name);
		return {YouTubeAccountProfileOperationLockStatus::Unavailable,
			error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error};
	}

	const DWORD waitResult = api_.wait(handle, kWaitMilliseconds, error);
	if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
		// Every remaining ownership transfer is allocation-free and noexcept.
		// This matters because the native mutex is already owned here.
		lock.name_.swap(*name);
		lock.api_ = &api_;
		lock.handle_ = handle;
		lock.ownerThreadId_ = GetCurrentThreadId();
		lock.claimed_ = true;
		lock.recovered_ = waitResult == WAIT_ABANDONED;
		return {waitResult == WAIT_ABANDONED ? YouTubeAccountProfileOperationLockStatus::Recovered
						     : YouTubeAccountProfileOperationLockStatus::Acquired,
			error};
	}

	DWORD closeError = ERROR_SUCCESS;
	(void)api_.closeHandle(handle, closeError);
	releaseName(*name);
	if (waitResult == WAIT_TIMEOUT) {
		return {YouTubeAccountProfileOperationLockStatus::Busy, WAIT_TIMEOUT};
	}
	return {YouTubeAccountProfileOperationLockStatus::Unavailable,
		error == ERROR_SUCCESS ? (waitResult == WAIT_FAILED ? ERROR_GEN_FAILURE : waitResult) : error};
}

} // namespace easy_multistream

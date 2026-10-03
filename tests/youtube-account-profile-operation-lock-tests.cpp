// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-profile-operation-lock.hpp"

#include "credential-vault.hpp"

#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using easy_multistream::YouTubeAccountProfileOperationLock;
using easy_multistream::YouTubeAccountProfileOperationLockApi;
using easy_multistream::YouTubeAccountProfileOperationLockProvider;
using easy_multistream::YouTubeAccountProfileOperationLockStatus;
using easy_multistream::NativeYouTubeAccountProfileOperationLockApi;

constexpr std::string_view kBindingA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kBindingB = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

static_assert(!std::is_copy_constructible_v<YouTubeAccountProfileOperationLock>);
static_assert(!std::is_move_constructible_v<YouTubeAccountProfileOperationLock>);

class FakeLockApi final : public YouTubeAccountProfileOperationLockApi {
public:
	HANDLE createMutex(LPCWSTR name, DWORD &error) noexcept override
	{
		++createCount;
		lastName = name == nullptr ? L"" : name;
		if (createResult == nullptr) {
			error = createError;
			return nullptr;
		}
		error = ERROR_SUCCESS;
		const auto value = static_cast<std::uintptr_t>(0x1000U + createCount);
		lastHandle = reinterpret_cast<HANDLE>(value);
		return lastHandle;
	}

	DWORD wait(HANDLE handle, DWORD timeoutMilliseconds, DWORD &error) noexcept override
	{
		(void)handle;
		CHECK(timeoutMilliseconds == 0);
		++waitCount;
		error = waitError;
		return waitResult;
	}

	bool releaseMutex(HANDLE handle, DWORD &error) noexcept override
	{
		CHECK(handle != nullptr);
		++releaseCount;
		error = releaseError;
		return releaseResult;
	}

	bool closeHandle(HANDLE handle, DWORD &error) noexcept override
	{
		CHECK(handle != nullptr);
		++closeCount;
		error = closeError;
		return closeResult;
	}

	HANDLE createResult = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1));
	DWORD createError = ERROR_ACCESS_DENIED;
	DWORD waitResult = WAIT_OBJECT_0;
	DWORD waitError = ERROR_SUCCESS;
	bool releaseResult = true;
	DWORD releaseError = ERROR_SUCCESS;
	bool closeResult = true;
	DWORD closeError = ERROR_SUCCESS;
	int createCount = 0;
	int waitCount = 0;
	int releaseCount = 0;
	int closeCount = 0;
	HANDLE lastHandle = nullptr;
	std::wstring lastName;
};

class ScopedWin32Handle final {
public:
	ScopedWin32Handle() noexcept = default;
	explicit ScopedWin32Handle(HANDLE handle) noexcept : handle_(handle) {}

	~ScopedWin32Handle()
	{
		if (handle_ != nullptr) {
			(void)CloseHandle(handle_);
		}
	}

	ScopedWin32Handle(const ScopedWin32Handle &) = delete;
	ScopedWin32Handle &operator=(const ScopedWin32Handle &) = delete;

	ScopedWin32Handle(ScopedWin32Handle &&other) noexcept : handle_(other.release()) {}

	ScopedWin32Handle &operator=(ScopedWin32Handle &&other) noexcept
	{
		if (this != &other) {
			if (handle_ != nullptr) {
				(void)CloseHandle(handle_);
			}
			handle_ = other.release();
		}
		return *this;
	}

	HANDLE get() const noexcept { return handle_; }
	bool valid() const noexcept { return handle_ != nullptr; }
	HANDLE release() noexcept
	{
		HANDLE result = handle_;
		handle_ = nullptr;
		return result;
	}

private:
	HANDLE handle_ = nullptr;
};

std::string makeUniqueValidBinding()
{
	std::string binding(kBindingA);
	const std::uint64_t value = (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^ GetTickCount64() ^
				    static_cast<std::uint64_t>(GetCurrentThreadId());
	constexpr char kHex[] = "0123456789abcdef";
	for (std::size_t index = 0; index < 16U; ++index) {
		const auto shift = static_cast<unsigned int>((15U - index) * 4U);
		binding[48U + index] = kHex[(value >> shift) & 0x0FU];
	}
	return binding;
}

std::wstring makeUniqueLocalEventName()
{
	return L"Local\\NPJigaK.obs-easy-multistream.test-ready-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
	       std::to_wstring(GetTickCount64());
}

std::wstring currentExecutablePath()
{
	std::vector<wchar_t> buffer(512U);
	for (;;) {
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0) {
			return {};
		}
		if (length + 1U < buffer.size()) {
			return std::wstring(buffer.data(), length);
		}
		if (buffer.size() >= 32768U) {
			return {};
		}
		buffer.resize(buffer.size() * 2U);
	}
}

bool terminateChild(ScopedWin32Handle &process)
{
	if (!process.valid()) {
		return true;
	}
	const DWORD state = WaitForSingleObject(process.get(), 0);
	if (state == WAIT_TIMEOUT) {
		if (!TerminateProcess(process.get(), 0xE501U)) {
			// The process can exit between the zero-time probe and the
			// termination request. Recheck before reporting cleanup failure.
			return WaitForSingleObject(process.get(), 1000U) == WAIT_OBJECT_0;
		}
	}
	return WaitForSingleObject(process.get(), 10000U) == WAIT_OBJECT_0;
}

int runCrossProcessLockChild(int argc, wchar_t **argv)
{
	if (argc != 4 || std::wcscmp(argv[1], L"--youtube-account-profile-lock-child") != 0) {
		return 2;
	}

	const std::wstring bindingWide(argv[2]);
	const std::wstring eventName(argv[3]);
	if (bindingWide.size() != kBindingA.size()) {
		return 3;
	}
	std::string binding;
	binding.reserve(bindingWide.size());
	for (const wchar_t character : bindingWide) {
		if (character < L'\0' || character > 0x7FU) {
			return 4;
		}
		binding.push_back(static_cast<char>(character));
	}
	if (!easy_multistream::isValidYouTubeAccountProfileBinding(binding)) {
		return 4;
	}

	ScopedWin32Handle readyEvent(OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str()));
	if (!readyEvent.valid()) {
		return 5;
	}

	NativeYouTubeAccountProfileOperationLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	const auto result = provider.acquire(binding, lock);
	if (!result.acquired() || !lock.acquired()) {
		return 6;
	}
	if (!SetEvent(readyEvent.get())) {
		return 7;
	}

	// The parent intentionally terminates this child to test an abandoned
	// mutex. The process-owned lock handle is closed by the OS on termination.
	Sleep(INFINITE);
	return 8;
}

void testRealCrossProcessContentionAndAbandonment()
{
	const std::string binding = makeUniqueValidBinding();
	const auto lockName = easy_multistream::makeYouTubeAccountProfileOperationLockName(binding);
	CHECK(lockName.has_value());
	if (!lockName.has_value()) {
		return;
	}

	const std::wstring eventName = makeUniqueLocalEventName();
	SetLastError(ERROR_SUCCESS);
	ScopedWin32Handle readyEvent(CreateEventW(nullptr, TRUE, FALSE, eventName.c_str()));
	CHECK(readyEvent.valid());
	if (!readyEvent.valid()) {
		return;
	}
	CHECK(GetLastError() != ERROR_ALREADY_EXISTS);

	const std::wstring executable = currentExecutablePath();
	CHECK(!executable.empty());
	if (executable.empty()) {
		return;
	}

	std::wstring command = L"\"" + executable + L"\" --youtube-account-profile-lock-child " +
			       std::wstring(binding.begin(), binding.end()) + L" \"" + eventName + L"\"";
	std::vector<wchar_t> commandLine(command.begin(), command.end());
	commandLine.push_back(L'\0');

	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION processInfo{};
	const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
					    nullptr, nullptr, &startup, &processInfo);
	CHECK(created != FALSE);
	if (!created) {
		return;
	}
	ScopedWin32Handle process(processInfo.hProcess);
	ScopedWin32Handle thread(processInfo.hThread);

	const DWORD readyWait = WaitForSingleObject(readyEvent.get(), 10000U);
	CHECK(readyWait == WAIT_OBJECT_0);
	if (readyWait != WAIT_OBJECT_0) {
		CHECK(terminateChild(process));
		return;
	}

	NativeYouTubeAccountProfileOperationLockApi nativeApi;
	DWORD error = ERROR_SUCCESS;
	ScopedWin32Handle observer(nativeApi.createMutex(lockName->c_str(), error));
	CHECK(observer.valid());
	if (!observer.valid()) {
		CHECK(terminateChild(process));
		return;
	}

	const DWORD observerWait = nativeApi.wait(observer.get(), 0, error);
	CHECK(observerWait == WAIT_TIMEOUT);

	YouTubeAccountProfileOperationLockProvider parentProvider(nativeApi);
	YouTubeAccountProfileOperationLock parentLock;
	const auto busy = parentProvider.acquire(binding, parentLock);
	CHECK(busy.status == YouTubeAccountProfileOperationLockStatus::Busy);
	CHECK(!parentLock.acquired());
	CHECK(WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT);

	const bool terminated = terminateChild(process);
	CHECK(terminated);
	if (!terminated) {
		return;
	}

	const DWORD abandoned = nativeApi.wait(observer.get(), 0, error);
	CHECK(abandoned == WAIT_ABANDONED);
	if (abandoned == WAIT_ABANDONED) {
		CHECK(nativeApi.releaseMutex(observer.get(), error));
	}
	const HANDLE observerRaw = observer.release();
	CHECK(CloseHandle(observerRaw) != FALSE);

	const auto reacquired = parentProvider.acquire(binding, parentLock);
	CHECK(reacquired.acquired());
	CHECK(parentLock.acquired());
	parentLock.release();
}

void testNameValidationAndDerivation()
{
	const auto name = easy_multistream::makeYouTubeAccountProfileOperationLockName(kBindingA);
	CHECK(name.has_value());
	if (name.has_value()) {
		CHECK(*name == L"Local\\NPJigaK.obs-easy-multistream.youtube-account.profile-" +
				       std::wstring(kBindingA.begin(), kBindingA.end()));
	}
	CHECK(!easy_multistream::makeYouTubeAccountProfileOperationLockName("").has_value());
	CHECK(!easy_multistream::makeYouTubeAccountProfileOperationLockName(std::string(63, 'a')).has_value());
	CHECK(!easy_multistream::makeYouTubeAccountProfileOperationLockName(std::string(65, 'a')).has_value());
	CHECK(!easy_multistream::makeYouTubeAccountProfileOperationLockName(std::string(kBindingA).replace(0, 1, "A"))
		       .has_value());
	CHECK(!easy_multistream::makeYouTubeAccountProfileOperationLockName(std::string(kBindingA).replace(0, 1, "-"))
		       .has_value());
}

void testInvalidBindingDoesNotTouchApi()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	const auto result = provider.acquire("not-a-binding", lock);
	CHECK(result.status == YouTubeAccountProfileOperationLockStatus::InvalidProfileBinding);
	CHECK(!lock.acquired());
	CHECK(api.createCount == 0);
}

void testAcquiredAndReleaseReacquire()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	const auto result = provider.acquire(kBindingA, lock);
	CHECK(result.status == YouTubeAccountProfileOperationLockStatus::Acquired);
	CHECK(result.acquired());
	CHECK(!result.recovered());
	CHECK(lock.acquired());
	CHECK(!lock.recovered());
	CHECK(api.lastName == *easy_multistream::makeYouTubeAccountProfileOperationLockName(kBindingA));
	lock.release();
	CHECK(!lock.acquired());
	CHECK(api.releaseCount == 1);
	CHECK(api.closeCount == 1);
	const auto second = provider.acquire(kBindingA, lock);
	CHECK(second.status == YouTubeAccountProfileOperationLockStatus::Acquired);
	CHECK(api.createCount == 2);
	lock.release();
	CHECK(api.releaseCount == 2);
}

void testSameProcessDuplicateAcrossFactoriesIsBusy()
{
	FakeLockApi firstApi;
	FakeLockApi secondApi;
	YouTubeAccountProfileOperationLockProvider firstProvider(firstApi);
	YouTubeAccountProfileOperationLockProvider secondProvider(secondApi);
	YouTubeAccountProfileOperationLock first;
	YouTubeAccountProfileOperationLock second;
	CHECK(firstProvider.acquire(kBindingA, first).acquired());
	const auto duplicate = secondProvider.acquire(kBindingA, second);
	CHECK(duplicate.status == YouTubeAccountProfileOperationLockStatus::Busy);
	CHECK(secondApi.createCount == 0);
	first.release();
	CHECK(secondProvider.acquire(kBindingA, second).acquired());
	CHECK(secondApi.createCount == 1);
	second.release();
}

void testDifferentBindingsCanBeHeldTogether()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock first;
	YouTubeAccountProfileOperationLock second;
	CHECK(provider.acquire(kBindingA, first).acquired());
	CHECK(provider.acquire(kBindingB, second).acquired());
	CHECK(api.createCount == 2);
	CHECK(first.acquired());
	CHECK(second.acquired());
	second.release();
	first.release();
}

void testWaitStatusMappingsAndRollback()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;

	api.waitResult = WAIT_TIMEOUT;
	const auto busy = provider.acquire(kBindingA, lock);
	CHECK(busy.status == YouTubeAccountProfileOperationLockStatus::Busy);
	CHECK(busy.nativeError == WAIT_TIMEOUT);
	CHECK(!lock.acquired());
	CHECK(api.closeCount == 1);
	CHECK(provider.acquire(kBindingA, lock).status == YouTubeAccountProfileOperationLockStatus::Busy);
	// The fake still reports timeout; the second attempt proves the process
	// registry reservation was rolled back (it reached the fake API again).
	CHECK(api.createCount == 2);

	api.waitResult = WAIT_FAILED;
	api.waitError = ERROR_ACCESS_DENIED;
	const auto failed = provider.acquire(kBindingA, lock);
	CHECK(failed.status == YouTubeAccountProfileOperationLockStatus::Unavailable);
	CHECK(failed.nativeError == ERROR_ACCESS_DENIED);
	CHECK(api.closeCount == 3);

	api.waitResult = WAIT_ABANDONED;
	api.waitError = ERROR_SUCCESS;
	const auto recovered = provider.acquire(kBindingA, lock);
	CHECK(recovered.status == YouTubeAccountProfileOperationLockStatus::Recovered);
	CHECK(recovered.recovered());
	CHECK(lock.acquired());
	CHECK(lock.recovered());
	lock.release();

	api.createResult = nullptr;
	api.createError = ERROR_ACCESS_DENIED;
	const auto createFailed = provider.acquire(kBindingA, lock);
	CHECK(createFailed.status == YouTubeAccountProfileOperationLockStatus::Unavailable);
	CHECK(createFailed.nativeError == ERROR_ACCESS_DENIED);
	api.createResult = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1));
	api.waitResult = WAIT_OBJECT_0;
	CHECK(provider.acquire(kBindingA, lock).acquired());
	lock.release();
}

void testHeldLockRejectsReplacement()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	CHECK(provider.acquire(kBindingA, lock).acquired());
	const auto result = provider.acquire(kBindingB, lock);
	CHECK(result.status == YouTubeAccountProfileOperationLockStatus::Busy);
	CHECK(api.createCount == 1);
	lock.release();
}

void testWrongThreadReleaseFailsClosed()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	CHECK(provider.acquire(kBindingA, lock).acquired());
	bool releasedOnWorker = true;
	std::thread worker([&]() { releasedOnWorker = lock.release(); });
	worker.join();
	CHECK(!releasedOnWorker);
	CHECK(lock.acquired());
	CHECK(api.releaseCount == 0);
	CHECK(api.closeCount == 0);

	YouTubeAccountProfileOperationLock duplicate;
	CHECK(provider.acquire(kBindingA, duplicate).status == YouTubeAccountProfileOperationLockStatus::Busy);
	CHECK(api.createCount == 1);
	CHECK(lock.release());
	CHECK(api.releaseCount == 1);
	CHECK(api.closeCount == 1);
}

void testReleaseFailureRetainsClaimForRetry()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	CHECK(provider.acquire(kBindingA, lock).acquired());
	api.releaseResult = false;
	api.releaseError = ERROR_ACCESS_DENIED;
	CHECK(!lock.release());
	CHECK(api.releaseCount == 1);
	CHECK(api.closeCount == 0);
	CHECK(lock.acquired());
	YouTubeAccountProfileOperationLock duplicate;
	CHECK(provider.acquire(kBindingA, duplicate).status == YouTubeAccountProfileOperationLockStatus::Busy);
	CHECK(api.createCount == 1);
	api.releaseResult = true;
	CHECK(lock.release());
	CHECK(api.releaseCount == 2);
	CHECK(api.closeCount == 1);
	CHECK(provider.acquire(kBindingA, lock).acquired());
	CHECK(lock.release());
	CHECK(api.releaseCount == 3);
	CHECK(api.closeCount == 2);
}

void testCloseFailureDoesNotRetainClaim()
{
	FakeLockApi api;
	YouTubeAccountProfileOperationLockProvider provider(api);
	YouTubeAccountProfileOperationLock lock;
	CHECK(provider.acquire(kBindingA, lock).acquired());
	api.closeResult = false;
	api.closeError = ERROR_INVALID_HANDLE;
	CHECK(!lock.release());
	CHECK(!lock.acquired());
	CHECK(api.releaseCount == 1);
	CHECK(api.closeCount == 1);
	api.closeResult = true;
	CHECK(provider.acquire(kBindingA, lock).acquired());
	CHECK(lock.release());
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
	if (argc > 1 && std::wcscmp(argv[1], L"--youtube-account-profile-lock-child") == 0) {
		return runCrossProcessLockChild(argc, argv);
	}

	testNameValidationAndDerivation();
	testInvalidBindingDoesNotTouchApi();
	testAcquiredAndReleaseReacquire();
	testSameProcessDuplicateAcrossFactoriesIsBusy();
	testDifferentBindingsCanBeHeldTogether();
	testWaitStatusMappingsAndRollback();
	testHeldLockRejectsReplacement();
	testWrongThreadReleaseFailsClosed();
	testReleaseFailureRetainsClaimForRetry();
	testCloseFailureDoesNotRetainClaim();
	testRealCrossProcessContentionAndAbandonment();

	if (failures != 0) {
		std::cerr << failures << " test(s) failed\n";
		return 1;
	}
	std::cout << "All YouTube account profile operation lock tests passed\n";
	return 0;
}

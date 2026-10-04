// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-runtime-destination-provider.hpp"

#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace easy_multistream {
namespace {

int failures = 0;

#define CHECK(condition)                                                                            \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			++failures;                                                                            \
			std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " #condition << '\n'; \
		}                                                                                          \
	} while (false)

using OwnerCompletion = YouTubeDestinationPrepareCompletion;
using OwnerAttempt = YouTubeDestinationPrepareAttempt;

OwnerCompletion success(OwnerAttempt attempt, std::string serverUrl = "rtmps://a.rtmps.youtube.com/live2",
					std::string streamKey = "owner-stream-key")
{
	OwnerCompletion completion;
	completion.attempt = attempt;
	completion.status = YouTubeDestinationPrepareStatus::Success;
	YouTubeResolvedIngestion ingestion;
	ingestion.serverUrl = std::move(serverUrl);
	ingestion.streamKey = SecureBuffer::copyOf(streamKey);
	completion.ingestion.emplace(std::move(ingestion));
	return completion;
}

OwnerCompletion status(OwnerAttempt attempt, YouTubeDestinationPrepareStatus value)
{
	OwnerCompletion completion;
	completion.attempt = attempt;
	completion.status = value;
	return completion;
}

class FakeOwner final : public IYouTubeAccountDestinationRuntime {
public:
	YouTubeAccountDestinationStartResult nextStart;
	CompletionHandler completion;
	std::vector<OwnerAttempt> cancellations;
	std::vector<OwnerAttempt> releases;
	bool completeSynchronously = false;
	std::optional<OwnerCompletion> synchronousCompletion;
	std::function<void()> onStartBeforeReturn;

	YouTubeAccountDestinationStartResult
	startDestinationPreparationWithAttempt(CompletionHandler handler) noexcept override
	{
		completion = std::move(handler);
		++startCount;
		if (completeSynchronously && completion && synchronousCompletion.has_value()) {
			completion(std::move(*synchronousCompletion));
			synchronousCompletion.reset();
		}
		if (onStartBeforeReturn) {
			onStartBeforeReturn();
		}
		return nextStart;
	}

	YouTubeAccountDestinationOperationStatus
	cancelDestinationPreparation(OwnerAttempt attempt) noexcept override
	{
		cancellations.push_back(attempt);
		return cancelStatus;
	}

	YouTubeAccountDestinationUseReleaseStatus releaseDestinationUse(OwnerAttempt attempt) noexcept override
	{
		releases.push_back(attempt);
		return releaseStatus;
	}

	void deliver(OwnerCompletion value)
	{
		if (completion) {
			completion(std::move(value));
		}
	}

	int startCount = 0;
	YouTubeAccountDestinationOperationStatus cancelStatus = YouTubeAccountDestinationOperationStatus::Cancelled;
	YouTubeAccountDestinationUseReleaseStatus releaseStatus = YouTubeAccountDestinationUseReleaseStatus::Released;
};

struct RuntimeResult final {
	OutputLease lease;
	RuntimeYouTubeDestinationCompletionStatus status = RuntimeYouTubeDestinationCompletionStatus::Failed;
	std::string serverUrl;
	std::string streamKey;
};

void capture(RuntimeResult &result, RuntimeYouTubeDestinationCompletion completion)
{
	result.lease = completion.lease;
	result.status = completion.status;
	result.serverUrl = std::move(completion.serverUrl);
	result.streamKey.assign(completion.streamKey.view());
}

void testMismatchedLeaseShapesAndSecretMove()
{
	FakeOwner owner;
	const OutputLease outputLease{7, 13};
	const OwnerAttempt ownerAttempt{101, 2};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	RuntimeResult received;
	CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion completion) {
		capture(received, std::move(completion));
	}) == RuntimeYouTubeDestinationStartStatus::Started);

	owner.deliver(success(ownerAttempt));
	CHECK(received.lease == outputLease);
	CHECK(received.status == RuntimeYouTubeDestinationCompletionStatus::Success);
	CHECK(received.serverUrl == "rtmps://a.rtmps.youtube.com/live2");
	CHECK(received.streamKey == "owner-stream-key");

	provider.release(outputLease);
	CHECK(owner.releases.size() == 1);
	CHECK(owner.releases.front() == ownerAttempt);
}

void testSynchronousCompletionIsDeliveredAfterAttemptIsKnown()
{
	FakeOwner owner;
	const OutputLease outputLease{9, 21};
	const OwnerAttempt ownerAttempt{400, 3};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	owner.completeSynchronously = true;
	owner.synchronousCompletion = success(ownerAttempt);
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	int completions = 0;
	CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion completion) {
		++completions;
		CHECK(completion.lease == outputLease);
		CHECK(completion.status == RuntimeYouTubeDestinationCompletionStatus::Success);
	}) == RuntimeYouTubeDestinationStartStatus::Started);
	CHECK(completions == 1);
	provider.release(outputLease);
	CHECK(owner.releases.size() == 1);
}

void testCancelSuppressesLateSuccessAndFailsClosed()
{
	FakeOwner owner;
	const OutputLease outputLease{10, 1};
	const OwnerAttempt ownerAttempt{900, 44};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	int completions = 0;
	CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion) { ++completions; }) ==
	      RuntimeYouTubeDestinationStartStatus::Started);
	CHECK(provider.cancel(outputLease) == RuntimeYouTubeDestinationCancelStatus::Cancelled);
	CHECK(owner.cancellations.size() == 1);
	CHECK(owner.cancellations.front() == ownerAttempt);

	owner.deliver(success(ownerAttempt));
	CHECK(completions == 0);
	// The late successful callback cannot be handed to the output. The bridge
	// still asks the owner to release the exact attempt as a fail-closed guard.
	CHECK(owner.releases.size() == 1);
	CHECK(owner.releases.front() == ownerAttempt);
}

void testExactAndStaleRelease()
{
	FakeOwner owner;
	const OutputLease outputLease{11, 8};
	const OutputLease staleLease{11, 9};
	const OwnerAttempt ownerAttempt{77, 5};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) {}) ==
	      RuntimeYouTubeDestinationStartStatus::Started);
	owner.deliver(success(ownerAttempt));
	provider.release(staleLease);
	CHECK(owner.releases.empty());
	provider.release(outputLease);
	CHECK(owner.releases.size() == 1);
	provider.release(outputLease);
	CHECK(owner.releases.size() == 1);
}

void testShutdownPendingAndActive()
{
	{
		FakeOwner owner;
		const OutputLease outputLease{12, 1};
		const OwnerAttempt ownerAttempt{120, 2};
		owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
		YouTubeAccountRuntimeDestinationProvider provider(owner);
		CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Started);
		provider.shutdown();
		CHECK(owner.cancellations.size() == 1);
		owner.deliver(success(ownerAttempt));
		CHECK(owner.releases.empty());
		CHECK(provider.start({12, 2}, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Closed);
	}

	{
		FakeOwner owner;
		const OutputLease outputLease{13, 1};
		const OwnerAttempt ownerAttempt{130, 2};
		owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
		YouTubeAccountRuntimeDestinationProvider provider(owner);
		CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Started);
		owner.deliver(success(ownerAttempt));
		provider.shutdown();
		CHECK(owner.releases.size() == 1);
		CHECK(owner.releases.front() == ownerAttempt);
		owner.deliver(success(ownerAttempt));
		CHECK(owner.releases.size() == 1);
	}
}

void testDestructionMakesLateCallbackInert()
{
	FakeOwner owner;
	const OwnerAttempt ownerAttempt{140, 4};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	{
		YouTubeAccountRuntimeDestinationProvider provider(owner);
		CHECK(provider.start({14, 1}, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Started);
	}
	owner.deliver(success(ownerAttempt));
	CHECK(owner.releases.empty());
}

void testNonStartedStatusesClearMappingAndReleaseUnexpectedSuccess()
{
	FakeOwner owner;
	const OwnerAttempt ownerAttempt{150, 1};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::NotConfigured, std::nullopt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	int completions = 0;
	CHECK(provider.start({15, 1}, [&](RuntimeYouTubeDestinationCompletion) { ++completions; }) ==
	      RuntimeYouTubeDestinationStartStatus::SetupRequired);
	CHECK(completions == 0);
	// A faulty owner may retain and invoke its handler even after returning a
	// non-started result. The provider must not deliver it to the adapter.
	owner.deliver(success(ownerAttempt));
	CHECK(completions == 0);
	CHECK(owner.releases.size() == 1);
	CHECK(owner.releases.front() == ownerAttempt);
}

void testFailureMappingDoesNotCarrySecret()
{
	FakeOwner owner;
	const OutputLease outputLease{16, 1};
	const OwnerAttempt ownerAttempt{160, 2};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	RuntimeResult received;
	CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion completion) {
		capture(received, std::move(completion));
	}) == RuntimeYouTubeDestinationStartStatus::Started);
	owner.deliver(status(ownerAttempt, YouTubeDestinationPrepareStatus::ReauthorizationRequired));
	CHECK(received.status == RuntimeYouTubeDestinationCompletionStatus::SetupRequired);
	CHECK(received.serverUrl.empty());
	CHECK(received.streamKey.empty());
}

void testReentrantCancelBeforeOwnerReturnsCancelsExactAttempt()
{
	FakeOwner owner;
	const OutputLease outputLease{17, 1};
	const OwnerAttempt ownerAttempt{170, 2};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);
	owner.onStartBeforeReturn = [&]() {
		CHECK(provider.cancel(outputLease) == RuntimeYouTubeDestinationCancelStatus::Cancelled);
	};

	CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) {}) ==
	      RuntimeYouTubeDestinationStartStatus::Cancelled);
	CHECK(owner.cancellations.size() == 1);
	CHECK(owner.cancellations.front() == ownerAttempt);
	CHECK(owner.releases.empty());
}

void testReentrantShutdownBeforeOwnerReturnsClosesExactAttempt()
{
	{
		FakeOwner owner;
		const OutputLease outputLease{18, 1};
		const OwnerAttempt ownerAttempt{180, 2};
		owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
		YouTubeAccountRuntimeDestinationProvider provider(owner);
		owner.onStartBeforeReturn = [&]() { provider.shutdown(); };

		CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Closed);
		CHECK(owner.cancellations.size() == 1);
		CHECK(owner.cancellations.front() == ownerAttempt);
		CHECK(owner.releases.empty());
	}

	{
		FakeOwner owner;
		const OutputLease outputLease{19, 1};
		const OwnerAttempt ownerAttempt{190, 2};
		owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
		owner.completeSynchronously = true;
		owner.synchronousCompletion = success(ownerAttempt);
		YouTubeAccountRuntimeDestinationProvider provider(owner);
		owner.onStartBeforeReturn = [&]() { provider.shutdown(); };

		CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Closed);
		CHECK(owner.cancellations.empty());
		CHECK(owner.releases.size() == 1);
		CHECK(owner.releases.front() == ownerAttempt);
	}
}

void testThrowingRuntimeHandlerReleasesUseLease()
{
	FakeOwner owner;
	const OutputLease outputLease{20, 1};
	const OwnerAttempt ownerAttempt{200, 2};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	CHECK(provider.start(outputLease, [](RuntimeYouTubeDestinationCompletion) { throw 1; }) ==
	      RuntimeYouTubeDestinationStartStatus::Started);
	owner.deliver(success(ownerAttempt));
	CHECK(owner.releases.size() == 1);
	CHECK(owner.releases.front() == ownerAttempt);
	provider.release(outputLease);
	CHECK(owner.releases.size() == 1);
}

void testDuplicateUsableCompletionBeforeStartReturnsIsReleased()
{
	FakeOwner owner;
	const OutputLease outputLease{21, 1};
	const OwnerAttempt ownerAttempt{210, 2};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	owner.completeSynchronously = true;
	owner.synchronousCompletion = status(ownerAttempt, YouTubeDestinationPrepareStatus::NetworkFailure);
	YouTubeAccountRuntimeDestinationProvider provider(owner);
	owner.onStartBeforeReturn = [&]() { owner.deliver(success(ownerAttempt)); };

	RuntimeResult received;
	CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion completion) {
		capture(received, std::move(completion));
	}) == RuntimeYouTubeDestinationStartStatus::Started);
	CHECK(received.status == RuntimeYouTubeDestinationCompletionStatus::Failed);
	CHECK(received.streamKey.empty());
	CHECK(owner.releases.size() == 1);
	CHECK(owner.releases.front() == ownerAttempt);
}

void testInvalidSuccessfulDestinationIsNeverDelivered()
{
	FakeOwner owner;
	const OutputLease outputLease{22, 1};
	const OwnerAttempt ownerAttempt{220, 2};
	owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, ownerAttempt};
	YouTubeAccountRuntimeDestinationProvider provider(owner);

	RuntimeResult received;
	CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion completion) {
		capture(received, std::move(completion));
	}) == RuntimeYouTubeDestinationStartStatus::Started);
	owner.deliver(success(ownerAttempt, "rtmp://not-secure.example/live", "secret-that-must-not-pass"));
	CHECK(received.status == RuntimeYouTubeDestinationCompletionStatus::Failed);
	CHECK(received.serverUrl.empty());
	CHECK(received.streamKey.empty());
	CHECK(owner.releases.size() == 1);
	CHECK(owner.releases.front() == ownerAttempt);
}

void testMismatchedDeferredCompletionTerminatesBothSides()
{
	for (const bool usableCompletion : {false, true}) {
		FakeOwner owner;
		const OutputLease outputLease{23, usableCompletion ? 2U : 1U};
		const OwnerAttempt returnedAttempt{230, 4};
		const OwnerAttempt mismatchedAttempt{231, 5};
		owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, returnedAttempt};
		owner.completeSynchronously = true;
		owner.synchronousCompletion = usableCompletion
						      ? success(mismatchedAttempt)
						      : status(mismatchedAttempt, YouTubeDestinationPrepareStatus::NetworkFailure);
		YouTubeAccountRuntimeDestinationProvider provider(owner);

		int completions = 0;
		CHECK(provider.start(outputLease, [&](RuntimeYouTubeDestinationCompletion) { ++completions; }) ==
		      RuntimeYouTubeDestinationStartStatus::Failed);
		CHECK(completions == 0);
		CHECK(owner.cancellations.size() == 1);
		CHECK(owner.cancellations.front() == returnedAttempt);
		CHECK(owner.releases.size() == (usableCompletion ? 1U : 0U));
		if (usableCompletion) {
			CHECK(owner.releases.front() == mismatchedAttempt);
		}

		owner.completeSynchronously = false;
		owner.nextStart = {YouTubeAccountDestinationOperationStatus::Started, OwnerAttempt{232, 6}};
		CHECK(provider.start({23, 9}, [](RuntimeYouTubeDestinationCompletion) {}) ==
		      RuntimeYouTubeDestinationStartStatus::Started);
	}
}

} // namespace
} // namespace easy_multistream

int main()
{
	easy_multistream::testMismatchedLeaseShapesAndSecretMove();
	easy_multistream::testSynchronousCompletionIsDeliveredAfterAttemptIsKnown();
	easy_multistream::testCancelSuppressesLateSuccessAndFailsClosed();
	easy_multistream::testExactAndStaleRelease();
	easy_multistream::testShutdownPendingAndActive();
	easy_multistream::testDestructionMakesLateCallbackInert();
	easy_multistream::testNonStartedStatusesClearMappingAndReleaseUnexpectedSuccess();
	easy_multistream::testFailureMappingDoesNotCarrySecret();
	easy_multistream::testReentrantCancelBeforeOwnerReturnsCancelsExactAttempt();
	easy_multistream::testReentrantShutdownBeforeOwnerReturnsClosesExactAttempt();
	easy_multistream::testThrowingRuntimeHandlerReleasesUseLease();
	easy_multistream::testDuplicateUsableCompletionBeforeStartReturnsIsReleased();
	easy_multistream::testInvalidSuccessfulDestinationIsNeverDelivered();
	easy_multistream::testMismatchedDeferredCompletionTerminatesBothSides();

	if (easy_multistream::failures != 0) {
		std::cerr << easy_multistream::failures << " destination-provider test(s) failed\n";
		return 1;
	}
	std::cout << "All destination-provider tests passed\n";
	return 0;
}

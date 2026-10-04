// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "runtime-controller.hpp"
#include "youtube-account-runtime-owner.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace easy_multistream {

// RuntimeController intentionally depends on a small destination-provider
// port. This owner-facing port keeps the bridge independently testable without
// making RuntimeController know anything about profiles, OAuth, or credentials.
class IYouTubeAccountDestinationRuntime {
public:
	using CompletionHandler = YouTubeAccountDestinationPreparer::CompletionHandler;

	virtual ~IYouTubeAccountDestinationRuntime() = default;

	// All methods, including completion delivery, run on the same owner thread.
	// The concrete owner must outlive the bridge, and the bridge must be shut
	// down before that owner is destroyed.
	virtual YouTubeAccountDestinationStartResult
	startDestinationPreparationWithAttempt(CompletionHandler completionHandler) noexcept = 0;
	virtual YouTubeAccountDestinationOperationStatus
	cancelDestinationPreparation(YouTubeDestinationPrepareAttempt attempt) noexcept = 0;
	virtual YouTubeAccountDestinationUseReleaseStatus
	releaseDestinationUse(YouTubeDestinationPrepareAttempt attempt) noexcept = 0;
};

// Production adapter for the concrete owner. Keeping this forwarding object
// separate means the bridge's tests do not need to construct OAuth, profile,
// or Windows credential implementations just to exercise lease handling.
class YouTubeAccountRuntimeOwnerForwarder final : public IYouTubeAccountDestinationRuntime {
public:
	explicit YouTubeAccountRuntimeOwnerForwarder(YouTubeAccountRuntimeOwner &owner) noexcept;

	YouTubeAccountRuntimeOwnerForwarder(const YouTubeAccountRuntimeOwnerForwarder &) = delete;
	YouTubeAccountRuntimeOwnerForwarder &operator=(const YouTubeAccountRuntimeOwnerForwarder &) = delete;

	YouTubeAccountDestinationStartResult
	startDestinationPreparationWithAttempt(CompletionHandler completionHandler) noexcept override;
	YouTubeAccountDestinationOperationStatus
	cancelDestinationPreparation(YouTubeDestinationPrepareAttempt attempt) noexcept override;
	YouTubeAccountDestinationUseReleaseStatus
	releaseDestinationUse(YouTubeDestinationPrepareAttempt attempt) noexcept override;

private:
	YouTubeAccountRuntimeOwner &owner_;
};

// Bridges one RuntimeController output lease to one account-owner preparation
// attempt. The two lease types have independent generators and are never
// converted by value; the bridge retains their exact relationship until the
// output or preparation is terminal.
class YouTubeAccountRuntimeDestinationProvider final : public IRuntimeYouTubeDestinationProvider {
public:
	explicit YouTubeAccountRuntimeDestinationProvider(IYouTubeAccountDestinationRuntime &owner) noexcept;
	~YouTubeAccountRuntimeDestinationProvider() override;

	YouTubeAccountRuntimeDestinationProvider(const YouTubeAccountRuntimeDestinationProvider &) = delete;
	YouTubeAccountRuntimeDestinationProvider &operator=(const YouTubeAccountRuntimeDestinationProvider &) = delete;

	RuntimeYouTubeDestinationStartStatus start(OutputLease lease,
									   CompletionHandler completionHandler) noexcept override;
	RuntimeYouTubeDestinationCancelStatus cancel(OutputLease lease) noexcept override;
	void release(OutputLease lease) noexcept override;
	void shutdown() noexcept override;

private:
	struct PendingPreparation final {
		OutputLease outputLease;
		std::optional<YouTubeDestinationPrepareAttempt> ownerAttempt;
		std::optional<YouTubeDestinationPrepareCompletion> deferredCompletion;
		CompletionHandler completionHandler;
		bool startReturned = false;
		bool cancelRequested = false;
	};

	struct ActiveDestinationUse final {
		OutputLease outputLease;
		YouTubeDestinationPrepareAttempt ownerAttempt;
	};

	struct CallbackState final {
		std::mutex mutex;
		bool accepting = true;
		std::optional<PendingPreparation> pending;
		std::optional<ActiveDestinationUse> activeUse;
		// A shutdown can happen synchronously while owner.start() is still on
		// the stack, before the owner attempt is returned. Remember a use that
		// was already released so the outer start call does not release twice.
		std::optional<YouTubeDestinationPrepareAttempt> retiredOwnerUse;
		// This is cleared before the bridge can outlive its owner. It is only
		// called after accepting/pending state has been checked and the mutex is
		// released.
		std::function<void(YouTubeDestinationPrepareAttempt)> releaseOwnerUse;
	};

	static void dispatchCompletion(const std::shared_ptr<CallbackState> &state,
							   YouTubeDestinationPrepareCompletion completion) noexcept;
	static void releaseOwnerUse(const std::shared_ptr<CallbackState> &state,
							YouTubeDestinationPrepareAttempt attempt) noexcept;
	static void releaseActiveUse(const std::shared_ptr<CallbackState> &state, OutputLease lease) noexcept;
	static RuntimeYouTubeDestinationCompletion makeRuntimeCompletion(
		OutputLease outputLease, YouTubeDestinationPrepareCompletion completion) noexcept;
	static RuntimeYouTubeDestinationStartStatus mapStartStatus(
		YouTubeAccountDestinationOperationStatus status) noexcept;
	static RuntimeYouTubeDestinationCompletionStatus mapCompletionStatus(
		YouTubeDestinationPrepareStatus status) noexcept;
	static bool hasValidLease(OutputLease lease) noexcept;

	IYouTubeAccountDestinationRuntime &owner_;
	std::shared_ptr<CallbackState> state_;
};

} // namespace easy_multistream

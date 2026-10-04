// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-runtime-destination-provider.hpp"

#include "youtube-destination.hpp"

#include <utility>

namespace easy_multistream {
namespace {

using OwnerCompletion = YouTubeDestinationPrepareCompletion;
using OwnerAttempt = YouTubeDestinationPrepareAttempt;

bool validIngestion(const std::optional<YouTubeResolvedIngestion> &ingestion) noexcept
{
	return ingestion.has_value() && validateYouTubeServerUrl(ingestion->serverUrl) ==
						YouTubeServerUrlValidationError::None &&
	       validateYouTubeStreamKey(ingestion->streamKey.view()) == StreamKeyValidationError::None;
}

} // namespace

YouTubeAccountRuntimeOwnerForwarder::YouTubeAccountRuntimeOwnerForwarder(
	YouTubeAccountRuntimeOwner &owner) noexcept
	: owner_(owner)
{
}

YouTubeAccountDestinationStartResult
YouTubeAccountRuntimeOwnerForwarder::startDestinationPreparationWithAttempt(CompletionHandler completionHandler) noexcept
{
	return owner_.startDestinationPreparationWithAttempt(std::move(completionHandler));
}

YouTubeAccountDestinationOperationStatus
YouTubeAccountRuntimeOwnerForwarder::cancelDestinationPreparation(YouTubeDestinationPrepareAttempt attempt) noexcept
{
	return owner_.cancelDestinationPreparation(attempt);
}

YouTubeAccountDestinationUseReleaseStatus
YouTubeAccountRuntimeOwnerForwarder::releaseDestinationUse(YouTubeDestinationPrepareAttempt attempt) noexcept
{
	return owner_.releaseDestinationUse(attempt);
}

YouTubeAccountRuntimeDestinationProvider::YouTubeAccountRuntimeDestinationProvider(
	IYouTubeAccountDestinationRuntime &owner) noexcept
	: owner_(owner), state_(std::make_shared<CallbackState>())
{
	// The callback passed to RuntimeOwner captures only a weak state. It never
	// captures this provider, the RuntimeController, or an OBS object.
	state_->releaseOwnerUse = [ownerPointer = &owner_](OwnerAttempt attempt) noexcept {
		if (ownerPointer != nullptr) {
			(void)ownerPointer->releaseDestinationUse(attempt);
		}
	};
}

YouTubeAccountRuntimeDestinationProvider::~YouTubeAccountRuntimeDestinationProvider()
{
	shutdown();
}

void YouTubeAccountRuntimeDestinationProvider::releaseOwnerUse(
	const std::shared_ptr<CallbackState> &state, OwnerAttempt attempt) noexcept
{
	std::function<void(OwnerAttempt)> release;
	{
		std::lock_guard lock(state->mutex);
		if (state->retiredOwnerUse.has_value() && *state->retiredOwnerUse == attempt) {
			return;
		}
		state->retiredOwnerUse = attempt;
		release = state->releaseOwnerUse;
	}
	if (release) {
		try {
			release(attempt);
		} catch (...) {
			// The owner-facing port is noexcept by contract. Do not let a test or
			// replacement provider unwind through an account callback regardless.
		}
	}
}

void YouTubeAccountRuntimeDestinationProvider::releaseActiveUse(
	const std::shared_ptr<CallbackState> &state, OutputLease lease) noexcept
{
	std::optional<OwnerAttempt> attempt;
	{
		std::lock_guard lock(state->mutex);
		if (state->activeUse.has_value() && state->activeUse->outputLease == lease) {
			attempt = state->activeUse->ownerAttempt;
			state->activeUse.reset();
		}
	}
	if (attempt.has_value()) {
		releaseOwnerUse(state, *attempt);
	}
}

RuntimeYouTubeDestinationStartStatus
YouTubeAccountRuntimeDestinationProvider::start(OutputLease lease,
									 CompletionHandler completionHandler) noexcept
{
	if (!hasValidLease(lease) || !completionHandler) {
		return RuntimeYouTubeDestinationStartStatus::Failed;
	}

	try {
		{
			std::lock_guard lock(state_->mutex);
			if (!state_->accepting) {
				return RuntimeYouTubeDestinationStartStatus::Closed;
			}
			if (state_->pending.has_value() || state_->activeUse.has_value()) {
				return RuntimeYouTubeDestinationStartStatus::Busy;
			}
			PendingPreparation pending;
			pending.outputLease = lease;
			pending.completionHandler = std::move(completionHandler);
			state_->pending.emplace(std::move(pending));
		}
	} catch (...) {
		return RuntimeYouTubeDestinationStartStatus::Failed;
	}

	const std::weak_ptr<CallbackState> weakState = state_;
	YouTubeAccountDestinationStartResult ownerResult;
	try {
		ownerResult = owner_.startDestinationPreparationWithAttempt(
			[weakState](OwnerCompletion completion) noexcept {
				const auto state = weakState.lock();
				if (state != nullptr) {
					YouTubeAccountRuntimeDestinationProvider::dispatchCompletion(state, std::move(completion));
				}
			});
	} catch (...) {
		ownerResult.status = YouTubeAccountDestinationOperationStatus::OperationFailed;
	}

	std::optional<OwnerCompletion> deferred;
	std::optional<OwnerAttempt> deferredRelease;
	std::optional<OwnerAttempt> cancelAttempt;
	RuntimeYouTubeDestinationStartStatus returnStatus = mapStartStatus(ownerResult.status);
	bool returnStarted = ownerResult.status == YouTubeAccountDestinationOperationStatus::Started;
	bool closedDuringStart = false;

	{
		std::lock_guard lock(state_->mutex);
		if (!state_->accepting) {
			closedDuringStart = true;
			if (ownerResult.status == YouTubeAccountDestinationOperationStatus::Started &&
			    ownerResult.attempt.has_value() && ownerResult.attempt->isValid() &&
			    (!state_->retiredOwnerUse.has_value() || !(*state_->retiredOwnerUse == *ownerResult.attempt))) {
				// shutdown() may have run re-entrantly before the owner returned
				// its attempt. In that case it could not cancel the owner operation;
				// the outer start call must close that exact attempt now.
				cancelAttempt = ownerResult.attempt;
			}
		} else if (!state_->pending.has_value()) {
			// A synchronous completion may already have consumed the pending
			// operation and started the adapter through the caller's handler.
			return returnStarted ? RuntimeYouTubeDestinationStartStatus::Started : returnStatus;
		} else {
			PendingPreparation &pending = *state_->pending;
			if (ownerResult.status == YouTubeAccountDestinationOperationStatus::Started &&
			    ownerResult.attempt.has_value() && ownerResult.attempt->isValid()) {
				pending.ownerAttempt = ownerResult.attempt;
				pending.startReturned = true;
				if (pending.cancelRequested) {
					if (pending.deferredCompletion.has_value() && pending.deferredCompletion->succeeded()) {
						deferredRelease = pending.deferredCompletion->attempt;
					}
					if (!state_->retiredOwnerUse.has_value() ||
					    !(*state_->retiredOwnerUse == *pending.ownerAttempt)) {
						cancelAttempt = pending.ownerAttempt;
					}
					state_->pending.reset();
					returnStarted = false;
					returnStatus = RuntimeYouTubeDestinationStartStatus::Cancelled;
				} else if (pending.deferredCompletion.has_value()) {
					if (!(pending.deferredCompletion->attempt == *pending.ownerAttempt)) {
						// A synchronous completion must describe the exact attempt
						// returned by start(). A faulty owner must not strand either
						// side of the mapping in Busy.
						if (pending.deferredCompletion->succeeded()) {
							deferredRelease = pending.deferredCompletion->attempt;
						}
						cancelAttempt = pending.ownerAttempt;
						state_->pending.reset();
						returnStarted = false;
						returnStatus = RuntimeYouTubeDestinationStartStatus::Failed;
					} else {
						deferred = std::move(pending.deferredCompletion);
						pending.deferredCompletion.reset();
					}
				}
			} else {
				if (pending.deferredCompletion.has_value() && pending.deferredCompletion->succeeded()) {
					deferredRelease = pending.deferredCompletion->attempt;
				}
				state_->pending.reset();
				returnStarted = false;
				returnStatus = ownerResult.status == YouTubeAccountDestinationOperationStatus::Started
						       ? RuntimeYouTubeDestinationStartStatus::Failed
						       : returnStatus;
			}
		}
	}

	if (cancelAttempt.has_value()) {
		try {
			(void)owner_.cancelDestinationPreparation(*cancelAttempt);
		} catch (...) {
		}
	}
	if (deferredRelease.has_value()) {
		releaseOwnerUse(state_, *deferredRelease);
	}
	if (deferred.has_value()) {
		dispatchCompletion(state_, std::move(*deferred));
	}
	if (closedDuringStart) {
		return RuntimeYouTubeDestinationStartStatus::Closed;
	}

	if (returnStarted) {
		return RuntimeYouTubeDestinationStartStatus::Started;
	}
	return returnStatus;
}

RuntimeYouTubeDestinationCancelStatus
YouTubeAccountRuntimeDestinationProvider::cancel(OutputLease lease) noexcept
{
	if (!hasValidLease(lease)) {
		return RuntimeYouTubeDestinationCancelStatus::Failed;
	}

	std::optional<OwnerAttempt> cancelAttempt;
	std::optional<OwnerAttempt> deferredRelease;
	bool releaseActive = false;
	{
		std::lock_guard lock(state_->mutex);
		if (!state_->accepting) {
			return RuntimeYouTubeDestinationCancelStatus::Closed;
		}
		if (state_->pending.has_value() && state_->pending->outputLease == lease) {
			state_->pending->cancelRequested = true;
			state_->pending->completionHandler = {};
			if (state_->pending->deferredCompletion.has_value() &&
			    state_->pending->deferredCompletion->succeeded()) {
				deferredRelease = state_->pending->deferredCompletion->attempt;
			}
			state_->pending->deferredCompletion.reset();
			if (state_->pending->ownerAttempt.has_value()) {
				if (!deferredRelease.has_value() || !(*deferredRelease == *state_->pending->ownerAttempt)) {
					cancelAttempt = state_->pending->ownerAttempt;
				}
				state_->pending.reset();
			}
		} else if (state_->activeUse.has_value() && state_->activeUse->outputLease == lease) {
			releaseActive = true;
		} else {
			return RuntimeYouTubeDestinationCancelStatus::NoActiveAttempt;
		}
	}
	if (deferredRelease.has_value()) {
		releaseOwnerUse(state_, *deferredRelease);
	}

	if (cancelAttempt.has_value()) {
		try {
			(void)owner_.cancelDestinationPreparation(*cancelAttempt);
		} catch (...) {
			return RuntimeYouTubeDestinationCancelStatus::Failed;
		}
	}
	if (releaseActive) {
		releaseActiveUse(state_, lease);
	}
	return RuntimeYouTubeDestinationCancelStatus::Cancelled;
}

void YouTubeAccountRuntimeDestinationProvider::release(OutputLease lease) noexcept
{
	releaseActiveUse(state_, lease);
}

void YouTubeAccountRuntimeDestinationProvider::shutdown() noexcept
{
	std::optional<OwnerAttempt> cancelAttempt;
	std::optional<OwnerAttempt> deferredRelease;
	std::optional<OwnerAttempt> activeRelease;
	{
		std::lock_guard lock(state_->mutex);
		if (!state_->accepting) {
			return;
		}
		state_->accepting = false;
		if (state_->pending.has_value()) {
			cancelAttempt = state_->pending->ownerAttempt;
			if (state_->pending->deferredCompletion.has_value() &&
			    state_->pending->deferredCompletion->succeeded()) {
				deferredRelease = state_->pending->deferredCompletion->attempt;
			}
			state_->pending.reset();
		}
		if (state_->activeUse.has_value()) {
			activeRelease = state_->activeUse->ownerAttempt;
			state_->activeUse.reset();
		}
	}

	if (cancelAttempt.has_value()) {
		try {
			(void)owner_.cancelDestinationPreparation(*cancelAttempt);
		} catch (...) {
		}
	}
	if (deferredRelease.has_value()) {
		releaseOwnerUse(state_, *deferredRelease);
	}
	if (activeRelease.has_value()) {
		releaseOwnerUse(state_, *activeRelease);
	}

	// No queued callback can call the owner after this point. Keep the state
	// object itself alive for weak callbacks, but make its owner release hook
	// inert before the owner may be destroyed.
	{
		std::lock_guard lock(state_->mutex);
		state_->releaseOwnerUse = {};
	}
}

void YouTubeAccountRuntimeDestinationProvider::dispatchCompletion(
	const std::shared_ptr<CallbackState> &state, OwnerCompletion completion) noexcept
{
	if (state == nullptr) {
		return;
	}

	CompletionHandler handler;
	OutputLease outputLease;
	OwnerAttempt ownerAttempt;
	bool dispatch = false;
	bool usable = false;
	std::optional<OwnerAttempt> staleRelease;
	std::optional<OwnerAttempt> invalidRelease;

	{
		std::lock_guard lock(state->mutex);
		if (!state->accepting) {
			return;
		}
		if (!state->pending.has_value()) {
			if (completion.succeeded() &&
			    (!state->activeUse.has_value() || !(state->activeUse->ownerAttempt == completion.attempt))) {
				staleRelease = completion.attempt;
			}
		} else {
			PendingPreparation &pending = *state->pending;
			if (pending.ownerAttempt.has_value() && !(*pending.ownerAttempt == completion.attempt)) {
				if (completion.succeeded()) {
					staleRelease = completion.attempt;
				}
			} else if (!pending.startReturned || !pending.ownerAttempt.has_value()) {
				if (!pending.deferredCompletion.has_value()) {
					pending.deferredCompletion.emplace(std::move(completion));
				} else if (completion.succeeded() &&
					   (!pending.deferredCompletion->succeeded() ||
					    !(pending.deferredCompletion->attempt == completion.attempt))) {
					// A provider contract permits exactly one completion. If a
					// faulty implementation delivers a later usable result before
					// start() returns, it must not leave an untracked owner-use
					// lease behind.
					staleRelease = completion.attempt;
				}
			} else {
				outputLease = pending.outputLease;
				ownerAttempt = *pending.ownerAttempt;
				handler = std::move(pending.completionHandler);
				usable = completion.succeeded() && validIngestion(completion.ingestion);
				if (completion.succeeded() && !usable) {
					// A concrete owner validates ingestion before setting its use lease,
					// but keep this bridge fail-closed for a faulty replacement owner.
					invalidRelease = ownerAttempt;
				}
				state->pending.reset();
				if (usable) {
					state->activeUse = ActiveDestinationUse{outputLease, ownerAttempt};
				}
				dispatch = true;
			}
		}
	}

	if (staleRelease.has_value()) {
		releaseOwnerUse(state, *staleRelease);
		return;
	}
	if (invalidRelease.has_value()) {
		releaseOwnerUse(state, *invalidRelease);
	}
	if (!dispatch) {
		return;
	}

	RuntimeYouTubeDestinationCompletion runtimeCompletion = makeRuntimeCompletion(outputLease, std::move(completion));
	if (!handler) {
		if (usable) {
			releaseActiveUse(state, outputLease);
		}
		return;
	}
	try {
		handler(std::move(runtimeCompletion));
	} catch (...) {
		if (usable) {
			releaseActiveUse(state, outputLease);
		}
	}
}

RuntimeYouTubeDestinationCompletion YouTubeAccountRuntimeDestinationProvider::makeRuntimeCompletion(
	OutputLease outputLease, OwnerCompletion completion) noexcept
{
	if (completion.status == YouTubeDestinationPrepareStatus::Success && validIngestion(completion.ingestion)) {
		YouTubeResolvedIngestion ingestion = std::move(*completion.ingestion);
		completion.ingestion.reset();
		return {outputLease, RuntimeYouTubeDestinationCompletionStatus::Success,
				std::move(ingestion.serverUrl), std::move(ingestion.streamKey)};
	}

	const RuntimeYouTubeDestinationCompletionStatus status = mapCompletionStatus(completion.status);
	completion.ingestion.reset();
	return {outputLease, status};
}

RuntimeYouTubeDestinationStartStatus YouTubeAccountRuntimeDestinationProvider::mapStartStatus(
	YouTubeAccountDestinationOperationStatus status) noexcept
{
	switch (status) {
	case YouTubeAccountDestinationOperationStatus::Started:
		return RuntimeYouTubeDestinationStartStatus::Started;
	case YouTubeAccountDestinationOperationStatus::Busy:
		return RuntimeYouTubeDestinationStartStatus::Busy;
	case YouTubeAccountDestinationOperationStatus::Closed:
		return RuntimeYouTubeDestinationStartStatus::Closed;
	case YouTubeAccountDestinationOperationStatus::Cancelled:
	case YouTubeAccountDestinationOperationStatus::ProfileChanged:
		return RuntimeYouTubeDestinationStartStatus::Cancelled;
	case YouTubeAccountDestinationOperationStatus::NotConfigured:
	case YouTubeAccountDestinationOperationStatus::NotRestored:
	case YouTubeAccountDestinationOperationStatus::NoSelection:
	case YouTubeAccountDestinationOperationStatus::NotAccountMode:
	case YouTubeAccountDestinationOperationStatus::ProfileUnavailable:
		return RuntimeYouTubeDestinationStartStatus::SetupRequired;
	case YouTubeAccountDestinationOperationStatus::WrongThread:
	case YouTubeAccountDestinationOperationStatus::StaleAttempt:
	case YouTubeAccountDestinationOperationStatus::OperationFailed:
		return RuntimeYouTubeDestinationStartStatus::Failed;
	}
	return RuntimeYouTubeDestinationStartStatus::Failed;
}

RuntimeYouTubeDestinationCompletionStatus YouTubeAccountRuntimeDestinationProvider::mapCompletionStatus(
	YouTubeDestinationPrepareStatus status) noexcept
{
	switch (status) {
	case YouTubeDestinationPrepareStatus::Success:
		return RuntimeYouTubeDestinationCompletionStatus::Failed;
	case YouTubeDestinationPrepareStatus::ReauthorizationRequired:
	case YouTubeDestinationPrepareStatus::CredentialUnavailable:
	case YouTubeDestinationPrepareStatus::DestinationUnavailable:
		return RuntimeYouTubeDestinationCompletionStatus::SetupRequired;
	case YouTubeDestinationPrepareStatus::Cancelled:
	case YouTubeDestinationPrepareStatus::ProfileChanged:
		return RuntimeYouTubeDestinationCompletionStatus::Cancelled;
	case YouTubeDestinationPrepareStatus::DestinationAlreadyActive:
	case YouTubeDestinationPrepareStatus::NetworkFailure:
	case YouTubeDestinationPrepareStatus::ServiceUnavailable:
	case YouTubeDestinationPrepareStatus::InvalidResponse:
		return RuntimeYouTubeDestinationCompletionStatus::Failed;
	}
	return RuntimeYouTubeDestinationCompletionStatus::Failed;
}

bool YouTubeAccountRuntimeDestinationProvider::hasValidLease(OutputLease lease) noexcept
{
	return lease.generation != 0 && lease.attempt != 0;
}

} // namespace easy_multistream

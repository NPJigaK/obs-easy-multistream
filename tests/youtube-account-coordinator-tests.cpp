// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-coordinator.hpp"

#include <cstdint>
#include <iostream>
#include <utility>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                 \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using easy_multistream::YouTubeAccountCoordinator;
using easy_multistream::YouTubeAccountDiscovery;
using easy_multistream::YouTubeAccountFailure;
using easy_multistream::YouTubeAccountLease;
using easy_multistream::YouTubeAccountSavedCredentialState;
using easy_multistream::YouTubeAccountSelectionValidationError;
using easy_multistream::YouTubeAccountState;
using easy_multistream::validateYouTubeAccountSelection;

static_assert(noexcept(
	std::declval<YouTubeAccountCoordinator &>().commitCredentialStored(std::declval<YouTubeAccountLease>())));

YouTubeAccountLease leaseFrom(const easy_multistream::YouTubeAccountTransition &transition)
{
	CHECK(transition.snapshot.lease.has_value());
	return transition.snapshot.lease.value_or(YouTubeAccountLease{});
}

YouTubeAccountDiscovery selection(const char *suffix)
{
	return {std::string("channel-") + suffix, std::string("Channel ") + suffix, std::string("stream-") + suffix,
		std::string("Stream ") + suffix};
}

void connect(YouTubeAccountCoordinator &coordinator, const YouTubeAccountDiscovery &discovery)
{
	const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
	CHECK(coordinator.authorizationCallbackAccepted(lease).snapshot.state == YouTubeAccountState::ExchangingCode);
	CHECK(coordinator.tokenExchangeSucceeded(lease).snapshot.state == YouTubeAccountState::Discovering);
	CHECK(coordinator.discoverySucceeded(lease, discovery).snapshot.state ==
	      YouTubeAccountState::PersistingCredential);
	CHECK(coordinator.credentialStored(lease).snapshot.state == YouTubeAccountState::Connected);
}

void testLifecycleAndSecretFreeSnapshot()
{
	YouTubeAccountCoordinator coordinator;
	CHECK(coordinator.snapshot().state == YouTubeAccountState::Disconnected);
	const auto began = coordinator.beginAuthorization();
	const YouTubeAccountLease lease = leaseFrom(began);
	CHECK(began.changed);
	CHECK(lease.generation == began.snapshot.generation);
	CHECK(lease.attempt != 0);
	CHECK(coordinator.authorizationCallbackAccepted(lease).snapshot.state == YouTubeAccountState::ExchangingCode);
	CHECK(coordinator.tokenExchangeSucceeded(lease).snapshot.state == YouTubeAccountState::Discovering);

	const auto discovered = coordinator.discoverySucceeded(lease, selection("one"));
	CHECK(discovered.snapshot.state == YouTubeAccountState::PersistingCredential);
	CHECK(discovered.snapshot.channelId.empty());
	const auto connected = coordinator.credentialStored(lease);
	CHECK(connected.snapshot.state == YouTubeAccountState::Connected);
	CHECK(!connected.snapshot.lease.has_value());
	CHECK(connected.snapshot.connectionLease == lease);
	CHECK(connected.snapshot.channelId == "channel-one");
	CHECK(connected.snapshot.streamId == "stream-one");
	CHECK(connected.snapshot.failure == YouTubeAccountFailure::None);
}

void testStaleAndDuplicateEventsAreNoOps()
{
	YouTubeAccountCoordinator coordinator;
	const auto began = coordinator.beginAuthorization();
	const YouTubeAccountLease lease = leaseFrom(began);
	CHECK(!coordinator.beginAuthorization().changed);
	CHECK(!coordinator.authorizationCallbackAccepted({lease.generation, lease.attempt + 1}).changed);
	CHECK(!coordinator.tokenExchangeSucceeded(lease).changed);

	CHECK(coordinator.authorizationCallbackAccepted(lease).changed);
	const std::uint64_t revision = coordinator.snapshot().revision;
	CHECK(!coordinator.authorizationCallbackAccepted(lease).changed);
	CHECK(coordinator.snapshot().revision == revision);
	CHECK(coordinator.tokenExchangeSucceeded(lease).changed);
	CHECK(coordinator.discoverySucceeded(lease, selection("one")).changed);
	CHECK(coordinator.credentialStored(lease).changed);
	CHECK(!coordinator.credentialStored(lease).changed);

	const auto after = coordinator.snapshot();
	CHECK(!coordinator.attemptFailed(lease, YouTubeAccountFailure::NetworkFailure).changed);
	CHECK(!coordinator.cancel(lease).changed);
	CHECK(!coordinator.attemptFailed(lease, YouTubeAccountFailure::Cancelled).changed);
	CHECK(coordinator.snapshot().revision == after.revision);
}

void testInvalidDiscoveryCannotConnect()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(lease);
	coordinator.tokenExchangeSucceeded(lease);
	const auto failed = coordinator.discoverySucceeded(lease, {});
	CHECK(failed.snapshot.state == YouTubeAccountState::Failed);
	CHECK(failed.snapshot.failure == YouTubeAccountFailure::InvalidResponse);
	CHECK(failed.snapshot.channelId.empty());
	CHECK(failed.snapshot.streamId.empty());
}

void testDiscoveryValuesAreBoundedAndSafeForSnapshots()
{
	const auto rejected = [](const YouTubeAccountDiscovery &discovery) {
		YouTubeAccountCoordinator coordinator;
		const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
		coordinator.authorizationCallbackAccepted(lease);
		coordinator.tokenExchangeSucceeded(lease);
		return coordinator.discoverySucceeded(lease, discovery).snapshot;
	};

	YouTubeAccountDiscovery oversizedId = selection("valid");
	oversizedId.channelId.assign(easy_multistream::kYouTubeAccountMaxIdentifierBytes + 1, 'a');
	CHECK(rejected(oversizedId).state == YouTubeAccountState::Failed);
	YouTubeAccountDiscovery maximumId = selection("valid");
	maximumId.channelId.assign(easy_multistream::kYouTubeAccountMaxIdentifierBytes, 'a');
	CHECK(validateYouTubeAccountSelection(maximumId) == YouTubeAccountSelectionValidationError::None);

	YouTubeAccountDiscovery maximumLabel = selection("valid");
	maximumLabel.channelLabel.assign(easy_multistream::kYouTubeAccountMaxLabelBytes, 'a');
	CHECK(validateYouTubeAccountSelection(maximumLabel) == YouTubeAccountSelectionValidationError::None);
	maximumLabel.channelLabel.push_back('a');
	CHECK(validateYouTubeAccountSelection(maximumLabel) ==
	      YouTubeAccountSelectionValidationError::InvalidChannelLabel);

	YouTubeAccountDiscovery controlLabel = selection("valid");
	controlLabel.channelLabel = "unsafe\nlabel";
	CHECK(rejected(controlLabel).state == YouTubeAccountState::Failed);

	YouTubeAccountDiscovery emptyChannelLabel = selection("valid");
	emptyChannelLabel.channelLabel.clear();
	CHECK(validateYouTubeAccountSelection(emptyChannelLabel) ==
	      YouTubeAccountSelectionValidationError::EmptyChannelLabel);
	CHECK(rejected(emptyChannelLabel).state == YouTubeAccountState::Failed);

	YouTubeAccountDiscovery emptyStreamLabel = selection("valid");
	emptyStreamLabel.streamLabel.clear();
	CHECK(validateYouTubeAccountSelection(emptyStreamLabel) ==
	      YouTubeAccountSelectionValidationError::EmptyStreamLabel);
	CHECK(rejected(emptyStreamLabel).state == YouTubeAccountState::Failed);

	YouTubeAccountDiscovery invalidUtf8 = selection("valid");
	invalidUtf8.streamLabel.assign("\xC0\xAF", 2);
	CHECK(rejected(invalidUtf8).state == YouTubeAccountState::Failed);

	YouTubeAccountCoordinator coordinator;
	YouTubeAccountDiscovery unicodeLabels = selection("valid");
	unicodeLabels.channelLabel = u8"配信チャンネル";
	unicodeLabels.streamLabel = u8"いつもの配信";
	connect(coordinator, unicodeLabels);
	CHECK(coordinator.snapshot().channelLabel == unicodeLabels.channelLabel);
	CHECK(coordinator.snapshot().streamLabel == unicodeLabels.streamLabel);
}

void testSharedSelectionValidationClassifiesEachField()
{
	const YouTubeAccountDiscovery valid = selection("valid");
	CHECK(validateYouTubeAccountSelection(valid) == YouTubeAccountSelectionValidationError::None);

	YouTubeAccountDiscovery emptyChannelId = valid;
	emptyChannelId.channelId.clear();
	CHECK(validateYouTubeAccountSelection(emptyChannelId) ==
	      YouTubeAccountSelectionValidationError::EmptyChannelId);

	YouTubeAccountDiscovery invalidChannelId = valid;
	invalidChannelId.channelId = "channel id";
	CHECK(validateYouTubeAccountSelection(invalidChannelId) ==
	      YouTubeAccountSelectionValidationError::InvalidChannelId);

	YouTubeAccountDiscovery invalidChannelLabel = valid;
	invalidChannelLabel.channelLabel.assign("bad\xC0\xAF", 5);
	CHECK(validateYouTubeAccountSelection(invalidChannelLabel) ==
	      YouTubeAccountSelectionValidationError::InvalidChannelLabel);

	YouTubeAccountDiscovery emptyStreamId = valid;
	emptyStreamId.streamId.clear();
	CHECK(validateYouTubeAccountSelection(emptyStreamId) == YouTubeAccountSelectionValidationError::EmptyStreamId);

	YouTubeAccountDiscovery invalidStreamId = valid;
	invalidStreamId.streamId.assign(1, '\x7F');
	CHECK(validateYouTubeAccountSelection(invalidStreamId) ==
	      YouTubeAccountSelectionValidationError::InvalidStreamId);

	YouTubeAccountDiscovery invalidStreamLabel = valid;
	invalidStreamLabel.streamLabel.assign("bad\xE0\x80\x80", 6);
	CHECK(validateYouTubeAccountSelection(invalidStreamLabel) ==
	      YouTubeAccountSelectionValidationError::InvalidStreamLabel);

	YouTubeAccountDiscovery lineSeparatorLabel = valid;
	lineSeparatorLabel.streamLabel = "bad\xE2\x80\xA8"
					 "label";
	CHECK(validateYouTubeAccountSelection(lineSeparatorLabel) ==
	      YouTubeAccountSelectionValidationError::InvalidStreamLabel);

	YouTubeAccountDiscovery bidiControlLabel = valid;
	bidiControlLabel.streamLabel = "bad\xE2\x80\xAE"
				       "label";
	CHECK(validateYouTubeAccountSelection(bidiControlLabel) ==
	      YouTubeAccountSelectionValidationError::InvalidStreamLabel);
}

void testReplacementIsTransactional()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease retry = leaseFrom(coordinator.beginAuthorization());
	CHECK(coordinator.snapshot().channelId == "channel-old");
	coordinator.authorizationCallbackAccepted(retry);
	coordinator.tokenExchangeSucceeded(retry);
	coordinator.discoverySucceeded(retry, selection("new"));
	const auto failed = coordinator.attemptFailed(retry, YouTubeAccountFailure::NetworkFailure);
	CHECK(failed.snapshot.state == YouTubeAccountState::Connected);
	CHECK(failed.snapshot.channelId == "channel-old");
	CHECK(failed.snapshot.failure == YouTubeAccountFailure::NetworkFailure);

	const YouTubeAccountLease cancelledRetry = leaseFrom(coordinator.beginAuthorization());
	const auto cancelled = coordinator.cancel(cancelledRetry);
	CHECK(cancelled.snapshot.state == YouTubeAccountState::Connected);
	CHECK(cancelled.snapshot.channelId == "channel-old");
}

void testReplacementCommitsOnlyAfterCredentialStorage()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));

	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(replacement);
	coordinator.tokenExchangeSucceeded(replacement);
	const auto staged = coordinator.discoverySucceeded(replacement, selection("new"));
	CHECK(staged.snapshot.state == YouTubeAccountState::PersistingCredential);
	CHECK(staged.snapshot.channelId == "channel-old");
	CHECK(staged.snapshot.streamId == "stream-old");

	const auto committed = coordinator.credentialStored(replacement);
	CHECK(committed.snapshot.state == YouTubeAccountState::Connected);
	CHECK(committed.snapshot.channelId == "channel-new");
	CHECK(committed.snapshot.streamId == "stream-new");
	CHECK(committed.snapshot.failure == YouTubeAccountFailure::None);
}

void testNoThrowCredentialCommitIsAttemptScoped()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(lease);
	coordinator.tokenExchangeSucceeded(lease);
	const auto staged = coordinator.discoverySucceeded(lease, selection("prepared"));
	const std::uint64_t stagedRevision = staged.snapshot.revision;

	CHECK(!coordinator.commitCredentialStored({lease.generation, lease.attempt + 1}));
	CHECK(coordinator.snapshot().state == YouTubeAccountState::PersistingCredential);
	CHECK(coordinator.commitCredentialStored(lease));
	const auto committed = coordinator.snapshot();
	CHECK(committed.state == YouTubeAccountState::Connected);
	CHECK(committed.channelId == "channel-prepared");
	CHECK(committed.streamId == "stream-prepared");
	CHECK(committed.connectionLease == lease);
	CHECK(committed.revision != stagedRevision);
	CHECK(!coordinator.commitCredentialStored(lease));
}

void testCredentialRollbackFailureFailsClosed()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(replacement);
	coordinator.tokenExchangeSucceeded(replacement);
	coordinator.discoverySucceeded(replacement, selection("new"));

	const auto failedClosed = coordinator.credentialStateUncertain(replacement);
	CHECK(failedClosed.changed);
	CHECK(failedClosed.snapshot.state == YouTubeAccountState::Unavailable);
	CHECK(failedClosed.snapshot.failure == YouTubeAccountFailure::CredentialUnavailable);
	CHECK(failedClosed.snapshot.channelId.empty());
	CHECK(failedClosed.snapshot.streamId.empty());
	CHECK(!failedClosed.snapshot.connectionLease.has_value());
	CHECK(failedClosed.snapshot.generation != replacement.generation);
	CHECK(!coordinator.commitCredentialStored(replacement));
}

void testInvalidReplacementPreservesCommittedConnection()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));

	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(replacement);
	coordinator.tokenExchangeSucceeded(replacement);
	const auto rejected = coordinator.discoverySucceeded(replacement, {});
	CHECK(rejected.snapshot.state == YouTubeAccountState::Connected);
	CHECK(rejected.snapshot.channelId == "channel-old");
	CHECK(rejected.snapshot.streamId == "stream-old");
	CHECK(rejected.snapshot.failure == YouTubeAccountFailure::InvalidResponse);
}

void testFailureClassificationAndExplicitReauthorization()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
	const auto unavailable = coordinator.attemptFailed(lease, YouTubeAccountFailure::CredentialUnavailable);
	CHECK(unavailable.snapshot.state == YouTubeAccountState::Unavailable);
	CHECK(unavailable.snapshot.failure == YouTubeAccountFailure::CredentialUnavailable);

	connect(coordinator, selection("connected"));
	const YouTubeAccountLease connectionLease = coordinator.snapshot().connectionLease.value();
	CHECK(coordinator.markUnavailable(connectionLease, YouTubeAccountFailure::CredentialUnavailable).changed);
	CHECK(!coordinator.markUnavailable(connectionLease, YouTubeAccountFailure::CredentialUnavailable).changed);
	const auto connectedReauth = coordinator.markNeedsReauthorization(connectionLease);
	CHECK(connectedReauth.snapshot.state == YouTubeAccountState::NeedsReauthorization);
	CHECK(connectedReauth.snapshot.channelId.empty());
	CHECK(connectedReauth.snapshot.streamId.empty());
}

void testOldConnectionResultsCannotReplaceNewConnection()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease oldConnection = coordinator.snapshot().connectionLease.value();

	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(replacement);
	coordinator.tokenExchangeSucceeded(replacement);
	coordinator.discoverySucceeded(replacement, selection("new"));
	coordinator.credentialStored(replacement);
	CHECK(coordinator.snapshot().channelId == "channel-new");
	CHECK(coordinator.snapshot().connectionLease == replacement);

	CHECK(!coordinator.markUnavailable(oldConnection, YouTubeAccountFailure::ServiceUnavailable).changed);
	CHECK(!coordinator.markNeedsReauthorization(oldConnection).changed);
	CHECK(coordinator.snapshot().state == YouTubeAccountState::Connected);
	CHECK(coordinator.snapshot().channelId == "channel-new");
	CHECK(coordinator.snapshot().failure == YouTubeAccountFailure::None);
}

void testInvalidatedOldConnectionIsNotRestoredAfterReplacementCancellation()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease oldConnection = coordinator.snapshot().connectionLease.value();
	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());

	CHECK(coordinator.markNeedsReauthorization(oldConnection).changed);
	const auto cancelled = coordinator.cancel(replacement);
	CHECK(cancelled.snapshot.state == YouTubeAccountState::NeedsReauthorization);
	CHECK(cancelled.snapshot.failure == YouTubeAccountFailure::ReauthorizationRequired);
	CHECK(cancelled.snapshot.channelId.empty());
	CHECK(!cancelled.snapshot.connectionLease.has_value());
}

void testInvalidatedOldConnectionIsNotRestoredAfterReplacementFailure()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease oldConnection = coordinator.snapshot().connectionLease.value();
	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());

	CHECK(coordinator.markNeedsReauthorization(oldConnection).changed);
	const auto failed = coordinator.attemptFailed(replacement, YouTubeAccountFailure::NetworkFailure);
	CHECK(failed.snapshot.state == YouTubeAccountState::NeedsReauthorization);
	CHECK(failed.snapshot.failure == YouTubeAccountFailure::ReauthorizationRequired);
	CHECK(failed.snapshot.channelId.empty());
	CHECK(!failed.snapshot.connectionLease.has_value());
}

void testInvalidatedOldConnectionDoesNotBlockSuccessfulReplacement()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease oldConnection = coordinator.snapshot().connectionLease.value();
	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	CHECK(coordinator.markNeedsReauthorization(oldConnection).changed);

	coordinator.authorizationCallbackAccepted(replacement);
	coordinator.tokenExchangeSucceeded(replacement);
	coordinator.discoverySucceeded(replacement, selection("new"));
	const auto connected = coordinator.credentialStored(replacement);
	CHECK(connected.snapshot.state == YouTubeAccountState::Connected);
	CHECK(connected.snapshot.channelId == "channel-new");
	CHECK(connected.snapshot.connectionLease == replacement);
	CHECK(connected.snapshot.failure == YouTubeAccountFailure::None);
}

void testUnavailableOldConnectionIsRestoredWithItsFailure()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease oldConnection = coordinator.snapshot().connectionLease.value();
	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	CHECK(coordinator.markUnavailable(oldConnection, YouTubeAccountFailure::CredentialUnavailable).changed);

	const auto cancelled = coordinator.cancel(replacement);
	CHECK(cancelled.snapshot.state == YouTubeAccountState::Connected);
	CHECK(cancelled.snapshot.channelId == "channel-old");
	CHECK(cancelled.snapshot.failure == YouTubeAccountFailure::CredentialUnavailable);
}

void testExistingConnectionFailureSurvivesReplacementCancellation()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const YouTubeAccountLease oldConnection = coordinator.snapshot().connectionLease.value();
	coordinator.markUnavailable(oldConnection, YouTubeAccountFailure::ServiceUnavailable);

	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	const auto cancelled = coordinator.cancel(replacement);
	CHECK(cancelled.snapshot.state == YouTubeAccountState::Connected);
	CHECK(cancelled.snapshot.channelId == "channel-old");
	CHECK(cancelled.snapshot.failure == YouTubeAccountFailure::ServiceUnavailable);
}

void testPersistingCredentialIsInvalidatedBeforeLateCommit()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
	coordinator.authorizationCallbackAccepted(lease);
	coordinator.tokenExchangeSucceeded(lease);
	coordinator.discoverySucceeded(lease, selection("new"));
	CHECK(coordinator.snapshot().state == YouTubeAccountState::PersistingCredential);

	coordinator.invalidateContext();
	CHECK(!coordinator.credentialStored(lease).changed);
	CHECK(coordinator.snapshot().state == YouTubeAccountState::Disconnected);
	CHECK(!coordinator.snapshot().connectionLease.has_value());
}

void testCancelledFailureMustUseExplicitCancelTransition()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease lease = leaseFrom(coordinator.beginAuthorization());
	CHECK(!coordinator.attemptFailed(lease, YouTubeAccountFailure::Cancelled).changed);
	CHECK(coordinator.snapshot().state == YouTubeAccountState::Authorizing);

	const auto cancelled = coordinator.cancel(lease);
	CHECK(cancelled.changed);
	CHECK(cancelled.snapshot.state == YouTubeAccountState::Disconnected);
	CHECK(cancelled.snapshot.failure == YouTubeAccountFailure::Cancelled);
}

void testContextInvalidationAndShutdown()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease oldLease = leaseFrom(coordinator.beginAuthorization());
	const auto invalidated = coordinator.invalidateContext();
	CHECK(invalidated.snapshot.state == YouTubeAccountState::Disconnected);
	CHECK(invalidated.snapshot.generation != oldLease.generation);
	CHECK(!coordinator.authorizationCallbackAccepted(oldLease).changed);

	const YouTubeAccountLease currentLease = leaseFrom(coordinator.beginAuthorization());
	const auto closed = coordinator.shutdown();
	CHECK(closed.snapshot.state == YouTubeAccountState::Closed);
	const std::uint64_t revision = closed.snapshot.revision;
	CHECK(!coordinator.shutdown().changed);
	CHECK(!coordinator.beginAuthorization().changed);
	CHECK(!coordinator.authorizationCallbackAccepted(currentLease).changed);
	CHECK(!coordinator.invalidateContext().changed);
	CHECK(coordinator.snapshot().revision == revision);
}

void testConnectedContextInvalidationClearsIdentity()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	const std::uint64_t oldGeneration = coordinator.snapshot().generation;

	const auto invalidated = coordinator.invalidateContext();
	CHECK(invalidated.snapshot.state == YouTubeAccountState::Disconnected);
	CHECK(invalidated.snapshot.generation != oldGeneration);
	CHECK(invalidated.snapshot.channelId.empty());
	CHECK(invalidated.snapshot.channelLabel.empty());
	CHECK(invalidated.snapshot.streamId.empty());
	CHECK(invalidated.snapshot.streamLabel.empty());
}

void testSavedConnectionRestoration()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountDiscovery saved = selection("saved");

	const auto restored = coordinator.restoreSavedConnection(saved, YouTubeAccountSavedCredentialState::Present);
	CHECK(restored.changed);
	CHECK(restored.snapshot.state == YouTubeAccountState::Configured);
	CHECK(restored.snapshot.failure == YouTubeAccountFailure::None);
	CHECK(restored.snapshot.channelId == saved.channelId);
	CHECK(restored.snapshot.channelLabel == saved.channelLabel);
	CHECK(restored.snapshot.streamId == saved.streamId);
	CHECK(restored.snapshot.streamLabel == saved.streamLabel);
	CHECK(!restored.snapshot.lease.has_value());
	CHECK(restored.snapshot.connectionLease.has_value());
	CHECK(restored.snapshot.connectionLease->generation == restored.snapshot.generation);
	CHECK(restored.snapshot.connectionLease->attempt != 0);

	const YouTubeAccountLease restoredLease = restored.snapshot.connectionLease.value_or(YouTubeAccountLease{});
	CHECK(coordinator.markUnavailable(restoredLease, YouTubeAccountFailure::ServiceUnavailable).changed);
	CHECK(coordinator.snapshot().state == YouTubeAccountState::Configured);
	CHECK(coordinator.snapshot().failure == YouTubeAccountFailure::ServiceUnavailable);
	CHECK(coordinator.markNeedsReauthorization(restoredLease).changed);
	CHECK(coordinator.snapshot().state == YouTubeAccountState::NeedsReauthorization);
	CHECK(!coordinator.snapshot().connectionLease.has_value());
}

void testSavedConnectionCredentialStatesRetainNonSecretSelection()
{
	for (const auto credentialState :
	     {YouTubeAccountSavedCredentialState::Missing, YouTubeAccountSavedCredentialState::Unavailable}) {
		YouTubeAccountCoordinator coordinator;
		const YouTubeAccountDiscovery saved = selection("saved");
		const auto restored = coordinator.restoreSavedConnection(saved, credentialState);
		CHECK(restored.changed);
		CHECK(restored.snapshot.state == (credentialState == YouTubeAccountSavedCredentialState::Missing
							  ? YouTubeAccountState::NeedsReauthorization
							  : YouTubeAccountState::Unavailable));
		CHECK(restored.snapshot.failure == (credentialState == YouTubeAccountSavedCredentialState::Missing
							    ? YouTubeAccountFailure::ReauthorizationRequired
							    : YouTubeAccountFailure::CredentialUnavailable));
		CHECK(restored.snapshot.channelId == saved.channelId);
		CHECK(restored.snapshot.channelLabel == saved.channelLabel);
		CHECK(restored.snapshot.streamId == saved.streamId);
		CHECK(restored.snapshot.streamLabel == saved.streamLabel);
		CHECK(!restored.snapshot.lease.has_value());
		CHECK(!restored.snapshot.connectionLease.has_value());

		const auto reconnect = coordinator.beginAuthorization();
		CHECK(reconnect.changed);
		CHECK(reconnect.snapshot.state == YouTubeAccountState::Authorizing);
		CHECK(reconnect.snapshot.channelId.empty());
		CHECK(reconnect.snapshot.streamId.empty());
	}
}

void testSavedConnectionRestoreInvalidatesPriorContext()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountLease oldAttempt = leaseFrom(coordinator.beginAuthorization());
	const std::uint64_t oldGeneration = oldAttempt.generation;

	const auto first =
		coordinator.restoreSavedConnection(selection("first"), YouTubeAccountSavedCredentialState::Present);
	CHECK(first.snapshot.generation != oldGeneration);
	CHECK(!coordinator.authorizationCallbackAccepted(oldAttempt).changed);
	const YouTubeAccountLease firstConnection = first.snapshot.connectionLease.value_or(YouTubeAccountLease{});

	const auto second =
		coordinator.restoreSavedConnection(selection("second"), YouTubeAccountSavedCredentialState::Present);
	CHECK(second.snapshot.generation != first.snapshot.generation);
	CHECK(second.snapshot.channelId == "channel-second");
	CHECK(!coordinator.markNeedsReauthorization(firstConnection).changed);
	CHECK(coordinator.snapshot().state == YouTubeAccountState::Configured);
	CHECK(coordinator.snapshot().channelId == "channel-second");
}

void testConfiguredReplacementPreservesSavedConnectionUntilCommit()
{
	YouTubeAccountCoordinator coordinator;
	const YouTubeAccountDiscovery saved = selection("saved");
	const auto restored = coordinator.restoreSavedConnection(saved, YouTubeAccountSavedCredentialState::Present);
	const YouTubeAccountLease savedLease = restored.snapshot.connectionLease.value_or(YouTubeAccountLease{});

	const YouTubeAccountLease cancelledAttempt = leaseFrom(coordinator.beginAuthorization());
	const auto cancelled = coordinator.cancel(cancelledAttempt);
	CHECK(cancelled.snapshot.state == YouTubeAccountState::Configured);
	CHECK(cancelled.snapshot.channelId == saved.channelId);
	CHECK(cancelled.snapshot.streamId == saved.streamId);
	CHECK(cancelled.snapshot.connectionLease == savedLease);

	const YouTubeAccountLease failedAttempt = leaseFrom(coordinator.beginAuthorization());
	const auto failed = coordinator.attemptFailed(failedAttempt, YouTubeAccountFailure::NetworkFailure);
	CHECK(failed.snapshot.state == YouTubeAccountState::Configured);
	CHECK(failed.snapshot.channelId == saved.channelId);
	CHECK(failed.snapshot.streamId == saved.streamId);
	CHECK(failed.snapshot.connectionLease == savedLease);

	const YouTubeAccountLease replacement = leaseFrom(coordinator.beginAuthorization());
	CHECK(coordinator.authorizationCallbackAccepted(replacement).changed);
	CHECK(coordinator.tokenExchangeSucceeded(replacement).changed);
	const YouTubeAccountDiscovery updated = selection("updated");
	CHECK(coordinator.discoverySucceeded(replacement, updated).changed);
	const auto connected = coordinator.credentialStored(replacement);
	CHECK(connected.snapshot.state == YouTubeAccountState::Connected);
	CHECK(connected.snapshot.channelId == updated.channelId);
	CHECK(connected.snapshot.streamId == updated.streamId);
	CHECK(connected.snapshot.connectionLease == replacement);
}

void testInvalidSavedConnectionFailsClosed()
{
	YouTubeAccountCoordinator coordinator;
	connect(coordinator, selection("old"));
	YouTubeAccountDiscovery invalid = selection("invalid");
	invalid.streamId.clear();

	const auto rejected = coordinator.restoreSavedConnection(invalid, YouTubeAccountSavedCredentialState::Present);
	CHECK(rejected.changed);
	CHECK(rejected.snapshot.state == YouTubeAccountState::Failed);
	CHECK(rejected.snapshot.failure == YouTubeAccountFailure::InvalidResponse);
	CHECK(rejected.snapshot.channelId.empty());
	CHECK(rejected.snapshot.streamId.empty());
	CHECK(!rejected.snapshot.connectionLease.has_value());

	const auto invalidState = coordinator.restoreSavedConnection(
		selection("valid"), static_cast<YouTubeAccountSavedCredentialState>(99));
	CHECK(invalidState.changed);
	CHECK(invalidState.snapshot.state == YouTubeAccountState::Failed);
	CHECK(invalidState.snapshot.failure == YouTubeAccountFailure::InvalidResponse);
}

} // namespace

int main()
{
	testLifecycleAndSecretFreeSnapshot();
	testStaleAndDuplicateEventsAreNoOps();
	testInvalidDiscoveryCannotConnect();
	testDiscoveryValuesAreBoundedAndSafeForSnapshots();
	testSharedSelectionValidationClassifiesEachField();
	testReplacementIsTransactional();
	testReplacementCommitsOnlyAfterCredentialStorage();
	testNoThrowCredentialCommitIsAttemptScoped();
	testCredentialRollbackFailureFailsClosed();
	testInvalidReplacementPreservesCommittedConnection();
	testFailureClassificationAndExplicitReauthorization();
	testOldConnectionResultsCannotReplaceNewConnection();
	testInvalidatedOldConnectionIsNotRestoredAfterReplacementCancellation();
	testInvalidatedOldConnectionIsNotRestoredAfterReplacementFailure();
	testInvalidatedOldConnectionDoesNotBlockSuccessfulReplacement();
	testUnavailableOldConnectionIsRestoredWithItsFailure();
	testExistingConnectionFailureSurvivesReplacementCancellation();
	testPersistingCredentialIsInvalidatedBeforeLateCommit();
	testCancelledFailureMustUseExplicitCancelTransition();
	testContextInvalidationAndShutdown();
	testConnectedContextInvalidationClearsIdentity();
	testSavedConnectionRestoration();
	testSavedConnectionCredentialStatesRetainNonSecretSelection();
	testSavedConnectionRestoreInvalidatesPriorContext();
	testConfiguredReplacementPreservesSavedConnectionUntilCommit();
	testInvalidSavedConnectionFailsClosed();

	if (failures != 0) {
		std::cerr << failures << " youtube-account-coordinator test(s) failed\n";
		return 1;
	}
	std::cout << "All youtube-account-coordinator tests passed\n";
	return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "credential-vault.hpp"
#include "google-oauth-authorization-session.hpp"
#include "google-oauth-token-transport.hpp"
#include "youtube-account-coordinator.hpp"
#include "youtube-api-discovery-pager.hpp"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace easy_multistream {

// These narrow ports keep orchestration independently testable. Production
// adapters wrap the fixed-origin OAuth/API implementations; only the private
// test constructor can replace them. Every completion must run on the
// provider's owner thread and must not be invoked synchronously from start,
// cancel, or shutdown. The production adapters satisfy this with queued
// QObject delivery. This non-reentrant contract is part of the port boundary,
// not merely an implementation detail.
class YouTubeAccountAuthorizationPort {
public:
	using CompletionHandler = std::function<void(GoogleOAuthAuthorizationCompletion)>;
	virtual ~YouTubeAccountAuthorizationPort() = default;
	virtual GoogleOAuthAuthorizationStartStatus start(GoogleOAuthLoopbackAttempt attempt, const QString &clientId,
							  GoogleOAuthConsentMode consentMode,
							  CompletionHandler completionHandler) noexcept = 0;
	virtual bool cancel(GoogleOAuthLoopbackAttempt attempt) noexcept = 0;
	virtual bool shutdown() noexcept = 0;
};

class YouTubeAccountTokenPort {
public:
	using CompletionHandler = std::function<void(GoogleOAuthTokenCompletion)>;
	virtual ~YouTubeAccountTokenPort() = default;
	virtual GoogleOAuthTokenStartStatus startExchange(GoogleOAuthTokenExchangeRequest request,
							  CompletionHandler completionHandler) noexcept = 0;
	virtual bool cancel(GoogleOAuthTokenAttempt attempt) noexcept = 0;
	virtual bool shutdown() noexcept = 0;
};

class YouTubeAccountDiscoveryPort {
public:
	using CompletionHandler = std::function<void(YouTubeApiPagedCompletion)>;
	virtual ~YouTubeAccountDiscoveryPort() = default;
	virtual YouTubeApiPagerStartStatus startListAllChannels(YouTubeListAllChannelsRequest request,
								CompletionHandler completionHandler) noexcept = 0;
	virtual YouTubeApiPagerStartStatus startListAllStreams(YouTubeListAllStreamsRequest request,
							       CompletionHandler completionHandler) noexcept = 0;
	virtual bool cancel(YouTubeApiAttempt attempt) noexcept = 0;
	virtual bool shutdown() noexcept = 0;
};

enum class YouTubeAccountProviderStage {
	Idle,
	Authorizing,
	ExchangingCode,
	ListingChannels,
	AwaitingChannelSelection,
	ListingStreams,
	AwaitingStreamSelection,
	PersistingCredential,
	// Local selection and credential are present but have not been validated
	// together with Google during this process lifetime.
	Configured,
	Connected,
	NeedsReauthorization,
	Unavailable,
	Failed,
	Closed,
};

enum class YouTubeAccountProviderStartStatus {
	Started,
	WrongThread,
	Busy,
	Closed,
	InvalidClientId,
	InvalidProfileBinding,
	InvalidSelectionCommitter,
	OperationFailed,
};

enum class YouTubeAccountProviderRestoreStatus {
	Configured,
	SetupRequired,
	ReauthorizationRequired,
	CredentialUnavailable,
	WrongThread,
	Busy,
	Closed,
	InvalidProfileBinding,
	InvalidSelection,
	OperationFailed,
};

enum class YouTubeAccountProviderSelectionStatus {
	Accepted,
	WrongThread,
	Closed,
	StaleLease,
	WrongStage,
	UnknownCandidate,
	OperationFailed,
};

struct YouTubeAccountProviderSnapshot final {
	std::uint64_t revision = 0;
	YouTubeAccountProviderStage stage = YouTubeAccountProviderStage::Idle;
	YouTubeAccountSnapshot account;
	std::vector<YouTubeOwnedChannel> channels;
	std::vector<YouTubeReusableStream> streams;
	std::optional<std::string> selectedChannelId;
	std::optional<std::string> selectedStreamId;
};

// The committer synchronously safe-saves the non-secret selection to the
// active OBS profile. It must not process events or re-enter the provider, and
// a false result must leave the prior profile value intact. The existing
// settings writer already provides that rollback contract.
using YouTubeAccountSelectionCommitter = std::function<bool(const std::optional<YouTubeAccountSelection> &selection)>;

// Headless, owner-thread-only orchestration for one interactive account
// connection. It is deliberately detached from PluginState, docks, runtime
// output, and the production OBS module until those layers have their own
// reviewed integration slices.
class YouTubeAccountProvider final : public QObject {
public:
	// profileBinding is a validated, non-secret identity for the active OBS
	// profile. It is kept in memory only and combined with the selected channel
	// for every credential-store operation.
	YouTubeAccountProvider(QString clientId, GoogleOAuthAuthorizationSession::BrowserOpener browserOpener,
			       YouTubeAccountRefreshTokenStore &refreshTokenStore, std::string profileBinding,
			       YouTubeAccountSelectionCommitter selectionCommitter, QObject *parent = nullptr);
	~YouTubeAccountProvider() override;

	YouTubeAccountProvider(const YouTubeAccountProvider &) = delete;
	YouTubeAccountProvider &operator=(const YouTubeAccountProvider &) = delete;
	YouTubeAccountProvider(YouTubeAccountProvider &&) = delete;
	YouTubeAccountProvider &operator=(YouTubeAccountProvider &&) = delete;

	YouTubeAccountProviderStartStatus
	startConnection(GoogleOAuthConsentMode consentMode = GoogleOAuthConsentMode::Standard) noexcept;
	YouTubeAccountProviderSelectionStatus selectChannel(YouTubeAccountLease lease,
							    std::string_view channelId) noexcept;
	YouTubeAccountProviderSelectionStatus selectStream(YouTubeAccountLease lease,
							   std::string_view streamId) noexcept;
	// Restore profile-scoped, non-secret selection state without opening a
	// browser or starting any network port. A missing selection is a valid
	// setup-required account state and does not inspect or delete any credential.
	// Credential presence produces only Configured state;
	// it does not prove that the token belongs to the saved channel. Call
	// invalidateContext() during PROFILE_CHANGING so an interactive attempt
	// cannot cross into the newly loaded profile.
	YouTubeAccountProviderRestoreStatus
	restoreSavedState(std::string profileBinding, std::optional<YouTubeAccountSelection> selection) noexcept;

	bool cancel(YouTubeAccountLease lease) noexcept;
	bool invalidateContext() noexcept;
	bool shutdown() noexcept;

	YouTubeAccountProviderSnapshot snapshot() const;

private:
	using QObject::moveToThread;
	friend class YouTubeAccountProviderTestAccess;

	YouTubeAccountProvider(QString clientId, std::unique_ptr<YouTubeAccountAuthorizationPort> authorizationPort,
			       std::unique_ptr<YouTubeAccountTokenPort> tokenPort,
			       std::unique_ptr<YouTubeAccountDiscoveryPort> discoveryPort,
			       YouTubeAccountRefreshTokenStore &refreshTokenStore, std::string profileBinding,
			       YouTubeAccountSelectionCommitter selectionCommitter, QObject *parent);

	class Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace easy_multistream

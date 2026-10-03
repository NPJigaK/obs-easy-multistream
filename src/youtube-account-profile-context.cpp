// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-profile-context.hpp"

#include "windows-credential-vault.hpp"

#include <limits>
#include <utility>

namespace easy_multistream {

YouTubeAccountProfileContext::YouTubeAccountProfileContext(ProfilePathReader profilePathReader,
							   ConfigReader configReader)
	: profilePathReader_(std::move(profilePathReader)),
	  configReader_(std::move(configReader))
{
}

std::uint64_t YouTubeAccountProfileContext::nextGeneration(std::uint64_t value) noexcept
{
	return value == std::numeric_limits<std::uint64_t>::max() ? 1 : value + 1;
}

YouTubeAccountProfileContext::LoadStatus YouTubeAccountProfileContext::mapLoadStatus(SettingsLoadStatus status) noexcept
{
	switch (status) {
	case SettingsLoadStatus::Loaded:
		return LoadStatus::Loaded;
	case SettingsLoadStatus::Defaults:
		return LoadStatus::Defaults;
	case SettingsLoadStatus::SetupRequired:
		return LoadStatus::SetupRequired;
	case SettingsLoadStatus::InvalidSchema:
		return LoadStatus::InvalidSettings;
	case SettingsLoadStatus::UnsupportedFutureSchema:
		return LoadStatus::UnsupportedFutureSettings;
	case SettingsLoadStatus::Unavailable:
		return LoadStatus::Unavailable;
	}
	return LoadStatus::Unavailable;
}

YouTubeAccountProfileContext::CommitStatus
YouTubeAccountProfileContext::mapCommitStatus(SettingsLoadStatus status) noexcept
{
	switch (status) {
	case SettingsLoadStatus::InvalidSchema:
		return CommitStatus::InvalidSettings;
	case SettingsLoadStatus::UnsupportedFutureSchema:
		return CommitStatus::UnsupportedFutureSettings;
	case SettingsLoadStatus::Unavailable:
		return CommitStatus::Unavailable;
	case SettingsLoadStatus::Loaded:
	case SettingsLoadStatus::Defaults:
	case SettingsLoadStatus::SetupRequired:
		break;
	}
	return CommitStatus::Unavailable;
}

YouTubeAccountProfileContext::Snapshot YouTubeAccountProfileContext::snapshotFor(std::uint64_t generation,
										 std::string profileBinding,
										 const SettingsLoadResult &settings)
{
	Snapshot snapshot;
	snapshot.generation = generation;
	snapshot.profileBinding = std::move(profileBinding);
	snapshot.connectionMode = settings.settings.youtubeConnectionMode;
	if (snapshot.connectionMode == YouTubeConnectionMode::Account) {
		snapshot.selection = settings.settings.youtubeAccountSelection;
	}
	snapshot.settingsStatus = settings.status;
	return snapshot;
}

std::optional<std::string> YouTubeAccountProfileContext::readProfileBinding() const
{
	if (!profilePathReader_) {
		return std::nullopt;
	}

	std::string profilePath = profilePathReader_();
	return makeYouTubeAccountProfileBinding(profilePath);
}

std::optional<std::string> YouTubeAccountProfileContext::currentProfileBinding() const noexcept
{
	try {
		return readProfileBinding();
	} catch (...) {
		return std::nullopt;
	}
}

YouTubeAccountProfileContext::LoadResult YouTubeAccountProfileContext::load()
{
	const std::uint64_t generation = nextGeneration(snapshot_.generation);
	Snapshot next;
	next.generation = generation;

	try {
		if (!profilePathReader_ || !configReader_) {
			snapshot_ = std::move(next);
			return {LoadStatus::Unavailable, snapshot_};
		}

		const auto profileBinding = readProfileBinding();
		if (!profileBinding.has_value()) {
			snapshot_ = std::move(next);
			return {LoadStatus::ProfileUnavailable, snapshot_};
		}

		config_t *config = configReader_();
		if (config == nullptr) {
			next.profileBinding = *profileBinding;
			snapshot_ = std::move(next);
			return {LoadStatus::Unavailable, snapshot_};
		}

		const SettingsLoadResult settings = loadProfileSettings(config);
		next = snapshotFor(generation, *profileBinding, settings);
		snapshot_ = next;
		return {mapLoadStatus(settings.status), std::move(next)};
	} catch (...) {
		snapshot_ = std::move(next);
		return {LoadStatus::Unavailable, snapshot_};
	}
}

void YouTubeAccountProfileContext::invalidate() noexcept
{
	Snapshot next;
	next.generation = nextGeneration(snapshot_.generation);
	snapshot_ = std::move(next);
}

YouTubeAccountProfileContext::Snapshot YouTubeAccountProfileContext::snapshot() const
{
	return snapshot_;
}

YouTubeAccountProfileContext::CommitResult
YouTubeAccountProfileContext::commitSelection(std::uint64_t expectedGeneration, std::string_view expectedProfileBinding,
					      std::optional<YouTubeAccountSelection> selection)
{
	const auto staleResult = [this]() {
		return CommitResult{CommitStatus::Stale, snapshot_};
	};
	if (expectedGeneration == 0 || expectedGeneration != snapshot_.generation || expectedProfileBinding.empty() ||
	    expectedProfileBinding != snapshot_.profileBinding ||
	    !isValidYouTubeAccountProfileBinding(expectedProfileBinding)) {
		return staleResult();
	}
	if (selection.has_value() &&
	    validateYouTubeAccountSelection(*selection) != YouTubeAccountSelectionValidationError::None) {
		return {CommitStatus::InvalidSelection, snapshot_};
	}

	try {
		if (!profilePathReader_ || !configReader_) {
			return {CommitStatus::Unavailable, snapshot_};
		}

		const auto currentBinding = readProfileBinding();
		if (!currentBinding.has_value() || *currentBinding != expectedProfileBinding) {
			invalidate();
			return staleResult();
		}

		config_t *config = configReader_();
		if (config == nullptr) {
			return {CommitStatus::Unavailable, snapshot_};
		}

		const SettingsLoadResult loaded = loadProfileSettings(config);
		if (loaded.status != SettingsLoadStatus::Loaded && loaded.status != SettingsLoadStatus::Defaults &&
		    loaded.status != SettingsLoadStatus::SetupRequired) {
			return {mapCommitStatus(loaded.status), snapshot_};
		}
		if (loaded.settings.youtubeConnectionMode != YouTubeConnectionMode::Account) {
			return {CommitStatus::NotAccountMode, snapshot_};
		}

		Settings desired = loaded.settings;
		desired.youtubeConnectionMode = YouTubeConnectionMode::Account;
		desired.youtubeAccountSelection = std::move(selection);
		if (saveProfileSettings(config, desired) != CONFIG_SUCCESS) {
			return {CommitStatus::SaveFailed, snapshot_};
		}

		Snapshot next = snapshot_;
		next.connectionMode = YouTubeConnectionMode::Account;
		next.selection = std::move(desired.youtubeAccountSelection);
		next.settingsStatus = next.selection.has_value() ? SettingsLoadStatus::Loaded
								 : SettingsLoadStatus::SetupRequired;
		snapshot_ = std::move(next);
		return {CommitStatus::Committed, snapshot_};
	} catch (...) {
		return {CommitStatus::Unavailable, snapshot_};
	}
}

} // namespace easy_multistream

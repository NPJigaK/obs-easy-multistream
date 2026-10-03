// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "settings.hpp"

#include <util/config-file.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace easy_multistream {

// This boundary is owned by the OBS frontend thread. Both readers must observe
// the same stable active profile and must not process events or re-enter profile
// switching. They are invoked synchronously and their results are copied
// immediately; the context never retains an OBS config pointer or a borrowed
// profile-path buffer.
class YouTubeAccountProfileContext final {
public:
	using ProfilePathReader = std::function<std::string()>;
	using ConfigReader = std::function<config_t *()>;

	enum class LoadStatus {
		Loaded,
		Defaults,
		SetupRequired,
		ProfileUnavailable,
		InvalidSettings,
		UnsupportedFutureSettings,
		Unavailable,
	};

	struct Snapshot final {
		std::uint64_t generation = 0;
		std::string profileBinding;
		YouTubeConnectionMode connectionMode = YouTubeConnectionMode::Manual;
		std::optional<YouTubeAccountSelection> selection;
		SettingsLoadStatus settingsStatus = SettingsLoadStatus::Unavailable;
	};

	struct LoadResult final {
		LoadStatus status = LoadStatus::Unavailable;
		Snapshot snapshot;
	};

	enum class CommitStatus {
		Committed,
		Stale,
		InvalidSelection,
		NotAccountMode,
		ProfileUnavailable,
		InvalidSettings,
		UnsupportedFutureSettings,
		Unavailable,
		SaveFailed,
	};

	struct CommitResult final {
		CommitStatus status = CommitStatus::Unavailable;
		Snapshot snapshot;
	};

	YouTubeAccountProfileContext(ProfilePathReader profilePathReader, ConfigReader configReader);

	YouTubeAccountProfileContext(const YouTubeAccountProfileContext &) = delete;
	YouTubeAccountProfileContext &operator=(const YouTubeAccountProfileContext &) = delete;

	// Reload the active profile. This always advances generation, including
	// when OBS cannot provide a usable path or config, so work from the previous
	// profile cannot commit into the new context.
	LoadResult load();

	// Read only the active profile binding. This is a side-effect-free
	// discovery operation: it never reads the OBS config, changes the snapshot,
	// or advances the generation. The returned binding is a value copy, so no
	// profile-path buffer is retained after the call.
	std::optional<std::string> currentProfileBinding() const noexcept;

	// Invalidate all profile-bound work. The generation is advanced even when
	// already invalid so a stale callback can never become current again.
	void invalidate() noexcept;

	Snapshot snapshot() const;

	// Commit only a validated, non-secret account selection. The expected
	// generation and binding must still describe the active snapshot. The
	// profile path and config are read again before saving to close the gap
	// between a discovery result and this commit.
	CommitResult commitSelection(std::uint64_t expectedGeneration, std::string_view expectedProfileBinding,
				     std::optional<YouTubeAccountSelection> selection);

private:
	static std::uint64_t nextGeneration(std::uint64_t value) noexcept;
	static LoadStatus mapLoadStatus(SettingsLoadStatus status) noexcept;
	static CommitStatus mapCommitStatus(SettingsLoadStatus status) noexcept;
	static Snapshot snapshotFor(std::uint64_t generation, std::string profileBinding,
				    const SettingsLoadResult &settings);
	std::optional<std::string> readProfileBinding() const;

	ProfilePathReader profilePathReader_;
	ConfigReader configReader_;
	Snapshot snapshot_;
};

} // namespace easy_multistream

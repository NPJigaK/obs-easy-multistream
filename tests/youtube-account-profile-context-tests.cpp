// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-profile-context.hpp"
#include "windows-credential-vault.hpp"

#include <util/config-file.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

int failures = 0;

#define CHECK(expression)                                                                                              \
	do {                                                                                                             \
		if (!(expression)) {                                                                                       \
			std::cerr << __FILE__ << ':' << __LINE__ << ": check failed: " #expression << '\n';                \
			++failures;                                                                                             \
		}                                                                                                          \
	} while (false)

using easy_multistream::YouTubeAccountProfileContext;
using easy_multistream::YouTubeAccountSelection;
using easy_multistream::YouTubeConnectionMode;

constexpr std::string_view kProfileA = "C:/obs/profiles/alpha";
constexpr std::string_view kProfileB = "C:/obs/profiles/beta";

YouTubeAccountSelection selectionA()
{
	return {"UC-alpha", "Alpha channel", "stream-alpha", "Alpha stream"};
}

YouTubeAccountSelection selectionB()
{
	return {"UC-beta", "Beta channel", "stream-beta", "Beta stream"};
}

bool sameSelection(const std::optional<YouTubeAccountSelection> &left,
		   const std::optional<YouTubeAccountSelection> &right)
{
	if (left.has_value() != right.has_value()) {
		return false;
	}
	if (!left.has_value()) {
		return true;
	}
	return left->channelId == right->channelId && left->channelLabel == right->channelLabel &&
	       left->streamId == right->streamId && left->streamLabel == right->streamLabel;
}

class ConfigHandle final {
public:
	ConfigHandle() = default;

	~ConfigHandle()
	{
		if (config_ != nullptr) {
			config_close(config_);
		}
	}

	ConfigHandle(const ConfigHandle &) = delete;
	ConfigHandle &operator=(const ConfigHandle &) = delete;

	bool open(const std::filesystem::path &path)
	{
		path_ = path;
		const std::string utf8Path = path.string();
		return config_open(&config_, utf8Path.c_str(), CONFIG_OPEN_ALWAYS) == CONFIG_SUCCESS &&
		       config_ != nullptr;
	}

	config_t *get() const noexcept { return config_; }

	void save()
	{
		CHECK(config_ != nullptr);
		if (config_ != nullptr) {
			CHECK(config_save_safe(config_, "tmp", nullptr) == CONFIG_SUCCESS);
		}
	}

	const std::filesystem::path &path() const noexcept { return path_; }

private:
	config_t *config_ = nullptr;
	std::filesystem::path path_;
};

std::filesystem::path testPath(const char *name)
{
	return std::filesystem::current_path() / name;
}

void removeTestPath(const std::filesystem::path &path)
{
	std::error_code error;
	std::filesystem::remove(path, error);
	std::filesystem::remove(path.string() + ".tmp", error);
}

void setAccountSettings(config_t *config, bool withSelection = false)
{
	CHECK(config != nullptr);
	if (config == nullptr) {
		return;
	}
	easy_multistream::Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = YouTubeConnectionMode::Account;
	if (withSelection) {
		settings.youtubeAccountSelection = selectionA();
	}
	easy_multistream::writeProfileSettings(config, settings);
}

void setManualSettings(config_t *config)
{
	CHECK(config != nullptr);
	if (config == nullptr) {
		return;
	}
	easy_multistream::Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = YouTubeConnectionMode::Manual;
	settings.youtubeServerUrl = "rtmps://a.rtmps.youtube.com/live2";
	easy_multistream::writeProfileSettings(config, settings);
}

void testLoadCopiesValueOnlyProfileContext()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-load.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get(), true);
	config.save();

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });

	const auto loaded = context.load();
	const auto binding = easy_multistream::makeYouTubeAccountProfileBinding(currentPath);
	CHECK(loaded.status == YouTubeAccountProfileContext::LoadStatus::Loaded);
	CHECK(loaded.snapshot.generation == 1);
	CHECK(binding.has_value());
	if (binding.has_value()) {
		CHECK(loaded.snapshot.profileBinding == *binding);
	}
	CHECK(loaded.snapshot.connectionMode == YouTubeConnectionMode::Account);
	CHECK(loaded.snapshot.selection.has_value());
	if (loaded.snapshot.selection.has_value()) {
		CHECK(loaded.snapshot.selection->channelId == "UC-alpha");
	}

	currentPath = std::string(kProfileB);
	CHECK(loaded.snapshot.profileBinding != currentPath);
	CHECK(context.snapshot().profileBinding == loaded.snapshot.profileBinding);
	removeTestPath(path);
}

void testCurrentProfileBindingIsSideEffectFreeAndBounded()
{
	int configReads = 0;
	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&configReads]() {
						     ++configReads;
						     throw std::runtime_error("config reader must not be called");
						     return static_cast<config_t *>(nullptr);
					     });

	const auto before = context.snapshot();
	const auto bindingA = context.currentProfileBinding();
	const auto expectedA = easy_multistream::makeYouTubeAccountProfileBinding(kProfileA);
	CHECK(bindingA.has_value());
	CHECK(expectedA.has_value());
	if (bindingA.has_value() && expectedA.has_value()) {
		CHECK(*bindingA == *expectedA);
	}
	CHECK(configReads == 0);
	CHECK(context.snapshot().generation == before.generation);
	CHECK(context.snapshot().profileBinding == before.profileBinding);

	currentPath = std::string(kProfileB);
	const auto bindingB = context.currentProfileBinding();
	const auto expectedB = easy_multistream::makeYouTubeAccountProfileBinding(kProfileB);
	CHECK(bindingB.has_value());
	CHECK(expectedB.has_value());
	if (bindingB.has_value() && expectedB.has_value()) {
		CHECK(*bindingB == *expectedB);
	}
	CHECK(bindingA != bindingB);
	CHECK(configReads == 0);
	CHECK(context.snapshot().generation == before.generation);
	CHECK(context.snapshot().profileBinding == before.profileBinding);
}

void testCurrentProfileBindingRejectsMissingThrowingAndInvalidReaders()
{
	YouTubeAccountProfileContext missing({}, []() { return static_cast<config_t *>(nullptr); });
	CHECK(!missing.currentProfileBinding().has_value());

	YouTubeAccountProfileContext throwing(
		[]() -> std::string { throw std::runtime_error("profile reader failed"); },
		[]() { return static_cast<config_t *>(nullptr); });
	CHECK(!throwing.currentProfileBinding().has_value());

	for (const std::string path : {std::string(), std::string("profile\0suffix", 14),
				       std::string(easy_multistream::kMaxYouTubeAccountProfilePathBytes + 1, 'p')}) {
		YouTubeAccountProfileContext invalid([&path]() { return path; },
						     []() { return static_cast<config_t *>(nullptr); });
		CHECK(!invalid.currentProfileBinding().has_value());
	}
}

void testActiveAccountCheckIsFreshAndDoesNotAdvanceGeneration()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-active-check.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get());
	config.save();

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });
	const auto loaded = context.load();
	const auto before = context.snapshot();
	CHECK(context.checkActiveAccount(loaded.snapshot.generation, loaded.snapshot.profileBinding) ==
	      YouTubeAccountProfileContext::ActiveAccountStatus::Current);
	CHECK(context.snapshot().generation == before.generation);
	CHECK(context.snapshot().profileBinding == before.profileBinding);

	// The preflight rereads the config rather than trusting the last load.
	setManualSettings(config.get());
	CHECK(context.checkActiveAccount(loaded.snapshot.generation, loaded.snapshot.profileBinding) ==
	      YouTubeAccountProfileContext::ActiveAccountStatus::NotAccountMode);
	CHECK(context.snapshot().generation == before.generation);

	setAccountSettings(config.get());
	currentPath = std::string(kProfileB);
	CHECK(context.checkActiveAccount(loaded.snapshot.generation, loaded.snapshot.profileBinding) ==
	      YouTubeAccountProfileContext::ActiveAccountStatus::Stale);
	CHECK(context.snapshot().generation == before.generation);
	CHECK(context.snapshot().profileBinding == before.profileBinding);
	removeTestPath(path);
}

void testGenerationInvalidationPreventsStaleCommit()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-generation.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get());
	config.save();

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });
	const auto loaded = context.load();
	const auto before = context.snapshot();
	context.invalidate();
	CHECK(context.snapshot().generation == before.generation + 1);
	CHECK(context.snapshot().profileBinding.empty());
	const auto rejected =
		context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding, selectionA());
	CHECK(rejected.status == YouTubeAccountProfileContext::CommitStatus::Stale);
	CHECK(context.snapshot().selection == std::nullopt);
	removeTestPath(path);
}

void testCommitSelectionRequiresFreshProfileAndAccountMode()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-commit.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get());
	config.save();

	std::string currentPath(kProfileA);
	int configReads = 0;
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config, &configReads]() {
						     ++configReads;
						     return config.get();
					     });
	const auto loaded = context.load();
	const int readsAfterLoad = configReads;
	const auto committed =
		context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding, selectionB());
	CHECK(committed.status == YouTubeAccountProfileContext::CommitStatus::Committed);
	CHECK(configReads > readsAfterLoad);
	CHECK(committed.snapshot.connectionMode == YouTubeConnectionMode::Account);
	CHECK(committed.snapshot.selection.has_value());
	if (committed.snapshot.selection.has_value()) {
		CHECK(committed.snapshot.selection->channelId == "UC-beta");
	}
	const auto persisted = easy_multistream::loadProfileSettings(config.get());
	CHECK(persisted.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(persisted.settings.youtubeConnectionMode == YouTubeConnectionMode::Account);
	CHECK(persisted.settings.youtubeAccountSelection.has_value());
	if (persisted.settings.youtubeAccountSelection.has_value()) {
		CHECK(persisted.settings.youtubeAccountSelection->streamId == "stream-beta");
	}

	// A manual profile is not silently converted by an account-selection
	// callback. This preserves the user's existing manual path until the UI
	// explicitly switches the profile to account mode.
	setManualSettings(config.get());
	config.save();
	const auto manualLoad = context.load();
	const auto manualCommit = context.commitSelection(manualLoad.snapshot.generation,
							  manualLoad.snapshot.profileBinding, selectionB());
	CHECK(manualCommit.status == YouTubeAccountProfileContext::CommitStatus::NotAccountMode);
	CHECK(context.snapshot().connectionMode == YouTubeConnectionMode::Manual);
	removeTestPath(path);
}

void testManualProfileDoesNotExposePreservedAccountSelection()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-manual-selection.ini");
	removeTestPath(path);
	CHECK(config.open(path));

	easy_multistream::Settings settings;
	settings.youtubeEnabled = true;
	settings.youtubeConnectionMode = YouTubeConnectionMode::Manual;
	settings.youtubeServerUrl = "rtmps://a.rtmps.youtube.com/live2";
	settings.youtubeAccountSelection = selectionA();
	easy_multistream::writeProfileSettings(config.get(), settings);
	config.save();

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });
	const auto loaded = context.load();
	CHECK(loaded.status == YouTubeAccountProfileContext::LoadStatus::Loaded);
	CHECK(loaded.snapshot.connectionMode == YouTubeConnectionMode::Manual);
	CHECK(!loaded.snapshot.selection.has_value());

	const auto persisted = easy_multistream::loadProfileSettings(config.get());
	CHECK(persisted.settings.youtubeAccountSelection.has_value());
	removeTestPath(path);
}

void testCommitReReadsProfilePathAndInvalidatesOnChange()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-path-change.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get());
	config.save();

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });
	const auto loaded = context.load();
	currentPath = std::string(kProfileB);
	const auto rejected =
		context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding, selectionB());
	CHECK(rejected.status == YouTubeAccountProfileContext::CommitStatus::Stale);
	CHECK(context.snapshot().generation == loaded.snapshot.generation + 1);
	CHECK(context.snapshot().profileBinding.empty());
	const auto persisted = easy_multistream::loadProfileSettings(config.get());
	CHECK(!persisted.settings.youtubeAccountSelection.has_value());
	removeTestPath(path);
}

void testCommitRejectsProfileChangeDuringConfigReadBeforeSave()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-config-read-change.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get(), true);
	config.save();

	std::string currentPath(kProfileA);
	int configReads = 0;
	YouTubeAccountProfileContext context(
		[&currentPath]() { return currentPath; },
		[&config, &currentPath, &configReads]() {
			++configReads;
			if (configReads == 2) {
				// The first read belongs to context.load().  Switch the active
				// profile while commitSelection is obtaining its config, before
				// it is allowed to mutate or save the old config.
				currentPath = std::string(kProfileB);
			}
			return config.get();
		});

	const auto loaded = context.load();
	CHECK(loaded.status == YouTubeAccountProfileContext::LoadStatus::Loaded);
	const auto rejected =
		context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding, selectionB());
	CHECK(rejected.status == YouTubeAccountProfileContext::CommitStatus::Stale);
	CHECK(configReads == 2);
	CHECK(context.snapshot().generation == loaded.snapshot.generation + 1);
	CHECK(context.snapshot().profileBinding.empty());

	// The old profile config must remain unchanged: the attempted B selection
	// must not be written after the active profile switched during config read.
	const auto persisted = easy_multistream::loadProfileSettings(config.get());
	CHECK(persisted.status == easy_multistream::SettingsLoadStatus::Loaded);
	CHECK(persisted.settings.youtubeAccountSelection.has_value());
	if (persisted.settings.youtubeAccountSelection.has_value()) {
		CHECK(persisted.settings.youtubeAccountSelection->channelId == "UC-alpha");
	}
	removeTestPath(path);
}

void testCommitOptionalSelectionAndValidation()
{
	ConfigHandle config;
	const auto path = testPath("easy-multistream-profile-context-optional.ini");
	removeTestPath(path);
	CHECK(config.open(path));
	setAccountSettings(config.get(), true);
	config.save();

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });
	const auto loaded = context.load();
	const auto invalid = context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding,
						     YouTubeAccountSelection{"", "", "", ""});
	CHECK(invalid.status == YouTubeAccountProfileContext::CommitStatus::InvalidSelection);
	CHECK(context.snapshot().selection.has_value());

	const auto cleared =
		context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding, std::nullopt);
	CHECK(cleared.status == YouTubeAccountProfileContext::CommitStatus::Committed);
	CHECK(!cleared.snapshot.selection.has_value());
	CHECK(cleared.snapshot.settingsStatus == easy_multistream::SettingsLoadStatus::SetupRequired);
	const auto persisted = easy_multistream::loadProfileSettings(config.get());
	CHECK(persisted.status == easy_multistream::SettingsLoadStatus::SetupRequired);
	CHECK(persisted.settings.youtubeConnectionMode == YouTubeConnectionMode::Account);
	CHECK(!persisted.settings.youtubeAccountSelection.has_value());
	removeTestPath(path);
}

void testLoadFailureStatusesAndInvalidation()
{
	std::string currentPath;
	config_t *config = nullptr;
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; }, [&config]() { return config; });

	const auto missingPath = context.load();
	CHECK(missingPath.status == YouTubeAccountProfileContext::LoadStatus::ProfileUnavailable);
	CHECK(missingPath.snapshot.profileBinding.empty());
	currentPath = std::string(kProfileA);
	const auto missingConfig = context.load();
	CHECK(missingConfig.status == YouTubeAccountProfileContext::LoadStatus::Unavailable);
	CHECK(missingConfig.snapshot.profileBinding.size() == 64);
	context.invalidate();
	CHECK(context.snapshot().profileBinding.empty());
	CHECK(context.snapshot().generation == missingConfig.snapshot.generation + 1);
}

void testCommitSaveFailureLeavesSnapshotAndConfigUnchanged()
{
	const auto directory = testPath("easy-multistream-profile-context-save-failure");
	const auto path = directory / "basic.ini";
	std::error_code error;
	std::filesystem::remove_all(directory, error);
	CHECK(std::filesystem::create_directory(directory, error));

	ConfigHandle config;
	CHECK(config.open(path));
	setAccountSettings(config.get(), true);
	config.save();
	std::filesystem::remove_all(directory, error);

	std::string currentPath(kProfileA);
	YouTubeAccountProfileContext context([&currentPath]() { return currentPath; },
					     [&config]() { return config.get(); });
	const auto loaded = context.load();
	const auto before = context.snapshot();
	const auto failed =
		context.commitSelection(loaded.snapshot.generation, loaded.snapshot.profileBinding, selectionB());
	CHECK(failed.status == YouTubeAccountProfileContext::CommitStatus::SaveFailed);
	CHECK(context.snapshot().generation == before.generation);
	CHECK(sameSelection(context.snapshot().selection, before.selection));
	const auto restored = easy_multistream::loadProfileSettings(config.get());
	CHECK(restored.settings.youtubeAccountSelection.has_value());
	if (restored.settings.youtubeAccountSelection.has_value()) {
		CHECK(restored.settings.youtubeAccountSelection->channelId == "UC-alpha");
	}

	std::filesystem::remove_all(directory, error);
}

} // namespace

int main()
{
	testLoadCopiesValueOnlyProfileContext();
	testCurrentProfileBindingIsSideEffectFreeAndBounded();
	testCurrentProfileBindingRejectsMissingThrowingAndInvalidReaders();
	testActiveAccountCheckIsFreshAndDoesNotAdvanceGeneration();
	testGenerationInvalidationPreventsStaleCommit();
	testCommitSelectionRequiresFreshProfileAndAccountMode();
	testManualProfileDoesNotExposePreservedAccountSelection();
	testCommitReReadsProfilePathAndInvalidatesOnChange();
	testCommitRejectsProfileChangeDuringConfigReadBeforeSave();
	testCommitOptionalSelectionAndValidation();
	testLoadFailureStatusesAndInvalidation();
	testCommitSaveFailureLeavesSnapshotAndConfigUnchanged();
	return failures == 0 ? 0 : 1;
}

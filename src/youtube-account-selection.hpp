// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace easy_multistream {

// These limits apply to the non-secret values returned by YouTube discovery
// and later stored with a profile. They deliberately do not apply to access
// tokens, refresh tokens, stream keys, or other credentials.
inline constexpr std::size_t kYouTubeAccountMaxIdentifierBytes = 256U;
inline constexpr std::size_t kYouTubeAccountMaxLabelBytes = 1024U;

// The OBS-native output remains the source of truth in Manual mode. Account
// mode selects a YouTube account/stream without changing the native output.
// Keep serialization strings outside this value model so persistence cannot
// accidentally become part of the state machine's public contract.
enum class YouTubeConnectionMode {
	Manual,
	Account,
};

// These identifiers and labels are deliberately non-secret. In particular,
// this type can never contain an authorization code, token, PKCE value, or
// stream key.
struct YouTubeAccountSelection final {
	std::string channelId;
	std::string channelLabel;
	std::string streamId;
	std::string streamLabel;
};

// The values of this enum are part of the pure validation boundary. Add new
// values only at the end so callers can safely classify a result without
// depending on provider or transport error text.
enum class YouTubeAccountSelectionValidationError {
	None,
	EmptyChannelId,
	InvalidChannelId,
	EmptyChannelLabel,
	InvalidChannelLabel,
	EmptyStreamId,
	InvalidStreamId,
	EmptyStreamLabel,
	InvalidStreamLabel,
};

bool isValidYouTubeAccountIdentifier(std::string_view value) noexcept;
bool isValidYouTubeAccountLabel(std::string_view value) noexcept;

YouTubeAccountSelectionValidationError
validateYouTubeAccountSelection(const YouTubeAccountSelection &selection) noexcept;

} // namespace easy_multistream

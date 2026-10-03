// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <cstddef>
#include <string_view>

namespace easy_multistream {

// These limits are shared by the local credential vault and the YouTube
// destination validation used by both settings and API-facing code.
inline constexpr std::size_t kMaxCredentialSecretBytes = 5U * 512U;
inline constexpr std::size_t kMaxYouTubeServerUrlBytes = 2048U;

enum class StreamKeyValidationError {
	None,
	Empty,
	TooLong,
	WhitespaceOrControlCharacter,
	InvalidUtf8,
};

StreamKeyValidationError validateYouTubeStreamKey(std::string_view streamKey) noexcept;

enum class YouTubeServerUrlValidationError {
	None,
	Empty,
	TooLong,
	EmbeddedNull,
	WhitespaceOrControlCharacter,
	InvalidUtf8,
	InvalidScheme,
	MissingHostname,
	UnsupportedHostname,
	UserInfoNotAllowed,
	InvalidPort,
	QueryNotAllowed,
	FragmentNotAllowed,
	InvalidPath,
};

YouTubeServerUrlValidationError validateYouTubeServerUrl(std::string_view serverUrl) noexcept;

} // namespace easy_multistream

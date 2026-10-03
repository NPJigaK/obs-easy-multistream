// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-account-selection.hpp"

#include <cstdint>

namespace easy_multistream {
namespace {

bool isVisibleAsciiIdentifier(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kYouTubeAccountMaxIdentifierBytes) {
		return false;
	}
	for (const unsigned char byte : value) {
		if (byte < 0x21U || byte > 0x7EU) {
			return false;
		}
	}
	return true;
}

bool isValidUtf8Label(std::string_view value) noexcept
{
	if (value.empty() || value.size() > kYouTubeAccountMaxLabelBytes) {
		return false;
	}

	std::size_t index = 0;
	while (index < value.size()) {
		const auto first = static_cast<unsigned char>(value[index++]);
		if (first <= 0x7FU) {
			// Reject C0 controls and DEL. The C1 range is rejected after UTF-8
			// decoding below so labels cannot carry terminal/control characters
			// through a multibyte representation either.
			if (first <= 0x1FU || first == 0x7FU) {
				return false;
			}
			continue;
		}

		std::size_t continuationCount = 0;
		std::uint32_t codePoint = 0;
		std::uint32_t minimum = 0;
		if (first >= 0xC2U && first <= 0xDFU) {
			continuationCount = 1;
			codePoint = first & 0x1FU;
			minimum = 0x80U;
		} else if (first >= 0xE0U && first <= 0xEFU) {
			continuationCount = 2;
			codePoint = first & 0x0FU;
			minimum = 0x800U;
		} else if (first >= 0xF0U && first <= 0xF4U) {
			continuationCount = 3;
			codePoint = first & 0x07U;
			minimum = 0x10000U;
		} else {
			return false;
		}

		if (value.size() - index < continuationCount) {
			return false;
		}
		for (std::size_t offset = 0; offset < continuationCount; ++offset) {
			const auto continuation = static_cast<unsigned char>(value[index++]);
			if ((continuation & 0xC0U) != 0x80U) {
				return false;
			}
			codePoint = (codePoint << 6U) | (continuation & 0x3FU);
		}

		const bool bidiControl = codePoint == 0x061CU || codePoint == 0x200EU || codePoint == 0x200FU ||
					 (codePoint >= 0x202AU && codePoint <= 0x202EU) ||
					 (codePoint >= 0x2066U && codePoint <= 0x2069U);
		if (codePoint < minimum || codePoint > 0x10FFFFU || (codePoint >= 0xD800U && codePoint <= 0xDFFFU) ||
		    (codePoint >= 0x80U && codePoint <= 0x9FU) || codePoint == 0x2028U || codePoint == 0x2029U ||
		    bidiControl) {
			return false;
		}
	}

	return true;
}

} // namespace

bool isValidYouTubeAccountIdentifier(std::string_view value) noexcept
{
	return isVisibleAsciiIdentifier(value);
}

bool isValidYouTubeAccountLabel(std::string_view value) noexcept
{
	return isValidUtf8Label(value);
}

YouTubeAccountSelectionValidationError
validateYouTubeAccountSelection(const YouTubeAccountSelection &selection) noexcept
{
	if (selection.channelId.empty()) {
		return YouTubeAccountSelectionValidationError::EmptyChannelId;
	}
	if (!isValidYouTubeAccountIdentifier(selection.channelId)) {
		return YouTubeAccountSelectionValidationError::InvalidChannelId;
	}
	if (selection.channelLabel.empty()) {
		return YouTubeAccountSelectionValidationError::EmptyChannelLabel;
	}
	if (!isValidYouTubeAccountLabel(selection.channelLabel)) {
		return YouTubeAccountSelectionValidationError::InvalidChannelLabel;
	}
	if (selection.streamId.empty()) {
		return YouTubeAccountSelectionValidationError::EmptyStreamId;
	}
	if (!isValidYouTubeAccountIdentifier(selection.streamId)) {
		return YouTubeAccountSelectionValidationError::InvalidStreamId;
	}
	if (selection.streamLabel.empty()) {
		return YouTubeAccountSelectionValidationError::EmptyStreamLabel;
	}
	if (!isValidYouTubeAccountLabel(selection.streamLabel)) {
		return YouTubeAccountSelectionValidationError::InvalidStreamLabel;
	}
	return YouTubeAccountSelectionValidationError::None;
}

} // namespace easy_multistream

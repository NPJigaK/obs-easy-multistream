// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"
#include "youtube-account-selection.hpp"
#include "youtube-destination.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace easy_multistream {

enum class CredentialError {
	None,
	NotFound,
	InvalidInput,
	AccessDenied,
	Unavailable,
	ResourceExhausted,
	CorruptData,
	// The credential exists, but its profile/channel binding does not match
	// the caller's requested scope. This must be surfaced separately so the
	// account flow can request authorization again without treating it as a
	// transient credential-store outage.
	ScopeMismatch,
	OperatingSystemError,
};

enum class CredentialState {
	Present,
	Missing,
	NeedsReauthorization,
	Unavailable,
};

struct CredentialResult {
	CredentialError error = CredentialError::None;
	std::uint32_t nativeError = 0;

	bool succeeded() const noexcept { return error == CredentialError::None; }
};

struct CredentialReadResult {
	CredentialResult result;
	SecureBuffer secret;
};

struct CredentialStatus {
	CredentialState state = CredentialState::Unavailable;
	CredentialResult result;
};

class CredentialVault {
public:
	virtual ~CredentialVault() = default;

	virtual CredentialResult write(std::string_view secret) noexcept = 0;
	virtual CredentialReadResult read() noexcept = 0;
	virtual CredentialResult erase() noexcept = 0;
	virtual CredentialStatus status() noexcept = 0;
};

// The account credential is scoped to the active OBS profile and the selected
// YouTube channel. The profile binding is deliberately supplied as a digest,
// not as a path, so the credential store never has to parse or persist a
// filesystem path. Both values are non-secret identifiers.
struct YouTubeAccountCredentialScope final {
	std::string profileBinding;
	std::string channelId;
};

inline constexpr std::size_t kYouTubeAccountProfileBindingHexBytes = 64U;

// Profile bindings are SHA-256 digests encoded as canonical lower-case hex.
// Keeping this validator platform-independent lets asynchronous account
// components reject a malformed scope before touching Credential Manager.
inline bool isValidYouTubeAccountProfileBinding(std::string_view value) noexcept
{
	if (value.size() != kYouTubeAccountProfileBindingHexBytes) {
		return false;
	}
	for (const unsigned char byte : value) {
		if (!((byte >= static_cast<unsigned char>('0') && byte <= static_cast<unsigned char>('9')) ||
		      (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('f')))) {
			return false;
		}
	}
	return true;
}

// Semantic capability for the Google account credential. Keeping this as a
// distinct type prevents the manual YouTube stream-key vault from being wired
// into OAuth code and accidentally sent to Google's token endpoint. Unlike a
// generic vault, every operation must identify its profile/channel scope.
class YouTubeAccountRefreshTokenStore {
public:
	virtual ~YouTubeAccountRefreshTokenStore() = default;

	virtual CredentialResult write(const YouTubeAccountCredentialScope &scope,
				       std::string_view secret) noexcept = 0;
	virtual CredentialReadResult read(const YouTubeAccountCredentialScope &scope) noexcept = 0;
	virtual CredentialResult erase(const YouTubeAccountCredentialScope &scope) noexcept = 0;
	virtual CredentialStatus status(const YouTubeAccountCredentialScope &scope) noexcept = 0;
};

} // namespace easy_multistream

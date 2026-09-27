// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "secure-buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace easy_multistream {

inline constexpr std::size_t kMaxCredentialSecretBytes = 5U * 512U;

enum class CredentialError {
	None,
	NotFound,
	InvalidInput,
	AccessDenied,
	Unavailable,
	ResourceExhausted,
	CorruptData,
	OperatingSystemError,
};

enum class CredentialState {
	Present,
	Missing,
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

} // namespace easy_multistream

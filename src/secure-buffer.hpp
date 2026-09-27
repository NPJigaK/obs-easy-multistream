// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace easy_multistream {

class SecureBuffer final {
public:
	SecureBuffer() noexcept = default;
	explicit SecureBuffer(std::size_t size);

	SecureBuffer(const SecureBuffer &) = delete;
	SecureBuffer &operator=(const SecureBuffer &) = delete;

	SecureBuffer(SecureBuffer &&other) noexcept;
	SecureBuffer &operator=(SecureBuffer &&other) noexcept;

	~SecureBuffer();

	static SecureBuffer copyOf(std::string_view value);

	std::uint8_t *data() noexcept;
	const std::uint8_t *data() const noexcept;
	std::size_t size() const noexcept;
	bool empty() const noexcept;
	std::string_view view() const noexcept;

	void clear() noexcept;

private:
	std::unique_ptr<std::uint8_t[]> data_;
	std::size_t size_ = 0;
};

} // namespace easy_multistream

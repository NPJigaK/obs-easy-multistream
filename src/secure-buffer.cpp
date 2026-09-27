// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "secure-buffer.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstring>
#include <utility>

namespace easy_multistream {

SecureBuffer::SecureBuffer(std::size_t size)
	: data_(size == 0 ? nullptr : std::make_unique<std::uint8_t[]>(size)),
	  size_(size)
{
}

SecureBuffer::SecureBuffer(SecureBuffer &&other) noexcept
	: data_(std::move(other.data_)),
	  size_(std::exchange(other.size_, 0))
{
}

SecureBuffer &SecureBuffer::operator=(SecureBuffer &&other) noexcept
{
	if (this != &other) {
		clear();
		data_ = std::move(other.data_);
		size_ = std::exchange(other.size_, 0);
	}

	return *this;
}

SecureBuffer::~SecureBuffer()
{
	clear();
}

SecureBuffer SecureBuffer::copyOf(std::string_view value)
{
	SecureBuffer result(value.size());
	if (!value.empty()) {
		std::memcpy(result.data(), value.data(), value.size());
	}
	return result;
}

std::uint8_t *SecureBuffer::data() noexcept
{
	return data_.get();
}

const std::uint8_t *SecureBuffer::data() const noexcept
{
	return data_.get();
}

std::size_t SecureBuffer::size() const noexcept
{
	return size_;
}

bool SecureBuffer::empty() const noexcept
{
	return size_ == 0;
}

std::string_view SecureBuffer::view() const noexcept
{
	if (data_ == nullptr) {
		return {};
	}

	return {reinterpret_cast<const char *>(data_.get()), size_};
}

void SecureBuffer::clear() noexcept
{
	if (data_ != nullptr && size_ != 0) {
		SecureZeroMemory(data_.get(), size_);
	}

	data_.reset();
	size_ = 0;
}

} // namespace easy_multistream

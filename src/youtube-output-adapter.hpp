// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include "runtime-controller.hpp"
#include "secure-buffer.hpp"
#include "session-coordinator.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace easy_multistream {

// These values are deliberately value-only.  An OBS callback may be delivered
// by an encoder or RTMP worker thread; the receiver must marshal the event to
// its owner thread before touching SessionCoordinator or Qt.
enum class YouTubeOutputEventKind {
	Starting,
	Started,
	Reconnecting,
	Reconnected,
	Stopping,
	Stopped,
	Failed,
	Released,
};

struct YouTubeOutputEvent {
	OutputLease lease{};
	YouTubeOutputEventKind kind = YouTubeOutputEventKind::Failed;
	int code = 0;
};

class YouTubeOutputEventReceiver {
public:
	virtual ~YouTubeOutputEventReceiver() = default;

	// Called from an OBS signal or the adapter reaper thread.  This method must
	// only copy the event and post it to the owner's serialized thread.  It must
	// not call OBS/Qt APIs or destroy the adapter from inside the callback.
	virtual void onYouTubeOutputEvent(YouTubeOutputEvent event) noexcept = 0;
};

enum class YouTubeOutputStartResult {
	Accepted,
	RejectedBusy,
	RejectedShuttingDown,
	RejectedInvalidRequest,
	RejectedNativeUnavailable,
	RejectedUnsupportedEncoders,
	RejectedServiceCreate,
	RejectedOutputCreate,
	RejectedAttach,
	RejectedStart,
};

struct YouTubeOutputStartRequest {
	OutputLease lease{};
	std::string serverUrl;
	SecureBuffer streamKey;
};

// A small, single-secondary-output adapter for the OBS 32.2.2 API.
//
// The adapter intentionally has no Qt dependency and never starts or stops the
// native OBS stream.  It attaches the native H.264/AAC encoders to one private
// rtmp_custom/rtmp_output pair.  The output is released by a private reaper
// thread because OBS output destruction may join RTMP and data-capture threads.
class YouTubeOutputAdapter final : public IRuntimeYouTubeOutputAdapter {
public:
	YouTubeOutputAdapter();
	~YouTubeOutputAdapter();

	YouTubeOutputAdapter(const YouTubeOutputAdapter &) = delete;
	YouTubeOutputAdapter &operator=(const YouTubeOutputAdapter &) = delete;

	// Rich OBS-facing entry point, useful to a bridge that wants the detailed
	// result. RuntimeController uses the noexcept port below.
	YouTubeOutputStartResult startOutput(YouTubeOutputStartRequest request);

	void setEventSink(IRuntimeYouTubeOutputAdapter::EventSink sink) noexcept override;
	void start(YouTubeStartRequest request) noexcept override;
	void requestStop(OutputLease lease) noexcept override;
	// Normal stop is asynchronous; shutdown is the final barrier and may join
	// the reaper. RuntimeController calls it at EXIT after Released is observed.
	void shutdown() noexcept override;

	bool isActive(OutputLease lease) const noexcept;

private:
	class Runtime;
	std::shared_ptr<Runtime> runtime_;
};

} // namespace easy_multistream

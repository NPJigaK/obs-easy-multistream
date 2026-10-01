// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "youtube-output-adapter.hpp"

#include "settings.hpp"

#include <obs-frontend-api.h>
#include <obs-output.h>
#include <obs.hpp>

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace easy_multistream {
namespace {

constexpr char kRtmpOutputId[] = "rtmp_output";
constexpr char kRtmpCustomServiceId[] = "rtmp_custom";
constexpr char kOutputNamePrefix[] = "Easy Multistream YouTube ";

constexpr int kAdapterErrorBase = -1000;

int adapterError(YouTubeOutputStartResult result) noexcept
{
	return kAdapterErrorBase - static_cast<int>(result);
}

int boundedCode(long long value) noexcept
{
	if (value < std::numeric_limits<int>::min()) {
		return std::numeric_limits<int>::min();
	}
	if (value > std::numeric_limits<int>::max()) {
		return std::numeric_limits<int>::max();
	}
	return static_cast<int>(value);
}

bool isCodec(const obs_encoder_t *encoder, enum obs_encoder_type expectedType, const char *expectedCodec) noexcept
{
	if (encoder == nullptr || obs_encoder_get_type(encoder) != expectedType) {
		return false;
	}

	const char *codec = obs_encoder_get_codec(encoder);
	return codec != nullptr && std::strcmp(codec, expectedCodec) == 0;
}

bool hasSupportedEncoderLayout(const obs_output_t *output, obs_encoder_t *&video, obs_encoder_t *&audio) noexcept
{
	video = obs_output_get_video_encoder2(output, 0);
	audio = obs_output_get_audio_encoder(output, 0);
	if (video == nullptr || audio == nullptr) {
		return false;
	}

	size_t videoCount = 0;
	for (size_t index = 0; index < MAX_OUTPUT_VIDEO_ENCODERS; ++index) {
		if (obs_output_get_video_encoder2(output, index) != nullptr) {
			++videoCount;
		}
	}

	// Index 0 is OBS's live-program audio encoder. Twitch's optional VOD
	// track is attached at index 1; it must not make otherwise ordinary
	// Twitch configurations incompatible, and it must not be sent to YouTube.
	return videoCount == 1;
}

} // namespace

class YouTubeOutputAdapter::Runtime : public std::enable_shared_from_this<YouTubeOutputAdapter::Runtime> {
private:
	struct Session;

	struct CallbackContext final {
		std::weak_ptr<Runtime> runtime;
		std::weak_ptr<Session> session;
		OutputLease lease{};
		std::atomic_bool accepting{true};
		std::atomic_bool terminalEventSent{false};
	};

	struct Session final {
		explicit Session(OutputLease leaseValue) : lease(leaseValue) {}

		void markStartFinished() noexcept
		{
			{
				std::lock_guard lock(startMutex);
				startFinished = true;
			}
			startCv.notify_all();
		}

		void waitForStartFinished() noexcept
		{
			std::unique_lock lock(startMutex);
			startCv.wait(lock, [this] { return startFinished; });
		}

		void disconnectSignals() noexcept
		{
			starting.Disconnect();
			started.Disconnect();
			reconnecting.Disconnect();
			reconnected.Disconnect();
			stopping.Disconnect();
			stopped.Disconnect();
			deactivated.Disconnect();
		}

		const OutputLease lease;

		// Declaration order is intentional.  The default destruction order is
		// signals, output, encoders, service, native output, context.  Runtime
		// teardown performs the same order explicitly before this object dies.
		std::shared_ptr<CallbackContext> callbackContext;
		OBSOutputAutoRelease nativeOutput;
		OBSServiceAutoRelease service;
		OBSEncoderAutoRelease nativeAudio;
		OBSEncoderAutoRelease nativeVideo;
		OBSOutputAutoRelease youtubeOutput;

		OBSSignal starting;
		OBSSignal started;
		OBSSignal reconnecting;
		OBSSignal reconnected;
		OBSSignal stopping;
		OBSSignal stopped;
		OBSSignal deactivated;

		std::atomic_bool teardownQueued{false};
		std::mutex startMutex;
		std::condition_variable startCv;
		bool startFinished = false;
	};

public:
	using EventSink = IRuntimeYouTubeOutputAdapter::EventSink;

	Runtime()
	{
		reaper_ = std::thread([this] { reaperLoop(); });
		reaperId_ = reaper_.get_id();
	}

	~Runtime()
	{
		shutdown();
	}

	void setEventSink(EventSink sink) noexcept
	{
		try {
			auto replacement = std::make_shared<EventSink>(std::move(sink));
			std::lock_guard lock(mutex_);
			eventSink_ = std::move(replacement);
		} catch (...) {
			// Keep the last valid sink.  Runtime teardown remains safe even when
			// an application is under allocation pressure.
		}
	}

	void rejectWithoutSession(OutputLease lease, YouTubeOutputStartResult result) noexcept
	{
		emitRejected(lease, result);
	}

	YouTubeOutputStartResult start(YouTubeOutputStartRequest request)
	{
		if (validateYouTubeServerUrl(request.serverUrl) != YouTubeServerUrlValidationError::None ||
		    validateYouTubeStreamKey(request.streamKey.view()) != StreamKeyValidationError::None) {
			emitRejected(request.lease, YouTubeOutputStartResult::RejectedInvalidRequest);
			return YouTubeOutputStartResult::RejectedInvalidRequest;
		}

		YouTubeOutputStartResult earlyRejection = YouTubeOutputStartResult::Accepted;
		{
			std::lock_guard lock(mutex_);
			if (shutdownRequested_) {
				earlyRejection = YouTubeOutputStartResult::RejectedShuttingDown;
			} else if (current_ || startInProgress_) {
				earlyRejection = YouTubeOutputStartResult::RejectedBusy;
			} else {
				startInProgress_ = true;
			}
		}
		if (earlyRejection != YouTubeOutputStartResult::Accepted) {
			emitRejected(request.lease, earlyRejection);
			return earlyRejection;
		}

		OBSOutputAutoRelease nativeOutput(obs_frontend_get_streaming_output());
		if (nativeOutput == nullptr || !obs_output_active(nativeOutput.Get())) {
			clearStartReservation();
			emitRejected(request.lease, YouTubeOutputStartResult::RejectedNativeUnavailable);
			return YouTubeOutputStartResult::RejectedNativeUnavailable;
		}

		const uint32_t nativeFlags = obs_output_get_flags(nativeOutput.Get());
		constexpr uint32_t kRequiredFlags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
		if ((nativeFlags & kRequiredFlags) != kRequiredFlags) {
			clearStartReservation();
			emitRejected(request.lease, YouTubeOutputStartResult::RejectedUnsupportedEncoders);
			return YouTubeOutputStartResult::RejectedUnsupportedEncoders;
		}

		obs_encoder_t *nativeVideo = nullptr;
		obs_encoder_t *nativeAudio = nullptr;
		if (!hasSupportedEncoderLayout(nativeOutput.Get(), nativeVideo, nativeAudio) ||
		    !isCodec(nativeVideo, OBS_ENCODER_VIDEO, "h264") || !isCodec(nativeAudio, OBS_ENCODER_AUDIO, "aac")) {
			clearStartReservation();
			emitRejected(request.lease, YouTubeOutputStartResult::RejectedUnsupportedEncoders);
			return YouTubeOutputStartResult::RejectedUnsupportedEncoders;
		}

		OBSEncoderAutoRelease videoReference(obs_encoder_get_ref(nativeVideo));
		OBSEncoderAutoRelease audioReference(obs_encoder_get_ref(nativeAudio));
		if (videoReference == nullptr || audioReference == nullptr) {
			clearStartReservation();
			emitRejected(request.lease, YouTubeOutputStartResult::RejectedUnsupportedEncoders);
			return YouTubeOutputStartResult::RejectedUnsupportedEncoders;
		}

		std::shared_ptr<Session> session;
		try {
			session = std::make_shared<Session>(request.lease);
			session->nativeOutput = std::move(nativeOutput);
			session->nativeVideo = std::move(videoReference);
			session->nativeAudio = std::move(audioReference);

			{
				OBSDataAutoRelease serviceSettings(obs_data_create());
				if (serviceSettings == nullptr) {
					return rejectSession(session, YouTubeOutputStartResult::RejectedServiceCreate);
				}

				obs_data_set_string(serviceSettings.Get(), "server", request.serverUrl.c_str());
				SecureBuffer nulTerminatedKey(request.streamKey.size() + 1U);
				if (!request.streamKey.empty()) {
					std::memcpy(nulTerminatedKey.data(), request.streamKey.data(), request.streamKey.size());
				}
				nulTerminatedKey.data()[request.streamKey.size()] = 0;
				obs_data_set_string(serviceSettings.Get(), "key",
						   reinterpret_cast<const char *>(nulTerminatedKey.data()));

				session->service = obs_service_create_private(kRtmpCustomServiceId, "Easy Multistream YouTube",
										 serviceSettings.Get());
				request.streamKey.clear();
				nulTerminatedKey.clear();
			}
			if (session->service == nullptr) {
				return rejectSession(session, YouTubeOutputStartResult::RejectedServiceCreate);
			}

			const std::string outputName = std::string(kOutputNamePrefix) + std::to_string(request.lease.generation) +
								       "." + std::to_string(request.lease.attempt);
			session->youtubeOutput = obs_output_create(kRtmpOutputId, outputName.c_str(), nullptr, nullptr);
			if (session->youtubeOutput == nullptr) {
				return rejectSession(session, YouTubeOutputStartResult::RejectedOutputCreate);
			}

			obs_output_set_video_encoder(session->youtubeOutput.Get(), session->nativeVideo.Get());
			obs_output_set_audio_encoder(session->youtubeOutput.Get(), session->nativeAudio.Get(), 0);
			obs_output_set_service(session->youtubeOutput.Get(), session->service.Get());
			obs_output_set_reconnect_settings(session->youtubeOutput.Get(), 0, 0);

			if (obs_output_get_video_encoder2(session->youtubeOutput.Get(), 0) != session->nativeVideo.Get() ||
			    obs_output_get_audio_encoder(session->youtubeOutput.Get(), 0) != session->nativeAudio.Get() ||
			    obs_output_get_service(session->youtubeOutput.Get()) != session->service.Get()) {
				return rejectSession(session, YouTubeOutputStartResult::RejectedAttach);
			}

			session->callbackContext = std::make_shared<CallbackContext>();
			session->callbackContext->runtime = shared_from_this();
			session->callbackContext->session = session;
			session->callbackContext->lease = session->lease;

			signal_handler_t *signals = obs_output_get_signal_handler(session->youtubeOutput.Get());
			if (signals == nullptr) {
				return rejectSession(session, YouTubeOutputStartResult::RejectedAttach);
			}
			session->starting.Connect(signals, "starting", &Runtime::onStarting, session->callbackContext.get());
			session->started.Connect(signals, "start", &Runtime::onStarted, session->callbackContext.get());
			session->reconnecting.Connect(signals, "reconnect", &Runtime::onReconnecting,
							       session->callbackContext.get());
			session->reconnected.Connect(signals, "reconnect_success", &Runtime::onReconnected,
							       session->callbackContext.get());
			session->stopping.Connect(signals, "stopping", &Runtime::onStopping, session->callbackContext.get());
			session->stopped.Connect(signals, "stop", &Runtime::onStopped, session->callbackContext.get());
			session->deactivated.Connect(signals, "deactivate", &Runtime::onDeactivated,
							      session->callbackContext.get());

			bool shutdownBeforeStart = false;
			{
				std::lock_guard lock(mutex_);
				shutdownBeforeStart = shutdownRequested_;
				current_ = session;
			}
			if (shutdownBeforeStart) {
				session->markStartFinished();
				{
					std::lock_guard lock(mutex_);
					startInProgress_ = false;
				}
				cv_.notify_all();
				sendSessionFailure(session, YouTubeOutputStartResult::RejectedShuttingDown);
				enqueueTeardown(session);
				return YouTubeOutputStartResult::RejectedShuttingDown;
			}

			// Keep a separate strong reference across the call.  A stop callback
			// may enqueue the reaper while OBS is still returning from start.
			OBSOutputAutoRelease startReference(obs_output_get_ref(session->youtubeOutput.Get()));
			bool accepted = false;
			if (startReference != nullptr && session->callbackContext->accepting.load(std::memory_order_acquire)) {
				accepted = obs_output_start(startReference.Get());
			}
			session->markStartFinished();

			{
				std::lock_guard lock(mutex_);
				startInProgress_ = false;
			}
			cv_.notify_all();

			if (!accepted) {
				sendSessionFailure(session, YouTubeOutputStartResult::RejectedStart);
				enqueueTeardown(session);
				return YouTubeOutputStartResult::RejectedStart;
			}

			if (shutdownRequested()) {
				session->callbackContext->accepting.store(false, std::memory_order_release);
				enqueueTeardown(session);
			}
			return YouTubeOutputStartResult::Accepted;
		} catch (...) {
			if (session) {
				// No OBS start call is made before the session is installed, so
				// a setup exception cannot have an RTMP worker to join.
				session->disconnectSignals();
				session.reset();
			}
			clearStartReservation();
			emitRejected(request.lease, YouTubeOutputStartResult::RejectedOutputCreate);
			return YouTubeOutputStartResult::RejectedOutputCreate;
		}
	}

	bool requestStop(OutputLease lease) noexcept
	{
		std::shared_ptr<Session> session;
		{
			std::lock_guard lock(mutex_);
			if (!current_ || current_->lease != lease) {
				return false;
			}
			session = current_;
		}

		if (session->callbackContext) {
			const bool wasAccepting = session->callbackContext->accepting.exchange(false, std::memory_order_acq_rel);
			if (wasAccepting) {
				emit({lease, YouTubeOutputEventKind::Stopping, 0});
			}
		}
		enqueueTeardown(session);
		return true;
	}

	bool isActive(OutputLease lease) const noexcept
	{
		std::lock_guard lock(mutex_);
		return current_ && current_->lease == lease && !current_->teardownQueued.load(std::memory_order_acquire);
	}

	void shutdown() noexcept
	{
		std::shared_ptr<Session> session;
		{
			std::lock_guard lock(mutex_);
			if (!shutdownRequested_) {
				shutdownRequested_ = true;
			}
			session = current_;
		}

		if (session) {
			if (session->callbackContext) {
				session->callbackContext->accepting.store(false, std::memory_order_release);
			}
			enqueueTeardown(session);
		}
		cv_.notify_all();

		if (reaper_.joinable() && std::this_thread::get_id() != reaperId_) {
			reaper_.join();
		}
	}

private:
	void clearStartReservation() noexcept
	{
		{
			std::lock_guard lock(mutex_);
			startInProgress_ = false;
		}
		cv_.notify_all();
	}

	bool shutdownRequested() const noexcept
	{
		std::lock_guard lock(mutex_);
		return shutdownRequested_;
	}

	YouTubeOutputStartResult rejectSession(const std::shared_ptr<Session> &session,
						       YouTubeOutputStartResult result) noexcept
	{
		{
			std::lock_guard lock(mutex_);
			current_ = session;
			startInProgress_ = false;
		}
		session->markStartFinished();
		cv_.notify_all();
		sendSessionFailure(session, result);
		enqueueTeardown(session);
		return result;
	}

	void emitRejected(OutputLease lease, YouTubeOutputStartResult result) noexcept
	{
		emit({lease, YouTubeOutputEventKind::Failed, adapterError(result)});
		emit({lease, YouTubeOutputEventKind::Released, 0});
	}

	void sendSessionFailure(const std::shared_ptr<Session> &session, YouTubeOutputStartResult result) noexcept
	{
		if (session->callbackContext) {
			session->callbackContext->accepting.store(false, std::memory_order_release);
			bool expected = false;
			if (session->callbackContext->terminalEventSent.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel)) {
				emit({session->lease, YouTubeOutputEventKind::Failed, adapterError(result)});
			}
		} else {
			emit({session->lease, YouTubeOutputEventKind::Failed, adapterError(result)});
		}
	}

	void enqueueTeardown(const std::shared_ptr<Session> &session) noexcept
	{
		if (!session || session->teardownQueued.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		{
			std::lock_guard lock(mutex_);
			if (!pending_) {
				pending_ = session;
			} else if (pending_.get() != session.get() && !deferred_) {
				// There is only one current lease by construction.  The second
				// slot is a defensive barrier for a callback racing shutdown.
				deferred_ = session;
			}
		}
		cv_.notify_one();
	}

	void emit(YouTubeOutputEvent event) noexcept
	{
		try {
			std::shared_ptr<const EventSink> sink;
			{
				std::lock_guard lock(mutex_);
				sink = eventSink_;
			}
			if (!sink || !*sink) {
				return;
			}

			RuntimeOutputEvent runtimeEvent{RuntimeOutputEventKind::Released, event.lease};
			switch (event.kind) {
			case YouTubeOutputEventKind::Started:
				runtimeEvent.kind = RuntimeOutputEventKind::Started;
				break;
			case YouTubeOutputEventKind::Reconnecting:
				runtimeEvent.kind = RuntimeOutputEventKind::Reconnecting;
				break;
			case YouTubeOutputEventKind::Reconnected:
				runtimeEvent.kind = RuntimeOutputEventKind::ReconnectSucceeded;
				break;
			case YouTubeOutputEventKind::Released:
				runtimeEvent.kind = RuntimeOutputEventKind::Released;
				break;
			case YouTubeOutputEventKind::Starting:
			case YouTubeOutputEventKind::Stopping:
			case YouTubeOutputEventKind::Stopped:
			case YouTubeOutputEventKind::Failed:
				return;
			}
			(*sink)(runtimeEvent);
		} catch (...) {
			// A plugin callback must never unwind through OBS's signal code.
		}
	}

	static void onStarting(void *data, calldata_t *) noexcept { handleSignal(data, YouTubeOutputEventKind::Starting, 0); }
	static void onStarted(void *data, calldata_t *) noexcept { handleSignal(data, YouTubeOutputEventKind::Started, 0); }
	static void onReconnecting(void *data, calldata_t *) noexcept
	{
		handleSignal(data, YouTubeOutputEventKind::Reconnecting, 0);
	}
	static void onReconnected(void *data, calldata_t *) noexcept
	{
		handleSignal(data, YouTubeOutputEventKind::Reconnected, 0);
	}
	static void onStopping(void *data, calldata_t *) noexcept { handleSignal(data, YouTubeOutputEventKind::Stopping, 0); }

	static void onStopped(void *data, calldata_t *params) noexcept
	{
		auto *context = static_cast<CallbackContext *>(data);
		if (!context) {
			return;
		}
		const auto runtime = context->runtime.lock();
		const auto session = context->session.lock();
		if (!runtime || !session) {
			return;
		}

		long long rawCode = OBS_OUTPUT_SUCCESS;
		if (params != nullptr) {
			(void)calldata_get_int(params, "code", &rawCode);
		}
		const int code = boundedCode(rawCode);

		const bool wasAccepting = context->accepting.exchange(false, std::memory_order_acq_rel);
		if (wasAccepting) {
			bool expected = false;
			if (context->terminalEventSent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				runtime->emit({context->lease,
					       code == OBS_OUTPUT_SUCCESS ? YouTubeOutputEventKind::Stopped
									     : YouTubeOutputEventKind::Failed,
					       code});
			}
		}
		runtime->enqueueTeardown(session);
	}

	static void onDeactivated(void *data, calldata_t *) noexcept
	{
		auto *context = static_cast<CallbackContext *>(data);
		if (!context) {
			return;
		}
		const auto runtime = context->runtime.lock();
		const auto session = context->session.lock();
		if (!runtime || !session) {
			return;
		}

		const bool wasAccepting = context->accepting.exchange(false, std::memory_order_acq_rel);
		if (wasAccepting) {
			bool expected = false;
			if (context->terminalEventSent.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
				runtime->emit({context->lease, YouTubeOutputEventKind::Stopped, OBS_OUTPUT_SUCCESS});
			}
		}
		runtime->enqueueTeardown(session);
	}

	static void handleSignal(void *data, YouTubeOutputEventKind kind, int code) noexcept
	{
		auto *context = static_cast<CallbackContext *>(data);
		if (!context || !context->accepting.load(std::memory_order_acquire)) {
			return;
		}
		const auto runtime = context->runtime.lock();
		if (!runtime) {
			return;
		}
		runtime->emit({context->lease, kind, code});
	}

	void processTeardown(const std::shared_ptr<Session> &session) noexcept
	{
		if (!session) {
			return;
		}

		session->waitForStartFinished();
		if (session->callbackContext) {
			session->callbackContext->accepting.store(false, std::memory_order_release);
		}

		// Disconnect first. signal_handler_disconnect waits for a callback that
		// is currently signalling to return. This makes the following force
		// stop's callbacks inert while still leaving the output alive for its
		// own RTMP/connect-thread shutdown.
		session->disconnectSignals();

		// obs_output_stop() intentionally is not used: rtmp_stream_start can
		// still be connecting while obs_output_active() is false. force_stop
		// enters the output's stop implementation and joins that connect thread.
		if (session->youtubeOutput != nullptr) {
			obs_output_force_stop(session->youtubeOutput.Get());
		}

		session->youtubeOutput = nullptr;
		session->nativeVideo = nullptr;
		session->nativeAudio = nullptr;
		session->service = nullptr;
		session->nativeOutput = nullptr;
		session->callbackContext.reset();

		{
			std::lock_guard lock(mutex_);
			if (current_.get() == session.get()) {
				current_.reset();
			}
			if (deferred_ && !pending_) {
				pending_ = std::move(deferred_);
			}
		}
		cv_.notify_all();
		emit({session->lease, YouTubeOutputEventKind::Released, 0});
	}

	void reaperLoop() noexcept
	{
		for (;;) {
			std::shared_ptr<Session> session;
			{
				std::unique_lock lock(mutex_);
				cv_.wait(lock, [this] {
					return pending_ || deferred_ || (shutdownRequested_ && !startInProgress_);
				});

				if (pending_) {
					session = std::move(pending_);
					if (deferred_) {
						pending_ = std::move(deferred_);
					}
				} else if (shutdownRequested_ && !startInProgress_) {
					break;
				}
			}

			if (session) {
				processTeardown(session);
			}
		}
	}

	std::shared_ptr<const EventSink> eventSink_;

	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::shared_ptr<Session> current_;
	std::shared_ptr<Session> pending_;
	std::shared_ptr<Session> deferred_;
	bool startInProgress_ = false;
	bool shutdownRequested_ = false;

	std::thread reaper_;
	std::thread::id reaperId_;
};

YouTubeOutputAdapter::YouTubeOutputAdapter() : runtime_(std::make_shared<Runtime>())
{
}

YouTubeOutputAdapter::~YouTubeOutputAdapter()
{
	if (runtime_) {
		runtime_->shutdown();
	}
}

YouTubeOutputStartResult YouTubeOutputAdapter::startOutput(YouTubeOutputStartRequest request)
{
	return runtime_ ? runtime_->start(std::move(request)) : YouTubeOutputStartResult::RejectedShuttingDown;
}

void YouTubeOutputAdapter::setEventSink(IRuntimeYouTubeOutputAdapter::EventSink sink) noexcept
{
	if (runtime_) {
		runtime_->setEventSink(std::move(sink));
	}
}

void YouTubeOutputAdapter::start(YouTubeStartRequest request) noexcept
{
	if (!runtime_) {
		return;
	}

	try {
		(void)runtime_->start(YouTubeOutputStartRequest{request.lease, std::move(request.serverUrl),
									 std::move(request.streamKey)});
	} catch (...) {
		// Runtime::start normally contains all setup exceptions.  This last
		// guard preserves the IRuntimeYouTubeOutputAdapter noexcept contract.
		runtime_->rejectWithoutSession(request.lease, YouTubeOutputStartResult::RejectedOutputCreate);
	}
}

void YouTubeOutputAdapter::requestStop(OutputLease lease) noexcept
{
	if (runtime_) {
		(void)runtime_->requestStop(lease);
	}
}

bool YouTubeOutputAdapter::isActive(OutputLease lease) const noexcept
{
	return runtime_ && runtime_->isActive(lease);
}

void YouTubeOutputAdapter::shutdown() noexcept
{
	if (runtime_) {
		runtime_->shutdown();
	}
}

} // namespace easy_multistream

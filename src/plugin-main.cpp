// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-view.hpp"
#include "version.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QPointer>
#include <QString>
#include <QThread>

#include <exception>
#include <memory>
#include <utility>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-easy-multistream", "en-US")

namespace {

constexpr char kDockId[] = "easy_multistream.dock";

struct PluginState {
	QPointer<easy_multistream::DockView> dock;
	bool dockRegistered = false;
	bool callbackRegistered = false;
	bool exitSeen = false;
};

std::unique_ptr<PluginState> pluginState;

QString moduleText(const char *key)
{
	const char *value = obs_module_text(key);
	return QString::fromUtf8(value != nullptr ? value : key);
}

void removeFrontendObjects(PluginState &state) noexcept;

void onFrontendEvent(enum obs_frontend_event event, void *privateData) noexcept
{
	auto *state = static_cast<PluginState *>(privateData);
	if (state == nullptr || event != OBS_FRONTEND_EVENT_EXIT) {
		return;
	}

	state->exitSeen = true;
	removeFrontendObjects(*state);
}

void removeFrontendObjects(PluginState &state) noexcept
{
	if (state.callbackRegistered) {
		obs_frontend_remove_event_callback(onFrontendEvent, &state);
		state.callbackRegistered = false;
	}

	if (state.dockRegistered) {
		obs_frontend_remove_dock(kDockId);
		state.dockRegistered = false;
	}

	state.dock.clear();
}

bool isUiThread() noexcept
{
	QCoreApplication *application = QCoreApplication::instance();
	return application != nullptr && application->thread() == QThread::currentThread();
}

} // namespace

const char *obs_module_description(void)
{
	return "A lightweight OBS Studio multistreaming plugin.";
}

bool obs_module_load(void)
{
	try {
		if (pluginState != nullptr) {
			blog(LOG_ERROR, "[obs-easy-multistream] Refusing duplicate module initialization");
			return false;
		}

		if (!isUiThread()) {
			blog(LOG_ERROR,
			     "[obs-easy-multistream] The frontend dock must be initialized on the Qt UI thread");
			return false;
		}

		auto state = std::make_unique<PluginState>();
		easy_multistream::DockText text{
			moduleText("Dock.Heading"),       moduleText("Dock.Destinations"),
			moduleText("Dock.Primary"),       moduleText("Dock.PrimaryStatus"),
			moduleText("Dock.YouTube"),       moduleText("Dock.YouTubeStatus"),
			moduleText("Dock.PreviewNotice"),
		};
		auto dock = std::make_unique<easy_multistream::DockView>(std::move(text));
		state->dock = dock.get();

		const QByteArray dockTitle = moduleText("Dock.Title").toUtf8();
		if (!obs_frontend_add_dock_by_id(kDockId, dockTitle.constData(), dock.get())) {
			blog(LOG_ERROR, "[obs-easy-multistream] Failed to register the OBS dock");
			return false;
		}

		state->dockRegistered = true;
		(void)dock.release();
		pluginState = std::move(state);

		try {
			obs_frontend_add_event_callback(onFrontendEvent, pluginState.get());
			pluginState->callbackRegistered = true;
		} catch (...) {
			removeFrontendObjects(*pluginState);
			pluginState.reset();
			throw;
		}

		blog(LOG_INFO, "[obs-easy-multistream] Loaded version %s", easy_multistream::kVersion);
		return true;
	} catch (const std::exception &error) {
		if (pluginState != nullptr) {
			removeFrontendObjects(*pluginState);
			pluginState.reset();
		}
		blog(LOG_ERROR, "[obs-easy-multistream] Module initialization failed: %s", error.what());
	} catch (...) {
		if (pluginState != nullptr) {
			removeFrontendObjects(*pluginState);
			pluginState.reset();
		}
		blog(LOG_ERROR, "[obs-easy-multistream] Module initialization failed with an unknown exception");
	}

	return false;
}

void obs_module_unload(void)
{
	if (pluginState == nullptr) {
		return;
	}

	if (!pluginState->exitSeen) {
		removeFrontendObjects(*pluginState);
	}

	pluginState.reset();
	blog(LOG_INFO, "[obs-easy-multistream] Unloaded");
}

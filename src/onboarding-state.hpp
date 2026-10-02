// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <util/config-file.h>

namespace easy_multistream {

// This preference belongs to the OBS user rather than an OBS profile.  The
// caller must pass obs_frontend_get_user_config(), never the active profile's
// basic.ini.
bool shouldAutoShowDock(config_t *userConfig) noexcept;
int markDockAutoShowHandled(config_t *userConfig) noexcept;

} // namespace easy_multistream

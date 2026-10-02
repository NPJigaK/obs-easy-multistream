// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-visibility.hpp"

#include <QDockWidget>
#include <QString>
#include <QWidget>

namespace easy_multistream {

bool showDockById(QWidget *mainWindow, const char *dockId) noexcept
{
	if (mainWindow == nullptr || dockId == nullptr || dockId[0] == '\0' || !mainWindow->isVisible() ||
	    mainWindow->isMinimized()) {
		return false;
	}

	QDockWidget *dock = mainWindow->findChild<QDockWidget *>(QString::fromUtf8(dockId));
	if (dock == nullptr) {
		return false;
	}

	dock->setVisible(true);
	dock->raise();
	return dock->isVisible();
}

} // namespace easy_multistream

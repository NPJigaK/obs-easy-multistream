// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#pragma once

#include <QString>
#include <QWidget>

namespace easy_multistream {

struct DockText {
	QString heading;
	QString destinations;
	QString primaryName;
	QString primaryStatus;
	QString youtubeName;
	QString youtubeStatus;
	QString previewNotice;
};

class DockView final : public QWidget {
public:
	explicit DockView(DockText text, QWidget *parent = nullptr);
};

} // namespace easy_multistream

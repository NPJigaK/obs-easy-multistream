// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 NPJigaK

#include "dock-view.hpp"

#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include <utility>

namespace easy_multistream {

DockView::DockView(DockText text, QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("easyMultistreamDockView"));
	setMinimumWidth(260);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *heading = new QLabel(std::move(text.heading), this);
	QFont headingFont = heading->font();
	headingFont.setBold(true);
	heading->setFont(headingFont);
	layout->addWidget(heading);

	auto *destinations = new QGroupBox(std::move(text.destinations), this);
	auto *destinationsLayout = new QFormLayout(destinations);
	destinationsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	destinationsLayout->addRow(std::move(text.primaryName),
				   new QLabel(std::move(text.primaryStatus), destinations));
	destinationsLayout->addRow(std::move(text.youtubeName),
				   new QLabel(std::move(text.youtubeStatus), destinations));
	layout->addWidget(destinations);

	auto *notice = new QLabel(std::move(text.previewNotice), this);
	notice->setObjectName(QStringLiteral("easyMultistreamPreviewNotice"));
	notice->setWordWrap(true);
	layout->addWidget(notice);
	layout->addStretch();
}

} // namespace easy_multistream

#include "gui/focusgraphwidget.h"

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace gui;

FocusGraphWidget::FocusGraphWidget (QWidget *parent):
	QWidget (parent)
{
	setMinimumHeight (150);
}

void FocusGraphWidget::setPoints (const QVector<Point> &points)
{
	m_points = points;
	update ();
}

void FocusGraphWidget::paintEvent (QPaintEvent *)
{
	QPainter painter (this);
	painter.fillRect (rect (), Qt::black);

	if (m_points.isEmpty ())
	{
		painter.setPen (Qt::white);
		painter.drawText (rect (), Qt::AlignCenter, "no data yet");
		return;
	}

	const int marginLeft = 40;
	const int marginBottom = 20;
	QRectF plotRect (marginLeft, 6, width () - marginLeft - 6, height () - marginBottom - 6);
	if (plotRect.width () <= 0 || plotRect.height () <= 0)
		return;

	double xMin = m_points.first ().x, xMax = m_points.first ().x;
	double yMin = std::numeric_limits<double>::max (), yMax = std::numeric_limits<double>::lowest ();
	for (const auto &p : m_points)
	{
		xMin = std::min (xMin, p.x);
		xMax = std::max (xMax, p.x);
		yMin = std::min ({ yMin, p.fwhmX, p.fwhmY });
		yMax = std::max ({ yMax, p.fwhmX, p.fwhmY });
	}
	if (xMax <= xMin)
		xMax = xMin + 1;
	if (yMax <= yMin)
		yMax = yMin + 1;
	double yPad = (yMax - yMin) * 0.1;
	yMin -= yPad;
	yMax += yPad;

	auto toPixel = [&] (double x, double y) -> QPointF
	{
		double px = plotRect.left () + (x - xMin) / (xMax - xMin) * plotRect.width ();
		double py = plotRect.bottom () - (y - yMin) / (yMax - yMin) * plotRect.height ();
		return QPointF (px, py);
	};

	painter.setPen (Qt::gray);
	painter.drawRect (plotRect);

	painter.setPen (Qt::white);
	painter.drawText (QRectF (0, plotRect.top () - 4, marginLeft - 4, 16), Qt::AlignRight | Qt::AlignTop, QString::number (yMax, 'f', 1));
	painter.drawText (QRectF (0, plotRect.bottom () - 12, marginLeft - 4, 16), Qt::AlignRight | Qt::AlignTop, QString::number (yMin, 'f', 1));
	painter.drawText (QRectF (plotRect.left (), plotRect.bottom () + 2, 60, marginBottom), Qt::AlignLeft, QString::number (xMin, 'f', 0));
	painter.drawText (QRectF (plotRect.right () - 60, plotRect.bottom () + 2, 60, marginBottom), Qt::AlignRight, QString::number (xMax, 'f', 0));

	QVector<QPointF> lineX, lineY;
	for (const auto &p : m_points)
	{
		lineX << toPixel (p.x, p.fwhmX);
		lineY << toPixel (p.x, p.fwhmY);
	}

	painter.setPen (QPen (Qt::red, 2));
	if (lineX.size () > 1)
		painter.drawPolyline (lineX.data (), lineX.size ());
	for (const auto &pt : lineX)
		painter.drawEllipse (pt, 2, 2);

	painter.setPen (QPen (Qt::blue, 2));
	if (lineY.size () > 1)
		painter.drawPolyline (lineY.data (), lineY.size ());
	for (const auto &pt : lineY)
		painter.drawEllipse (pt, 2, 2);
}

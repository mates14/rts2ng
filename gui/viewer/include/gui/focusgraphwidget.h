#pragma once

#include <QWidget>
#include <QVector>

namespace gui
{

/**
 * Minimal custom-drawn FWHM-vs-(image index or focuser position) plot -
 * Qt5Charts's dev headers aren't installed on this host, and a two-series
 * line plot is simple enough to draw directly with QPainter, same spirit
 * as MainWindow's zoomed star crop.
 */
class FocusGraphWidget : public QWidget
{
	Q_OBJECT

	public:
		struct Point
		{
			double x;
			double fwhmX;
			double fwhmY;
		};

		explicit FocusGraphWidget (QWidget *parent = nullptr);

		void setPoints (const QVector<Point> &points);

	protected:
		void paintEvent (QPaintEvent *event) override;

	private:
		QVector<Point> m_points;
};

}

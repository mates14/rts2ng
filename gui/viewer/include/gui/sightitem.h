#pragma once

#include <QGraphicsItem>
#include <QColor>

namespace gui
{

/**
 * Draggable crosshair box overlay on the image canvas - a C++/Qt port of
 * fiber_pointing_client.py's SightGraphicsItem (its "target"/"source" red
 * and green boxes): fixed-size box, crosshair through its centre, dragged
 * position clamped to stay inside the scene rect. Stripped of that
 * script's instrument-specific spin-box binding and star-centroid
 * autodetection - just the crosshair and drag-clamp behaviour, which is
 * the part of fiber_pointing's UI this project is actually reusing.
 */
class SightItem : public QGraphicsItem
{
	public:
		SightItem (qreal width, qreal height, const QColor &color, bool rectangle = true, bool showCrosshair = true);

		QRectF boundingRect () const override;
		void paint (QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

		void setSize (qreal width, qreal height);
		qreal width () const { return m_width; }
		qreal height () const { return m_height; }

	protected:
		QVariant itemChange (GraphicsItemChange change, const QVariant &value) override;

	private:
		qreal m_width;
		qreal m_height;
		QColor m_color;
		bool m_rectangle;
		bool m_showCrosshair;
};

}

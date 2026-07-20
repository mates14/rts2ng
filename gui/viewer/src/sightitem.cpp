#include "gui/sightitem.h"

#include <QPainter>
#include <QGraphicsScene>
#include <algorithm>

using namespace gui;

SightItem::SightItem (qreal width, qreal height, const QColor &color, bool rectangle, bool showCrosshair):
	m_width (width), m_height (height), m_color (color), m_rectangle (rectangle), m_showCrosshair (showCrosshair)
{
	setFlag (QGraphicsItem::ItemIsMovable, true);
	setFlag (QGraphicsItem::ItemSendsScenePositionChanges, true);
}

QRectF SightItem::boundingRect () const
{
	const qreal border = 10;
	return QRectF (-border, -border, m_width + border * 2, m_height + border * 2);
}

void SightItem::paint (QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
	painter->setPen (QPen (m_color, 1, Qt::SolidLine));

	if (m_showCrosshair)
	{
		const qreal size = 25;
		qreal xc = m_width / 2;
		qreal yc = m_height / 2;

		painter->drawLine (QPointF (xc - size, yc), QPointF (xc + size, yc));
		painter->drawLine (QPointF (xc, yc - size), QPointF (xc, yc + size));
	}

	if (m_rectangle)
		painter->drawRect (QRectF (0, 0, m_width, m_height));
}

void SightItem::setSize (qreal width, qreal height)
{
	if (width != m_width || height != m_height)
	{
		prepareGeometryChange ();
		m_width = width;
		m_height = height;
	}
}

QVariant SightItem::itemChange (GraphicsItemChange change, const QVariant &value)
{
	if (change == ItemPositionChange && scene ())
	{
		QRectF rect = scene ()->sceneRect ();
		QPointF newPos = value.toPointF ();

		qreal maxX = rect.right () - m_width;
		qreal maxY = rect.bottom () - m_height;

		newPos.setX (std::min (maxX, std::max (newPos.x (), rect.left ())));
		newPos.setY (std::min (maxY, std::max (newPos.y (), rect.top ())));

		return newPos;
	}
	return QGraphicsItem::itemChange (change, value);
}

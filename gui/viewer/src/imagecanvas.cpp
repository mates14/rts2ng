#include "gui/imagecanvas.h"

#include <QPixmap>
#include <QBrush>

using namespace gui;

ImageCanvas::ImageCanvas (QWidget *parent):
	QGraphicsView (parent)
{
	m_scene = new QGraphicsScene (this);
	m_scene->setSceneRect (0, 0, 1, 1);

	m_pixmapItem = new QGraphicsPixmapItem ();
	m_scene->addItem (m_pixmapItem);

	// Red "measure" cursor - the region the focus centroid/FWHM fit runs
	// on (ViewerCamera::runFit()). Default size matches the user's own
	// expectation of this project's earlier size (32x32).
	m_crosshair = new SightItem (32, 32, Qt::red, true, true);
	m_scene->addItem (m_crosshair);

	// Blue "window" cursor - picks the chip subframe (WINDOW ValueRectangle)
	// for the next exposure. Hidden until Windowing is switched on
	// (MainWindow's checkbox); no crosshair, it's a region selector, not a
	// point marker. Default 256x256, per the described windowing flow.
	m_windowItem = new SightItem (256, 256, Qt::blue, true, false);
	m_windowItem->setVisible (false);
	m_scene->addItem (m_windowItem);

	// The window box is usually much larger than the measure box and, once
	// a windowed exposure comes back, can cover the entire visible image -
	// without an explicit z-order both items default to the same stacking
	// level and whichever was added to the scene later (the window box)
	// wins every mouse event, making the red cursor completely
	// undraggable underneath it. Keep the red cursor on top always.
	m_crosshair->setZValue (1);
	m_windowItem->setZValue (0);

	setScene (m_scene);
	setBackgroundBrush (QBrush (Qt::black));
}

QRect ImageCanvas::itemRect (SightItem *item) const
{
	QPointF pos = item->scenePos ();
	return QRect ((int) pos.x (), (int) pos.y (), (int) item->width (), (int) item->height ());
}

QRect ImageCanvas::measureRect () const
{
	return itemRect (m_crosshair);
}

void ImageCanvas::setMeasureSize (int size)
{
	m_crosshair->setSize (size, size);
}

QRect ImageCanvas::windowRect () const
{
	return itemRect (m_windowItem);
}

void ImageCanvas::setWindowSize (int width, int height)
{
	m_windowItem->setSize (width, height);
}

void ImageCanvas::setWindowingEnabled (bool enabled)
{
	m_windowItem->setVisible (enabled);
	if (enabled && !m_currentImage.isNull ())
	{
		m_windowItem->setPos ((m_currentImage.width () - m_windowItem->width ()) / 2.0,
			(m_currentImage.height () - m_windowItem->height ()) / 2.0);
	}
}

bool ImageCanvas::windowingEnabled () const
{
	return m_windowItem->isVisible ();
}

void ImageCanvas::setImage (const QImage &image)
{
	if (image.isNull ())
		return;

	QRectF oldRect = m_scene->sceneRect ();
	bool sizeChanged = (oldRect.width () != image.width ()) || (oldRect.height () != image.height ());

	m_scene->setSceneRect (0, 0, image.width (), image.height ());
	m_pixmapItem->setPixmap (QPixmap::fromImage (image));
	m_currentImage = image;

	if (sizeChanged)
	{
		m_crosshair->setPos ((image.width () - m_crosshair->width ()) / 2.0, (image.height () - m_crosshair->height ()) / 2.0);
		if (m_windowItem->isVisible ())
			m_windowItem->setPos ((image.width () - m_windowItem->width ()) / 2.0, (image.height () - m_windowItem->height ()) / 2.0);
		// Deliberately no fitInView()/zoom here - it rescaled the view's
		// transform on every size change, so a windowed (smaller) image got
		// stretched to fill the same viewport as the last full-frame one,
		// looking "zoomed in" and making the red cursor hard to grab where
		// the user expected it. Always show at native 1:1 scale for now;
		// zooming in/out (useful for large chips vs small windowed
		// sections) is worth adding later, deliberately deferred rather
		// than getting the coordinate math wrong under time pressure.
	}
}

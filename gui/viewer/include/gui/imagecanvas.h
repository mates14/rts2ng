#pragma once

#include <QGraphicsView>
#include <QGraphicsScene>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QRect>
#include <QWheelEvent>

#include "gui/sightitem.h"

namespace gui
{

/**
 * Central image display: a QGraphicsView/Scene canvas modeled on
 * fiber_pointing_client.py's approach (QGraphicsPixmapItem + draggable
 * SightGraphicsItem-style boxes), rebuilt in Qt5/C++. Unlike that script,
 * pixel data arrives already flipped top-down
 * (rts2image::Image::getChannelGrayscaleImage's invert_y=true), so - unlike
 * fiber_pointing_client.py's `graphicsView.scale(1, -1)` - no extra
 * view-level Y flip is needed here.
 *
 * Two independent boxes sit on top of the image: the red "measure" cursor
 * (always present, used for the focus centroid/FWHM fit) and the blue
 * "window" cursor (hidden until Windowing is switched on, used to pick
 * the chip subframe for the next exposure). Both report their rects in
 * the same top-down display-pixel coordinates as the displayed QImage.
 */
class ImageCanvas : public QGraphicsView
{
	Q_OBJECT

	public:
		explicit ImageCanvas (QWidget *parent = nullptr);

		QRect measureRect () const;
		void setMeasureSize (int size);

		QRect windowRect () const;
		// width/height separately: MainWindow computes these from its own
		// "size" spinbox divided by the current binning factor (WINDOW is
		// always unbinned chip pixels, but this box is drawn in display
		// pixels - see MainWindow::updateWindowBoxSize()), so the two axes
		// can legitimately differ if BINX != BINY.
		void setWindowSize (int width, int height);
		void setWindowingEnabled (bool enabled);
		bool windowingEnabled () const;

		QImage currentImage () const { return m_currentImage; }

		double zoom () const { return m_zoom; }
		static constexpr double minZoom = 0.1;
		static constexpr double maxZoom = 8.0;

	public slots:
		void setImage (const QImage &image);

		/**
		 * Sets the view's display scale (1.0 = native pixel-for-pixel), on
		 * top of whatever image is currently shown - unlike the deliberately
		 * absent fitInView() call in setImage() (see its comment), this is
		 * a user-driven scale that persists across new images, not
		 * recomputed per frame. Clamped to [minZoom, maxZoom] and a no-op
		 * if the clamped value doesn't actually change anything, both to
		 * keep MainWindow's spinbox<->wheelEvent() feedback loop
		 * (zoomChanged() -> QDoubleSpinBox::setValue() -> valueChanged() ->
		 * setZoom() again) from re-entering endlessly.
		 */
		void setZoom (double zoom);

	signals:
		/**
		 * Fired whenever the zoom actually changes, whether from
		 * setZoom() (MainWindow's spinbox) or wheelEvent() (mouse wheel
		 * over the image) - MainWindow connects this back to its spinbox's
		 * setValue() so the two stay in sync regardless of which one drove
		 * the change.
		 */
		void zoomChanged (double zoom);

	protected:
		void wheelEvent (QWheelEvent *event) override;

	private:
		QRect itemRect (SightItem *item) const;

		QGraphicsScene *m_scene;
		QGraphicsPixmapItem *m_pixmapItem;
		SightItem *m_crosshair;
		SightItem *m_windowItem;
		QImage m_currentImage;

		// 1.0 = native pixel-for-pixel, same convention as MainWindow's zoom
		// spinbox - see setZoom()/wheelEvent().
		double m_zoom = 1.0;
};

}

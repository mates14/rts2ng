#pragma once

#include <QColor>
#include <QString>

namespace gui
{

/**
 * Red-on-black "night vision" look for the controls, shared by
 * rts2-viewer and rts2-stacker - everything drawn through the application
 * palette (widgets, the log, combo popups, scroll bars) turns dim red on
 * black, so the observer's dark adaptation survives a glance at the
 * screen. The image itself is not touched: it is drawn from its own
 * grey-level data on the canvas's own black background.
 *
 * Widgets that set their own colours (status labels' green/red
 * backgrounds, the focus graph) ask statusStyle()/activeStyle()/
 * nightText() instead of hard-coding them, and have to be refreshed by
 * their owner after setNightMode() - the palette part applies by itself.
 */
bool nightMode ();

/**
 * Switch the whole application. Uses the Fusion style while on (the
 * platform styles, e.g. a GTK theme, ignore much of the palette); off
 * restores the style and palette that were in use before.
 */
void setNightMode (bool on);

/** Stylesheet for a status label that is fine (good) or needs attention. */
QString statusStyle (bool good);

/** Stylesheet for a toggle button that is on (e.g. image saving). */
QString activeStyle ();

/** Foreground colour for custom-painted text and lines. */
QColor nightText ();

}

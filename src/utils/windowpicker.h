// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QPoint>
#include <QRect>
#include <QVector>

namespace WindowPicker {

/**
 * @brief Rectangle of the top-most window under a global point.
 *
 * The rectangle is the visible outer geometry of the window that contains the
 * point, in the logical coordinates of the screen under it:
 * - X11: the top-level window (direct child of the root window).
 * - Windows: the top-level window, using the DWM extended frame bounds when
 *   available so the invisible resize border and drop shadow are excluded.
 *
 * Windows owned by this process (the capture overlay), input-only windows on
 * X11 and the desktop window itself are ignored.
 *
 * @param globalPos Point in global (screen) coordinates.
 * @return The window rectangle, or a null QRect when no window is there or the
 * platform offers no lookup (macOS, Wayland).
 */
QRect windowRectAt(const QPoint& globalPos);

/**
 * @brief Rectangles of the windows on the current workspace, top-most first.
 *
 * Meant to be called before the capture overlay is mapped: on X11 a tiling
 * window manager re-layouts its windows when the overlay appears, so the live
 * geometry no longer describes what the user was looking at. On X11 the list
 * is filtered to the current desktop (`_NET_CURRENT_DESKTOP`, sticky windows
 * included) and excludes windows owned by this process, input-only windows and
 * empty rectangles. Coordinates are logical; mixed-DPI multi-screen setups are
 * approximated with the primary screen's device pixel ratio.
 *
 * @return The rectangles, or an empty vector when the platform offers no
 * enumeration (Windows has no workspace concept; macOS and Wayland return an
 * empty vector).
 */
QVector<QRect> windowsInCurrentWorkspace();

} // namespace WindowPicker

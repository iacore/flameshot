// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "windowpicker.h"

#include <QGuiApplication>
#include <QScreen>

#if defined(Q_OS_WIN)
#include <dwmapi.h>
#include <windows.h>
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <xcb/xcb.h>

#include <unistd.h>

#include <cstring>
#endif

namespace {

#if defined(Q_OS_WIN)

/**
 * The DWM reports the visible frame (without the invisible resize border and
 * drop shadow) through DWMWA_EXTENDED_FRAME_BOUNDS; fall back to the full
 * window rectangle when DWM is not running.
 */
RECT frameBounds(HWND hwnd)
{
    RECT rect{};
    if (FAILED(DwmGetWindowAttribute(
          hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        if (!GetWindowRect(hwnd, &rect)) {
            return RECT{};
        }
    }
    return rect;
}

bool isCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(
          hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        return false;
    }
    return cloaked != 0;
}

bool isOwnWindow(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

bool isDesktopWindow(HWND hwnd)
{
    wchar_t name[64] = {};
    if (GetClassNameW(hwnd, name, 64) == 0) {
        return false;
    }
    return wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0;
}

struct PickState
{
    POINT point;
    HWND found;
};

BOOL CALLBACK pickWindowProc(HWND hwnd, LPARAM lparam)
{
    auto* state = reinterpret_cast<PickState*>(lparam);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    if (isOwnWindow(hwnd) || isDesktopWindow(hwnd) || isCloaked(hwnd)) {
        return TRUE;
    }
    const RECT rect = frameBounds(hwnd);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return TRUE;
    }
    if (!PtInRect(&rect, state->point)) {
        return TRUE;
    }
    // EnumWindows walks top-level windows in front-to-back order
    state->found = hwnd;
    return FALSE;
}

#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)

struct WindowRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

xcb_atom_t internAtom(xcb_connection_t* connection, const char* name)
{
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
      connection,
      xcb_intern_atom(connection, 0, std::strlen(name), name),
      nullptr);
    const xcb_atom_t atom = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return atom;
}

uint32_t cardinalProperty(xcb_connection_t* connection,
                          xcb_window_t window,
                          xcb_atom_t atom,
                          bool& found)
{
    found = false;
    if (atom == XCB_ATOM_NONE) {
        return 0;
    }
    xcb_get_property_reply_t* reply = xcb_get_property_reply(
      connection,
      xcb_get_property(connection, 0, window, atom, XCB_ATOM_CARDINAL, 0, 1),
      nullptr);
    if (reply == nullptr) {
        return 0;
    }
    uint32_t value = 0;
    if (reply->format == 32 && xcb_get_property_value_length(reply) >= 4) {
        value =
          *reinterpret_cast<const uint32_t*>(xcb_get_property_value(reply));
        found = true;
    }
    free(reply);
    return value;
}

bool isOwnWindow(xcb_connection_t* connection,
                 xcb_window_t window,
                 xcb_atom_t pidAtom)
{
    bool found = false;
    const uint32_t pid = cardinalProperty(connection, window, pidAtom, found);
    return found && pid == static_cast<uint32_t>(getpid());
}

/**
 * Viewable top-level windows (direct children of the root window), top-most
 * first, in root (physical pixel) coordinates. Windows owned by this process
 * are skipped so the capture overlay does not shadow what is below it. With
 * @p currentWorkspaceOnly the list is restricted to the current desktop,
 * keeping windows that are sticky (`_NET_WM_DESKTOP` 0xFFFFFFFF).
 */
QVector<WindowRect> topLevelWindows(bool currentWorkspaceOnly)
{
    QVector<WindowRect> result;
    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        return result;
    }
    const xcb_setup_t* setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    if (screens.rem == 0) {
        xcb_disconnect(connection);
        return result;
    }
    const xcb_window_t root = screens.data->root;

    const xcb_atom_t pidAtom = internAtom(connection, "_NET_WM_PID");
    xcb_atom_t desktopAtom = XCB_ATOM_NONE;
    bool haveDesktop = false;
    uint32_t currentDesktop = 0;
    if (currentWorkspaceOnly) {
        currentDesktop =
          cardinalProperty(connection,
                           root,
                           internAtom(connection, "_NET_CURRENT_DESKTOP"),
                           haveDesktop);
        desktopAtom = internAtom(connection, "_NET_WM_DESKTOP");
        currentWorkspaceOnly = haveDesktop && desktopAtom != XCB_ATOM_NONE;
    }

    xcb_query_tree_reply_t* tree = xcb_query_tree_reply(
      connection, xcb_query_tree(connection, root), nullptr);
    if (tree != nullptr) {
        const xcb_window_t* children = xcb_query_tree_children(tree);
        const int count = xcb_query_tree_children_length(tree);
        // children are listed bottom-most first, so walk back to front
        for (int i = count - 1; i >= 0; --i) {
            const xcb_window_t candidate = children[i];
            xcb_get_window_attributes_reply_t* attrs =
              xcb_get_window_attributes_reply(
                connection,
                xcb_get_window_attributes(connection, candidate),
                nullptr);
            if (attrs == nullptr) {
                continue;
            }
            const bool viewable = attrs->map_state == XCB_MAP_STATE_VIEWABLE &&
                                  attrs->_class != XCB_WINDOW_CLASS_INPUT_ONLY;
            free(attrs);
            if (!viewable) {
                continue;
            }
            xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(
              connection, xcb_get_geometry(connection, candidate), nullptr);
            if (geometry == nullptr) {
                continue;
            }
            WindowRect window{
                geometry->x, geometry->y, geometry->width, geometry->height
            };
            const bool usable = window.width > 0 && window.height > 0;
            free(geometry);
            if (!usable || isOwnWindow(connection, candidate, pidAtom)) {
                continue;
            }
            if (currentWorkspaceOnly) {
                bool hasWindowDesktop = false;
                const uint32_t windowDesktop = cardinalProperty(
                  connection, candidate, desktopAtom, hasWindowDesktop);
                constexpr uint32_t STICKY = 0xFFFFFFFFu;
                if (hasWindowDesktop && windowDesktop != STICKY &&
                    windowDesktop != currentDesktop) {
                    continue;
                }
            }
            result.append(window);
        }
        free(tree);
    }
    xcb_disconnect(connection);
    return result;
}

#endif

} // namespace

QRect WindowPicker::windowRectAt(const QPoint& globalPos)
{
#if defined(Q_OS_WIN)
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    PickState state{ POINT{ qRound(globalPos.x() * dpr),
                            qRound(globalPos.y() * dpr) },
                     nullptr };
    EnumWindows(&pickWindowProc, reinterpret_cast<LPARAM>(&state));
    if (state.found == nullptr) {
        return {};
    }
    const RECT rect = frameBounds(state.found);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return {};
    }
    return QRect(qRound(rect.left / dpr),
                 qRound(rect.top / dpr),
                 qRound((rect.right - rect.left) / dpr),
                 qRound((rect.bottom - rect.top) / dpr));
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        return {};
    }
    QScreen* screen = QGuiApplication::screenAt(globalPos);
    const qreal dpr = screen ? screen->devicePixelRatio() : 1.0;
    const int x = qRound(globalPos.x() * dpr);
    const int y = qRound(globalPos.y() * dpr);
    const QVector<WindowRect> windows = topLevelWindows(false);
    for (const WindowRect& window : windows) {
        if (x >= window.x && y >= window.y && x < window.x + window.width &&
            y < window.y + window.height) {
            return QRect(qRound(window.x / dpr),
                         qRound(window.y / dpr),
                         qRound(window.width / dpr),
                         qRound(window.height / dpr));
        }
    }
    return {};
#else
    Q_UNUSED(globalPos)
    return {};
#endif
}

QVector<QRect> WindowPicker::windowsInCurrentWorkspace()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        return {};
    }
    QScreen* primary = QGuiApplication::primaryScreen();
    const qreal dpr = primary ? primary->devicePixelRatio() : 1.0;
    QVector<QRect> result;
    const QVector<WindowRect> windows = topLevelWindows(true);
    result.reserve(windows.size());
    for (const WindowRect& window : windows) {
        result.append(QRect(qRound(window.x / dpr),
                            qRound(window.y / dpr),
                            qRound(window.width / dpr),
                            qRound(window.height / dpr)));
    }
    return result;
#else
    return {};
#endif
}

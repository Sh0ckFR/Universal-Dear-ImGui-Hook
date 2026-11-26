// While the menu is open, the application reads no presses or motion from
// the X server. Xlib reads its events through these xcb functions too.
#include "internal.h"

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>

namespace uih::detail {
namespace {

using EventFn = xcb_generic_event_t* (*)(xcb_connection_t*);

constexpr uint8_t kGenericEvent = 35;

uint32_t XiDetail(const xcb_generic_event_t* event) {
    uint32_t detail = 0;
    memcpy(&detail, reinterpret_cast<const uint8_t*>(event) + 16, sizeof(detail));
    return detail;
}

bool WheelButton(uint32_t button) { return button >= 4 && button <= 7; }

// Wheel buttons 4 to 7 scroll the menu instead.
void Scroll(uint32_t button) {
    static const int kSteps[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
    input::AddWheel(kSteps[button - 4][0], kSteps[button - 4][1]);
}

// Releases still reach the application, so that it does not see keys or
// buttons held while the menu is open, except those of the wheel.
bool Hidden(const xcb_generic_event_t* event) {
    switch (event->response_type & 0x7f) {
    case XCB_KEY_PRESS:
    case XCB_MOTION_NOTIFY:
        return true;
    case XCB_BUTTON_PRESS: {
        uint8_t button = reinterpret_cast<const xcb_button_press_event_t*>(event)->detail;
        if (WheelButton(button))
            Scroll(button);
        return true;
    }
    case XCB_BUTTON_RELEASE:
        return WheelButton(reinterpret_cast<const xcb_button_release_event_t*>(event)->detail);
    case kGenericEvent: {
        auto* generic = reinterpret_cast<const xcb_ge_generic_event_t*>(event);
        uint8_t opcode = input::XInputOpcode();
        if (opcode == 0 || generic->extension != opcode)
            return false;
        switch (generic->event_type) {
        case 2:  // XI_KeyPress
        case 6:  // XI_Motion
        case 13: // XI_RawKeyPress
        case 17: // XI_RawMotion
            return true;
        case 4: // XI_ButtonPress
            if (WheelButton(XiDetail(event)))
                Scroll(XiDetail(event));
            return true;
        case 15: // XI_RawButtonPress, also sent with the press above
            return true;
        case 5:  // XI_ButtonRelease
        case 16: // XI_RawButtonRelease
            return WheelButton(XiDetail(event));
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

xcb_generic_event_t* Filter(EventFn next, xcb_connection_t* connection) {
    for (;;) {
        xcb_generic_event_t* event = next(connection);
        if (!event || !IsMenuOpen() || input::OwnConnection(connection) || !Hidden(event))
            return event;
        free(event);
    }
}

EventFn Next(const char* name) { return reinterpret_cast<EventFn>(interpose::kXcb.Next(name)); }

} // namespace
} // namespace uih::detail

using namespace uih::detail;

UIH_EXPORT xcb_generic_event_t* xcb_wait_for_event(xcb_connection_t* connection) {
    static const EventFn next = Next("xcb_wait_for_event");
    return Filter(next, connection);
}

UIH_EXPORT xcb_generic_event_t* xcb_poll_for_event(xcb_connection_t* connection) {
    static const EventFn next = Next("xcb_poll_for_event");
    return Filter(next, connection);
}

UIH_EXPORT xcb_generic_event_t* xcb_poll_for_queued_event(xcb_connection_t* connection) {
    static const EventFn next = Next("xcb_poll_for_queued_event");
    return Filter(next, connection);
}

namespace uih::detail {
namespace {

const char* const kXcbSonames[] = {"libxcb.so.1", nullptr};

const interpose::Function kXcbExports[] = {
    {"xcb_wait_for_event", reinterpret_cast<void*>(&xcb_wait_for_event)},
    {"xcb_poll_for_event", reinterpret_cast<void*>(&xcb_poll_for_event)},
    {"xcb_poll_for_queued_event", reinterpret_cast<void*>(&xcb_poll_for_queued_event)},
};

} // namespace

const interpose::Library interpose::kXcb = {kXcbSonames, kXcbExports, sizeof(kXcbExports) / sizeof(kXcbExports[0])};

} // namespace uih::detail

// The platform layer on the PlayStation 2 (see platform/platform.hpp): the controller stands for the keyboard, and the clock is the C++ library's.
// Only what the game's console modes use is here: a window with OpenGL, cursors, message boxes and folders have no meaning on the console.
//
// The pad (port 1): the d-pad and the left stick pan (the arrow keys), L2 and R2 zoom (Z, X), L1 and R1 turn the view a quarter (Q, E), triangle and cross
// change storey (Page Up, Page Down), square the wall view (Tab), circle the roofs (R), and Start leaves (Escape).
#include "platform/platform.hpp"

#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>

#include <cstring>
#include <deque>

namespace ts2::platform {

namespace {

bool g_padOpen = false;
alignas(64) char g_padArea[256];
unsigned g_buttons = 0, g_previous = 0;
float g_stickX = 0, g_stickY = 0;
std::deque<Event> g_events;
std::string g_error;

struct Mapping {
    unsigned button;
    Key key;
};

// Buttons that are keys pressed once (an event each time they go down).
constexpr Mapping kPressed[] = {{PAD_L1, Key::Q},       {PAD_R1, Key::E},      {PAD_TRIANGLE, Key::PageUp}, {PAD_CROSS, Key::PageDown},
                                {PAD_SQUARE, Key::Tab}, {PAD_CIRCLE, Key::R}, {PAD_START, Key::Escape}};

// Reads the pad once, and queues the events for what went down since the last time.
void poll() {
    if (!g_padOpen) {
        return;
    }
    padButtonStatus status;
    std::memset(&status, 0, sizeof status);
    if (padGetState(0, 0) != PAD_STATE_STABLE || padRead(0, 0, &status) == 0) {
        return;
    }
    g_buttons = static_cast<unsigned>(status.btns ^ 0xFFFF);  // active low
    g_stickX = (static_cast<int>(status.ljoy_h) - 128) / 128.0f;
    g_stickY = (static_cast<int>(status.ljoy_v) - 128) / 128.0f;
    for (const Mapping& m : kPressed) {
        if ((g_buttons & m.button) != 0 && (g_previous & m.button) == 0) {
            Event e;
            e.type = Event::Type::KeyDown;
            e.key = m.key;
            e.scancode = m.key;
            g_events.push_back(e);
        }
    }
    g_previous = g_buttons;
}

} // namespace

bool init() {
    SifInitRpc(0);
    // The controller: the ROM's pad modules.
    SifLoadModule("rom0:SIO2MAN", 0, nullptr);
    SifLoadModule("rom0:PADMAN", 0, nullptr);
    padInit(0);
    g_padOpen = padPortOpen(0, 0, g_padArea) != 0;
    if (!g_padOpen) {
        g_error = "the controller port would not open";
    }
    return true;
}

void quit() {
    if (g_padOpen) {
        padPortClose(0, 0);
        g_padOpen = false;
    }
}

std::string lastError() {
    return g_error;
}

bool pollEvent(Event& out) {
    if (g_events.empty()) {
        poll();
    }
    if (g_events.empty()) {
        return false;
    }
    out = g_events.front();
    g_events.pop_front();
    return true;
}

bool keyHeld(Key key) {
    constexpr float kDead = 0.35f;
    switch (key) {
    case Key::Up:
        return (g_buttons & PAD_UP) != 0 || g_stickY < -kDead;
    case Key::Down:
        return (g_buttons & PAD_DOWN) != 0 || g_stickY > kDead;
    case Key::Left:
        return (g_buttons & PAD_LEFT) != 0 || g_stickX < -kDead;
    case Key::Right:
        return (g_buttons & PAD_RIGHT) != 0 || g_stickX > kDead;
    case Key::Z:
        return (g_buttons & PAD_L2) != 0;
    case Key::X:
        return (g_buttons & PAD_R2) != 0;
    default:
        return false;
    }
}

void mousePosition(float& x, float& y) {
    x = 0;
    y = 0;
}

} // namespace ts2::platform

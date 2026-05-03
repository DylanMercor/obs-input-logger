/*
 * macOS input hooks (CGEventTap).
 *
 * Runs on a dedicated background thread with its own CFRunLoop so the OBS
 * UI/render threads are never touched from the input callback.
 *
 * Requires Accessibility / Input Monitoring permission the first time it
 * runs; macOS will prompt. Without permission the tap creation returns NULL
 * and we log a warning but otherwise fail gracefully.
 */

#import <Foundation/Foundation.h>
#import <ApplicationServices/ApplicationServices.h>
#include <pthread.h>

#include "input-logger.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <util/threading.h>

static pthread_t g_thr;
static CFMachPortRef g_tap = NULL;
static CFRunLoopSourceRef g_src = NULL;
static CFRunLoopRef g_runloop = NULL;
static volatile bool g_running = false;

/* Map macOS keycodes to short stable names matching the sample schema. */
static const char *il_mac_keycode_name(CGKeyCode kc)
{
    switch (kc) {
        case 0x00:
            return "a";
        case 0x0B:
            return "b";
        case 0x08:
            return "c";
        case 0x02:
            return "d";
        case 0x0E:
            return "e";
        case 0x03:
            return "f";
        case 0x05:
            return "g";
        case 0x04:
            return "h";
        case 0x22:
            return "i";
        case 0x26:
            return "j";
        case 0x28:
            return "k";
        case 0x25:
            return "l";
        case 0x2E:
            return "m";
        case 0x2D:
            return "n";
        case 0x1F:
            return "o";
        case 0x23:
            return "p";
        case 0x0C:
            return "q";
        case 0x0F:
            return "r";
        case 0x01:
            return "s";
        case 0x11:
            return "t";
        case 0x20:
            return "u";
        case 0x09:
            return "v";
        case 0x0D:
            return "w";
        case 0x07:
            return "x";
        case 0x10:
            return "y";
        case 0x06:
            return "z";
        case 0x1D:
            return "0";
        case 0x12:
            return "1";
        case 0x13:
            return "2";
        case 0x14:
            return "3";
        case 0x15:
            return "4";
        case 0x17:
            return "5";
        case 0x16:
            return "6";
        case 0x1A:
            return "7";
        case 0x1C:
            return "8";
        case 0x19:
            return "9";
        case 0x31:
            return "space";
        case 0x24:
            return "return";
        case 0x30:
            return "tab";
        case 0x33:
            return "backspace";
        case 0x35:
            return "escape";
        case 0x7E:
            return "up";
        case 0x7D:
            return "down";
        case 0x7B:
            return "left";
        case 0x7C:
            return "right";
        case 0x38:
            return "shift";
        case 0x3C:
            return "shift_r";
        case 0x3B:
            return "ctrl";
        case 0x3E:
            return "ctrl_r";
        case 0x3A:
            return "alt";
        case 0x3D:
            return "alt_r";
        case 0x37:
            return "cmd";
        case 0x36:
            return "cmd_r";
        case 0x39:
            return "capslock";
        case 0x2B:
            return "comma";
        case 0x2F:
            return "period";
        case 0x2C:
            return "slash";
        case 0x2A:
            return "backslash";
        case 0x29:
            return "semicolon";
        case 0x27:
            return "quote";
        case 0x21:
            return "lbracket";
        case 0x1E:
            return "rbracket";
        case 0x32:
            return "backtick";
        case 0x1B:
            return "minus";
        case 0x18:
            return "equals";
        case 0x7A:
            return "f1";
        case 0x78:
            return "f2";
        case 0x63:
            return "f3";
        case 0x76:
            return "f4";
        case 0x60:
            return "f5";
        case 0x61:
            return "f6";
        case 0x62:
            return "f7";
        case 0x64:
            return "f8";
        case 0x65:
            return "f9";
        case 0x6D:
            return "f10";
        case 0x67:
            return "f11";
        case 0x6F:
            return "f12";
        default:
            return "unknown";
    }
}

static CGEventRef il_tap_cb(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *ctx)
{
    (void) proxy;
    (void) ctx;
    if (!input_logger_is_active())
        return event;

    uint64_t t = input_logger_now_us();

    /* For any mouse event, capture the cursor location. CGEventGetLocation
     * works on every CGEvent, including clicks/scrolls — this guarantees
     * button events always carry a position context, not just motion. */
    switch (type) {
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged:
        case kCGEventLeftMouseDown:
        case kCGEventLeftMouseUp:
        case kCGEventRightMouseDown:
        case kCGEventRightMouseUp:
        case kCGEventOtherMouseDown:
        case kCGEventOtherMouseUp:
        case kCGEventScrollWheel: {
            CGPoint p = CGEventGetLocation(event);
            input_logger_push_mouse_pos(t, (int32_t) p.x, (int32_t) p.y);
            break;
        }
        default:
            break;
    }

    switch (type) {
        case kCGEventKeyDown:
        case kCGEventKeyUp: {
            CGKeyCode kc = (CGKeyCode) CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
            bool is_repeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
            /* We deliberately emit repeats as extra "down" events (matching
             * the sample which shows a long stream of w-downs while holding). */
            (void) is_repeat;
            input_logger_push_key(t, il_mac_keycode_name(kc), type == kCGEventKeyDown);
            break;
        }
        case kCGEventFlagsChanged: {
            /* Modifier key press/release. Derive down/up from current mask vs keycode. */
            CGKeyCode kc = (CGKeyCode) CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
            CGEventFlags f = CGEventGetFlags(event);
            const char *name = il_mac_keycode_name(kc);
            bool down = false;
            switch (kc) {
                case 0x38:
                case 0x3C:
                    down = (f & kCGEventFlagMaskShift) != 0;
                    break;
                case 0x3B:
                case 0x3E:
                    down = (f & kCGEventFlagMaskControl) != 0;
                    break;
                case 0x3A:
                case 0x3D:
                    down = (f & kCGEventFlagMaskAlternate) != 0;
                    break;
                case 0x37:
                case 0x36:
                    down = (f & kCGEventFlagMaskCommand) != 0;
                    break;
                case 0x39:
                    down = (f & kCGEventFlagMaskAlphaShift) != 0;
                    break;
                default:
                    break;
            }
            input_logger_push_key(t, name, down);
            break;
        }
        case kCGEventMouseMoved:
        case kCGEventLeftMouseDragged:
        case kCGEventRightMouseDragged:
        case kCGEventOtherMouseDragged: {
            int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
            int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
            input_logger_push_mouse_move(t, (int32_t) dx, (int32_t) dy);
            /* Absolute position already emitted by the top-level mouse-event
             * pos switch above; no need to repeat here. */
            break;
        }
        case kCGEventLeftMouseDown:
            input_logger_push_mouse_button(t, "mouse_left", true);
            break;
        case kCGEventLeftMouseUp:
            input_logger_push_mouse_button(t, "mouse_left", false);
            break;
        case kCGEventRightMouseDown:
            input_logger_push_mouse_button(t, "mouse_right", true);
            break;
        case kCGEventRightMouseUp:
            input_logger_push_mouse_button(t, "mouse_right", false);
            break;
        case kCGEventOtherMouseDown: {
            int64_t btn = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            const char *n = (btn == 2) ? "mouse_middle" : (btn == 3 ? "mouse_x1" : "mouse_x2");
            input_logger_push_mouse_button(t, n, true);
            break;
        }
        case kCGEventOtherMouseUp: {
            int64_t btn = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
            const char *n = (btn == 2) ? "mouse_middle" : (btn == 3 ? "mouse_x1" : "mouse_x2");
            input_logger_push_mouse_button(t, n, false);
            break;
        }
        case kCGEventScrollWheel: {
            int64_t dy = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
            int64_t dx = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2);
            input_logger_push_mouse_wheel(t, (int32_t) dx, (int32_t) dy);
            break;
        }
        /* Re-enable our tap if the OS disabled it (e.g. timeout/userinput). */
        case kCGEventTapDisabledByTimeout:
        case kCGEventTapDisabledByUserInput:
            if (g_tap)
                CGEventTapEnable(g_tap, true);
            break;
        default:
            break;
    }

    return event; /* Listen-only; never modify the stream. */
}

static void *il_hooks_thread(void *arg)
{
    (void) arg;
    @autoreleasepool {
        CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
                           CGEventMaskBit(kCGEventFlagsChanged) | CGEventMaskBit(kCGEventMouseMoved) |
                           CGEventMaskBit(kCGEventLeftMouseDragged) | CGEventMaskBit(kCGEventRightMouseDragged) |
                           CGEventMaskBit(kCGEventOtherMouseDragged) | CGEventMaskBit(kCGEventLeftMouseDown) |
                           CGEventMaskBit(kCGEventLeftMouseUp) | CGEventMaskBit(kCGEventRightMouseDown) |
                           CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventOtherMouseDown) |
                           CGEventMaskBit(kCGEventOtherMouseUp) | CGEventMaskBit(kCGEventScrollWheel);

        g_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionListenOnly, mask,
                                 il_tap_cb, NULL);
        if (!g_tap) {
            obs_log(
                LOG_ERROR,
                "CGEventTapCreate failed — grant OBS 'Input Monitoring' (and Accessibility) permission in System Settings › Privacy & Security.");
            os_atomic_store_bool(&g_running, false);
            return NULL;
        }

        g_src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, g_tap, 0);
        g_runloop = CFRunLoopGetCurrent();
        CFRunLoopAddSource(g_runloop, g_src, kCFRunLoopCommonModes);
        CGEventTapEnable(g_tap, true);

        while (os_atomic_load_bool(&g_running)) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.2, true);
        }

        CGEventTapEnable(g_tap, false);
        CFRunLoopRemoveSource(g_runloop, g_src, kCFRunLoopCommonModes);
        CFRelease(g_src);
        g_src = NULL;
        CFRelease(g_tap);
        g_tap = NULL;
        g_runloop = NULL;
    }
    return NULL;
}

bool input_logger_hooks_start(void)
{
    if (os_atomic_set_bool(&g_running, true))
        return true;
    if (pthread_create(&g_thr, NULL, il_hooks_thread, NULL) != 0) {
        os_atomic_store_bool(&g_running, false);
        return false;
    }
    return true;
}

void input_logger_hooks_stop(void)
{
    if (!os_atomic_set_bool(&g_running, false))
        return;
    /* Nudge the run loop so it observes the flag promptly. */
    if (g_runloop)
        CFRunLoopWakeUp(g_runloop);
    pthread_join(g_thr, NULL);
}

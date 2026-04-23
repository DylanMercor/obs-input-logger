/*
 * Windows input hooks (WH_KEYBOARD_LL + WH_MOUSE_LL).
 *
 * Low-level hooks require a thread with a running message pump. We spin one
 * up and install both hooks from it so the UI/render threads are untouched.
 */

#include <windows.h>

#include "input-logger.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <util/threading.h>

static HANDLE g_thr = NULL;
static DWORD g_tid = 0;
static HHOOK g_kb = NULL, g_ms = NULL;
static volatile bool g_running = false;

static const char *il_vk_name(DWORD vk)
{
	/* Printable letters & digits */
	if (vk >= 'A' && vk <= 'Z') {
		static const char *n[26] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
					    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
		return n[vk - 'A'];
	}
	if (vk >= '0' && vk <= '9') {
		static const char *n[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
		return n[vk - '0'];
	}
	switch (vk) {
	case VK_SPACE:
		return "space";
	case VK_RETURN:
		return "return";
	case VK_TAB:
		return "tab";
	case VK_BACK:
		return "backspace";
	case VK_ESCAPE:
		return "escape";
	case VK_UP:
		return "up";
	case VK_DOWN:
		return "down";
	case VK_LEFT:
		return "left";
	case VK_RIGHT:
		return "right";
	case VK_LSHIFT:
	case VK_SHIFT:
		return "shift";
	case VK_RSHIFT:
		return "shift_r";
	case VK_LCONTROL:
	case VK_CONTROL:
		return "ctrl";
	case VK_RCONTROL:
		return "ctrl_r";
	case VK_LMENU:
	case VK_MENU:
		return "alt";
	case VK_RMENU:
		return "alt_r";
	case VK_LWIN:
		return "win";
	case VK_RWIN:
		return "win_r";
	case VK_CAPITAL:
		return "capslock";
	case VK_OEM_COMMA:
		return "comma";
	case VK_OEM_PERIOD:
		return "period";
	case VK_OEM_2:
		return "slash";
	case VK_OEM_5:
		return "backslash";
	case VK_OEM_1:
		return "semicolon";
	case VK_OEM_7:
		return "quote";
	case VK_OEM_4:
		return "lbracket";
	case VK_OEM_6:
		return "rbracket";
	case VK_OEM_3:
		return "backtick";
	case VK_OEM_MINUS:
		return "minus";
	case VK_OEM_PLUS:
		return "equals";
	case VK_F1:
		return "f1";
	case VK_F2:
		return "f2";
	case VK_F3:
		return "f3";
	case VK_F4:
		return "f4";
	case VK_F5:
		return "f5";
	case VK_F6:
		return "f6";
	case VK_F7:
		return "f7";
	case VK_F8:
		return "f8";
	case VK_F9:
		return "f9";
	case VK_F10:
		return "f10";
	case VK_F11:
		return "f11";
	case VK_F12:
		return "f12";
	default:
		return "unknown";
	}
}

static LRESULT CALLBACK il_kb_proc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && input_logger_is_active()) {
		KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;
		bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
		if (down || up)
			input_logger_push_key(input_logger_now_us(), il_vk_name(k->vkCode), down);
	}
	return CallNextHookEx(NULL, code, wParam, lParam);
}

/* We use absolute cursor deltas between successive MOUSE_LL callbacks.
 * This mirrors the sample's "dx/dy" semantics (relative motion per event). */
static LONG g_last_x = 0, g_last_y = 0;
static bool g_have_last = false;

static LRESULT CALLBACK il_ms_proc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code == HC_ACTION && input_logger_is_active()) {
		MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lParam;
		uint64_t t = input_logger_now_us();
		switch (wParam) {
		case WM_MOUSEMOVE: {
			if (g_have_last) {
				int32_t dx = (int32_t)(m->pt.x - g_last_x);
				int32_t dy = (int32_t)(m->pt.y - g_last_y);
				input_logger_push_mouse_move(t, dx, dy);
			}
			g_last_x = m->pt.x;
			g_last_y = m->pt.y;
			g_have_last = true;
			break;
		}
		case WM_LBUTTONDOWN:
			input_logger_push_mouse_button(t, "mouse_left", true);
			break;
		case WM_LBUTTONUP:
			input_logger_push_mouse_button(t, "mouse_left", false);
			break;
		case WM_RBUTTONDOWN:
			input_logger_push_mouse_button(t, "mouse_right", true);
			break;
		case WM_RBUTTONUP:
			input_logger_push_mouse_button(t, "mouse_right", false);
			break;
		case WM_MBUTTONDOWN:
			input_logger_push_mouse_button(t, "mouse_middle", true);
			break;
		case WM_MBUTTONUP:
			input_logger_push_mouse_button(t, "mouse_middle", false);
			break;
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP: {
			WORD xb = HIWORD(m->mouseData);
			const char *n = (xb == XBUTTON1) ? "mouse_x1" : "mouse_x2";
			input_logger_push_mouse_button(t, n, wParam == WM_XBUTTONDOWN);
			break;
		}
		case WM_MOUSEWHEEL: {
			short d = (short)HIWORD(m->mouseData);
			input_logger_push_mouse_wheel(t, 0, d / WHEEL_DELTA);
			break;
		}
		case WM_MOUSEHWHEEL: {
			short d = (short)HIWORD(m->mouseData);
			input_logger_push_mouse_wheel(t, d / WHEEL_DELTA, 0);
			break;
		}
		}
	}
	return CallNextHookEx(NULL, code, wParam, lParam);
}

static DWORD WINAPI il_hooks_thread(LPVOID p)
{
	(void)p;
	HINSTANCE hinst = GetModuleHandleW(NULL);
	g_kb = SetWindowsHookExW(WH_KEYBOARD_LL, il_kb_proc, hinst, 0);
	g_ms = SetWindowsHookExW(WH_MOUSE_LL, il_ms_proc, hinst, 0);
	if (!g_kb || !g_ms) {
		obs_log(LOG_ERROR, "SetWindowsHookEx failed (gle=%lu)", GetLastError());
	}
	MSG msg;
	while (os_atomic_load_bool(&g_running)) {
		/* PeekMessage with timed sleep keeps the thread cheap and responsive. */
		if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		} else {
			MsgWaitForMultipleObjects(0, NULL, FALSE, 200, QS_ALLINPUT);
		}
	}
	if (g_kb)
		UnhookWindowsHookEx(g_kb);
	if (g_ms)
		UnhookWindowsHookEx(g_ms);
	g_kb = g_ms = NULL;
	return 0;
}

bool input_logger_hooks_start(void)
{
	if (os_atomic_set_bool(&g_running, true))
		return true;
	g_have_last = false;
	g_thr = CreateThread(NULL, 0, il_hooks_thread, NULL, 0, &g_tid);
	if (!g_thr) {
		os_atomic_store_bool(&g_running, false);
		return false;
	}
	return true;
}

void input_logger_hooks_stop(void)
{
	if (!os_atomic_set_bool(&g_running, false))
		return;
	/* Post a null message so the message pump wakes. */
	if (g_tid)
		PostThreadMessageW(g_tid, WM_NULL, 0, 0);
	if (g_thr) {
		WaitForSingleObject(g_thr, 2000);
		CloseHandle(g_thr);
		g_thr = NULL;
	}
}

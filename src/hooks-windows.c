/*
 * Windows input hooks.
 *
 * Two sources running on a dedicated thread with its own message pump:
 *
 * 1. Mouse:    Raw Input (WM_INPUT) via a hidden message-only window.
 *              Gives true HID device deltas (lLastX/lLastY) that bypass
 *              Windows' pointer acceleration and work when games put the
 *              mouse into relative / captured mode. `WH_MOUSE_LL` is
 *              *not* used — it reports cursor-space pixels and breaks
 *              whenever a game grabs the cursor.
 *
 * 2. Keyboard: WH_KEYBOARD_LL low-level hook. Keyboards don't have the
 *              acceleration / capture problem, and the LL hook gives a
 *              stable cross-layout vkCode. Auto-repeat is de-duped by
 *              the logger core, so the spam disappears there.
 *
 * Everything runs off the OBS UI/render threads. The callbacks do zero
 * allocation and zero formatting — they enqueue events into the logger's
 * lock-free ring buffer.
 */

#include <windows.h>

#include "input-logger.h"
#include "plugin-support.h"
#include <obs-module.h>
#include <util/threading.h>

#ifndef HID_USAGE_PAGE_GENERIC
#define HID_USAGE_PAGE_GENERIC ((USHORT)0x01)
#endif
#ifndef HID_USAGE_GENERIC_MOUSE
#define HID_USAGE_GENERIC_MOUSE ((USHORT)0x02)
#endif

static HANDLE g_thr = NULL;
static DWORD g_tid = 0;
static HHOOK g_kb = NULL;
static HWND g_rawin_hwnd = NULL;
static volatile bool g_running = false;

static const char *il_vk_name(DWORD vk)
{
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

/* --- Raw Input mouse --- */

static void il_handle_raw_mouse(const RAWMOUSE *m)
{
	if (!input_logger_is_active())
		return;
	uint64_t t = input_logger_now_us();

	/* Buttons — one flag per (down, up) transition. */
	USHORT bf = m->usButtonFlags;
	if (bf & RI_MOUSE_LEFT_BUTTON_DOWN)
		input_logger_push_mouse_button(t, "mouse_left", true);
	if (bf & RI_MOUSE_LEFT_BUTTON_UP)
		input_logger_push_mouse_button(t, "mouse_left", false);
	if (bf & RI_MOUSE_RIGHT_BUTTON_DOWN)
		input_logger_push_mouse_button(t, "mouse_right", true);
	if (bf & RI_MOUSE_RIGHT_BUTTON_UP)
		input_logger_push_mouse_button(t, "mouse_right", false);
	if (bf & RI_MOUSE_MIDDLE_BUTTON_DOWN)
		input_logger_push_mouse_button(t, "mouse_middle", true);
	if (bf & RI_MOUSE_MIDDLE_BUTTON_UP)
		input_logger_push_mouse_button(t, "mouse_middle", false);
	if (bf & RI_MOUSE_BUTTON_4_DOWN)
		input_logger_push_mouse_button(t, "mouse_x1", true);
	if (bf & RI_MOUSE_BUTTON_4_UP)
		input_logger_push_mouse_button(t, "mouse_x1", false);
	if (bf & RI_MOUSE_BUTTON_5_DOWN)
		input_logger_push_mouse_button(t, "mouse_x2", true);
	if (bf & RI_MOUSE_BUTTON_5_UP)
		input_logger_push_mouse_button(t, "mouse_x2", false);

	if (bf & RI_MOUSE_WHEEL) {
		short d = (short)m->usButtonData;
		input_logger_push_mouse_wheel(t, 0, d / WHEEL_DELTA);
	}
	if (bf & RI_MOUSE_HWHEEL) {
		short d = (short)m->usButtonData;
		input_logger_push_mouse_wheel(t, d / WHEEL_DELTA, 0);
	}

	/* Motion. Raw Input reports relative deltas for normal mice
	 * (usFlags == MOUSE_MOVE_RELATIVE == 0, the default). Absolute-mode
	 * devices (touch digitizers, some VMs, Remote Desktop) report screen
	 * coords in lLastX/lLastY; our schema is strictly device-relative, so
	 * we skip those rather than emit misleading deltas. */
	if ((m->usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
		if (m->lLastX != 0 || m->lLastY != 0)
			input_logger_push_mouse_move(t, (int32_t)m->lLastX, (int32_t)m->lLastY);
	}

	/* Absolute cursor position. Raw Input doesn't carry one (it's HID-level
	 * device data), so we ask the OS for the current cursor location. This
	 * is virtual-screen pixels, so multi-monitor setups give consistent
	 * coords across the whole desktop. */
	POINT cp;
	if (GetCursorPos(&cp))
		input_logger_push_mouse_pos(t, (int32_t)cp.x, (int32_t)cp.y);
}

static LRESULT CALLBACK il_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_INPUT) {
		/* MSDN: call GetRawInputData twice. The first call with pData=NULL
		 * writes the required size into *pcbSize. Reset pcbSize between
		 * calls — GetRawInputData is documented to update it both ways. */
		UINT needed = 0;
		if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, NULL, &needed, sizeof(RAWINPUTHEADER)) == (UINT)-1)
			goto done;
		if (needed == 0 || needed > 1024) /* sanity cap; typical RAWMOUSE is ~40B */
			goto done;

		BYTE buf[1024];
		UINT cb = sizeof(buf);
		UINT got = GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &cb, sizeof(RAWINPUTHEADER));
		if (got == (UINT)-1 || got < sizeof(RAWINPUTHEADER))
			goto done;

		RAWINPUT *ri = (RAWINPUT *)buf;
		if (ri->header.dwType == RIM_TYPEMOUSE)
			il_handle_raw_mouse(&ri->data.mouse);
	done:
		/* MSDN REQUIRES DefWindowProc for WM_INPUT so the OS can close the
		 * RAWINPUT handle. Skipping this leaks kernel handles per event —
		 * at 1 kHz that crashes OBS within minutes. Return its value too. */
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool il_register_rawinput(HWND hwnd)
{
	RAWINPUTDEVICE rid = {0};
	rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
	rid.usUsage = HID_USAGE_GENERIC_MOUSE;
	rid.dwFlags = RIDEV_INPUTSINK; /* deliver events even when not focused */
	rid.hwndTarget = hwnd;
	if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
		obs_log(LOG_ERROR, "RegisterRawInputDevices failed (gle=%lu)", GetLastError());
		return false;
	}
	return true;
}

static DWORD WINAPI il_hooks_thread(LPVOID p)
{
	(void)p;
	HINSTANCE hinst = GetModuleHandleW(NULL);

	/* Hidden message-only window to receive WM_INPUT. */
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = il_wndproc;
	wc.hInstance = hinst;
	wc.lpszClassName = L"OBSInputLoggerRawInput";
	RegisterClassW(&wc);
	g_rawin_hwnd = CreateWindowExW(0, wc.lpszClassName, L"obs-input-logger", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
				       hinst, NULL);
	if (!g_rawin_hwnd) {
		obs_log(LOG_ERROR, "CreateWindow (message-only) failed (gle=%lu)", GetLastError());
	} else if (!il_register_rawinput(g_rawin_hwnd)) {
		DestroyWindow(g_rawin_hwnd);
		g_rawin_hwnd = NULL;
	}

	/* Keyboard LL hook for key events. */
	g_kb = SetWindowsHookExW(WH_KEYBOARD_LL, il_kb_proc, hinst, 0);
	if (!g_kb)
		obs_log(LOG_ERROR, "SetWindowsHookExW(WH_KEYBOARD_LL) failed (gle=%lu)", GetLastError());

	/* Message pump. */
	MSG msg;
	while (os_atomic_load_bool(&g_running)) {
		if (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		} else {
			MsgWaitForMultipleObjects(0, NULL, FALSE, 200, QS_ALLINPUT);
		}
	}

	if (g_kb) {
		UnhookWindowsHookEx(g_kb);
		g_kb = NULL;
	}
	if (g_rawin_hwnd) {
		/* Unregister Raw Input subscription. */
		RAWINPUTDEVICE rid = {0};
		rid.usUsagePage = HID_USAGE_PAGE_GENERIC;
		rid.usUsage = HID_USAGE_GENERIC_MOUSE;
		rid.dwFlags = RIDEV_REMOVE;
		rid.hwndTarget = NULL;
		RegisterRawInputDevices(&rid, 1, sizeof(rid));
		DestroyWindow(g_rawin_hwnd);
		g_rawin_hwnd = NULL;
	}
	UnregisterClassW(L"OBSInputLoggerRawInput", hinst);
	return 0;
}

bool input_logger_hooks_start(void)
{
	if (os_atomic_set_bool(&g_running, true))
		return true;
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
	if (g_tid)
		PostThreadMessageW(g_tid, WM_NULL, 0, 0);
	if (g_thr) {
		WaitForSingleObject(g_thr, 2000);
		CloseHandle(g_thr);
		g_thr = NULL;
	}
}

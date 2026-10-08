/*
 * menus.c -- a window with a menu bar, accelerators, a modal dialog
 * with an edit control, MessageBox and string resources.  Each event
 * is reported to RESULT.TXT.
 */
#include <windows.h>
#include <stdio.h>
#include "menus.h"
#include "result.h"

static HINSTANCE inst;

BOOL FAR PASCAL _export
AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	char buf[64], line[96];

	switch (msg) {
	case WM_INITDIALOG:
		SetDlgItemText(dlg, IDC_NAME, "nobody");
		return TRUE;
	case WM_COMMAND:
		if (wp == IDOK || wp == IDCANCEL) {
			GetDlgItemText(dlg, IDC_NAME, buf, sizeof buf);
			wsprintf(line, "about %s name=%s", (LPSTR)(wp == IDOK ? "ok" : "cancel"), (LPSTR)buf);
			report(line);
			EndDialog(dlg, wp);
			return TRUE;
		}
	}
	return FALSE;
}

long FAR PASCAL _export
WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	char buf[64];
	FARPROC fp;
	HMENU m;
	int r;

	switch (msg) {
	case WM_COMMAND:
		switch (wp) {
		case IDM_NEW:
			report(HIWORD(lp) == 1 ? "new accel" : "new menu");
			return 0;
		case IDM_OPEN:
			LoadString(inst, 2, buf, sizeof buf);
			r = MessageBox(hwnd, "Open a file?", buf, MB_YESNO | MB_ICONQUESTION);
			report(r == IDYES ? "open yes" : r == IDNO ? "open no" : "open other");
			return 0;
		case IDM_CHECK:
			m = GetMenu(hwnd);
			r = GetMenuState(m, IDM_CHECK, MF_BYCOMMAND) & MF_CHECKED;
			CheckMenuItem(m, IDM_CHECK, r ? MF_UNCHECKED : MF_CHECKED);
			report(r ? "check off" : "check on");
			return 0;
		case IDM_ABOUT:
			fp = MakeProcInstance((FARPROC)AboutProc, inst);
			r = DialogBox(inst, MAKEINTRESOURCE(IDD_ABOUT), hwnd, (DLGPROC)fp);
			FreeProcInstance(fp);
			wsprintf(buf, "dialog returned %d", r);
			report(buf);
			return 0;
		case IDM_EXIT:
			report("exit");
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

int PASCAL
WinMain(HINSTANCE hinst, HINSTANCE hprev, LPSTR cmd, int show)
{
	WNDCLASS wc;
	HWND hwnd;
	MSG msg;
	HACCEL acc;
	char title[32];

	inst = hinst;
	LoadString(hinst, 1, title, sizeof title);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hinst;
	wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = COLOR_WINDOW + 1;
	wc.lpszMenuName = "MainMenu";
	wc.lpszClassName = "MenusClass";
	RegisterClass(&wc);
	hwnd = CreateWindow("MenusClass", title, WS_OVERLAPPEDWINDOW, 20, 20, 400, 300, NULL, NULL, hinst, NULL);
	acc = LoadAccelerators(hinst, "MainAccel");
	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);
	report("started");
	while (GetMessage(&msg, NULL, 0, 0)) {
		if (!TranslateAccelerator(hwnd, acc, &msg)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
	return msg.wParam;
}

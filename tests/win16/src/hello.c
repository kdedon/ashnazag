/*
 * hello.c -- the Win16 environment's first test program: a window with
 * a line of text, closed by a timer.  Built with Open Watcom (-bt=windows).
 */
#include <windows.h>

static char szClass[] = "HelloClass";

long FAR PASCAL _export
WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	PAINTSTRUCT ps;
	HDC dc;
	RECT r;

	switch (msg) {
	case WM_CREATE:
		SetTimer(hwnd, 1, 500, NULL);
		return 0;
	case WM_TIMER:
		KillTimer(hwnd, 1);
		DestroyWindow(hwnd);
		return 0;
	case WM_PAINT:
		dc = BeginPaint(hwnd, &ps);
		GetClientRect(hwnd, &r);
		DrawText(dc, "Hello from Win16", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		TextOut(dc, 4, 4, "Ashnazag", 8);
		Rectangle(dc, 10, 30, 60, 60);
		MoveTo(dc, 10, 70);
		LineTo(dc, 120, 90);
		EndPaint(hwnd, &ps);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(7);
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

	if (!hprev) {
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = WndProc;
		wc.cbClsExtra = 0;
		wc.cbWndExtra = 0;
		wc.hInstance = hinst;
		wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = GetStockObject(WHITE_BRUSH);
		wc.lpszMenuName = NULL;
		wc.lpszClassName = szClass;
		if (!RegisterClass(&wc))
			return 1;
	}
	hwnd = CreateWindow(szClass, "Hello", WS_OVERLAPPEDWINDOW, 40, 40, 300, 200,
	    NULL, NULL, hinst, NULL);
	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return msg.wParam;
}

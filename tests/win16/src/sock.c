/*
 * sock.c -- WINSOCK over the host's loopback: startup, addresses, a
 * name, a connection made with blocking calls (the blocking hook running),
 * select, WSAAsyncSelect's events and an asynchronous lookup as messages.
 * Each result a line of RESULT.TXT.
 */
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include "result.h"

#define	WM_SOCK		(WM_USER + 1)
#define	WM_HOST		(WM_USER + 2)

static char line[160];
static int events, hosterr = -1;
static char hostbuf[MAXGETHOSTSTRUCT];

static void
say(char *what, long v)
{
	sprintf(line, "%s %ld", what, v);
	report(line);
}

LRESULT CALLBACK
proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_SOCK) {
		events |= WSAGETSELECTEVENT(lp);
		if (WSAGETSELECTERROR(lp))
			say("event error", WSAGETSELECTERROR(lp));
		return 0;
	}
	if (msg == WM_HOST) {
		hosterr = WSAGETASYNCERROR(lp);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wp, lp);
}

/* messages until the events have come, or about two seconds */
static void
pump(int want)
{
	MSG m;
	DWORD start = GetTickCount();

	while ((events & want) != want && GetTickCount() - start < 2000) {
		if (PeekMessage(&m, 0, 0, 0, PM_REMOVE)) {
			TranslateMessage(&m);
			DispatchMessage(&m);
		} else
			Yield();
	}
}

int PASCAL
WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show)
{
	WSADATA wd;
	WNDCLASS wc;
	HWND hwnd;
	SOCKET srv, cli, con;
	struct sockaddr_in a;
	struct hostent FAR *h;
	int len, n;
	char buf[32];
	fd_set rs;
	struct timeval tv;
	MSG m;
	DWORD start;

	memset(&wc, 0, sizeof wc);
	wc.lpfnWndProc = proc;
	wc.hInstance = hi;
	wc.lpszClassName = "socktest";
	RegisterClass(&wc);
	hwnd = CreateWindow("socktest", "sock", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, 0, 0, hi, 0);

	say("startup", WSAStartup(0x0101, &wd));
	say("version", wd.wVersion);
	say("htons", htons(0x1234));
	say("inet_addr", inet_addr("127.0.0.1") == htonl(0x7f000001L));
	{
		struct in_addr ia;
		char FAR *p;

		ia.s_addr = inet_addr("10.1.2.3");
		p = inet_ntoa(ia);
		lstrcpy(line, p);
		report(line);
	}
	h = gethostbyname("localhost");
	say("localhost", h != 0 && h->h_length == 4 && ((unsigned char FAR *)h->h_addr_list[0])[0] == 127);
	say("bad socket", socket(AF_INET, 7, 0) == INVALID_SOCKET && WSAGetLastError() == WSAESOCKTNOSUPPORT);

	srv = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = inet_addr("127.0.0.1");
	a.sin_port = 0;
	say("bind", bind(srv, (struct sockaddr FAR *)&a, sizeof a));
	say("listen", listen(srv, 2));
	len = sizeof a;
	say("getsockname", getsockname(srv, (struct sockaddr FAR *)&a, &len));
	say("port given", ntohs(a.sin_port) != 0);

	cli = socket(AF_INET, SOCK_STREAM, 0);
	say("connect", connect(cli, (struct sockaddr FAR *)&a, sizeof a));
	len = sizeof a;
	con = accept(srv, (struct sockaddr FAR *)&a, &len);
	say("accept", con != INVALID_SOCKET);
	say("send", send(cli, "hello", 5, 0));
	n = recv(con, buf, sizeof buf - 1, 0);
	buf[n > 0 ? n : 0] = 0;
	report(buf);

	FD_ZERO(&rs);
	FD_SET(con, &rs);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	say("select idle", select(0, &rs, 0, 0, &tv));

	say("async", WSAAsyncSelect(con, hwnd, WM_SOCK, FD_READ | FD_CLOSE));
	send(cli, "x", 1, 0);
	pump(FD_READ);
	say("read event", (events & FD_READ) != 0);
	say("recv", recv(con, buf, sizeof buf, 0));
	closesocket(cli);
	pump(FD_CLOSE);
	say("close event", (events & FD_CLOSE) != 0);
	say("recv end", recv(con, buf, sizeof buf, 0));
	closesocket(con);
	closesocket(srv);

	say("async lookup", WSAAsyncGetHostByName(hwnd, WM_HOST, "localhost", hostbuf, sizeof hostbuf) != 0);
	start = GetTickCount();
	while (hosterr < 0 && GetTickCount() - start < 2000)
		if (PeekMessage(&m, 0, 0, 0, PM_REMOVE))
			DispatchMessage(&m);
	say("lookup answer", hosterr == 0 && ((struct hostent FAR *)hostbuf)->h_length == 4);
	say("cleanup", WSACleanup());
	say("after cleanup", socket(AF_INET, SOCK_STREAM, 0) == INVALID_SOCKET &&
	    WSAGetLastError() == WSANOTINITIALISED);
	DestroyWindow(hwnd);
	return 0;
}

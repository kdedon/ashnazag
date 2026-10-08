/*
 * ctrls.c -- the standard controls in a dialog: edits, a list box, a
 * drop-down list, a check box, radio buttons and a scroll bar.  On OK
 * their state goes to RESULT.TXT.
 */
#include <windows.h>
#include "ctrls.h"
#include "result.h"

static char *fruit[] = { "Pear", "Apple", "Cherry", "Banana", "Fig", "Grape", "Kiwi", "Lemon", "Mango", 0 };

BOOL FAR PASCAL _export
CtrlProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
	char a[64], m[128], l[32], c[32], line[320];
	int i, pos;
	HWND sb;

	switch (msg) {
	case WM_INITDIALOG:
		for (i = 0; fruit[i]; i++) {
			SendDlgItemMessage(dlg, IDC_LIST, LB_ADDSTRING, 0, (LPARAM)(LPSTR)fruit[i]);
			SendDlgItemMessage(dlg, IDC_COMBO, CB_ADDSTRING, 0, (LPARAM)(LPSTR)fruit[i]);
		}
		SendDlgItemMessage(dlg, IDC_COMBO, CB_SETCURSEL, 0, 0);
		CheckRadioButton(dlg, IDC_RADIO1, IDC_RADIO2, IDC_RADIO1);
		sb = GetDlgItem(dlg, IDC_SCROLL);
		SetScrollRange(sb, SB_CTL, 0, 10, FALSE);
		SetScrollPos(sb, SB_CTL, 0, TRUE);
		report("init");
		return TRUE;
	case WM_HSCROLL:
		sb = (HWND)HIWORD(lp);
		pos = GetScrollPos(sb, SB_CTL);
		switch (wp) {
		case SB_LINEDOWN: pos++; break;
		case SB_LINEUP: pos--; break;
		case SB_PAGEDOWN: pos += 3; break;
		case SB_PAGEUP: pos -= 3; break;
		case SB_THUMBPOSITION: pos = LOWORD(lp); break;
		}
		if (pos < 0) pos = 0;
		if (pos > 10) pos = 10;
		SetScrollPos(sb, SB_CTL, pos, TRUE);
		SetDlgItemInt(dlg, IDC_POS, pos, FALSE);
		return TRUE;
	case WM_COMMAND:
		switch (wp) {
		case IDC_LIST:
			if (HIWORD(lp) == LBN_DBLCLK)
				report("list dblclk");
			return TRUE;
		case IDOK:
			GetDlgItemText(dlg, IDC_EDIT, a, sizeof a);
			GetDlgItemText(dlg, IDC_MEMO, m, sizeof m);
			for (i = 0; m[i]; i++)
				if (m[i] == '\r') m[i] = '|';
				else if (m[i] == '\n') m[i] = '/';
			i = (int)SendDlgItemMessage(dlg, IDC_LIST, LB_GETCURSEL, 0, 0);
			l[0] = 0;
			if (i >= 0)
				SendDlgItemMessage(dlg, IDC_LIST, LB_GETTEXT, i, (LPARAM)(LPSTR)l);
			i = (int)SendDlgItemMessage(dlg, IDC_COMBO, CB_GETCURSEL, 0, 0);
			c[0] = 0;
			if (i >= 0)
				SendDlgItemMessage(dlg, IDC_COMBO, CB_GETLBTEXT, i, (LPARAM)(LPSTR)c);
			wsprintf(line, "edit=%s memo=%s list=%s combo=%s check=%d radio=%d scroll=%d",
			    (LPSTR)a, (LPSTR)m, (LPSTR)l, (LPSTR)c, IsDlgButtonChecked(dlg, IDC_CHECK),
			    IsDlgButtonChecked(dlg, IDC_RADIO2) ? 2 : 1, GetDlgItemInt(dlg, IDC_POS, NULL, FALSE));
			report(line);
			EndDialog(dlg, 1);
			return TRUE;
		case IDCANCEL:
			report("cancel");
			EndDialog(dlg, 0);
			return TRUE;
		}
	}
	return FALSE;
}

int PASCAL
WinMain(HINSTANCE hinst, HINSTANCE hprev, LPSTR cmd, int show)
{
	FARPROC fp = MakeProcInstance((FARPROC)CtrlProc, hinst);
	int r = DialogBox(hinst, MAKEINTRESOURCE(IDD_CTRLS), NULL, (DLGPROC)fp);

	FreeProcInstance(fp);
	return r;
}

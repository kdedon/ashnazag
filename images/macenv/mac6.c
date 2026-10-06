/*
 * mac6 -- A/UX 2.0.1's startmac in its own root, /a201, as the caller.
 *
 *	mac6 [arg ...]
 *
 * Set-user-ID root only to chroot: it takes the caller's IDs back before
 * the exec.  root's System Folder there is /mac/sys/System Folder,
 * another user's /home/<login>/System Folder.  TBMEMORY defaults to 4M.
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#define	ROOT6	"/a201"
#define	STARTMAC "/mac/bin/startmac"

int
main(argc, argv)
	int argc;
	char **argv;
{
	static char home[300], sys[340], tb[360], hv[310], mv[40];
	static char *env[] = { "PATH=/mac/bin:/bin:/usr/bin", hv, tb, mv, 0 };
	uid_t uid = getuid();
	gid_t gid = getgid();
	struct passwd *pw = getpwuid(uid);
	char *m = getenv("TBMEMORY");

	if (uid == 0) {
		strcpy(home, "/");
		strcpy(sys, "/mac/sys/System Folder");
	} else {
		if (pw == 0 || strlen(pw->pw_name) > 64) {
			fprintf(stderr, "startmac6: who are you?\n");
			return 1;
		}
		sprintf(home, "/home/%s", pw->pw_name);
		sprintf(sys, "%s/System Folder", home);
	}
	if (chroot(ROOT6) < 0 || chdir("/") < 0 || setgid(gid) < 0 || setuid(uid) < 0) {
		perror("startmac6");
		return 1;
	}
	sprintf(tb, "%s/System", sys);
	if (access(tb, R_OK) < 0) {
		fprintf(stderr, "startmac6: no %s%s\n", ROOT6, sys);
		return 1;
	}
	if (m == 0 || *m == 0 || strlen(m) > 20 || strspn(m, "0123456789KMkm") != strlen(m))
		m = "4M";
	sprintf(hv, "HOME=%s", home);
	sprintf(tb, "TBSYSTEM=%s", sys);
	sprintf(mv, "TBMEMORY=%s", m);
	argv[0] = STARTMAC;
	execve(STARTMAC, argv, env);
	perror("startmac6: " ROOT6 STARTMAC);
	return 1;
}

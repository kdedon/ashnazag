/*
 * env.c - setenv / unsetenv for Amix (SVR4.0, m68k).
 *
 * Copyright (c) 2026 Jussi Alanärä.  BSD-2-Clause; see LICENSE.
 *
 * SVR4.0 has putenv(3) and nothing else.  putenv has one property that makes it
 * a poor building block and a worse direct call: IT KEEPS THE CALLER'S POINTER.
 * The environment ends up holding whatever storage the caller passed, so the
 * textbook
 *
 *     char buf[64]; sprintf(buf, "TZ=%s", z); putenv(buf);
 *
 * leaves environ pointing into a stack frame that is about to be reused.  On
 * this box the symptom is not a crash at the putenv; it is a variable that
 * reads back as garbage much later, in some unrelated call to getenv.
 *
 * So setenv() below always installs a heap copy.  The bookkeeping exists for
 * one reason: a program that setenv()s the same name in a loop would otherwise
 * leak a string per iteration, and this target has 4-128 MB of RAM.  We remember
 * the strings WE allocated, and free the previous one when replacing the same
 * name.  Strings that were already in environ when the process started are
 * never freed -- they are not ours, and on SVR4 they may not even be malloc'd.
 *
 * WHY unsetenv EDITS environ DIRECTLY.  There is no SVR4 primitive for removal:
 * putenv("NAME=") sets an EMPTY variable, which getenv() reports as present.
 * Programs that test presence rather than value (and configure scripts do) see
 * the opposite of what was intended.  Compacting the environ array in place is
 * the only thing that actually removes the entry.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>

#include "amixcompat.h"

/*
 * THE ENVIRON SYMBOL IS SPELLED WITH A LEADING UNDERSCORE HERE.
 *
 * Measured against the sysroot:
 *
 *     libc.so.1     B _environ                 <- and nothing else
 *     libc.a        D _environ,  W environ     <- both
 *     /usr/include  neither, in any header
 *
 * So `extern char **environ;` compiles fine, links fine against the STATIC
 * libc, and fails at link time against the shared one -- which is what an Amix
 * program actually links.  This is the third member of a family: gettimeofday
 * and environ are in libc.a but not libc.so.1, and strcasecmp is in libresolv.a
 * but declared in no header.  Each one makes a build probe answer a question
 * about whichever library it happened to test.
 *
 * amixcompat.h offers consumers `#define environ _environ` -- the same trick
 * <rpc/types.h> already uses for gettimeofday, so it is the platform's own
 * idiom rather than an invention.  This file does NOT use that macro: it names
 * the real symbol everywhere, because a shim that hides the spelling from its
 * own source is a shim nobody can debug.
 */
extern char **_environ;

/*
 * Strings this library handed to the environment.  A flat array with linear
 * search: a process's environment is tens of entries, and a hash table here
 * would be more code than the thing it manages.
 */
#define OWNED_MAX	64

static char	*owned[OWNED_MAX];
static int	 owned_n;

static void
owned_forget(char *p)
{
	int i;

	for (i = 0; i < owned_n; i++) {
		if (owned[i] == p) {
			owned[i] = owned[--owned_n];
			free(p);
			return;
		}
	}
}

static void
owned_remember(char *p)
{
	if (owned_n < OWNED_MAX)
		owned[owned_n++] = p;
	/*
	 * Overflow is a leak, not a corruption: past OWNED_MAX we simply stop
	 * tracking and the string stays allocated for the life of the process.
	 * Losing 64 strings' worth of bookkeeping is survivable; freeing a
	 * pointer the environment still references is not.
	 */
}

/*
 * Find the environ slot whose name matches, or -1.  The comparison stops at
 * '=' on the environ side, which is what makes "PATH" not match "PATHEXT=...".
 */
static int
env_find(const char *name, size_t namelen)
{
	int i;

	if (_environ == NULL)
		return (-1);
	for (i = 0; _environ[i] != NULL; i++) {
		if (strncmp(_environ[i], name, namelen) == 0 &&
		    _environ[i][namelen] == '=')
			return (i);
	}
	return (-1);
}

int
setenv(const char *name, const char *value, int overwrite)
{
	size_t namelen;
	size_t vallen;
	char *entry;
	int slot;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (value == NULL)
		value = "";

	namelen = strlen(name);
	slot = env_find(name, namelen);
	if (slot >= 0 && !overwrite)
		return (0);

	vallen = strlen(value);
	entry = (char *)malloc(namelen + vallen + 2);
	if (entry == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	(void)memcpy(entry, name, namelen);
	entry[namelen] = '=';
	(void)memcpy(entry + namelen + 1, value, vallen);
	entry[namelen + vallen + 1] = '\0';

	if (putenv(entry) != 0) {
		free(entry);
		errno = ENOMEM;
		return (-1);
	}

	/*
	 * putenv succeeded, so the OLD string (if it was ours) is no longer
	 * referenced by environ and can go.  Order matters: forget after
	 * putenv, never before -- if putenv had failed we would have freed a
	 * pointer the environment still held.
	 */
	if (slot >= 0) {
		/*
		 * Re-find rather than reusing `slot`: putenv may have moved or
		 * reallocated the environ array, so the old index is not
		 * trustworthy.  What we want is the pointer we previously
		 * owned for this name, and owned_forget matches by pointer.
		 */
		int i;

		for (i = 0; i < owned_n; i++) {
			if (strncmp(owned[i], name, namelen) == 0 &&
			    owned[i][namelen] == '=' && owned[i] != entry) {
				owned_forget(owned[i]);
				break;
			}
		}
	}
	owned_remember(entry);
	return (0);
}

int
unsetenv(const char *name)
{
	size_t namelen;
	int slot;
	int i;
	char *victim;

	if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
		errno = EINVAL;
		return (-1);
	}
	namelen = strlen(name);

	/*
	 * Loop, because a broken environment can hold the same name twice and
	 * removing only the first leaves getenv() still finding it.
	 */
	for (;;) {
		slot = env_find(name, namelen);
		if (slot < 0)
			break;
		victim = _environ[slot];
		for (i = slot; _environ[i] != NULL; i++)
			_environ[i] = _environ[i + 1];
		owned_forget(victim);
	}
	return (0);
}

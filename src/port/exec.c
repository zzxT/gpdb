/*-------------------------------------------------------------------------
 *
 * exec.c
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/port/exec.c,v 1.8 2004/05/18 20:18:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/wait.h>

#define _(x) gettext((x))

#include "miscadmin.h"

/* $PATH (or %PATH%) path separator */
#ifdef WIN32
#define PATHSEP ';'
#else
#define PATHSEP ':'
#endif

#ifndef S_IRUSR					/* XXX [TRH] should be in a header */
#define S_IRUSR		 S_IREAD
#define S_IWUSR		 S_IWRITE
#define S_IXUSR		 S_IEXEC
#define S_IRGRP		 ((S_IRUSR)>>3)
#define S_IWGRP		 ((S_IWUSR)>>3)
#define S_IXGRP		 ((S_IXUSR)>>3)
#define S_IROTH		 ((S_IRUSR)>>6)
#define S_IWOTH		 ((S_IWUSR)>>6)
#define S_IXOTH		 ((S_IXUSR)>>6)
#endif

static void win32_make_absolute(char *path);

/*
 * validate_exec -- validate "path" as an executable file
 *
 * returns 0 if the file is found and no error is encountered.
 *		  -1 if the regular file "path" does not exist or cannot be executed.
 *		  -2 if the file is otherwise valid but cannot be read.
 */
static int
validate_exec(char *path)
{
	struct stat buf;

#ifndef WIN32
	uid_t		euid;
	struct group *gp;
	struct passwd *pwp;
	int			i;
	int			in_grp = 0;
#else
	char		path_exe[MAXPGPATH + 2 + strlen(".exe")];
#endif
	int			is_r = 0;
	int			is_x = 0;

#ifdef WIN32
	/* Win32 requires a .exe suffix for stat() */
	if (strlen(path) >= strlen(".exe") &&
		pg_strcasecmp(path + strlen(path) - strlen(".exe"), ".exe") != 0)
	{
		strcpy(path_exe, path);
		strcat(path_exe, ".exe");
		path = path_exe;
	}
#endif

	/*
	 * Ensure that the file exists and is a regular file.
	 *
	 * XXX if you have a broken system where stat() looks at the symlink
	 * instead of the underlying file, you lose.
	 */
	if (stat(path, &buf) < 0)
	{
		fprintf(stderr, "could not stat \"%s\": %m", path);
		return -1;
	}

	if ((buf.st_mode & S_IFMT) != S_IFREG)
	{
		fprintf(stderr, "\"%s\" is not a regular file", path);
		return -1;
	}

	/*
	 * Ensure that we are using an authorized executable.
	 */

	/*
	 * Ensure that the file is both executable and readable (required for
	 * dynamic loading).
	 */
#ifdef WIN32
	is_r = buf.st_mode & S_IRUSR;
	is_x = buf.st_mode & S_IXUSR;
	return is_x ? (is_r ? 0 : -2) : -1;
#else
	euid = geteuid();

	/* If owned by us, just check owner bits */
	if (euid == buf.st_uid)
	{
		is_r = buf.st_mode & S_IRUSR;
		is_x = buf.st_mode & S_IXUSR;
		if (!(is_r && is_x))
			fprintf(stderr, "\"%s\" is not user read/execute", path);
		return is_x ? (is_r ? 0 : -2) : -1;
	}

	/* OK, check group bits */
	
	pwp = getpwuid(euid);	/* not thread-safe */
	if (pwp)
	{
		if (pwp->pw_gid == buf.st_gid)	/* my primary group? */
			++in_grp;
		else if (pwp->pw_name &&
				 (gp = getgrgid(buf.st_gid)) != NULL && /* not thread-safe */
				 gp->gr_mem != NULL)
		{	/* try list of member groups */
			for (i = 0; gp->gr_mem[i]; ++i)
			{
				if (!strcmp(gp->gr_mem[i], pwp->pw_name))
				{
					++in_grp;
					break;
				}
			}
		}
		if (in_grp)
		{
			is_r = buf.st_mode & S_IRGRP;
			is_x = buf.st_mode & S_IXGRP;
			if (!(is_r && is_x))
				fprintf(stderr, "\"%s\" is not group read/execute", path);
			return is_x ? (is_r ? 0 : -2) : -1;
		}
	}

	/* Check "other" bits */
	is_r = buf.st_mode & S_IROTH;
	is_x = buf.st_mode & S_IXOTH;
	if (!(is_r && is_x))
		fprintf(stderr, "\"%s\" is not other read/execute", path);
	return is_x ? (is_r ? 0 : -2) : -1;

#endif
}

/*
 * find_my_exec -- find an absolute path to a valid executable
 *
 * The reason we have to work so hard to find an absolute path is that
 * on some platforms we can't do dynamic loading unless we know the
 * executable's location.  Also, we need a full path not a relative
 * path because we will later change working directory.
 *
 * This function is not thread-safe because of it calls validate_exec(),
 * which calls getgrgid().  This function should be used only in
 * non-threaded binaries, not in library routines.
 */
int
find_my_exec(const char *argv0, char *full_path)
{
	char		buf[MAXPGPATH + 2];
	char	   *p;
	char	   *path,
			   *startp,
			   *endp;

	/*
	 * First try: use the binary that's located in the
	 * same directory if it was invoked with an explicit path.
	 * Presumably the user used an explicit path because it
	 * wasn't in PATH, and we don't want to use incompatible executables.
	 *
	 * For the binary: First try: if we're given some kind of path, use it
	 * (making sure that a relative path is made absolute before returning
	 * it).
	 */
	/* Does argv0 have a separator? */
	if (argv0 && (p = last_path_separator(argv0)))
	{
		if (*++p == '\0')
		{
			fprintf(stderr, "argv[0] ends with a path separator \"%s\"", argv0);
			return -1;
		}
		if (is_absolute_path(argv0) || !getcwd(buf, MAXPGPATH))
			buf[0] = '\0';
		else	/* path is not absolute and getcwd worked */
			strcat(buf, "/");
		strcat(buf, argv0);
		if (validate_exec(buf) == 0)
		{
			strncpy(full_path, buf, MAXPGPATH);
			win32_make_absolute(full_path);
			return 0;
		}
		else
		{
			fprintf(stderr, "invalid binary \"%s\"", buf);
			return -1;
		}
	}

	/*
	 * Second try: since no explicit path was supplied, the user must have
	 * been relying on PATH.  We'll use the same PATH.
	 */
	if ((p = getenv("PATH")) && *p)
	{
		path = strdup(p);		/* make a modifiable copy */
		for (startp = path, endp = strchr(path, PATHSEP);
			 startp && *startp;
			 startp = endp + 1, endp = strchr(startp, PATHSEP))
		{
			if (startp == endp) /* it's a "::" */
				continue;
			if (endp)
				*endp = '\0';
			if (is_absolute_path(startp) || !getcwd(buf, MAXPGPATH))
				buf[0] = '\0';
			else	/* path is not absolute and getcwd worked */
				strcat(buf, "/");
			strcat(buf, startp);
			strcat(buf, "/");
			strcat(buf, argv0);
			switch (validate_exec(buf))
			{
				case 0: /* found ok */
					strncpy(full_path, buf, MAXPGPATH);
					win32_make_absolute(full_path);
					free(path);
					return 0;
				case -1:		/* wasn't even a candidate, keep looking */
					break;
				case -2:		/* found but disqualified */
					fprintf(stderr, "could not read binary \"%s\"", buf);
					free(path);
					return -1;
			}
			if (!endp)			/* last one */
				break;
		}
		free(path);
	}

	fprintf(stderr, "could not find a \"%s\" to execute", argv0);
	return -1;

#if 0
	/*
	 *	Win32 has a native way to find the executable name, but the above
	 *	method works too.
	 */
	if (GetModuleFileName(NULL,basename,MAXPGPATH) == 0)
		ereport(FATAL,
				(errmsg("GetModuleFileName failed (%i)",(int)GetLastError())));
#endif
}


/*
 * Find our binary directory, then make sure the "target" executable
 * is the proper version.
 */
int find_other_exec(const char *argv0, char const *target,
					const char *versionstr, char *retpath)
{
	char		cmd[MAXPGPATH];
	char		line[100];
	FILE	   *pgver;

	if (find_my_exec(argv0, retpath) < 0)
		return -1;

	/* Trim off program name and keep just directory */	
	*last_path_separator(retpath) = '\0';

	snprintf(retpath + strlen(retpath), MAXPGPATH - strlen(retpath),
			 "/%s%s", target, EXE);

	if (validate_exec(retpath))
		return -1;
	
	snprintf(cmd, sizeof(cmd), "\"%s\" -V 2>%s", retpath, DEVNULL);

	/* flush output buffers in case popen does not... */
	fflush(stdout);
	fflush(stderr);

	if ((pgver = popen(cmd, "r")) == NULL)
		return -1;

	if (fgets(line, sizeof(line), pgver) == NULL)
		perror("fgets failure");

	if (pclose_check(pgver))
		return -1;

	if (strcmp(line, versionstr) != 0)
		return -2;

	return 0;
}


/*
 * pclose() plus useful error reporting
 * Is this necessary?  bjm 2004-05-11
 * It is better here because pipe.c has win32 backend linkage.
 */
int
pclose_check(FILE *stream)
{
	int		exitstatus;

	exitstatus = pclose(stream);

	if (exitstatus == 0)
		return 0;					/* all is well */

	if (exitstatus == -1)
	{
		/* pclose() itself failed, and hopefully set errno */
		perror("pclose failed");
	}
	else if (WIFEXITED(exitstatus))
	{
		fprintf(stderr, _("child process exited with exit code %d\n"),
				WEXITSTATUS(exitstatus));
	}
	else if (WIFSIGNALED(exitstatus))
	{
		fprintf(stderr, _("child process was terminated by signal %d\n"),
				WTERMSIG(exitstatus));
	}
	else
	{
		fprintf(stderr, _("child process exited with unrecognized status %d\n"),
				exitstatus);
	}

	return -1;
}


/*
 * Windows doesn't like relative paths to executables (other things work fine)
 * so we call its builtin function to expand them. Elsewhere this is a NOOP
 *
 * Returns malloc'ed memory.
 */
static void
win32_make_absolute(char *path)
{
#ifdef WIN32
	char		abspath[MAXPGPATH];

	if (_fullpath(abspath, path, MAXPGPATH) == NULL)
	{
		fprintf(stderr, "Win32 path expansion failed:  %s", strerror(errno));
		StrNCpy(abspath, path, MAXPGPATH);
	}
	canonicalize_path(abspath);

	StrNCpy(path, abspath, MAXPGPATH);
#endif
	return;
}


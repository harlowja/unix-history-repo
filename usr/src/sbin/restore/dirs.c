/* Copyright (c) 1983 Regents of the University of California */

#ifndef lint
static char sccsid[] = "@(#)dirs.c	3.8	(Berkeley)	83/04/19";
#endif

#include "restore.h"
#include <dumprestor.h>
#include <sys/file.h>
#include <dir.h>

/*
 * Symbol table of directories read from tape.
 */
#define HASHSIZE	1000
#define INOHASH(val) (val % HASHSIZE)
struct inotab {
	struct inotab *t_next;
	ino_t	t_ino;
	daddr_t	t_seekpt;
	long t_size;
};
static struct inotab *inotab[HASHSIZE];
extern struct inotab *inotablookup();
extern struct inotab *allocinotab();

/*
 * Information retained about directories.
 */
struct modeinfo {
	ino_t ino;
	time_t timep[2];
	short mode;
	short uid;
	short gid;
};

/*
 * Global variables for this file.
 */
static daddr_t	seekpt;
static FILE	*df, *mf;
static DIR	*dirp;
static char	dirfile[32] = "#";	/* No file */
static char	modefile[32] = "#";	/* No file */
extern ino_t	search();

/*
 * Format of old style directories.
 */
#define ODIRSIZ 14
struct odirect {
	u_short	d_ino;
	char	d_name[ODIRSIZ];
};

/*
 * Structure and routines associated with listing directories.
 */
struct afile {
	ino_t	fnum;		/* inode number of file */
	char	*fname;		/* file name */
	short	fflags;		/* extraction flags, if any */
	char	ftype;		/* file type, e.g. LEAF or NODE */
};
extern int fcmp();
extern char *fmtentry();

/*
 *	Extract directory contents, building up a directory structure
 *	on disk for extraction by name.
 *	If genmode is requested, save mode, owner, and times for all
 *	directories on the tape.
 */
extractdirs(genmode)
	int genmode;
{
	register int i;
	register struct dinode *ip;
	struct inotab *itp;
	struct direct nulldir;
	int putdir(), null();

	vprintf(stdout, "Extract directories from tape\n");
	(void) sprintf(dirfile, "/tmp/rstdir%d", dumpdate);
	df = fopen(dirfile, "w");
	if (df == 0) {
		fprintf(stderr,
		    "restor: %s - cannot create directory temporary\n",
		    dirfile);
		perror("fopen");
		done(1);
	}
	if (genmode != 0) {
		(void) sprintf(modefile, "/tmp/rstmode%d", dumpdate);
		mf = fopen(modefile, "w");
		if (mf == 0) {
			fprintf(stderr,
			    "restor: %s - cannot create modefile \n",
			    modefile);
			perror("fopen");
			done(1);
		}
	}
	nulldir.d_ino = 0;
	nulldir.d_namlen = 1;
	(void) strncpy(nulldir.d_name, "/", (int)nulldir.d_namlen);
	nulldir.d_reclen = DIRSIZ(&nulldir);
	for (;;) {
		curfile.name = "<directory file - name unknown>";
		curfile.action = USING;
		ip = curfile.dip;
		i = ip->di_mode & IFMT;
		if (i != IFDIR) {
			(void) fclose(df);
			dirp = opendir(dirfile);
			if (dirp == NULL)
				perror("opendir");
			if (mf != NULL)
				(void) fclose(mf);
			i = dirlookup(".");
			if (i == 0)
				panic("Root directory is not on tape\n");
			return;
		}
		itp = allocinotab(curfile.ino, ip, seekpt);
		getfile(putdir, null);
		putent(&nulldir);
		flushent();
		itp->t_size = seekpt - itp->t_seekpt;
	}
}

/*
 * skip over all the directories on the tape
 */
skipdirs()
{

	while ((curfile.dip->di_mode & IFMT) == IFDIR) {
		skipfile();
	}
}

/*
 *	Recursively find names and inumbers of all files in subtree 
 *	pname and pass them off to be processed.
 */
treescan(pname, ino, todo)
	char *pname;
	ino_t ino;
	long (*todo)();
{
	register struct inotab *itp;
	int namelen;
	daddr_t bpt;
	register struct direct *dp;
	char locname[MAXPATHLEN + 1];

	itp = inotablookup(ino);
	if (itp == NULL) {
		/*
		 * Pname is name of a simple file or an unchanged directory.
		 */
		(void) (*todo)(pname, ino, LEAF);
		return;
	}
	/*
	 * Pname is a dumped directory name.
	 */
	if ((*todo)(pname, ino, NODE) == FAIL)
		return;
	/*
	 * begin search through the directory
	 * skipping over "." and ".."
	 */
	(void) strncpy(locname, pname, MAXPATHLEN);
	(void) strncat(locname, "/", MAXPATHLEN);
	namelen = strlen(locname);
	seekdir(dirp, itp->t_seekpt, itp->t_seekpt);
	dp = readdir(dirp); /* "." */
	dp = readdir(dirp); /* ".." */
	dp = readdir(dirp); /* first real entry */
	bpt = telldir(dirp);
	/*
	 * "/" signals end of directory
	 */
	while (dp != NULL && !(dp->d_namlen == 1 && dp->d_name[0] == '/')) {
		locname[namelen] = '\0';
		if (namelen + dp->d_namlen >= MAXPATHLEN) {
			fprintf(stderr, "%s%s: name exceeds %d char\n",
				locname, dp->d_name, MAXPATHLEN);
		} else {
			(void) strncat(locname, dp->d_name, (int)dp->d_namlen);
			treescan(locname, dp->d_ino, todo);
			seekdir(dirp, bpt, itp->t_seekpt);
		}
		dp = readdir(dirp);
		bpt = telldir(dirp);
	}
	if (dp == NULL)
		fprintf(stderr, "corrupted directory: %s.\n", locname);
}

/*
 * Search the directory tree rooted at inode ROOTINO
 * for the path pointed at by n
 */
ino_t
psearch(n)
	char	*n;
{
	register char *cp, *cp1;
	ino_t ino;
	char c;

	ino = ROOTINO;
	if (*(cp = n) == '/')
		cp++;
next:
	cp1 = cp + 1;
	while (*cp1 != '/' && *cp1)
		cp1++;
	c = *cp1;
	*cp1 = 0;
	ino = search(ino, cp);
	if (ino == 0) {
		*cp1 = c;
		return(0);
	}
	*cp1 = c;
	if (c == '/') {
		cp = cp1+1;
		goto next;
	}
	return(ino);
}

/*
 * search the directory inode ino
 * looking for entry cp
 */
ino_t
search(inum, cp)
	ino_t	inum;
	char	*cp;
{
	register struct direct *dp;
	register struct inotab *itp;
	int len;

	itp = inotablookup(inum);
	if (itp == NULL)
		return(0);
	seekdir(dirp, itp->t_seekpt, itp->t_seekpt);
	len = strlen(cp);
	do {
		dp = readdir(dirp);
		if (dp->d_namlen == 1 && dp->d_name[0] == '/')
			return(0);
	} while (dp->d_namlen != len || strncmp(dp->d_name, cp, len) != 0);
	return(dp->d_ino);
}

/*
 * Put the directory entries in the directory file
 */
putdir(buf, size)
	char *buf;
	int size;
{
	struct direct cvtbuf;
	register struct odirect *odp;
	struct odirect *eodp;
	register struct direct *dp;
	long loc, i;

	if (cvtflag) {
		eodp = (struct odirect *)&buf[size];
		for (odp = (struct odirect *)buf; odp < eodp; odp++)
			if (odp->d_ino != 0) {
				dcvt(odp, &cvtbuf);
				putent(&cvtbuf);
			}
	} else {
		for (loc = 0; loc < size; ) {
			dp = (struct direct *)(buf + loc);
			i = DIRBLKSIZ - (loc & (DIRBLKSIZ - 1));
			if (dp->d_reclen == 0 || dp->d_reclen > i) {
				loc += i;
				continue;
			}
			loc += dp->d_reclen;
			if (dp->d_ino != 0) {
				putent(dp);
			}
		}
	}
}

/*
 * These variables are "local" to the following two functions.
 */
char dirbuf[DIRBLKSIZ];
long dirloc = 0;
long prev = 0;

/*
 * add a new directory entry to a file.
 */
putent(dp)
	struct direct *dp;
{
	dp->d_reclen = DIRSIZ(dp);
	if (dirloc + dp->d_reclen > DIRBLKSIZ) {
		((struct direct *)(dirbuf + prev))->d_reclen =
		    DIRBLKSIZ - prev;
		(void) fwrite(dirbuf, 1, DIRBLKSIZ, df);
		dirloc = 0;
	}
	bcopy((char *)dp, dirbuf + dirloc, (long)dp->d_reclen);
	prev = dirloc;
	dirloc += dp->d_reclen;
}

/*
 * flush out a directory that is finished.
 */
flushent()
{

	((struct direct *)(dirbuf + prev))->d_reclen = DIRBLKSIZ - prev;
	(void) fwrite(dirbuf, (int)dirloc, 1, df);
	seekpt = ftell(df);
	dirloc = 0;
}

dcvt(odp, ndp)
	register struct odirect *odp;
	register struct direct *ndp;
{

	bzero((char *)ndp, (long)(sizeof *ndp));
	ndp->d_ino =  odp->d_ino;
	(void) strncpy(ndp->d_name, odp->d_name, ODIRSIZ);
	ndp->d_namlen = strlen(ndp->d_name);
	ndp->d_reclen = DIRSIZ(ndp);
}

/*
 * Seek to an entry in a directory.
 * Only values returned by ``telldir'' should be passed to seekdir.
 * This routine handles many directories in a single file.
 * It takes the base of the directory in the file, plus
 * the desired seek offset into it.
 */
void
seekdir(dirp, loc, base)
	register DIR *dirp;
	daddr_t loc, base;
{

	if (loc == telldir(dirp))
		return;
	loc -= base;
	if (loc < 0)
		fprintf(stderr, "bad seek pointer to seekdir %d\n", loc);
	(void) lseek(dirp->dd_fd, base + (loc & ~(DIRBLKSIZ - 1)), 0);
	dirp->dd_loc = loc & (DIRBLKSIZ - 1);
	if (dirp->dd_loc != 0)
		dirp->dd_size = read(dirp->dd_fd, dirp->dd_buf, DIRBLKSIZ);
}

/*
 * get next entry in a directory.
 */
struct direct *
readdir(dirp)
	register DIR *dirp;
{
	register struct direct *dp;

	for (;;) {
		if (dirp->dd_loc == 0) {
			dirp->dd_size = read(dirp->dd_fd, dirp->dd_buf, 
			    DIRBLKSIZ);
			if (dirp->dd_size <= 0)
				return NULL;
		}
		if (dirp->dd_loc >= dirp->dd_size) {
			dirp->dd_loc = 0;
			continue;
		}
		dp = (struct direct *)(dirp->dd_buf + dirp->dd_loc);
		if (dp->d_reclen == 0 ||
		    dp->d_reclen > DIRBLKSIZ + 1 - dirp->dd_loc)
			return NULL;
		dirp->dd_loc += dp->d_reclen;
		return (dp);
	}
}

/*
 * Set the mode, owner, and times for all new or changed directories
 */
setdirmodes()
{
	FILE *mf;
	struct modeinfo node;
	struct entry *ep;
	char *cp;
	
	vprintf(stdout, "Set directory mode, owner, and times.\n");
	mf = fopen(modefile, "r");
	if (mf == NULL) {
		perror("fopen");
		panic("cannot open mode file %s\n", modefile);
	}
	clearerr(mf);
	for (;;) {
		(void) fread((char *)&node, 1, sizeof(struct modeinfo), mf);
		if (feof(mf))
			break;
		ep = lookupino(node.ino);
		if (ep == NIL || (ep->e_flags & NEW) == 0) {
			if (command != 'r' && command != 'R')
				continue;
			panic("cannot find directory inode %d\n", node.ino);
		}
		cp = myname(ep);
		(void) chown(cp, node.uid, node.gid);
		(void) chmod(cp, node.mode);
		utime(cp, node.timep);
		ep->e_flags &= ~NEW;
	}
	if (ferror(mf))
		panic("error setting directory modes\n");
	(void) fclose(mf);
}

/*
 * Generate a literal copy of a directory.
 */
genliteraldir(name, ino)
	char *name;
	ino_t ino;
{
	register struct inotab *itp;
	int ofile, dp, i, size;
	char buf[BUFSIZ];

	itp = inotablookup(ino);
	if (itp == NULL)
		panic("Cannot find directory inode %d named %s\n", ino, name);
	if ((ofile = open(name, FWRONLY|FCREATE, 0666)) < 0) {
		fprintf(stderr, "%s: cannot create file\n", name);
		return (FAIL);
	}
	seekdir(dirp, itp->t_seekpt, itp->t_seekpt);
	dp = dup(dirp->dd_fd);
	for (i = itp->t_size; i > 0; i -= BUFSIZ) {
		size = i < BUFSIZ ? i : BUFSIZ;
		if (read(dp, buf, (int) size) == -1) {
			fprintf(stderr,
				"write error extracting inode %d, name %s\n",
				curfile.ino, curfile.name);
			perror("read");
			done(1);
		}
		if (write(ofile, buf, (int) size) == -1) {
			fprintf(stderr,
				"write error extracting inode %d, name %s\n",
				curfile.ino, curfile.name);
			perror("write");
			done(1);
		}
	}
	(void) close(dp);
	(void) close(ofile);
	return (GOOD);
}

/*
 * Do an "ls" style listing of a directory
 */
printlist(name, ino)
	char *name;
	ino_t ino;
{
	register struct afile *fp;
	register struct inotab *itp;
	struct afile *dfp0, *dfplast;
	struct afile single;

	itp = inotablookup(ino);
	if (itp == NULL) {
		single.fnum = ino;
		single.fname = savename(rindex(name, '/') + 1);
		dfp0 = &single;
		dfplast = dfp0 + 1;
	} else {
		seekdir(dirp, itp->t_seekpt, itp->t_seekpt);
		if (getdir(dirp, &dfp0, &dfplast) == FAIL)
			return;
	}
	qsort((char *)dfp0, dfplast - dfp0, sizeof (struct afile), fcmp);
	formatf(dfp0, dfplast);
	for (fp = dfp0; fp < dfplast; fp++)
		freename(fp->fname);
}

/*
 * Read the contents of a directory.
 */
getdir(dirp, pfp0, pfplast)
	DIR *dirp;
	struct afile **pfp0, **pfplast;
{
	register struct afile *fp;
	register struct direct *dp;
	static struct afile *basefp = NULL;
	static long nent = 20;

	if (basefp == NULL)
		basefp = (struct afile *)calloc((unsigned)nent,
			sizeof (struct afile));
	fp = *pfp0 = basefp;
	*pfplast = *pfp0 + nent;
	while (dp = readdir(dirp)) {
		if (dp->d_ino == 0 && strcmp(dp->d_name, "/") == 0)
			break;
		if (BIT(dp->d_ino, dumpmap) == 0)
			continue;
		if (vflag == 0 &&
		    (strcmp(dp->d_name, ".") == 0 ||
		     strcmp(dp->d_name, "..") == 0))
			continue;
		fp->fnum = dp->d_ino;
		fp->fname = savename(dp->d_name);
		fp++;
		if (fp == *pfplast) {
			basefp = (struct afile *)realloc((char *)basefp,
			    (unsigned)(2 * nent * sizeof (struct afile)));
			if (basefp == 0) {
				fprintf(stderr, "ls: out of memory\n");
				return (FAIL);
			}
			*pfp0 = basefp;
			fp = *pfp0 + nent;
			*pfplast = fp + nent;
			nent *= 2;
		}
	}
	*pfplast = fp;
	return (GOOD);
}

/*
 * Print out a pretty listing of a directory
 */
formatf(fp0, fplast)
	struct afile *fp0, *fplast;
{
	register struct afile *fp;
	struct entry *np;
	int width = 0, w, nentry = fplast - fp0;
	int i, j, len, columns, lines;
	char *cp;

	if (fp0 == fplast)
		return;
	for (fp = fp0; fp < fplast; fp++) {
		fp->ftype = inodetype(fp->fnum);
		np = lookupino(fp->fnum);
		if (np != NIL)
			fp->fflags = np->e_flags;
		else
			fp->fflags = 0;
		len = strlen(fmtentry(fp));
		if (len > width)
			width = len;
	}
	width += 2;
	columns = 80 / width;
	if (columns == 0)
		columns = 1;
	lines = (nentry + columns - 1) / columns;
	for (i = 0; i < lines; i++) {
		for (j = 0; j < columns; j++) {
			fp = fp0 + j * lines + i;
			cp = fmtentry(fp);
			fprintf(stderr, "%s", cp);
			if (fp + lines >= fplast) {
				fprintf(stderr, "\n");
				break;
			}
			w = strlen(cp);
			while (w < width) {
				w++;
				fprintf(stderr, " ");
			}
		}
	}
}

/*
 * Comparison routine for qsort.
 */
fcmp(f1, f2)
	register struct afile *f1, *f2;
{

	return (strcmp(f1->fname, f2->fname));
}

/*
 * Format a directory entry.
 */
char *
fmtentry(fp)
	register struct afile *fp;
{
	static char fmtres[BUFSIZ];
	register char *cp, *dp;

	if (vflag)
		(void) sprintf(fmtres, "%5d ", fp->fnum);
	else
		fmtres[0] = '\0';
	dp = &fmtres[strlen(fmtres)];
	if ((fp->fflags & NEW) != 0)
		*dp++ = '*';
	else
		*dp++ = ' ';
	for (cp = fp->fname; *cp; cp++)
		if (!vflag && (*cp < ' ' || *cp >= 0177))
			*dp++ = '?';
		else
			*dp++ = *cp;
	if (fp->ftype == NODE)
		*dp++ = '/';
	*dp++ = 0;
	return (fmtres);
}

/*
 * Determine the type of an inode
 */
inodetype(ino)
	ino_t ino;
{
	struct inotab *itp;

	itp = inotablookup(ino);
	if (itp == NULL)
		return (LEAF);
	return (NODE);
}

/*
 * Allocate and initialize a directory inode entry.
 * If requested, save its pertinent mode, owner, and time info.
 */
struct inotab *
allocinotab(ino, dip, seekpt)
	ino_t ino;
	struct dinode *dip;
	daddr_t seekpt;
{
	register struct inotab	*itp;
	struct modeinfo node;

	itp = (struct inotab *)calloc(1, sizeof(struct inotab));
	itp->t_next = inotab[INOHASH(ino)];
	inotab[INOHASH(ino)] = itp;
	itp->t_ino = ino;
	itp->t_seekpt = seekpt;
	if (mf == NULL)
		return(itp);
	node.ino = ino;
	node.timep[0] = dip->di_atime;
	node.timep[1] = dip->di_mtime;
	node.mode = dip->di_mode;
	node.uid = dip->di_uid;
	node.gid = dip->di_gid;
	(void) fwrite((char *)&node, 1, sizeof(struct modeinfo), mf);
	return(itp);
}

/*
 * Look up an inode in the table of directories
 */
struct inotab *
inotablookup(ino)
	ino_t	ino;
{
	register struct inotab *itp;

	for (itp = inotab[INOHASH(ino)]; itp != NULL; itp = itp->t_next)
		if (itp->t_ino == ino)
			return(itp);
	return ((struct inotab *)0);
}

/*
 * Clean up and exit
 */
done(exitcode)
	int exitcode;
{

	closemt();
	if (modefile[0] != '#')
		(void) unlink(modefile);
	if (dirfile[0] != '#')
		(void) unlink(dirfile);
	exit(exitcode);
}

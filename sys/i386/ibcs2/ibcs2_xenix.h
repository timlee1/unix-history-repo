/*
 * System call prototypes.
 *
 * DO NOT EDIT-- this file is automatically generated.
 * created from	Id: syscalls.xenix,v 1.5 1997/04/09 15:44:47 bde Exp 
 */

#ifndef _IBCS2_XENIX_H_
#define	_IBCS2_XENIX_H_

#include <sys/signal.h>

struct	xenix_rdchk_args {
	int fd;
};
struct	xenix_chsize_args {
	int fd;
	long size;
};
struct	xenix_ftime_args {
	struct timeb * tp;
};
struct	xenix_nap_args {
	int millisec;
};
struct	xenix_scoinfo_args {
	int dummy;
};
struct	xenix_eaccess_args {
	char * path;
	int flags;
};
struct	ibcs2_sigaction_args {
	int sig;
	struct ibcs2_sigaction * act;
	struct ibcs2_sigaction * oact;
};
struct	ibcs2_sigprocmask_args {
	int how;
	ibcs2_sigset_t * set;
	ibcs2_sigset_t * oset;
};
struct	ibcs2_sigpending_args {
	ibcs2_sigset_t * mask;
};
struct	ibcs2_sigsuspend_args {
	ibcs2_sigset_t * mask;
};
struct	ibcs2_getgroups_args {
	int gidsetsize;
	ibcs2_gid_t * gidset;
};
struct	ibcs2_setgroups_args {
	int gidsetsize;
	ibcs2_gid_t * gidset;
};
struct	ibcs2_sysconf_args {
	int name;
};
struct	ibcs2_pathconf_args {
	char * path;
	int name;
};
struct	ibcs2_fpathconf_args {
	int fd;
	int name;
};
struct	ibcs2_rename_args {
	char * from;
	char * to;
};
struct	xenix_utsname_args {
	long addr;
};
int	xenix_rdchk __P((struct proc *, struct xenix_rdchk_args *, int []));
int	xenix_chsize __P((struct proc *, struct xenix_chsize_args *, int []));
int	xenix_ftime __P((struct proc *, struct xenix_ftime_args *, int []));
int	xenix_nap __P((struct proc *, struct xenix_nap_args *, int []));
int	xenix_scoinfo __P((struct proc *, struct xenix_scoinfo_args *, int []));
int	xenix_eaccess __P((struct proc *, struct xenix_eaccess_args *, int []));
int	ibcs2_sigaction __P((struct proc *, struct ibcs2_sigaction_args *, int []));
int	ibcs2_sigprocmask __P((struct proc *, struct ibcs2_sigprocmask_args *, int []));
int	ibcs2_sigpending __P((struct proc *, struct ibcs2_sigpending_args *, int []));
int	ibcs2_sigsuspend __P((struct proc *, struct ibcs2_sigsuspend_args *, int []));
int	ibcs2_getgroups __P((struct proc *, struct ibcs2_getgroups_args *, int []));
int	ibcs2_setgroups __P((struct proc *, struct ibcs2_setgroups_args *, int []));
int	ibcs2_sysconf __P((struct proc *, struct ibcs2_sysconf_args *, int []));
int	ibcs2_pathconf __P((struct proc *, struct ibcs2_pathconf_args *, int []));
int	ibcs2_fpathconf __P((struct proc *, struct ibcs2_fpathconf_args *, int []));
int	ibcs2_rename __P((struct proc *, struct ibcs2_rename_args *, int []));
int	xenix_utsname __P((struct proc *, struct xenix_utsname_args *, int []));

#ifdef COMPAT_43


#endif /* COMPAT_43 */

#endif /* !_IBCS2_XENIX_H_ */

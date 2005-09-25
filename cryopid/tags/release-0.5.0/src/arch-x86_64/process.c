/*
 * Process state saver
 *   (C) 2004 Bernard Blackham <bernard@blackham.com.au>
 *
 * Licensed under a BSD-ish license.
 */

/* large file support */
//#define _FILE_OFFSET_BITS 64

#include <malloc.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <linux/user.h>
#include <linux/kdev_t.h>
#include <asm/ldt.h>
#include <asm/unistd.h>
#include <asm/ptrace.h>
#include <assert.h>
#include <asm/termios.h>

#include "cryopid.h"
#include "cpimage.h"
#include "list.h"

static int process_was_stopped = 0;

char* backup_page(pid_t target, void* addr)
{
    long* page = xmalloc(PAGE_SIZE);
    int i;
    long ret;
    for(i = 0; i < PAGE_SIZE/sizeof(long); i++) {
	ret = ptrace(PTRACE_PEEKTEXT, target, (void*)((long)addr+(i*sizeof(long))), 0);
	if (errno) {
	    perror("ptrace(PTRACE_PEEKTEXT)");
	    free(page);
	    return NULL;
	}
	page[i] = ret;
	if (ptrace(PTRACE_POKETEXT, target, (void*)((long)addr+(i*sizeof(long))), 0xdeadbeef) == -1) {
	    perror("ptrace(PTRACE_POKETEXT)");
	    free(page);
	    return NULL;
	}
    }

    return (char*)page;
}

int restore_page(pid_t target, void* addr, char* page)
{
    long *p = (long*)page;
    int i;
    assert(page);
    for (i = 0; i < PAGE_SIZE/sizeof(long); i++) {
	if (ptrace(PTRACE_POKETEXT, target, (void*)((long)addr+(i*sizeof(long))), p[i]) == -1) {
	    perror("ptrace(PTRACE_POKETEXT)");
	    free(page);
	    return 0;
	}
    }
    free(page);
    return 1;
}

int memcpy_into_target(pid_t pid, void* dest, const void* src, size_t n)
{
    /* just like memcpy, but copies it into the space of the target pid */
    /* n must be a multiple of 4, or will otherwise be rounded down to be so */
    int i;
    long *d, *s;
    d = (long*) dest;
    s = (long*) src;
    for (i = 0; i < n / sizeof(long); i++) {
	if (ptrace(PTRACE_POKETEXT, pid, d+i, s[i]) == -1) {
	    perror("ptrace(PTRACE_POKETEXT)");
	    return 0;
	}
    }
    return 1;
}

int memcpy_from_target(pid_t pid, void* dest, const void* src, size_t n)
{
    /* just like memcpy, but copies it from the space of the target pid */
    /* n must be a multiple of 4, or will otherwise be rounded down to be so */
    int i;
    long *d, *s;
    d = (long*) dest;
    s = (long*) src;
    n /= sizeof(long);
    for (i = 0; i < n; i++) {
	d[i] = ptrace(PTRACE_PEEKTEXT, pid, s+i, 0);
	if (errno) {
	    perror("ptrace(PTRACE_PEEKTEXT)");
	    return 0;
	}
    }
    return 1;
}

static int save_registers(pid_t pid, struct user_regs_struct *r)
{
    if (ptrace(PTRACE_GETREGS, pid, NULL, r) < 0) {
	perror("ptrace getregs");
	return errno;
    }
    return 0;
}

static int restore_registers(pid_t pid, struct user_regs_struct *r)
{
    if (ptrace(PTRACE_SETREGS, pid, NULL, r) < 0) {
	perror("ptrace setregs");
	return errno;
    }
    return 0;
}

int do_syscall(pid_t pid, struct user_regs_struct *regs)
{
    long loc;
    struct user_regs_struct orig_regs;
    long old_insn;
    int status, ret;

    if (save_registers(pid, &orig_regs) < 0)
	return -EACCES;

    loc = scribble_zone+0x10;

    old_insn = ptrace(PTRACE_PEEKTEXT, pid, loc, 0);
    if (errno) {
	perror("ptrace peektext");
	return -EACCES;
    }
    //printf("original instruction at 0x%lx was 0x%lx\n", loc, old_insn);

    if (ptrace(PTRACE_POKETEXT, pid, loc, 0x80cd) < 0) {
	perror("ptrace poketext");
	return -EACCES;
    }

    /* Set up registers for ptrace syscall */
    regs->rip = loc;
    if (restore_registers(pid, regs) < 0)
	return -EACCES;

    /* Execute call */
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0) {
	perror("ptrace singlestep");
	return -EACCES;
    }
    ret = waitpid(pid, &status, 0);
    if (ret == -1) {
	perror("Failed to wait for child");
	exit(1);
    }

    /* Get our new registers */
    if (save_registers(pid, regs) < 0)
	return -EACCES;

    /* Return everything back to normal */
    if (restore_registers(pid, &orig_regs) < 0)
	return -EACCES;

    if (ptrace(PTRACE_POKETEXT, pid, loc, old_insn) < 0) {
	perror("ptrace poketext");
	return -EACCES;
    }

    return 1;
}

int is_in_syscall(pid_t pid, struct user *user)
{
    long inst;
    inst = ptrace(PTRACE_PEEKDATA, pid, user->regs.rip-2, 0);
    if (errno) {
	perror("ptrace(PEEKDATA)");
	return 0;
    }
    if ((inst&0xffff) == 0x80cd)
	return 1;
    if ((inst&0xffff) == 0x050f)
	return 1;
    return 0;
}

void set_syscall_return(struct user* user, unsigned long val) {
    user->regs.rax = val;
}

static int process_is_stopped(pid_t pid)
{
    char buf[30];
    char mode;
    FILE *f;
    snprintf(buf, 30, "/proc/%d/stat", pid);
    f = fopen(buf, "r");
    if (f == NULL) return -1;
    fscanf(f, "%*s %*s %c", &mode);
    fclose(f);
    return mode == 'T';
}

static void start_ptrace(pid_t pid)
{
    long ret;
    int status;

    process_was_stopped = process_is_stopped(pid);

    ret = ptrace(PTRACE_ATTACH, pid, 0, 0);
    if (ret == -1) {
	perror("Failed to ptrace");
	exit(1);
    }

    if (process_was_stopped)
	return; /* don't bother waiting for it, we'll just hang */

    ret = waitpid(pid, &status, 0);
    if (ret == -1) {
	perror("Failed to wait for child");
	exit(1);
    }
    if (!WIFSTOPPED(status)) {
	fprintf(stderr, "Failed to get child stopped.\n");
    }
}

static void end_ptrace(pid_t pid)
{
    long ret;

    ret = ptrace(PTRACE_DETACH, pid, 0, 0);
    if (ret == -1) {
	perror("Failed to detach");
	exit(1);
    }
}

void get_process(pid_t pid, int flags, struct list *process_image, long *bin_offset)
{
    int success = 0;
    char* pagebackup;
    struct user_regs_struct r;

    start_ptrace(pid);

    if (save_registers(pid, &r) < 0) {
	fprintf(stderr, "Unable to save process's registers!\n");
	goto out_ptrace;
    }

    /* The order below is very important. Do not change without good reason and
     * careful thought.
     */

    /* this gives us a scribble zone: */
    fetch_chunks_vma(pid, flags, process_image, bin_offset);

    if (!scribble_zone) {
	fprintf(stderr, "[-] No suitable scribble zone could be found. Aborting.\n");
	goto out_ptrace;
    }
    pagebackup = backup_page(pid, (void*)scribble_zone);

    fetch_chunks_fd(pid, flags, process_image);

    fetch_chunks_sighand(pid, flags, process_image);
    fetch_chunks_regs(pid, flags, process_image, process_was_stopped);

    success = 1;

    restore_page(pid, (void*)scribble_zone, pagebackup);
    restore_registers(pid, &r);
out_ptrace:
    end_ptrace(pid);
    
    if (!success)
	abort();
}

static inline unsigned long __remote_syscall(pid_t pid,
	int syscall_no, char *syscall_name,
	int use_rdi, unsigned long rdi,
	int use_rsi, unsigned long rsi,
	int use_rdx, unsigned long rdx,
	int use_r10, unsigned long r10,
	int use_r8 , unsigned long r8 )
{
    struct user_regs_struct orig_regs, regs;
    unsigned long loc, old_insn, ret;
    int status;

    if (save_registers(pid, &orig_regs) < 0)
	abort();

    memcpy(&regs, &orig_regs, sizeof(regs));

    loc = scribble_zone+0x10;

    old_insn = ptrace(PTRACE_PEEKTEXT, pid, loc, 0);
    if (errno) {
	perror("ptrace peektext");
	abort();
    }

    if (ptrace(PTRACE_POKETEXT, pid, loc, 0x050f) < 0) {
	perror("ptrace poketext");
	abort();
    }

    regs.rax = syscall_no;
    if (use_rdi) regs.rdi = rdi;
    if (use_rsi) regs.rsi = rsi;
    if (use_rdx) regs.rdx = rdx;
    if (use_r10) regs.r10 = r10;
    if (use_r8 ) regs.r8  = r8 ;

    /* Set up registers for ptrace syscall */
    regs.rip = loc;
    if (restore_registers(pid, &regs) < 0)
	abort();

    /* Execute call */
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) < 0) {
	perror("ptrace singlestep");
	abort();
    }
    ret = waitpid(pid, &status, 0);
    if (ret == -1) {
	perror("Failed to wait for child");
	abort();
    }

    /* Get our new registers */
    if (save_registers(pid, &regs) < 0)
	abort();

    /* Return everything back to normal */
    if (restore_registers(pid, &orig_regs) < 0)
	abort();

    if (ptrace(PTRACE_POKETEXT, pid, loc, old_insn) < 0) {
	perror("ptrace poketext");
	abort();
    }

    if (regs.rax < 0) {
	errno = -regs.rax;
	fprintf(stderr, "[%d] %s: %s\n", pid, syscall_name, strerror(errno));
	return -1;
    }

    return regs.rax;
}

#define __rsyscall0(type,name) \
    type __r_##name(pid_t pid) { \
	return (type)__remote_syscall(pid, __NR_##name, #name, \
		0,0,0,0,0,0,0,0,0,0); \
    }

#define __rsyscall1(type,name,type1,arg1) \
    type __r_##name(pid_t pid, type1 arg1) { \
	return (type)__remote_syscall(pid, __NR_##name, #name, \
		1, (unsigned long)arg1, \
		0,0,0,0,0,0,0,0); \
    }

#define __rsyscall2(type,name,type1,arg1,type2,arg2) \
    type __r_##name(pid_t pid, type1 arg1, type2 arg2) { \
	return (type)__remote_syscall(pid, __NR_##name, #name, \
		1, (unsigned long)arg1, \
		1, (unsigned long)arg2, \
		0,0,0,0,0,0); \
    }

#define __rsyscall3(type,name,type1,arg1,type2,arg2,type3,arg3) \
    type __r_##name(pid_t pid, type1 arg1, type2 arg2, type3 arg3) { \
	return (type)__remote_syscall(pid, __NR_##name, #name, \
		1, (unsigned long)arg1, \
		1, (unsigned long)arg2, \
		1, (unsigned long)arg3, \
		0,0,0,0); \
    }

#define __rsyscall4(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4) \
    type __r_##name(pid_t pid, type1 arg1, type2 arg2, type3 arg3, type4 arg4) { \
	return (type)__remote_syscall(pid, __NR_##name, #name, \
		1, (unsigned long)arg1, \
		1, (unsigned long)arg2, \
		1, (unsigned long)arg3, \
		1, (unsigned long)arg4, \
		0,0); \
    }

#define __rsyscall5(type,name,type1,arg1,type2,arg2,type3,arg3,type4,arg4,type5,arg5) \
    type __r_##name(pid_t pid, type1 arg1, type2 arg2, type3 arg3, type4 arg4, type5 arg5) { \
	return (type)__remote_syscall(pid, __NR_##name, #name, \
		1, (unsigned long)arg1, \
		1, (unsigned long)arg2, \
		1, (unsigned long)arg3, \
		1, (unsigned long)arg4, \
		1, (unsigned long)arg5); \
    }

__rsyscall3(off_t, lseek, int, fd, off_t, offset, int, whence);
off_t r_lseek(pid_t pid, int fd, off_t offset, int whence)
{
    return __r_lseek(pid, fd, offset, whence);
}

__rsyscall2(off_t, fcntl, int, fd, int, cmd);
int r_fcntl(pid_t pid, int fd, int cmd)
{
    return __r_fcntl(pid, fd, cmd);
}

__rsyscall3(int, mprotect, void*, start, size_t, len, int, flags);
int r_mprotect(pid_t pid, void* start, size_t len, int flags)
{
    return __r_mprotect(pid, start, len, flags);
}

__rsyscall4(int, rt_sigaction, int, sig, struct k_sigaction*, ksa, struct k_sigaction*, oksa, size_t, masksz);
int r_rt_sigaction(pid_t pid, int sig, struct k_sigaction *ksa, struct k_sigaction *oksa, size_t masksz)
{
    return __r_rt_sigaction(pid, sig, ksa, oksa, masksz);
}

__rsyscall3(int, ioctl, int, fd, int, req, void*, val);
int r_ioctl(pid_t pid, int fd, int req, void* val)
{
    return __r_ioctl(pid, fd, req, val);
}

__rsyscall5(int, getsockopt, int, s, int, level, int, optname, void*, optval, socklen_t*, optlen);
int r_getsockopt(pid_t pid, int s, int level, int optname, void* optval, socklen_t *optlen)
{
    return __r_getsockopt(pid, s, level, optname, optval, optlen);
}
/* vim:set ts=8 sw=4 noet: */
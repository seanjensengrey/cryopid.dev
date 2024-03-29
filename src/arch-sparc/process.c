#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <assert.h>
#include <netinet/tcp.h>
#include <linux/net.h>
#include <asm/reg.h>
#include <asm/page.h>
#include <asm/ptrace.h>

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
	if (ptrace(PTRACE_POKETEXT, target, (void*)((long)addr+(i*sizeof(long))), ARCH_POISON) == -1) {
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
    /* n must be a multiple of word size, or will otherwise be rounded down to
     * be so */
    int i;
    long *d, *s;
    d = (long*) dest;
    s = (long*) src;
    n /= sizeof(long);
    for (i = 0; i < n; i++) {
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
    /* n must be a multiple of word size, or will otherwise be rounded down to
     * be so */
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

static int save_registers(pid_t pid, struct regs *r)
{
    if (ptrace(PTRACE_GETREGS, pid, r, NULL) < 0) {
	perror("ptrace getregs");
	return -errno;
    }
    return 0;
}

static int restore_registers(pid_t pid, struct regs *r)
{
    if (ptrace(PTRACE_SETREGS, pid, r, NULL) < 0) {
	perror("ptrace setregs");
	return -errno;
    }
    return 0;
}

int is_a_nop(unsigned long inst, int canonical)
{
    return inst == 0x01000000;
}

int is_a_syscall(unsigned long inst, int canonical)
{
    if (inst == 0x91d02010)
	return 1;
    return 0;
}

int is_in_syscall(pid_t pid, struct user *user)
{
    long inst;
    /* FIXME npc or pc? see esky? */
    inst = ptrace(PTRACE_PEEKDATA, pid, user->regs.npc-4, 0);
    if (errno) {
	perror("ptrace(PEEKDATA)");
	return 0;
    }
    return is_a_syscall(inst, 0);
}

void set_syscall_return(struct user* user, unsigned long val) {
    /* FIXME - set carry bit on error */
    user->regs.regs[7] = val;
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
    struct regs r;

    start_ptrace(pid);

    if (save_registers(pid, &r) < 0) {
	fprintf(stderr, "Unable to save process's registers!\n");
	goto out_ptrace;
    }

    extern unsigned long mysp;
    mysp = r.r_o6;

    /* The order below is very important. Do not change without good reason and
     * careful thought.
     */

    /* this gives us a scribble zone: */
    fetch_chunks_vma(pid, flags, process_image, bin_offset);

    if (!scribble_zone) {
	fprintf(stderr, "[-] No suitable scribble zone could be found. Aborting.\n");
	goto out_ptrace_regs;
    }

    pagebackup = backup_page(pid, (void*)scribble_zone);

    fetch_chunks_fd(pid, flags, process_image);
    fetch_chunks_regs(pid, flags, process_image, process_was_stopped);
    fetch_chunks_sighand(pid, flags, process_image);

    success = 1;

    restore_page(pid, (void*)scribble_zone, pagebackup);

out_ptrace_regs:
    restore_registers(pid, &r);

out_ptrace:
    end_ptrace(pid);
    
    if (!success)
	abort();
}

static inline unsigned long __remote_syscall(pid_t pid,
	int syscall_no, char *syscall_name,
	int use_o0, unsigned long o0,
	int use_o1, unsigned long o1,
	int use_o2, unsigned long o2,
	int use_o3, unsigned long o3,
	int use_o4, unsigned long o4)
{
    struct regs orig_regs, regs;
    unsigned long ret;
    int status;

    if (!syscall_loc) {
	fprintf(stderr, "No syscall locations found! Cannot do remote syscall.\n");
	abort();
    }

    if (save_registers(pid, &orig_regs) < 0)
	abort();

    memcpy(&regs, &orig_regs, sizeof(regs));

    regs.r_g1 = syscall_no;
    if (use_o0) regs.r_o0 = o0;
    if (use_o1) regs.r_o1 = o1;
    if (use_o2) regs.r_o2 = o2;
    if (use_o3) regs.r_o3 = o3;
    if (use_o4) regs.r_o4 = o4;

    /* Set up registers for ptrace syscall */
    regs.r_pc = syscall_loc;
    regs.r_npc = syscall_loc;
    if (restore_registers(pid, &regs) < 0)
	abort();

    printf("Regs at 0x%lx/0x%lx are g1, o0, o1, o2, o3 : 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
	    regs.r_pc, 
	    regs.r_npc, 
	    regs.r_g1, 
	    regs.r_o0, 
	    regs.r_o1, 
	    regs.r_o2, 
	    regs.r_o3);
    /* Execute call - there's no PTRACE_SINGLESTEP on sparc. Instead use
     * PTRACE_SYSCALL
     */
    if (ptrace(PTRACE_SYSCALL, pid, 1, 0) < 0) {
	perror("ptrace syscall");
	abort();
    }
    ret = waitpid(pid, &status, 0);
    if (ret == -1) {
	perror("Failed to wait for child");
	abort();
    }
    printf("waited and got SIG %d\n", WSTOPSIG(status));
    if (WSTOPSIG(status) != SIGTRAP) {
	struct regs new_regs;
	save_registers(pid, &new_regs);
	printf("Interrupted at 0x%lx/0x%lx Mid regs are g1, o0, o1, o2, o3 : 0x%lx 0x%lx 0x%lx 0x%lx 0x%lx\n",
		new_regs.r_pc,
		new_regs.r_npc,
		new_regs.r_g1,
		new_regs.r_o0,
		new_regs.r_o1,
		new_regs.r_o2,
		new_regs.r_o3);
	/* do it again */
	restore_registers(pid, &regs);
	if (ptrace(PTRACE_SYSCALL, pid, 1, 0) < 0) {
	    perror("ptrace syscall");
	    abort();
	}
	ret = waitpid(pid, &status, 0);
	if (ret == -1) {
	    perror("Failed to wait for child");
	    abort();
	}
	printf("waited again got SIG %d\n", WSTOPSIG(status));
    }

    if (ptrace(PTRACE_SYSCALL, pid, 1, 0) < 0) {
	perror("ptrace syscall");
	abort();
    }
    ret = waitpid(pid, &status, 0);
    if (ret == -1) {
	perror("Failed to wait for child");
	abort();
    }
    printf("waited and got SIG %d\n", WSTOPSIG(status));

    /* Get our new registers */
    if (save_registers(pid, &regs) < 0)
	abort();

    /* Return everything back to normal */
    if (restore_registers(pid, &orig_regs) < 0)
	abort();

    if (regs.r_psr & PSR_C) { /* error */
	errno = regs.r_o0;
	fprintf(stderr, "syscall %s returns error %s\n", syscall_name, strerror(errno));
	errno = regs.r_o0;
	return -1;
    }

    errno = 0;
    fprintf(stderr, "syscall %s returns %d\n", syscall_name, regs.r_o0);
    return regs.r_o0;
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

__rsyscall5(int, rt_sigaction, int, sig, struct k_sigaction*, ksa, struct k_sigaction*, oksa, void*, restorer, size_t, masksz);
int r_rt_sigaction(pid_t pid, int sig, struct k_sigaction *ksa, struct k_sigaction *oksa, size_t masksz)
{
    int ret;
    if (ksa)
	memcpy_into_target(pid, (void*)(scribble_zone+0x100), ksa, sizeof(*ksa));
    ret = __r_rt_sigaction(pid, sig, ksa?(void*)(scribble_zone+0x100):NULL,
	    oksa?(void*)(scribble_zone+0x100+sizeof(*ksa)):NULL, NULL, masksz);
    if (oksa)
	memcpy_from_target(pid, oksa, (void*)(scribble_zone+0x100+sizeof(*ksa)), sizeof(*oksa));

    return ret;
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

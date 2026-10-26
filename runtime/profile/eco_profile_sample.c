// REG_RIP / REG_RBP are GNU; POSIX ucontext.h names the array and not the slots
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "eco_profile_internal.h"

#if defined(_WIN32)

typedef int eco_profile_sample_unused;

#else

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#endif

#if defined(__linux__)
#include <ucontext.h>
#endif

static struct sigaction g_old_prof;
static int g_had_old_prof;

uint32_t capture_stack(uint64_t *frames, uint32_t cap, void *pc, void *fp)
{
    uint32_t depth = 0;
    if (pc != NULL && depth < cap) {
        frames[depth++] = (uint64_t)(uintptr_t)pc;
    }

    uintptr_t prev = 0;
    uintptr_t *frame = (uintptr_t *)fp;
    while (depth < cap && frame != NULL) {
        if (((uintptr_t)frame & (sizeof(void *) - 1u)) != 0) {
            break;
        }
        if (prev != 0 && (uintptr_t)frame <= prev) {
            break;
        }
        uintptr_t next = frame[0];
        uintptr_t ret = frame[1];
        if (next != 0) {
            if (next < (uintptr_t)frame) {
                break;
            }
            if (next - (uintptr_t)frame > 8u * 1024u * 1024u) {
                break;
            }
        }
        if (ret != 0) {
            frames[depth++] = (uint64_t)ret;
        }
        prev = (uintptr_t)frame;
        if (next == 0) {
            break;
        }
        frame = (uintptr_t *)next;
    }

    return depth;
}

uint32_t walk_stack(thread_state *st, void *pc, void *fp)
{
    uint64_t frames[k_max_depth];
    uint32_t depth = capture_stack(frames, k_max_depth, pc, fp);
    return intern_stack(st, frames, depth);
}

uint32_t walk_here(thread_state *st)
{
    return walk_stack(st, (void *)(uintptr_t)__builtin_return_address(0), __builtin_frame_address(0));
}

static void on_prof(int sig, siginfo_t *info, void *ucontext)
{
    (void)sig;
    (void)info;
    if (atomic_load(&g_closed)) {
        return;
    }
    thread_state *st = current_state(1);
    if (st == NULL || atomic_load(&st->busy)) {
        atomic_fetch_add(&g_dropped, 1);
        return;
    }

    // park only: intern is flush_pending's, sampling is wait_idle's at finish
    atomic_store(&st->sampling, 1);

    void *pc = NULL;
    void *fp = NULL;
    ucontext_t *uc = (ucontext_t *)ucontext;
    if (uc != NULL) {
#if defined(__APPLE__) && defined(__aarch64__)
        pc = (void *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
        fp = (void *)(uintptr_t)uc->uc_mcontext->__ss.__fp;
#elif defined(__APPLE__) && defined(__x86_64__)
        pc = (void *)(uintptr_t)uc->uc_mcontext->__ss.__rip;
        fp = (void *)(uintptr_t)uc->uc_mcontext->__ss.__rbp;
#elif defined(__linux__) && defined(__aarch64__)
        pc = (void *)(uintptr_t)uc->uc_mcontext.pc;
        fp = (void *)(uintptr_t)uc->uc_mcontext.regs[29];
#elif defined(__linux__) && defined(__x86_64__)
        pc = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        fp = (void *)(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
#endif
    }
    if (st->pending == NULL || st->pending_count >= st->pending_cap) {
        st->dropped += 1;
    } else {
        pending_sample *slot = &st->pending[st->pending_count];
        slot->depth = capture_stack(slot->frames, k_max_depth, pc, fp);
        st->pending_count += 1;
    }
    atomic_store(&st->sampling, 0);
}

void start_sampler(void)
{
    const char *hz = getenv("ECHO_PROFILE_HZ");
    if (hz != NULL && hz[0] != 0) {
        unsigned long v = strtoul(hz, NULL, 10);
        if (v > 0 && v < 100000) {
            g_sample_hz = (uint32_t)v;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, &g_old_prof) != 0) {
        return;
    }
    g_had_old_prof = 1;

    const uint64_t period = 1000000ull / (uint64_t)g_sample_hz;
    struct itimerval tv;
    memset(&tv, 0, sizeof(tv));
    tv.it_interval.tv_sec = (time_t)(period / 1000000ull);
    tv.it_interval.tv_usec = (suseconds_t)(period % 1000000ull);
    if (tv.it_interval.tv_sec == 0 && tv.it_interval.tv_usec == 0) {
        tv.it_interval.tv_usec = 1;
    }
    tv.it_value = tv.it_interval;
    if (setitimer(ITIMER_PROF, &tv, NULL) != 0) {
        sigaction(SIGPROF, &g_old_prof, NULL);
        g_had_old_prof = 0;
    }
}

void stop_sampler(void)
{
    sigset_t set;
    sigset_t old;
    sigemptyset(&set);
    sigaddset(&set, SIGPROF);
    pthread_sigmask(SIG_BLOCK, &set, &old);

    struct itimerval tv;
    memset(&tv, 0, sizeof(tv));
    setitimer(ITIMER_PROF, &tv, NULL);

    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPROF, &ignore, NULL);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    if (g_had_old_prof) {
        sigaction(SIGPROF, &g_old_prof, NULL);
        g_had_old_prof = 0;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

#endif

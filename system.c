/*
 * Copyright (C) 1996-2000,2013 Michael R. Elkins <me@mutt.org>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#ifdef USE_IMAP
# include "imap.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

void mutt_close_range(int min_fd)
{
  int closed = 0;

  /* Close all inherited file descriptors above or equal to min_fd */
#if defined(HAVE_CLOSE_RANGE)
  if (close_range(min_fd, ~0U, 0) == 0)
    closed = 1;
#elif defined(SYS_close_range)
  /* Direct Linux kernel syscall fallback */
  if (syscall(SYS_close_range, min_fd, ~0U, 0) == 0)
    closed = 1;
#elif defined(HAVE_CLOSEFROM)
  closefrom(min_fd);
  closed = 1;
#endif

  if (!closed)
  {
    int fd;
    int maxfd = sysconf(_SC_OPEN_MAX);

    if (maxfd < 0)
      maxfd = 1024;
    for (fd = min_fd; fd < maxfd; fd++)
      close(fd);
  }
}

void mutt_child_harden(pid_t parent_pid, int set_pdeathsig)
{
#ifdef PR_SET_DUMPABLE
  /* Protect Mutt's cloned address space before execle replaces it */
  prctl(PR_SET_DUMPABLE, 0);
#endif

  if (set_pdeathsig && parent_pid > 0)
  {
#ifdef PR_SET_PDEATHSIG
    /* Terminate child if parent Mutt process dies abruptly */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (getppid() != parent_pid)
      _exit(127);
#endif
  }
}

void mutt_exec_shell(const char *cmd)
{
  mutt_unblock_signals_system(0);
  mutt_reset_child_signals();

  execle(EXECSHELL, "sh", "-c", cmd, NULL, mutt_envlist());
  _exit(127); /* execl error */
}

static int sys_pidfd_open(pid_t pid, unsigned int flags)
{
#ifdef SYS_pidfd_open
  return syscall(SYS_pidfd_open, pid, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

static int sys_pidfd_send_signal(int pidfd, int sig, siginfo_t *info, unsigned int flags)
{
#ifdef SYS_pidfd_send_signal
  return syscall(SYS_pidfd_send_signal, pidfd, sig, info, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

#ifdef PR_SET_CHILD_SUBREAPER
static void cleanup_subreaper_orphans(void)
{
  char path[64];
  FILE *fp;
  pid_t self = getpid();
  pid_t pids[64];
  int pfds[64];
  struct pollfd pollfds[64];
  int count = 0;
  int i;

  /* Read adopted children from /proc/<pid>/task/<pid>/children */
  snprintf(path, sizeof(path), "/proc/%d/task/%d/children", (int)self, (int)self);
  fp = fopen(path, "r");
  if (!fp)
  {
    /* Fallback: reap anything reparented if /proc children is inaccessible */
    while (waitpid(-1, NULL, WNOHANG) > 0)
      ;
    return;
  }

  while (count < 64 && fscanf(fp, "%d", &pids[count]) == 1)
  {
    if (pids[count] > 1 && pids[count] != self)
      count++;
  }
  fclose(fp);

  if (count == 0)
    return;

  /* Obtain a pidfd for each reparented child */
  int pidfd_supported = 1;
  for (i = 0; i < count; i++)
  {
    pfds[i] = sys_pidfd_open(pids[i], 0);
    if (pfds[i] < 0)
    {
      pidfd_supported = 0;
      break;
    }
    pollfds[i].fd = pfds[i];
    pollfds[i].events = POLLIN;
    pollfds[i].revents = 0;
  }

  if (pidfd_supported)
  {
    int remaining = count;

    /* Request graceful exit with SIGHUP (standard controlling terminal hangup) */
    for (i = 0; i < count; i++)
      sys_pidfd_send_signal(pfds[i], SIGHUP, NULL, 0);

    /* Wait up to 100ms for reparented processes to exit */
    int timeout_ms = 100;
    while (remaining > 0 && timeout_ms > 0)
    {
      struct timespec ts_start, ts_end;
      clock_gettime(CLOCK_MONOTONIC, &ts_start);

      int ready = poll(pollfds, count, timeout_ms);

      clock_gettime(CLOCK_MONOTONIC, &ts_end);
      int spent = (int)((ts_end.tv_sec - ts_start.tv_sec) * 1000 +
                        (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000);
      if (spent <= 0)
        spent = 1;
      timeout_ms -= spent;

      if (ready < 0)
      {
        if (errno == EINTR)
          continue; /* Interrupted by signal; loop again with remaining timeout */
        break;
      }

      if (ready > 0)
      {
        remaining = 0;
        for (i = 0; i < count; i++)
        {
          if (!(pollfds[i].revents & POLLIN))
            remaining++;
        }
      }
    }

    /* Force termination for any tasks that did not exit */
    if (remaining > 0)
    {
      for (i = 0; i < count; i++)
      {
        if (!(pollfds[i].revents & POLLIN))
          sys_pidfd_send_signal(pfds[i], SIGKILL, NULL, 0);
      }
      /* Final 50ms wait for SIGKILL processing */
      poll(pollfds, count, 50);
    }

    /* Deterministically reap each child and release the pidfd */
    for (i = 0; i < count; i++)
    {
      siginfo_t info;
      memset(&info, 0, sizeof(info));
      waitid(P_PIDFD, pfds[i], &info, WEXITED | WNOHANG);
      close(pfds[i]);
    }
  }
  else
  {
    int j;
    for (j = 0; j < i; j++)
      close(pfds[j]);

    /* Standard kill fallback: request termination with SIGHUP */
    for (i = 0; i < count; i++)
      kill(pids[i], SIGHUP);

    for (i = 0; i < 10; i++)
    {
      pid_t w;
      while ((w = waitpid(-1, NULL, WNOHANG)) > 0)
        ;
      if (w < 0 && errno == ECHILD)
        return;
      usleep(10000);
    }

    for (i = 0; i < count; i++)
      kill(pids[i], SIGKILL);

    while (waitpid(-1, NULL, WNOHANG) > 0)
      ;
  }
}
#endif

int _mutt_system(const char *cmd, int flags)
{
  int rc = -1;
  struct sigaction act;
  struct sigaction oldtstp;
  struct sigaction oldcont;
  sigset_t set;
  pid_t thepid;
  pid_t parent_pid = getpid();

  if (!cmd || !*cmd)
    return (0);

  /* must ignore SIGINT and SIGQUIT */
  mutt_block_signals_system();

  memset(&act, 0, sizeof(act));

  /* also don't want to be stopped right now */
  if (flags & MUTT_DETACH_PROCESS)
  {
    sigemptyset(&set);
    sigaddset(&set, SIGTSTP);
    sigprocmask(SIG_BLOCK, &set, NULL);
  }
  else
  {
    act.sa_handler = SIG_DFL;
    /* we want to restart the waitpid() below */
#ifdef SA_RESTART
    act.sa_flags = SA_RESTART;
#endif
    sigemptyset(&act.sa_mask);
    sigaction(SIGTSTP, &act, &oldtstp);
    sigaction(SIGCONT, &act, &oldcont);
  }

  if ((thepid = fork()) == 0)
  {
    mutt_child_harden(parent_pid, !(flags & MUTT_DETACH_PROCESS));

    act.sa_flags = 0;

    if (flags & MUTT_DETACH_PROCESS)
    {
      /* give up controlling terminal */
      setsid();

      switch (fork())
      {
        case 0:
        {
          int devnull;

          mutt_close_range(3);

          /* Attach standard streams to /dev/null */
          devnull = open("/dev/null", O_RDWR);
          if (devnull != -1)
          {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2)
              close(devnull);
          }

          if (chdir("/") != 0)
          {
            /* Prevent retaining locks on current working directories */
          }

          memset(&act, 0, sizeof(act));
          act.sa_handler = SIG_DFL;
          sigemptyset(&act.sa_mask);
          sigaction(SIGCHLD, &act, NULL);
          break;
        }

        case -1:
          _exit(127);

        default:
          _exit(0);
      }

      mutt_exec_shell(cmd);
    }
    else
    {
#ifdef PR_SET_CHILD_SUBREAPER
      /* Subreaper supervisor adopts all grandchildren (e.g. bash background jobs) */
      if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0)
      {
        pid_t cmd_pid = fork();
        if (cmd_pid == 0)
        {
#ifdef PR_SET_PDEATHSIG
          /* Terminate shell if the supervisor dies unexpectedly */
          prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
          mutt_exec_shell(cmd);
        }
        else if (cmd_pid > 0)
        {
          int status = 0;
          int wait_ok = 0;
          pid_t w;

          /* Wait for foreground shell to finish */
          while ((w = waitpid(cmd_pid, &status, 0)) < 0)
          {
            if (errno != EINTR)
              break;
          }
          if (w == cmd_pid)
            wait_ok = 1;

          /* Clean up any orphaned background processes */
          cleanup_subreaper_orphans();

          if (wait_ok)
          {
            _exit(WIFEXITED(status) ? WEXITSTATUS(status) :
                 (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127));
          }
          else
          {
            _exit(127);
          }
        }
        else
        {
          _exit(127);
        }
      }
#endif
      /* Fallback if subreaper is unsupported */
      mutt_exec_shell(cmd);
    }
  }
  else if (thepid != -1)
  {
    int status = 0;
    int wait_ok = 0;

#ifndef USE_IMAP
    /* wait for the (first) child process to finish */
    pid_t w;
    while ((w = waitpid(thepid, &status, 0)) < 0)
    {
      if (errno != EINTR)
        break;
    }
    if (w == thepid)
      wait_ok = 1;
#else
    status = imap_wait_keepalive(thepid);
    if (status >= 0)
      wait_ok = 1;
#endif

    if (wait_ok)
    {
      if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
      else if (WIFSIGNALED(status))
        rc = 128 + WTERMSIG(status);
      else
        rc = -1;
    }
    else
    {
      rc = -1;
    }
  }

  /* Restore signal handlers only if they were modified */
  if (!(flags & MUTT_DETACH_PROCESS))
  {
    sigaction(SIGCONT, &oldcont, NULL);
    sigaction(SIGTSTP, &oldtstp, NULL);
  }

  /* reset SIGINT, SIGQUIT and SIGCHLD */
  mutt_unblock_signals_system(1);
  if (flags & MUTT_DETACH_PROCESS)
    sigprocmask(SIG_UNBLOCK, &set, NULL);

  return (rc);
}

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
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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
    }

    mutt_exec_shell(cmd);
  }
  else if (thepid != -1)
  {
#ifndef USE_IMAP
    /* wait for the (first) child process to finish */
    while (waitpid(thepid, &rc, 0) < 0)
    {
      if (errno != EINTR)
      {
        rc = -1;
        break;
      }
    }
#else
    rc = imap_wait_keepalive(thepid);
#endif
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

  if (thepid != -1)
  {
    if (WIFEXITED(rc))
      rc = WEXITSTATUS(rc);
    else if (WIFSIGNALED(rc))
      rc = 128 + WTERMSIG(rc);
    else
      rc = -1;
  }
  else
  {
    rc = -1;
  }

  return (rc);
}

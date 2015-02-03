// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "subprocess.h"

#include <sys/select.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <spawn.h>

#if defined(USE_PPOLL)
#include <poll.h>
#else
#include <sys/select.h>
#endif

extern char** environ;

#include "util.h"

using namespace std;

Subprocess::Subprocess(bool use_console, bool use_stderr) : pid_(-1),
                                                            use_console_(use_console),
                                                            use_stderr_(use_stderr) {
}

Subprocess::~Subprocess() {
  if (out_.fd >= 0)
    close(out_.fd);
  if (err_.fd >= 0)
    close(err_.fd);
  // Reap child if forgotten.
  if (pid_ != -1)
    Finish();
}

bool Subprocess::Start(SubprocessSet* set, const string& command) {
  int output_pipe[2];
  if (pipe(output_pipe) < 0)
    Fatal("pipe: %s", strerror(errno));
  out_.fd = output_pipe[0];

  int error_pipe[2];
  if (use_stderr_) {
    if (pipe(error_pipe) < 0)
      Fatal("error pipe: %s", strerror(errno));
    err_.fd = error_pipe[0];
  }

#if !defined(USE_PPOLL)
  // If available, we use ppoll in DoWork(); otherwise we use pselect
  // and so must avoid overly-large FDs.
  if (out_.fd >= static_cast<int>(FD_SETSIZE))
    Fatal("pipe: %s", strerror(EMFILE));
  if (use_stderr_ && err_.fd >= static_cast<int>(FD_SETSIZE))
    Fatal("error pipe: %s", strerror(EMFILE));
#endif  // !USE_PPOLL
  SetCloseOnExec(out_.fd);
  if (use_stderr_)
    SetCloseOnExec(err_.fd);

  posix_spawn_file_actions_t action;
  int err = posix_spawn_file_actions_init(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_init: %s", strerror(err));

  err = posix_spawn_file_actions_addclose(&action, output_pipe[0]);
  if (err != 0)
    Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));

  if (use_stderr_) {
    if (posix_spawn_file_actions_addclose(&action, error_pipe[0]) != 0)
      Fatal("posix_spawn_file_actions_addclose: %s", strerror(errno));
  }

  posix_spawnattr_t attr;
  err = posix_spawnattr_init(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_init: %s", strerror(err));

  short flags = 0;

  flags |= POSIX_SPAWN_SETSIGMASK;
  err = posix_spawnattr_setsigmask(&attr, &set->old_mask_);
  if (err != 0)
    Fatal("posix_spawnattr_setsigmask: %s", strerror(err));
  // Signals which are set to be caught in the calling process image are set to
  // default action in the new process image, so no explicit
  // POSIX_SPAWN_SETSIGDEF parameter is needed.

  if (!use_console_) {
    // Put the child in its own process group, so ctrl-c won't reach it.
    flags |= POSIX_SPAWN_SETPGROUP;
    // No need to posix_spawnattr_setpgroup(&attr, 0), it's the default.

    // Open /dev/null over stdin.
    err = posix_spawn_file_actions_addopen(&action, 0, "/dev/null", O_RDONLY,
          0);
    if (err != 0) {
      Fatal("posix_spawn_file_actions_addopen: %s", strerror(err));
    }

    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 1);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_adddup2(&action, use_stderr_ ?
                                           error_pipe[1] : output_pipe[1], 2);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_addclose(&action, output_pipe[1]);
    if (err != 0)
      Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));
    if (use_stderr_) {
      err = posix_spawn_file_actions_addclose(&action, error_pipe[1]);
      if (err != 0)
        Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));
    }
    // In the console case, output_pipe is still inherited by the child and
    // closed when the subprocess finishes, which then notifies ninja.
  }
#ifdef POSIX_SPAWN_USEVFORK
  flags |= POSIX_SPAWN_USEVFORK;
#endif

  err = posix_spawnattr_setflags(&attr, flags);
  if (err != 0)
    Fatal("posix_spawnattr_setflags: %s", strerror(err));

  const char* spawned_args[] = { "/bin/sh", "-c", command.c_str(), NULL };
  err = posix_spawn(&pid_, "/bin/sh", &action, &attr,
        const_cast<char**>(spawned_args), environ);
  if (err != 0)
    Fatal("posix_spawn: %s", strerror(err));

  err = posix_spawnattr_destroy(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_destroy: %s", strerror(err));
  err = posix_spawn_file_actions_destroy(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_destroy: %s", strerror(err));

  close(output_pipe[1]);
  if (use_stderr_)
    close(error_pipe[1]);
  return true;
}

void Subprocess::OnPipeReady(Pipe & pipe) {
  char buf[4 << 10];
  ssize_t len = read(pipe.fd, buf, sizeof(buf));
  if (len > 0) {
    pipe.buf.append(buf, static_cast<size_t>(len));
  } else {
    if (len < 0)
      Fatal("read: %s", strerror(errno));
    close(pipe.fd);
    pipe.fd = -1;
  }
}

ExitStatus Subprocess::Finish() {
  assert(pid_ != -1);
  int status;
  if (waitpid(pid_, &status, 0) < 0)
    Fatal("waitpid(%d): %s", pid_, strerror(errno));
  pid_ = -1;

#ifdef _AIX
  if (WIFEXITED(status) && WEXITSTATUS(status) & 0x80) {
    // Map the shell's exit code used for signal failure (128 + signal) to the
    // status code expected by AIX WIFSIGNALED and WTERMSIG macros which, unlike
    // other systems, uses a different bit layout.
    int signal = WEXITSTATUS(status) & 0x7f;
    status = (signal << 16) | signal;
  }
#endif

  if (WIFEXITED(status)) {
    int exit = WEXITSTATUS(status);
    if (exit == 0)
      return ExitSuccess;
  } else if (WIFSIGNALED(status)) {
    if (WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGTERM
        || WTERMSIG(status) == SIGHUP)
      return ExitInterrupted;
  }
  return ExitFailure;
}

bool Subprocess::Done() const {
  return out_.fd == -1 && err_.fd == -1;
}

const string& Subprocess::GetOutput() const {
  return out_.buf;
}

const string& Subprocess::GetError() const {
  return err_.buf;
}

int SubprocessSet::interrupted_;

void SubprocessSet::SetInterruptedFlag(int signum) {
  interrupted_ = signum;
}

void SubprocessSet::HandlePendingInterruption() {
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == -1) {
    perror("ninja: sigpending");
    return;
  }
  if (sigismember(&pending, SIGINT))
    interrupted_ = SIGINT;
  else if (sigismember(&pending, SIGTERM))
    interrupted_ = SIGTERM;
  else if (sigismember(&pending, SIGHUP))
    interrupted_ = SIGHUP;
}

SubprocessSet::SubprocessSet() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &set, &old_mask_) < 0)
    Fatal("sigprocmask: %s", strerror(errno));

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SetInterruptedFlag;
  if (sigaction(SIGINT, &act, &old_int_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &act, &old_term_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &act, &old_hup_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
}

SubprocessSet::~SubprocessSet() {
  Clear();

  if (sigaction(SIGINT, &old_int_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &old_term_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &old_hup_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigprocmask(SIG_SETMASK, &old_mask_, 0) < 0)
    Fatal("sigprocmask: %s", strerror(errno));
}

Subprocess *SubprocessSet::Add(const string& command, bool use_console, bool use_stderr) {
  Subprocess *subprocess = new Subprocess(use_console, use_stderr);
  if (!subprocess->Start(this, command)) {
    delete subprocess;
    return 0;
  }
  running_.push_back(subprocess);
  return subprocess;
}

#ifdef USE_PPOLL
bool SubprocessSet::DoWork() {
  vector<pollfd> fds;
  nfds_t nfds = 0;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    Subprocess::Pipe* const pipes[2] = { &(*i)->out_, &(*i)->err_ };
    for (int pipe_i = 0; pipe_i < 2; ++pipe_i) {
      const Subprocess::Pipe& pipe = *pipes[pipe_i];
      if (pipe.fd >= 0) {
        pollfd pfd = { pipe.fd, POLLIN | POLLPRI, 0 };
        fds.push_back(pfd);
        ++nfds;
      }
    }
  }

  interrupted_ = 0;
  int ret = ppoll(&fds.front(), nfds, NULL, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: ppoll");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  nfds_t cur_nfd = 0;
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    Subprocess::Pipe * const pipes[2] = { &(*i)->out_, &(*i)->err_ };
    for (int pipe_i = 0; pipe_i < 2; ++pipe_i) {
      Subprocess::Pipe& pipe = *pipes[pipe_i];
      if (pipe.fd >= 0) {
        assert(pipe.fd == fds[cur_nfd].fd);
        if (fds[cur_nfd++].revents)
          (*i)->OnPipeReady(pipe);
      }
    }
  }

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    if ((*i)->Done()) {
      finished_.push(*i);
      i = running_.erase(i);
    } else {
      ++i;
    }
  }

  return IsInterrupted();
}

#else  // !defined(USE_PPOLL)
bool SubprocessSet::DoWork() {
  fd_set set;
  int nfds = 0;
  FD_ZERO(&set);

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    assert(!(*i)->Done());
    Subprocess::Pipe * const pipes[2] = { &(*i)->out_, &(*i)->err_ };
    for (int pipeIndex = 0; pipeIndex < 2; ++pipeIndex) {
      Subprocess::Pipe & pipe = *pipes[pipeIndex];
      if (pipe.fd >= 0) {
        FD_SET(pipe.fd, &set);
        if (nfds < pipe.fd+1)
          nfds = pipe.fd+1;
        }
    }
  }

  interrupted_ = 0;
  int ret = pselect(nfds, &set, 0, 0, 0, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: pselect");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    assert(!(*i)->Done());
    Subprocess::Pipe * const pipes[2] = { &(*i)->out_, &(*i)->err_ };
    for (int pipeIndex = 0; pipeIndex < 2; ++pipeIndex) {
      Subprocess::Pipe & pipe = *pipes[pipeIndex];
      if (pipe.fd >= 0 && FD_ISSET(pipe.fd, &set)) {
        (*i)->OnPipeReady(pipe);
      }
    }
    if ((*i)->Done()) {
      finished_.push(*i);
      i = running_.erase(i);
      continue;
    }
    ++i;
  }

  return IsInterrupted();
}
#endif  // !defined(USE_PPOLL)

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    // Since the foreground process is in our process group, it will receive
    // the interruption signal (i.e. SIGINT or SIGTERM) at the same time as us.
    if (!(*i)->use_console_)
      kill(-(*i)->pid_, interrupted_);
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
}

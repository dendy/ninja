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

#ifndef NINJA_SUBPROCESS_H_
#define NINJA_SUBPROCESS_H_

#include <string>
#include <vector>
#include <queue>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

// ppoll() exists on FreeBSD, but only on newer versions.
#ifdef __FreeBSD__
#  include <sys/param.h>
#  if defined USE_PPOLL && __FreeBSD_version < 1002000
#    undef USE_PPOLL
#  endif
#endif

#include "exit_status.h"

/// Subprocess wraps a single async subprocess.  It is entirely
/// passive: it expects the caller to notify it when its fds are ready
/// for reading, as well as call Finish() to reap the child once done()
/// is true.
struct Subprocess {
  ~Subprocess();

  /// Returns ExitSuccess on successful process exit, ExitInterrupted if
  /// the process was interrupted, ExitFailure if it otherwise failed.
  ExitStatus Finish();

  bool UseStdErr() const { return use_stderr_; }
  bool Done() const;

  const std::string& GetOutput() const;
  const std::string& GetError() const;

 private:
  Subprocess(bool use_console, bool use_stderr);
  bool Start(struct SubprocessSet* set, const std::string& command);

#ifdef _WIN32
  struct Pipe {
    HANDLE handle = NULL;
    OVERLAPPED overlapped{};
    char overlapped_buf[4 << 10];
    bool is_reading = false;
    Subprocess * subprocess;
    std::string buf;
  };

  /// Set up pipe_ as the parent-side pipe of the subprocess; return the
  /// other end of the pipe, usable in the child process.
  HANDLE SetupPipe(HANDLE ioport, Pipe & pipe, bool out);

  HANDLE child_;
  Pipe out_, err_;
  void OnPipeReady(Pipe & pipe);
#else
  struct Pipe {
    Pipe() : fd(-1) {}
    int fd;
    std::string buf;
  };

  void OnPipeReady(Pipe & pipe);

  Pipe out_, err_;
  pid_t pid_;
#endif
  bool use_console_;
  bool use_stderr_;

  friend struct SubprocessSet;
};

/// SubprocessSet runs a ppoll/pselect() loop around a set of Subprocesses.
/// DoWork() waits for any state change in subprocesses; finished_
/// is a queue of subprocesses as they finish.
struct SubprocessSet {
  SubprocessSet();
  ~SubprocessSet();

  Subprocess* Add(const std::string& command, bool use_console = false, bool use_stderr = false);
  bool DoWork();
  Subprocess* NextFinished();
  void Clear();

  std::vector<Subprocess*> running_;
  std::queue<Subprocess*> finished_;

#ifdef _WIN32
  static BOOL WINAPI NotifyInterrupted(DWORD dwCtrlType);
  static HANDLE ioport_;
#else
  static void SetInterruptedFlag(int signum);
  static void HandlePendingInterruption();
  /// Store the signal number that causes the interruption.
  /// 0 if not interruption.
  static int interrupted_;

  static bool IsInterrupted() { return interrupted_ != 0; }

  struct sigaction old_int_act_;
  struct sigaction old_term_act_;
  struct sigaction old_hup_act_;
  sigset_t old_mask_;
#endif
};

#endif // NINJA_SUBPROCESS_H_

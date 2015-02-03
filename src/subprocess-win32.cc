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

#include <assert.h>
#include <stdio.h>

#include <algorithm>

#include "util.h"

using namespace std;

Subprocess::Subprocess(bool use_console, bool use_stderr) : child_(NULL),
                                           use_console_(use_console),
                                           use_stderr_(use_stderr) {
}

Subprocess::~Subprocess() {
  if (out_.handle) {
    if (!CloseHandle(out_.handle))
      Win32Fatal("CloseHandle");
  }
  if (use_stderr_) {
    if (err_.handle) {
      if (!CloseHandle(err_.handle))
        Win32Fatal("CloseHandle");
    }
  }
  // Reap child if forgotten.
  if (child_)
    Finish();
}

HANDLE Subprocess::SetupPipe(HANDLE ioport, Pipe & pipe, bool out) {
  char pipe_name[100];
  snprintf(pipe_name, sizeof(pipe_name),
           "\\\\.\\pipe\\ninja_pid%lu_sp%p%s", GetCurrentProcessId(), this, out ? "out" : "err");

  pipe.subprocess = this;

  pipe.handle = ::CreateNamedPipeA(pipe_name,
                             PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE,
                             PIPE_UNLIMITED_INSTANCES,
                             0, 0, INFINITE, NULL);
  if (pipe.handle == INVALID_HANDLE_VALUE)
    Win32Fatal("CreateNamedPipe");

  if (!CreateIoCompletionPort(pipe.handle, ioport, (ULONG_PTR)&pipe, 0))
    Win32Fatal("CreateIoCompletionPort");

  memset(&pipe.overlapped, 0, sizeof(pipe.overlapped));
  if (!ConnectNamedPipe(pipe.handle, &pipe.overlapped) &&
      GetLastError() != ERROR_IO_PENDING) {
    Win32Fatal("ConnectNamedPipe");
  }

  // Get the write end of the pipe as a handle inheritable across processes.
  HANDLE output_write_handle =
      CreateFileA(pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  HANDLE output_write_child;
  if (!DuplicateHandle(GetCurrentProcess(), output_write_handle,
                       GetCurrentProcess(), &output_write_child,
                       0, TRUE, DUPLICATE_SAME_ACCESS)) {
    Win32Fatal("DuplicateHandle");
  }
  CloseHandle(output_write_handle);

  return output_write_child;
}

bool Subprocess::Start(SubprocessSet* set, const string& command) {
  HANDLE out_child_pipe = SetupPipe(set->ioport_, out_, true);
  HANDLE err_child_pipe = use_stderr_ ? SetupPipe(set->ioport_, err_, false) : nullptr;

  SECURITY_ATTRIBUTES security_attributes;
  memset(&security_attributes, 0, sizeof(SECURITY_ATTRIBUTES));
  security_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
  security_attributes.bInheritHandle = TRUE;
  // Must be inheritable so subprocesses can dup to children.
  HANDLE nul =
      CreateFileA("NUL", GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  &security_attributes, OPEN_EXISTING, 0, NULL);
  if (nul == INVALID_HANDLE_VALUE)
    Fatal("couldn't open nul");

  STARTUPINFOA startup_info;
  memset(&startup_info, 0, sizeof(startup_info));
  startup_info.cb = sizeof(STARTUPINFO);
  if (!use_console_) {
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = nul;
    startup_info.hStdOutput = out_child_pipe;
    startup_info.hStdError = use_stderr_ ? err_child_pipe : out_child_pipe;
  }
  // In the console case, child_pipe is still inherited by the child and closed
  // when the subprocess finishes, which then notifies ninja.

  PROCESS_INFORMATION process_info;
  memset(&process_info, 0, sizeof(process_info));

  // Ninja handles ctrl-c, except for subprocesses in console pools.
  DWORD process_flags = use_console_ ? 0 : CREATE_NEW_PROCESS_GROUP;

  // Do not prepend 'cmd /c' on Windows, this breaks command
  // lines greater than 8,191 chars.
  if (!CreateProcessA(NULL, (char*)command.c_str(), NULL, NULL,
                      /* inherit handles */ TRUE, process_flags,
                      NULL, NULL,
                      &startup_info, &process_info)) {
    DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      // File (program) not found error is treated as a normal build
      // action failure.
      if (out_child_pipe)
        CloseHandle(out_child_pipe);
      CloseHandle(out_.handle);
      if (err_child_pipe)
          CloseHandle(err_child_pipe);
      CloseHandle(nul);
      out_.handle = NULL;
      err_.handle = NULL;
      // child_ is already NULL;
      (use_stderr_ ? out_.buf : err_.buf) = "CreateProcess failed: The system cannot find the file "
          "specified.\n";
      return true;
    } else {
      fprintf(stderr, "\nCreateProcess failed. Command attempted:\n\"%s\"\n",
              command.c_str());
      const char* hint = NULL;
      // ERROR_INVALID_PARAMETER means the command line was formatted
      // incorrectly. This can be caused by a command line being too long or
      // leading whitespace in the command. Give extra context for this case.
      if (error == ERROR_INVALID_PARAMETER) {
        if (command.length() > 0 && (command[0] == ' ' || command[0] == '\t'))
          hint = "command contains leading whitespace";
        else
          hint = "is the command line too long?";
      }
      Win32Fatal("CreateProcess", hint);
    }
  }

  // Close pipe channel only used by the child.
  if (out_child_pipe)
    CloseHandle(out_child_pipe);
  if (err_child_pipe)
    CloseHandle(err_child_pipe);
  CloseHandle(nul);

  CloseHandle(process_info.hThread);
  child_ = process_info.hProcess;

  return true;
}

void Subprocess::OnPipeReady(Pipe & pipe) {
  DWORD bytes;
  if (!GetOverlappedResult(pipe.handle, &pipe.overlapped, &bytes, TRUE)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      CloseHandle(pipe.handle);
      pipe.handle = NULL;
      return;
    }
    Win32Fatal("GetOverlappedResult");
  }

  if (pipe.is_reading && bytes)
    pipe.buf.append(pipe.overlapped_buf, bytes);

  memset(&pipe.overlapped, 0, sizeof(pipe.overlapped));
  pipe.is_reading = true;
  if (!::ReadFile(pipe.handle, pipe.overlapped_buf, sizeof(pipe.overlapped_buf),
                  &bytes, &pipe.overlapped)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      CloseHandle(pipe.handle);
      pipe.handle = NULL;
      return;
    }
    if (GetLastError() != ERROR_IO_PENDING)
      Win32Fatal("ReadFile");
  }

  // Even if we read any bytes in the readfile call, we'll enter this
  // function again later and get them at that point.
}

ExitStatus Subprocess::Finish() {
  if (!child_)
    return ExitFailure;

  // TODO: add error handling for all of these.
  WaitForSingleObject(child_, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(child_, &exit_code);

  CloseHandle(child_);
  child_ = NULL;

  return exit_code == 0              ? ExitSuccess :
         exit_code == CONTROL_C_EXIT ? ExitInterrupted :
                                       ExitFailure;
}

bool Subprocess::Done() const {
  return out_.handle == NULL && err_.handle == NULL;
}

const string& Subprocess::GetOutput() const {
  return out_.buf;
}

const string& Subprocess::GetError() const {
  return err_.buf;
}

HANDLE SubprocessSet::ioport_;

SubprocessSet::SubprocessSet() {
  ioport_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!ioport_)
    Win32Fatal("CreateIoCompletionPort");
  if (!SetConsoleCtrlHandler(NotifyInterrupted, TRUE))
    Win32Fatal("SetConsoleCtrlHandler");
}

SubprocessSet::~SubprocessSet() {
  Clear();

  SetConsoleCtrlHandler(NotifyInterrupted, FALSE);
  CloseHandle(ioport_);
}

BOOL WINAPI SubprocessSet::NotifyInterrupted(DWORD dwCtrlType) {
  if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
    if (!PostQueuedCompletionStatus(ioport_, 0, 0, NULL))
      Win32Fatal("PostQueuedCompletionStatus");
    return TRUE;
  }

  return FALSE;
}

Subprocess *SubprocessSet::Add(const string& command, bool use_console, bool use_stderr) {
  Subprocess *subprocess = new Subprocess(use_console, use_stderr);
  if (!subprocess->Start(this, command)) {
    delete subprocess;
    return 0;
  }
  if (subprocess->child_)
    running_.push_back(subprocess);
  else
    finished_.push(subprocess);
  return subprocess;
}

bool SubprocessSet::DoWork() {
  DWORD bytes_read;
  OVERLAPPED* overlapped;
  Subprocess::Pipe * pipe;

  if (!GetQueuedCompletionStatus(ioport_, &bytes_read, (PULONG_PTR)&pipe,
                                 &overlapped, INFINITE)) {
    if (GetLastError() != ERROR_BROKEN_PIPE)
      Win32Fatal("GetQueuedCompletionStatus");
  }

  if (!pipe) // A NULL indicates that we were interrupted and is
                // delivered by NotifyInterrupted above.
    return true;

  Subprocess * subproc = pipe->subprocess;

  subproc->OnPipeReady(*pipe);

  if (subproc->Done()) {
    vector<Subprocess*>::iterator end =
        remove(running_.begin(), running_.end(), subproc);
    if (running_.end() != end) {
      finished_.push(subproc);
      running_.resize(end - running_.begin());
    }
  }

  return false;
}

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    // Since the foreground process is in our process group, it will receive a
    // CTRL_C_EVENT or CTRL_BREAK_EVENT at the same time as us.
    if ((*i)->child_ && !(*i)->use_console_) {
      if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                    GetProcessId((*i)->child_))) {
        Win32Fatal("GenerateConsoleCtrlEvent");
      }
    }
  }
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
}

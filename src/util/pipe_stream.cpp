/*******************************************************************\

Module: A stdin/stdout pipe as STL stream

Author:

\*******************************************************************/

/// \file
/// A stdin/stdout pipe as STL stream

#include <cstdio>
#include <istream>
#include <vector>

#include "unicode.h"
#include "pipe_stream.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <cstring>
#endif

#define READ_BUFFER_SIZE 1024

/// Constructor for external process
pipe_streamt::pipe_streamt(
  const std::string &_executable,
  const std::list<std::string> &_args):
  std::iostream(&buffer),
  executable(_executable),
  args(_args)
{
  #ifdef _WIN32
  pi.hProcess = 0;
  #endif
}

/// Starts an external process. A new process is forked and run returns
/// immediately.
/// \return Returns -1 if an error occurs.
#ifdef _WIN32

int pipe_streamt::run()
{
  HANDLE hOutputReadTmp, hOutputRead, hOutputWrite;
  HANDLE hInputWriteTmp, hInputRead, hInputWrite;
  HANDLE hErrorWrite;
  SECURITY_ATTRIBUTES sa;

  sa.nLength=sizeof(SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor=NULL;
  sa.bInheritHandle=TRUE;

  // Create the child output pipe
  if(!CreatePipe(&hOutputReadTmp, &hOutputWrite, &sa, 0))
    return -1;

  // Create duplicate of output write handle for the std error handle
  if(!DuplicateHandle(GetCurrentProcess(), hOutputWrite,
                      GetCurrentProcess(), &hErrorWrite, 0,
                      TRUE, DUPLICATE_SAME_ACCESS))
    return -1;

  // Create child input pipe
  if(!CreatePipe(&hInputRead, &hInputWriteTmp, &sa, 0))
    return -1;

  // Create new output read handle and the input write handles
  if(!DuplicateHandle(GetCurrentProcess(), hOutputReadTmp,
                       GetCurrentProcess(),
                       &hOutputRead,
                       0, FALSE, // uninheritable.
                       DUPLICATE_SAME_ACCESS))
    return -1;

  if(!DuplicateHandle(GetCurrentProcess(), hInputWriteTmp,
                       GetCurrentProcess(),
                       &hInputWrite,
                       0, FALSE, //  uninheritable.
                       DUPLICATE_SAME_ACCESS))
    return -1;

  if(!CloseHandle(hOutputReadTmp) || !CloseHandle(hInputWriteTmp))
    return -1;

  // now execute the child process
  STARTUPINFOW si;

  ZeroMemory(&si, sizeof(STARTUPINFO));
  si.cb=sizeof(STARTUPINFO);
  si.dwFlags=STARTF_USESTDHANDLES;
  si.hStdOutput=hOutputWrite;
  si.hStdInput=hInputRead;
  si.hStdError=hErrorWrite;

  std::wstring command = ::widen(executable);

  for(const auto &s : args)
        command += L" " + ::widen(s);

  LPWSTR lpCommandLine = new wchar_t[command.length()+1];

  #ifdef _MSC_VER
  wcscpy_s(lpCommandLine, command.length()+1, command.c_str());
  #else
  wcsncpy(lpCommandLine, command.c_str(), command.length()+1);
  #endif

  BOOL ret=CreateProcessW(NULL, lpCommandLine, NULL, NULL, TRUE,
                          CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

  delete lpCommandLine; // clean up

  if(!ret)
    return -1;

  buffer.set_in(hInputWrite);
  buffer.set_out(hOutputRead);

  return 0;
}

#else

int pipe_streamt::run()
{
  filedescriptor_streambuft::HANDLE in[2], out[2];

  if(pipe(in)==-1 || pipe(out)==-1)
    return -1;

  pid=fork();

  if(pid==0)
  {
    // child
    close(in[1]);
    close(out[0]);
    dup2(in[0], STDIN_FILENO);
    dup2(out[1], STDOUT_FILENO);

    char **_argv=new char * [args.size()+2];

    _argv[0]=strdup(executable.c_str());

    unsigned i=1;

    for(std::list<std::string>::const_iterator
        a_it=args.begin();
        a_it!=args.end();
        a_it++, i++)
       _argv[i]=strdup(a_it->c_str());

    _argv[args.size()+1]=NULL;

    int result=execvp(executable.c_str(), _argv);

    if(result==-1)
      perror(0);

    return result;
  }
  else if(pid==-1)
  {
    // error on parent
    return -1;
  }

  // parent, mild cleanup
  close(in[0]);
  close(out[1]);

  // attach to streambuf
  buffer.set_in(in[1]);
  buffer.set_out(out[0]);

  return pid;
}

#endif

/// Wait for the process to terminate
int pipe_streamt::wait()
{
  #ifdef _WIN32
  DWORD status;

  if(pi.hProcess==0)
    return -1;

  if(WaitForSingleObject(pi.hProcess, INFINITE)==WAIT_FAILED)
    return -1;

  GetExitCodeProcess(pi.hProcess, &status);
  return status;
  #else
  if(pid<=0)
    return -1;

  int result, status;
  result=waitpid(pid, &status, WUNTRACED);
  if(result<=0)
    return -1;

  return WEXITSTATUS(status);
  #endif
}

/// Constructor
filedescriptor_streambuft::filedescriptor_streambuft():
  #ifdef _WIN32
  proc_in(INVALID_HANDLE_VALUE),
  proc_out(INVALID_HANDLE_VALUE)
  #else
  proc_in(STDOUT_FILENO),
  proc_out(STDIN_FILENO)
  #endif
{
  in_buffer=new char[READ_BUFFER_SIZE];
  setg(in_buffer, in_buffer, in_buffer);
}

/// Destructor
filedescriptor_streambuft::~filedescriptor_streambuft()
{
  #ifdef _WIN32

  if(proc_in!=INVALID_HANDLE_VALUE)
    CloseHandle(proc_in);

  if(proc_out!=INVALID_HANDLE_VALUE)
    CloseHandle(proc_out);

  #else

  if(proc_in!=STDOUT_FILENO)
    close(proc_in);

  if(proc_out!=STDIN_FILENO)
    close(proc_out);

  #endif

  delete in_buffer;
}

/// write one character to the piped process
std::streambuf::int_type filedescriptor_streambuft::overflow(
  std::streambuf::int_type character)
{
  if(character!=EOF)
  {
    char buf=character;
#ifdef _WIN32
    DWORD len;
    WriteFile(proc_in, &buf, 1, &len, NULL);
#else
    int len=write(proc_in, &buf, 1);
#endif
    if(len!=1)
    {
      return EOF;
    }
  }
  return character;
}

/// write a number of character to the piped process
std::streamsize filedescriptor_streambuft::xsputn(
  const char* str, std::streamsize count)
{
#ifdef _WIN32
  DWORD len;
  WriteFile(proc_in, str, (DWORD)count, &len, NULL);
  return len;
#else
  return write(proc_in, str, count);
#endif
}

/// read a character from the piped process
std::streambuf::int_type filedescriptor_streambuft::underflow()
{
  if(gptr()==0)
    return traits_type::eof();

  if(gptr()<egptr())
    return traits_type::to_int_type(*gptr());

  #ifdef _WIN32
  DWORD len;
  if(!ReadFile(proc_out, eback(), READ_BUFFER_SIZE, &len, NULL))
    return traits_type::eof();
  #else
  ssize_t len=read(proc_out, eback(), READ_BUFFER_SIZE);
  if(len==-1)
    return traits_type::eof();
  #endif

  setg(eback(), eback(), eback()+(sizeof(char_type)*len));

  if(len==0)
    return traits_type::eof();

  return traits_type::to_int_type(*gptr());
}

/// read a number of characters from the piped process
std::streamsize filedescriptor_streambuft::xsgetn(
  char *target, std::streamsize count)
{
  std::streamsize available = showmanyc();

  if(count <= available)
  {
    memcpy(target, gptr(), count * sizeof(char_type));
    gbump(count);

    return count;
  }

  memcpy(target, gptr(), available * sizeof(char_type));
  gbump(available);

  if(traits_type::eof() == underflow())
    return available;

  return (available + xsgetn(target+available, count-available));
}

/// determine number of available characters in stream
std::streamsize filedescriptor_streambuft::showmanyc()
{
  if(gptr()==0)
    return 0;

  if(gptr()<egptr())
    return egptr()-gptr();

  return 0;
}

#ifdef UNIT

#include <iostream>

int main(int argc, char** argv)
{
  std::string command("cat");
  std::list<std::string> arguments;

  pipe_streamt process(command, arguments);
  if(process.run() < 0)
    return -1;

  process << "xxx\n\n";

  char token;
  for(int i=0; i<3; ++i)
  {
    process >> token;
    std::cout << "Answer: " << token << '\n';
  }

  return process.wait();
}

#endif

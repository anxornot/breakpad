// Copyright 2003 Google LLC
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google LLC nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <Windows.h>
#include <shellapi.h>

#include <string>
#include <utility>

#include "breakpad_googletest_includes.h"

namespace tools {
namespace windows {
namespace dump_syms {

namespace {

// Root names of PDB and dumped symbol files to be regression tested. These are
// specified in complexity of the resulting dumped symbol files.
const wchar_t* kRootNames[] = {
  // A PDB file with no OMAP data.
  L"dump_syms_regtest",
  // A PDB file with OMAP data for an image that has been function-level
  // reordered.
  L"omap_reorder_funcs",
  // A PDB file with OMAP data for an image that had new content injected, all
  // of it with source data.
  L"omap_stretched_filled",
  // A PDB file with OMAP data for an image that had new content injected, but
  // without source data.
  L"omap_stretched",
  // A PDB file with OMAP data for an image that has been basic block reordered.
  L"omap_reorder_bbs",
  // A 64bit PDB file with no OMAP data.
  L"dump_syms_regtest64",
};

const wchar_t* kPEOnlyRootNames[] = {
  L"pe_only_symbol_test",
};

void TrimLastComponent(const std::wstring& path,
                       std::wstring* trimmed,
                       std::wstring* component) {
  size_t len = path.size();
  while (len > 0 && path[len - 1] != '\\')
    --len;

  if (component != nullptr)
    component->assign(path.c_str() + len, path.c_str() + path.size());

  while (len > 0 && path[len - 1] == '\\')
    --len;

  if (trimmed != nullptr)
    trimmed->assign(path.c_str(), len);
}

// Get the directory of the current executable.
bool GetSelfDirectory(std::wstring* self_dir) {
  std::wstring command_line = GetCommandLineW();

  int num_args = 0;
  wchar_t** args = nullptr;
  args = ::CommandLineToArgvW(command_line.c_str(), &num_args);
  if (args == nullptr)
    return false;

  *self_dir = args[0];
  TrimLastComponent(*self_dir, self_dir, nullptr);

  return true;
}

void RunCommand(const std::wstring& command_line,
                std::string* stdout_string) {
  // Create a PIPE for the child process stdout.
  HANDLE child_stdout_read = 0;
  HANDLE child_stdout_write = 0;
  SECURITY_ATTRIBUTES sec_attr_stdout = {};
  sec_attr_stdout.nLength = sizeof(sec_attr_stdout);
  sec_attr_stdout.bInheritHandle = TRUE;
  ASSERT_TRUE(::CreatePipe(&child_stdout_read, &child_stdout_write,
                           &sec_attr_stdout, 0));
  ASSERT_TRUE(::SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT,
                                     0));

  // Create a PIPE for the child process stdin.
  HANDLE child_stdin_read = 0;
  HANDLE child_stdin_write = 0;
  SECURITY_ATTRIBUTES sec_attr_stdin = {};
  sec_attr_stdin.nLength = sizeof(sec_attr_stdin);
  sec_attr_stdin.bInheritHandle = TRUE;
  ASSERT_TRUE(::CreatePipe(&child_stdin_read, &child_stdin_write,
                           &sec_attr_stdin, 0));
  ASSERT_TRUE(::SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT,
                                     0));

  // Startup the child.
  STARTUPINFO startup_info = {};
  PROCESS_INFORMATION process_info = {};
  startup_info.cb = sizeof(STARTUPINFO);
  startup_info.hStdError = nullptr;
  startup_info.hStdInput = child_stdin_read;
  startup_info.hStdOutput = child_stdout_write;
  startup_info.dwFlags = STARTF_USESTDHANDLES;
  ASSERT_TRUE(::CreateProcessW(nullptr, (LPWSTR)command_line.c_str(), nullptr,
                               nullptr, TRUE, 0, nullptr, nullptr,
                               &startup_info, &process_info));

  // Collect the output.
  ASSERT_TRUE(::CloseHandle(child_stdout_write));
  char buffer[4096] = {};
  DWORD bytes_read = 0;
  while (::ReadFile(child_stdout_read, buffer, sizeof(buffer), &bytes_read,
                    nullptr) && bytes_read > 0) {
    stdout_string->append(buffer, bytes_read);
  }

  // Wait for the process to finish.
  ::WaitForSingleObject(process_info.hProcess, INFINITE);

  // Shut down all of our handles.
  ASSERT_TRUE(::CloseHandle(process_info.hThread));
  ASSERT_TRUE(::CloseHandle(process_info.hProcess));
  ASSERT_TRUE(::CloseHandle(child_stdin_write));
  ASSERT_TRUE(::CloseHandle(child_stdin_read));
  ASSERT_TRUE(::CloseHandle(child_stdout_read));
}

void GetFileContents(const std::wstring& path, std::string* content) {
  FILE* f = ::_wfopen(path.c_str(), L"rb");
  ASSERT_TRUE(f != nullptr);

  char buffer[4096] = {};
  while (true) {
    size_t bytes_read = ::fread(buffer, 1, sizeof(buffer), f);
    if (bytes_read == 0)
      break;
    content->append(buffer, bytes_read);
  }
}

class DumpSymsRegressionTest : public testing::TestWithParam<const wchar_t*> {
 public:
  virtual void SetUp() {
    std::wstring self_dir;
    ASSERT_TRUE(GetSelfDirectory(&self_dir));
    dump_syms_exe = self_dir + L"\\dump_syms.exe";

    TrimLastComponent(self_dir, &testdata_dir, nullptr);
    testdata_dir += L"\\testdata";
  }

  std::wstring dump_syms_exe;
  std::wstring testdata_dir;
};

class DumpSymsPEOnlyRegressionTest : public testing::TestWithParam<const wchar_t*> {
public:
  virtual void SetUp() {
    std::wstring self_dir;
    ASSERT_TRUE(GetSelfDirectory(&self_dir));
    dump_syms_exe = self_dir + L"\\dump_syms.exe";

    TrimLastComponent(self_dir, &testdata_dir, nullptr);
    testdata_dir += L"\\testdata";
  }

  std::wstring dump_syms_exe;
  std::wstring testdata_dir;
};

}  //namespace

TEST_P(DumpSymsRegressionTest, EnsureDumpedSymbolsMatch) {
  const wchar_t* root_name = GetParam();
  std::wstring root_path = testdata_dir + L"\\" + root_name;

  std::wstring sym_path = root_path + L".sym";
  std::string expected_symbols;
  ASSERT_NO_FATAL_FAILURE(GetFileContents(sym_path, &expected_symbols));

  std::wstring pdb_path = root_path + L".pdb";
  std::wstring command_line = L"\"" + dump_syms_exe + L"\" \"" +
    pdb_path + L"\"";
  std::string symbols;
  ASSERT_NO_FATAL_FAILURE(RunCommand(command_line, &symbols));

  EXPECT_EQ(expected_symbols, symbols);
}

INSTANTIATE_TEST_SUITE_P(DumpSyms, DumpSymsRegressionTest,
  testing::ValuesIn(kRootNames));

TEST_P(DumpSymsPEOnlyRegressionTest, EnsurePEOnlyDumpedSymbolsMatch) {
  const wchar_t* root_name = GetParam();
  std::wstring root_path = testdata_dir + L"\\" + root_name;

  std::wstring sym_path = root_path + L".sym";
  std::string expected_symbols;
  ASSERT_NO_FATAL_FAILURE(GetFileContents(sym_path, &expected_symbols));

  std::wstring dll_path = root_path + L".dll";
  std::wstring command_line = L"\"" + dump_syms_exe + L"\" --pe \"" +
    dll_path + L"\"";
  std::string symbols;
  ASSERT_NO_FATAL_FAILURE(RunCommand(command_line, &symbols));

  EXPECT_EQ(expected_symbols, symbols);
}

INSTANTIATE_TEST_SUITE_P(PEOnlyDumpSyms, DumpSymsPEOnlyRegressionTest,
  testing::ValuesIn(kPEOnlyRootNames));


}  // namespace dump_syms
}  // namespace windows
}  // namespace tools

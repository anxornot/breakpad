// Copyright 2012 Google LLC
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

// linux_core_dumper_unittest.cc:
// Unit tests for google_breakpad::LinuxCoreDumoer.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <string>

#include "breakpad_googletest_includes.h"
#include "client/linux/minidump_writer/linux_core_dumper.h"
#include "common/linux/tests/crash_generator.h"
#include "common/tests/auto_tempdir.h"
#include "common/tests/file_utils.h"
#include "common/using_std_string.h"

namespace google_breakpad {

class LinuxCoreDumperTest : public testing::Test {
 protected:
  AutoTempDir temp_dir;
};

TEST_F(LinuxCoreDumperTest, GetMappingAbsolutePath) {
  const LinuxCoreDumper dumper(getpid(), "core", "/tmp", "/mnt/root");
  const MappingInfo mapping = {0, 0, {0, 0}, 0, false, "/usr/lib/libc.so"};

  char path[PATH_MAX];
  dumper.GetMappingAbsolutePath(mapping, path);

  EXPECT_STREQ("/mnt/root/usr/lib/libc.so", path);
}

TEST_F(LinuxCoreDumperTest, BuildProcPath) {
  const pid_t pid = getpid();
  const char procfs_path[] = "/procfs_copy";
  LinuxCoreDumper dumper(getpid(), "core_file", procfs_path);

  char maps_path[NAME_MAX] = "";
  char maps_path_expected[NAME_MAX];
  snprintf(maps_path_expected, sizeof(maps_path_expected),
           "%s/maps", procfs_path);
  EXPECT_TRUE(dumper.BuildProcPath(maps_path, pid, "maps"));
  EXPECT_STREQ(maps_path_expected, maps_path);

  EXPECT_FALSE(dumper.BuildProcPath(nullptr, pid, "maps"));
  EXPECT_FALSE(dumper.BuildProcPath(maps_path, pid, ""));
  EXPECT_FALSE(dumper.BuildProcPath(maps_path, pid, nullptr));

  char long_node[NAME_MAX];
  size_t long_node_len = NAME_MAX - strlen(procfs_path) - 1;
  memset(long_node, 'a', long_node_len);
  long_node[long_node_len] = '\0';
  EXPECT_FALSE(dumper.BuildProcPath(maps_path, pid, long_node));
}

TEST_F(LinuxCoreDumperTest, VerifyDumpWithMultipleThreads) {
  CrashGenerator crash_generator;
  if (!crash_generator.HasDefaultCorePattern()) {
    fprintf(stderr, "LinuxCoreDumperTest.VerifyDumpWithMultipleThreads test "
            "is skipped due to non-default core pattern\n");
    return;
  }

  const unsigned kNumOfThreads = 3;
  const unsigned kCrashThread = 1;
  const int kCrashSignal = SIGABRT;
  pid_t child_pid;
  ASSERT_TRUE(crash_generator.CreateChildCrash(kNumOfThreads, kCrashThread,
                                               kCrashSignal, &child_pid));

  const string core_file = crash_generator.GetCoreFilePath();
  const string procfs_path = crash_generator.GetDirectoryOfProcFilesCopy();

#if defined(__ANDROID__)
  struct stat st;
  if (stat(core_file.c_str(), &st) != 0) {
    fprintf(stderr, "LinuxCoreDumperTest.VerifyDumpWithMultipleThreads test is "
            "skipped due to no core file being generated\n");
    return;
  }
#endif

  LinuxCoreDumper dumper(child_pid, core_file.c_str(), procfs_path.c_str());

  EXPECT_TRUE(dumper.Init());

  EXPECT_TRUE(dumper.IsPostMortem());

  // These are no-ops and should always return true.
  EXPECT_TRUE(dumper.ThreadsSuspend());
  EXPECT_TRUE(dumper.ThreadsResume());

  // Linux does not set the crash address with SIGABRT, so make sure it always
  // sets the crash address to 0.
  EXPECT_EQ(0U, dumper.crash_address());
  EXPECT_EQ(kCrashSignal, dumper.crash_signal());
  EXPECT_EQ(crash_generator.GetThreadId(kCrashThread),
            dumper.crash_thread());

#if defined(THREAD_SANITIZER)
  EXPECT_GE(dumper.threads().size(), kNumOfThreads);
#else
  EXPECT_EQ(dumper.threads().size(), kNumOfThreads);
#endif
  for (unsigned i = 0; i < kNumOfThreads; ++i) {
    ThreadInfo info;
    EXPECT_TRUE(dumper.GetThreadInfoByIndex(i, &info));
    const void* stack;
    size_t stack_len;
    EXPECT_TRUE(dumper.GetStackInfo(&stack, &stack_len, info.stack_pointer));
    EXPECT_EQ(getpid(), info.ppid);
  }
}

TEST_F(LinuxCoreDumperTest, VerifyExceptionDetails) {
  CrashGenerator crash_generator;
  if (!crash_generator.HasDefaultCorePattern()) {
    fprintf(stderr, "LinuxCoreDumperTest.VerifyDumpWithMultipleThreads test "
            "is skipped due to non-default core pattern\n");
    return;
  }

#ifndef si_syscall
  fprintf(stderr, "LinuxCoreDumperTest.VerifyDumpWithMultipleThreads test is "
          "skipped due to old kernel/C library headers\n");
  return;
#endif

  const unsigned kNumOfThreads = 2;
  const unsigned kCrashThread = 1;
  const int kCrashSignal = SIGSYS;
  pid_t child_pid;
  ASSERT_TRUE(crash_generator.CreateChildCrash(kNumOfThreads, kCrashThread,
                                               kCrashSignal, &child_pid));

  const string core_file = crash_generator.GetCoreFilePath();
  const string procfs_path = crash_generator.GetDirectoryOfProcFilesCopy();

#if defined(__ANDROID__)
  struct stat st;
  if (stat(core_file.c_str(), &st) != 0) {
    fprintf(stderr, "LinuxCoreDumperTest.VerifyExceptionDetails test is "
            "skipped due to no core file being generated\n");
    return;
  }
#endif

  LinuxCoreDumper dumper(child_pid, core_file.c_str(), procfs_path.c_str());

  EXPECT_TRUE(dumper.Init());

  EXPECT_TRUE(dumper.IsPostMortem());

#if defined(__ANDROID__)
  // TODO: For some reason, Android doesn't seem to pass this.
  if (!dumper.crash_address()) {
    fprintf(stderr, "LinuxCoreDumperTest.VerifyExceptionDetails test is "
            "skipped due to missing signal details on Android\n");
    return;
  }
#endif

  // Check the exception details.
  EXPECT_NE(0U, dumper.crash_address());
  EXPECT_EQ(kCrashSignal, dumper.crash_signal());
  EXPECT_EQ(crash_generator.GetThreadId(kCrashThread),
            dumper.crash_thread());

  // We check the length, but not the actual fields.  We sent SIGSYS ourselves
  // instead of the kernel, so the extended fields are garbage.
  const std::vector<uint64_t> info(dumper.crash_exception_info());
  EXPECT_EQ(2U, info.size());
}

TEST_F(LinuxCoreDumperTest, EnumerateMappings) {
  const char proc_maps_content[] =
      "00000000-00000001 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000002-00000004 r-xp 00000000 00:00 0    /app/libfoo.so\n"
      "00000004-00000005 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000005-00000006 rw-p 00000000 00:00 0    /app/libfoo.so\n"
      "00000006-00000007 rw-p 00000000 00:00 0    [anno]\n";

  string test_file = temp_dir.path() + "/maps";
  ASSERT_TRUE(WriteFile(test_file.c_str(), proc_maps_content,
                        sizeof(proc_maps_content)));

  LinuxCoreDumper dumper(0, "core_file", temp_dir.path().c_str());

  EXPECT_TRUE(dumper.EnumerateMappings());
  // no merge due to the address is not continuous
  EXPECT_EQ(4U, dumper.mappings().size());
  EXPECT_FALSE(dumper.mappings()[0]->exec);
  EXPECT_EQ(1U, dumper.mappings()[0]->size);
}

TEST_F(LinuxCoreDumperTest, EnumerateMappings_diffname) {
  const char proc_maps_content[] =
      "00000000-00000001 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000001-00000002 r-xp 00000000 00:00 0    /app/libbar.so\n"
      "00000002-00000003 r--p 00000000 00:00 0    /app/libbar.so\n"
      "00000003-00000004 rw-p 00000000 00:00 0    /app/libbar.so\n"
      "00000004-00000005 rw-p 00000000 00:00 0    [anno]\n";

  string test_file = temp_dir.path() + "/maps";
  ASSERT_TRUE(WriteFile(test_file.c_str(), proc_maps_content,
                        sizeof(proc_maps_content)));

  LinuxCoreDumper dumper(0, "core_file", temp_dir.path().c_str());

  EXPECT_TRUE(dumper.EnumerateMappings());
  // no merge due to the lib names are different
  EXPECT_EQ(4U, dumper.mappings().size());
  EXPECT_FALSE(dumper.mappings()[0]->exec);
  EXPECT_EQ(1U, dumper.mappings()[0]->size);
  EXPECT_STREQ("/app/libfoo.so", dumper.mappings()[0]->name);
  EXPECT_STREQ("/app/libbar.so", dumper.mappings()[1]->name);
}

TEST_F(LinuxCoreDumperTest, EnumerateMappings_merge) {
  const char proc_maps_content[] =
      "00000000-00000001 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000001-00000002 r-xp 00000000 00:00 0    /app/libfoo.so\n"
      "00000002-00000003 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000003-00000004 rw-p 00000000 00:00 0    /app/libfoo.so\n"
      "00000004-00000005 rw-p 00000000 00:00 0    [anno]\n";

  string test_file = temp_dir.path() + "/maps";
  ASSERT_TRUE(WriteFile(test_file.c_str(), proc_maps_content,
                        sizeof(proc_maps_content)));

  LinuxCoreDumper dumper(0, "core_file", temp_dir.path().c_str());

  EXPECT_TRUE(dumper.EnumerateMappings());
  EXPECT_EQ(3U, dumper.mappings().size());
  EXPECT_TRUE(dumper.mappings()[0]->exec);
  // merged #1 and #2 in proc_maps_content
  EXPECT_EQ(2U, dumper.mappings()[0]->size);
}

TEST_F(LinuxCoreDumperTest, EnumerateMappings_16K_padding) {
  const char proc_maps_content[] =
      "00000000-00000001 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000001-00000002 ---p 00000000 00:00 0    \n"
      "00000002-00000003 r-xp 00000000 00:00 0    /app/libfoo.so\n"
      "00000003-00000004 ---p 00000000 00:00 0    \n"
      "00000004-00000005 r--p 00000000 00:00 0    /app/libfoo.so\n"
      "00000005-00000006 ---p 00000000 00:00 0    \n"
      "00000006-00000007 rw-p 00000000 00:00 0    /app/libfoo.so\n"
      "00000007-00000008 rw-p 00000000 00:00 0    [anno]\n";

  string test_file = temp_dir.path() + "/maps";
  ASSERT_TRUE(WriteFile(test_file.c_str(), proc_maps_content,
                        sizeof(proc_maps_content)));

  LinuxCoreDumper dumper(0, "core_file", temp_dir.path().c_str());

  EXPECT_TRUE(dumper.EnumerateMappings());
  EXPECT_EQ(3U, dumper.mappings().size());

  EXPECT_STREQ("/app/libfoo.so", dumper.mappings()[0]->name);
  EXPECT_TRUE(dumper.mappings()[0]->exec);
  EXPECT_FALSE(dumper.mappings()[2]->exec);
  // merged #[1-4] in proc_maps_content
  EXPECT_EQ(4U, dumper.mappings()[0]->size);
}

}  // namespace google_breakpad

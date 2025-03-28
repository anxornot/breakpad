// Copyright 2010 Google LLC
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
//
// fast_source_line_resolver_unittest.cc: Unit tests for FastSourceLineResolver.
// Two different approaches for testing fast source line resolver:
// First, use the same unit test data for basic source line resolver.
// Second, read data from symbol files, load them as basic modules, and then
// serialize them and load the serialized data as fast modules.  Then compare
// modules to assure the fast module contains exactly the same data as
// basic module.
//
// Author: Siyang Xie (lambxsy@google.com)

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <assert.h>
#include <stdio.h>

#include <memory>
#include <sstream>
#include <string>

#include "breakpad_googletest_includes.h"
#include "common/using_std_string.h"
#include "google_breakpad/processor/code_module.h"
#include "google_breakpad/processor/stack_frame.h"
#include "google_breakpad/processor/memory_region.h"
#include "processor/logging.h"
#include "processor/module_serializer.h"
#include "processor/module_comparer.h"

namespace {

using google_breakpad::SourceLineResolverBase;
using google_breakpad::BasicSourceLineResolver;
using google_breakpad::FastSourceLineResolver;
using google_breakpad::ModuleSerializer;
using google_breakpad::ModuleComparer;
using google_breakpad::CFIFrameInfo;
using google_breakpad::CodeModule;
using google_breakpad::MemoryRegion;
using google_breakpad::StackFrame;
using google_breakpad::WindowsFrameInfo;

class TestCodeModule : public CodeModule {
 public:
  explicit TestCodeModule(const string& code_file) : code_file_(code_file) {}
  virtual ~TestCodeModule() {}

  virtual uint64_t base_address() const { return 0; }
  virtual uint64_t size() const { return 0xb000; }
  virtual string code_file() const { return code_file_; }
  virtual string code_identifier() const { return ""; }
  virtual string debug_file() const { return ""; }
  virtual string debug_identifier() const { return ""; }
  virtual string version() const { return ""; }
  virtual CodeModule* Copy() const {
    return new TestCodeModule(code_file_);
  }
  virtual bool is_unloaded() const { return false; }
  virtual uint64_t shrink_down_delta() const { return 0; }
  virtual void SetShrinkDownDelta(uint64_t shrink_down_delta) {}

 private:
  string code_file_;
};

// A mock memory region object, for use by the STACK CFI tests.
class MockMemoryRegion: public MemoryRegion {
  uint64_t GetBase() const { return 0x10000; }
  uint32_t GetSize() const { return 0x01000; }
  bool GetMemoryAtAddress(uint64_t address, uint8_t* value) const {
    *value = address & 0xff;
    return true;
  }
  bool GetMemoryAtAddress(uint64_t address, uint16_t* value) const {
    *value = address & 0xffff;
    return true;
  }
  bool GetMemoryAtAddress(uint64_t address, uint32_t* value) const {
    switch (address) {
      case 0x10008: *value = 0x98ecadc3; break;  // saved %ebx
      case 0x1000c: *value = 0x878f7524; break;  // saved %esi
      case 0x10010: *value = 0x6312f9a5; break;  // saved %edi
      case 0x10014: *value = 0x10038;    break;  // caller's %ebp
      case 0x10018: *value = 0xf6438648; break;  // return address
      default: *value = 0xdeadbeef;      break;  // junk
    }
    return true;
  }
  bool GetMemoryAtAddress(uint64_t address, uint64_t* value) const {
    *value = address;
    return true;
  }
  void Print() const {
    assert(false);
  }
};

// Verify that, for every association in ACTUAL, EXPECTED has the same
// association. (That is, ACTUAL's associations should be a subset of
// EXPECTED's.) Also verify that ACTUAL has associations for ".ra" and
// ".cfa".
static bool VerifyRegisters(
    const char* file, int line,
    const CFIFrameInfo::RegisterValueMap<uint32_t>& expected,
    const CFIFrameInfo::RegisterValueMap<uint32_t>& actual) {
  CFIFrameInfo::RegisterValueMap<uint32_t>::const_iterator a;
  a = actual.find(".cfa");
  if (a == actual.end())
    return false;
  a = actual.find(".ra");
  if (a == actual.end())
    return false;
  for (a = actual.begin(); a != actual.end(); a++) {
    CFIFrameInfo::RegisterValueMap<uint32_t>::const_iterator e =
      expected.find(a->first);
    if (e == expected.end()) {
      fprintf(stderr, "%s:%d: unexpected register '%s' recovered, value 0x%x\n",
              file, line, a->first.c_str(), a->second);
      return false;
    }
    if (e->second != a->second) {
      fprintf(stderr,
              "%s:%d: register '%s' recovered value was 0x%x, expected 0x%x\n",
              file, line, a->first.c_str(), a->second, e->second);
      return false;
    }
    // Don't complain if this doesn't recover all registers. Although
    // the DWARF spec says that unmentioned registers are undefined,
    // GCC uses omission to mean that they are unchanged.
  }
  return true;
}

static bool VerifyEmpty(const StackFrame& frame) {
  if (frame.function_name.empty() &&
      frame.source_file_name.empty() &&
      frame.source_line == 0)
    return true;
  return false;
}

static void ClearSourceLineInfo(StackFrame* frame) {
  frame->function_name.clear();
  frame->module = nullptr;
  frame->source_file_name.clear();
  frame->source_line = 0;
}

class TestFastSourceLineResolver : public ::testing::Test {
 public:
  void SetUp() {
    testdata_dir = string(getenv("srcdir") ? getenv("srcdir") : ".") +
                         "/src/processor/testdata";
  }

  string symbol_file(int file_index) {
    std::stringstream ss;
    ss << testdata_dir << "/module" << file_index << ".out";
    return ss.str();
  }

  ModuleSerializer serializer;
  BasicSourceLineResolver basic_resolver;
  FastSourceLineResolver fast_resolver;
  ModuleComparer module_comparer;

  string testdata_dir;
};

// Test adapted from basic_source_line_resolver_unittest.
TEST_F(TestFastSourceLineResolver, TestLoadAndResolve) {
  TestCodeModule module1("module1");
  ASSERT_TRUE(basic_resolver.LoadModule(&module1, symbol_file(1)));
  ASSERT_TRUE(basic_resolver.HasModule(&module1));
  // Convert module1 to fast_module:
  ASSERT_TRUE(serializer.ConvertOneModule(
      module1.code_file(), &basic_resolver, &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module1));

  TestCodeModule module2("module2");
  ASSERT_TRUE(basic_resolver.LoadModule(&module2, symbol_file(2)));
  ASSERT_TRUE(basic_resolver.HasModule(&module2));
  // Convert module2 to fast_module:
  ASSERT_TRUE(serializer.ConvertOneModule(
      module2.code_file(), &basic_resolver, &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module2));

  StackFrame frame;
  std::unique_ptr<WindowsFrameInfo> windows_frame_info;
  std::unique_ptr<CFIFrameInfo> cfi_frame_info;
  frame.instruction = 0x1000;
  frame.module = nullptr;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_FALSE(frame.module);
  ASSERT_TRUE(frame.function_name.empty());
  ASSERT_EQ(frame.function_base, 0U);
  ASSERT_TRUE(frame.source_file_name.empty());
  ASSERT_EQ(frame.source_line, 0);
  ASSERT_EQ(frame.source_line_base, 0U);
  ASSERT_EQ(frame.is_multiple, false);

  frame.module = &module1;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, "Function1_1");
  ASSERT_TRUE(frame.module);
  ASSERT_EQ(frame.module->code_file(), "module1");
  ASSERT_EQ(frame.function_base, 0x1000U);
  ASSERT_EQ(frame.source_file_name, "file1_1.cc");
  ASSERT_EQ(frame.source_line, 44);
  ASSERT_EQ(frame.source_line_base, 0x1000U);
  ASSERT_EQ(frame.is_multiple, true);
  windows_frame_info.reset(fast_resolver.FindWindowsFrameInfo(&frame));
  ASSERT_TRUE(windows_frame_info.get());
  ASSERT_FALSE(windows_frame_info->allocates_base_pointer);
  ASSERT_EQ(windows_frame_info->program_string,
            "$eip 4 + ^ = $esp $ebp 8 + = $ebp $ebp ^ =");

  ClearSourceLineInfo(&frame);
  frame.instruction = 0x800;
  frame.module = &module1;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_TRUE(VerifyEmpty(frame));
  windows_frame_info.reset(fast_resolver.FindWindowsFrameInfo(&frame));
  ASSERT_FALSE(windows_frame_info.get());

  frame.instruction = 0x1280;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, "Function1_3");
  ASSERT_TRUE(frame.source_file_name.empty());
  ASSERT_EQ(frame.source_line, 0);
  windows_frame_info.reset(fast_resolver.FindWindowsFrameInfo(&frame));
  ASSERT_TRUE(windows_frame_info.get());
  ASSERT_EQ(windows_frame_info->type_, WindowsFrameInfo::STACK_INFO_UNKNOWN);
  ASSERT_FALSE(windows_frame_info->allocates_base_pointer);
  ASSERT_TRUE(windows_frame_info->program_string.empty());

  frame.instruction = 0x1380;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, "Function1_4");
  ASSERT_TRUE(frame.source_file_name.empty());
  ASSERT_EQ(frame.source_line, 0);
  windows_frame_info.reset(fast_resolver.FindWindowsFrameInfo(&frame));
  ASSERT_TRUE(windows_frame_info.get());
  ASSERT_EQ(windows_frame_info->type_, WindowsFrameInfo::STACK_INFO_FRAME_DATA);
  ASSERT_FALSE(windows_frame_info->allocates_base_pointer);
  ASSERT_FALSE(windows_frame_info->program_string.empty());

  frame.instruction = 0x2000;
  windows_frame_info.reset(fast_resolver.FindWindowsFrameInfo(&frame));
  ASSERT_FALSE(windows_frame_info.get());

  // module1 has STACK CFI records covering 3d40..3def;
  // module2 has STACK CFI records covering 3df0..3e9f;
  // check that FindCFIFrameInfo doesn't claim to find any outside those ranges.
  frame.instruction = 0x3d3f;
  frame.module = &module1;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_FALSE(cfi_frame_info.get());

  frame.instruction = 0x3e9f;
  frame.module = &module1;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_FALSE(cfi_frame_info.get());

  CFIFrameInfo::RegisterValueMap<uint32_t> current_registers;
  CFIFrameInfo::RegisterValueMap<uint32_t> caller_registers;
  CFIFrameInfo::RegisterValueMap<uint32_t> expected_caller_registers;
  MockMemoryRegion memory;

  // Regardless of which instruction evaluation takes place at, it
  // should produce the same values for the caller's registers.
  expected_caller_registers[".cfa"] = 0x1001c;
  expected_caller_registers[".ra"]  = 0xf6438648;
  expected_caller_registers["$ebp"] = 0x10038;
  expected_caller_registers["$ebx"] = 0x98ecadc3;
  expected_caller_registers["$esi"] = 0x878f7524;
  expected_caller_registers["$edi"] = 0x6312f9a5;

  frame.instruction = 0x3d40;
  frame.module = &module1;
  current_registers.clear();
  current_registers["$esp"] = 0x10018;
  current_registers["$ebp"] = 0x10038;
  current_registers["$ebx"] = 0x98ecadc3;
  current_registers["$esi"] = 0x878f7524;
  current_registers["$edi"] = 0x6312f9a5;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_TRUE(cfi_frame_info.get());
  ASSERT_TRUE(cfi_frame_info.get()
              ->FindCallerRegs<uint32_t>(current_registers, memory,
                                          &caller_registers));
  ASSERT_TRUE(VerifyRegisters(__FILE__, __LINE__,
                              expected_caller_registers, caller_registers));

  frame.instruction = 0x3d41;
  current_registers["$esp"] = 0x10014;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_TRUE(cfi_frame_info.get());
  ASSERT_TRUE(cfi_frame_info.get()
              ->FindCallerRegs<uint32_t>(current_registers, memory,
                                          &caller_registers));
  ASSERT_TRUE(VerifyRegisters(__FILE__, __LINE__,
                              expected_caller_registers, caller_registers));

  frame.instruction = 0x3d43;
  current_registers["$ebp"] = 0x10014;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_TRUE(cfi_frame_info.get());
  ASSERT_TRUE(cfi_frame_info.get()
              ->FindCallerRegs<uint32_t>(current_registers, memory,
                                          &caller_registers));
  VerifyRegisters(__FILE__, __LINE__,
                  expected_caller_registers, caller_registers);

  frame.instruction = 0x3d54;
  current_registers["$ebx"] = 0x6864f054U;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_TRUE(cfi_frame_info.get());
  ASSERT_TRUE(cfi_frame_info.get()
              ->FindCallerRegs<uint32_t>(current_registers, memory,
                                          &caller_registers));
  VerifyRegisters(__FILE__, __LINE__,
                  expected_caller_registers, caller_registers);

  frame.instruction = 0x3d5a;
  current_registers["$esi"] = 0x6285f79aU;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_TRUE(cfi_frame_info.get());
  ASSERT_TRUE(cfi_frame_info.get()
              ->FindCallerRegs<uint32_t>(current_registers, memory,
                                          &caller_registers));
  VerifyRegisters(__FILE__, __LINE__,
                  expected_caller_registers, caller_registers);

  frame.instruction = 0x3d84;
  current_registers["$edi"] = 0x64061449U;
  cfi_frame_info.reset(fast_resolver.FindCFIFrameInfo(&frame));
  ASSERT_TRUE(cfi_frame_info.get());
  ASSERT_TRUE(cfi_frame_info.get()
              ->FindCallerRegs<uint32_t>(current_registers, memory,
                                          &caller_registers));
  VerifyRegisters(__FILE__, __LINE__,
                  expected_caller_registers, caller_registers);

  frame.instruction = 0x2900;
  frame.module = &module1;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, string("PublicSymbol"));
  EXPECT_EQ(frame.is_multiple, true);

  frame.instruction = 0x4000;
  frame.module = &module1;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, string("LargeFunction"));

  frame.instruction = 0x2181;
  frame.module = &module2;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, "Function2_2");
  ASSERT_EQ(frame.function_base, 0x2170U);
  ASSERT_TRUE(frame.module);
  ASSERT_EQ(frame.module->code_file(), "module2");
  ASSERT_EQ(frame.source_file_name, "file2_2.cc");
  ASSERT_EQ(frame.source_line, 21);
  ASSERT_EQ(frame.source_line_base, 0x2180U);
  ASSERT_EQ(frame.is_multiple, false);
  windows_frame_info.reset(fast_resolver.FindWindowsFrameInfo(&frame));
  ASSERT_TRUE(windows_frame_info.get());
  ASSERT_EQ(windows_frame_info->type_, WindowsFrameInfo::STACK_INFO_FRAME_DATA);
  ASSERT_EQ(windows_frame_info->prolog_size, 1U);

  frame.instruction = 0x216f;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, "Public2_1");
  EXPECT_EQ(frame.is_multiple, false);

  ClearSourceLineInfo(&frame);
  frame.instruction = 0x219f;
  frame.module = &module2;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_TRUE(frame.function_name.empty());

  frame.instruction = 0x21a0;
  frame.module = &module2;
  fast_resolver.FillSourceLineInfo(&frame, nullptr);
  ASSERT_EQ(frame.function_name, "Public2_2");
}

// Test adapted from basic_source_line_resolver_unittest.
TEST_F(TestFastSourceLineResolver, TestLoadAndResolveOldInlines) {
  TestCodeModule module("linux_inline");
  ASSERT_TRUE(basic_resolver.LoadModule(
      &module, testdata_dir +
                   "/symbols/linux_inline/BBA6FA10B8AAB33D00000000000000000/"
                   "linux_inline.old.sym"));
  ASSERT_TRUE(basic_resolver.HasModule(&module));
  // Convert module1 to fast_module:
  ASSERT_TRUE(serializer.ConvertOneModule(module.code_file(), &basic_resolver,
                                          &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module));

  StackFrame frame;
  std::deque<std::unique_ptr<StackFrame>> inlined_frames;
  frame.instruction = 0x161b6;
  frame.module = &module;
  fast_resolver.FillSourceLineInfo(&frame, &inlined_frames);

  // main frame.
  ASSERT_EQ(frame.function_name, "main");
  ASSERT_EQ(frame.function_base, 0x15b30U);
  ASSERT_EQ(frame.source_file_name, "linux_inline.cpp");
  ASSERT_EQ(frame.source_line, 42);
  ASSERT_EQ(frame.source_line_base, 0x161b6U);
  ASSERT_EQ(frame.is_multiple, false);

  ASSERT_EQ(inlined_frames.size(), 3UL);

  // Inlined frames inside main frame.
  ASSERT_EQ(inlined_frames[2]->function_name, "foo()");
  ASSERT_EQ(inlined_frames[2]->function_base, 0x15b45U);
  ASSERT_EQ(inlined_frames[2]->source_file_name, "linux_inline.cpp");
  ASSERT_EQ(inlined_frames[2]->source_line, 39);
  ASSERT_EQ(inlined_frames[2]->source_line_base, 0x161b6U);
  ASSERT_EQ(inlined_frames[2]->trust, StackFrame::FRAME_TRUST_INLINE);

  ASSERT_EQ(inlined_frames[1]->function_name, "bar()");
  ASSERT_EQ(inlined_frames[1]->function_base, 0x15b72U);
  ASSERT_EQ(inlined_frames[1]->source_file_name, "linux_inline.cpp");
  ASSERT_EQ(inlined_frames[1]->source_line, 32);
  ASSERT_EQ(inlined_frames[1]->source_line_base, 0x161b6U);
  ASSERT_EQ(inlined_frames[1]->trust, StackFrame::FRAME_TRUST_INLINE);

  ASSERT_EQ(inlined_frames[0]->function_name, "func()");
  ASSERT_EQ(inlined_frames[0]->function_base, 0x15b83U);
  ASSERT_EQ(inlined_frames[0]->source_file_name, "linux_inline.cpp");
  ASSERT_EQ(inlined_frames[0]->source_line, 27);
  ASSERT_EQ(inlined_frames[0]->source_line_base, 0x161b6U);
  ASSERT_EQ(inlined_frames[0]->trust, StackFrame::FRAME_TRUST_INLINE);
}

// Test adapted from basic_source_line_resolver_unittest.
TEST_F(TestFastSourceLineResolver, TestLoadAndResolveNewInlines) {
  TestCodeModule module("linux_inline");
  ASSERT_TRUE(basic_resolver.LoadModule(
      &module, testdata_dir +
                   "/symbols/linux_inline/BBA6FA10B8AAB33D00000000000000000/"
                   "linux_inline.new.sym"));
  ASSERT_TRUE(basic_resolver.HasModule(&module));
  // Convert module1 to fast_module:
  ASSERT_TRUE(serializer.ConvertOneModule(module.code_file(), &basic_resolver,
                                          &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module));

  StackFrame frame;
  std::deque<std::unique_ptr<StackFrame>> inlined_frames;
  frame.instruction = 0x161b6;
  frame.module = &module;
  fast_resolver.FillSourceLineInfo(&frame, &inlined_frames);

  // main frame.
  ASSERT_EQ(frame.function_name, "main");
  ASSERT_EQ(frame.function_base, 0x15b30U);
  ASSERT_EQ(frame.source_file_name, "a.cpp");
  ASSERT_EQ(frame.source_line, 42);
  ASSERT_EQ(frame.source_line_base, 0x161b6U);
  ASSERT_EQ(frame.is_multiple, false);

  ASSERT_EQ(inlined_frames.size(), 3UL);

  // Inlined frames inside main frame.
  ASSERT_EQ(inlined_frames[2]->function_name, "foo()");
  ASSERT_EQ(inlined_frames[2]->function_base, 0x15b45U);
  ASSERT_EQ(inlined_frames[2]->source_file_name, "b.cpp");
  ASSERT_EQ(inlined_frames[2]->source_line, 39);
  ASSERT_EQ(inlined_frames[2]->source_line_base, 0x161b6U);
  ASSERT_EQ(inlined_frames[2]->trust, StackFrame::FRAME_TRUST_INLINE);

  ASSERT_EQ(inlined_frames[1]->function_name, "bar()");
  ASSERT_EQ(inlined_frames[1]->function_base, 0x15b72U);
  ASSERT_EQ(inlined_frames[1]->source_file_name, "c.cpp");
  ASSERT_EQ(inlined_frames[1]->source_line, 32);
  ASSERT_EQ(inlined_frames[1]->source_line_base, 0x161b6U);
  ASSERT_EQ(inlined_frames[1]->trust, StackFrame::FRAME_TRUST_INLINE);

  ASSERT_EQ(inlined_frames[0]->function_name, "func()");
  ASSERT_EQ(inlined_frames[0]->function_base, 0x15b83U);
  ASSERT_EQ(inlined_frames[0]->source_file_name, "linux_inline.cpp");
  ASSERT_EQ(inlined_frames[0]->source_line, 27);
  ASSERT_EQ(inlined_frames[0]->source_line_base, 0x161b6U);
  ASSERT_EQ(inlined_frames[0]->trust, StackFrame::FRAME_TRUST_INLINE);
}

TEST_F(TestFastSourceLineResolver, TestInvalidLoads) {
  TestCodeModule module3("module3");
  ASSERT_TRUE(basic_resolver.LoadModule(&module3,
                                        testdata_dir + "/module3_bad.out"));
  ASSERT_TRUE(basic_resolver.HasModule(&module3));
  ASSERT_TRUE(basic_resolver.IsModuleCorrupt(&module3));
  // Convert module3 to fast_module:
  ASSERT_TRUE(serializer.ConvertOneModule(module3.code_file(),
                                          &basic_resolver,
                                          &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module3));
  ASSERT_TRUE(fast_resolver.IsModuleCorrupt(&module3));

  TestCodeModule module4("module4");
  ASSERT_TRUE(basic_resolver.LoadModule(&module4,
                                        testdata_dir + "/module4_bad.out"));
  ASSERT_TRUE(basic_resolver.HasModule(&module4));
  ASSERT_TRUE(basic_resolver.IsModuleCorrupt(&module4));
  // Convert module4 to fast_module:
  ASSERT_TRUE(serializer.ConvertOneModule(module4.code_file(),
                                          &basic_resolver,
                                          &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module4));
  ASSERT_TRUE(fast_resolver.IsModuleCorrupt(&module4));

  TestCodeModule module5("module5");
  ASSERT_FALSE(fast_resolver.LoadModule(&module5,
                                        testdata_dir + "/invalid-filename"));
  ASSERT_FALSE(fast_resolver.HasModule(&module5));

  TestCodeModule invalidmodule("invalid-module");
  ASSERT_FALSE(fast_resolver.HasModule(&invalidmodule));
}

TEST_F(TestFastSourceLineResolver, TestUnload) {
  TestCodeModule module1("module1");
  ASSERT_FALSE(basic_resolver.HasModule(&module1));

  ASSERT_TRUE(basic_resolver.LoadModule(&module1, symbol_file(1)));
  ASSERT_TRUE(basic_resolver.HasModule(&module1));
  // Convert module1 to fast_module.
  ASSERT_TRUE(serializer.ConvertOneModule(module1.code_file(),
                                          &basic_resolver,
                                          &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module1));
  basic_resolver.UnloadModule(&module1);
  fast_resolver.UnloadModule(&module1);
  ASSERT_FALSE(fast_resolver.HasModule(&module1));

  ASSERT_TRUE(basic_resolver.LoadModule(&module1, symbol_file(1)));
  ASSERT_TRUE(basic_resolver.HasModule(&module1));
  // Convert module1 to fast_module.
  ASSERT_TRUE(serializer.ConvertOneModule(module1.code_file(),
                                          &basic_resolver,
                                          &fast_resolver));
  ASSERT_TRUE(fast_resolver.HasModule(&module1));
}

TEST_F(TestFastSourceLineResolver, CompareModule) {
  char* symbol_data;
  size_t symbol_data_size;
  string symbol_data_string;
  string filename;

  for (int module_index = 0; module_index < 3; ++module_index) {
    std::stringstream ss;
    ss << testdata_dir << "/module" << module_index << ".out";
    filename = ss.str();
    ASSERT_TRUE(SourceLineResolverBase::ReadSymbolFile(
        symbol_file(module_index), &symbol_data, &symbol_data_size));
    symbol_data_string.assign(symbol_data, symbol_data_size);
    delete [] symbol_data;
    ASSERT_TRUE(module_comparer.Compare(symbol_data_string));
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

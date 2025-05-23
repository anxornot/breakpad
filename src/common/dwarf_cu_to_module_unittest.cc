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

// Original author: Jim Blandy <jimb@mozilla.com> <jimb@red-bean.com>

// dwarf_cu_to_module.cc: Unit tests for google_breakpad::DwarfCUToModule.

#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

#include "breakpad_googletest_includes.h"
#include "common/dwarf_cu_to_module.h"
#include "common/using_std_string.h"

using std::make_pair;
using std::vector;

using google_breakpad::DIEHandler;
using google_breakpad::DwarfTag;
using google_breakpad::DwarfAttribute;
using google_breakpad::DwarfForm;
using google_breakpad::DwarfInline;
using google_breakpad::DwarfCUToModule;
using google_breakpad::Module;

using ::testing::_;
using ::testing::AtMost;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::Test;
using ::testing::TestWithParam;
using ::testing::Values;
using ::testing::ValuesIn;

// Mock classes.

class MockLineToModuleHandler: public DwarfCUToModule::LineToModuleHandler {
 public:
  MOCK_METHOD1(StartCompilationUnit, void(const string& compilation_dir));
  MOCK_METHOD9(ReadProgram, void(const uint8_t* program, uint64_t length,
                                 const uint8_t* string_section,
                                 uint64_t string_section_length,
                                 const uint8_t* line_string_section,
                                 uint64_t line_string_section_length,
                                 Module* module, vector<Module::Line>* lines,
                                 std::map<uint32_t, Module::File*>* files));
};

class MockWarningReporter: public DwarfCUToModule::WarningReporter {
 public:
  MockWarningReporter(const string& filename, uint64_t cu_offset)
      : DwarfCUToModule::WarningReporter(filename, cu_offset) { }
  MOCK_METHOD1(SetCUName, void(const string& name));
  MOCK_METHOD2(UnknownSpecification, void(uint64_t offset, uint64_t target));
  MOCK_METHOD2(UnknownAbstractOrigin, void(uint64_t offset, uint64_t target));
  MOCK_METHOD1(MissingSection, void(const string& section_name));
  MOCK_METHOD1(BadLineInfoOffset, void(uint64_t offset));
  MOCK_METHOD1(UncoveredFunction, void(const Module::Function& function));
  MOCK_METHOD1(UncoveredLine, void(const Module::Line& line));
  MOCK_METHOD1(UnnamedFunction, void(uint64_t offset));
  MOCK_METHOD1(DemangleError, void(const string& input));
  MOCK_METHOD2(UnhandledInterCUReference, void(uint64_t offset, uint64_t target));
};

// A fixture class including all the objects needed to handle a
// compilation unit, and their entourage. It includes member functions
// for doing common kinds of setup and tests.
class CUFixtureBase {
 public:
  // If we have:
  //
  //   vector<Module::Line> lines;
  //   AppendLinesFunctor appender(lines);
  //
  // then doing:
  //
  //   appender(line_program, length, module, line_vector);
  //
  // will append lines to the end of line_vector.  We can use this with
  // MockLineToModuleHandler like this:
  //
  //   MockLineToModuleHandler l2m;
  //   EXPECT_CALL(l2m, ReadProgram(_,_,_,_))
  //       .WillOnce(DoAll(Invoke(appender), Return()));
  //
  // in which case calling l2m with some line vector will append lines.
  class AppendLinesFunctor {
   public:
    explicit AppendLinesFunctor(
        const vector<Module::Line>* lines) : lines_(lines) { }
    void operator()(const uint8_t* program, uint64_t length,
                    const uint8_t* string_section,
                    uint64_t string_section_length,
                    const uint8_t* line_string_section,
                    uint64_t line_string_section_length,
                    Module *module, vector<Module::Line>* lines, 
                    std::map<uint32_t, Module::File*>* files) {
      lines->insert(lines->end(), lines_->begin(), lines_->end());
    }
   private:
    const vector<Module::Line>* lines_;
  };

  CUFixtureBase()
      : module_("module-name", "module-os", "module-arch", "module-id"),
        file_context_("dwarf-filename", &module_, true),
        language_(google_breakpad::DW_LANG_none),
        language_signed_(false),
        appender_(&lines_),
        reporter_("dwarf-filename", 0xcf8f9bb6443d29b5LL),
        root_handler_(&file_context_, &line_reader_,
                      /* ranges_reader */ nullptr, &reporter_),
        functions_filled_(false) {
    // By default, expect no warnings to be reported, and expect the
    // compilation unit's name to be provided. The test can override
    // these expectations.
    EXPECT_CALL(reporter_, SetCUName("compilation-unit-name")).Times(1);
    EXPECT_CALL(reporter_, UnknownSpecification(_, _)).Times(0);
    EXPECT_CALL(reporter_, UnknownAbstractOrigin(_, _)).Times(0);
    EXPECT_CALL(reporter_, MissingSection(_)).Times(0);
    EXPECT_CALL(reporter_, BadLineInfoOffset(_)).Times(0);
    EXPECT_CALL(reporter_, UncoveredFunction(_)).Times(0);
    EXPECT_CALL(reporter_, UncoveredLine(_)).Times(0);
    EXPECT_CALL(reporter_, UnnamedFunction(_)).Times(0);
    EXPECT_CALL(reporter_, UnhandledInterCUReference(_, _)).Times(0);

    // By default, expect the line program reader not to be invoked. We
    // may override this in StartCU.
    EXPECT_CALL(line_reader_, StartCompilationUnit(_)).Times(0);
    EXPECT_CALL(line_reader_, ReadProgram(_,_,_,_,_,_,_,_,_)).Times(0);

    // The handler will consult this section map to decide what to
    // pass to our line reader.
    file_context_.AddSectionToSectionMap(".debug_line",
                                         dummy_line_program_,
                                         dummy_line_size_);
  }

  // Add a line with the given address, size, filename, and line
  // number to the end of the statement list the handler will receive
  // when it invokes its LineToModuleHandler. Call this before calling
  // StartCU.
  void PushLine(Module::Address address, Module::Address size,
                const string& filename, int line_number);

  // Use LANGUAGE for the compilation unit. More precisely, arrange
  // for StartCU to pass the compilation unit's root DIE a
  // DW_AT_language attribute whose value is LANGUAGE.
  void SetLanguage(google_breakpad::DwarfLanguage language) {
    language_ = language;
  }

  // If SIGNED true, have StartCU report DW_AT_language as a signed
  // attribute; if false, have it report it as unsigned.
  void SetLanguageSigned(bool is_signed) { language_signed_ = is_signed; }

  // Call the handler this.root_handler_'s StartCompilationUnit and
  // StartRootDIE member functions, passing it appropriate attributes as
  // determined by prior calls to PushLine and SetLanguage. Leave
  // this.root_handler_ ready to hear about children: call
  // this.root_handler_.EndAttributes, but not this.root_handler_.Finish.
  void StartCU();

  // Have HANDLER process some strange attribute/form/value triples.
  void ProcessStrangeAttributes(google_breakpad::DIEHandler* handler);

  // Start a child DIE of PARENT with the given tag and name. Leave
  // the handler ready to hear about children: call EndAttributes, but
  // not Finish.
  DIEHandler* StartNamedDIE(DIEHandler* parent, DwarfTag tag,
                            const string& name);

  // Start a child DIE of PARENT with the given tag and a
  // DW_AT_specification attribute whose value is SPECIFICATION. Leave
  // the handler ready to hear about children: call EndAttributes, but
  // not Finish. If NAME is non-zero, use it as the DW_AT_name
  // attribute.
  DIEHandler* StartSpecifiedDIE(DIEHandler* parent, DwarfTag tag,
                                uint64_t specification,
                                const char* name = nullptr);

  // Define a function as a child of PARENT with the given name, address, and
  // size. If high_pc_form is DW_FORM_addr then the DW_AT_high_pc attribute
  // will be written as an address; otherwise it will be written as the
  // function's size. Call EndAttributes and Finish; one cannot define
  // children of the defined function's DIE.
  void DefineFunction(DIEHandler* parent, const string& name,
                      Module::Address address, Module::Address size,
                      const char* mangled_name,
                      DwarfForm high_pc_form = google_breakpad::DW_FORM_addr);

  // Create a declaration DIE as a child of PARENT with the given
  // offset, tag and name. If NAME is the empty string, don't provide
  // a DW_AT_name attribute. Call EndAttributes and Finish.
  void DeclarationDIE(DIEHandler* parent, uint64_t offset,
                      DwarfTag tag, const string& name,
                      const string& mangled_name);

  // Create a definition DIE as a child of PARENT with the given tag
  // that refers to the declaration DIE at offset SPECIFICATION as its
  // specification. If NAME is non-empty, pass it as the DW_AT_name
  // attribute. If SIZE is non-zero, record ADDRESS and SIZE as
  // low_pc/high_pc attributes.
  void DefinitionDIE(DIEHandler* parent, DwarfTag tag,
                     uint64_t specification, const string& name,
                     Module::Address address = 0, Module::Address size = 0);

  // Create an inline DW_TAG_subprogram DIE as a child of PARENT.  If
  // SPECIFICATION is non-zero, then the DIE refers to the declaration DIE at
  // offset SPECIFICATION as its specification.  If Name is non-empty, pass it
  // as the DW_AT_name attribute.
  void AbstractInstanceDIE(DIEHandler* parent, uint64_t offset,
                           DwarfInline type, uint64_t specification,
                           const string& name,
                           DwarfForm form = google_breakpad::DW_FORM_data1);

  // Create a DW_TAG_subprogram DIE as a child of PARENT that refers to
  // ORIGIN in its DW_AT_abstract_origin attribute.  If NAME is the empty
  // string, don't provide a DW_AT_name attribute.
  void DefineInlineInstanceDIE(DIEHandler* parent, const string& name,
                               uint64_t origin, Module::Address address,
                               Module::Address size);

  // The following Test* functions should be called after calling
  // this.root_handler_.Finish. After that point, no further calls
  // should be made on the handler.

  // Test that the number of functions defined in the module this.module_ is
  // equal to EXPECTED.
  void TestFunctionCount(size_t expected);

  // Test that the I'th function (ordered by address) in the module
  // this.module_ has the given name, address, and size, and that its
  // parameter size is zero.
  void TestFunction(int i, const string& name,
                    Module::Address address, Module::Address size);

  // Test that the I'th function (ordered by address) in the module
  // this.module_ has the given prefer_extern_name.
  void TestFunctionPreferExternName(int i, bool prefer_extern_name);

  // Test that the number of source lines owned by the I'th function
  // in the module this.module_ is equal to EXPECTED.
  void TestLineCount(int i, size_t expected);

  // Test that the J'th line (ordered by address) of the I'th function
  // (again, by address) has the given address, size, filename, and
  // line number.
  void TestLine(int i, int j, Module::Address address, Module::Address size,
                const string& filename, int number);

  // Actual objects under test.
  Module module_;
  DwarfCUToModule::FileContext file_context_;

  // If this is not DW_LANG_none, we'll pass it as a DW_AT_language
  // attribute to the compilation unit. This defaults to DW_LANG_none.
  google_breakpad::DwarfLanguage language_;

  // If this is true, report DW_AT_language as a signed value; if false,
  // report it as an unsigned value.
  bool language_signed_;

  // If this is not empty, we'll give the CU a DW_AT_comp_dir attribute that
  // indicates the path that this compilation unit was compiled in.
  string compilation_dir_;

  // If this is not empty, we'll give the CU a DW_AT_stmt_list
  // attribute that, when passed to line_reader_, adds these lines to the
  // provided lines array.
  vector<Module::Line> lines_;

  // Mock line program reader.
  MockLineToModuleHandler line_reader_;
  AppendLinesFunctor appender_;
  static const uint8_t dummy_line_program_[];
  static const size_t dummy_line_size_;

  MockWarningReporter reporter_;
  DwarfCUToModule root_handler_;

 private:
  // Fill functions_, if we haven't already.
  void FillFunctions();

  // If functions_filled_ is true, this is a table of functions we've
  // extracted from module_, sorted by address.
  vector<Module::Function*> functions_;
  // True if we have filled the above vector with this.module_'s function list.
  bool functions_filled_;
};

const uint8_t CUFixtureBase::dummy_line_program_[] = "lots of fun data";
const size_t CUFixtureBase::dummy_line_size_ =
    sizeof(CUFixtureBase::dummy_line_program_);

void CUFixtureBase::PushLine(Module::Address address, Module::Address size,
                             const string& filename, int line_number) {
  Module::Line l;
  l.address = address;
  l.size = size;
  l.file = module_.FindFile(filename);
  l.number = line_number;
  lines_.push_back(l);
}

void CUFixtureBase::StartCU() {
  if (!compilation_dir_.empty())
    EXPECT_CALL(line_reader_,
                StartCompilationUnit(compilation_dir_)).Times(1);

  // If we have lines, make the line reader expect to be invoked at
  // most once. (Hey, if the handler can pass its tests without
  // bothering to read the line number data, that's great.)
  // Have it add the lines passed to PushLine. Otherwise, leave the
  // initial expectation (no calls) in force.
  if (!lines_.empty())
    EXPECT_CALL(line_reader_,
                ReadProgram(&dummy_line_program_[0], dummy_line_size_,
                            _,_,_,_,
                            &module_, _,_))
        .Times(AtMost(1))
        .WillOnce(DoAll(Invoke(appender_), Return()));
  ASSERT_TRUE(root_handler_
              .StartCompilationUnit(0x51182ec307610b51ULL, 0x81, 0x44,
                                    0x4241b4f33720dd5cULL, 3));
  {
    ASSERT_TRUE(root_handler_.StartRootDIE(0x02e56bfbda9e7337ULL,
                                           google_breakpad::DW_TAG_compile_unit));
  }
  root_handler_.ProcessAttributeString(google_breakpad::DW_AT_name,
                                       google_breakpad::DW_FORM_strp,
                                       "compilation-unit-name");
  if (!compilation_dir_.empty())
    root_handler_.ProcessAttributeString(google_breakpad::DW_AT_comp_dir,
                                         google_breakpad::DW_FORM_strp,
                                         compilation_dir_);
  if (!lines_.empty())
    root_handler_.ProcessAttributeUnsigned(google_breakpad::DW_AT_stmt_list,
                                           google_breakpad::DW_FORM_ref4,
                                           0);
  if (language_ != google_breakpad::DW_LANG_none) {
    if (language_signed_)
      root_handler_.ProcessAttributeSigned(google_breakpad::DW_AT_language,
                                           google_breakpad::DW_FORM_sdata,
                                           language_);
    else
      root_handler_.ProcessAttributeUnsigned(google_breakpad::DW_AT_language,
                                             google_breakpad::DW_FORM_udata,
                                             language_);
  }
  ASSERT_TRUE(root_handler_.EndAttributes());
}

void CUFixtureBase::ProcessStrangeAttributes(
    google_breakpad::DIEHandler* handler) {
  handler->ProcessAttributeUnsigned((DwarfAttribute) 0xf560dead,
                                    (DwarfForm) 0x4106e4db,
                                    0xa592571997facda1ULL);
  handler->ProcessAttributeSigned((DwarfAttribute) 0x85380095,
                                  (DwarfForm) 0x0f16fe87,
                                  0x12602a4e3bf1f446LL);
  handler->ProcessAttributeReference((DwarfAttribute) 0xf7f7480f,
                                     (DwarfForm) 0x829e038a,
                                     0x50fddef44734fdecULL);
  static const uint8_t buffer[10] = "frobynode";
  handler->ProcessAttributeBuffer((DwarfAttribute) 0xa55ffb51,
                                  (DwarfForm) 0x2f43b041,
                                  buffer, sizeof(buffer));
  handler->ProcessAttributeString((DwarfAttribute) 0x2f43b041,
                                  (DwarfForm) 0x895ffa23,
                                  "strange string");
}

DIEHandler* CUFixtureBase::StartNamedDIE(DIEHandler* parent,
                                         DwarfTag tag,
                                         const string& name) {
  google_breakpad::DIEHandler* handler
    = parent->FindChildHandler(0x8f4c783c0467c989ULL, tag);
  if (!handler)
    return nullptr;
  handler->ProcessAttributeString(google_breakpad::DW_AT_name,
                                  google_breakpad::DW_FORM_strp,
                                  name);
  ProcessStrangeAttributes(handler);
  if (!handler->EndAttributes()) {
    handler->Finish();
    delete handler;
    return nullptr;
  }

  return handler;
}

DIEHandler* CUFixtureBase::StartSpecifiedDIE(DIEHandler* parent,
                                             DwarfTag tag,
                                             uint64_t specification,
                                             const char* name) {
  google_breakpad::DIEHandler* handler
    = parent->FindChildHandler(0x8f4c783c0467c989ULL, tag);
  if (!handler)
    return nullptr;
  if (name)
    handler->ProcessAttributeString(google_breakpad::DW_AT_name,
                                    google_breakpad::DW_FORM_strp,
                                    name);
  handler->ProcessAttributeReference(google_breakpad::DW_AT_specification,
                                     google_breakpad::DW_FORM_ref4,
                                     specification);
  if (!handler->EndAttributes()) {
    handler->Finish();
    delete handler;
    return nullptr;
  }

  return handler;
}

void CUFixtureBase::DefineFunction(google_breakpad::DIEHandler* parent,
                                   const string& name, Module::Address address,
                                   Module::Address size,
                                   const char* mangled_name,
                                   DwarfForm high_pc_form) {
  google_breakpad::DIEHandler* func
      = parent->FindChildHandler(0xe34797c7e68590a8LL,
                                 google_breakpad::DW_TAG_subprogram);
  ASSERT_TRUE(func != nullptr);
  func->ProcessAttributeString(google_breakpad::DW_AT_name,
                               google_breakpad::DW_FORM_strp,
                               name);
  func->ProcessAttributeUnsigned(google_breakpad::DW_AT_low_pc,
                                 google_breakpad::DW_FORM_addr,
                                 address);

  Module::Address high_pc = size;
  if (high_pc_form == google_breakpad::DW_FORM_addr) {
    high_pc += address;
  }
  func->ProcessAttributeUnsigned(google_breakpad::DW_AT_high_pc,
                                 high_pc_form,
                                 high_pc);

  if (mangled_name)
    func->ProcessAttributeString(google_breakpad::DW_AT_MIPS_linkage_name,
                                 google_breakpad::DW_FORM_strp,
                                 mangled_name);

  ProcessStrangeAttributes(func);
  EXPECT_TRUE(func->EndAttributes());
  func->Finish();
  delete func;
}

void CUFixtureBase::DeclarationDIE(DIEHandler* parent, uint64_t offset,
                                   DwarfTag tag,
                                   const string& name,
                                   const string& mangled_name) {
  google_breakpad::DIEHandler* die = parent->FindChildHandler(offset, tag);
  ASSERT_TRUE(die != nullptr);
  if (!name.empty())
    die->ProcessAttributeString(google_breakpad::DW_AT_name,
                                google_breakpad::DW_FORM_strp,
                                name);
  if (!mangled_name.empty())
    die->ProcessAttributeString(google_breakpad::DW_AT_MIPS_linkage_name,
                                google_breakpad::DW_FORM_strp,
                                mangled_name);

  die->ProcessAttributeUnsigned(google_breakpad::DW_AT_declaration,
                                google_breakpad::DW_FORM_flag,
                                1);
  EXPECT_TRUE(die->EndAttributes());
  die->Finish();
  delete die;
}

void CUFixtureBase::DefinitionDIE(DIEHandler* parent,
                                  DwarfTag tag,
                                  uint64_t specification,
                                  const string& name,
                                  Module::Address address,
                                  Module::Address size) {
  google_breakpad::DIEHandler* die
    = parent->FindChildHandler(0x6ccfea031a9e6cc9ULL, tag);
  ASSERT_TRUE(die != nullptr);
  die->ProcessAttributeReference(google_breakpad::DW_AT_specification,
                                 google_breakpad::DW_FORM_ref4,
                                 specification);
  if (!name.empty())
    die->ProcessAttributeString(google_breakpad::DW_AT_name,
                                google_breakpad::DW_FORM_strp,
                                name);
  if (size) {
    die->ProcessAttributeUnsigned(google_breakpad::DW_AT_low_pc,
                                  google_breakpad::DW_FORM_addr,
                                  address);
    die->ProcessAttributeUnsigned(google_breakpad::DW_AT_high_pc,
                                  google_breakpad::DW_FORM_addr,
                                  address + size);
  }
  EXPECT_TRUE(die->EndAttributes());
  die->Finish();
  delete die;
}

void CUFixtureBase::AbstractInstanceDIE(DIEHandler* parent,
                                        uint64_t offset,
                                        DwarfInline type,
                                        uint64_t specification,
                                        const string& name,
                                        DwarfForm form) {
  google_breakpad::DIEHandler* die
    = parent->FindChildHandler(offset, google_breakpad::DW_TAG_subprogram);
  ASSERT_TRUE(die != nullptr);
  if (specification != 0ULL)
    die->ProcessAttributeReference(google_breakpad::DW_AT_specification,
                                   google_breakpad::DW_FORM_ref4,
                                   specification);
  if (form == google_breakpad::DW_FORM_sdata) {
    die->ProcessAttributeSigned(google_breakpad::DW_AT_inline, form, type);
  } else {
    die->ProcessAttributeUnsigned(google_breakpad::DW_AT_inline, form, type);
  }
  if (!name.empty())
    die->ProcessAttributeString(google_breakpad::DW_AT_name,
                                google_breakpad::DW_FORM_strp,
                                name);

  EXPECT_TRUE(die->EndAttributes());
  die->Finish();
  delete die;
}

void CUFixtureBase::DefineInlineInstanceDIE(DIEHandler* parent,
                                            const string& name,
                                            uint64_t origin,
                                            Module::Address address,
                                            Module::Address size) {
  google_breakpad::DIEHandler* func
      = parent->FindChildHandler(0x11c70f94c6e87ccdLL,
                                 google_breakpad::DW_TAG_subprogram);
  ASSERT_TRUE(func != nullptr);
  if (!name.empty()) {
    func->ProcessAttributeString(google_breakpad::DW_AT_name,
                                 google_breakpad::DW_FORM_strp,
                                 name);
  }
  func->ProcessAttributeUnsigned(google_breakpad::DW_AT_low_pc,
                                 google_breakpad::DW_FORM_addr,
                                 address);
  func->ProcessAttributeUnsigned(google_breakpad::DW_AT_high_pc,
                                 google_breakpad::DW_FORM_addr,
                                 address + size);
  func->ProcessAttributeReference(google_breakpad::DW_AT_abstract_origin,
                                 google_breakpad::DW_FORM_ref4,
                                 origin);
  ProcessStrangeAttributes(func);
  EXPECT_TRUE(func->EndAttributes());
  func->Finish();
  delete func;
}

void CUFixtureBase::FillFunctions() {
  if (functions_filled_)
    return;
  module_.GetFunctions(&functions_, functions_.end());
  sort(functions_.begin(), functions_.end(),
       Module::Function::CompareByAddress);
  functions_filled_ = true;
}

void CUFixtureBase::TestFunctionCount(size_t expected) {
  FillFunctions();
  ASSERT_EQ(expected, functions_.size());
}

void CUFixtureBase::TestFunction(int i, const string& name,
                                 Module::Address address,
                                 Module::Address size) {
  FillFunctions();
  ASSERT_LT((size_t) i, functions_.size());

  Module::Function* function = functions_[i];
  EXPECT_EQ(name,    function->name);
  EXPECT_EQ(address, function->address);
  EXPECT_EQ(size,    function->ranges[0].size);
  EXPECT_EQ(0U,      function->parameter_size);
}

void CUFixtureBase::TestFunctionPreferExternName(int i,
                                                 bool prefer_extern_name) {
  FillFunctions();
  ASSERT_LT((size_t)i, functions_.size());

  Module::Function* function = functions_[i];
  EXPECT_EQ(prefer_extern_name, function->prefer_extern_name);
}

void CUFixtureBase::TestLineCount(int i, size_t expected) {
  FillFunctions();
  ASSERT_LT((size_t) i, functions_.size());

  ASSERT_EQ(expected, functions_[i]->lines.size());
}

void CUFixtureBase::TestLine(int i, int j,
                             Module::Address address, Module::Address size,
                             const string& filename, int number) {
  FillFunctions();
  ASSERT_LT((size_t) i, functions_.size());
  ASSERT_LT((size_t) j, functions_[i]->lines.size());

  Module::Line* line = &functions_[i]->lines[j];
  EXPECT_EQ(address,  line->address);
  EXPECT_EQ(size,     line->size);
  EXPECT_EQ(filename, line->file->name.c_str());
  EXPECT_EQ(number,   line->number);
}

// Include caller locations for our test subroutines.
#define TRACE(call) do { SCOPED_TRACE("called from here"); call; } while (0)
#define PushLine(a,b,c,d)         TRACE(PushLine((a),(b),(c),(d)))
#define SetLanguage(a)            TRACE(SetLanguage(a))
#define StartCU()                 TRACE(StartCU())
#define DefineFunction(a,b,c,d,e) TRACE(DefineFunction((a),(b),(c),(d),(e)))
// (DefineFunction) instead of DefineFunction to avoid macro expansion.
#define DefineFunction6(a,b,c,d,e,f) \
    TRACE((DefineFunction)((a),(b),(c),(d),(e),(f)))
#define DeclarationDIE(a,b,c,d,e) TRACE(DeclarationDIE((a),(b),(c),(d),(e)))
#define DefinitionDIE(a,b,c,d,e,f) \
    TRACE(DefinitionDIE((a),(b),(c),(d),(e),(f)))
#define TestFunctionCount(a)      TRACE(TestFunctionCount(a))
#define TestFunction(a,b,c,d)     TRACE(TestFunction((a),(b),(c),(d)))
#define TestLineCount(a,b)        TRACE(TestLineCount((a),(b)))
#define TestLine(a,b,c,d,e,f)     TRACE(TestLine((a),(b),(c),(d),(e),(f)))

class SimpleCU: public CUFixtureBase, public Test {
};

TEST_F(SimpleCU, CompilationDir) {
  compilation_dir_ = "/src/build/";

  StartCU();
  root_handler_.Finish();
}

TEST_F(SimpleCU, OneFunc) {
  PushLine(0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, "line-file", 246571772);

  StartCU();
  DefineFunction(&root_handler_, "function1",
                 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, nullptr);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "function1", 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL);
  TestLineCount(0, 1);
  TestLine(0, 0, 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, "line-file",
           246571772);
}

// As above, only DW_AT_high_pc is a length rather than an address.
TEST_F(SimpleCU, OneFuncHighPcIsLength) {
  PushLine(0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, "line-file", 246571772);

  StartCU();
  DefineFunction6(&root_handler_, "function1",
                  0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, nullptr,
                  google_breakpad::DW_FORM_udata);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "function1", 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL);
  TestLineCount(0, 1);
  TestLine(0, 0, 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, "line-file",
           246571772);
}

TEST_F(SimpleCU, MangledName) {
  PushLine(0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, "line-file", 246571772);

  StartCU();
  DefineFunction(&root_handler_, "function1",
                 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL, "_ZN1n1fEi");
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "n::f(int)", 0x938cf8c07def4d34ULL, 0x55592d727f6cd01fLL);
}

TEST_F(SimpleCU, IrrelevantRootChildren) {
  StartCU();
  EXPECT_FALSE(root_handler_
               .FindChildHandler(0x7db32bff4e2dcfb1ULL,
                                 google_breakpad::DW_TAG_lexical_block));
}

TEST_F(SimpleCU, IrrelevantNamedScopeChildren) {
  StartCU();
  DIEHandler* class_A_handler
    = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type, "class_A");
  EXPECT_TRUE(class_A_handler != nullptr);
  EXPECT_FALSE(class_A_handler
               ->FindChildHandler(0x02e55999b865e4e9ULL,
                                  google_breakpad::DW_TAG_lexical_block));
  delete class_A_handler;
}

// Verify that FileContexts can safely be deleted unused.
TEST_F(SimpleCU, UnusedFileContext) {
  Module m("module-name", "module-os", "module-arch", "module-id");
  DwarfCUToModule::FileContext fc("dwarf-filename", &m, true);

  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");
}

TEST_F(SimpleCU, InlineFunction) {
  PushLine(0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL, "line-file", 75173118);

  StartCU();
  AbstractInstanceDIE(&root_handler_, 0x1e8dac5d507ed7abULL,
                      google_breakpad::DW_INL_inlined, 0, "inline-name");
  DefineInlineInstanceDIE(&root_handler_, "", 0x1e8dac5d507ed7abULL,
                       0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "inline-name",
               0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
}

TEST_F(SimpleCU, InlineFunctionSignedAttribute) {
  PushLine(0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL, "line-file", 75173118);

  StartCU();
  AbstractInstanceDIE(&root_handler_, 0x1e8dac5d507ed7abULL,
                      google_breakpad::DW_INL_inlined, 0, "inline-name",
                      google_breakpad::DW_FORM_sdata);
  DefineInlineInstanceDIE(&root_handler_, "", 0x1e8dac5d507ed7abULL,
                       0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "inline-name",
               0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
}

// Any DIE with an DW_AT_inline attribute can be cited by
// DW_AT_abstract_origin attributes --- even if the value of the
// DW_AT_inline attribute is DW_INL_not_inlined.
TEST_F(SimpleCU, AbstractOriginNotInlined) {
  PushLine(0x2805c4531be6ca0eULL, 0x686b52155a8d4d2cULL, "line-file", 6111581);

  StartCU();
  AbstractInstanceDIE(&root_handler_, 0x93e9cdad52826b39ULL,
                      google_breakpad::DW_INL_not_inlined, 0, "abstract-instance");
  DefineInlineInstanceDIE(&root_handler_, "", 0x93e9cdad52826b39ULL,
                          0x2805c4531be6ca0eULL, 0x686b52155a8d4d2cULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "abstract-instance",
               0x2805c4531be6ca0eULL, 0x686b52155a8d4d2cULL);
}

TEST_F(SimpleCU, UnknownAbstractOrigin) {
  PushLine(0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL, "line-file", 75173118);

  StartCU();
  AbstractInstanceDIE(&root_handler_, 0x1e8dac5d507ed7abULL,
                      google_breakpad::DW_INL_inlined, 0, "inline-name");
  DefineInlineInstanceDIE(&root_handler_, "", 1ULL,
                       0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "<name omitted>",
               0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
}

TEST_F(SimpleCU, UnnamedFunction) {
  PushLine(0x72b80e41a0ac1d40ULL, 0x537174f231ee181cULL, "line-file", 14044850);

  StartCU();
  DefineFunction(&root_handler_, "",
                 0x72b80e41a0ac1d40ULL, 0x537174f231ee181cULL, nullptr);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "<name omitted>",
               0x72b80e41a0ac1d40ULL, 0x537174f231ee181cULL);
}

// An address range.
struct Range {
  Module::Address start, end;
};

// Test data for pairing functions and lines.
struct Situation {
  // Two function intervals, and two line intervals.
  Range functions[2], lines[2];

  // The number of lines we expect to be assigned to each of the
  // functions, and the address ranges.
  int paired_count[2];
  Range paired[2][2];

  // The number of functions that are not entirely covered by lines,
  // and vice versa.
  int uncovered_functions, uncovered_lines;
};

#define PAIRING(func1_start, func1_end, func2_start, func2_end, \
                line1_start, line1_end, line2_start, line2_end, \
                func1_num_lines, func2_num_lines,               \
                func1_line1_start, func1_line1_end,             \
                func1_line2_start, func1_line2_end,             \
                func2_line1_start, func2_line1_end,             \
                func2_line2_start, func2_line2_end,             \
                uncovered_functions, uncovered_lines)           \
  { { { func1_start, func1_end }, { func2_start, func2_end } }, \
    { { line1_start, line1_end }, { line2_start, line2_end } }, \
    { func1_num_lines, func2_num_lines },                       \
    { { { func1_line1_start, func1_line1_end },                 \
        { func1_line2_start, func1_line2_end } },               \
      { { func2_line1_start, func2_line1_end },                 \
          { func2_line2_start, func2_line2_end } } },           \
    uncovered_functions, uncovered_lines },

Situation situations[] = {
#include "common/testdata/func-line-pairing.h"
};

#undef PAIRING

class FuncLinePairing: public CUFixtureBase,
                       public TestWithParam<Situation> { };

INSTANTIATE_TEST_SUITE_P(AllSituations, FuncLinePairing,
                         ValuesIn(situations));

TEST_P(FuncLinePairing, Pairing) {
  const Situation& s = GetParam();
  PushLine(s.lines[0].start,
           s.lines[0].end - s.lines[0].start,
           "line-file", 67636963);
  PushLine(s.lines[1].start,
           s.lines[1].end - s.lines[1].start,
           "line-file", 67636963);
  if (s.uncovered_functions)
    EXPECT_CALL(reporter_, UncoveredFunction(_))
      .Times(s.uncovered_functions)
      .WillRepeatedly(Return());
  if (s.uncovered_lines)
    EXPECT_CALL(reporter_, UncoveredLine(_))
      .Times(s.uncovered_lines)
      .WillRepeatedly(Return());

  StartCU();
  DefineFunction(&root_handler_, "function1",
                 s.functions[0].start,
                 s.functions[0].end - s.functions[0].start, nullptr);
  DefineFunction(&root_handler_, "function2",
                 s.functions[1].start,
                 s.functions[1].end - s.functions[1].start, nullptr);
  root_handler_.Finish();

  TestFunctionCount(2);
  TestFunction(0, "function1",
               s.functions[0].start,
               s.functions[0].end - s.functions[0].start);
  TestLineCount(0, s.paired_count[0]);
  for (int i = 0; i < s.paired_count[0]; i++)
    TestLine(0, i, s.paired[0][i].start,
             s.paired[0][i].end - s.paired[0][i].start,
             "line-file", 67636963);
  TestFunction(1, "function2",
               s.functions[1].start,
               s.functions[1].end - s.functions[1].start);
  TestLineCount(1, s.paired_count[1]);
  for (int i = 0; i < s.paired_count[1]; i++)
    TestLine(1, i, s.paired[1][i].start,
             s.paired[1][i].end - s.paired[1][i].start,
             "line-file", 67636963);
}

TEST_F(FuncLinePairing, EmptyCU) {
  StartCU();
  root_handler_.Finish();

  TestFunctionCount(0);
}

TEST_F(FuncLinePairing, LinesNoFuncs) {
  PushLine(40, 2, "line-file", 82485646);
  EXPECT_CALL(reporter_, UncoveredLine(_)).WillOnce(Return());

  StartCU();
  root_handler_.Finish();

  TestFunctionCount(0);
}

TEST_F(FuncLinePairing, FuncsNoLines) {
  EXPECT_CALL(reporter_, UncoveredFunction(_)).WillOnce(Return());

  StartCU();
  DefineFunction(&root_handler_, "function1", 0x127da12ffcf5c51fULL, 0x1000U,
                 nullptr);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "function1", 0x127da12ffcf5c51fULL, 0x1000U);
}

TEST_F(FuncLinePairing, GapThenFunction) {
  PushLine(20, 2, "line-file-2", 174314698);
  PushLine(10, 2, "line-file-1", 263008005);

  StartCU();
  DefineFunction(&root_handler_, "function1", 10, 2, nullptr);
  DefineFunction(&root_handler_, "function2", 20, 2, nullptr);
  root_handler_.Finish();

  TestFunctionCount(2);
  TestFunction(0, "function1", 10, 2);
  TestLineCount(0, 1);
  TestLine(0, 0, 10, 2, "line-file-1", 263008005);
  TestFunction(1, "function2", 20, 2);
  TestLineCount(1, 1);
  TestLine(1, 0, 20, 2, "line-file-2", 174314698);
}

// If GCC emits padding after one function to align the start of
// the next, then it will attribute the padding instructions to
// the last source line of function (to reduce the size of the
// line number info), but omit it from the DW_AT_{low,high}_pc
// range given in .debug_info (since it costs nothing to be
// precise there).  If we did use at least some of the line
// we're about to skip, then assume this is what happened, and
// don't warn.
TEST_F(FuncLinePairing, GCCAlignmentStretch) {
  PushLine(10, 10, "line-file", 63351048);
  PushLine(20, 10, "line-file", 61661044);

  StartCU();
  DefineFunction(&root_handler_, "function1", 10, 5, nullptr);
  // five-byte gap between functions, covered by line 63351048.
  // This should not elicit a warning.
  DefineFunction(&root_handler_, "function2", 20, 10, nullptr);
  root_handler_.Finish();

  TestFunctionCount(2);
  TestFunction(0, "function1", 10, 5);
  TestLineCount(0, 1);
  TestLine(0, 0, 10, 5, "line-file", 63351048);
  TestFunction(1, "function2", 20, 10);
  TestLineCount(1, 1);
  TestLine(1, 0, 20, 10, "line-file", 61661044);
}

// Unfortunately, neither the DWARF parser's handler interface nor the
// DIEHandler interface is capable of expressing a function that abuts
// the end of the address space: the high_pc value looks like zero.

TEST_F(FuncLinePairing, LineAtEndOfAddressSpace) {
  PushLine(0xfffffffffffffff0ULL, 16, "line-file", 63351048);
  EXPECT_CALL(reporter_, UncoveredLine(_)).WillOnce(Return());

  StartCU();
  DefineFunction(&root_handler_, "function1", 0xfffffffffffffff0ULL, 6,
                 nullptr);
  DefineFunction(&root_handler_, "function2", 0xfffffffffffffffaULL, 5,
                 nullptr);
  root_handler_.Finish();

  TestFunctionCount(2);
  TestFunction(0, "function1", 0xfffffffffffffff0ULL, 6);
  TestLineCount(0, 1);
  TestLine(0, 0, 0xfffffffffffffff0ULL, 6, "line-file", 63351048);
  TestFunction(1, "function2", 0xfffffffffffffffaULL, 5);
  TestLineCount(1, 1);
  TestLine(1, 0, 0xfffffffffffffffaULL, 5, "line-file", 63351048);
}

// A function with more than one uncovered area should only be warned
// about once.
TEST_F(FuncLinePairing, WarnOnceFunc) {
  PushLine(20, 1, "line-file-2", 262951329);
  PushLine(11, 1, "line-file-1", 219964021);
  EXPECT_CALL(reporter_, UncoveredFunction(_)).WillOnce(Return());

  StartCU();
  DefineFunction(&root_handler_, "function", 10, 11, nullptr);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "function", 10, 11);
  TestLineCount(0, 2);
  TestLine(0, 0, 11, 1, "line-file-1", 219964021);
  TestLine(0, 1, 20, 1, "line-file-2", 262951329);
}

// A line with more than one uncovered area should only be warned
// about once.
TEST_F(FuncLinePairing, WarnOnceLine) {
  PushLine(10, 20, "filename1", 118581871);
  EXPECT_CALL(reporter_, UncoveredLine(_)).WillOnce(Return());

  StartCU();
  DefineFunction(&root_handler_, "function1", 11, 1, nullptr);
  DefineFunction(&root_handler_, "function2", 13, 1, nullptr);
  root_handler_.Finish();

  TestFunctionCount(2);
  TestFunction(0, "function1", 11, 1);
  TestLineCount(0, 1);
  TestLine(0, 0, 11, 1, "filename1", 118581871);
  TestFunction(1, "function2", 13, 1);
  TestLineCount(1, 1);
  TestLine(1, 0, 13, 1, "filename1", 118581871);
}

class CXXQualifiedNames: public CUFixtureBase,
                         public TestWithParam<DwarfTag> { };

INSTANTIATE_TEST_SUITE_P(VersusEnclosures, CXXQualifiedNames,
                         Values(google_breakpad::DW_TAG_class_type,
                                google_breakpad::DW_TAG_structure_type,
                                google_breakpad::DW_TAG_union_type,
                                google_breakpad::DW_TAG_namespace));

TEST_P(CXXQualifiedNames, TwoFunctions) {
  DwarfTag tag = GetParam();

  SetLanguage(google_breakpad::DW_LANG_C_plus_plus);
  PushLine(10, 1, "filename1", 69819327);
  PushLine(20, 1, "filename2", 95115701);

  StartCU();
  DIEHandler* enclosure_handler = StartNamedDIE(&root_handler_, tag,
                                                "Enclosure");
  EXPECT_TRUE(enclosure_handler != nullptr);
  DefineFunction(enclosure_handler, "func_B", 10, 1, nullptr);
  DefineFunction(enclosure_handler, "func_C", 20, 1, nullptr);
  enclosure_handler->Finish();
  delete enclosure_handler;
  root_handler_.Finish();

  TestFunctionCount(2);
  TestFunction(0, "Enclosure::func_B", 10, 1);
  TestFunction(1, "Enclosure::func_C", 20, 1);
}

TEST_P(CXXQualifiedNames, FuncInEnclosureInNamespace) {
  DwarfTag tag = GetParam();

  SetLanguage(google_breakpad::DW_LANG_C_plus_plus);
  PushLine(10, 1, "line-file", 69819327);

  StartCU();
  DIEHandler* namespace_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_namespace,
                      "Namespace");
  EXPECT_TRUE(namespace_handler != nullptr);
  DIEHandler* enclosure_handler = StartNamedDIE(namespace_handler, tag,
                                                "Enclosure");
  EXPECT_TRUE(enclosure_handler != nullptr);
  DefineFunction(enclosure_handler, "function", 10, 1, nullptr);
  enclosure_handler->Finish();
  delete enclosure_handler;
  namespace_handler->Finish();
  delete namespace_handler;
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "Namespace::Enclosure::function", 10, 1);
}

TEST_F(CXXQualifiedNames, FunctionInClassInStructInNamespace) {
  SetLanguage(google_breakpad::DW_LANG_C_plus_plus);
  PushLine(10, 1, "filename1", 69819327);

  StartCU();
  DIEHandler* namespace_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_namespace,
                      "namespace_A");
  EXPECT_TRUE(namespace_handler != nullptr);
  DIEHandler* struct_handler
      = StartNamedDIE(namespace_handler, google_breakpad::DW_TAG_structure_type,
                      "struct_B");
  EXPECT_TRUE(struct_handler != nullptr);
  DIEHandler* class_handler
      = StartNamedDIE(struct_handler, google_breakpad::DW_TAG_class_type,
                      "class_C");
  DefineFunction(class_handler, "function_D", 10, 1, nullptr);
  class_handler->Finish();
  delete class_handler;
  struct_handler->Finish();
  delete struct_handler;
  namespace_handler->Finish();
  delete namespace_handler;
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "namespace_A::struct_B::class_C::function_D", 10, 1);
}

struct LanguageAndQualifiedName {
  google_breakpad::DwarfLanguage language;
  const char* name;
};

const LanguageAndQualifiedName LanguageAndQualifiedNameCases[] = {
  { google_breakpad::DW_LANG_none,           "class_A::function_B" },
  { google_breakpad::DW_LANG_C,              "class_A::function_B" },
  { google_breakpad::DW_LANG_C89,            "class_A::function_B" },
  { google_breakpad::DW_LANG_C99,            "class_A::function_B" },
  { google_breakpad::DW_LANG_C_plus_plus,    "class_A::function_B" },
  { google_breakpad::DW_LANG_Java,           "class_A.function_B" },
  { google_breakpad::DW_LANG_Cobol74,        "class_A::function_B" },
  { google_breakpad::DW_LANG_Mips_Assembler, nullptr }
};

class QualifiedForLanguage
    : public CUFixtureBase,
      public TestWithParam<LanguageAndQualifiedName> { };

INSTANTIATE_TEST_SUITE_P(LanguageAndQualifiedName, QualifiedForLanguage,
                         ValuesIn(LanguageAndQualifiedNameCases));

TEST_P(QualifiedForLanguage, MemberFunction) {
  const LanguageAndQualifiedName& param = GetParam();

  PushLine(10, 1, "line-file", 212966758);
  SetLanguage(param.language);

  StartCU();
  DIEHandler* class_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                      "class_A");
  DefineFunction(class_handler, "function_B", 10, 1, nullptr);
  class_handler->Finish();
  delete class_handler;
  root_handler_.Finish();

  if (param.name) {
    TestFunctionCount(1);
    TestFunction(0, param.name, 10, 1);
  } else {
    TestFunctionCount(0);
  }
}

TEST_P(QualifiedForLanguage, MemberFunctionSignedLanguage) {
  const LanguageAndQualifiedName& param = GetParam();

  PushLine(10, 1, "line-file", 212966758);
  SetLanguage(param.language);
  SetLanguageSigned(true);

  StartCU();
  DIEHandler* class_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                      "class_A");
  DefineFunction(class_handler, "function_B", 10, 1, nullptr);
  class_handler->Finish();
  delete class_handler;
  root_handler_.Finish();

  if (param.name) {
    TestFunctionCount(1);
    TestFunction(0, param.name, 10, 1);
  } else {
    TestFunctionCount(0);
  }
}

class Specifications: public CUFixtureBase, public Test { };

TEST_F(Specifications, Function) {
  PushLine(0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL, "line-file", 54883661);

  StartCU();
  DeclarationDIE(&root_handler_, 0xcd3c51b946fb1eeeLL,
                 google_breakpad::DW_TAG_subprogram, "declaration-name", "");
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0xcd3c51b946fb1eeeLL, "",
                0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "declaration-name",
               0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
}

TEST_F(Specifications, MangledName) {
  // Language defaults to C++, so no need to set it here.
  PushLine(0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL, "line-file", 54883661);

  StartCU();
  DeclarationDIE(&root_handler_, 0xcd3c51b946fb1eeeLL,
                 google_breakpad::DW_TAG_subprogram, "declaration-name",
                 "_ZN1C1fEi");
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0xcd3c51b946fb1eeeLL, "",
                0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "C::f(int)",
               0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
}

TEST_F(Specifications, MangledNameSwift) {
  // Swift mangled names should pass through untouched.
  SetLanguage(google_breakpad::DW_LANG_Swift);
  PushLine(0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL, "line-file", 54883661);
  StartCU();
  const string kName = "_TFC9swifttest5Shape17simpleDescriptionfS0_FT_Si";
  DeclarationDIE(&root_handler_, 0xcd3c51b946fb1eeeLL,
                 google_breakpad::DW_TAG_subprogram, "declaration-name",
                 kName);
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0xcd3c51b946fb1eeeLL, "",
                0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, kName,
               0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
}

TEST_F(Specifications, MangledNameRust) {
  SetLanguage(google_breakpad::DW_LANG_Rust);
  PushLine(0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL, "line-file", 54883661);

  StartCU();
  const string kName = "_ZN14rustc_demangle8demangle17h373defa94bffacdeE";
  DeclarationDIE(&root_handler_, 0xcd3c51b946fb1eeeLL,
                 google_breakpad::DW_TAG_subprogram, "declaration-name",
                 kName);
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0xcd3c51b946fb1eeeLL, "",
                0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0,
#ifndef HAVE_RUSTC_DEMANGLE
               // Rust mangled names should pass through untouched if not
               // using rustc-demangle.
               kName,
#else
               // If rustc-demangle is available this should be properly
               // demangled.
               "rustc_demangle::demangle",
#endif
               0x93cd3dfc1aa10097ULL, 0x0397d47a0b4ca0d4ULL);
}

TEST_F(Specifications, MemberFunction) {
  PushLine(0x3341a248634e7170ULL, 0x5f6938ee5553b953ULL, "line-file", 18116691);

  StartCU();
  DIEHandler* class_handler
    = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type, "class_A");
  DeclarationDIE(class_handler, 0x7d83028c431406e8ULL,
                 google_breakpad::DW_TAG_subprogram, "declaration-name", "");
  class_handler->Finish();
  delete class_handler;
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0x7d83028c431406e8ULL, "",
                0x3341a248634e7170ULL, 0x5f6938ee5553b953ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "class_A::declaration-name",
               0x3341a248634e7170ULL, 0x5f6938ee5553b953ULL);
}

// This case should gather the name from both the definition and the
// declaration's parent.
TEST_F(Specifications, FunctionDeclarationParent) {
  PushLine(0x463c9ddf405be227ULL, 0x6a47774af5049680ULL, "line-file", 70254922);

  StartCU();
  {
    DIEHandler* class_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                      "class_A");
    ASSERT_TRUE(class_handler != nullptr);
    DeclarationDIE(class_handler, 0x0e0e877c8404544aULL,
                   google_breakpad::DW_TAG_subprogram, "declaration-name", "");
    class_handler->Finish();
    delete class_handler;
  }

  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0x0e0e877c8404544aULL, "definition-name",
                0x463c9ddf405be227ULL, 0x6a47774af5049680ULL);

  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "class_A::definition-name",
               0x463c9ddf405be227ULL, 0x6a47774af5049680ULL);
}

// Named scopes should also gather enclosing name components from
// their declarations.
TEST_F(Specifications, NamedScopeDeclarationParent) {
  PushLine(0x5d13433d0df13d00ULL, 0x48ebebe5ade2cab4ULL, "line-file", 77392604);

  StartCU();
  {
    DIEHandler* space_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_namespace,
                      "space_A");
    ASSERT_TRUE(space_handler != nullptr);
    DeclarationDIE(space_handler, 0x419bb1d12f9a73a2ULL,
                   google_breakpad::DW_TAG_class_type, "class-declaration-name",
                   "");
    space_handler->Finish();
    delete space_handler;
  }

  {
    DIEHandler* class_handler
      = StartSpecifiedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                          0x419bb1d12f9a73a2ULL, "class-definition-name");
    ASSERT_TRUE(class_handler != nullptr);
    DefineFunction(class_handler, "function",
                   0x5d13433d0df13d00ULL, 0x48ebebe5ade2cab4ULL, nullptr);
    class_handler->Finish();
    delete class_handler;
  }

  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "space_A::class-definition-name::function",
               0x5d13433d0df13d00ULL, 0x48ebebe5ade2cab4ULL);
}

// This test recreates bug 364.
TEST_F(Specifications, InlineFunction) {
  PushLine(0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL, "line-file", 75173118);

  StartCU();
  DeclarationDIE(&root_handler_, 0xcd3c51b946fb1eeeLL,
                 google_breakpad::DW_TAG_subprogram, "inline-name", "");
  AbstractInstanceDIE(&root_handler_, 0x1e8dac5d507ed7abULL,
                      google_breakpad::DW_INL_inlined, 0xcd3c51b946fb1eeeLL, "");
  DefineInlineInstanceDIE(&root_handler_, "", 0x1e8dac5d507ed7abULL,
                       0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "inline-name",
               0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
}

// An inline function in a namespace should correctly derive its
// name from its abstract origin, and not just the namespace name.
TEST_F(Specifications, InlineFunctionInNamespace) {
  PushLine(0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL, "line-file", 75173118);

  StartCU();
  DIEHandler* space_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_namespace,
                      "Namespace");
  ASSERT_TRUE(space_handler != nullptr);
  AbstractInstanceDIE(space_handler, 0x1e8dac5d507ed7abULL,
                      google_breakpad::DW_INL_inlined, 0LL, "func-name");
  DefineInlineInstanceDIE(space_handler, "", 0x1e8dac5d507ed7abULL,
                       0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
  space_handler->Finish();
  delete space_handler;
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "Namespace::func-name",
               0x1758a0f941b71efbULL, 0x1cf154f1f545e146ULL);
}

// Check name construction for a long chain containing each combination of:
// - struct, union, class, namespace
// - direct and definition
TEST_F(Specifications, LongChain) {
  PushLine(0x5a0dd6bb85db754cULL, 0x3bccb213d08c7fd3ULL, "line-file", 21192926);
  SetLanguage(google_breakpad::DW_LANG_C_plus_plus);

  StartCU();
  // The structure we're building here is:
  // space_A full definition
  //   space_B declaration
  // space_B definition
  //   struct_C full definition
  //     struct_D declaration
  // struct_D definition
  //   union_E full definition
  //     union_F declaration
  // union_F definition
  //   class_G full definition
  //     class_H declaration
  // class_H definition
  //   func_I declaration
  // func_I definition
  //
  // So:
  // - space_A, struct_C, union_E, and class_G don't use specifications;
  // - space_B, struct_D, union_F, and class_H do.
  // - func_I uses a specification.
  //
  // The full name for func_I is thus:
  //
  // space_A::space_B::struct_C::struct_D::union_E::union_F::
  //   class_G::class_H::func_I
  {
    DIEHandler* space_A_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_namespace,
                      "space_A");
    DeclarationDIE(space_A_handler, 0x2e111126496596e2ULL,
                   google_breakpad::DW_TAG_namespace, "space_B", "");
    space_A_handler->Finish();
    delete space_A_handler;
  }

  {
    DIEHandler* space_B_handler
      = StartSpecifiedDIE(&root_handler_, google_breakpad::DW_TAG_namespace,
                          0x2e111126496596e2ULL);
    DIEHandler* struct_C_handler
      = StartNamedDIE(space_B_handler, google_breakpad::DW_TAG_structure_type,
                      "struct_C");
    DeclarationDIE(struct_C_handler, 0x20cd423bf2a25a4cULL,
                   google_breakpad::DW_TAG_structure_type, "struct_D", "");
    struct_C_handler->Finish();
    delete struct_C_handler;
    space_B_handler->Finish();
    delete space_B_handler;
  }

  {
    DIEHandler* struct_D_handler
      = StartSpecifiedDIE(&root_handler_, google_breakpad::DW_TAG_structure_type,
                          0x20cd423bf2a25a4cULL);
    DIEHandler* union_E_handler
      = StartNamedDIE(struct_D_handler, google_breakpad::DW_TAG_union_type,
                      "union_E");
    DeclarationDIE(union_E_handler, 0xe25c84805aa58c32ULL,
                   google_breakpad::DW_TAG_union_type, "union_F", "");
    union_E_handler->Finish();
    delete union_E_handler;
    struct_D_handler->Finish();
    delete struct_D_handler;
  }

  {
    DIEHandler* union_F_handler
      = StartSpecifiedDIE(&root_handler_, google_breakpad::DW_TAG_union_type,
                          0xe25c84805aa58c32ULL);
    DIEHandler* class_G_handler
      = StartNamedDIE(union_F_handler, google_breakpad::DW_TAG_class_type,
                      "class_G");
    DeclarationDIE(class_G_handler, 0xb70d960dcc173b6eULL,
                   google_breakpad::DW_TAG_class_type, "class_H", "");
    class_G_handler->Finish();
    delete class_G_handler;
    union_F_handler->Finish();
    delete union_F_handler;
  }

  {
    DIEHandler* class_H_handler
      = StartSpecifiedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                          0xb70d960dcc173b6eULL);
    DeclarationDIE(class_H_handler, 0x27ff829e3bf69f37ULL,
                   google_breakpad::DW_TAG_subprogram, "func_I", "");
    class_H_handler->Finish();
    delete class_H_handler;
  }

  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0x27ff829e3bf69f37ULL, "",
                0x5a0dd6bb85db754cULL, 0x3bccb213d08c7fd3ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "space_A::space_B::struct_C::struct_D::union_E::union_F"
               "::class_G::class_H::func_I",
               0x5a0dd6bb85db754cULL, 0x3bccb213d08c7fd3ULL);
}

TEST_F(Specifications, InterCU) {
  Module m("module-name", "module-os", "module-arch", "module-id");
  DwarfCUToModule::FileContext fc("dwarf-filename", &m, true);
  EXPECT_CALL(reporter_, UncoveredFunction(_)).WillOnce(Return());
  MockLineToModuleHandler lr;
  EXPECT_CALL(lr, ReadProgram(_,_,_,_,_,_,_,_,_)).Times(0);

  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");

  // First CU.  Declares class_A.
  {
    DwarfCUToModule root1_handler(&fc, &lr, nullptr, &reporter_);
    ASSERT_TRUE(root1_handler.StartCompilationUnit(0, 1, 2, 3, 3));
    ASSERT_TRUE(root1_handler.StartRootDIE(1,
                                           google_breakpad::DW_TAG_compile_unit));
    ProcessStrangeAttributes(&root1_handler);
    ASSERT_TRUE(root1_handler.EndAttributes());
    DeclarationDIE(&root1_handler, 0xb8fbfdd5f0b26fceULL,
                   google_breakpad::DW_TAG_class_type, "class_A", "");
    root1_handler.Finish();
  }

  // Second CU.  Defines class_A, declares member_func_B.
  {
    DwarfCUToModule root2_handler(&fc, &lr, nullptr, &reporter_);
    ASSERT_TRUE(root2_handler.StartCompilationUnit(0, 1, 2, 3, 3));
    ASSERT_TRUE(root2_handler.StartRootDIE(1,
                                           google_breakpad::DW_TAG_compile_unit));
    ASSERT_TRUE(root2_handler.EndAttributes());
    DIEHandler* class_A_handler
      = StartSpecifiedDIE(&root2_handler, google_breakpad::DW_TAG_class_type,
                          0xb8fbfdd5f0b26fceULL);
    DeclarationDIE(class_A_handler, 0xb01fef8b380bd1a2ULL,
                   google_breakpad::DW_TAG_subprogram, "member_func_B", "");
    class_A_handler->Finish();
    delete class_A_handler;
    root2_handler.Finish();
  }

  // Third CU.  Defines member_func_B.
  {
    DwarfCUToModule root3_handler(&fc, &lr, nullptr, &reporter_);
    ASSERT_TRUE(root3_handler.StartCompilationUnit(0, 1, 2, 3, 3));
    ASSERT_TRUE(root3_handler.StartRootDIE(1,
                                           google_breakpad::DW_TAG_compile_unit));
    ASSERT_TRUE(root3_handler.EndAttributes());
    DefinitionDIE(&root3_handler, google_breakpad::DW_TAG_subprogram,
                  0xb01fef8b380bd1a2ULL, "",
                  0x2618f00a1a711e53ULL, 0x4fd94b76d7c2caf5ULL);
    root3_handler.Finish();
  }

  vector<Module::Function*> functions;
  m.GetFunctions(&functions, functions.end());
  EXPECT_EQ(1U, functions.size());
  EXPECT_STREQ("class_A::member_func_B", functions[0]->name.str().c_str());
}

TEST_F(Specifications, UnhandledInterCU) {
  Module m("module-name", "module-os", "module-arch", "module-id");
  DwarfCUToModule::FileContext fc("dwarf-filename", &m, false);
  EXPECT_CALL(reporter_, UncoveredFunction(_)).WillOnce(Return());
  MockLineToModuleHandler lr;
  EXPECT_CALL(lr, ReadProgram(_,_,_,_,_,_,_,_,_)).Times(0);

  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");

  // First CU.  Declares class_A.
  {
    DwarfCUToModule root1_handler(&fc, &lr, nullptr, &reporter_);
    ASSERT_TRUE(root1_handler.StartCompilationUnit(0, 1, 2, 3, 3));
    ASSERT_TRUE(root1_handler.StartRootDIE(1,
                                           google_breakpad::DW_TAG_compile_unit));
    ProcessStrangeAttributes(&root1_handler);
    ASSERT_TRUE(root1_handler.EndAttributes());
    DeclarationDIE(&root1_handler, 0xb8fbfdd5f0b26fceULL,
                   google_breakpad::DW_TAG_class_type, "class_A", "");
    root1_handler.Finish();
  }

  // Second CU.  Defines class_A, declares member_func_B.
  {
    DwarfCUToModule root2_handler(&fc, &lr, nullptr, &reporter_);
    ASSERT_TRUE(root2_handler.StartCompilationUnit(0, 1, 2, 3, 3));
    ASSERT_TRUE(root2_handler.StartRootDIE(1,
                                           google_breakpad::DW_TAG_compile_unit));
    ASSERT_TRUE(root2_handler.EndAttributes());
    EXPECT_CALL(reporter_, UnhandledInterCUReference(_, _)).Times(1);
    DIEHandler* class_A_handler
      = StartSpecifiedDIE(&root2_handler, google_breakpad::DW_TAG_class_type,
                          0xb8fbfdd5f0b26fceULL);
    DeclarationDIE(class_A_handler, 0xb01fef8b380bd1a2ULL,
                   google_breakpad::DW_TAG_subprogram, "member_func_B", "");
    class_A_handler->Finish();
    delete class_A_handler;
    root2_handler.Finish();
  }

  // Third CU.  Defines member_func_B.
  {
    DwarfCUToModule root3_handler(&fc, &lr, nullptr, &reporter_);
    ASSERT_TRUE(root3_handler.StartCompilationUnit(0, 1, 2, 3, 3));
    ASSERT_TRUE(root3_handler.StartRootDIE(1,
                                           google_breakpad::DW_TAG_compile_unit));
    ASSERT_TRUE(root3_handler.EndAttributes());
    EXPECT_CALL(reporter_, UnhandledInterCUReference(_, _)).Times(1);
    DefinitionDIE(&root3_handler, google_breakpad::DW_TAG_subprogram,
                  0xb01fef8b380bd1a2ULL, "",
                  0x2618f00a1a711e53ULL, 0x4fd94b76d7c2caf5ULL);
    root3_handler.Finish();
  }
}

TEST_F(Specifications, BadOffset) {
  PushLine(0xa0277efd7ce83771ULL, 0x149554a184c730c1ULL, "line-file", 56636272);

  StartCU();
  DeclarationDIE(&root_handler_, 0xefd7f7752c27b7e4ULL,
                 google_breakpad::DW_TAG_subprogram, "", "");
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0x2be953efa6f9a996ULL, "function",
                0xa0277efd7ce83771ULL, 0x149554a184c730c1ULL);
  root_handler_.Finish();
}

TEST_F(Specifications, FunctionDefinitionHasOwnName) {
  PushLine(0xced50b3eea81022cULL, 0x08dd4d301cc7a7d2ULL, "line-file", 56792403);

  StartCU();
  DeclarationDIE(&root_handler_, 0xc34ff4786cae78bdULL,
                 google_breakpad::DW_TAG_subprogram, "declaration-name", "");
  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0xc34ff4786cae78bdULL, "definition-name",
                0xced50b3eea81022cULL, 0x08dd4d301cc7a7d2ULL);
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "definition-name",
               0xced50b3eea81022cULL, 0x08dd4d301cc7a7d2ULL);
}

TEST_F(Specifications, ClassDefinitionHasOwnName) {
  PushLine(0x1d0f5e0f6ce309bdULL, 0x654e1852ec3599e7ULL, "line-file", 57119241);

  StartCU();
  DeclarationDIE(&root_handler_, 0xd0fe467ec2f1a58cULL,
                 google_breakpad::DW_TAG_class_type, "class-declaration-name", "");

  google_breakpad::DIEHandler* class_definition
    = StartSpecifiedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                        0xd0fe467ec2f1a58cULL, "class-definition-name");
  ASSERT_TRUE(class_definition);
  DeclarationDIE(class_definition, 0x6d028229c15623dbULL,
                 google_breakpad::DW_TAG_subprogram,
                 "function-declaration-name", "");
  class_definition->Finish();
  delete class_definition;

  DefinitionDIE(&root_handler_, google_breakpad::DW_TAG_subprogram,
                0x6d028229c15623dbULL, "function-definition-name",
                0x1d0f5e0f6ce309bdULL, 0x654e1852ec3599e7ULL);

  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "class-definition-name::function-definition-name",
               0x1d0f5e0f6ce309bdULL, 0x654e1852ec3599e7ULL);
}

// DIEs that cite a specification should prefer the specification's
// parents over their own when choosing qualified names. In this test,
// we take the name from our definition but the enclosing scope name
// from our declaration. I don't see why they'd ever be different, but
// we want to verify what DwarfCUToModule is looking at.
TEST_F(Specifications, PreferSpecificationParents) {
  PushLine(0xbbd9d54dce3b95b7ULL, 0x39188b7b52b0899fULL, "line-file", 79488694);

  StartCU();
  {
    google_breakpad::DIEHandler* declaration_class_handler =
      StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                    "declaration-class");
    DeclarationDIE(declaration_class_handler, 0x9ddb35517455ef7aULL,
                   google_breakpad::DW_TAG_subprogram, "function-declaration",
                   "");
    declaration_class_handler->Finish();
    delete declaration_class_handler;
  }
  {
    google_breakpad::DIEHandler* definition_class_handler
      = StartNamedDIE(&root_handler_, google_breakpad::DW_TAG_class_type,
                      "definition-class");
    DefinitionDIE(definition_class_handler, google_breakpad::DW_TAG_subprogram,
                  0x9ddb35517455ef7aULL, "function-definition",
                  0xbbd9d54dce3b95b7ULL, 0x39188b7b52b0899fULL);
    definition_class_handler->Finish();
    delete definition_class_handler;
  }
  root_handler_.Finish();

  TestFunctionCount(1);
  TestFunction(0, "declaration-class::function-definition",
               0xbbd9d54dce3b95b7ULL, 0x39188b7b52b0899fULL);
}

class CUErrors: public CUFixtureBase, public Test { };

TEST_F(CUErrors, BadStmtList) {
  EXPECT_CALL(reporter_, BadLineInfoOffset(dummy_line_size_ + 10)).Times(1);

  ASSERT_TRUE(root_handler_
              .StartCompilationUnit(0xc591d5b037543d7cULL, 0x11, 0xcd,
                                    0x2d7d19546cf6590cULL, 3));
  ASSERT_TRUE(root_handler_.StartRootDIE(0xae789dc102cfca54ULL,
                                         google_breakpad::DW_TAG_compile_unit));
  root_handler_.ProcessAttributeString(google_breakpad::DW_AT_name,
                                       google_breakpad::DW_FORM_strp,
                                       "compilation-unit-name");
  root_handler_.ProcessAttributeUnsigned(google_breakpad::DW_AT_stmt_list,
                                         google_breakpad::DW_FORM_ref4,
                                         dummy_line_size_ + 10);
  root_handler_.EndAttributes();
  root_handler_.Finish();
}

TEST_F(CUErrors, NoLineSection) {
  EXPECT_CALL(reporter_, MissingSection(".debug_line")).Times(1);
  PushLine(0x88507fb678052611ULL, 0x42c8e9de6bbaa0faULL, "line-file", 64472290);
  // Delete the entry for .debug_line added by the fixture class's constructor.
  file_context_.ClearSectionMapForTest();

  StartCU();
  root_handler_.Finish();
}

TEST_F(CUErrors, BadDwarfVersion1) {
  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");

  ASSERT_FALSE(root_handler_
               .StartCompilationUnit(0xadf6e0eb71e2b0d9ULL, 0x4d, 0x90,
                                     0xc9de224ccb99ac3eULL, 1));
}

TEST_F(CUErrors, GoodDwarfVersion2) {
  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");

  ASSERT_TRUE(root_handler_
               .StartCompilationUnit(0xadf6e0eb71e2b0d9ULL, 0x4d, 0x90,
                                     0xc9de224ccb99ac3eULL, 2));
}

TEST_F(CUErrors, GoodDwarfVersion3) {
  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");

  ASSERT_TRUE(root_handler_
               .StartCompilationUnit(0xadf6e0eb71e2b0d9ULL, 0x4d, 0x90,
                                     0xc9de224ccb99ac3eULL, 3));
}

TEST_F(CUErrors, BadCURootDIETag) {
  // Kludge: satisfy reporter_'s expectation.
  reporter_.SetCUName("compilation-unit-name");

  ASSERT_TRUE(root_handler_
               .StartCompilationUnit(0xadf6e0eb71e2b0d9ULL, 0x4d, 0x90,
                                     0xc9de224ccb99ac3eULL, 3));

  ASSERT_FALSE(root_handler_.StartRootDIE(0x02e56bfbda9e7337ULL,
                                          google_breakpad::DW_TAG_subprogram));
}

// Tests for DwarfCUToModule::Reporter. These just produce (or fail to
// produce) output, so their results need to be checked by hand.
struct Reporter: public Test {
  Reporter()
      : reporter("filename", 0x123456789abcdef0ULL),
        function("function name", 0x19c45c30770c1eb0ULL),
        file("source file name") {
    reporter.SetCUName("compilation-unit-name");

    Module::Range range(0x19c45c30770c1eb0ULL, 0x89808a5bdfa0a6a3ULL);
    function.ranges.push_back(range);
    function.parameter_size = 0x6a329f18683dcd51ULL;

    line.address = 0x3606ac6267aebeccULL;
    line.size = 0x5de482229f32556aULL;
    line.file = &file;
    line.number = 93400201;
  }

  DwarfCUToModule::WarningReporter reporter;
  Module::Function function;
  Module::File file;
  Module::Line line;
};

TEST_F(Reporter, UnknownSpecification) {
  reporter.UnknownSpecification(0x123456789abcdef1ULL, 0x323456789abcdef2ULL);
}

TEST_F(Reporter, UnknownAbstractOrigin) {
  reporter.UnknownAbstractOrigin(0x123456789abcdef1ULL, 0x323456789abcdef2ULL);
}

TEST_F(Reporter, MissingSection) {
  reporter.MissingSection("section name");
}

TEST_F(Reporter, BadLineInfoOffset) {
  reporter.BadLineInfoOffset(0x123456789abcdef1ULL);
}

TEST_F(Reporter, UncoveredFunctionDisabled) {
  reporter.UncoveredFunction(function);
  EXPECT_FALSE(reporter.uncovered_warnings_enabled());
}

TEST_F(Reporter, UncoveredFunctionEnabled) {
  reporter.set_uncovered_warnings_enabled(true);
  reporter.UncoveredFunction(function);
  EXPECT_TRUE(reporter.uncovered_warnings_enabled());
}

TEST_F(Reporter, UncoveredLineDisabled) {
  reporter.UncoveredLine(line);
  EXPECT_FALSE(reporter.uncovered_warnings_enabled());
}

TEST_F(Reporter, UncoveredLineEnabled) {
  reporter.set_uncovered_warnings_enabled(true);
  reporter.UncoveredLine(line);
  EXPECT_TRUE(reporter.uncovered_warnings_enabled());
}

TEST_F(Reporter, UnnamedFunction) {
  reporter.UnnamedFunction(0x90c0baff9dedb2d9ULL);
}

// Would be nice to also test:
// - overlapping lines, functions

// Copyright 2013 Google LLC
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

// stackwalker_ppc64.cc: ppc64-specific stackwalker.
//
// See stackwalker_ppc64.h for documentation.


#ifdef HAVE_CONFIG_H
#include <config.h>  // Must come first
#endif

#include <memory>

#include "processor/stackwalker_ppc64.h"
#include "google_breakpad/processor/call_stack.h"
#include "google_breakpad/processor/memory_region.h"
#include "google_breakpad/processor/stack_frame_cpu.h"
#include "processor/logging.h"

#include <stdio.h>

namespace google_breakpad {


StackwalkerPPC64::StackwalkerPPC64(const SystemInfo* system_info,
                                   const MDRawContextPPC64* context,
                                   MemoryRegion* memory,
                                   const CodeModules* modules,
                                   StackFrameSymbolizer* resolver_helper)
    : Stackwalker(system_info, memory, modules, resolver_helper),
      context_(context) {
}


StackFrame* StackwalkerPPC64::GetContextFrame() {
  if (!context_) {
    BPLOG(ERROR) << "Can't get context frame without context";
    return nullptr;
  }

  StackFramePPC64* frame = new StackFramePPC64();

  // The instruction pointer is stored directly in a register, so pull it
  // straight out of the CPU context structure.
  frame->context = *context_;
  frame->context_validity = StackFramePPC64::CONTEXT_VALID_ALL;
  frame->trust = StackFrame::FRAME_TRUST_CONTEXT;
  frame->instruction = frame->context.srr0;

  return frame;
}


StackFrame* StackwalkerPPC64::GetCallerFrame(const CallStack* stack,
                                             bool stack_scan_allowed) {
  if (!memory_ || !stack) {
    BPLOG(ERROR) << "Can't get caller frame without memory or stack";
    return nullptr;
  }

  // The instruction pointers for previous frames are saved on the stack.
  // The typical ppc64 calling convention is for the called procedure to store
  // its return address in the calling procedure's stack frame at 8(%r1),
  // and to allocate its own stack frame by decrementing %r1 (the stack
  // pointer) and saving the old value of %r1 at 0(%r1).  Because the ppc64 has
  // no hardware stack, there is no distinction between the stack pointer and
  // frame pointer, and what is typically thought of as the frame pointer on
  // an x86 is usually referred to as the stack pointer on a ppc64.

  StackFramePPC64* last_frame = static_cast<StackFramePPC64*>(
      stack->frames()->back());

  // A caller frame must reside higher in memory than its callee frames.
  // Anything else is an error, or an indication that we've reached the
  // end of the stack.
  uint64_t stack_pointer;
  if (!memory_->GetMemoryAtAddress(last_frame->context.gpr[1],
                                   &stack_pointer) ||
      stack_pointer <= last_frame->context.gpr[1]) {
    return nullptr;
  }

  // Mac OS X/Darwin gives 1 as the return address from the bottom-most
  // frame in a stack (a thread's entry point).  I haven't found any
  // documentation on this, but 0 or 1 would be bogus return addresses,
  // so check for them here and return false (end of stack) when they're
  // hit to avoid having a phantom frame.
  uint64_t instruction;
  if (!memory_->GetMemoryAtAddress(stack_pointer + 16, &instruction) ||
      instruction <= 1) {
    return nullptr;
  }

  std::unique_ptr<StackFramePPC64> frame(new StackFramePPC64());

  frame->context = last_frame->context;
  frame->context.srr0 = instruction;
  frame->context.gpr[1] = stack_pointer;
  frame->context_validity = StackFramePPC64::CONTEXT_VALID_SRR0 |
                            StackFramePPC64::CONTEXT_VALID_GPR1;
  frame->trust = StackFrame::FRAME_TRUST_FP;

  // Should we terminate the stack walk? (end-of-stack or broken invariant)
  if (TerminateWalk(instruction, stack_pointer, last_frame->context.gpr[1],
                    /*is_context_frame=*/last_frame->trust ==
                        StackFrame::FRAME_TRUST_CONTEXT)) {
    return nullptr;
  }

  // frame->context.srr0 is the return address, which is one instruction
  // past the branch that caused us to arrive at the callee.  Set
  // frame_ppc64->instruction to eight less than that.  Since all ppc64
  // instructions are 8 bytes wide, this is the address of the branch
  // instruction.  This allows source line information to match up with the
  // line that contains a function call.  Callers that require the exact
  // return address value may access the context.srr0 field of StackFramePPC64.
  frame->instruction = frame->context.srr0 - 8;

  return frame.release();
}


}  // namespace google_breakpad

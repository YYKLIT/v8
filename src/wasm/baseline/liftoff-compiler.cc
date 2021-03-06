// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/wasm/baseline/liftoff-assembler.h"

#include "src/assembler-inl.h"
#include "src/compiler/linkage.h"
#include "src/compiler/wasm-compiler.h"
#include "src/counters.h"
#include "src/macro-assembler-inl.h"
#include "src/wasm/function-body-decoder-impl.h"
#include "src/wasm/wasm-objects.h"
#include "src/wasm/wasm-opcodes.h"

namespace v8 {
namespace internal {
namespace wasm {

constexpr auto kRegister = LiftoffAssembler::VarState::kRegister;
constexpr auto kConstant = LiftoffAssembler::VarState::kConstant;
constexpr auto kStack = LiftoffAssembler::VarState::kStack;

namespace {

#define __ asm_->

#define TRACE(...)                                            \
  do {                                                        \
    if (FLAG_trace_liftoff) PrintF("[liftoff] " __VA_ARGS__); \
  } while (false)

#if V8_TARGET_ARCH_ARM64
// On ARM64, the Assembler keeps track of pointers to Labels to resolve
// branches to distant targets. Moving labels would confuse the Assembler,
// thus store the label on the heap and keep a unique_ptr.
class MovableLabel {
 public:
  Label* get() { return label_.get(); }

 private:
  std::unique_ptr<Label> label_ = base::make_unique<Label>();
};
#else
// On all other platforms, just store the Label directly.
class MovableLabel {
 public:
  Label* get() { return &label_; }

 private:
  Label label_;
};
#endif

class LiftoffCompiler {
 public:
  MOVE_ONLY_NO_DEFAULT_CONSTRUCTOR(LiftoffCompiler);

  // TODO(clemensh): Make this a template parameter.
  static constexpr wasm::Decoder::ValidateFlag validate =
      wasm::Decoder::kValidate;

  using Value = ValueBase;

  struct Control : public ControlWithNamedConstructors<Control, Value> {
    MOVE_ONLY_WITH_DEFAULT_CONSTRUCTORS(Control);

    LiftoffAssembler::CacheState label_state;
    MovableLabel label;
  };

  using Decoder = WasmFullDecoder<validate, LiftoffCompiler>;

  LiftoffCompiler(LiftoffAssembler* liftoff_asm,
                  compiler::CallDescriptor* call_desc, compiler::ModuleEnv* env)
      : asm_(liftoff_asm),
        call_desc_(call_desc),
        env_(env),
        compilation_zone_(liftoff_asm->isolate()->allocator(),
                          "liftoff compilation"),
        safepoint_table_builder_(&compilation_zone_) {}

  bool ok() const { return ok_; }

  void unsupported(Decoder* decoder, const char* reason) {
    ok_ = false;
    TRACE("unsupported: %s\n", reason);
    decoder->errorf(decoder->pc(), "unsupported liftoff operation: %s", reason);
    BindUnboundLabels(decoder);
  }

  int GetSafepointTableOffset() const {
    return safepoint_table_builder_.GetCodeOffset();
  }

  void BindUnboundLabels(Decoder* decoder) {
#ifdef DEBUG
    // Bind all labels now, otherwise their destructor will fire a DCHECK error
    // if they where referenced before.
    for (uint32_t i = 0, e = decoder->control_depth(); i < e; ++i) {
      Label* label = decoder->control_at(i)->label.get();
      if (!label->is_bound()) __ bind(label);
    }
#endif
  }

  void CheckStackSizeLimit(Decoder* decoder) {
    DCHECK_GE(__ cache_state()->stack_height(), __ num_locals());
    int stack_height = __ cache_state()->stack_height() - __ num_locals();
    if (stack_height > LiftoffAssembler::kMaxValueStackHeight) {
      unsupported(decoder, "value stack grows too large");
    }
  }

  void StartFunction(Decoder* decoder) {
    int num_locals = decoder->NumLocals();
    __ set_num_locals(num_locals);
    for (int i = 0; i < num_locals; ++i) {
      __ set_local_type(i, decoder->GetLocalType(i));
    }
  }

  void ProcessParameter(uint32_t param_idx, uint32_t input_location) {
    ValueType type = __ local_type(param_idx);
    RegClass rc = reg_class_for(type);
    compiler::LinkageLocation param_loc =
        call_desc_->GetInputLocation(input_location);
    if (param_loc.IsRegister()) {
      DCHECK(!param_loc.IsAnyRegister());
      int reg_code = param_loc.AsRegister();
      LiftoffRegister reg =
          rc == kGpReg ? LiftoffRegister(Register::from_code(reg_code))
                       : LiftoffRegister(DoubleRegister::from_code(reg_code));
      LiftoffRegList cache_regs =
          rc == kGpReg ? kGpCacheRegList : kFpCacheRegList;
      if (cache_regs.has(reg)) {
        // This is a cache register, just use it.
        __ PushRegister(type, reg);
        return;
      }
      // Move to a cache register.
      LiftoffRegister cache_reg = __ GetUnusedRegister(rc);
      __ Move(cache_reg, reg);
      __ PushRegister(type, reg);
      return;
    }
    if (param_loc.IsCallerFrameSlot()) {
      LiftoffRegister tmp_reg = __ GetUnusedRegister(rc);
      __ LoadCallerFrameSlot(tmp_reg, -param_loc.AsCallerFrameSlot());
      __ PushRegister(type, tmp_reg);
      return;
    }
    UNREACHABLE();
  }

  void StartFunctionBody(Decoder* decoder, Control* block) {
    if (!kLiftoffAssemblerImplementedOnThisPlatform) {
      unsupported(decoder, "platform");
      return;
    }
    __ EnterFrame(StackFrame::WASM_COMPILED);
    __ ReserveStackSpace(__ GetTotalFrameSlotCount());
    // Parameter 0 is the wasm context.
    uint32_t num_params =
        static_cast<uint32_t>(call_desc_->ParameterCount()) - 1;
    for (uint32_t i = 0; i < __ num_locals(); ++i) {
      switch (__ local_type(i)) {
        case kWasmI32:
        case kWasmF32:
          // supported.
          break;
        case kWasmI64:
          unsupported(decoder, "i64 param/local");
          return;
        case kWasmF64:
          unsupported(decoder, "f64 param/local");
          return;
        default:
          unsupported(decoder, "exotic param/local");
          return;
      }
    }
    // Input 0 is the call target, the context is at 1.
    constexpr int kContextParameterIndex = 1;
    // Store the context parameter to a special stack slot.
    compiler::LinkageLocation context_loc =
        call_desc_->GetInputLocation(kContextParameterIndex);
    DCHECK(context_loc.IsRegister());
    DCHECK(!context_loc.IsAnyRegister());
    Register context_reg = Register::from_code(context_loc.AsRegister());
    __ SpillContext(context_reg);
    uint32_t param_idx = 0;
    for (; param_idx < num_params; ++param_idx) {
      constexpr int kFirstActualParameterIndex = kContextParameterIndex + 1;
      ProcessParameter(param_idx, param_idx + kFirstActualParameterIndex);
    }
    // Set to a gp register, to mark this uninitialized.
    LiftoffRegister zero_double_reg(Register::from_code<0>());
    DCHECK(zero_double_reg.is_gp());
    for (; param_idx < __ num_locals(); ++param_idx) {
      ValueType type = decoder->GetLocalType(param_idx);
      switch (type) {
        case kWasmI32:
          __ cache_state()->stack_state.emplace_back(kWasmI32, uint32_t{0});
          break;
        case kWasmF32:
          if (zero_double_reg.is_gp()) {
            // Note: This might spill one of the registers used to hold
            // parameters.
            zero_double_reg = __ GetUnusedRegister(kFpReg);
            __ LoadConstant(zero_double_reg, WasmValue(0.f));
          }
          __ PushRegister(kWasmF32, zero_double_reg);
          break;
        default:
          UNIMPLEMENTED();
      }
    }
    block->label_state.stack_base = __ num_locals();
    DCHECK_EQ(__ num_locals(), param_idx);
    DCHECK_EQ(__ num_locals(), __ cache_state()->stack_height());
    CheckStackSizeLimit(decoder);
  }

  void FinishFunction(Decoder* decoder) {
    safepoint_table_builder_.Emit(asm_, __ GetTotalFrameSlotCount());
  }

  void OnFirstError(Decoder* decoder) {
    ok_ = false;
    BindUnboundLabels(decoder);
  }

  void Block(Decoder* decoder, Control* new_block) {
    // Note: This is called for blocks and loops.
    DCHECK_EQ(new_block, decoder->control_at(0));

    TraceCacheState(decoder);

    new_block->label_state.stack_base = __ cache_state()->stack_height();

    if (new_block->is_loop()) {
      // Before entering a loop, spill all locals to the stack, in order to free
      // the cache registers, and to avoid unnecessarily reloading stack values
      // into registers at branches.
      // TODO(clemensh): Come up with a better strategy here, involving
      // pre-analysis of the function.
      __ SpillLocals();

      // Loop labels bind at the beginning of the block, block labels at the
      // end.
      __ bind(new_block->label.get());

      new_block->label_state.Split(*__ cache_state());
    }
  }

  void Loop(Decoder* decoder, Control* block) { Block(decoder, block); }

  void Try(Decoder* decoder, Control* block) { unsupported(decoder, "try"); }
  void If(Decoder* decoder, const Value& cond, Control* if_block) {
    unsupported(decoder, "if");
  }

  void FallThruTo(Decoder* decoder, Control* c) {
    TraceCacheState(decoder);
    if (c->end_merge.reached) {
      __ MergeFullStackWith(c->label_state);
    } else {
      c->label_state.Split(*__ cache_state());
    }
  }

  void PopControl(Decoder* decoder, Control* c) {
    if (!c->is_loop() && c->end_merge.reached) {
      __ cache_state()->Steal(c->label_state);
    }
    if (!c->label.get()->is_bound()) {
      __ bind(c->label.get());
    }
  }

  void EndControl(Decoder* decoder, Control* c) {}

  void UnOp(Decoder* decoder, WasmOpcode opcode, FunctionSig*,
            const Value& value, Value* result) {
    unsupported(decoder, WasmOpcodes::OpcodeName(opcode));
  }

  void I32BinOp(void (LiftoffAssembler::*emit_fn)(Register, Register,
                                                  Register)) {
    LiftoffRegList pinned_regs;
    LiftoffRegister target_reg =
        pinned_regs.set(__ GetBinaryOpTargetRegister(kGpReg));
    LiftoffRegister rhs_reg =
        pinned_regs.set(__ PopToRegister(kGpReg, pinned_regs));
    LiftoffRegister lhs_reg = __ PopToRegister(kGpReg, pinned_regs);
    (asm_->*emit_fn)(target_reg.gp(), lhs_reg.gp(), rhs_reg.gp());
    __ PushRegister(kWasmI32, target_reg);
  }

  void F32BinOp(void (LiftoffAssembler::*emit_fn)(DoubleRegister,
                                                  DoubleRegister,
                                                  DoubleRegister)) {
    LiftoffRegList pinned_regs;
    LiftoffRegister target_reg =
        pinned_regs.set(__ GetBinaryOpTargetRegister(kFpReg));
    LiftoffRegister rhs_reg =
        pinned_regs.set(__ PopToRegister(kFpReg, pinned_regs));
    LiftoffRegister lhs_reg = __ PopToRegister(kFpReg, pinned_regs);
    (asm_->*emit_fn)(target_reg.fp(), lhs_reg.fp(), rhs_reg.fp());
    __ PushRegister(kWasmF32, target_reg);
  }

  void BinOp(Decoder* decoder, WasmOpcode opcode, FunctionSig*,
             const Value& lhs, const Value& rhs, Value* result) {
    TraceCacheState(decoder);
#define CASE_BINOP(opcode, type, fn) \
  case WasmOpcode::kExpr##opcode:    \
    return type##BinOp(&LiftoffAssembler::emit_##fn);
    switch (opcode) {
      CASE_BINOP(I32Add, I32, i32_add)
      CASE_BINOP(I32Sub, I32, i32_sub)
      CASE_BINOP(I32Mul, I32, i32_mul)
      CASE_BINOP(I32And, I32, i32_and)
      CASE_BINOP(I32Ior, I32, i32_or)
      CASE_BINOP(I32Xor, I32, i32_xor)
      CASE_BINOP(F32Add, F32, f32_add)
      CASE_BINOP(F32Sub, F32, f32_sub)
      CASE_BINOP(F32Mul, F32, f32_mul)
      default:
        return unsupported(decoder, WasmOpcodes::OpcodeName(opcode));
    }
#undef CASE_BINOP
  }

  void I32Const(Decoder* decoder, Value* result, int32_t value) {
    TraceCacheState(decoder);
    __ cache_state()->stack_state.emplace_back(kWasmI32, value);
    CheckStackSizeLimit(decoder);
  }

  void I64Const(Decoder* decoder, Value* result, int64_t value) {
    unsupported(decoder, "i64.const");
  }

  void F32Const(Decoder* decoder, Value* result, float value) {
    LiftoffRegister reg = __ GetUnusedRegister(kFpReg);
    __ LoadConstant(reg, WasmValue(value));
    __ PushRegister(kWasmF32, reg);
    CheckStackSizeLimit(decoder);
  }

  void F64Const(Decoder* decoder, Value* result, double value) {
    unsupported(decoder, "f64.const");
  }

  void Drop(Decoder* decoder, const Value& value) {
    TraceCacheState(decoder);
    __ DropStackSlot(&__ cache_state()->stack_state.back());
    __ cache_state()->stack_state.pop_back();
  }

  void DoReturn(Decoder* decoder, Vector<Value> values, bool implicit) {
    if (implicit) {
      DCHECK_EQ(1, decoder->control_depth());
      Control* func_block = decoder->control_at(0);
      __ bind(func_block->label.get());
      __ cache_state()->Steal(func_block->label_state);
    }
    if (!values.is_empty()) {
      if (values.size() > 1) return unsupported(decoder, "multi-return");
      RegClass rc = reg_class_for(values[0].type);
      LiftoffRegister reg = __ PopToRegister(rc);
      __ MoveToReturnRegister(reg);
    }
    __ LeaveFrame(StackFrame::WASM_COMPILED);
    __ Ret();
  }

  void GetLocal(Decoder* decoder, Value* result,
                const LocalIndexOperand<validate>& operand) {
    auto& slot = __ cache_state()->stack_state[operand.index];
    DCHECK_EQ(slot.type(), operand.type);
    switch (slot.loc()) {
      case kRegister:
        __ PushRegister(slot.type(), slot.reg());
        break;
      case kConstant:
        __ cache_state()->stack_state.emplace_back(operand.type,
                                                   slot.i32_const());
        break;
      case kStack: {
        auto rc = reg_class_for(operand.type);
        LiftoffRegister reg = __ GetUnusedRegister(rc);
        __ Fill(reg, operand.index);
        __ PushRegister(slot.type(), reg);
        break;
      }
    }
    CheckStackSizeLimit(decoder);
  }

  void SetLocalFromStackSlot(LiftoffAssembler::VarState& dst_slot,
                             uint32_t local_index) {
    auto& state = *__ cache_state();
    if (dst_slot.is_reg()) {
      LiftoffRegister slot_reg = dst_slot.reg();
      if (state.get_use_count(slot_reg) == 1) {
        __ Fill(dst_slot.reg(), state.stack_height() - 1);
        return;
      }
      state.dec_used(slot_reg);
    }
    ValueType type = dst_slot.type();
    DCHECK_EQ(type, __ local_type(local_index));
    RegClass rc = reg_class_for(type);
    LiftoffRegister dst_reg = __ GetUnusedRegister(rc);
    __ Fill(dst_reg, __ cache_state()->stack_height() - 1);
    dst_slot = LiftoffAssembler::VarState(type, dst_reg);
    __ cache_state()->inc_used(dst_reg);
  }

  void SetLocal(uint32_t local_index, bool is_tee) {
    auto& state = *__ cache_state();
    auto& source_slot = state.stack_state.back();
    auto& target_slot = state.stack_state[local_index];
    switch (source_slot.loc()) {
      case kRegister:
        __ DropStackSlot(&target_slot);
        target_slot = source_slot;
        if (is_tee) state.inc_used(target_slot.reg());
        break;
      case kConstant:
        __ DropStackSlot(&target_slot);
        target_slot = source_slot;
        break;
      case kStack:
        SetLocalFromStackSlot(target_slot, local_index);
        break;
    }
    if (!is_tee) __ cache_state()->stack_state.pop_back();
  }

  void SetLocal(Decoder* decoder, const Value& value,
                const LocalIndexOperand<validate>& operand) {
    SetLocal(operand.index, false);
  }

  void TeeLocal(Decoder* decoder, const Value& value, Value* result,
                const LocalIndexOperand<validate>& operand) {
    SetLocal(operand.index, true);
  }

  void GetGlobal(Decoder* decoder, Value* result,
                 const GlobalIndexOperand<validate>& operand) {
    const auto* global = &env_->module->globals[operand.index];
    if (global->type != kWasmI32 && global->type != kWasmI64)
      return unsupported(decoder, "non-int global");
    LiftoffRegList pinned;
    Register addr = pinned.set(__ GetUnusedRegister(kGpReg)).gp();
    __ LoadFromContext(addr, offsetof(WasmContext, globals_start),
                       kPointerSize);
    LiftoffRegister value =
        pinned.set(__ GetUnusedRegister(reg_class_for(global->type), pinned));
    int size = 1 << ElementSizeLog2Of(global->type);
    if (size > kPointerSize)
      return unsupported(decoder, "global > kPointerSize");
    __ Load(value, addr, global->offset, size, pinned);
    __ PushRegister(global->type, value);
    CheckStackSizeLimit(decoder);
  }

  void SetGlobal(Decoder* decoder, const Value& value,
                 const GlobalIndexOperand<validate>& operand) {
    auto* global = &env_->module->globals[operand.index];
    if (global->type != kWasmI32) return unsupported(decoder, "non-i32 global");
    LiftoffRegList pinned;
    Register addr = pinned.set(__ GetUnusedRegister(kGpReg)).gp();
    __ LoadFromContext(addr, offsetof(WasmContext, globals_start),
                       kPointerSize);
    LiftoffRegister reg =
        pinned.set(__ PopToRegister(reg_class_for(global->type), pinned));
    int size = 1 << ElementSizeLog2Of(global->type);
    __ Store(addr, global->offset, reg, size, pinned);
  }

  void Unreachable(Decoder* decoder) { unsupported(decoder, "unreachable"); }

  void Select(Decoder* decoder, const Value& cond, const Value& fval,
              const Value& tval, Value* result) {
    unsupported(decoder, "select");
  }

  void Br(Control* target) {
    if (!target->br_merge()->reached) {
      target->label_state.InitMerge(*__ cache_state(), __ num_locals(),
                                    target->br_merge()->arity);
    }
    __ MergeStackWith(target->label_state, target->br_merge()->arity);
    __ jmp(target->label.get());
  }

  void Br(Decoder* decoder, Control* target) {
    TraceCacheState(decoder);
    Br(target);
  }

  void BrIf(Decoder* decoder, const Value& cond, Control* target) {
    TraceCacheState(decoder);
    Label cont_false;
    Register value = __ PopToRegister(kGpReg).gp();
    __ JumpIfZero(value, &cont_false);

    Br(target);
    __ bind(&cont_false);
  }

  void BrTable(Decoder* decoder, const BranchTableOperand<validate>& operand,
               const Value& key) {
    unsupported(decoder, "br_table");
  }
  void Else(Decoder* decoder, Control* if_block) {
    unsupported(decoder, "else");
  }
  void LoadMem(Decoder* decoder, LoadType type,
               const MemoryAccessOperand<validate>& operand, const Value& index,
               Value* result) {
    unsupported(decoder, "memory load");
  }
  void StoreMem(Decoder* decoder, StoreType type,
                const MemoryAccessOperand<validate>& operand,
                const Value& index, const Value& value) {
    unsupported(decoder, "memory store");
  }
  void CurrentMemoryPages(Decoder* decoder, Value* result) {
    unsupported(decoder, "current_memory");
  }
  void GrowMemory(Decoder* decoder, const Value& value, Value* result) {
    unsupported(decoder, "grow_memory");
  }
  void CallDirect(Decoder* decoder,
                  const CallFunctionOperand<validate>& operand,
                  const Value args[], Value returns[]) {
    unsupported(decoder, "call");
  }
  void CallIndirect(Decoder* decoder, const Value& index,
                    const CallIndirectOperand<validate>& operand,
                    const Value args[], Value returns[]) {
    unsupported(decoder, "call_indirect");
  }
  void SimdOp(Decoder* decoder, WasmOpcode opcode, Vector<Value> args,
              Value* result) {
    unsupported(decoder, "simd");
  }
  void SimdLaneOp(Decoder* decoder, WasmOpcode opcode,
                  const SimdLaneOperand<validate>& operand,
                  const Vector<Value> inputs, Value* result) {
    unsupported(decoder, "simd");
  }
  void SimdShiftOp(Decoder* decoder, WasmOpcode opcode,
                   const SimdShiftOperand<validate>& operand,
                   const Value& input, Value* result) {
    unsupported(decoder, "simd");
  }
  void Simd8x16ShuffleOp(Decoder* decoder,
                         const Simd8x16ShuffleOperand<validate>& operand,
                         const Value& input0, const Value& input1,
                         Value* result) {
    unsupported(decoder, "simd");
  }
  void Throw(Decoder* decoder, const ExceptionIndexOperand<validate>&,
             Control* block, const Vector<Value>& args) {
    unsupported(decoder, "throw");
  }
  void CatchException(Decoder* decoder,
                      const ExceptionIndexOperand<validate>& operand,
                      Control* block, Vector<Value> caught_values) {
    unsupported(decoder, "catch");
  }
  void AtomicOp(Decoder* decoder, WasmOpcode opcode, Vector<Value> args,
                const MemoryAccessOperand<validate>& operand, Value* result) {
    unsupported(decoder, "atomicop");
  }

 private:
  LiftoffAssembler* const asm_;
  compiler::CallDescriptor* const call_desc_;
  compiler::ModuleEnv* const env_;
  bool ok_ = true;
  // Zone used to store information during compilation. The result will be
  // stored independently, such that this zone can die together with the
  // LiftoffCompiler after compilation.
  Zone compilation_zone_;
  SafepointTableBuilder safepoint_table_builder_;

  void TraceCacheState(Decoder* decoder) const {
#ifdef DEBUG
    if (!FLAG_trace_liftoff) return;
    for (int control_depth = decoder->control_depth() - 1; control_depth >= -1;
         --control_depth) {
      LiftoffAssembler::CacheState* cache_state =
          control_depth == -1
              ? asm_->cache_state()
              : &decoder->control_at(control_depth)->label_state;
      int idx = 0;
      for (LiftoffAssembler::VarState& slot : cache_state->stack_state) {
        if (idx++) PrintF("-");
        PrintF("%s:", WasmOpcodes::TypeName(slot.type()));
        switch (slot.loc()) {
          case kStack:
            PrintF("s");
            break;
          case kRegister:
            if (slot.reg().is_gp()) {
              PrintF("gp%d", slot.reg().gp().code());
            } else {
              PrintF("fp%d", slot.reg().fp().code());
            }
            break;
          case kConstant:
            PrintF("c");
            break;
        }
      }
      if (control_depth != -1) PrintF("; ");
    }
    PrintF("\n");
#endif
  }
};

}  // namespace
}  // namespace wasm

bool compiler::WasmCompilationUnit::ExecuteLiftoffCompilation() {
  base::ElapsedTimer compile_timer;
  if (FLAG_trace_wasm_decode_time) {
    compile_timer.Start();
  }

  Zone zone(isolate_->allocator(), "LiftoffCompilationZone");
  const wasm::WasmModule* module = env_ ? env_->module : nullptr;
  auto* call_desc = compiler::GetWasmCallDescriptor(&zone, func_body_.sig);
  wasm::WasmFullDecoder<wasm::Decoder::kValidate, wasm::LiftoffCompiler>
      decoder(&zone, module, func_body_, &liftoff_.asm_, call_desc, env_);
  decoder.Decode();
  if (!decoder.interface().ok()) {
    // Liftoff compilation failed.
    isolate_->counters()->liftoff_unsupported_functions()->Increment();
    return false;
  }
  if (decoder.failed()) return false;  // Validation error

  if (FLAG_trace_wasm_decode_time) {
    double compile_ms = compile_timer.Elapsed().InMillisecondsF();
    PrintF(
        "wasm-compilation liftoff phase 1 ok: %u bytes, %0.3f ms decode and "
        "compile\n",
        static_cast<unsigned>(func_body_.end - func_body_.start), compile_ms);
  }

  // Record the memory cost this unit places on the system until
  // it is finalized.
  memory_cost_ = liftoff_.asm_.pc_offset();
  liftoff_.safepoint_table_offset_ =
      decoder.interface().GetSafepointTableOffset();
  isolate_->counters()->liftoff_compiled_functions()->Increment();
  return true;
}

#undef __
#undef TRACE

}  // namespace internal
}  // namespace v8

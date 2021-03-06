// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/backend/mid-tier-register-allocator.h"

#include "src/base/bits.h"
#include "src/base/logging.h"
#include "src/base/macros.h"
#include "src/base/optional.h"
#include "src/codegen/machine-type.h"
#include "src/codegen/register-configuration.h"
#include "src/codegen/tick-counter.h"
#include "src/common/globals.h"
#include "src/compiler/backend/instruction.h"
#include "src/compiler/linkage.h"
#include "src/logging/counters.h"
#include "src/utils/bit-vector.h"
#include "src/zone/zone-containers.h"

namespace v8 {
namespace internal {
namespace compiler {

class RegisterState;

// BlockState stores details associated with a particular basic block.
class BlockState final {
 public:
  BlockState(int block_count, Zone* zone)
      : dominated_blocks_(block_count, zone) {}

  // Returns a bitvector representing all the basic blocks that are dominated
  // by this basic block.
  BitVector* dominated_blocks() { return &dominated_blocks_; }

 private:
  BitVector dominated_blocks_;
};

MidTierRegisterAllocationData::MidTierRegisterAllocationData(
    const RegisterConfiguration* config, Zone* zone, Frame* frame,
    InstructionSequence* code, TickCounter* tick_counter,
    const char* debug_name)
    : RegisterAllocationData(Type::kMidTier),
      allocation_zone_(zone),
      frame_(frame),
      code_(code),
      debug_name_(debug_name),
      config_(config),
      virtual_register_data_(code->VirtualRegisterCount(), allocation_zone()),
      block_states_(allocation_zone()),
      reference_map_instructions_(allocation_zone()),
      spilled_virtual_registers_(code->VirtualRegisterCount(),
                                 allocation_zone()),
      tick_counter_(tick_counter) {
  int basic_block_count = code->InstructionBlockCount();
  block_states_.reserve(basic_block_count);
  for (int i = 0; i < basic_block_count; i++) {
    block_states_.emplace_back(basic_block_count, allocation_zone());
  }
}

MoveOperands* MidTierRegisterAllocationData::AddGapMove(
    int instr_index, Instruction::GapPosition position,
    const InstructionOperand& from, const InstructionOperand& to) {
  Instruction* instr = code()->InstructionAt(instr_index);
  ParallelMove* moves = instr->GetOrCreateParallelMove(position, code_zone());
  return moves->AddMove(from, to);
}

MoveOperands* MidTierRegisterAllocationData::AddPendingOperandGapMove(
    int instr_index, Instruction::GapPosition position) {
  return AddGapMove(instr_index, position, PendingOperand(), PendingOperand());
}

MachineRepresentation MidTierRegisterAllocationData::RepresentationFor(
    int virtual_register) {
  if (virtual_register == InstructionOperand::kInvalidVirtualRegister) {
    return InstructionSequence::DefaultRepresentation();
  } else {
    DCHECK_LT(virtual_register, code()->VirtualRegisterCount());
    return code()->GetRepresentation(virtual_register);
  }
}

BlockState& MidTierRegisterAllocationData::block_state(RpoNumber rpo_number) {
  return block_states_[rpo_number.ToInt()];
}

const InstructionBlock* MidTierRegisterAllocationData::GetBlock(
    RpoNumber rpo_number) {
  return code()->InstructionBlockAt(rpo_number);
}

const InstructionBlock* MidTierRegisterAllocationData::GetBlock(
    int instr_index) {
  return code()->InstructionAt(instr_index)->block();
}

const BitVector* MidTierRegisterAllocationData::GetBlocksDominatedBy(
    int instr_index) {
  const InstructionBlock* block = GetBlock(instr_index);
  return block_state(block->rpo_number()).dominated_blocks();
}

// RegisterIndex represents a particular register of a given kind (depending
// on the RegisterKind of the allocator).
class RegisterIndex final {
 public:
  RegisterIndex() : index_(kInvalidIndex) {}
  explicit RegisterIndex(int index) : index_(index) {}
  static RegisterIndex Invalid() { return RegisterIndex(); }

  bool is_valid() const { return index_ != kInvalidIndex; }

  int ToInt() const {
    DCHECK(is_valid());
    return index_;
  }
  uintptr_t ToBit(MachineRepresentation rep) const { return 1ull << ToInt(); }

  bool operator==(const RegisterIndex& rhs) const {
    return index_ == rhs.index_;
  }
  bool operator!=(const RegisterIndex& rhs) const {
    return index_ != rhs.index_;
  }

  class Iterator {
   public:
    explicit Iterator(int index) : index_(index) {}

    bool operator!=(const Iterator& rhs) const { return index_ != rhs.index_; }
    void operator++() { index_++; }
    RegisterIndex operator*() const { return RegisterIndex(index_); }

   private:
    int index_;
  };

 private:
  static const int kInvalidIndex = -1;
  int8_t index_;
};

// A Range from [start, end] of instructions, inclusive of start and end.
class Range {
 public:
  Range() : start_(kMaxInt), end_(0) {}
  Range(int start, int end) : start_(start), end_(end) {}

  void AddInstr(int index) {
    start_ = std::min(start_, index);
    end_ = std::max(end_, index);
  }

  void AddRange(const Range& other) {
    start_ = std::min(start_, other.start_);
    end_ = std::max(end_, other.end_);
  }

  // Returns true if index is greater than start and less than or equal to end.
  bool Contains(int index) { return index >= start_ && index <= end_; }

  int start() const { return start_; }
  int end() const { return end_; }

 private:
  int start_;
  int end_;
};

// VirtualRegisterData stores data specific to a particular virtual register,
// and tracks spilled operands for that virtual register.
class VirtualRegisterData final {
 public:
  VirtualRegisterData() = default;

  // Define VirtualRegisterData with the type of output that produces this
  // virtual register.
  void DefineAsUnallocatedOperand(int virtual_register, int instr_index);
  void DefineAsFixedSpillOperand(AllocatedOperand* operand,
                                 int virtual_register, int instr_index);
  void DefineAsConstantOperand(ConstantOperand* operand, int instr_index);
  void DefineAsPhi(int virtual_register, int instr_index);

  // Spill an operand that is assigned to this virtual register.
  void SpillOperand(InstructionOperand* operand, int instr_index,
                    MidTierRegisterAllocationData* data);

  // Emit gap moves to / from the spill slot.
  void EmitGapMoveToInputFromSpillSlot(AllocatedOperand to_operand,
                                       int instr_index,
                                       MidTierRegisterAllocationData* data);
  void EmitGapMoveFromOutputToSpillSlot(AllocatedOperand from_operand,
                                        const InstructionBlock* current_block,
                                        int instr_index,
                                        MidTierRegisterAllocationData* data);
  void EmitGapMoveToSpillSlot(AllocatedOperand from_operand, int instr_index,
                              MidTierRegisterAllocationData* data);

  // Accessors for spill operand, which may still be pending allocation.
  bool HasSpillOperand() const { return spill_operand_ != nullptr; }
  InstructionOperand* spill_operand() const {
    DCHECK(HasSpillOperand());
    return spill_operand_;
  }

  bool HasPendingSpillOperand() const {
    return HasSpillOperand() && spill_operand_->IsPending();
  }
  bool HasAllocatedSpillOperand() const {
    return HasSpillOperand() && spill_operand_->IsAllocated();
  }
  bool HasConstantSpillOperand() const {
    DCHECK_EQ(is_constant(), HasSpillOperand() && spill_operand_->IsConstant());
    return is_constant();
  }
  bool NeedsSpillAtOutput() const;

  // Allocates pending spill operands to the |allocated| spill slot.
  void AllocatePendingSpillOperand(const AllocatedOperand& allocated);

  int vreg() const { return vreg_; }
  int output_instr_index() const { return output_instr_index_; }
  bool is_constant() const { return is_constant_; }

  bool is_phi() const { return is_phi_; }
  void set_is_phi(bool value) { is_phi_ = value; }

  // Represents the range of instructions for which this virtual register needs
  // to be spilled on the stack.
  class SpillRange : public ZoneObject {
   public:
    // Defines a spill range for an output operand.
    SpillRange(int definition_instr_index, MidTierRegisterAllocationData* data)
        : live_range_(definition_instr_index, definition_instr_index),
          live_blocks_(data->GetBlocksDominatedBy(definition_instr_index)) {}

    // Defines a spill range for a Phi variable.
    SpillRange(const InstructionBlock* phi_block,
               MidTierRegisterAllocationData* data)
        : live_range_(phi_block->first_instruction_index(),
                      phi_block->first_instruction_index()),
          live_blocks_(data->GetBlocksDominatedBy(
              phi_block->first_instruction_index())) {
      // For phis, add the gap move instructions in the predecssor blocks to
      // the live range.
      for (RpoNumber pred_rpo : phi_block->predecessors()) {
        const InstructionBlock* block = data->GetBlock(pred_rpo);
        live_range_.AddInstr(block->last_instruction_index());
      }
    }

    bool IsLiveAt(int instr_index, InstructionBlock* block) {
      return live_range_.Contains(instr_index) &&
             live_blocks_->Contains(block->rpo_number().ToInt());
    }

    void ExtendRangeTo(int instr_index) { live_range_.AddInstr(instr_index); }

    Range& live_range() { return live_range_; }

   private:
    Range live_range_;
    const BitVector* live_blocks_;

    DISALLOW_COPY_AND_ASSIGN(SpillRange);
  };

  bool HasSpillRange() const { return spill_range_ != nullptr; }
  SpillRange* spill_range() const {
    DCHECK(HasSpillRange());
    return spill_range_;
  }

 private:
  void Initialize(int virtual_register, InstructionOperand* spill_operand,
                  int instr_index, bool is_phi, bool is_constant);

  void AddPendingSpillOperand(PendingOperand* pending_operand);
  void AddSpillUse(int instr_index, MidTierRegisterAllocationData* data);
  void EnsureSpillRange(MidTierRegisterAllocationData* data);

  InstructionOperand* spill_operand_;
  SpillRange* spill_range_;
  int output_instr_index_;

  int vreg_;
  bool is_phi_ : 1;
  bool is_constant_ : 1;
};

VirtualRegisterData& MidTierRegisterAllocationData::VirtualRegisterDataFor(
    int virtual_register) {
  DCHECK_GE(virtual_register, 0);
  DCHECK_LT(virtual_register, virtual_register_data_.size());
  return virtual_register_data_[virtual_register];
}

void VirtualRegisterData::Initialize(int virtual_register,
                                     InstructionOperand* spill_operand,
                                     int instr_index, bool is_phi,
                                     bool is_constant) {
  vreg_ = virtual_register;
  spill_operand_ = spill_operand;
  spill_range_ = nullptr;
  output_instr_index_ = instr_index;
  is_phi_ = is_phi;
  is_constant_ = is_constant;
}

void VirtualRegisterData::DefineAsConstantOperand(ConstantOperand* operand,
                                                  int instr_index) {
  Initialize(operand->virtual_register(), operand, instr_index, false, true);
}

void VirtualRegisterData::DefineAsFixedSpillOperand(AllocatedOperand* operand,
                                                    int virtual_register,
                                                    int instr_index) {
  Initialize(virtual_register, operand, instr_index, false, false);
}

void VirtualRegisterData::DefineAsUnallocatedOperand(int virtual_register,
                                                     int instr_index) {
  Initialize(virtual_register, nullptr, instr_index, false, false);
}

void VirtualRegisterData::DefineAsPhi(int virtual_register, int instr_index) {
  Initialize(virtual_register, nullptr, instr_index, true, false);
}

void VirtualRegisterData::EnsureSpillRange(
    MidTierRegisterAllocationData* data) {
  DCHECK(!is_constant());
  if (HasSpillRange()) return;

  if (is_phi()) {
    // Define a spill slot that is defined for the phi's range.
    const InstructionBlock* definition_block =
        data->code()->InstructionAt(output_instr_index_)->block();
    spill_range_ =
        data->allocation_zone()->New<SpillRange>(definition_block, data);
  } else {
    // The spill slot will be defined after the instruction that outputs it.
    spill_range_ =
        data->allocation_zone()->New<SpillRange>(output_instr_index_ + 1, data);
  }
  data->spilled_virtual_registers().Add(vreg());
}

void VirtualRegisterData::AddSpillUse(int instr_index,
                                      MidTierRegisterAllocationData* data) {
  if (is_constant()) return;
  EnsureSpillRange(data);
  spill_range_->ExtendRangeTo(instr_index);
}

void VirtualRegisterData::SpillOperand(InstructionOperand* operand,
                                       int instr_index,
                                       MidTierRegisterAllocationData* data) {
  AddSpillUse(instr_index, data);
  if (HasAllocatedSpillOperand() || HasConstantSpillOperand()) {
    InstructionOperand::ReplaceWith(operand, spill_operand());
  } else {
    PendingOperand pending_op;
    InstructionOperand::ReplaceWith(operand, &pending_op);
    AddPendingSpillOperand(PendingOperand::cast(operand));
  }
}

bool VirtualRegisterData::NeedsSpillAtOutput() const {
  return HasSpillOperand() && !is_constant();
}

void VirtualRegisterData::EmitGapMoveToInputFromSpillSlot(
    AllocatedOperand to_operand, int instr_index,
    MidTierRegisterAllocationData* data) {
  AddSpillUse(instr_index, data);
  DCHECK(!to_operand.IsPending());
  if (HasAllocatedSpillOperand() || HasConstantSpillOperand()) {
    data->AddGapMove(instr_index, Instruction::END, *spill_operand(),
                     to_operand);
  } else {
    MoveOperands* move_ops =
        data->AddPendingOperandGapMove(instr_index, Instruction::END);
    AddPendingSpillOperand(PendingOperand::cast(&move_ops->source()));
    InstructionOperand::ReplaceWith(&move_ops->destination(), &to_operand);
  }
}

void VirtualRegisterData::EmitGapMoveToSpillSlot(
    AllocatedOperand from_operand, int instr_index,
    MidTierRegisterAllocationData* data) {
  AddSpillUse(instr_index, data);
  if (HasAllocatedSpillOperand() || HasConstantSpillOperand()) {
    data->AddGapMove(instr_index, Instruction::START, from_operand,
                     *spill_operand());
  } else {
    MoveOperands* move_ops =
        data->AddPendingOperandGapMove(instr_index, Instruction::START);
    InstructionOperand::ReplaceWith(&move_ops->source(), &from_operand);
    AddPendingSpillOperand(PendingOperand::cast(&move_ops->destination()));
  }
}

void VirtualRegisterData::EmitGapMoveFromOutputToSpillSlot(
    AllocatedOperand from_operand, const InstructionBlock* current_block,
    int instr_index, MidTierRegisterAllocationData* data) {
  DCHECK_EQ(data->GetBlock(instr_index), current_block);
  if (instr_index == current_block->last_instruction_index()) {
    // Add gap move to the first instruction of every successor block.
    for (const RpoNumber& succ : current_block->successors()) {
      const InstructionBlock* successor = data->GetBlock(succ);
      DCHECK_EQ(1, successor->PredecessorCount());
      EmitGapMoveToSpillSlot(from_operand, successor->first_instruction_index(),
                             data);
    }
  } else {
    // Add gap move to the next instruction.
    EmitGapMoveToSpillSlot(from_operand, instr_index + 1, data);
  }
}

void VirtualRegisterData::AddPendingSpillOperand(PendingOperand* pending_op) {
  DCHECK(HasSpillRange());
  DCHECK_NULL(pending_op->next());
  if (HasSpillOperand()) {
    pending_op->set_next(PendingOperand::cast(spill_operand()));
  }
  spill_operand_ = pending_op;
}

void VirtualRegisterData::AllocatePendingSpillOperand(
    const AllocatedOperand& allocated) {
  DCHECK(!HasAllocatedSpillOperand() && !HasConstantSpillOperand());
  PendingOperand* current = PendingOperand::cast(spill_operand_);
  while (current) {
    PendingOperand* next = current->next();
    InstructionOperand::ReplaceWith(current, &allocated);
    current = next;
  }
}

// RegisterState represents the state of the |kind| registers at a particular
// point in program execution. The RegisterState can be cloned or merged with
// other RegisterStates to model branches and merges in program control flow.
class RegisterState final : public ZoneObject {
 public:
  static RegisterState* New(RegisterKind kind, int num_allocatable_registers,
                            Zone* zone) {
    return zone->New<RegisterState>(kind, num_allocatable_registers, zone);
  }

  RegisterState(RegisterKind kind, int num_allocatable_registers, Zone* zone);
  RegisterState(const RegisterState& other) V8_NOEXCEPT;

  bool IsAllocated(RegisterIndex reg);
  int VirtualRegisterForRegister(RegisterIndex reg);

  // Commit the |reg| with the |allocated| operand.
  void Commit(RegisterIndex reg, AllocatedOperand allocated,
              InstructionOperand* operand, MidTierRegisterAllocationData* data);

  // Spill the contents of |reg| using the |allocated| operand to commit the
  // spill gap move.
  void Spill(RegisterIndex reg, AllocatedOperand allocated,
             MidTierRegisterAllocationData* data);

  // Allocate |reg| to |virtual_register| for the instruction at |instr_index|.
  // If the register is later spilled, a gap move will be added immediately
  // before |instr_index| to move |virtual_register| into this register.
  void AllocateUse(RegisterIndex reg, int virtual_register,
                   InstructionOperand* operand, int instr_index,
                   MidTierRegisterAllocationData* data);

  // Allocate |reg| as a pending use of |virtual_register| for |operand| in the
  // instruction at |instr_index|. If |virtual_register| later gets committed to
  // this register, then |operand| will be too, otherwise |operand| will be
  // replaced with |virtual_register|'s spill operand.
  void AllocatePendingUse(RegisterIndex reg, int virtual_register,
                          InstructionOperand* operand, int instr_index);

  // Mark that the register is holding a phi operand that is yet to be allocated
  // by the source block in the gap just before the last instruction in the
  // source block.
  void UseForPhiGapMove(RegisterIndex reg);
  bool IsPhiGapMove(RegisterIndex reg);

  // Returns true if |reg| only has pending uses allocated to it.
  bool HasPendingUsesOnly(RegisterIndex reg);

  // Enable range-based for on allocatable register indices.
  RegisterIndex::Iterator begin() const { return RegisterIndex::Iterator(0); }
  RegisterIndex::Iterator end() const {
    return RegisterIndex::Iterator(num_allocatable_registers());
  }

 private:
  // Represents a particular register and details of what virtual_register it is
  // currently holding, and how it should be updated if committed or spilled.
  class Register final : public ZoneObject {
   public:
    Register();
    void Reset();

    // Operations for committing, spilling and allocating uses of the register.
    void Commit(AllocatedOperand allocated_operand);
    void Spill(AllocatedOperand allocated_op,
               MidTierRegisterAllocationData* data);
    void Use(int virtual_register, int instr_index);
    void PendingUse(InstructionOperand* operand, int virtual_register,
                    int instr_index);

    bool is_allocated() const {
      return virtual_register_ != InstructionOperand::kInvalidVirtualRegister;
    }

    // The current virtual register held by this register.
    int virtual_register() const { return virtual_register_; }

    // The instruction index for the last use of the current in-progress
    // allocation of this register in the instruction stream. Used both
    // as the instruction too add a gap move if |needs_gap_move_on_spill| and
    // the intruction which the virtual register's spill range should be
    // extended too if the register is spilled.
    int last_use_instr_index() const { return last_use_instr_index_; }

    // Returns true if a gap move should be added if the register is spilled.
    bool needs_gap_move_on_spill() const { return needs_gap_move_on_spill_; }

    // Returns a threaded list of the operands that have pending uses of this
    // register and will be resolved either to the register, or a spill slot
    // depending on whether this register is spilled or committed.
    PendingOperand* pending_uses() const { return pending_uses_; }

   private:
    void SpillPendingUses(MidTierRegisterAllocationData* data);

    bool needs_gap_move_on_spill_;
    int last_use_instr_index_;
    int virtual_register_;
    PendingOperand* pending_uses_;
  };

  void ResetDataFor(RegisterIndex reg);

  bool HasRegisterData(RegisterIndex reg);
  void EnsureRegisterData(RegisterIndex reg);

  int num_allocatable_registers() const {
    return static_cast<int>(register_data_.size());
  }
  Register& reg_data(RegisterIndex reg);
  Zone* zone() const { return zone_; }

  ZoneVector<Register*> register_data_;
  Zone* zone_;
};

RegisterState::Register::Register() { Reset(); }

void RegisterState::Register::Reset() {
  needs_gap_move_on_spill_ = false;
  last_use_instr_index_ = -1;
  virtual_register_ = InstructionOperand::kInvalidVirtualRegister;
  pending_uses_ = nullptr;
}

void RegisterState::Register::Use(int virtual_register, int instr_index) {
  // A register can have many pending uses, but should only ever have a single
  // non-pending use, since any subsiquent use will commit the preceeding use
  // first.
  DCHECK(!is_allocated());
  needs_gap_move_on_spill_ = true;
  virtual_register_ = virtual_register;
  last_use_instr_index_ = instr_index;
}

void RegisterState::Register::PendingUse(InstructionOperand* operand,
                                         int virtual_register,
                                         int instr_index) {
  if (!is_allocated()) {
    virtual_register_ = virtual_register;
    last_use_instr_index_ = instr_index;
  }
  DCHECK_EQ(virtual_register_, virtual_register);
  DCHECK_GE(last_use_instr_index_, instr_index);

  PendingOperand pending_op(pending_uses());
  InstructionOperand::ReplaceWith(operand, &pending_op);
  pending_uses_ = PendingOperand::cast(operand);
}

void RegisterState::Register::Commit(AllocatedOperand allocated_op) {
  DCHECK(is_allocated());

  // Allocate all pending uses to |allocated_op|.
  PendingOperand* pending_use = pending_uses();
  while (pending_use) {
    PendingOperand* next = pending_use->next();
    InstructionOperand::ReplaceWith(pending_use, &allocated_op);
    pending_use = next;
  }
  pending_uses_ = nullptr;
}

void RegisterState::Register::Spill(AllocatedOperand allocated_op,
                                    MidTierRegisterAllocationData* data) {
  if (needs_gap_move_on_spill()) {
    VirtualRegisterData& vreg_data =
        data->VirtualRegisterDataFor(virtual_register());
    vreg_data.EmitGapMoveToInputFromSpillSlot(allocated_op,
                                              last_use_instr_index(), data);
  }
  SpillPendingUses(data);
  virtual_register_ = InstructionOperand::kInvalidVirtualRegister;
}

void RegisterState::Register::SpillPendingUses(
    MidTierRegisterAllocationData* data) {
  VirtualRegisterData& vreg_data =
      data->VirtualRegisterDataFor(virtual_register());
  PendingOperand* pending_use = pending_uses();
  while (pending_use) {
    // Spill all the pending operands associated with this register.
    PendingOperand* next = pending_use->next();
    vreg_data.SpillOperand(pending_use, last_use_instr_index(), data);
    pending_use = next;
  }
  pending_uses_ = nullptr;
}

RegisterState::RegisterState(RegisterKind kind, int num_allocatable_registers,
                             Zone* zone)
    : register_data_(num_allocatable_registers, zone), zone_(zone) {}

RegisterState::RegisterState(const RegisterState& other) V8_NOEXCEPT
    : register_data_(other.register_data_.begin(), other.register_data_.end(),
                     other.zone_),
      zone_(other.zone_) {}

int RegisterState::VirtualRegisterForRegister(RegisterIndex reg) {
  if (IsAllocated(reg)) {
    return reg_data(reg).virtual_register();
  } else {
    return InstructionOperand::kInvalidVirtualRegister;
  }
}

void RegisterState::Commit(RegisterIndex reg, AllocatedOperand allocated,
                           InstructionOperand* operand,
                           MidTierRegisterAllocationData* data) {
  InstructionOperand::ReplaceWith(operand, &allocated);
  if (IsAllocated(reg)) {
    reg_data(reg).Commit(allocated);
    ResetDataFor(reg);
  }
}

void RegisterState::Spill(RegisterIndex reg, AllocatedOperand allocated,
                          MidTierRegisterAllocationData* data) {
  DCHECK(IsAllocated(reg));
  reg_data(reg).Spill(allocated, data);
  ResetDataFor(reg);
}

void RegisterState::AllocateUse(RegisterIndex reg, int virtual_register,
                                InstructionOperand* operand, int instr_index,
                                MidTierRegisterAllocationData* data) {
  EnsureRegisterData(reg);
  reg_data(reg).Use(virtual_register, instr_index);
}

void RegisterState::AllocatePendingUse(RegisterIndex reg, int virtual_register,
                                       InstructionOperand* operand,
                                       int instr_index) {
  EnsureRegisterData(reg);
  reg_data(reg).PendingUse(operand, virtual_register, instr_index);
}

RegisterState::Register& RegisterState::reg_data(RegisterIndex reg) {
  DCHECK(HasRegisterData(reg));
  return *register_data_[reg.ToInt()];
}

bool RegisterState::IsAllocated(RegisterIndex reg) {
  return HasRegisterData(reg) && reg_data(reg).is_allocated();
}

bool RegisterState::HasPendingUsesOnly(RegisterIndex reg) {
  DCHECK(IsAllocated(reg));
  return !reg_data(reg).needs_gap_move_on_spill();
}

void RegisterState::ResetDataFor(RegisterIndex reg) {
  DCHECK(HasRegisterData(reg));
  reg_data(reg).Reset();
}

bool RegisterState::HasRegisterData(RegisterIndex reg) {
  DCHECK_LT(reg.ToInt(), register_data_.size());
  return register_data_[reg.ToInt()] != nullptr;
}

void RegisterState::EnsureRegisterData(RegisterIndex reg) {
  if (!HasRegisterData(reg)) {
    register_data_[reg.ToInt()] = zone()->New<RegisterState::Register>();
  }
}

// A SinglePassRegisterAllocator is a fast register allocator that does a single
// pass through the instruction stream without performing any live-range
// analysis beforehand. It deals with a single RegisterKind, either general or
// double registers, with the MidTierRegisterAllocator choosing the correct
// SinglePassRegisterAllocator based on a values representation.
class SinglePassRegisterAllocator final {
 public:
  SinglePassRegisterAllocator(RegisterKind kind,
                              MidTierRegisterAllocationData* data);

  // Convert to / from a register code and a register index.
  RegisterIndex FromRegCode(int reg_code, MachineRepresentation rep) const;
  int ToRegCode(RegisterIndex index, MachineRepresentation rep) const;

  // Allocation routines used to allocate a particular operand to either a
  // register or a spill slot.
  void AllocateConstantOutput(ConstantOperand* operand);
  void AllocateOutput(UnallocatedOperand* operand, int instr_index);
  void AllocateInput(UnallocatedOperand* operand, int instr_index);
  void AllocateSameInputOutput(UnallocatedOperand* output,
                               UnallocatedOperand* input, int instr_index);
  void AllocateGapMoveInput(UnallocatedOperand* operand, int instr_index);
  void AllocateTemp(UnallocatedOperand* operand, int instr_index);

  // Reserve any fixed registers for the operands on an instruction before doing
  // allocation on the operands.
  void ReserveFixedInputRegister(const UnallocatedOperand* operand,
                                 int instr_index);
  void ReserveFixedTempRegister(const UnallocatedOperand* operand,
                                int instr_index);
  void ReserveFixedOutputRegister(const UnallocatedOperand* operand,
                                  int instr_index);

  // Spills all registers that are currently holding data, for example, due to
  // an instruction that clobbers all registers.
  void SpillAllRegisters();

  // Inform the allocator that we are starting / ending a block or ending
  // allocation for the current instruction.
  void StartBlock(const InstructionBlock* block);
  void EndBlock(const InstructionBlock* block);
  void EndInstruction();

  RegisterKind kind() const { return kind_; }
  BitVector* assigned_registers() const { return assigned_registers_; }

 private:
  enum class UsePosition {
    // Operand used at start of instruction.
    kStart,
    // Operand used at end of instruction.
    kEnd,
    // Operand is used at both the start and end of instruction.
    kAll,
    // Operand is not used in the instruction (used when initializing register
    // state on block entry).
    kNone,
  };

  // The allocator is initialized without any RegisterState by default to avoid
  // having to allocate per-block allocator state for functions that don't
  // allocate registers of a particular type. All allocation functions should
  // call EnsureRegisterState to allocate a RegisterState if necessary.
  void EnsureRegisterState();

  // Returns true if |virtual_register| is defined after use position |pos| at
  // |instr_index|.
  bool DefinedAfter(int virtual_register, int instr_index, UsePosition pos);

  // Allocate |reg| to |virtual_register| for |operand| of the instruction at
  // |instr_index|. The register will be reserved for this use for the specified
  // |pos| use position.
  void AllocateUse(RegisterIndex reg, int virtual_register,
                   InstructionOperand* operand, int instr_index,
                   UsePosition pos);

  // Allocate |reg| to |virtual_register| as a pending use (i.e., only if the
  // register is not subsequently spilled) for |operand| of the instruction at
  // |instr_index|.
  void AllocatePendingUse(RegisterIndex reg, int virtual_register,
                          InstructionOperand* operand, int instr_index);

  // Allocate |operand| to |reg| and add a gap move to move |virtual_register|
  // to this register for the instruction at |instr_index|. |reg| will be
  // reserved for this use for the specified |pos| use position.
  void AllocateUseWithMove(RegisterIndex reg, int virtual_register,
                           UnallocatedOperand* operand, int instr_index,
                           UsePosition pos);

  void CommitRegister(RegisterIndex reg, int virtual_register,
                      InstructionOperand* operand, UsePosition pos);
  void SpillRegister(RegisterIndex reg);
  void SpillRegisterForVirtualRegister(int virtual_register);

  // Returns an AllocatedOperand corresponding to the use of |reg| for
  // |virtual_register|.
  AllocatedOperand AllocatedOperandForReg(RegisterIndex reg,
                                          int virtual_register);

  void ReserveFixedRegister(const UnallocatedOperand* operand, int instr_index,
                            UsePosition pos);
  RegisterIndex AllocateOutput(UnallocatedOperand* operand, int instr_index,
                               UsePosition pos);
  void EmitGapMoveFromOutput(InstructionOperand from, InstructionOperand to,
                             int instr_index);

  // Helper functions to choose the best register for a given operand.
  V8_INLINE RegisterIndex
  ChooseRegisterFor(VirtualRegisterData& virtual_register, UsePosition pos,
                    bool must_use_register);
  V8_INLINE RegisterIndex ChooseRegisterFor(MachineRepresentation rep,
                                            UsePosition pos,
                                            bool must_use_register);
  V8_INLINE RegisterIndex ChooseFreeRegister(MachineRepresentation rep,
                                             UsePosition pos);
  V8_INLINE RegisterIndex ChooseRegisterToSpill(MachineRepresentation rep,
                                                UsePosition pos);

  // Assign, free and mark use's of |reg| for a |virtual_register| at use
  // position |pos|.
  V8_INLINE void AssignRegister(RegisterIndex reg, int virtual_register,
                                UsePosition pos);
  V8_INLINE void FreeRegister(RegisterIndex reg, int virtual_register);
  V8_INLINE void MarkRegisterUse(RegisterIndex reg, MachineRepresentation rep,
                                 UsePosition pos);
  V8_INLINE uintptr_t InUseBitmap(UsePosition pos);

  // Return the register allocated to |virtual_register|, if any.
  RegisterIndex RegisterForVirtualRegister(int virtual_register);
  // Return the virtual register being held by |reg|, or kInvalidVirtualRegister
  // if |reg| is unallocated.
  int VirtualRegisterForRegister(RegisterIndex reg);

  // Returns true if |reg| is unallocated or holds |virtual_register|.
  bool IsFreeOrSameVirtualRegister(RegisterIndex reg, int virtual_register);
  // Returns true if |virtual_register| is unallocated or is allocated to |reg|.
  bool VirtualRegisterIsUnallocatedOrInReg(int virtual_register,
                                           RegisterIndex reg);

  // Check the consistency of reg->vreg and vreg->reg mappings if a debug build.
  void CheckConsistency();

  bool HasRegisterState() const { return register_state_; }
  RegisterState* register_state() const {
    DCHECK(HasRegisterState());
    return register_state_;
  }

  VirtualRegisterData& VirtualRegisterDataFor(int virtual_register) const {
    return data()->VirtualRegisterDataFor(virtual_register);
  }

  MachineRepresentation RepresentationFor(int virtual_register) const {
    return data()->RepresentationFor(virtual_register);
  }

  int num_allocatable_registers() const { return num_allocatable_registers_; }
  MidTierRegisterAllocationData* data() const { return data_; }

  // Virtual register to register mapping.
  ZoneVector<RegisterIndex> virtual_register_to_reg_;

  // Current register state during allocation.
  RegisterState* register_state_;

  const RegisterKind kind_;
  const int num_allocatable_registers_;
  ZoneVector<RegisterIndex> reg_code_to_index_;
  const int* index_to_reg_code_;
  BitVector* assigned_registers_;

  MidTierRegisterAllocationData* data_;

  static_assert(RegisterConfiguration::kMaxRegisters <= sizeof(uintptr_t) * 8,
                "Maximum registers must fit in uintptr_t bitmap");
  uintptr_t in_use_at_instr_start_bits_;
  uintptr_t in_use_at_instr_end_bits_;
  uintptr_t allocated_registers_bits_;
};

SinglePassRegisterAllocator::SinglePassRegisterAllocator(
    RegisterKind kind, MidTierRegisterAllocationData* data)
    : virtual_register_to_reg_(data->code()->VirtualRegisterCount(),
                               data->allocation_zone()),
      register_state_(nullptr),
      kind_(kind),
      num_allocatable_registers_(
          GetAllocatableRegisterCount(data->config(), kind)),
      reg_code_to_index_(GetRegisterCount(data->config(), kind),
                         data->allocation_zone()),
      index_to_reg_code_(GetAllocatableRegisterCodes(data->config(), kind)),
      assigned_registers_(data->code_zone()->New<BitVector>(
          GetRegisterCount(data->config(), kind), data->code_zone())),
      data_(data),
      in_use_at_instr_start_bits_(0),
      in_use_at_instr_end_bits_(0),
      allocated_registers_bits_(0) {
  for (int i = 0; i < num_allocatable_registers_; i++) {
    int reg_code = index_to_reg_code_[i];
    reg_code_to_index_[reg_code] = RegisterIndex(i);
  }
}

int SinglePassRegisterAllocator::VirtualRegisterForRegister(RegisterIndex reg) {
  return register_state()->VirtualRegisterForRegister(reg);
}

RegisterIndex SinglePassRegisterAllocator::RegisterForVirtualRegister(
    int virtual_register) {
  DCHECK_NE(virtual_register, InstructionOperand::kInvalidVirtualRegister);
  return virtual_register_to_reg_[virtual_register];
}

void SinglePassRegisterAllocator::EndInstruction() {
  in_use_at_instr_end_bits_ = 0;
  in_use_at_instr_start_bits_ = 0;
}

void SinglePassRegisterAllocator::StartBlock(const InstructionBlock* block) {
  DCHECK(!HasRegisterState());
  DCHECK_EQ(in_use_at_instr_start_bits_, 0);
  DCHECK_EQ(in_use_at_instr_end_bits_, 0);
  DCHECK_EQ(allocated_registers_bits_, 0);
}

void SinglePassRegisterAllocator::EndBlock(const InstructionBlock* block) {
  DCHECK_EQ(in_use_at_instr_start_bits_, 0);
  DCHECK_EQ(in_use_at_instr_end_bits_, 0);
  register_state_ = nullptr;
}

void SinglePassRegisterAllocator::CheckConsistency() {
#ifdef DEBUG
  for (int virtual_register = 0;
       virtual_register < data()->code()->VirtualRegisterCount();
       virtual_register++) {
    RegisterIndex reg = RegisterForVirtualRegister(virtual_register);
    if (reg.is_valid()) {
      CHECK_EQ(virtual_register, VirtualRegisterForRegister(reg));
      CHECK(allocated_registers_bits_ &
            reg.ToBit(RepresentationFor(virtual_register)));
    }
  }

  for (RegisterIndex reg : *register_state()) {
    int virtual_register = VirtualRegisterForRegister(reg);
    if (virtual_register != InstructionOperand::kInvalidVirtualRegister) {
      CHECK_EQ(reg, RegisterForVirtualRegister(virtual_register));
      CHECK(allocated_registers_bits_ &
            reg.ToBit(RepresentationFor(virtual_register)));
    }
  }
#endif
}

RegisterIndex SinglePassRegisterAllocator::FromRegCode(
    int reg_code, MachineRepresentation rep) const {
  return RegisterIndex(reg_code_to_index_[reg_code]);
}

int SinglePassRegisterAllocator::ToRegCode(RegisterIndex reg,
                                           MachineRepresentation rep) const {
  return index_to_reg_code_[reg.ToInt()];
}

bool SinglePassRegisterAllocator::VirtualRegisterIsUnallocatedOrInReg(
    int virtual_register, RegisterIndex reg) {
  RegisterIndex existing_reg = RegisterForVirtualRegister(virtual_register);
  return !existing_reg.is_valid() || existing_reg == reg;
}

bool SinglePassRegisterAllocator::IsFreeOrSameVirtualRegister(
    RegisterIndex reg, int virtual_register) {
  int allocated_vreg = VirtualRegisterForRegister(reg);
  return allocated_vreg == InstructionOperand::kInvalidVirtualRegister ||
         allocated_vreg == virtual_register;
}

void SinglePassRegisterAllocator::EmitGapMoveFromOutput(InstructionOperand from,
                                                        InstructionOperand to,
                                                        int instr_index) {
  DCHECK(from.IsAllocated());
  DCHECK(to.IsAllocated());
  const InstructionBlock* block = data()->GetBlock(instr_index);
  if (instr_index == block->last_instruction_index()) {
    // Add gap move to the first instruction of every successor block.
    for (const RpoNumber succ : block->successors()) {
      const InstructionBlock* successor = data()->GetBlock(succ);
      DCHECK_EQ(1, successor->PredecessorCount());
      data()->AddGapMove(successor->first_instruction_index(),
                         Instruction::START, from, to);
    }
  } else {
    data()->AddGapMove(instr_index + 1, Instruction::START, from, to);
  }
}

void SinglePassRegisterAllocator::AssignRegister(RegisterIndex reg,
                                                 int virtual_register,
                                                 UsePosition pos) {
  MachineRepresentation rep = RepresentationFor(virtual_register);
  assigned_registers()->Add(ToRegCode(reg, rep));
  MarkRegisterUse(reg, rep, pos);
  allocated_registers_bits_ |= reg.ToBit(rep);
  if (virtual_register != InstructionOperand::kInvalidVirtualRegister) {
    virtual_register_to_reg_[virtual_register] = reg;
  }
}

void SinglePassRegisterAllocator::MarkRegisterUse(RegisterIndex reg,
                                                  MachineRepresentation rep,
                                                  UsePosition pos) {
  if (pos == UsePosition::kStart || pos == UsePosition::kAll) {
    in_use_at_instr_start_bits_ |= reg.ToBit(rep);
  }
  if (pos == UsePosition::kEnd || pos == UsePosition::kAll) {
    in_use_at_instr_end_bits_ |= reg.ToBit(rep);
  }
}

void SinglePassRegisterAllocator::FreeRegister(RegisterIndex reg,
                                               int virtual_register) {
  allocated_registers_bits_ &= ~reg.ToBit(RepresentationFor(virtual_register));
  if (virtual_register != InstructionOperand::kInvalidVirtualRegister) {
    virtual_register_to_reg_[virtual_register] = RegisterIndex::Invalid();
  }
}

RegisterIndex SinglePassRegisterAllocator::ChooseRegisterFor(
    VirtualRegisterData& virtual_register, UsePosition pos,
    bool must_use_register) {
  // If register is already allocated to the virtual register, use that.
  RegisterIndex reg = RegisterForVirtualRegister(virtual_register.vreg());
  // If we don't need a register, only try to allocate one if the virtual
  // register hasn't yet been spilled, to try to avoid spilling it.
  if (!reg.is_valid() &&
      (must_use_register || !virtual_register.HasSpillOperand())) {
    reg = ChooseRegisterFor(RepresentationFor(virtual_register.vreg()), pos,
                            must_use_register);
  }
  return reg;
}

RegisterIndex SinglePassRegisterAllocator::ChooseRegisterFor(
    MachineRepresentation rep, UsePosition pos, bool must_use_register) {
  RegisterIndex reg = ChooseFreeRegister(rep, pos);
  if (!reg.is_valid() && must_use_register) {
    reg = ChooseRegisterToSpill(rep, pos);
    SpillRegister(reg);
  }
  return reg;
}

uintptr_t SinglePassRegisterAllocator::InUseBitmap(UsePosition pos) {
  switch (pos) {
    case UsePosition::kStart:
      return in_use_at_instr_start_bits_;
    case UsePosition::kEnd:
      return in_use_at_instr_end_bits_;
    case UsePosition::kAll:
      return in_use_at_instr_start_bits_ | in_use_at_instr_end_bits_;
    case UsePosition::kNone:
      UNREACHABLE();
  }
}

RegisterIndex SinglePassRegisterAllocator::ChooseFreeRegister(
    MachineRepresentation rep, UsePosition pos) {
  // Take the first free, non-blocked register, if available.
  // TODO(rmcilroy): Consider a better heuristic.
  uintptr_t allocated_or_in_use = InUseBitmap(pos) | allocated_registers_bits_;

  int reg_index = base::bits::CountTrailingZeros(~allocated_or_in_use);
  if (reg_index >= num_allocatable_registers()) {
    return RegisterIndex::Invalid();
  }
  return RegisterIndex(reg_index);
}

RegisterIndex SinglePassRegisterAllocator::ChooseRegisterToSpill(
    MachineRepresentation rep, UsePosition pos) {
  uintptr_t in_use = InUseBitmap(pos);

  // Choose a register that will need to be spilled. Preferentially choose:
  //  - A register with only pending uses, to avoid having to add a gap move for
  //    a non-pending use.
  //  - A register holding a virtual register that has already been spilled, to
  //    avoid adding a new gap move to spill the virtual register when it is
  //    output.
  //  - Prefer the register holding the virtual register with the earliest
  //    definition point, since it is more likely to be spilled anyway.
  RegisterIndex chosen_reg;
  int earliest_definition = kMaxInt;
  bool pending_only_use = false;
  bool already_spilled = false;
  for (RegisterIndex reg : *register_state()) {
    // Skip if register is in use, or not valid for representation.
    if (in_use & reg.ToBit(rep)) continue;

    VirtualRegisterData& vreg_data =
        VirtualRegisterDataFor(VirtualRegisterForRegister(reg));
    if ((!pending_only_use && register_state()->HasPendingUsesOnly(reg)) ||
        (!already_spilled && vreg_data.HasSpillOperand()) ||
        vreg_data.output_instr_index() < earliest_definition) {
      chosen_reg = reg;
      earliest_definition = vreg_data.output_instr_index();
      pending_only_use = register_state()->HasPendingUsesOnly(reg);
      already_spilled = vreg_data.HasSpillOperand();
    }
  }

  // There should always be an unblocked register available.
  DCHECK(chosen_reg.is_valid());
  return chosen_reg;
}

void SinglePassRegisterAllocator::CommitRegister(RegisterIndex reg,
                                                 int virtual_register,
                                                 InstructionOperand* operand,
                                                 UsePosition pos) {
  // Committing the output operation, and mark the register use in this
  // instruction, then mark it as free going forward.
  AllocatedOperand allocated = AllocatedOperandForReg(reg, virtual_register);
  register_state()->Commit(reg, allocated, operand, data());
  MarkRegisterUse(reg, RepresentationFor(virtual_register), pos);
  FreeRegister(reg, virtual_register);
  CheckConsistency();
}

void SinglePassRegisterAllocator::SpillRegister(RegisterIndex reg) {
  if (!register_state()->IsAllocated(reg)) return;

  // Spill the register and free register.
  int virtual_register = VirtualRegisterForRegister(reg);
  AllocatedOperand allocated = AllocatedOperandForReg(reg, virtual_register);
  register_state()->Spill(reg, allocated, data());
  FreeRegister(reg, virtual_register);
}

void SinglePassRegisterAllocator::SpillAllRegisters() {
  if (!HasRegisterState()) return;

  for (RegisterIndex reg : *register_state()) {
    SpillRegister(reg);
  }
}

void SinglePassRegisterAllocator::SpillRegisterForVirtualRegister(
    int virtual_register) {
  DCHECK_NE(virtual_register, InstructionOperand::kInvalidVirtualRegister);
  RegisterIndex reg = RegisterForVirtualRegister(virtual_register);
  if (reg.is_valid()) {
    SpillRegister(reg);
  }
}

AllocatedOperand SinglePassRegisterAllocator::AllocatedOperandForReg(
    RegisterIndex reg, int virtual_register) {
  MachineRepresentation rep = RepresentationFor(virtual_register);
  return AllocatedOperand(AllocatedOperand::REGISTER, rep, ToRegCode(reg, rep));
}

void SinglePassRegisterAllocator::AllocateUse(RegisterIndex reg,
                                              int virtual_register,
                                              InstructionOperand* operand,
                                              int instr_index,
                                              UsePosition pos) {
  DCHECK_NE(virtual_register, InstructionOperand::kInvalidVirtualRegister);
  DCHECK(IsFreeOrSameVirtualRegister(reg, virtual_register));

  AllocatedOperand allocated = AllocatedOperandForReg(reg, virtual_register);
  register_state()->Commit(reg, allocated, operand, data());
  register_state()->AllocateUse(reg, virtual_register, operand, instr_index,
                                data());
  AssignRegister(reg, virtual_register, pos);
  CheckConsistency();
}

void SinglePassRegisterAllocator::AllocatePendingUse(
    RegisterIndex reg, int virtual_register, InstructionOperand* operand,
    int instr_index) {
  DCHECK_NE(virtual_register, InstructionOperand::kInvalidVirtualRegister);
  DCHECK(IsFreeOrSameVirtualRegister(reg, virtual_register));

  register_state()->AllocatePendingUse(reg, virtual_register, operand,
                                       instr_index);
  // Since this is a pending use and the operand doesn't need to use a register,
  // allocate with UsePosition::kNone to avoid blocking it's use by other
  // operands in this instruction.
  AssignRegister(reg, virtual_register, UsePosition::kNone);
  CheckConsistency();
}

void SinglePassRegisterAllocator::AllocateUseWithMove(
    RegisterIndex reg, int virtual_register, UnallocatedOperand* operand,
    int instr_index, UsePosition pos) {
  AllocatedOperand to = AllocatedOperandForReg(reg, virtual_register);
  UnallocatedOperand from = UnallocatedOperand(
      UnallocatedOperand::REGISTER_OR_SLOT, virtual_register);
  data()->AddGapMove(instr_index, Instruction::END, from, to);
  InstructionOperand::ReplaceWith(operand, &to);
  MarkRegisterUse(reg, RepresentationFor(virtual_register), pos);
  CheckConsistency();
}

void SinglePassRegisterAllocator::AllocateInput(UnallocatedOperand* operand,
                                                int instr_index) {
  EnsureRegisterState();
  int virtual_register = operand->virtual_register();
  MachineRepresentation rep = RepresentationFor(virtual_register);
  VirtualRegisterData& vreg_data = VirtualRegisterDataFor(virtual_register);

  // Spill slot policy operands.
  if (operand->HasFixedSlotPolicy()) {
    // If the operand is from a fixed slot, allocate it to that fixed slot,
    // then add a gap move from an unconstrained copy of that input operand,
    // and spill the gap move's input operand.
    // TODO(rmcilroy): We could allocate a register for the gap move however
    // we would need to wait until we've done all the allocations for the
    // instruction since the allocation needs to reflect the state before
    // the instruction (at the gap move). For now spilling is fine since
    // fixed slot inputs are uncommon.
    UnallocatedOperand input_copy(UnallocatedOperand::REGISTER_OR_SLOT,
                                  virtual_register);
    AllocatedOperand allocated = AllocatedOperand(
        AllocatedOperand::STACK_SLOT, rep, operand->fixed_slot_index());
    InstructionOperand::ReplaceWith(operand, &allocated);
    MoveOperands* move_op =
        data()->AddGapMove(instr_index, Instruction::END, input_copy, *operand);
    vreg_data.SpillOperand(&move_op->source(), instr_index, data());
    return;
  } else if (operand->HasSlotPolicy()) {
    vreg_data.SpillOperand(operand, instr_index, data());
    return;
  }

  // Otherwise try to allocate a register for the operation.
  UsePosition pos =
      operand->IsUsedAtStart() ? UsePosition::kStart : UsePosition::kAll;
  if (operand->HasFixedRegisterPolicy() ||
      operand->HasFixedFPRegisterPolicy()) {
    // With a fixed register operand, we must use that register.
    RegisterIndex reg = FromRegCode(operand->fixed_register_index(), rep);
    if (!VirtualRegisterIsUnallocatedOrInReg(virtual_register, reg)) {
      // If the virtual register is already in a different register, then just
      // add a gap move from that register to the fixed register.
      AllocateUseWithMove(reg, virtual_register, operand, instr_index, pos);
    } else {
      // Otherwise allocate a use of the fixed register for |virtual_register|.
      AllocateUse(reg, virtual_register, operand, instr_index, pos);
    }
  } else {
    bool must_use_register = operand->HasRegisterPolicy() ||
                             (vreg_data.is_constant() &&
                              !operand->HasRegisterOrSlotOrConstantPolicy());
    RegisterIndex reg = ChooseRegisterFor(vreg_data, pos, must_use_register);

    if (reg.is_valid()) {
      if (must_use_register) {
        AllocateUse(reg, virtual_register, operand, instr_index, pos);
      } else {
        AllocatePendingUse(reg, virtual_register, operand, instr_index);
      }
    } else {
      vreg_data.SpillOperand(operand, instr_index, data());
    }
  }
}

void SinglePassRegisterAllocator::AllocateGapMoveInput(
    UnallocatedOperand* operand, int instr_index) {
  EnsureRegisterState();
  int virtual_register = operand->virtual_register();
  VirtualRegisterData& vreg_data = VirtualRegisterDataFor(virtual_register);

  // Gap move inputs should be unconstrained.
  DCHECK(operand->HasRegisterOrSlotPolicy());
  RegisterIndex reg = ChooseRegisterFor(vreg_data, UsePosition::kStart, false);
  if (reg.is_valid()) {
    AllocatePendingUse(reg, virtual_register, operand, instr_index);
  } else {
    vreg_data.SpillOperand(operand, instr_index, data());
  }
}

void SinglePassRegisterAllocator::AllocateConstantOutput(
    ConstantOperand* operand) {
  EnsureRegisterState();
  // If the constant is allocated to a register, spill it now to add the
  // necessary gap moves from the constant operand to the register.
  int virtual_register = operand->virtual_register();
  SpillRegisterForVirtualRegister(virtual_register);
}

void SinglePassRegisterAllocator::AllocateOutput(UnallocatedOperand* operand,
                                                 int instr_index) {
  AllocateOutput(operand, instr_index, UsePosition::kEnd);
}

RegisterIndex SinglePassRegisterAllocator::AllocateOutput(
    UnallocatedOperand* operand, int instr_index, UsePosition pos) {
  EnsureRegisterState();
  int virtual_register = operand->virtual_register();
  VirtualRegisterData& vreg_data = VirtualRegisterDataFor(virtual_register);

  RegisterIndex reg;
  if (operand->HasSlotPolicy() || operand->HasFixedSlotPolicy()) {
    // We can't allocate a register for output given the policy, so make sure
    // to spill the register holding this virtual register if any.
    SpillRegisterForVirtualRegister(virtual_register);
    reg = RegisterIndex::Invalid();
  } else if (operand->HasFixedPolicy()) {
    reg = FromRegCode(operand->fixed_register_index(),
                      RepresentationFor(virtual_register));
  } else {
    reg = ChooseRegisterFor(vreg_data, pos, operand->HasRegisterPolicy());
  }

  // TODO(rmcilroy): support secondary storage.
  if (!reg.is_valid()) {
    vreg_data.SpillOperand(operand, instr_index, data());
  } else {
    InstructionOperand move_output_to;
    if (!VirtualRegisterIsUnallocatedOrInReg(virtual_register, reg)) {
      // If the |virtual register| was in a different register (e.g., due to
      // the output having a fixed register), then commit its use in that
      // register here, and move it from the output operand below.
      RegisterIndex existing_reg = RegisterForVirtualRegister(virtual_register);
      // Don't mark |existing_reg| as used in this instruction, since it is used
      // in the (already allocated) following instruction's gap-move.
      CommitRegister(existing_reg, virtual_register, &move_output_to,
                     UsePosition::kNone);
    }
    CommitRegister(reg, virtual_register, operand, pos);
    if (move_output_to.IsAllocated()) {
      // Emit a move from output to the register that the |virtual_register| was
      // allocated to.
      EmitGapMoveFromOutput(*operand, move_output_to, instr_index);
    }
    if (vreg_data.NeedsSpillAtOutput()) {
      vreg_data.EmitGapMoveFromOutputToSpillSlot(
          *AllocatedOperand::cast(operand), data()->GetBlock(instr_index),
          instr_index, data());
    }
  }

  return reg;
}

void SinglePassRegisterAllocator::AllocateSameInputOutput(
    UnallocatedOperand* output, UnallocatedOperand* input, int instr_index) {
  EnsureRegisterState();
  int input_vreg = input->virtual_register();
  int output_vreg = output->virtual_register();

  // The input operand has the details of the register constraints, so replace
  // the output operand with a copy of the input, with the output's vreg.
  UnallocatedOperand output_as_input(*input, output_vreg);
  InstructionOperand::ReplaceWith(output, &output_as_input);
  RegisterIndex reg = AllocateOutput(output, instr_index, UsePosition::kAll);

  if (reg.is_valid()) {
    // Replace the input operand with an unallocated fixed register policy for
    // the same register.
    UnallocatedOperand::ExtendedPolicy policy =
        kind() == RegisterKind::kGeneral
            ? UnallocatedOperand::FIXED_REGISTER
            : UnallocatedOperand::FIXED_FP_REGISTER;
    MachineRepresentation rep = RepresentationFor(input_vreg);
    UnallocatedOperand fixed_input(policy, ToRegCode(reg, rep), input_vreg);
    InstructionOperand::ReplaceWith(input, &fixed_input);
  } else {
    // Output was spilled. Due to the SameAsInput allocation policy, we need to
    // make the input operand the same as the output, i.e., the output virtual
    // register's spill slot. As such, spill this input operand using the output
    // virtual register's spill slot, then add a gap-move to move the input
    // value into this spill slot.
    VirtualRegisterData& output_vreg_data = VirtualRegisterDataFor(output_vreg);
    output_vreg_data.SpillOperand(input, instr_index, data());

    // Add an unconstrained gap move for the input virtual register.
    UnallocatedOperand unconstrained_input(UnallocatedOperand::REGISTER_OR_SLOT,
                                           input_vreg);
    MoveOperands* move_ops = data()->AddGapMove(
        instr_index, Instruction::END, unconstrained_input, PendingOperand());
    output_vreg_data.SpillOperand(&move_ops->destination(), instr_index,
                                  data());
  }
}

void SinglePassRegisterAllocator::AllocateTemp(UnallocatedOperand* operand,
                                               int instr_index) {
  EnsureRegisterState();
  int virtual_register = operand->virtual_register();
  RegisterIndex reg;
  DCHECK(!operand->HasFixedSlotPolicy());
  if (operand->HasSlotPolicy()) {
    reg = RegisterIndex::Invalid();
  } else if (operand->HasFixedRegisterPolicy() ||
             operand->HasFixedFPRegisterPolicy()) {
    reg = FromRegCode(operand->fixed_register_index(),
                      RepresentationFor(virtual_register));
  } else {
    reg = ChooseRegisterFor(RepresentationFor(virtual_register),
                            UsePosition::kAll, operand->HasRegisterPolicy());
  }

  if (reg.is_valid()) {
    DCHECK(virtual_register == InstructionOperand::kInvalidVirtualRegister ||
           VirtualRegisterIsUnallocatedOrInReg(virtual_register, reg));
    CommitRegister(reg, virtual_register, operand, UsePosition::kAll);
  } else {
    VirtualRegisterData& vreg_data = VirtualRegisterDataFor(virtual_register);
    vreg_data.SpillOperand(operand, instr_index, data());
  }
}

bool SinglePassRegisterAllocator::DefinedAfter(int virtual_register,
                                               int instr_index,
                                               UsePosition pos) {
  if (virtual_register == InstructionOperand::kInvalidVirtualRegister)
    return false;
  int defined_at =
      VirtualRegisterDataFor(virtual_register).output_instr_index();
  return defined_at > instr_index ||
         (defined_at == instr_index && pos == UsePosition::kStart);
}

void SinglePassRegisterAllocator::ReserveFixedInputRegister(
    const UnallocatedOperand* operand, int instr_index) {
  ReserveFixedRegister(
      operand, instr_index,
      operand->IsUsedAtStart() ? UsePosition::kStart : UsePosition::kAll);
}

void SinglePassRegisterAllocator::ReserveFixedTempRegister(
    const UnallocatedOperand* operand, int instr_index) {
  ReserveFixedRegister(operand, instr_index, UsePosition::kAll);
}

void SinglePassRegisterAllocator::ReserveFixedOutputRegister(
    const UnallocatedOperand* operand, int instr_index) {
  ReserveFixedRegister(operand, instr_index, UsePosition::kEnd);
}

void SinglePassRegisterAllocator::ReserveFixedRegister(
    const UnallocatedOperand* operand, int instr_index, UsePosition pos) {
  EnsureRegisterState();
  int virtual_register = operand->virtual_register();
  MachineRepresentation rep = RepresentationFor(virtual_register);
  RegisterIndex reg = FromRegCode(operand->fixed_register_index(), rep);
  if (!IsFreeOrSameVirtualRegister(reg, virtual_register) &&
      !DefinedAfter(virtual_register, instr_index, pos)) {
    // If register is in-use by a different virtual register, spill it now.
    // TODO(rmcilroy): Consider moving to a unconstrained register instead of
    // spilling.
    SpillRegister(reg);
  }
  MarkRegisterUse(reg, rep, pos);
}

void SinglePassRegisterAllocator::EnsureRegisterState() {
  if (!HasRegisterState()) {
    register_state_ = RegisterState::New(kind(), num_allocatable_registers_,
                                         data()->allocation_zone());
  }
}

MidTierRegisterAllocator::MidTierRegisterAllocator(
    MidTierRegisterAllocationData* data)
    : data_(data),
      general_reg_allocator_(
          new SinglePassRegisterAllocator(RegisterKind::kGeneral, data)),
      double_reg_allocator_(
          new SinglePassRegisterAllocator(RegisterKind::kDouble, data)) {}

MidTierRegisterAllocator::~MidTierRegisterAllocator() = default;

void MidTierRegisterAllocator::DefineOutputs() {
  for (const InstructionBlock* block :
       base::Reversed(code()->instruction_blocks())) {
    data_->tick_counter()->TickAndMaybeEnterSafepoint();

    InitializeBlockState(block);
    DefineOutputs(block);
  }
}

void MidTierRegisterAllocator::InitializeBlockState(
    const InstructionBlock* block) {
  // Mark this block as dominating itself.
  BlockState& block_state = data()->block_state(block->rpo_number());
  block_state.dominated_blocks()->Add(block->rpo_number().ToInt());

  if (block->dominator().IsValid()) {
    // Add all the blocks this block dominates to its dominator.
    BlockState& dominator_block_state = data()->block_state(block->dominator());
    dominator_block_state.dominated_blocks()->Union(
        *block_state.dominated_blocks());
  } else {
    // Only the first block shouldn't have a dominator.
    DCHECK_EQ(block, code()->instruction_blocks().front());
  }
}

void MidTierRegisterAllocator::DefineOutputs(const InstructionBlock* block) {
  int block_start = block->first_instruction_index();
  for (int index = block->last_instruction_index(); index >= block_start;
       index--) {
    Instruction* instr = code()->InstructionAt(index);

    // For each instruction, define details of the output with the associated
    // virtual register data.
    for (size_t i = 0; i < instr->OutputCount(); i++) {
      InstructionOperand* output = instr->OutputAt(i);
      if (output->IsConstant()) {
        ConstantOperand* constant_operand = ConstantOperand::cast(output);
        int virtual_register = constant_operand->virtual_register();
        VirtualRegisterDataFor(virtual_register)
            .DefineAsConstantOperand(constant_operand, index);
      } else {
        DCHECK(output->IsUnallocated());
        UnallocatedOperand* unallocated_operand =
            UnallocatedOperand::cast(output);
        int virtual_register = unallocated_operand->virtual_register();
        if (unallocated_operand->HasFixedSlotPolicy()) {
          // If output has a fixed slot policy, allocate its spill operand now
          // so that the register allocator can use this knowledge.
          MachineRepresentation rep = RepresentationFor(virtual_register);
          AllocatedOperand* fixed_spill_operand = AllocatedOperand::New(
              allocation_zone(), AllocatedOperand::STACK_SLOT, rep,
              unallocated_operand->fixed_slot_index());
          VirtualRegisterDataFor(virtual_register)
              .DefineAsFixedSpillOperand(fixed_spill_operand, virtual_register,
                                         index);
        } else {
          VirtualRegisterDataFor(virtual_register)
              .DefineAsUnallocatedOperand(virtual_register, index);
        }
      }
    }

    // Mark any instructions that require reference maps for later reference map
    // processing.
    if (instr->HasReferenceMap()) {
      data()->reference_map_instructions().push_back(index);
    }
  }

  // Define phi output operands.
  for (PhiInstruction* phi : block->phis()) {
    int virtual_register = phi->virtual_register();
    VirtualRegisterDataFor(virtual_register)
        .DefineAsPhi(virtual_register, block->first_instruction_index());
  }
}

void MidTierRegisterAllocator::AllocateRegisters() {
  for (InstructionBlock* block : base::Reversed(code()->instruction_blocks())) {
    data_->tick_counter()->TickAndMaybeEnterSafepoint();
    AllocateRegisters(block);
  }

  UpdateSpillRangesForLoops();

  data()->frame()->SetAllocatedRegisters(
      general_reg_allocator().assigned_registers());
  data()->frame()->SetAllocatedDoubleRegisters(
      double_reg_allocator().assigned_registers());
}

void MidTierRegisterAllocator::AllocateRegisters(
    const InstructionBlock* block) {
  general_reg_allocator().StartBlock(block);
  double_reg_allocator().StartBlock(block);

  // Allocate registers for instructions in reverse, from the end of the block
  // to the start.
  int block_start = block->first_instruction_index();
  for (int instr_index = block->last_instruction_index();
       instr_index >= block_start; instr_index--) {
    Instruction* instr = code()->InstructionAt(instr_index);

    // Reserve any fixed register operands to prevent the register being
    // allocated to another operand.
    ReserveFixedRegisters(instr_index);

    // Allocate outputs.
    for (size_t i = 0; i < instr->OutputCount(); i++) {
      InstructionOperand* output = instr->OutputAt(i);
      DCHECK(!output->IsAllocated());
      if (output->IsConstant()) {
        ConstantOperand* constant_operand = ConstantOperand::cast(output);
        AllocatorFor(constant_operand).AllocateConstantOutput(constant_operand);
      } else {
        UnallocatedOperand* unallocated_output =
            UnallocatedOperand::cast(output);
        if (unallocated_output->HasSameAsInputPolicy()) {
          DCHECK_EQ(i, 0);
          UnallocatedOperand* unallocated_input =
              UnallocatedOperand::cast(instr->InputAt(0));
          DCHECK_EQ(AllocatorFor(unallocated_input).kind(),
                    AllocatorFor(unallocated_output).kind());
          AllocatorFor(unallocated_output)
              .AllocateSameInputOutput(unallocated_output, unallocated_input,
                                       instr_index);
        } else {
          AllocatorFor(unallocated_output)
              .AllocateOutput(unallocated_output, instr_index);
        }
      }
    }

    if (instr->ClobbersRegisters()) {
      general_reg_allocator().SpillAllRegisters();
    }
    if (instr->ClobbersDoubleRegisters()) {
      double_reg_allocator().SpillAllRegisters();
    }

    // Allocate temporaries.
    for (size_t i = 0; i < instr->TempCount(); i++) {
      UnallocatedOperand* temp = UnallocatedOperand::cast(instr->TempAt(i));
      AllocatorFor(temp).AllocateTemp(temp, instr_index);
    }

    // Allocate inputs that are used across the whole instruction.
    for (size_t i = 0; i < instr->InputCount(); i++) {
      if (!instr->InputAt(i)->IsUnallocated()) continue;
      UnallocatedOperand* input = UnallocatedOperand::cast(instr->InputAt(i));
      if (input->IsUsedAtStart()) continue;
      AllocatorFor(input).AllocateInput(input, instr_index);
    }

    // Then allocate inputs that are only used at the start of the instruction.
    for (size_t i = 0; i < instr->InputCount(); i++) {
      if (!instr->InputAt(i)->IsUnallocated()) continue;
      UnallocatedOperand* input = UnallocatedOperand::cast(instr->InputAt(i));
      DCHECK(input->IsUsedAtStart());
      AllocatorFor(input).AllocateInput(input, instr_index);
    }

    // Allocate any unallocated gap move inputs.
    ParallelMove* moves = instr->GetParallelMove(Instruction::END);
    if (moves != nullptr) {
      for (MoveOperands* move : *moves) {
        DCHECK(!move->destination().IsUnallocated());
        if (move->source().IsUnallocated()) {
          UnallocatedOperand* source =
              UnallocatedOperand::cast(&move->source());
          AllocatorFor(source).AllocateGapMoveInput(source, instr_index);
        }
      }
    }

    general_reg_allocator().EndInstruction();
    double_reg_allocator().EndInstruction();
  }

  // TODO(rmcilroy): Add support for cross-block allocations.
  general_reg_allocator().SpillAllRegisters();
  double_reg_allocator().SpillAllRegisters();

  general_reg_allocator().EndBlock(block);
  double_reg_allocator().EndBlock(block);
}

SinglePassRegisterAllocator& MidTierRegisterAllocator::AllocatorFor(
    MachineRepresentation rep) {
  if (IsFloatingPoint(rep)) {
    return double_reg_allocator();
  } else {
    return general_reg_allocator();
  }
}

SinglePassRegisterAllocator& MidTierRegisterAllocator::AllocatorFor(
    const UnallocatedOperand* operand) {
  return AllocatorFor(RepresentationFor(operand->virtual_register()));
}

SinglePassRegisterAllocator& MidTierRegisterAllocator::AllocatorFor(
    const ConstantOperand* operand) {
  return AllocatorFor(RepresentationFor(operand->virtual_register()));
}

bool MidTierRegisterAllocator::IsFixedRegisterPolicy(
    const UnallocatedOperand* operand) {
  return operand->HasFixedRegisterPolicy() ||
         operand->HasFixedFPRegisterPolicy();
}

void MidTierRegisterAllocator::ReserveFixedRegisters(int instr_index) {
  Instruction* instr = code()->InstructionAt(instr_index);
  for (size_t i = 0; i < instr->OutputCount(); i++) {
    if (!instr->OutputAt(i)->IsUnallocated()) continue;
    const UnallocatedOperand* operand =
        UnallocatedOperand::cast(instr->OutputAt(i));
    if (operand->HasSameAsInputPolicy()) {
      // Input operand has the register constraints, use it here to reserve the
      // register for the output (it will be reserved for input below).
      operand = UnallocatedOperand::cast(instr->InputAt(i));
    }
    if (IsFixedRegisterPolicy(operand)) {
      AllocatorFor(operand).ReserveFixedOutputRegister(operand, instr_index);
    }
  }
  for (size_t i = 0; i < instr->TempCount(); i++) {
    if (!instr->TempAt(i)->IsUnallocated()) continue;
    const UnallocatedOperand* operand =
        UnallocatedOperand::cast(instr->TempAt(i));
    if (IsFixedRegisterPolicy(operand)) {
      AllocatorFor(operand).ReserveFixedTempRegister(operand, instr_index);
    }
  }
  for (size_t i = 0; i < instr->InputCount(); i++) {
    if (!instr->InputAt(i)->IsUnallocated()) continue;
    const UnallocatedOperand* operand =
        UnallocatedOperand::cast(instr->InputAt(i));
    if (IsFixedRegisterPolicy(operand)) {
      AllocatorFor(operand).ReserveFixedInputRegister(operand, instr_index);
    }
  }
}

void MidTierRegisterAllocator::UpdateSpillRangesForLoops() {
  // Extend the spill range of any spill that crosses a loop header to
  // the full loop.
  for (InstructionBlock* block : code()->instruction_blocks()) {
    if (block->IsLoopHeader()) {
      RpoNumber last_loop_block =
          RpoNumber::FromInt(block->loop_end().ToInt() - 1);
      int last_loop_instr =
          data()->GetBlock(last_loop_block)->last_instruction_index();
      // Extend spill range for all spilled values that are live on entry to the
      // loop header.
      BitVector::Iterator iterator(&data()->spilled_virtual_registers());
      for (; !iterator.Done(); iterator.Advance()) {
        const VirtualRegisterData& vreg_data =
            VirtualRegisterDataFor(iterator.Current());
        if (vreg_data.HasSpillRange() &&
            vreg_data.spill_range()->IsLiveAt(block->first_instruction_index(),
                                              block)) {
          vreg_data.spill_range()->ExtendRangeTo(last_loop_instr);
        }
      }
    }
  }
}

// Spill slot allocator for mid-tier register allocation.
class MidTierSpillSlotAllocator final {
 public:
  explicit MidTierSpillSlotAllocator(MidTierRegisterAllocationData* data);

  void Allocate(VirtualRegisterData* virtual_register);

 private:
  class SpillSlot;

  void AdvanceTo(int instr_index);
  SpillSlot* GetFreeSpillSlot(int byte_width);

  MidTierRegisterAllocationData* data() const { return data_; }
  InstructionSequence* code() const { return data()->code(); }
  Frame* frame() const { return data()->frame(); }
  Zone* zone() const { return data()->allocation_zone(); }

  struct OrderByLastUse {
    bool operator()(const SpillSlot* a, const SpillSlot* b) const;
  };

  MidTierRegisterAllocationData* data_;
  ZonePriorityQueue<SpillSlot*, OrderByLastUse> allocated_slots_;
  ZoneLinkedList<SpillSlot*> free_slots_;
  int position_;

  DISALLOW_COPY_AND_ASSIGN(MidTierSpillSlotAllocator);
};

class MidTierSpillSlotAllocator::SpillSlot : public ZoneObject {
 public:
  SpillSlot(int stack_slot, int byte_width)
      : stack_slot_(stack_slot), byte_width_(byte_width), range_() {}

  void AddRange(const Range& range) { range_.AddRange(range); }

  AllocatedOperand ToOperand(MachineRepresentation rep) const {
    return AllocatedOperand(AllocatedOperand::STACK_SLOT, rep, stack_slot_);
  }

  int byte_width() const { return byte_width_; }
  int last_use() const { return range_.end(); }

 private:
  int stack_slot_;
  int byte_width_;
  Range range_;

  DISALLOW_COPY_AND_ASSIGN(SpillSlot);
};

bool MidTierSpillSlotAllocator::OrderByLastUse::operator()(
    const SpillSlot* a, const SpillSlot* b) const {
  return a->last_use() > b->last_use();
}

MidTierSpillSlotAllocator::MidTierSpillSlotAllocator(
    MidTierRegisterAllocationData* data)
    : data_(data),
      allocated_slots_(data->allocation_zone()),
      free_slots_(data->allocation_zone()),
      position_(0) {}

void MidTierSpillSlotAllocator::AdvanceTo(int instr_index) {
  // Move any slots that are no longer in use to the free slots list.
  DCHECK_LE(position_, instr_index);
  while (!allocated_slots_.empty() &&
         instr_index > allocated_slots_.top()->last_use()) {
    free_slots_.push_front(allocated_slots_.top());
    allocated_slots_.pop();
  }
  position_ = instr_index;
}

MidTierSpillSlotAllocator::SpillSlot*
MidTierSpillSlotAllocator::GetFreeSpillSlot(int byte_width) {
  for (auto it = free_slots_.begin(); it != free_slots_.end(); ++it) {
    SpillSlot* slot = *it;
    if (slot->byte_width() == byte_width) {
      free_slots_.erase(it);
      return slot;
    }
  }
  return nullptr;
}

void MidTierSpillSlotAllocator::Allocate(
    VirtualRegisterData* virtual_register) {
  DCHECK(virtual_register->HasPendingSpillOperand());
  VirtualRegisterData::SpillRange* spill_range =
      virtual_register->spill_range();
  MachineRepresentation rep =
      data()->RepresentationFor(virtual_register->vreg());
  int byte_width = ByteWidthForStackSlot(rep);
  Range live_range = spill_range->live_range();

  AdvanceTo(live_range.start());

  // Try to re-use an existing free spill slot.
  SpillSlot* slot = GetFreeSpillSlot(byte_width);
  if (slot == nullptr) {
    // Otherwise allocate a new slot.
    int stack_slot_ = frame()->AllocateSpillSlot(byte_width);
    slot = zone()->New<SpillSlot>(stack_slot_, byte_width);
  }

  // Extend the range of the slot to include this spill range, and allocate the
  // pending spill operands with this slot.
  slot->AddRange(live_range);
  virtual_register->AllocatePendingSpillOperand(slot->ToOperand(rep));
  allocated_slots_.push(slot);
}

void AllocateSpillSlots(MidTierRegisterAllocationData* data) {
  ZoneVector<VirtualRegisterData*> spilled(data->allocation_zone());
  BitVector::Iterator iterator(&data->spilled_virtual_registers());
  for (; !iterator.Done(); iterator.Advance()) {
    VirtualRegisterData& vreg_data =
        data->VirtualRegisterDataFor(iterator.Current());
    if (vreg_data.HasPendingSpillOperand()) {
      spilled.push_back(&vreg_data);
    }
  }

  // Sort the spill ranges by order of their first use to enable linear
  // allocation of spill slots.
  std::sort(spilled.begin(), spilled.end(),
            [](const VirtualRegisterData* a, const VirtualRegisterData* b) {
              return a->spill_range()->live_range().start() <
                     b->spill_range()->live_range().start();
            });

  // Allocate a spill slot for each virtual register with a spill range.
  MidTierSpillSlotAllocator allocator(data);
  for (VirtualRegisterData* spill : spilled) {
    allocator.Allocate(spill);
  }
}

// Populates reference maps for mid-tier register allocation.
class MidTierReferenceMapPopulator final {
 public:
  explicit MidTierReferenceMapPopulator(MidTierRegisterAllocationData* data);

  void RecordReferences(const VirtualRegisterData& virtual_register);

 private:
  MidTierRegisterAllocationData* data() const { return data_; }
  InstructionSequence* code() const { return data()->code(); }

  MidTierRegisterAllocationData* data_;

  DISALLOW_COPY_AND_ASSIGN(MidTierReferenceMapPopulator);
};

MidTierReferenceMapPopulator::MidTierReferenceMapPopulator(
    MidTierRegisterAllocationData* data)
    : data_(data) {}

void MidTierReferenceMapPopulator::RecordReferences(
    const VirtualRegisterData& virtual_register) {
  if (!virtual_register.HasAllocatedSpillOperand()) return;
  if (!code()->IsReference(virtual_register.vreg())) return;

  VirtualRegisterData::SpillRange* spill_range = virtual_register.spill_range();
  Range& live_range = spill_range->live_range();
  AllocatedOperand allocated =
      *AllocatedOperand::cast(virtual_register.spill_operand());
  for (int instr_index : data()->reference_map_instructions()) {
    if (instr_index > live_range.end() || instr_index < live_range.start())
      continue;
    Instruction* instr = data()->code()->InstructionAt(instr_index);
    DCHECK(instr->HasReferenceMap());

    if (spill_range->IsLiveAt(instr_index, instr->block())) {
      instr->reference_map()->RecordReference(allocated);
    }
  }
}

void PopulateReferenceMaps(MidTierRegisterAllocationData* data) {
  MidTierReferenceMapPopulator populator(data);
  BitVector::Iterator iterator(&data->spilled_virtual_registers());
  for (; !iterator.Done(); iterator.Advance()) {
    populator.RecordReferences(
        data->VirtualRegisterDataFor(iterator.Current()));
  }
}

}  // namespace compiler
}  // namespace internal
}  // namespace v8

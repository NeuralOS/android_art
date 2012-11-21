/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "arm_lir.h"
#include "../codegen_util.h"
#include "../ralloc_util.h"

namespace art {

/* This file contains codegen for the Thumb ISA. */

static int EncodeImmSingle(int value)
{
  int res;
  int bit_a =  (value & 0x80000000) >> 31;
  int not_bit_b = (value & 0x40000000) >> 30;
  int bit_b =  (value & 0x20000000) >> 29;
  int b_smear =  (value & 0x3e000000) >> 25;
  int slice =   (value & 0x01f80000) >> 19;
  int zeroes =  (value & 0x0007ffff);
  if (zeroes != 0)
    return -1;
  if (bit_b) {
    if ((not_bit_b != 0) || (b_smear != 0x1f))
      return -1;
  } else {
    if ((not_bit_b != 1) || (b_smear != 0x0))
      return -1;
  }
  res = (bit_a << 7) | (bit_b << 6) | slice;
  return res;
}

static LIR* LoadFPConstantValue(CompilationUnit* cu, int r_dest, int value)
{
  int encoded_imm = EncodeImmSingle(value);
  DCHECK(ARM_SINGLEREG(r_dest));
  if (encoded_imm >= 0) {
    return NewLIR2(cu, kThumb2Vmovs_IMM8, r_dest, encoded_imm);
  }
  LIR* data_target = ScanLiteralPool(cu->literal_list, value, 0);
  if (data_target == NULL) {
    data_target = AddWordData(cu, &cu->literal_list, value);
  }
  LIR* load_pc_rel = RawLIR(cu, cu->current_dalvik_offset, kThumb2Vldrs,
                          r_dest, r15pc, 0, 0, 0, data_target);
  SetMemRefType(load_pc_rel, true, kLiteral);
  load_pc_rel->alias_info = reinterpret_cast<uintptr_t>(data_target);
  AppendLIR(cu, load_pc_rel);
  return load_pc_rel;
}

static int LeadingZeros(uint32_t val)
{
  uint32_t alt;
  int n;
  int count;

  count = 16;
  n = 32;
  do {
    alt = val >> count;
    if (alt != 0) {
      n = n - count;
      val = alt;
    }
    count >>= 1;
  } while (count);
  return n - val;
}

/*
 * Determine whether value can be encoded as a Thumb2 modified
 * immediate.  If not, return -1.  If so, return i:imm3:a:bcdefgh form.
 */
int ModifiedImmediate(uint32_t value)
{
   int z_leading;
   int z_trailing;
   uint32_t b0 = value & 0xff;

   /* Note: case of value==0 must use 0:000:0:0000000 encoding */
   if (value <= 0xFF)
     return b0;  // 0:000:a:bcdefgh
   if (value == ((b0 << 16) | b0))
     return (0x1 << 8) | b0; /* 0:001:a:bcdefgh */
   if (value == ((b0 << 24) | (b0 << 16) | (b0 << 8) | b0))
     return (0x3 << 8) | b0; /* 0:011:a:bcdefgh */
   b0 = (value >> 8) & 0xff;
   if (value == ((b0 << 24) | (b0 << 8)))
     return (0x2 << 8) | b0; /* 0:010:a:bcdefgh */
   /* Can we do it with rotation? */
   z_leading = LeadingZeros(value);
   z_trailing = 32 - LeadingZeros(~value & (value - 1));
   /* A run of eight or fewer active bits? */
   if ((z_leading + z_trailing) < 24)
     return -1;  /* No - bail */
   /* left-justify the constant, discarding msb (known to be 1) */
   value <<= z_leading + 1;
   /* Create bcdefgh */
   value >>= 25;
   /* Put it all together */
   return value | ((0x8 + z_leading) << 7); /* [01000..11111]:bcdefgh */
}

/*
 * Load a immediate using a shortcut if possible; otherwise
 * grab from the per-translation literal pool.
 *
 * No additional register clobbering operation performed. Use this version when
 * 1) r_dest is freshly returned from AllocTemp or
 * 2) The codegen is under fixed register usage
 */
LIR* LoadConstantNoClobber(CompilationUnit* cu, int r_dest, int value)
{
  LIR* res;
  int mod_imm;

  if (ARM_FPREG(r_dest)) {
    return LoadFPConstantValue(cu, r_dest, value);
  }

  /* See if the value can be constructed cheaply */
  if (ARM_LOWREG(r_dest) && (value >= 0) && (value <= 255)) {
    return NewLIR2(cu, kThumbMovImm, r_dest, value);
  }
  /* Check Modified immediate special cases */
  mod_imm = ModifiedImmediate(value);
  if (mod_imm >= 0) {
    res = NewLIR2(cu, kThumb2MovImmShift, r_dest, mod_imm);
    return res;
  }
  mod_imm = ModifiedImmediate(~value);
  if (mod_imm >= 0) {
    res = NewLIR2(cu, kThumb2MvnImm12, r_dest, mod_imm);
    return res;
  }
  /* 16-bit immediate? */
  if ((value & 0xffff) == value) {
    res = NewLIR2(cu, kThumb2MovImm16, r_dest, value);
    return res;
  }
  /* No shortcut - go ahead and use literal pool */
  LIR* data_target = ScanLiteralPool(cu->literal_list, value, 0);
  if (data_target == NULL) {
    data_target = AddWordData(cu, &cu->literal_list, value);
  }
  LIR* load_pc_rel = RawLIR(cu, cu->current_dalvik_offset,
                          kThumb2LdrPcRel12, r_dest, 0, 0, 0, 0, data_target);
  SetMemRefType(load_pc_rel, true, kLiteral);
  load_pc_rel->alias_info = reinterpret_cast<uintptr_t>(data_target);
  res = load_pc_rel;
  AppendLIR(cu, load_pc_rel);

  /*
   * To save space in the constant pool, we use the ADD_RRI8 instruction to
   * add up to 255 to an existing constant value.
   */
  if (data_target->operands[0] != value) {
    OpRegImm(cu, kOpAdd, r_dest, value - data_target->operands[0]);
  }
  return res;
}

LIR* OpBranchUnconditional(CompilationUnit* cu, OpKind op)
{
  DCHECK_EQ(op, kOpUncondBr);
  return NewLIR1(cu, kThumbBUncond, 0 /* offset to be patched */);
}

LIR* OpCondBranch(CompilationUnit* cu, ConditionCode cc, LIR* target)
{
  LIR* branch = NewLIR2(cu, kThumb2BCond, 0 /* offset to be patched */,
                        ArmConditionEncoding(cc));
  branch->target = target;
  return branch;
}

LIR* OpReg(CompilationUnit* cu, OpKind op, int r_dest_src)
{
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpBlx:
      opcode = kThumbBlxR;
      break;
    default:
      LOG(FATAL) << "Bad opcode " << op;
  }
  return NewLIR1(cu, opcode, r_dest_src);
}

LIR* OpRegRegShift(CompilationUnit* cu, OpKind op, int r_dest_src1,
                   int r_src2, int shift)
{
  bool thumb_form = ((shift == 0) && ARM_LOWREG(r_dest_src1) && ARM_LOWREG(r_src2));
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpAdc:
      opcode = (thumb_form) ? kThumbAdcRR : kThumb2AdcRRR;
      break;
    case kOpAnd:
      opcode = (thumb_form) ? kThumbAndRR : kThumb2AndRRR;
      break;
    case kOpBic:
      opcode = (thumb_form) ? kThumbBicRR : kThumb2BicRRR;
      break;
    case kOpCmn:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbCmnRR : kThumb2CmnRR;
      break;
    case kOpCmp:
      if (thumb_form)
        opcode = kThumbCmpRR;
      else if ((shift == 0) && !ARM_LOWREG(r_dest_src1) && !ARM_LOWREG(r_src2))
        opcode = kThumbCmpHH;
      else if ((shift == 0) && ARM_LOWREG(r_dest_src1))
        opcode = kThumbCmpLH;
      else if (shift == 0)
        opcode = kThumbCmpHL;
      else
        opcode = kThumb2CmpRR;
      break;
    case kOpXor:
      opcode = (thumb_form) ? kThumbEorRR : kThumb2EorRRR;
      break;
    case kOpMov:
      DCHECK_EQ(shift, 0);
      if (ARM_LOWREG(r_dest_src1) && ARM_LOWREG(r_src2))
        opcode = kThumbMovRR;
      else if (!ARM_LOWREG(r_dest_src1) && !ARM_LOWREG(r_src2))
        opcode = kThumbMovRR_H2H;
      else if (ARM_LOWREG(r_dest_src1))
        opcode = kThumbMovRR_H2L;
      else
        opcode = kThumbMovRR_L2H;
      break;
    case kOpMul:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbMul : kThumb2MulRRR;
      break;
    case kOpMvn:
      opcode = (thumb_form) ? kThumbMvn : kThumb2MnvRR;
      break;
    case kOpNeg:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbNeg : kThumb2NegRR;
      break;
    case kOpOr:
      opcode = (thumb_form) ? kThumbOrr : kThumb2OrrRRR;
      break;
    case kOpSbc:
      opcode = (thumb_form) ? kThumbSbc : kThumb2SbcRRR;
      break;
    case kOpTst:
      opcode = (thumb_form) ? kThumbTst : kThumb2TstRR;
      break;
    case kOpLsl:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbLslRR : kThumb2LslRRR;
      break;
    case kOpLsr:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbLsrRR : kThumb2LsrRRR;
      break;
    case kOpAsr:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbAsrRR : kThumb2AsrRRR;
      break;
    case kOpRor:
      DCHECK_EQ(shift, 0);
      opcode = (thumb_form) ? kThumbRorRR : kThumb2RorRRR;
      break;
    case kOpAdd:
      opcode = (thumb_form) ? kThumbAddRRR : kThumb2AddRRR;
      break;
    case kOpSub:
      opcode = (thumb_form) ? kThumbSubRRR : kThumb2SubRRR;
      break;
    case kOp2Byte:
      DCHECK_EQ(shift, 0);
      return NewLIR4(cu, kThumb2Sbfx, r_dest_src1, r_src2, 0, 8);
    case kOp2Short:
      DCHECK_EQ(shift, 0);
      return NewLIR4(cu, kThumb2Sbfx, r_dest_src1, r_src2, 0, 16);
    case kOp2Char:
      DCHECK_EQ(shift, 0);
      return NewLIR4(cu, kThumb2Ubfx, r_dest_src1, r_src2, 0, 16);
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  DCHECK_GE(static_cast<int>(opcode), 0);
  if (EncodingMap[opcode].flags & IS_BINARY_OP)
    return NewLIR2(cu, opcode, r_dest_src1, r_src2);
  else if (EncodingMap[opcode].flags & IS_TERTIARY_OP) {
    if (EncodingMap[opcode].field_loc[2].kind == kFmtShift)
      return NewLIR3(cu, opcode, r_dest_src1, r_src2, shift);
    else
      return NewLIR3(cu, opcode, r_dest_src1, r_dest_src1, r_src2);
  } else if (EncodingMap[opcode].flags & IS_QUAD_OP)
    return NewLIR4(cu, opcode, r_dest_src1, r_dest_src1, r_src2, shift);
  else {
    LOG(FATAL) << "Unexpected encoding operand count";
    return NULL;
  }
}

LIR* OpRegReg(CompilationUnit* cu, OpKind op, int r_dest_src1, int r_src2)
{
  return OpRegRegShift(cu, op, r_dest_src1, r_src2, 0);
}

LIR* OpRegRegRegShift(CompilationUnit* cu, OpKind op, int r_dest, int r_src1,
            int r_src2, int shift)
{
  ArmOpcode opcode = kThumbBkpt;
  bool thumb_form = (shift == 0) && ARM_LOWREG(r_dest) && ARM_LOWREG(r_src1) &&
      ARM_LOWREG(r_src2);
  switch (op) {
    case kOpAdd:
      opcode = (thumb_form) ? kThumbAddRRR : kThumb2AddRRR;
      break;
    case kOpSub:
      opcode = (thumb_form) ? kThumbSubRRR : kThumb2SubRRR;
      break;
    case kOpRsub:
      opcode = kThumb2RsubRRR;
      break;
    case kOpAdc:
      opcode = kThumb2AdcRRR;
      break;
    case kOpAnd:
      opcode = kThumb2AndRRR;
      break;
    case kOpBic:
      opcode = kThumb2BicRRR;
      break;
    case kOpXor:
      opcode = kThumb2EorRRR;
      break;
    case kOpMul:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2MulRRR;
      break;
    case kOpOr:
      opcode = kThumb2OrrRRR;
      break;
    case kOpSbc:
      opcode = kThumb2SbcRRR;
      break;
    case kOpLsl:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2LslRRR;
      break;
    case kOpLsr:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2LsrRRR;
      break;
    case kOpAsr:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2AsrRRR;
      break;
    case kOpRor:
      DCHECK_EQ(shift, 0);
      opcode = kThumb2RorRRR;
      break;
    default:
      LOG(FATAL) << "Bad opcode: " << op;
      break;
  }
  DCHECK_GE(static_cast<int>(opcode), 0);
  if (EncodingMap[opcode].flags & IS_QUAD_OP)
    return NewLIR4(cu, opcode, r_dest, r_src1, r_src2, shift);
  else {
    DCHECK(EncodingMap[opcode].flags & IS_TERTIARY_OP);
    return NewLIR3(cu, opcode, r_dest, r_src1, r_src2);
  }
}

LIR* OpRegRegReg(CompilationUnit* cu, OpKind op, int r_dest, int r_src1,
                 int r_src2)
{
  return OpRegRegRegShift(cu, op, r_dest, r_src1, r_src2, 0);
}

LIR* OpRegRegImm(CompilationUnit* cu, OpKind op, int r_dest, int r_src1,
                 int value)
{
  LIR* res;
  bool neg = (value < 0);
  int abs_value = (neg) ? -value : value;
  ArmOpcode opcode = kThumbBkpt;
  ArmOpcode alt_opcode = kThumbBkpt;
  bool all_low_regs = (ARM_LOWREG(r_dest) && ARM_LOWREG(r_src1));
  int mod_imm = ModifiedImmediate(value);
  int mod_imm_neg = ModifiedImmediate(-value);

  switch (op) {
    case kOpLsl:
      if (all_low_regs)
        return NewLIR3(cu, kThumbLslRRI5, r_dest, r_src1, value);
      else
        return NewLIR3(cu, kThumb2LslRRI5, r_dest, r_src1, value);
    case kOpLsr:
      if (all_low_regs)
        return NewLIR3(cu, kThumbLsrRRI5, r_dest, r_src1, value);
      else
        return NewLIR3(cu, kThumb2LsrRRI5, r_dest, r_src1, value);
    case kOpAsr:
      if (all_low_regs)
        return NewLIR3(cu, kThumbAsrRRI5, r_dest, r_src1, value);
      else
        return NewLIR3(cu, kThumb2AsrRRI5, r_dest, r_src1, value);
    case kOpRor:
      return NewLIR3(cu, kThumb2RorRRI5, r_dest, r_src1, value);
    case kOpAdd:
      if (ARM_LOWREG(r_dest) && (r_src1 == r13sp) &&
        (value <= 1020) && ((value & 0x3)==0)) {
        return NewLIR3(cu, kThumbAddSpRel, r_dest, r_src1, value >> 2);
      } else if (ARM_LOWREG(r_dest) && (r_src1 == r15pc) &&
          (value <= 1020) && ((value & 0x3)==0)) {
        return NewLIR3(cu, kThumbAddPcRel, r_dest, r_src1, value >> 2);
      }
      // Note: intentional fallthrough
    case kOpSub:
      if (all_low_regs && ((abs_value & 0x7) == abs_value)) {
        if (op == kOpAdd)
          opcode = (neg) ? kThumbSubRRI3 : kThumbAddRRI3;
        else
          opcode = (neg) ? kThumbAddRRI3 : kThumbSubRRI3;
        return NewLIR3(cu, opcode, r_dest, r_src1, abs_value);
      } else if ((abs_value & 0xff) == abs_value) {
        if (op == kOpAdd)
          opcode = (neg) ? kThumb2SubRRI12 : kThumb2AddRRI12;
        else
          opcode = (neg) ? kThumb2AddRRI12 : kThumb2SubRRI12;
        return NewLIR3(cu, opcode, r_dest, r_src1, abs_value);
      }
      if (mod_imm_neg >= 0) {
        op = (op == kOpAdd) ? kOpSub : kOpAdd;
        mod_imm = mod_imm_neg;
      }
      if (op == kOpSub) {
        opcode = kThumb2SubRRI8;
        alt_opcode = kThumb2SubRRR;
      } else {
        opcode = kThumb2AddRRI8;
        alt_opcode = kThumb2AddRRR;
      }
      break;
    case kOpAdc:
      opcode = kThumb2AdcRRI8;
      alt_opcode = kThumb2AdcRRR;
      break;
    case kOpSbc:
      opcode = kThumb2SbcRRI8;
      alt_opcode = kThumb2SbcRRR;
      break;
    case kOpOr:
      opcode = kThumb2OrrRRI8;
      alt_opcode = kThumb2OrrRRR;
      break;
    case kOpAnd:
      opcode = kThumb2AndRRI8;
      alt_opcode = kThumb2AndRRR;
      break;
    case kOpXor:
      opcode = kThumb2EorRRI8;
      alt_opcode = kThumb2EorRRR;
      break;
    case kOpMul:
      //TUNING: power of 2, shift & add
      mod_imm = -1;
      alt_opcode = kThumb2MulRRR;
      break;
    case kOpCmp: {
      int mod_imm = ModifiedImmediate(value);
      LIR* res;
      if (mod_imm >= 0) {
        res = NewLIR2(cu, kThumb2CmpRI8, r_src1, mod_imm);
      } else {
        int r_tmp = AllocTemp(cu);
        res = LoadConstant(cu, r_tmp, value);
        OpRegReg(cu, kOpCmp, r_src1, r_tmp);
        FreeTemp(cu, r_tmp);
      }
      return res;
    }
    default:
      LOG(FATAL) << "Bad opcode: " << op;
  }

  if (mod_imm >= 0) {
    return NewLIR3(cu, opcode, r_dest, r_src1, mod_imm);
  } else {
    int r_scratch = AllocTemp(cu);
    LoadConstant(cu, r_scratch, value);
    if (EncodingMap[alt_opcode].flags & IS_QUAD_OP)
      res = NewLIR4(cu, alt_opcode, r_dest, r_src1, r_scratch, 0);
    else
      res = NewLIR3(cu, alt_opcode, r_dest, r_src1, r_scratch);
    FreeTemp(cu, r_scratch);
    return res;
  }
}

/* Handle Thumb-only variants here - otherwise punt to OpRegRegImm */
LIR* OpRegImm(CompilationUnit* cu, OpKind op, int r_dest_src1, int value)
{
  bool neg = (value < 0);
  int abs_value = (neg) ? -value : value;
  bool short_form = (((abs_value & 0xff) == abs_value) && ARM_LOWREG(r_dest_src1));
  ArmOpcode opcode = kThumbBkpt;
  switch (op) {
    case kOpAdd:
      if ( !neg && (r_dest_src1 == r13sp) && (value <= 508)) { /* sp */
        DCHECK_EQ((value & 0x3), 0);
        return NewLIR1(cu, kThumbAddSpI7, value >> 2);
      } else if (short_form) {
        opcode = (neg) ? kThumbSubRI8 : kThumbAddRI8;
      }
      break;
    case kOpSub:
      if (!neg && (r_dest_src1 == r13sp) && (value <= 508)) { /* sp */
        DCHECK_EQ((value & 0x3), 0);
        return NewLIR1(cu, kThumbSubSpI7, value >> 2);
      } else if (short_form) {
        opcode = (neg) ? kThumbAddRI8 : kThumbSubRI8;
      }
      break;
    case kOpCmp:
      if (ARM_LOWREG(r_dest_src1) && short_form)
        opcode = (short_form) ?  kThumbCmpRI8 : kThumbCmpRR;
      else if (ARM_LOWREG(r_dest_src1))
        opcode = kThumbCmpRR;
      else {
        short_form = false;
        opcode = kThumbCmpHL;
      }
      break;
    default:
      /* Punt to OpRegRegImm - if bad case catch it there */
      short_form = false;
      break;
  }
  if (short_form)
    return NewLIR2(cu, opcode, r_dest_src1, abs_value);
  else {
    return OpRegRegImm(cu, op, r_dest_src1, r_dest_src1, value);
  }
}

/*
 * Determine whether value can be encoded as a Thumb2 floating point
 * immediate.  If not, return -1.  If so return encoded 8-bit value.
 */
static int EncodeImmDoubleHigh(int value)
{
  int res;
  int bit_a =  (value & 0x80000000) >> 31;
  int not_bit_b = (value & 0x40000000) >> 30;
  int bit_b =  (value & 0x20000000) >> 29;
  int b_smear =  (value & 0x3fc00000) >> 22;
  int slice =   (value & 0x003f0000) >> 16;
  int zeroes =  (value & 0x0000ffff);
  if (zeroes != 0)
    return -1;
  if (bit_b) {
    if ((not_bit_b != 0) || (b_smear != 0xff))
      return -1;
  } else {
    if ((not_bit_b != 1) || (b_smear != 0x0))
      return -1;
  }
  res = (bit_a << 7) | (bit_b << 6) | slice;
  return res;
}

static int EncodeImmDouble(int val_lo, int val_hi)
{
  int res = -1;
  if (val_lo == 0)
    res = EncodeImmDoubleHigh(val_hi);
  return res;
}

LIR* LoadConstantValueWide(CompilationUnit* cu, int r_dest_lo, int r_dest_hi,
               int val_lo, int val_hi)
{
  int encoded_imm = EncodeImmDouble(val_lo, val_hi);
  LIR* res;
  if (ARM_FPREG(r_dest_lo)) {
    if (encoded_imm >= 0) {
      res = NewLIR2(cu, kThumb2Vmovd_IMM8, S2d(r_dest_lo, r_dest_hi),
              encoded_imm);
    } else {
      LIR* data_target = ScanLiteralPoolWide(cu->literal_list, val_lo, val_hi);
      if (data_target == NULL) {
        data_target = AddWideData(cu, &cu->literal_list, val_lo, val_hi);
      }
      LIR* load_pc_rel =
          RawLIR(cu, cu->current_dalvik_offset, kThumb2Vldrd,
                 S2d(r_dest_lo, r_dest_hi), r15pc, 0, 0, 0, data_target);
      SetMemRefType(load_pc_rel, true, kLiteral);
      load_pc_rel->alias_info = reinterpret_cast<uintptr_t>(data_target);
      AppendLIR(cu, load_pc_rel);
      res = load_pc_rel;
    }
  } else {
    res = LoadConstantNoClobber(cu, r_dest_lo, val_lo);
    LoadConstantNoClobber(cu, r_dest_hi, val_hi);
  }
  return res;
}

int EncodeShift(int code, int amount) {
  return ((amount & 0x1f) << 2) | code;
}

LIR* LoadBaseIndexed(CompilationUnit* cu, int rBase, int r_index, int r_dest,
                     int scale, OpSize size)
{
  bool all_low_regs = ARM_LOWREG(rBase) && ARM_LOWREG(r_index) && ARM_LOWREG(r_dest);
  LIR* load;
  ArmOpcode opcode = kThumbBkpt;
  bool thumb_form = (all_low_regs && (scale == 0));
  int reg_ptr;

  if (ARM_FPREG(r_dest)) {
    if (ARM_SINGLEREG(r_dest)) {
      DCHECK((size == kWord) || (size == kSingle));
      opcode = kThumb2Vldrs;
      size = kSingle;
    } else {
      DCHECK(ARM_DOUBLEREG(r_dest));
      DCHECK((size == kLong) || (size == kDouble));
      DCHECK_EQ((r_dest & 0x1), 0);
      opcode = kThumb2Vldrd;
      size = kDouble;
    }
  } else {
    if (size == kSingle)
      size = kWord;
  }

  switch (size) {
    case kDouble: // fall-through
    case kSingle:
      reg_ptr = AllocTemp(cu);
      if (scale) {
        NewLIR4(cu, kThumb2AddRRR, reg_ptr, rBase, r_index,
                EncodeShift(kArmLsl, scale));
      } else {
        OpRegRegReg(cu, kOpAdd, reg_ptr, rBase, r_index);
      }
      load = NewLIR3(cu, opcode, r_dest, reg_ptr, 0);
      FreeTemp(cu, reg_ptr);
      return load;
    case kWord:
      opcode = (thumb_form) ? kThumbLdrRRR : kThumb2LdrRRR;
      break;
    case kUnsignedHalf:
      opcode = (thumb_form) ? kThumbLdrhRRR : kThumb2LdrhRRR;
      break;
    case kSignedHalf:
      opcode = (thumb_form) ? kThumbLdrshRRR : kThumb2LdrshRRR;
      break;
    case kUnsignedByte:
      opcode = (thumb_form) ? kThumbLdrbRRR : kThumb2LdrbRRR;
      break;
    case kSignedByte:
      opcode = (thumb_form) ? kThumbLdrsbRRR : kThumb2LdrsbRRR;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }
  if (thumb_form)
    load = NewLIR3(cu, opcode, r_dest, rBase, r_index);
  else
    load = NewLIR4(cu, opcode, r_dest, rBase, r_index, scale);

  return load;
}

LIR* StoreBaseIndexed(CompilationUnit* cu, int rBase, int r_index, int r_src,
                      int scale, OpSize size)
{
  bool all_low_regs = ARM_LOWREG(rBase) && ARM_LOWREG(r_index) && ARM_LOWREG(r_src);
  LIR* store;
  ArmOpcode opcode = kThumbBkpt;
  bool thumb_form = (all_low_regs && (scale == 0));
  int reg_ptr;

  if (ARM_FPREG(r_src)) {
    if (ARM_SINGLEREG(r_src)) {
      DCHECK((size == kWord) || (size == kSingle));
      opcode = kThumb2Vstrs;
      size = kSingle;
    } else {
      DCHECK(ARM_DOUBLEREG(r_src));
      DCHECK((size == kLong) || (size == kDouble));
      DCHECK_EQ((r_src & 0x1), 0);
      opcode = kThumb2Vstrd;
      size = kDouble;
    }
  } else {
    if (size == kSingle)
      size = kWord;
  }

  switch (size) {
    case kDouble: // fall-through
    case kSingle:
      reg_ptr = AllocTemp(cu);
      if (scale) {
        NewLIR4(cu, kThumb2AddRRR, reg_ptr, rBase, r_index,
                EncodeShift(kArmLsl, scale));
      } else {
        OpRegRegReg(cu, kOpAdd, reg_ptr, rBase, r_index);
      }
      store = NewLIR3(cu, opcode, r_src, reg_ptr, 0);
      FreeTemp(cu, reg_ptr);
      return store;
    case kWord:
      opcode = (thumb_form) ? kThumbStrRRR : kThumb2StrRRR;
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      opcode = (thumb_form) ? kThumbStrhRRR : kThumb2StrhRRR;
      break;
    case kUnsignedByte:
    case kSignedByte:
      opcode = (thumb_form) ? kThumbStrbRRR : kThumb2StrbRRR;
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }
  if (thumb_form)
    store = NewLIR3(cu, opcode, r_src, rBase, r_index);
  else
    store = NewLIR4(cu, opcode, r_src, rBase, r_index, scale);

  return store;
}

/*
 * Load value from base + displacement.  Optionally perform null check
 * on base (which must have an associated s_reg and MIR).  If not
 * performing null check, incoming MIR can be null.
 */
LIR* LoadBaseDispBody(CompilationUnit* cu, int rBase,
                      int displacement, int r_dest, int r_dest_hi, OpSize size,
                      int s_reg)
{
  LIR* res;
  LIR* load;
  ArmOpcode opcode = kThumbBkpt;
  bool short_form = false;
  bool thumb2Form = (displacement < 4092 && displacement >= 0);
  bool all_low_regs = (ARM_LOWREG(rBase) && ARM_LOWREG(r_dest));
  int encoded_disp = displacement;
  bool is64bit = false;
  switch (size) {
    case kDouble:
    case kLong:
      is64bit = true;
      if (ARM_FPREG(r_dest)) {
        if (ARM_SINGLEREG(r_dest)) {
          DCHECK(ARM_FPREG(r_dest_hi));
          r_dest = S2d(r_dest, r_dest_hi);
        }
        opcode = kThumb2Vldrd;
        if (displacement <= 1020) {
          short_form = true;
          encoded_disp >>= 2;
        }
        break;
      } else {
        res = LoadBaseDispBody(cu, rBase, displacement, r_dest,
                               -1, kWord, s_reg);
        LoadBaseDispBody(cu, rBase, displacement + 4, r_dest_hi,
                         -1, kWord, INVALID_SREG);
        return res;
      }
    case kSingle:
    case kWord:
      if (ARM_FPREG(r_dest)) {
        opcode = kThumb2Vldrs;
        if (displacement <= 1020) {
          short_form = true;
          encoded_disp >>= 2;
        }
        break;
      }
      if (ARM_LOWREG(r_dest) && (rBase == r15pc) &&
          (displacement <= 1020) && (displacement >= 0)) {
        short_form = true;
        encoded_disp >>= 2;
        opcode = kThumbLdrPcRel;
      } else if (ARM_LOWREG(r_dest) && (rBase == r13sp) &&
          (displacement <= 1020) && (displacement >= 0)) {
        short_form = true;
        encoded_disp >>= 2;
        opcode = kThumbLdrSpRel;
      } else if (all_low_regs && displacement < 128 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x3), 0);
        short_form = true;
        encoded_disp >>= 2;
        opcode = kThumbLdrRRI5;
      } else if (thumb2Form) {
        short_form = true;
        opcode = kThumb2LdrRRI12;
      }
      break;
    case kUnsignedHalf:
      if (all_low_regs && displacement < 64 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x1), 0);
        short_form = true;
        encoded_disp >>= 1;
        opcode = kThumbLdrhRRI5;
      } else if (displacement < 4092 && displacement >= 0) {
        short_form = true;
        opcode = kThumb2LdrhRRI12;
      }
      break;
    case kSignedHalf:
      if (thumb2Form) {
        short_form = true;
        opcode = kThumb2LdrshRRI12;
      }
      break;
    case kUnsignedByte:
      if (all_low_regs && displacement < 32 && displacement >= 0) {
        short_form = true;
        opcode = kThumbLdrbRRI5;
      } else if (thumb2Form) {
        short_form = true;
        opcode = kThumb2LdrbRRI12;
      }
      break;
    case kSignedByte:
      if (thumb2Form) {
        short_form = true;
        opcode = kThumb2LdrsbRRI12;
      }
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }

  if (short_form) {
    load = res = NewLIR3(cu, opcode, r_dest, rBase, encoded_disp);
  } else {
    int reg_offset = AllocTemp(cu);
    res = LoadConstant(cu, reg_offset, encoded_disp);
    load = LoadBaseIndexed(cu, rBase, reg_offset, r_dest, 0, size);
    FreeTemp(cu, reg_offset);
  }

  // TODO: in future may need to differentiate Dalvik accesses w/ spills
  if (rBase == rARM_SP) {
    AnnotateDalvikRegAccess(load, displacement >> 2, true /* is_load */, is64bit);
  }
  return load;
}

LIR* LoadBaseDisp(CompilationUnit* cu, int rBase,
                  int displacement, int r_dest, OpSize size, int s_reg)
{
  return LoadBaseDispBody(cu, rBase, displacement, r_dest, -1, size,
                          s_reg);
}

 LIR* LoadBaseDispWide(CompilationUnit* cu, int rBase,
                       int displacement, int r_dest_lo, int r_dest_hi, int s_reg)
{
  return LoadBaseDispBody(cu, rBase, displacement, r_dest_lo, r_dest_hi,
                          kLong, s_reg);
}


LIR* StoreBaseDispBody(CompilationUnit* cu, int rBase, int displacement,
                       int r_src, int r_src_hi, OpSize size)
{
  LIR* res, *store;
  ArmOpcode opcode = kThumbBkpt;
  bool short_form = false;
  bool thumb2Form = (displacement < 4092 && displacement >= 0);
  bool all_low_regs = (ARM_LOWREG(rBase) && ARM_LOWREG(r_src));
  int encoded_disp = displacement;
  bool is64bit = false;
  switch (size) {
    case kLong:
    case kDouble:
      is64bit = true;
      if (!ARM_FPREG(r_src)) {
        res = StoreBaseDispBody(cu, rBase, displacement, r_src, -1, kWord);
        StoreBaseDispBody(cu, rBase, displacement + 4, r_src_hi, -1, kWord);
        return res;
      }
      if (ARM_SINGLEREG(r_src)) {
        DCHECK(ARM_FPREG(r_src_hi));
        r_src = S2d(r_src, r_src_hi);
      }
      opcode = kThumb2Vstrd;
      if (displacement <= 1020) {
        short_form = true;
        encoded_disp >>= 2;
      }
      break;
    case kSingle:
    case kWord:
      if (ARM_FPREG(r_src)) {
        DCHECK(ARM_SINGLEREG(r_src));
        opcode = kThumb2Vstrs;
        if (displacement <= 1020) {
          short_form = true;
          encoded_disp >>= 2;
        }
      break;
      }
      if (all_low_regs && displacement < 128 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x3), 0);
        short_form = true;
        encoded_disp >>= 2;
        opcode = kThumbStrRRI5;
      } else if (thumb2Form) {
        short_form = true;
        opcode = kThumb2StrRRI12;
      }
      break;
    case kUnsignedHalf:
    case kSignedHalf:
      if (all_low_regs && displacement < 64 && displacement >= 0) {
        DCHECK_EQ((displacement & 0x1), 0);
        short_form = true;
        encoded_disp >>= 1;
        opcode = kThumbStrhRRI5;
      } else if (thumb2Form) {
        short_form = true;
        opcode = kThumb2StrhRRI12;
      }
      break;
    case kUnsignedByte:
    case kSignedByte:
      if (all_low_regs && displacement < 32 && displacement >= 0) {
        short_form = true;
        opcode = kThumbStrbRRI5;
      } else if (thumb2Form) {
        short_form = true;
        opcode = kThumb2StrbRRI12;
      }
      break;
    default:
      LOG(FATAL) << "Bad size: " << size;
  }
  if (short_form) {
    store = res = NewLIR3(cu, opcode, r_src, rBase, encoded_disp);
  } else {
    int r_scratch = AllocTemp(cu);
    res = LoadConstant(cu, r_scratch, encoded_disp);
    store = StoreBaseIndexed(cu, rBase, r_scratch, r_src, 0, size);
    FreeTemp(cu, r_scratch);
  }

  // TODO: In future, may need to differentiate Dalvik & spill accesses
  if (rBase == rARM_SP) {
    AnnotateDalvikRegAccess(store, displacement >> 2, false /* is_load */,
                            is64bit);
  }
  return res;
}

LIR* StoreBaseDisp(CompilationUnit* cu, int rBase, int displacement,
                   int r_src, OpSize size)
{
  return StoreBaseDispBody(cu, rBase, displacement, r_src, -1, size);
}

LIR* StoreBaseDispWide(CompilationUnit* cu, int rBase, int displacement,
                       int r_src_lo, int r_src_hi)
{
  return StoreBaseDispBody(cu, rBase, displacement, r_src_lo, r_src_hi, kLong);
}

void LoadPair(CompilationUnit* cu, int base, int low_reg, int high_reg)
{
  LoadBaseDispWide(cu, base, 0, low_reg, high_reg, INVALID_SREG);
}

LIR* FpRegCopy(CompilationUnit* cu, int r_dest, int r_src)
{
  int opcode;
  DCHECK_EQ(ARM_DOUBLEREG(r_dest), ARM_DOUBLEREG(r_src));
  if (ARM_DOUBLEREG(r_dest)) {
    opcode = kThumb2Vmovd;
  } else {
    if (ARM_SINGLEREG(r_dest)) {
      opcode = ARM_SINGLEREG(r_src) ? kThumb2Vmovs : kThumb2Fmsr;
    } else {
      DCHECK(ARM_SINGLEREG(r_src));
      opcode = kThumb2Fmrs;
    }
  }
  LIR* res = RawLIR(cu, cu->current_dalvik_offset, opcode, r_dest, r_src);
  if (!(cu->disable_opt & (1 << kSafeOptimizations)) && r_dest == r_src) {
    res->flags.is_nop = true;
  }
  return res;
}

LIR* OpThreadMem(CompilationUnit* cu, OpKind op, int thread_offset)
{
  LOG(FATAL) << "Unexpected use of OpThreadMem for Arm";
  return NULL;
}

LIR* OpMem(CompilationUnit* cu, OpKind op, int rBase, int disp)
{
  LOG(FATAL) << "Unexpected use of OpMem for Arm";
  return NULL;
}

LIR* StoreBaseIndexedDisp(CompilationUnit *cu,
                          int rBase, int r_index, int scale, int displacement,
                          int r_src, int r_src_hi,
                          OpSize size, int s_reg)
{
  LOG(FATAL) << "Unexpected use of StoreBaseIndexedDisp for Arm";
  return NULL;
}

LIR* OpRegMem(CompilationUnit *cu, OpKind op, int r_dest, int rBase,
              int offset)
{
  LOG(FATAL) << "Unexpected use of OpRegMem for Arm";
  return NULL;
}

LIR* LoadBaseIndexedDisp(CompilationUnit *cu,
                         int rBase, int r_index, int scale, int displacement,
                         int r_dest, int r_dest_hi,
                         OpSize size, int s_reg)
{
  LOG(FATAL) << "Unexpected use of LoadBaseIndexedDisp for Arm";
  return NULL;
}

}  // namespace art

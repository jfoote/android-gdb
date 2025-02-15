/* Common target dependent code for GDB on ARM systems.

   Copyright (C) 1988-1989, 1991-1993, 1995-1996, 1998-2012 Free
   Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include <ctype.h>		/* XXX for isupper ().  */

#include "defs.h"
#include "frame.h"
#include "inferior.h"
#include "gdbcmd.h"
#include "gdbcore.h"
#include "gdb_string.h"
#include "dis-asm.h"		/* For register styles.  */
#include "regcache.h"
#include "reggroups.h"
#include "doublest.h"
#include "value.h"
#include "arch-utils.h"
#include "osabi.h"
#include "frame-unwind.h"
#include "frame-base.h"
#include "trad-frame.h"
#include "objfiles.h"
#include "dwarf2-frame.h"
#include "gdbtypes.h"
#include "prologue-value.h"
#include "target-descriptions.h"
#include "user-regs.h"
#include "observer.h"

#include "arm-tdep.h"
#include "gdb/sim-arm.h"

#include "elf-bfd.h"
#include "coff/internal.h"
#include "elf/arm.h"

#include "gdb_assert.h"
#include "vec.h"

#include "features/arm-with-m.c"
#include "features/arm-with-iwmmxt.c"
#include "features/arm-with-vfpv2.c"
#include "features/arm-with-vfpv3.c"
#include "features/arm-with-neon.c"

static int arm_debug;

/* Macros for setting and testing a bit in a minimal symbol that marks
   it as Thumb function.  The MSB of the minimal symbol's "info" field
   is used for this purpose.

   MSYMBOL_SET_SPECIAL	Actually sets the "special" bit.
   MSYMBOL_IS_SPECIAL   Tests the "special" bit in a minimal symbol.  */

#define MSYMBOL_SET_SPECIAL(msym)				\
	MSYMBOL_TARGET_FLAG_1 (msym) = 1

#define MSYMBOL_IS_SPECIAL(msym)				\
	MSYMBOL_TARGET_FLAG_1 (msym)

/* Per-objfile data used for mapping symbols.  */
static const struct objfile_data *arm_objfile_data_key;

struct arm_mapping_symbol
{
  bfd_vma value;
  char type;
};
typedef struct arm_mapping_symbol arm_mapping_symbol_s;
DEF_VEC_O(arm_mapping_symbol_s);

struct arm_per_objfile
{
  VEC(arm_mapping_symbol_s) **section_maps;
};

/* The list of available "set arm ..." and "show arm ..." commands.  */
static struct cmd_list_element *setarmcmdlist = NULL;
static struct cmd_list_element *showarmcmdlist = NULL;

/* The type of floating-point to use.  Keep this in sync with enum
   arm_float_model, and the help string in _initialize_arm_tdep.  */
static const char *fp_model_strings[] =
{
  "auto",
  "softfpa",
  "fpa",
  "softvfp",
  "vfp",
  NULL
};

/* A variable that can be configured by the user.  */
static enum arm_float_model arm_fp_model = ARM_FLOAT_AUTO;
static const char *current_fp_model = "auto";

/* The ABI to use.  Keep this in sync with arm_abi_kind.  */
static const char *arm_abi_strings[] =
{
  "auto",
  "APCS",
  "AAPCS",
  NULL
};

/* A variable that can be configured by the user.  */
static enum arm_abi_kind arm_abi_global = ARM_ABI_AUTO;
static const char *arm_abi_string = "auto";

/* The execution mode to assume.  */
static const char *arm_mode_strings[] =
  {
    "auto",
    "arm",
    "thumb",
    NULL
  };

static const char *arm_fallback_mode_string = "auto";
static const char *arm_force_mode_string = "auto";

/* Internal override of the execution mode.  -1 means no override,
   0 means override to ARM mode, 1 means override to Thumb mode.
   The effect is the same as if arm_force_mode has been set by the
   user (except the internal override has precedence over a user's
   arm_force_mode override).  */
static int arm_override_mode = -1;

/* Number of different reg name sets (options).  */
static int num_disassembly_options;

/* The standard register names, and all the valid aliases for them.  Note
   that `fp', `sp' and `pc' are not added in this alias list, because they
   have been added as builtin user registers in
   std-regs.c:_initialize_frame_reg.  */
static const struct
{
  const char *name;
  int regnum;
} arm_register_aliases[] = {
  /* Basic register numbers.  */
  { "r0", 0 },
  { "r1", 1 },
  { "r2", 2 },
  { "r3", 3 },
  { "r4", 4 },
  { "r5", 5 },
  { "r6", 6 },
  { "r7", 7 },
  { "r8", 8 },
  { "r9", 9 },
  { "r10", 10 },
  { "r11", 11 },
  { "r12", 12 },
  { "r13", 13 },
  { "r14", 14 },
  { "r15", 15 },
  /* Synonyms (argument and variable registers).  */
  { "a1", 0 },
  { "a2", 1 },
  { "a3", 2 },
  { "a4", 3 },
  { "v1", 4 },
  { "v2", 5 },
  { "v3", 6 },
  { "v4", 7 },
  { "v5", 8 },
  { "v6", 9 },
  { "v7", 10 },
  { "v8", 11 },
  /* Other platform-specific names for r9.  */
  { "sb", 9 },
  { "tr", 9 },
  /* Special names.  */
  { "ip", 12 },
  { "lr", 14 },
  /* Names used by GCC (not listed in the ARM EABI).  */
  { "sl", 10 },
  /* A special name from the older ATPCS.  */
  { "wr", 7 },
};

static const char *const arm_register_names[] =
{"r0",  "r1",  "r2",  "r3",	/*  0  1  2  3 */
 "r4",  "r5",  "r6",  "r7",	/*  4  5  6  7 */
 "r8",  "r9",  "r10", "r11",	/*  8  9 10 11 */
 "r12", "sp",  "lr",  "pc",	/* 12 13 14 15 */
 "f0",  "f1",  "f2",  "f3",	/* 16 17 18 19 */
 "f4",  "f5",  "f6",  "f7",	/* 20 21 22 23 */
 "fps", "cpsr" };		/* 24 25       */

/* Valid register name styles.  */
static const char **valid_disassembly_styles;

/* Disassembly style to use. Default to "std" register names.  */
static const char *disassembly_style;

/* This is used to keep the bfd arch_info in sync with the disassembly
   style.  */
static void set_disassembly_style_sfunc(char *, int,
					 struct cmd_list_element *);
static void set_disassembly_style (void);

static void convert_from_extended (const struct floatformat *, const void *,
				   void *, int);
static void convert_to_extended (const struct floatformat *, void *,
				 const void *, int);

static enum register_status arm_neon_quad_read (struct gdbarch *gdbarch,
						struct regcache *regcache,
						int regnum, gdb_byte *buf);
static void arm_neon_quad_write (struct gdbarch *gdbarch,
				 struct regcache *regcache,
				 int regnum, const gdb_byte *buf);

static int thumb_insn_size (unsigned short inst1);

struct arm_prologue_cache
{
  /* The stack pointer at the time this frame was created; i.e. the
     caller's stack pointer when this function was called.  It is used
     to identify this frame.  */
  CORE_ADDR prev_sp;

  /* The frame base for this frame is just prev_sp - frame size.
     FRAMESIZE is the distance from the frame pointer to the
     initial stack pointer.  */

  int framesize;

  /* The register used to hold the frame pointer for this frame.  */
  int framereg;

  /* Saved register offsets.  */
  struct trad_frame_saved_reg *saved_regs;
};

static CORE_ADDR arm_analyze_prologue (struct gdbarch *gdbarch,
				       CORE_ADDR prologue_start,
				       CORE_ADDR prologue_end,
				       struct arm_prologue_cache *cache);

/* Architecture version for displaced stepping.  This effects the behaviour of
   certain instructions, and really should not be hard-wired.  */

#define DISPLACED_STEPPING_ARCH_VERSION		5

/* Addresses for calling Thumb functions have the bit 0 set.
   Here are some macros to test, set, or clear bit 0 of addresses.  */
#define IS_THUMB_ADDR(addr)	((addr) & 1)
#define MAKE_THUMB_ADDR(addr)	((addr) | 1)
#define UNMAKE_THUMB_ADDR(addr) ((addr) & ~1)

/* Set to true if the 32-bit mode is in use.  */

int arm_apcs_32 = 1;

/* Return the bit mask in ARM_PS_REGNUM that indicates Thumb mode.  */

int
arm_psr_thumb_bit (struct gdbarch *gdbarch)
{
  if (gdbarch_tdep (gdbarch)->is_m)
    return XPSR_T;
  else
    return CPSR_T;
}

/* Determine if FRAME is executing in Thumb mode.  */

int
arm_frame_is_thumb (struct frame_info *frame)
{
  CORE_ADDR cpsr;
  ULONGEST t_bit = arm_psr_thumb_bit (get_frame_arch (frame));

  /* Every ARM frame unwinder can unwind the T bit of the CPSR, either
     directly (from a signal frame or dummy frame) or by interpreting
     the saved LR (from a prologue or DWARF frame).  So consult it and
     trust the unwinders.  */
  cpsr = get_frame_register_unsigned (frame, ARM_PS_REGNUM);

  return (cpsr & t_bit) != 0;
}

/* Callback for VEC_lower_bound.  */

static inline int
arm_compare_mapping_symbols (const struct arm_mapping_symbol *lhs,
			     const struct arm_mapping_symbol *rhs)
{
  return lhs->value < rhs->value;
}

/* Search for the mapping symbol covering MEMADDR.  If one is found,
   return its type.  Otherwise, return 0.  If START is non-NULL,
   set *START to the location of the mapping symbol.  */

static char
arm_find_mapping_symbol (CORE_ADDR memaddr, CORE_ADDR *start)
{
  struct obj_section *sec;

  /* If there are mapping symbols, consult them.  */
  sec = find_pc_section (memaddr);
  if (sec != NULL)
    {
      struct arm_per_objfile *data;
      VEC(arm_mapping_symbol_s) *map;
      struct arm_mapping_symbol map_key = { memaddr - obj_section_addr (sec),
					    0 };
      unsigned int idx;

      data = objfile_data (sec->objfile, arm_objfile_data_key);
      if (data != NULL)
	{
	  map = data->section_maps[sec->the_bfd_section->index];
	  if (!VEC_empty (arm_mapping_symbol_s, map))
	    {
	      struct arm_mapping_symbol *map_sym;

	      idx = VEC_lower_bound (arm_mapping_symbol_s, map, &map_key,
				     arm_compare_mapping_symbols);

	      /* VEC_lower_bound finds the earliest ordered insertion
		 point.  If the following symbol starts at this exact
		 address, we use that; otherwise, the preceding
		 mapping symbol covers this address.  */
	      if (idx < VEC_length (arm_mapping_symbol_s, map))
		{
		  map_sym = VEC_index (arm_mapping_symbol_s, map, idx);
		  if (map_sym->value == map_key.value)
		    {
		      if (start)
			*start = map_sym->value + obj_section_addr (sec);
		      return map_sym->type;
		    }
		}

	      if (idx > 0)
		{
		  map_sym = VEC_index (arm_mapping_symbol_s, map, idx - 1);
		  if (start)
		    *start = map_sym->value + obj_section_addr (sec);
		  return map_sym->type;
		}
	    }
	}
    }

  return 0;
}

/* Determine if the program counter specified in MEMADDR is in a Thumb
   function.  This function should be called for addresses unrelated to
   any executing frame; otherwise, prefer arm_frame_is_thumb.  */

int
arm_pc_is_thumb (struct gdbarch *gdbarch, CORE_ADDR memaddr)
{
  struct obj_section *sec;
  struct minimal_symbol *sym;
  char type;
  struct displaced_step_closure* dsc
    = get_displaced_step_closure_by_addr(memaddr);

  /* If checking the mode of displaced instruction in copy area, the mode
     should be determined by instruction on the original address.  */
  if (dsc)
    {
      if (debug_displaced)
	fprintf_unfiltered (gdb_stdlog,
			    "displaced: check mode of %.8lx instead of %.8lx\n",
			    (unsigned long) dsc->insn_addr,
			    (unsigned long) memaddr);
      memaddr = dsc->insn_addr;
    }

  /* If bit 0 of the address is set, assume this is a Thumb address.  */
  if (IS_THUMB_ADDR (memaddr))
    return 1;

  /* Respect internal mode override if active.  */
  if (arm_override_mode != -1)
    return arm_override_mode;

  /* If the user wants to override the symbol table, let him.  */
  if (strcmp (arm_force_mode_string, "arm") == 0)
    return 0;
  if (strcmp (arm_force_mode_string, "thumb") == 0)
    return 1;

  /* ARM v6-M and v7-M are always in Thumb mode.  */
  if (gdbarch_tdep (gdbarch)->is_m)
    return 1;

  /* If there are mapping symbols, consult them.  */
  type = arm_find_mapping_symbol (memaddr, NULL);
  if (type)
    return type == 't';

  /* Thumb functions have a "special" bit set in minimal symbols.  */
  sym = lookup_minimal_symbol_by_pc (memaddr);
  if (sym)
    return (MSYMBOL_IS_SPECIAL (sym));

  /* If the user wants to override the fallback mode, let them.  */
  if (strcmp (arm_fallback_mode_string, "arm") == 0)
    return 0;
  if (strcmp (arm_fallback_mode_string, "thumb") == 0)
    return 1;

  /* If we couldn't find any symbol, but we're talking to a running
     target, then trust the current value of $cpsr.  This lets
     "display/i $pc" always show the correct mode (though if there is
     a symbol table we will not reach here, so it still may not be
     displayed in the mode it will be executed).  */
  if (target_has_registers)
    return arm_frame_is_thumb (get_current_frame ());

  /* Otherwise we're out of luck; we assume ARM.  */
  return 0;
}

/* Remove useless bits from addresses in a running program.  */
static CORE_ADDR
arm_addr_bits_remove (struct gdbarch *gdbarch, CORE_ADDR val)
{
  if (arm_apcs_32)
    return UNMAKE_THUMB_ADDR (val);
  else
    return (val & 0x03fffffc);
}

/* When reading symbols, we need to zap the low bit of the address,
   which may be set to 1 for Thumb functions.  */
static CORE_ADDR
arm_smash_text_address (struct gdbarch *gdbarch, CORE_ADDR val)
{
  return val & ~1;
}

/* Return 1 if PC is the start of a compiler helper function which
   can be safely ignored during prologue skipping.  IS_THUMB is true
   if the function is known to be a Thumb function due to the way it
   is being called.  */
static int
skip_prologue_function (struct gdbarch *gdbarch, CORE_ADDR pc, int is_thumb)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  struct minimal_symbol *msym;

  msym = lookup_minimal_symbol_by_pc (pc);
  if (msym != NULL
      && SYMBOL_VALUE_ADDRESS (msym) == pc
      && SYMBOL_LINKAGE_NAME (msym) != NULL)
    {
      const char *name = SYMBOL_LINKAGE_NAME (msym);

      /* The GNU linker's Thumb call stub to foo is named
	 __foo_from_thumb.  */
      if (strstr (name, "_from_thumb") != NULL)
	name += 2;

      /* On soft-float targets, __truncdfsf2 is called to convert promoted
	 arguments to their argument types in non-prototyped
	 functions.  */
      if (strncmp (name, "__truncdfsf2", strlen ("__truncdfsf2")) == 0)
	return 1;
      if (strncmp (name, "__aeabi_d2f", strlen ("__aeabi_d2f")) == 0)
	return 1;

      /* Internal functions related to thread-local storage.  */
      if (strncmp (name, "__tls_get_addr", strlen ("__tls_get_addr")) == 0)
	return 1;
      if (strncmp (name, "__aeabi_read_tp", strlen ("__aeabi_read_tp")) == 0)
	return 1;
    }
  else
    {
      /* If we run against a stripped glibc, we may be unable to identify
	 special functions by name.  Check for one important case,
	 __aeabi_read_tp, by comparing the *code* against the default
	 implementation (this is hand-written ARM assembler in glibc).  */

      if (!is_thumb
	  && read_memory_unsigned_integer (pc, 4, byte_order_for_code)
	     == 0xe3e00a0f /* mov r0, #0xffff0fff */
	  && read_memory_unsigned_integer (pc + 4, 4, byte_order_for_code)
	     == 0xe240f01f) /* sub pc, r0, #31 */
	return 1;
    }

  return 0;
}

/* Support routines for instruction parsing.  */
#define submask(x) ((1L << ((x) + 1)) - 1)
#define bit(obj,st) (((obj) >> (st)) & 1)
#define bits(obj,st,fn) (((obj) >> (st)) & submask ((fn) - (st)))
#define sbits(obj,st,fn) \
  ((long) (bits(obj,st,fn) | ((long) bit(obj,fn) * ~ submask (fn - st))))
#define BranchDest(addr,instr) \
  ((CORE_ADDR) (((long) (addr)) + 8 + (sbits (instr, 0, 23) << 2)))

/* Extract the immediate from instruction movw/movt of encoding T.  INSN1 is
   the first 16-bit of instruction, and INSN2 is the second 16-bit of
   instruction.  */
#define EXTRACT_MOVW_MOVT_IMM_T(insn1, insn2) \
  ((bits ((insn1), 0, 3) << 12)               \
   | (bits ((insn1), 10, 10) << 11)           \
   | (bits ((insn2), 12, 14) << 8)            \
   | bits ((insn2), 0, 7))

/* Extract the immediate from instruction movw/movt of encoding A.  INSN is
   the 32-bit instruction.  */
#define EXTRACT_MOVW_MOVT_IMM_A(insn) \
  ((bits ((insn), 16, 19) << 12) \
   | bits ((insn), 0, 11))

/* Decode immediate value; implements ThumbExpandImmediate pseudo-op.  */

static unsigned int
thumb_expand_immediate (unsigned int imm)
{
  unsigned int count = imm >> 7;

  if (count < 8)
    switch (count / 2)
      {
      case 0:
	return imm & 0xff;
      case 1:
	return (imm & 0xff) | ((imm & 0xff) << 16);
      case 2:
	return ((imm & 0xff) << 8) | ((imm & 0xff) << 24);
      case 3:
	return (imm & 0xff) | ((imm & 0xff) << 8)
		| ((imm & 0xff) << 16) | ((imm & 0xff) << 24);
      }

  return (0x80 | (imm & 0x7f)) << (32 - count);
}

/* Return 1 if the 16-bit Thumb instruction INST might change
   control flow, 0 otherwise.  */

static int
thumb_instruction_changes_pc (unsigned short inst)
{
  if ((inst & 0xff00) == 0xbd00)	/* pop {rlist, pc} */
    return 1;

  if ((inst & 0xf000) == 0xd000)	/* conditional branch */
    return 1;

  if ((inst & 0xf800) == 0xe000)	/* unconditional branch */
    return 1;

  if ((inst & 0xff00) == 0x4700)	/* bx REG, blx REG */
    return 1;

  if ((inst & 0xff87) == 0x4687)	/* mov pc, REG */
    return 1;

  if ((inst & 0xf500) == 0xb100)	/* CBNZ or CBZ.  */
    return 1;

  return 0;
}

/* Return 1 if the 32-bit Thumb instruction in INST1 and INST2
   might change control flow, 0 otherwise.  */

static int
thumb2_instruction_changes_pc (unsigned short inst1, unsigned short inst2)
{
  if ((inst1 & 0xf800) == 0xf000 && (inst2 & 0x8000) == 0x8000)
    {
      /* Branches and miscellaneous control instructions.  */

      if ((inst2 & 0x1000) != 0 || (inst2 & 0xd001) == 0xc000)
	{
	  /* B, BL, BLX.  */
	  return 1;
	}
      else if (inst1 == 0xf3de && (inst2 & 0xff00) == 0x3f00)
	{
	  /* SUBS PC, LR, #imm8.  */
	  return 1;
	}
      else if ((inst2 & 0xd000) == 0x8000 && (inst1 & 0x0380) != 0x0380)
	{
	  /* Conditional branch.  */
	  return 1;
	}

      return 0;
    }

  if ((inst1 & 0xfe50) == 0xe810)
    {
      /* Load multiple or RFE.  */

      if (bit (inst1, 7) && !bit (inst1, 8))
	{
	  /* LDMIA or POP */
	  if (bit (inst2, 15))
	    return 1;
	}
      else if (!bit (inst1, 7) && bit (inst1, 8))
	{
	  /* LDMDB */
	  if (bit (inst2, 15))
	    return 1;
	}
      else if (bit (inst1, 7) && bit (inst1, 8))
	{
	  /* RFEIA */
	  return 1;
	}
      else if (!bit (inst1, 7) && !bit (inst1, 8))
	{
	  /* RFEDB */
	  return 1;
	}

      return 0;
    }

  if ((inst1 & 0xffef) == 0xea4f && (inst2 & 0xfff0) == 0x0f00)
    {
      /* MOV PC or MOVS PC.  */
      return 1;
    }

  if ((inst1 & 0xff70) == 0xf850 && (inst2 & 0xf000) == 0xf000)
    {
      /* LDR PC.  */
      if (bits (inst1, 0, 3) == 15)
	return 1;
      if (bit (inst1, 7))
	return 1;
      if (bit (inst2, 11))
	return 1;
      if ((inst2 & 0x0fc0) == 0x0000)
	return 1;	

      return 0;
    }

  if ((inst1 & 0xfff0) == 0xe8d0 && (inst2 & 0xfff0) == 0xf000)
    {
      /* TBB.  */
      return 1;
    }

  if ((inst1 & 0xfff0) == 0xe8d0 && (inst2 & 0xfff0) == 0xf010)
    {
      /* TBH.  */
      return 1;
    }

  return 0;
}

/* Analyze a Thumb prologue, looking for a recognizable stack frame
   and frame pointer.  Scan until we encounter a store that could
   clobber the stack frame unexpectedly, or an unknown instruction.
   Return the last address which is definitely safe to skip for an
   initial breakpoint.  */

static CORE_ADDR
thumb_analyze_prologue (struct gdbarch *gdbarch,
			CORE_ADDR start, CORE_ADDR limit,
			struct arm_prologue_cache *cache)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  int i;
  pv_t regs[16];
  struct pv_area *stack;
  struct cleanup *back_to;
  CORE_ADDR offset;
  CORE_ADDR unrecognized_pc = 0;

  for (i = 0; i < 16; i++)
    regs[i] = pv_register (i, 0);
  stack = make_pv_area (ARM_SP_REGNUM, gdbarch_addr_bit (gdbarch));
  back_to = make_cleanup_free_pv_area (stack);

  while (start < limit)
    {
      unsigned short insn;

      insn = read_memory_unsigned_integer (start, 2, byte_order_for_code);

      if ((insn & 0xfe00) == 0xb400)		/* push { rlist } */
	{
	  int regno;
	  int mask;

	  if (pv_area_store_would_trash (stack, regs[ARM_SP_REGNUM]))
	    break;

	  /* Bits 0-7 contain a mask for registers R0-R7.  Bit 8 says
	     whether to save LR (R14).  */
	  mask = (insn & 0xff) | ((insn & 0x100) << 6);

	  /* Calculate offsets of saved R0-R7 and LR.  */
	  for (regno = ARM_LR_REGNUM; regno >= 0; regno--)
	    if (mask & (1 << regno))
	      {
		regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM],
						       -4);
		pv_area_store (stack, regs[ARM_SP_REGNUM], 4, regs[regno]);
	      }
	}
      else if ((insn & 0xff00) == 0xb000)	/* add sp, #simm  OR  
						   sub sp, #simm */
	{
	  offset = (insn & 0x7f) << 2;		/* get scaled offset */
	  if (insn & 0x80)			/* Check for SUB.  */
	    regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM],
						   -offset);
	  else
	    regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM],
						   offset);
	}
      else if ((insn & 0xf800) == 0xa800)	/* add Rd, sp, #imm */
	regs[bits (insn, 8, 10)] = pv_add_constant (regs[ARM_SP_REGNUM],
						    (insn & 0xff) << 2);
      else if ((insn & 0xfe00) == 0x1c00	/* add Rd, Rn, #imm */
	       && pv_is_register (regs[bits (insn, 3, 5)], ARM_SP_REGNUM))
	regs[bits (insn, 0, 2)] = pv_add_constant (regs[bits (insn, 3, 5)],
						   bits (insn, 6, 8));
      else if ((insn & 0xf800) == 0x3000	/* add Rd, #imm */
	       && pv_is_register (regs[bits (insn, 8, 10)], ARM_SP_REGNUM))
	regs[bits (insn, 8, 10)] = pv_add_constant (regs[bits (insn, 8, 10)],
						    bits (insn, 0, 7));
      else if ((insn & 0xfe00) == 0x1800	/* add Rd, Rn, Rm */
	       && pv_is_register (regs[bits (insn, 6, 8)], ARM_SP_REGNUM)
	       && pv_is_constant (regs[bits (insn, 3, 5)]))
	regs[bits (insn, 0, 2)] = pv_add (regs[bits (insn, 3, 5)],
					  regs[bits (insn, 6, 8)]);
      else if ((insn & 0xff00) == 0x4400	/* add Rd, Rm */
	       && pv_is_constant (regs[bits (insn, 3, 6)]))
	{
	  int rd = (bit (insn, 7) << 3) + bits (insn, 0, 2);
	  int rm = bits (insn, 3, 6);
	  regs[rd] = pv_add (regs[rd], regs[rm]);
	}
      else if ((insn & 0xff00) == 0x4600)	/* mov hi, lo or mov lo, hi */
	{
	  int dst_reg = (insn & 0x7) + ((insn & 0x80) >> 4);
	  int src_reg = (insn & 0x78) >> 3;
	  regs[dst_reg] = regs[src_reg];
	}
      else if ((insn & 0xf800) == 0x9000)	/* str rd, [sp, #off] */
	{
	  /* Handle stores to the stack.  Normally pushes are used,
	     but with GCC -mtpcs-frame, there may be other stores
	     in the prologue to create the frame.  */
	  int regno = (insn >> 8) & 0x7;
	  pv_t addr;

	  offset = (insn & 0xff) << 2;
	  addr = pv_add_constant (regs[ARM_SP_REGNUM], offset);

	  if (pv_area_store_would_trash (stack, addr))
	    break;

	  pv_area_store (stack, addr, 4, regs[regno]);
	}
      else if ((insn & 0xf800) == 0x6000)	/* str rd, [rn, #off] */
	{
	  int rd = bits (insn, 0, 2);
	  int rn = bits (insn, 3, 5);
	  pv_t addr;

	  offset = bits (insn, 6, 10) << 2;
	  addr = pv_add_constant (regs[rn], offset);

	  if (pv_area_store_would_trash (stack, addr))
	    break;

	  pv_area_store (stack, addr, 4, regs[rd]);
	}
      else if (((insn & 0xf800) == 0x7000	/* strb Rd, [Rn, #off] */
		|| (insn & 0xf800) == 0x8000)	/* strh Rd, [Rn, #off] */
	       && pv_is_register (regs[bits (insn, 3, 5)], ARM_SP_REGNUM))
	/* Ignore stores of argument registers to the stack.  */
	;
      else if ((insn & 0xf800) == 0xc800	/* ldmia Rn!, { registers } */
	       && pv_is_register (regs[bits (insn, 8, 10)], ARM_SP_REGNUM))
	/* Ignore block loads from the stack, potentially copying
	   parameters from memory.  */
	;
      else if ((insn & 0xf800) == 0x9800	/* ldr Rd, [Rn, #immed] */
	       || ((insn & 0xf800) == 0x6800	/* ldr Rd, [sp, #immed] */
		   && pv_is_register (regs[bits (insn, 3, 5)], ARM_SP_REGNUM)))
	/* Similarly ignore single loads from the stack.  */
	;
      else if ((insn & 0xffc0) == 0x0000	/* lsls Rd, Rm, #0 */
	       || (insn & 0xffc0) == 0x1c00)	/* add Rd, Rn, #0 */
	/* Skip register copies, i.e. saves to another register
	   instead of the stack.  */
	;
      else if ((insn & 0xf800) == 0x2000)	/* movs Rd, #imm */
	/* Recognize constant loads; even with small stacks these are necessary
	   on Thumb.  */
	regs[bits (insn, 8, 10)] = pv_constant (bits (insn, 0, 7));
      else if ((insn & 0xf800) == 0x4800)	/* ldr Rd, [pc, #imm] */
	{
	  /* Constant pool loads, for the same reason.  */
	  unsigned int constant;
	  CORE_ADDR loc;

	  loc = start + 4 + bits (insn, 0, 7) * 4;
	  constant = read_memory_unsigned_integer (loc, 4, byte_order);
	  regs[bits (insn, 8, 10)] = pv_constant (constant);
	}
      else if (thumb_insn_size (insn) == 4) /* 32-bit Thumb-2 instructions.  */
	{
	  unsigned short inst2;

	  inst2 = read_memory_unsigned_integer (start + 2, 2,
						byte_order_for_code);

	  if ((insn & 0xf800) == 0xf000 && (inst2 & 0xe800) == 0xe800)
	    {
	      /* BL, BLX.  Allow some special function calls when
		 skipping the prologue; GCC generates these before
		 storing arguments to the stack.  */
	      CORE_ADDR nextpc;
	      int j1, j2, imm1, imm2;

	      imm1 = sbits (insn, 0, 10);
	      imm2 = bits (inst2, 0, 10);
	      j1 = bit (inst2, 13);
	      j2 = bit (inst2, 11);

	      offset = ((imm1 << 12) + (imm2 << 1));
	      offset ^= ((!j2) << 22) | ((!j1) << 23);

	      nextpc = start + 4 + offset;
	      /* For BLX make sure to clear the low bits.  */
	      if (bit (inst2, 12) == 0)
		nextpc = nextpc & 0xfffffffc;

	      if (!skip_prologue_function (gdbarch, nextpc,
					   bit (inst2, 12) != 0))
		break;
	    }

	  else if ((insn & 0xffd0) == 0xe900    /* stmdb Rn{!},
						   { registers } */
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    {
	      pv_t addr = regs[bits (insn, 0, 3)];
	      int regno;

	      if (pv_area_store_would_trash (stack, addr))
		break;

	      /* Calculate offsets of saved registers.  */
	      for (regno = ARM_LR_REGNUM; regno >= 0; regno--)
		if (inst2 & (1 << regno))
		  {
		    addr = pv_add_constant (addr, -4);
		    pv_area_store (stack, addr, 4, regs[regno]);
		  }

	      if (insn & 0x0020)
		regs[bits (insn, 0, 3)] = addr;
	    }

	  else if ((insn & 0xff50) == 0xe940	/* strd Rt, Rt2,
						   [Rn, #+/-imm]{!} */
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    {
	      int regno1 = bits (inst2, 12, 15);
	      int regno2 = bits (inst2, 8, 11);
	      pv_t addr = regs[bits (insn, 0, 3)];

	      offset = inst2 & 0xff;
	      if (insn & 0x0080)
		addr = pv_add_constant (addr, offset);
	      else
		addr = pv_add_constant (addr, -offset);

	      if (pv_area_store_would_trash (stack, addr))
		break;

	      pv_area_store (stack, addr, 4, regs[regno1]);
	      pv_area_store (stack, pv_add_constant (addr, 4),
			     4, regs[regno2]);

	      if (insn & 0x0020)
		regs[bits (insn, 0, 3)] = addr;
	    }

	  else if ((insn & 0xfff0) == 0xf8c0	/* str Rt,[Rn,+/-#imm]{!} */
		   && (inst2 & 0x0c00) == 0x0c00
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    {
	      int regno = bits (inst2, 12, 15);
	      pv_t addr = regs[bits (insn, 0, 3)];

	      offset = inst2 & 0xff;
	      if (inst2 & 0x0200)
		addr = pv_add_constant (addr, offset);
	      else
		addr = pv_add_constant (addr, -offset);

	      if (pv_area_store_would_trash (stack, addr))
		break;

	      pv_area_store (stack, addr, 4, regs[regno]);

	      if (inst2 & 0x0100)
		regs[bits (insn, 0, 3)] = addr;
	    }

	  else if ((insn & 0xfff0) == 0xf8c0	/* str.w Rt,[Rn,#imm] */
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    {
	      int regno = bits (inst2, 12, 15);
	      pv_t addr;

	      offset = inst2 & 0xfff;
	      addr = pv_add_constant (regs[bits (insn, 0, 3)], offset);

	      if (pv_area_store_would_trash (stack, addr))
		break;

	      pv_area_store (stack, addr, 4, regs[regno]);
	    }

	  else if ((insn & 0xffd0) == 0xf880	/* str{bh}.w Rt,[Rn,#imm] */
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    /* Ignore stores of argument registers to the stack.  */
	    ;

	  else if ((insn & 0xffd0) == 0xf800	/* str{bh} Rt,[Rn,#+/-imm] */
		   && (inst2 & 0x0d00) == 0x0c00
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    /* Ignore stores of argument registers to the stack.  */
	    ;

	  else if ((insn & 0xffd0) == 0xe890	/* ldmia Rn[!],
						   { registers } */
		   && (inst2 & 0x8000) == 0x0000
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    /* Ignore block loads from the stack, potentially copying
	       parameters from memory.  */
	    ;

	  else if ((insn & 0xffb0) == 0xe950	/* ldrd Rt, Rt2,
						   [Rn, #+/-imm] */
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    /* Similarly ignore dual loads from the stack.  */
	    ;

	  else if ((insn & 0xfff0) == 0xf850	/* ldr Rt,[Rn,#+/-imm] */
		   && (inst2 & 0x0d00) == 0x0c00
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    /* Similarly ignore single loads from the stack.  */
	    ;

	  else if ((insn & 0xfff0) == 0xf8d0	/* ldr.w Rt,[Rn,#imm] */
		   && pv_is_register (regs[bits (insn, 0, 3)], ARM_SP_REGNUM))
	    /* Similarly ignore single loads from the stack.  */
	    ;

	  else if ((insn & 0xfbf0) == 0xf100	/* add.w Rd, Rn, #imm */
		   && (inst2 & 0x8000) == 0x0000)
	    {
	      unsigned int imm = ((bits (insn, 10, 10) << 11)
				  | (bits (inst2, 12, 14) << 8)
				  | bits (inst2, 0, 7));

	      regs[bits (inst2, 8, 11)]
		= pv_add_constant (regs[bits (insn, 0, 3)],
				   thumb_expand_immediate (imm));
	    }

	  else if ((insn & 0xfbf0) == 0xf200	/* addw Rd, Rn, #imm */
		   && (inst2 & 0x8000) == 0x0000)
	    {
	      unsigned int imm = ((bits (insn, 10, 10) << 11)
				  | (bits (inst2, 12, 14) << 8)
				  | bits (inst2, 0, 7));

	      regs[bits (inst2, 8, 11)]
		= pv_add_constant (regs[bits (insn, 0, 3)], imm);
	    }

	  else if ((insn & 0xfbf0) == 0xf1a0	/* sub.w Rd, Rn, #imm */
		   && (inst2 & 0x8000) == 0x0000)
	    {
	      unsigned int imm = ((bits (insn, 10, 10) << 11)
				  | (bits (inst2, 12, 14) << 8)
				  | bits (inst2, 0, 7));

	      regs[bits (inst2, 8, 11)]
		= pv_add_constant (regs[bits (insn, 0, 3)],
				   - (CORE_ADDR) thumb_expand_immediate (imm));
	    }

	  else if ((insn & 0xfbf0) == 0xf2a0	/* subw Rd, Rn, #imm */
		   && (inst2 & 0x8000) == 0x0000)
	    {
	      unsigned int imm = ((bits (insn, 10, 10) << 11)
				  | (bits (inst2, 12, 14) << 8)
				  | bits (inst2, 0, 7));

	      regs[bits (inst2, 8, 11)]
		= pv_add_constant (regs[bits (insn, 0, 3)], - (CORE_ADDR) imm);
	    }

	  else if ((insn & 0xfbff) == 0xf04f)	/* mov.w Rd, #const */
	    {
	      unsigned int imm = ((bits (insn, 10, 10) << 11)
				  | (bits (inst2, 12, 14) << 8)
				  | bits (inst2, 0, 7));

	      regs[bits (inst2, 8, 11)]
		= pv_constant (thumb_expand_immediate (imm));
	    }

	  else if ((insn & 0xfbf0) == 0xf240)	/* movw Rd, #const */
	    {
	      unsigned int imm
		= EXTRACT_MOVW_MOVT_IMM_T (insn, inst2);

	      regs[bits (inst2, 8, 11)] = pv_constant (imm);
	    }

	  else if (insn == 0xea5f		/* mov.w Rd,Rm */
		   && (inst2 & 0xf0f0) == 0)
	    {
	      int dst_reg = (inst2 & 0x0f00) >> 8;
	      int src_reg = inst2 & 0xf;
	      regs[dst_reg] = regs[src_reg];
	    }

	  else if ((insn & 0xff7f) == 0xf85f)	/* ldr.w Rt,<label> */
	    {
	      /* Constant pool loads.  */
	      unsigned int constant;
	      CORE_ADDR loc;

	      offset = bits (insn, 0, 11);
	      if (insn & 0x0080)
		loc = start + 4 + offset;
	      else
		loc = start + 4 - offset;

	      constant = read_memory_unsigned_integer (loc, 4, byte_order);
	      regs[bits (inst2, 12, 15)] = pv_constant (constant);
	    }

	  else if ((insn & 0xff7f) == 0xe95f)	/* ldrd Rt,Rt2,<label> */
	    {
	      /* Constant pool loads.  */
	      unsigned int constant;
	      CORE_ADDR loc;

	      offset = bits (insn, 0, 7) << 2;
	      if (insn & 0x0080)
		loc = start + 4 + offset;
	      else
		loc = start + 4 - offset;

	      constant = read_memory_unsigned_integer (loc, 4, byte_order);
	      regs[bits (inst2, 12, 15)] = pv_constant (constant);

	      constant = read_memory_unsigned_integer (loc + 4, 4, byte_order);
	      regs[bits (inst2, 8, 11)] = pv_constant (constant);
	    }

	  else if (thumb2_instruction_changes_pc (insn, inst2))
	    {
	      /* Don't scan past anything that might change control flow.  */
	      break;
	    }
	  else
	    {
	      /* The optimizer might shove anything into the prologue,
		 so we just skip what we don't recognize.  */
	      unrecognized_pc = start;
	    }

	  start += 2;
	}
      else if (thumb_instruction_changes_pc (insn))
	{
	  /* Don't scan past anything that might change control flow.  */
	  break;
	}
      else
	{
	  /* The optimizer might shove anything into the prologue,
	     so we just skip what we don't recognize.  */
	  unrecognized_pc = start;
	}

      start += 2;
    }

  if (arm_debug)
    fprintf_unfiltered (gdb_stdlog, "Prologue scan stopped at %s\n",
			paddress (gdbarch, start));

  if (unrecognized_pc == 0)
    unrecognized_pc = start;

  if (cache == NULL)
    {
      do_cleanups (back_to);
      return unrecognized_pc;
    }

  if (pv_is_register (regs[ARM_FP_REGNUM], ARM_SP_REGNUM))
    {
      /* Frame pointer is fp.  Frame size is constant.  */
      cache->framereg = ARM_FP_REGNUM;
      cache->framesize = -regs[ARM_FP_REGNUM].k;
    }
  else if (pv_is_register (regs[THUMB_FP_REGNUM], ARM_SP_REGNUM))
    {
      /* Frame pointer is r7.  Frame size is constant.  */
      cache->framereg = THUMB_FP_REGNUM;
      cache->framesize = -regs[THUMB_FP_REGNUM].k;
    }
  else
    {
      /* Try the stack pointer... this is a bit desperate.  */
      cache->framereg = ARM_SP_REGNUM;
      cache->framesize = -regs[ARM_SP_REGNUM].k;
    }

  for (i = 0; i < 16; i++)
    if (pv_area_find_reg (stack, gdbarch, i, &offset))
      cache->saved_regs[i].addr = offset;

  do_cleanups (back_to);
  return unrecognized_pc;
}


/* Try to analyze the instructions starting from PC, which load symbol
   __stack_chk_guard.  Return the address of instruction after loading this
   symbol, set the dest register number to *BASEREG, and set the size of
   instructions for loading symbol in OFFSET.  Return 0 if instructions are
   not recognized.  */

static CORE_ADDR
arm_analyze_load_stack_chk_guard(CORE_ADDR pc, struct gdbarch *gdbarch,
				 unsigned int *destreg, int *offset)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  int is_thumb = arm_pc_is_thumb (gdbarch, pc);
  unsigned int low, high, address;

  address = 0;
  if (is_thumb)
    {
      unsigned short insn1
	= read_memory_unsigned_integer (pc, 2, byte_order_for_code);

      if ((insn1 & 0xf800) == 0x4800) /* ldr Rd, #immed */
	{
	  *destreg = bits (insn1, 8, 10);
	  *offset = 2;
	  address = bits (insn1, 0, 7);
	}
      else if ((insn1 & 0xfbf0) == 0xf240) /* movw Rd, #const */
	{
	  unsigned short insn2
	    = read_memory_unsigned_integer (pc + 2, 2, byte_order_for_code);

	  low = EXTRACT_MOVW_MOVT_IMM_T (insn1, insn2);

	  insn1
	    = read_memory_unsigned_integer (pc + 4, 2, byte_order_for_code);
	  insn2
	    = read_memory_unsigned_integer (pc + 6, 2, byte_order_for_code);

	  /* movt Rd, #const */
	  if ((insn1 & 0xfbc0) == 0xf2c0)
	    {
	      high = EXTRACT_MOVW_MOVT_IMM_T (insn1, insn2);
	      *destreg = bits (insn2, 8, 11);
	      *offset = 8;
	      address = (high << 16 | low);
	    }
	}
    }
  else
    {
      unsigned int insn
	= read_memory_unsigned_integer (pc, 4, byte_order_for_code);

      if ((insn & 0x0e5f0000) == 0x041f0000) /* ldr Rd, #immed */
	{
	  address = bits (insn, 0, 11);
	  *destreg = bits (insn, 12, 15);
	  *offset = 4;
	}
      else if ((insn & 0x0ff00000) == 0x03000000) /* movw Rd, #const */
	{
	  low = EXTRACT_MOVW_MOVT_IMM_A (insn);

	  insn
	    = read_memory_unsigned_integer (pc + 4, 4, byte_order_for_code);

	  if ((insn & 0x0ff00000) == 0x03400000) /* movt Rd, #const */
	    {
	      high = EXTRACT_MOVW_MOVT_IMM_A (insn);
	      *destreg = bits (insn, 12, 15);
	      *offset = 8;
	      address = (high << 16 | low);
	    }
	}
    }

  return address;
}

/* Try to skip a sequence of instructions used for stack protector.  If PC
   points to the first instruction of this sequence, return the address of
   first instruction after this sequence, otherwise, return original PC.

   On arm, this sequence of instructions is composed of mainly three steps,
     Step 1: load symbol __stack_chk_guard,
     Step 2: load from address of __stack_chk_guard,
     Step 3: store it to somewhere else.

   Usually, instructions on step 2 and step 3 are the same on various ARM
   architectures.  On step 2, it is one instruction 'ldr Rx, [Rn, #0]', and
   on step 3, it is also one instruction 'str Rx, [r7, #immd]'.  However,
   instructions in step 1 vary from different ARM architectures.  On ARMv7,
   they are,

	movw	Rn, #:lower16:__stack_chk_guard
	movt	Rn, #:upper16:__stack_chk_guard

   On ARMv5t, it is,

	ldr	Rn, .Label
	....
	.Lable:
	.word	__stack_chk_guard

   Since ldr/str is a very popular instruction, we can't use them as
   'fingerprint' or 'signature' of stack protector sequence.  Here we choose
   sequence {movw/movt, ldr}/ldr/str plus symbol __stack_chk_guard, if not
   stripped, as the 'fingerprint' of a stack protector cdoe sequence.  */

static CORE_ADDR
arm_skip_stack_protector(CORE_ADDR pc, struct gdbarch *gdbarch)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned int address, basereg;
  struct minimal_symbol *stack_chk_guard;
  int offset;
  int is_thumb = arm_pc_is_thumb (gdbarch, pc);
  CORE_ADDR addr;

  /* Try to parse the instructions in Step 1.  */
  addr = arm_analyze_load_stack_chk_guard (pc, gdbarch,
					   &basereg, &offset);
  if (!addr)
    return pc;

  stack_chk_guard = lookup_minimal_symbol_by_pc (addr);
  /* If name of symbol doesn't start with '__stack_chk_guard', this
     instruction sequence is not for stack protector.  If symbol is
     removed, we conservatively think this sequence is for stack protector.  */
  if (stack_chk_guard
      && strncmp (SYMBOL_LINKAGE_NAME (stack_chk_guard), "__stack_chk_guard",
		  strlen ("__stack_chk_guard")) != 0)
   return pc;

  if (is_thumb)
    {
      unsigned int destreg;
      unsigned short insn
	= read_memory_unsigned_integer (pc + offset, 2, byte_order_for_code);

      /* Step 2: ldr Rd, [Rn, #immed], encoding T1.  */
      if ((insn & 0xf800) != 0x6800)
	return pc;
      if (bits (insn, 3, 5) != basereg)
	return pc;
      destreg = bits (insn, 0, 2);

      insn = read_memory_unsigned_integer (pc + offset + 2, 2,
					   byte_order_for_code);
      /* Step 3: str Rd, [Rn, #immed], encoding T1.  */
      if ((insn & 0xf800) != 0x6000)
	return pc;
      if (destreg != bits (insn, 0, 2))
	return pc;
    }
  else
    {
      unsigned int destreg;
      unsigned int insn
	= read_memory_unsigned_integer (pc + offset, 4, byte_order_for_code);

      /* Step 2: ldr Rd, [Rn, #immed], encoding A1.  */
      if ((insn & 0x0e500000) != 0x04100000)
	return pc;
      if (bits (insn, 16, 19) != basereg)
	return pc;
      destreg = bits (insn, 12, 15);
      /* Step 3: str Rd, [Rn, #immed], encoding A1.  */
      insn = read_memory_unsigned_integer (pc + offset + 4,
					   4, byte_order_for_code);
      if ((insn & 0x0e500000) != 0x04000000)
	return pc;
      if (bits (insn, 12, 15) != destreg)
	return pc;
    }
  /* The size of total two instructions ldr/str is 4 on Thumb-2, while 8
     on arm.  */
  if (is_thumb)
    return pc + offset + 4;
  else
    return pc + offset + 8;
}

/* Subroutine of skip_gcc_pic_assignment to simplify it.
   After establishing that we're at a gcc pic assignment,
   return a possibly adjusted value for the end of the prologue.
   PIC_ASSIGNMENT_LENGTH is the total length of insns that assign
   the pic reg.  */

static CORE_ADDR
skip_gcc_pic_assignment_1 (struct gdbarch *gdbarch,
                           CORE_ADDR func_addr, CORE_ADDR post_prologue_pc,
                           int pic_assignment_length)
{
  struct symtab_and_line func_addr_sal, post_prologue_sal, sal2;

  func_addr_sal = find_pc_line (func_addr, 0);
  sal2 = find_pc_line (post_prologue_pc + pic_assignment_length, 0);
  /* Catch the case of being in the middle of the prologue.  */
  if (func_addr_sal.line != 0
      && func_addr_sal.line == sal2.line)
    post_prologue_pc = sal2.end;
  /* Catch the case of being at the end of the prologue.  */
  else
    {
      post_prologue_sal = find_pc_line (post_prologue_pc, 0);
      if (post_prologue_sal.line != 0
          && sal2.line != 0
          && sal2.line <= post_prologue_sal.line)
        post_prologue_pc = post_prologue_sal.end;
    }

  return post_prologue_pc;
}

/* Subroutine of skip_gcc_pic_assignment to simplify it.
   Skip version 1 of a thumb pic register assignment.
   The result is the length of the code in bytes or zero if not for pic.  */

static int
skip_gcc_thumb_pic_assignment_1 (struct gdbarch *gdbarch, CORE_ADDR addr)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned short insn;
  unsigned int reg;

  insn = read_memory_unsigned_integer (addr, 2, byte_order_for_code);
  if ((insn & 0xf800) != 0x4800) /* ldr rN,[pc,foo] */
    return 0;

  reg = (insn >> 8) & 7;
  insn = read_memory_unsigned_integer (addr + 2, 2, byte_order_for_code);
  if ((insn & 0xffff)
      /* Note: We know reg < 8 here.  */
      != (0x4400 + 0x78 /* pc */ + reg)) /* add rN, pc */
    return 0;

  return 4;
}

/* Subroutine of skip_gcc_pic_assignment to simplify it.
   Skip version 1 of an arm pic register assignment.
   The result is the length of the code in bytes or zero if not for pic.  */

static int
skip_gcc_arm_pic_assignment_1 (struct gdbarch *gdbarch, CORE_ADDR addr)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned int insn;
  unsigned int reg;

  insn = read_memory_unsigned_integer (addr, 4, byte_order_for_code);
  if ((insn & 0xffff0000)
      != (0xe5900000 + 0xf0000 /*pc*/)) /* ldr rN,[pc,foo] */
    return 0;

  reg = (insn >> 12) & 15;
  insn = read_memory_unsigned_integer (addr + 4, 4, byte_order_for_code);
  if ((insn & 0xffffffff)
      /* add rN, pc, rN */
      != (0xe0800000 + 0xf0000 /*pc*/ + (reg << 12) + reg))
    return 0;

  return 8;
}

/* Subroutine of skip_gcc_pic_assignment to simplify it.
   Skip version 2 of an arm pic register assignment.
   The result is the length of the code in bytes or zero if not for pic.  */

static int
skip_gcc_arm_pic_assignment_2 (struct gdbarch *gdbarch, CORE_ADDR addr)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned int insn;
  unsigned int regM, regN, regX, offset;

  insn = read_memory_unsigned_integer (addr, 4, byte_order_for_code);
  if ((insn & 0xffff0000)
      != (0xe5900000 + 0xf0000 /*pc*/)) /* ldr rM,[pc,foo] */
    return 0;

  regM = (insn >> 12) & 15;
  insn = read_memory_unsigned_integer (addr + 4, 4, byte_order_for_code);
  if ((insn & 0xfff0f000)
      != (0xe5000000 + (regM << 12))) /* str rM,[rX,-offset] */
    return 0;
  regX = (insn >> 16) & 15;
  offset = insn & 4095;

  insn = read_memory_unsigned_integer (addr + 8, 4, byte_order_for_code);
  if ((insn & 0xffff0fff)
      != (0xe5100000 + (regX << 16) + offset)) /* ldr rN,[rX,-offset] */
    return 0;

  regN = (insn >> 12) & 15;
  insn = read_memory_unsigned_integer (addr + 12, 4, byte_order_for_code);
  if ((insn & 0xffffffff)
      /* add rN, pc, rN */
      != (0xe0800000 + 0xf0000 /*pc*/ + (regN << 12) + regN))
    return 0;

  insn = read_memory_unsigned_integer (addr + 16, 4, byte_order_for_code);
  if ((insn & 0xffffffff)
      != (0xe5000000 + (regX << 16) + (regN << 12) + offset)) /* str rN,[rX,-offset] */
    return 0;

  return 20;
}

/* Subroutine of arm_skip_prologue to simplify it.
   Compensate for GCC 4.4.0 inserting pic register initialization in the
   middle of the prologue with a line number outside the prologue.
   This breaks skip_prologue_using_sal.
   FUNC_ADDR is the start_pc result of find_pc_partial_function.
   POST_PROLOGUE_PC is the pc returned by skip_prologue_using_sal.
   The result is the new post-prologue-pc to use.

   Two variations of loading the pic register have been seen:

   (1) ldr rN,[pc,foo]
       add rN, pc, rN

   (2) ldr rM, [pc, foo]
       str rM, [rX, offset]
       ldr rN, [rX, offset]
       add rN, pc, rN
       str rN, [rX, offset]
*/

static CORE_ADDR
skip_gcc_pic_assignment (struct gdbarch *gdbarch,
                         CORE_ADDR func_addr, CORE_ADDR post_prologue_pc)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned int length;

  if (arm_pc_is_thumb (gdbarch, func_addr))
    {
      length = skip_gcc_thumb_pic_assignment_1 (gdbarch, post_prologue_pc);
    }
  else
    {
      length = skip_gcc_arm_pic_assignment_1 (gdbarch, post_prologue_pc);
      if (length == 0)
        length = skip_gcc_arm_pic_assignment_2 (gdbarch, post_prologue_pc);
    }

  if (length != 0)
    post_prologue_pc =
      skip_gcc_pic_assignment_1 (gdbarch, func_addr, post_prologue_pc, length);

  return post_prologue_pc;
}

/* Advance the PC across any function entry prologue instructions to
   reach some "real" code.

   The APCS (ARM Procedure Call Standard) defines the following
   prologue:

   mov          ip, sp
   [stmfd       sp!, {a1,a2,a3,a4}]
   stmfd        sp!, {...,fp,ip,lr,pc}
   [stfe        f7, [sp, #-12]!]
   [stfe        f6, [sp, #-12]!]
   [stfe        f5, [sp, #-12]!]
   [stfe        f4, [sp, #-12]!]
   sub fp, ip, #nn @@ nn == 20 or 4 depending on second insn.  */

static CORE_ADDR
arm_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR pc)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned long inst;
  CORE_ADDR skip_pc;
  CORE_ADDR func_addr, limit_pc;
  struct symtab_and_line sal;

  /* See if we can determine the end of the prologue via the symbol table.
     If so, then return either PC, or the PC after the prologue, whichever
     is greater.  */
  if (find_pc_partial_function (pc, NULL, &func_addr, NULL))
    {
      CORE_ADDR post_prologue_pc
	= skip_prologue_using_sal (gdbarch, func_addr);
      struct symtab *s = find_pc_symtab (func_addr);

      if (post_prologue_pc)
	post_prologue_pc
	  = arm_skip_stack_protector (post_prologue_pc, gdbarch);

      if (post_prologue_pc)
        {
          /* GCC 4.4.0 will move the setting of the pic register into the
             middle of the prologue using line numbers from the original
             location.  :-(  Compensate.  */
          CORE_ADDR post_pic_pc = skip_gcc_pic_assignment (gdbarch, func_addr,
                                                           post_prologue_pc);
          post_prologue_pc = max (pc, post_pic_pc);
        }

      /* GCC always emits a line note before the prologue and another
	 one after, even if the two are at the same address or on the
	 same line.  Take advantage of this so that we do not need to
	 know every instruction that might appear in the prologue.  We
	 will have producer information for most binaries; if it is
	 missing (e.g. for -gstabs), assuming the GNU tools.  */
      if (post_prologue_pc
	  && (s == NULL
	      || s->producer == NULL
	      || strncmp (s->producer, "GNU ", sizeof ("GNU ") - 1) == 0))
	return post_prologue_pc;

      if (post_prologue_pc != 0)
	{
	  CORE_ADDR analyzed_limit;

	  /* For non-GCC compilers, make sure the entire line is an
	     acceptable prologue; GDB will round this function's
	     return value up to the end of the following line so we
	     can not skip just part of a line (and we do not want to).

	     RealView does not treat the prologue specially, but does
	     associate prologue code with the opening brace; so this
	     lets us skip the first line if we think it is the opening
	     brace.  */
	  if (arm_pc_is_thumb (gdbarch, func_addr))
	    analyzed_limit = thumb_analyze_prologue (gdbarch, func_addr,
						     post_prologue_pc, NULL);
	  else
	    analyzed_limit = arm_analyze_prologue (gdbarch, func_addr,
						   post_prologue_pc, NULL);

	  if (analyzed_limit != post_prologue_pc)
	    return func_addr;

	  return post_prologue_pc;
	}
    }

  /* Can't determine prologue from the symbol table, need to examine
     instructions.  */

  /* Find an upper limit on the function prologue using the debug
     information.  If the debug information could not be used to provide
     that bound, then use an arbitrary large number as the upper bound.  */
  /* Like arm_scan_prologue, stop no later than pc + 64.  */
  limit_pc = skip_prologue_using_sal (gdbarch, pc);
  if (limit_pc == 0)
    limit_pc = pc + 64;          /* Magic.  */


  /* Check if this is Thumb code.  */
  if (arm_pc_is_thumb (gdbarch, pc))
    return thumb_analyze_prologue (gdbarch, pc, limit_pc, NULL);

  for (skip_pc = pc; skip_pc < limit_pc; skip_pc += 4)
    {
      inst = read_memory_unsigned_integer (skip_pc, 4, byte_order_for_code);

      /* "mov ip, sp" is no longer a required part of the prologue.  */
      if (inst == 0xe1a0c00d)			/* mov ip, sp */
	continue;

      if ((inst & 0xfffff000) == 0xe28dc000)    /* add ip, sp #n */
	continue;

      if ((inst & 0xfffff000) == 0xe24dc000)    /* sub ip, sp #n */
	continue;

      /* Some prologues begin with "str lr, [sp, #-4]!".  */
      if (inst == 0xe52de004)			/* str lr, [sp, #-4]! */
	continue;

      if ((inst & 0xfffffff0) == 0xe92d0000)	/* stmfd sp!,{a1,a2,a3,a4} */
	continue;

      if ((inst & 0xfffff800) == 0xe92dd800)	/* stmfd sp!,{fp,ip,lr,pc} */
	continue;

      /* Any insns after this point may float into the code, if it makes
	 for better instruction scheduling, so we skip them only if we
	 find them, but still consider the function to be frame-ful.  */

      /* We may have either one sfmfd instruction here, or several stfe
	 insns, depending on the version of floating point code we
	 support.  */
      if ((inst & 0xffbf0fff) == 0xec2d0200)	/* sfmfd fn, <cnt>, [sp]! */
	continue;

      if ((inst & 0xffff8fff) == 0xed6d0103)	/* stfe fn, [sp, #-12]! */
	continue;

      if ((inst & 0xfffff000) == 0xe24cb000)	/* sub fp, ip, #nn */
	continue;

      if ((inst & 0xfffff000) == 0xe24dd000)	/* sub sp, sp, #nn */
	continue;

      if ((inst & 0xffffc000) == 0xe54b0000	/* strb r(0123),[r11,#-nn] */
	  || (inst & 0xffffc0f0) == 0xe14b00b0	/* strh r(0123),[r11,#-nn] */
	  || (inst & 0xffffc000) == 0xe50b0000)	/* str  r(0123),[r11,#-nn] */
	continue;

      if ((inst & 0xffffc000) == 0xe5cd0000	/* strb r(0123),[sp,#nn] */
	  || (inst & 0xffffc0f0) == 0xe1cd00b0	/* strh r(0123),[sp,#nn] */
	  || (inst & 0xffffc000) == 0xe58d0000)	/* str  r(0123),[sp,#nn] */
	continue;

      /* Un-recognized instruction; stop scanning.  */
      break;
    }

  return skip_pc;		/* End of prologue.  */
}

/* *INDENT-OFF* */
/* Function: thumb_scan_prologue (helper function for arm_scan_prologue)
   This function decodes a Thumb function prologue to determine:
     1) the size of the stack frame
     2) which registers are saved on it
     3) the offsets of saved regs
     4) the offset from the stack pointer to the frame pointer

   A typical Thumb function prologue would create this stack frame
   (offsets relative to FP)
     old SP ->	24  stack parameters
		20  LR
		16  R7
     R7 ->       0  local variables (16 bytes)
     SP ->     -12  additional stack space (12 bytes)
   The frame size would thus be 36 bytes, and the frame offset would be
   12 bytes.  The frame register is R7.
   
   The comments for thumb_skip_prolog() describe the algorithm we use
   to detect the end of the prolog.  */
/* *INDENT-ON* */

static void
thumb_scan_prologue (struct gdbarch *gdbarch, CORE_ADDR prev_pc,
		     CORE_ADDR block_addr, struct arm_prologue_cache *cache)
{
  CORE_ADDR prologue_start;
  CORE_ADDR prologue_end;
  CORE_ADDR current_pc;

  if (find_pc_partial_function (block_addr, NULL, &prologue_start,
				&prologue_end))
    {
      /* See comment in arm_scan_prologue for an explanation of
	 this heuristics.  */
      if (prologue_end > prologue_start + 64)
	{
	  prologue_end = prologue_start + 64;
	}
    }
  else
    /* We're in the boondocks: we have no idea where the start of the
       function is.  */
    return;

  prologue_end = min (prologue_end, prev_pc);

  thumb_analyze_prologue (gdbarch, prologue_start, prologue_end, cache);
}

/* Return 1 if THIS_INSTR might change control flow, 0 otherwise.  */

static int
arm_instruction_changes_pc (uint32_t this_instr)
{
  if (bits (this_instr, 28, 31) == INST_NV)
    /* Unconditional instructions.  */
    switch (bits (this_instr, 24, 27))
      {
      case 0xa:
      case 0xb:
	/* Branch with Link and change to Thumb.  */
	return 1;
      case 0xc:
      case 0xd:
      case 0xe:
	/* Coprocessor register transfer.  */
        if (bits (this_instr, 12, 15) == 15)
	  error (_("Invalid update to pc in instruction"));
	return 0;
      default:
	return 0;
      }
  else
    switch (bits (this_instr, 25, 27))
      {
      case 0x0:
	if (bits (this_instr, 23, 24) == 2 && bit (this_instr, 20) == 0)
	  {
	    /* Multiplies and extra load/stores.  */
	    if (bit (this_instr, 4) == 1 && bit (this_instr, 7) == 1)
	      /* Neither multiplies nor extension load/stores are allowed
		 to modify PC.  */
	      return 0;

	    /* Otherwise, miscellaneous instructions.  */

	    /* BX <reg>, BXJ <reg>, BLX <reg> */
	    if (bits (this_instr, 4, 27) == 0x12fff1
		|| bits (this_instr, 4, 27) == 0x12fff2
		|| bits (this_instr, 4, 27) == 0x12fff3)
	      return 1;

	    /* Other miscellaneous instructions are unpredictable if they
	       modify PC.  */
	    return 0;
	  }
	/* Data processing instruction.  Fall through.  */

      case 0x1:
	if (bits (this_instr, 12, 15) == 15)
	  return 1;
	else
	  return 0;

      case 0x2:
      case 0x3:
	/* Media instructions and architecturally undefined instructions.  */
	if (bits (this_instr, 25, 27) == 3 && bit (this_instr, 4) == 1)
	  return 0;

	/* Stores.  */
	if (bit (this_instr, 20) == 0)
	  return 0;

	/* Loads.  */
	if (bits (this_instr, 12, 15) == ARM_PC_REGNUM)
	  return 1;
	else
	  return 0;

      case 0x4:
	/* Load/store multiple.  */
	if (bit (this_instr, 20) == 1 && bit (this_instr, 15) == 1)
	  return 1;
	else
	  return 0;

      case 0x5:
	/* Branch and branch with link.  */
	return 1;

      case 0x6:
      case 0x7:
	/* Coprocessor transfers or SWIs can not affect PC.  */
	return 0;

      default:
	internal_error (__FILE__, __LINE__, _("bad value in switch"));
      }
}

/* Analyze an ARM mode prologue starting at PROLOGUE_START and
   continuing no further than PROLOGUE_END.  If CACHE is non-NULL,
   fill it in.  Return the first address not recognized as a prologue
   instruction.

   We recognize all the instructions typically found in ARM prologues,
   plus harmless instructions which can be skipped (either for analysis
   purposes, or a more restrictive set that can be skipped when finding
   the end of the prologue).  */

static CORE_ADDR
arm_analyze_prologue (struct gdbarch *gdbarch,
		      CORE_ADDR prologue_start, CORE_ADDR prologue_end,
		      struct arm_prologue_cache *cache)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  int regno;
  CORE_ADDR offset, current_pc;
  pv_t regs[ARM_FPS_REGNUM];
  struct pv_area *stack;
  struct cleanup *back_to;
  int framereg, framesize;
  CORE_ADDR unrecognized_pc = 0;

  /* Search the prologue looking for instructions that set up the
     frame pointer, adjust the stack pointer, and save registers.

     Be careful, however, and if it doesn't look like a prologue,
     don't try to scan it.  If, for instance, a frameless function
     begins with stmfd sp!, then we will tell ourselves there is
     a frame, which will confuse stack traceback, as well as "finish" 
     and other operations that rely on a knowledge of the stack
     traceback.  */

  for (regno = 0; regno < ARM_FPS_REGNUM; regno++)
    regs[regno] = pv_register (regno, 0);
  stack = make_pv_area (ARM_SP_REGNUM, gdbarch_addr_bit (gdbarch));
  back_to = make_cleanup_free_pv_area (stack);

  for (current_pc = prologue_start;
       current_pc < prologue_end;
       current_pc += 4)
    {
      unsigned int insn
	= read_memory_unsigned_integer (current_pc, 4, byte_order_for_code);

      if (insn == 0xe1a0c00d)		/* mov ip, sp */
	{
	  regs[ARM_IP_REGNUM] = regs[ARM_SP_REGNUM];
	  continue;
	}
      else if ((insn & 0xfff00000) == 0xe2800000	/* add Rd, Rn, #n */
	       && pv_is_register (regs[bits (insn, 16, 19)], ARM_SP_REGNUM))
	{
	  unsigned imm = insn & 0xff;                   /* immediate value */
	  unsigned rot = (insn & 0xf00) >> 7;           /* rotate amount */
	  int rd = bits (insn, 12, 15);
	  imm = (imm >> rot) | (imm << (32 - rot));
	  regs[rd] = pv_add_constant (regs[bits (insn, 16, 19)], imm);
	  continue;
	}
      else if ((insn & 0xfff00000) == 0xe2400000	/* sub Rd, Rn, #n */
	       && pv_is_register (regs[bits (insn, 16, 19)], ARM_SP_REGNUM))
	{
	  unsigned imm = insn & 0xff;                   /* immediate value */
	  unsigned rot = (insn & 0xf00) >> 7;           /* rotate amount */
	  int rd = bits (insn, 12, 15);
	  imm = (imm >> rot) | (imm << (32 - rot));
	  regs[rd] = pv_add_constant (regs[bits (insn, 16, 19)], -imm);
	  continue;
	}
      else if ((insn & 0xffff0fff) == 0xe52d0004)	/* str Rd,
							   [sp, #-4]! */
	{
	  if (pv_area_store_would_trash (stack, regs[ARM_SP_REGNUM]))
	    break;
	  regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM], -4);
	  pv_area_store (stack, regs[ARM_SP_REGNUM], 4,
			 regs[bits (insn, 12, 15)]);
	  continue;
	}
      else if ((insn & 0xffff0000) == 0xe92d0000)
	/* stmfd sp!, {..., fp, ip, lr, pc}
	   or
	   stmfd sp!, {a1, a2, a3, a4}  */
	{
	  int mask = insn & 0xffff;

	  if (pv_area_store_would_trash (stack, regs[ARM_SP_REGNUM]))
	    break;

	  /* Calculate offsets of saved registers.  */
	  for (regno = ARM_PC_REGNUM; regno >= 0; regno--)
	    if (mask & (1 << regno))
	      {
		regs[ARM_SP_REGNUM]
		  = pv_add_constant (regs[ARM_SP_REGNUM], -4);
		pv_area_store (stack, regs[ARM_SP_REGNUM], 4, regs[regno]);
	      }
	}
      else if ((insn & 0xffff0000) == 0xe54b0000	/* strb rx,[r11,#-n] */
	       || (insn & 0xffff00f0) == 0xe14b00b0	/* strh rx,[r11,#-n] */
	       || (insn & 0xffffc000) == 0xe50b0000)	/* str  rx,[r11,#-n] */
	{
	  /* No need to add this to saved_regs -- it's just an arg reg.  */
	  continue;
	}
      else if ((insn & 0xffff0000) == 0xe5cd0000	/* strb rx,[sp,#n] */
	       || (insn & 0xffff00f0) == 0xe1cd00b0	/* strh rx,[sp,#n] */
	       || (insn & 0xffffc000) == 0xe58d0000)	/* str  rx,[sp,#n] */
	{
	  /* No need to add this to saved_regs -- it's just an arg reg.  */
	  continue;
	}
      else if ((insn & 0xfff00000) == 0xe8800000	/* stm Rn,
							   { registers } */
	       && pv_is_register (regs[bits (insn, 16, 19)], ARM_SP_REGNUM))
	{
	  /* No need to add this to saved_regs -- it's just arg regs.  */
	  continue;
	}
      else if ((insn & 0xfffff000) == 0xe24cb000)	/* sub fp, ip #n */
	{
	  unsigned imm = insn & 0xff;			/* immediate value */
	  unsigned rot = (insn & 0xf00) >> 7;		/* rotate amount */
	  imm = (imm >> rot) | (imm << (32 - rot));
	  regs[ARM_FP_REGNUM] = pv_add_constant (regs[ARM_IP_REGNUM], -imm);
	}
      else if ((insn & 0xfffff000) == 0xe24dd000)	/* sub sp, sp #n */
	{
	  unsigned imm = insn & 0xff;			/* immediate value */
	  unsigned rot = (insn & 0xf00) >> 7;		/* rotate amount */
	  imm = (imm >> rot) | (imm << (32 - rot));
	  regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM], -imm);
	}
      else if ((insn & 0xffff7fff) == 0xed6d0103	/* stfe f?,
							   [sp, -#c]! */
	       && gdbarch_tdep (gdbarch)->have_fpa_registers)
	{
	  if (pv_area_store_would_trash (stack, regs[ARM_SP_REGNUM]))
	    break;

	  regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM], -12);
	  regno = ARM_F0_REGNUM + ((insn >> 12) & 0x07);
	  pv_area_store (stack, regs[ARM_SP_REGNUM], 12, regs[regno]);
	}
      else if ((insn & 0xffbf0fff) == 0xec2d0200	/* sfmfd f0, 4,
							   [sp!] */
	       && gdbarch_tdep (gdbarch)->have_fpa_registers)
	{
	  int n_saved_fp_regs;
	  unsigned int fp_start_reg, fp_bound_reg;

	  if (pv_area_store_would_trash (stack, regs[ARM_SP_REGNUM]))
	    break;

	  if ((insn & 0x800) == 0x800)		/* N0 is set */
	    {
	      if ((insn & 0x40000) == 0x40000)	/* N1 is set */
		n_saved_fp_regs = 3;
	      else
		n_saved_fp_regs = 1;
	    }
	  else
	    {
	      if ((insn & 0x40000) == 0x40000)	/* N1 is set */
		n_saved_fp_regs = 2;
	      else
		n_saved_fp_regs = 4;
	    }

	  fp_start_reg = ARM_F0_REGNUM + ((insn >> 12) & 0x7);
	  fp_bound_reg = fp_start_reg + n_saved_fp_regs;
	  for (; fp_start_reg < fp_bound_reg; fp_start_reg++)
	    {
	      regs[ARM_SP_REGNUM] = pv_add_constant (regs[ARM_SP_REGNUM], -12);
	      pv_area_store (stack, regs[ARM_SP_REGNUM], 12,
			     regs[fp_start_reg++]);
	    }
	}
      else if ((insn & 0xff000000) == 0xeb000000 && cache == NULL) /* bl */
	{
	  /* Allow some special function calls when skipping the
	     prologue; GCC generates these before storing arguments to
	     the stack.  */
	  CORE_ADDR dest = BranchDest (current_pc, insn);

	  if (skip_prologue_function (gdbarch, dest, 0))
	    continue;
	  else
	    break;
	}
      else if ((insn & 0xf0000000) != 0xe0000000)
	break;			/* Condition not true, exit early.  */
      else if (arm_instruction_changes_pc (insn))
	/* Don't scan past anything that might change control flow.  */
	break;
      else if ((insn & 0xfe500000) == 0xe8100000	/* ldm */
	       && pv_is_register (regs[bits (insn, 16, 19)], ARM_SP_REGNUM))
	/* Ignore block loads from the stack, potentially copying
	   parameters from memory.  */
	continue;
      else if ((insn & 0xfc500000) == 0xe4100000
	       && pv_is_register (regs[bits (insn, 16, 19)], ARM_SP_REGNUM))
	/* Similarly ignore single loads from the stack.  */
	continue;
      else if ((insn & 0xffff0ff0) == 0xe1a00000)
	/* MOV Rd, Rm.  Skip register copies, i.e. saves to another
	   register instead of the stack.  */
	continue;
      else
	{
	  /* The optimizer might shove anything into the prologue,
	     so we just skip what we don't recognize.  */
	  unrecognized_pc = current_pc;
	  continue;
	}
    }

  if (unrecognized_pc == 0)
    unrecognized_pc = current_pc;

  /* The frame size is just the distance from the frame register
     to the original stack pointer.  */
  if (pv_is_register (regs[ARM_FP_REGNUM], ARM_SP_REGNUM))
    {
      /* Frame pointer is fp.  */
      framereg = ARM_FP_REGNUM;
      framesize = -regs[ARM_FP_REGNUM].k;
    }
  else
    {
      /* Try the stack pointer... this is a bit desperate.  */
      framereg = ARM_SP_REGNUM;
      framesize = -regs[ARM_SP_REGNUM].k;
    }

  if (cache)
    {
      cache->framereg = framereg;
      cache->framesize = framesize;

      for (regno = 0; regno < ARM_FPS_REGNUM; regno++)
	if (pv_area_find_reg (stack, gdbarch, regno, &offset))
	  cache->saved_regs[regno].addr = offset;
    }

  if (arm_debug)
    fprintf_unfiltered (gdb_stdlog, "Prologue scan stopped at %s\n",
			paddress (gdbarch, unrecognized_pc));

  do_cleanups (back_to);
  return unrecognized_pc;
}

static void
arm_scan_prologue (struct frame_info *this_frame,
		   struct arm_prologue_cache *cache)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  int regno;
  CORE_ADDR prologue_start, prologue_end, current_pc;
  CORE_ADDR prev_pc = get_frame_pc (this_frame);
  CORE_ADDR block_addr = get_frame_address_in_block (this_frame);
  pv_t regs[ARM_FPS_REGNUM];
  struct pv_area *stack;
  struct cleanup *back_to;
  CORE_ADDR offset;

  /* Assume there is no frame until proven otherwise.  */
  cache->framereg = ARM_SP_REGNUM;
  cache->framesize = 0;

  /* Check for Thumb prologue.  */
  if (arm_frame_is_thumb (this_frame))
    {
      thumb_scan_prologue (gdbarch, prev_pc, block_addr, cache);
      return;
    }

  /* Find the function prologue.  If we can't find the function in
     the symbol table, peek in the stack frame to find the PC.  */
  if (find_pc_partial_function (block_addr, NULL, &prologue_start,
				&prologue_end))
    {
      /* One way to find the end of the prologue (which works well
         for unoptimized code) is to do the following:

	    struct symtab_and_line sal = find_pc_line (prologue_start, 0);

	    if (sal.line == 0)
	      prologue_end = prev_pc;
	    else if (sal.end < prologue_end)
	      prologue_end = sal.end;

	 This mechanism is very accurate so long as the optimizer
	 doesn't move any instructions from the function body into the
	 prologue.  If this happens, sal.end will be the last
	 instruction in the first hunk of prologue code just before
	 the first instruction that the scheduler has moved from
	 the body to the prologue.

	 In order to make sure that we scan all of the prologue
	 instructions, we use a slightly less accurate mechanism which
	 may scan more than necessary.  To help compensate for this
	 lack of accuracy, the prologue scanning loop below contains
	 several clauses which'll cause the loop to terminate early if
	 an implausible prologue instruction is encountered.

	 The expression

	      prologue_start + 64

	 is a suitable endpoint since it accounts for the largest
	 possible prologue plus up to five instructions inserted by
	 the scheduler.  */

      if (prologue_end > prologue_start + 64)
	{
	  prologue_end = prologue_start + 64;	/* See above.  */
	}
    }
  else
    {
      /* We have no symbol information.  Our only option is to assume this
	 function has a standard stack frame and the normal frame register.
	 Then, we can find the value of our frame pointer on entrance to
	 the callee (or at the present moment if this is the innermost frame).
	 The value stored there should be the address of the stmfd + 8.  */
      CORE_ADDR frame_loc;
      LONGEST return_value;
      gdb_byte return_buf[sizeof (LONGEST)];

      frame_loc = get_frame_register_unsigned (this_frame, ARM_FP_REGNUM);
      if (target_read_memory (frame_loc, return_buf, 4))
        return;
      else
        {
          return_value = extract_signed_integer (return_buf, 4, byte_order);
          prologue_start = gdbarch_addr_bits_remove
			     (gdbarch, return_value) - 8;
          prologue_end = prologue_start + 64;	/* See above.  */
        }
    }

  if (prev_pc < prologue_end)
    prologue_end = prev_pc;

  arm_analyze_prologue (gdbarch, prologue_start, prologue_end, cache);
}

static struct arm_prologue_cache *
arm_make_prologue_cache (struct frame_info *this_frame)
{
  int reg;
  struct arm_prologue_cache *cache;
  CORE_ADDR unwound_fp;

  cache = FRAME_OBSTACK_ZALLOC (struct arm_prologue_cache);
  cache->saved_regs = trad_frame_alloc_saved_regs (this_frame);

  arm_scan_prologue (this_frame, cache);

  unwound_fp = get_frame_register_unsigned (this_frame, cache->framereg);
  if (unwound_fp == 0)
    return cache;

  cache->prev_sp = unwound_fp + cache->framesize;

  /* Calculate actual addresses of saved registers using offsets
     determined by arm_scan_prologue.  */
  for (reg = 0; reg < gdbarch_num_regs (get_frame_arch (this_frame)); reg++)
    if (trad_frame_addr_p (cache->saved_regs, reg))
      cache->saved_regs[reg].addr += cache->prev_sp;

  return cache;
}

/* Our frame ID for a normal frame is the current function's starting PC
   and the caller's SP when we were called.  */

static void
arm_prologue_this_id (struct frame_info *this_frame,
		      void **this_cache,
		      struct frame_id *this_id)
{
  struct arm_prologue_cache *cache;
  struct frame_id id;
  CORE_ADDR pc, func;

  if (*this_cache == NULL)
    *this_cache = arm_make_prologue_cache (this_frame);
  cache = *this_cache;

  /* This is meant to halt the backtrace at "_start".  */
  pc = get_frame_pc (this_frame);
  if (pc <= gdbarch_tdep (get_frame_arch (this_frame))->lowest_pc)
    return;

  /* If we've hit a wall, stop.  */
  if (cache->prev_sp == 0)
    return;

  /* Use function start address as part of the frame ID.  If we cannot
     identify the start address (due to missing symbol information),
     fall back to just using the current PC.  */
  func = get_frame_func (this_frame);
  if (!func)
    func = pc;

  id = frame_id_build (cache->prev_sp, func);
  *this_id = id;
}

static struct value *
arm_prologue_prev_register (struct frame_info *this_frame,
			    void **this_cache,
			    int prev_regnum)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  struct arm_prologue_cache *cache;

  if (*this_cache == NULL)
    *this_cache = arm_make_prologue_cache (this_frame);
  cache = *this_cache;

  /* If we are asked to unwind the PC, then we need to return the LR
     instead.  The prologue may save PC, but it will point into this
     frame's prologue, not the next frame's resume location.  Also
     strip the saved T bit.  A valid LR may have the low bit set, but
     a valid PC never does.  */
  if (prev_regnum == ARM_PC_REGNUM)
    {
      CORE_ADDR lr;

      lr = frame_unwind_register_unsigned (this_frame, ARM_LR_REGNUM);
      return frame_unwind_got_constant (this_frame, prev_regnum,
					arm_addr_bits_remove (gdbarch, lr));
    }

  /* SP is generally not saved to the stack, but this frame is
     identified by the next frame's stack pointer at the time of the call.
     The value was already reconstructed into PREV_SP.  */
  if (prev_regnum == ARM_SP_REGNUM)
    return frame_unwind_got_constant (this_frame, prev_regnum, cache->prev_sp);

  /* The CPSR may have been changed by the call instruction and by the
     called function.  The only bit we can reconstruct is the T bit,
     by checking the low bit of LR as of the call.  This is a reliable
     indicator of Thumb-ness except for some ARM v4T pre-interworking
     Thumb code, which could get away with a clear low bit as long as
     the called function did not use bx.  Guess that all other
     bits are unchanged; the condition flags are presumably lost,
     but the processor status is likely valid.  */
  if (prev_regnum == ARM_PS_REGNUM)
    {
      CORE_ADDR lr, cpsr;
      ULONGEST t_bit = arm_psr_thumb_bit (gdbarch);

      cpsr = get_frame_register_unsigned (this_frame, prev_regnum);
      lr = frame_unwind_register_unsigned (this_frame, ARM_LR_REGNUM);
      if (IS_THUMB_ADDR (lr))
	cpsr |= t_bit;
      else
	cpsr &= ~t_bit;
      return frame_unwind_got_constant (this_frame, prev_regnum, cpsr);
    }

  return trad_frame_get_prev_register (this_frame, cache->saved_regs,
				       prev_regnum);
}

struct frame_unwind arm_prologue_unwind = {
  NORMAL_FRAME,
  default_frame_unwind_stop_reason,
  arm_prologue_this_id,
  arm_prologue_prev_register,
  NULL,
  default_frame_sniffer
};

/* Maintain a list of ARM exception table entries per objfile, similar to the
   list of mapping symbols.  We only cache entries for standard ARM-defined
   personality routines; the cache will contain only the frame unwinding
   instructions associated with the entry (not the descriptors).  */

static const struct objfile_data *arm_exidx_data_key;

struct arm_exidx_entry
{
  bfd_vma addr;
  gdb_byte *entry;
};
typedef struct arm_exidx_entry arm_exidx_entry_s;
DEF_VEC_O(arm_exidx_entry_s);

struct arm_exidx_data
{
  VEC(arm_exidx_entry_s) **section_maps;
};

static void
arm_exidx_data_free (struct objfile *objfile, void *arg)
{
  struct arm_exidx_data *data = arg;
  unsigned int i;

  for (i = 0; i < objfile->obfd->section_count; i++)
    VEC_free (arm_exidx_entry_s, data->section_maps[i]);
}

static inline int
arm_compare_exidx_entries (const struct arm_exidx_entry *lhs,
			   const struct arm_exidx_entry *rhs)
{
  return lhs->addr < rhs->addr;
}

static struct obj_section *
arm_obj_section_from_vma (struct objfile *objfile, bfd_vma vma)
{
  struct obj_section *osect;

  ALL_OBJFILE_OSECTIONS (objfile, osect)
    if (bfd_get_section_flags (objfile->obfd,
			       osect->the_bfd_section) & SEC_ALLOC)
      {
	bfd_vma start, size;
	start = bfd_get_section_vma (objfile->obfd, osect->the_bfd_section);
	size = bfd_get_section_size (osect->the_bfd_section);

	if (start <= vma && vma < start + size)
	  return osect;
      }

  return NULL;
}

/* Parse contents of exception table and exception index sections
   of OBJFILE, and fill in the exception table entry cache.

   For each entry that refers to a standard ARM-defined personality
   routine, extract the frame unwinding instructions (from either
   the index or the table section).  The unwinding instructions
   are normalized by:
    - extracting them from the rest of the table data
    - converting to host endianness
    - appending the implicit 0xb0 ("Finish") code

   The extracted and normalized instructions are stored for later
   retrieval by the arm_find_exidx_entry routine.  */
 
static void
arm_exidx_new_objfile (struct objfile *objfile)
{
  struct cleanup *cleanups;
  struct arm_exidx_data *data;
  asection *exidx, *extab;
  bfd_vma exidx_vma = 0, extab_vma = 0;
  bfd_size_type exidx_size = 0, extab_size = 0;
  gdb_byte *exidx_data = NULL, *extab_data = NULL;
  LONGEST i;

  /* If we've already touched this file, do nothing.  */
  if (!objfile || objfile_data (objfile, arm_exidx_data_key) != NULL)
    return;
  cleanups = make_cleanup (null_cleanup, NULL);

  /* Read contents of exception table and index.  */
  exidx = bfd_get_section_by_name (objfile->obfd, ".ARM.exidx");
  if (exidx)
    {
      exidx_vma = bfd_section_vma (objfile->obfd, exidx);
      exidx_size = bfd_get_section_size (exidx);
      exidx_data = xmalloc (exidx_size);
      make_cleanup (xfree, exidx_data);

      if (!bfd_get_section_contents (objfile->obfd, exidx,
				     exidx_data, 0, exidx_size))
	{
	  do_cleanups (cleanups);
	  return;
	}
    }

  extab = bfd_get_section_by_name (objfile->obfd, ".ARM.extab");
  if (extab)
    {
      extab_vma = bfd_section_vma (objfile->obfd, extab);
      extab_size = bfd_get_section_size (extab);
      extab_data = xmalloc (extab_size);
      make_cleanup (xfree, extab_data);

      if (!bfd_get_section_contents (objfile->obfd, extab,
				     extab_data, 0, extab_size))
	{
	  do_cleanups (cleanups);
	  return;
	}
    }

  /* Allocate exception table data structure.  */
  data = OBSTACK_ZALLOC (&objfile->objfile_obstack, struct arm_exidx_data);
  set_objfile_data (objfile, arm_exidx_data_key, data);
  data->section_maps = OBSTACK_CALLOC (&objfile->objfile_obstack,
				       objfile->obfd->section_count,
				       VEC(arm_exidx_entry_s) *);

  /* Fill in exception table.  */
  for (i = 0; i < exidx_size / 8; i++)
    {
      struct arm_exidx_entry new_exidx_entry;
      bfd_vma idx = bfd_h_get_32 (objfile->obfd, exidx_data + i * 8);
      bfd_vma val = bfd_h_get_32 (objfile->obfd, exidx_data + i * 8 + 4);
      bfd_vma addr = 0, word = 0;
      int n_bytes = 0, n_words = 0;
      struct obj_section *sec;
      gdb_byte *entry = NULL;

      /* Extract address of start of function.  */
      idx = ((idx & 0x7fffffff) ^ 0x40000000) - 0x40000000;
      idx += exidx_vma + i * 8;

      /* Find section containing function and compute section offset.  */
      sec = arm_obj_section_from_vma (objfile, idx);
      if (sec == NULL)
	continue;
      idx -= bfd_get_section_vma (objfile->obfd, sec->the_bfd_section);

      /* Determine address of exception table entry.  */
      if (val == 1)
	{
	  /* EXIDX_CANTUNWIND -- no exception table entry present.  */
	}
      else if ((val & 0xff000000) == 0x80000000)
	{
	  /* Exception table entry embedded in .ARM.exidx
	     -- must be short form.  */
	  word = val;
	  n_bytes = 3;
	}
      else if (!(val & 0x80000000))
	{
	  /* Exception table entry in .ARM.extab.  */
	  addr = ((val & 0x7fffffff) ^ 0x40000000) - 0x40000000;
	  addr += exidx_vma + i * 8 + 4;

	  if (addr >= extab_vma && addr + 4 <= extab_vma + extab_size)
	    {
	      word = bfd_h_get_32 (objfile->obfd,
				   extab_data + addr - extab_vma);
	      addr += 4;

	      if ((word & 0xff000000) == 0x80000000)
		{
		  /* Short form.  */
		  n_bytes = 3;
		}
	      else if ((word & 0xff000000) == 0x81000000
		       || (word & 0xff000000) == 0x82000000)
		{
		  /* Long form.  */
		  n_bytes = 2;
		  n_words = ((word >> 16) & 0xff);
		}
	      else if (!(word & 0x80000000))
		{
		  bfd_vma pers;
		  struct obj_section *pers_sec;
		  int gnu_personality = 0;

		  /* Custom personality routine.  */
		  pers = ((word & 0x7fffffff) ^ 0x40000000) - 0x40000000;
		  pers = UNMAKE_THUMB_ADDR (pers + addr - 4);

		  /* Check whether we've got one of the variants of the
		     GNU personality routines.  */
		  pers_sec = arm_obj_section_from_vma (objfile, pers);
		  if (pers_sec)
		    {
		      static const char *personality[] = 
			{
			  "__gcc_personality_v0",
			  "__gxx_personality_v0",
			  "__gcj_personality_v0",
			  "__gnu_objc_personality_v0",
			  NULL
			};

		      CORE_ADDR pc = pers + obj_section_offset (pers_sec);
		      int k;

		      for (k = 0; personality[k]; k++)
			if (lookup_minimal_symbol_by_pc_name
			      (pc, personality[k], objfile))
			  {
			    gnu_personality = 1;
			    break;
			  }
		    }

		  /* If so, the next word contains a word count in the high
		     byte, followed by the same unwind instructions as the
		     pre-defined forms.  */
		  if (gnu_personality
		      && addr + 4 <= extab_vma + extab_size)
		    {
		      word = bfd_h_get_32 (objfile->obfd,
					   extab_data + addr - extab_vma);
		      addr += 4;
		      n_bytes = 3;
		      n_words = ((word >> 24) & 0xff);
		    }
		}
	    }
	}

      /* Sanity check address.  */
      if (n_words)
	if (addr < extab_vma || addr + 4 * n_words > extab_vma + extab_size)
	  n_words = n_bytes = 0;

      /* The unwind instructions reside in WORD (only the N_BYTES least
	 significant bytes are valid), followed by N_WORDS words in the
	 extab section starting at ADDR.  */
      if (n_bytes || n_words)
	{
	  gdb_byte *p = entry = obstack_alloc (&objfile->objfile_obstack,
					       n_bytes + n_words * 4 + 1);

	  while (n_bytes--)
	    *p++ = (gdb_byte) ((word >> (8 * n_bytes)) & 0xff);

	  while (n_words--)
	    {
	      word = bfd_h_get_32 (objfile->obfd,
				   extab_data + addr - extab_vma);
	      addr += 4;

	      *p++ = (gdb_byte) ((word >> 24) & 0xff);
	      *p++ = (gdb_byte) ((word >> 16) & 0xff);
	      *p++ = (gdb_byte) ((word >> 8) & 0xff);
	      *p++ = (gdb_byte) (word & 0xff);
	    }

	  /* Implied "Finish" to terminate the list.  */
	  *p++ = 0xb0;
	}

      /* Push entry onto vector.  They are guaranteed to always
	 appear in order of increasing addresses.  */
      new_exidx_entry.addr = idx;
      new_exidx_entry.entry = entry;
      VEC_safe_push (arm_exidx_entry_s,
		     data->section_maps[sec->the_bfd_section->index],
		     &new_exidx_entry);
    }

  do_cleanups (cleanups);
}

/* Search for the exception table entry covering MEMADDR.  If one is found,
   return a pointer to its data.  Otherwise, return 0.  If START is non-NULL,
   set *START to the start of the region covered by this entry.  */

static gdb_byte *
arm_find_exidx_entry (CORE_ADDR memaddr, CORE_ADDR *start)
{
  struct obj_section *sec;

  sec = find_pc_section (memaddr);
  if (sec != NULL)
    {
      struct arm_exidx_data *data;
      VEC(arm_exidx_entry_s) *map;
      struct arm_exidx_entry map_key = { memaddr - obj_section_addr (sec), 0 };
      unsigned int idx;

      data = objfile_data (sec->objfile, arm_exidx_data_key);
      if (data != NULL)
	{
	  map = data->section_maps[sec->the_bfd_section->index];
	  if (!VEC_empty (arm_exidx_entry_s, map))
	    {
	      struct arm_exidx_entry *map_sym;

	      idx = VEC_lower_bound (arm_exidx_entry_s, map, &map_key,
				     arm_compare_exidx_entries);

	      /* VEC_lower_bound finds the earliest ordered insertion
		 point.  If the following symbol starts at this exact
		 address, we use that; otherwise, the preceding
		 exception table entry covers this address.  */
	      if (idx < VEC_length (arm_exidx_entry_s, map))
		{
		  map_sym = VEC_index (arm_exidx_entry_s, map, idx);
		  if (map_sym->addr == map_key.addr)
		    {
		      if (start)
			*start = map_sym->addr + obj_section_addr (sec);
		      return map_sym->entry;
		    }
		}

	      if (idx > 0)
		{
		  map_sym = VEC_index (arm_exidx_entry_s, map, idx - 1);
		  if (start)
		    *start = map_sym->addr + obj_section_addr (sec);
		  return map_sym->entry;
		}
	    }
	}
    }

  return NULL;
}

/* Given the current frame THIS_FRAME, and its associated frame unwinding
   instruction list from the ARM exception table entry ENTRY, allocate and
   return a prologue cache structure describing how to unwind this frame.

   Return NULL if the unwinding instruction list contains a "spare",
   "reserved" or "refuse to unwind" instruction as defined in section
   "9.3 Frame unwinding instructions" of the "Exception Handling ABI
   for the ARM Architecture" document.  */

static struct arm_prologue_cache *
arm_exidx_fill_cache (struct frame_info *this_frame, gdb_byte *entry)
{
  CORE_ADDR vsp = 0;
  int vsp_valid = 0;

  struct arm_prologue_cache *cache;
  cache = FRAME_OBSTACK_ZALLOC (struct arm_prologue_cache);
  cache->saved_regs = trad_frame_alloc_saved_regs (this_frame);

  for (;;)
    {
      gdb_byte insn;

      /* Whenever we reload SP, we actually have to retrieve its
	 actual value in the current frame.  */
      if (!vsp_valid)
	{
	  if (trad_frame_realreg_p (cache->saved_regs, ARM_SP_REGNUM))
	    {
	      int reg = cache->saved_regs[ARM_SP_REGNUM].realreg;
	      vsp = get_frame_register_unsigned (this_frame, reg);
	    }
	  else
	    {
	      CORE_ADDR addr = cache->saved_regs[ARM_SP_REGNUM].addr;
	      vsp = get_frame_memory_unsigned (this_frame, addr, 4);
	    }

	  vsp_valid = 1;
	}

      /* Decode next unwind instruction.  */
      insn = *entry++;

      if ((insn & 0xc0) == 0)
	{
	  int offset = insn & 0x3f;
	  vsp += (offset << 2) + 4;
	}
      else if ((insn & 0xc0) == 0x40)
	{
	  int offset = insn & 0x3f;
	  vsp -= (offset << 2) + 4;
	}
      else if ((insn & 0xf0) == 0x80)
	{
	  int mask = ((insn & 0xf) << 8) | *entry++;
	  int i;

	  /* The special case of an all-zero mask identifies
	     "Refuse to unwind".  We return NULL to fall back
	     to the prologue analyzer.  */
	  if (mask == 0)
	    return NULL;

	  /* Pop registers r4..r15 under mask.  */
	  for (i = 0; i < 12; i++)
	    if (mask & (1 << i))
	      {
	        cache->saved_regs[4 + i].addr = vsp;
		vsp += 4;
	      }

	  /* Special-case popping SP -- we need to reload vsp.  */
	  if (mask & (1 << (ARM_SP_REGNUM - 4)))
	    vsp_valid = 0;
	}
      else if ((insn & 0xf0) == 0x90)
	{
	  int reg = insn & 0xf;

	  /* Reserved cases.  */
	  if (reg == ARM_SP_REGNUM || reg == ARM_PC_REGNUM)
	    return NULL;

	  /* Set SP from another register and mark VSP for reload.  */
	  cache->saved_regs[ARM_SP_REGNUM] = cache->saved_regs[reg];
	  vsp_valid = 0;
	}
      else if ((insn & 0xf0) == 0xa0)
	{
	  int count = insn & 0x7;
	  int pop_lr = (insn & 0x8) != 0;
	  int i;

	  /* Pop r4..r[4+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[4 + i].addr = vsp;
	      vsp += 4;
	    }

	  /* If indicated by flag, pop LR as well.  */
	  if (pop_lr)
	    {
	      cache->saved_regs[ARM_LR_REGNUM].addr = vsp;
	      vsp += 4;
	    }
	}
      else if (insn == 0xb0)
	{
	  /* We could only have updated PC by popping into it; if so, it
	     will show up as address.  Otherwise, copy LR into PC.  */
	  if (!trad_frame_addr_p (cache->saved_regs, ARM_PC_REGNUM))
	    cache->saved_regs[ARM_PC_REGNUM]
	      = cache->saved_regs[ARM_LR_REGNUM];

	  /* We're done.  */
	  break;
	}
      else if (insn == 0xb1)
	{
	  int mask = *entry++;
	  int i;

	  /* All-zero mask and mask >= 16 is "spare".  */
	  if (mask == 0 || mask >= 16)
	    return NULL;

	  /* Pop r0..r3 under mask.  */
	  for (i = 0; i < 4; i++)
	    if (mask & (1 << i))
	      {
		cache->saved_regs[i].addr = vsp;
		vsp += 4;
	      }
	}
      else if (insn == 0xb2)
	{
	  ULONGEST offset = 0;
	  unsigned shift = 0;

	  do
	    {
	      offset |= (*entry & 0x7f) << shift;
	      shift += 7;
	    }
	  while (*entry++ & 0x80);

	  vsp += 0x204 + (offset << 2);
	}
      else if (insn == 0xb3)
	{
	  int start = *entry >> 4;
	  int count = (*entry++) & 0xf;
	  int i;

	  /* Only registers D0..D15 are valid here.  */
	  if (start + count >= 16)
	    return NULL;

	  /* Pop VFP double-precision registers D[start]..D[start+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_D0_REGNUM + start + i].addr = vsp;
	      vsp += 8;
	    }

	  /* Add an extra 4 bytes for FSTMFDX-style stack.  */
	  vsp += 4;
	}
      else if ((insn & 0xf8) == 0xb8)
	{
	  int count = insn & 0x7;
	  int i;

	  /* Pop VFP double-precision registers D[8]..D[8+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_D0_REGNUM + 8 + i].addr = vsp;
	      vsp += 8;
	    }

	  /* Add an extra 4 bytes for FSTMFDX-style stack.  */
	  vsp += 4;
	}
      else if (insn == 0xc6)
	{
	  int start = *entry >> 4;
	  int count = (*entry++) & 0xf;
	  int i;

	  /* Only registers WR0..WR15 are valid.  */
	  if (start + count >= 16)
	    return NULL;

	  /* Pop iwmmx registers WR[start]..WR[start+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_WR0_REGNUM + start + i].addr = vsp;
	      vsp += 8;
	    }
	}
      else if (insn == 0xc7)
	{
	  int mask = *entry++;
	  int i;

	  /* All-zero mask and mask >= 16 is "spare".  */
	  if (mask == 0 || mask >= 16)
	    return NULL;

	  /* Pop iwmmx general-purpose registers WCGR0..WCGR3 under mask.  */
	  for (i = 0; i < 4; i++)
	    if (mask & (1 << i))
	      {
		cache->saved_regs[ARM_WCGR0_REGNUM + i].addr = vsp;
		vsp += 4;
	      }
	}
      else if ((insn & 0xf8) == 0xc0)
	{
	  int count = insn & 0x7;
	  int i;

	  /* Pop iwmmx registers WR[10]..WR[10+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_WR0_REGNUM + 10 + i].addr = vsp;
	      vsp += 8;
	    }
	}
      else if (insn == 0xc8)
	{
	  int start = *entry >> 4;
	  int count = (*entry++) & 0xf;
	  int i;

	  /* Only registers D0..D31 are valid.  */
	  if (start + count >= 16)
	    return NULL;

	  /* Pop VFP double-precision registers
	     D[16+start]..D[16+start+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_D0_REGNUM + 16 + start + i].addr = vsp;
	      vsp += 8;
	    }
	}
      else if (insn == 0xc9)
	{
	  int start = *entry >> 4;
	  int count = (*entry++) & 0xf;
	  int i;

	  /* Pop VFP double-precision registers D[start]..D[start+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_D0_REGNUM + start + i].addr = vsp;
	      vsp += 8;
	    }
	}
      else if ((insn & 0xf8) == 0xd0)
	{
	  int count = insn & 0x7;
	  int i;

	  /* Pop VFP double-precision registers D[8]..D[8+count].  */
	  for (i = 0; i <= count; i++)
	    {
	      cache->saved_regs[ARM_D0_REGNUM + 8 + i].addr = vsp;
	      vsp += 8;
	    }
	}
      else
	{
	  /* Everything else is "spare".  */
	  return NULL;
	}
    }

  /* If we restore SP from a register, assume this was the frame register.
     Otherwise just fall back to SP as frame register.  */
  if (trad_frame_realreg_p (cache->saved_regs, ARM_SP_REGNUM))
    cache->framereg = cache->saved_regs[ARM_SP_REGNUM].realreg;
  else
    cache->framereg = ARM_SP_REGNUM;

  /* Determine offset to previous frame.  */
  cache->framesize
    = vsp - get_frame_register_unsigned (this_frame, cache->framereg);

  /* We already got the previous SP.  */
  cache->prev_sp = vsp;

  return cache;
}

/* Unwinding via ARM exception table entries.  Note that the sniffer
   already computes a filled-in prologue cache, which is then used
   with the same arm_prologue_this_id and arm_prologue_prev_register
   routines also used for prologue-parsing based unwinding.  */

static int
arm_exidx_unwind_sniffer (const struct frame_unwind *self,
			  struct frame_info *this_frame,
			  void **this_prologue_cache)
{
  struct gdbarch *gdbarch = get_frame_arch (this_frame);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  CORE_ADDR addr_in_block, exidx_region, func_start;
  struct arm_prologue_cache *cache;
  gdb_byte *entry;

  /* See if we have an ARM exception table entry covering this address.  */
  addr_in_block = get_frame_address_in_block (this_frame);
  entry = arm_find_exidx_entry (addr_in_block, &exidx_region);
  if (!entry)
    return 0;

  /* The ARM exception table does not describe unwind information
     for arbitrary PC values, but is guaranteed to be correct only
     at call sites.  We have to decide here whether we want to use
     ARM exception table information for this frame, or fall back
     to using prologue parsing.  (Note that if we have DWARF CFI,
     this sniffer isn't even called -- CFI is always preferred.)

     Before we make this decision, however, we check whether we
     actually have *symbol* information for the current frame.
     If not, prologue parsing would not work anyway, so we might
     as well use the exception table and hope for the best.  */
  if (find_pc_partial_function (addr_in_block, NULL, &func_start, NULL))
    {
      int exc_valid = 0;

      /* If the next frame is "normal", we are at a call site in this
	 frame, so exception information is guaranteed to be valid.  */
      if (get_next_frame (this_frame)
	  && get_frame_type (get_next_frame (this_frame)) == NORMAL_FRAME)
	exc_valid = 1;

      /* We also assume exception information is valid if we're currently
	 blocked in a system call.  The system library is supposed to
	 ensure this, so that e.g. pthread cancellation works.  */
      if (arm_frame_is_thumb (this_frame))
	{
	  LONGEST insn;

	  if (safe_read_memory_integer (get_frame_pc (this_frame) - 2, 2,
					byte_order_for_code, &insn)
	      && (insn & 0xff00) == 0xdf00 /* svc */)
	    exc_valid = 1;
	}
      else
	{
	  LONGEST insn;

	  if (safe_read_memory_integer (get_frame_pc (this_frame) - 4, 4,
					byte_order_for_code, &insn)
	      && (insn & 0x0f000000) == 0x0f000000 /* svc */)
	    exc_valid = 1;
	}
	
      /* Bail out if we don't know that exception information is valid.  */
      if (!exc_valid)
	return 0;

     /* The ARM exception index does not mark the *end* of the region
	covered by the entry, and some functions will not have any entry.
	To correctly recognize the end of the covered region, the linker
	should have inserted dummy records with a CANTUNWIND marker.

	Unfortunately, current versions of GNU ld do not reliably do
	this, and thus we may have found an incorrect entry above.
	As a (temporary) sanity check, we only use the entry if it
	lies *within* the bounds of the function.  Note that this check
	might reject perfectly valid entries that just happen to cover
	multiple functions; therefore this check ought to be removed
	once the linker is fixed.  */
      if (func_start > exidx_region)
	return 0;
    }

  /* Decode the list of unwinding instructions into a prologue cache.
     Note that this may fail due to e.g. a "refuse to unwind" code.  */
  cache = arm_exidx_fill_cache (this_frame, entry);
  if (!cache)
    return 0;

  *this_prologue_cache = cache;
  return 1;
}

struct frame_unwind arm_exidx_unwind = {
  NORMAL_FRAME,
  default_frame_unwind_stop_reason,
  arm_prologue_this_id,
  arm_prologue_prev_register,
  NULL,
  arm_exidx_unwind_sniffer
};

static struct arm_prologue_cache *
arm_make_stub_cache (struct frame_info *this_frame)
{
  struct arm_prologue_cache *cache;

  cache = FRAME_OBSTACK_ZALLOC (struct arm_prologue_cache);
  cache->saved_regs = trad_frame_alloc_saved_regs (this_frame);

  cache->prev_sp = get_frame_register_unsigned (this_frame, ARM_SP_REGNUM);

  return cache;
}

/* Our frame ID for a stub frame is the current SP and LR.  */

static void
arm_stub_this_id (struct frame_info *this_frame,
		  void **this_cache,
		  struct frame_id *this_id)
{
  struct arm_prologue_cache *cache;

  if (*this_cache == NULL)
    *this_cache = arm_make_stub_cache (this_frame);
  cache = *this_cache;

  *this_id = frame_id_build (cache->prev_sp, get_frame_pc (this_frame));
}

static int
arm_stub_unwind_sniffer (const struct frame_unwind *self,
			 struct frame_info *this_frame,
			 void **this_prologue_cache)
{
  CORE_ADDR addr_in_block;
  char dummy[4];

  addr_in_block = get_frame_address_in_block (this_frame);
  if (in_plt_section (addr_in_block, NULL)
      /* We also use the stub winder if the target memory is unreadable
	 to avoid having the prologue unwinder trying to read it.  */
      || target_read_memory (get_frame_pc (this_frame), dummy, 4) != 0)
    return 1;

  return 0;
}

struct frame_unwind arm_stub_unwind = {
  NORMAL_FRAME,
  default_frame_unwind_stop_reason,
  arm_stub_this_id,
  arm_prologue_prev_register,
  NULL,
  arm_stub_unwind_sniffer
};

static CORE_ADDR
arm_normal_frame_base (struct frame_info *this_frame, void **this_cache)
{
  struct arm_prologue_cache *cache;

  if (*this_cache == NULL)
    *this_cache = arm_make_prologue_cache (this_frame);
  cache = *this_cache;

  return cache->prev_sp - cache->framesize;
}

struct frame_base arm_normal_base = {
  &arm_prologue_unwind,
  arm_normal_frame_base,
  arm_normal_frame_base,
  arm_normal_frame_base
};

/* Assuming THIS_FRAME is a dummy, return the frame ID of that
   dummy frame.  The frame ID's base needs to match the TOS value
   saved by save_dummy_frame_tos() and returned from
   arm_push_dummy_call, and the PC needs to match the dummy frame's
   breakpoint.  */

static struct frame_id
arm_dummy_id (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  return frame_id_build (get_frame_register_unsigned (this_frame,
						      ARM_SP_REGNUM),
			 get_frame_pc (this_frame));
}

/* Given THIS_FRAME, find the previous frame's resume PC (which will
   be used to construct the previous frame's ID, after looking up the
   containing function).  */

static CORE_ADDR
arm_unwind_pc (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  CORE_ADDR pc;
  pc = frame_unwind_register_unsigned (this_frame, ARM_PC_REGNUM);
  return arm_addr_bits_remove (gdbarch, pc);
}

static CORE_ADDR
arm_unwind_sp (struct gdbarch *gdbarch, struct frame_info *this_frame)
{
  return frame_unwind_register_unsigned (this_frame, ARM_SP_REGNUM);
}

static struct value *
arm_dwarf2_prev_register (struct frame_info *this_frame, void **this_cache,
			  int regnum)
{
  struct gdbarch * gdbarch = get_frame_arch (this_frame);
  CORE_ADDR lr, cpsr;
  ULONGEST t_bit = arm_psr_thumb_bit (gdbarch);

  switch (regnum)
    {
    case ARM_PC_REGNUM:
      /* The PC is normally copied from the return column, which
	 describes saves of LR.  However, that version may have an
	 extra bit set to indicate Thumb state.  The bit is not
	 part of the PC.  */
      lr = frame_unwind_register_unsigned (this_frame, ARM_LR_REGNUM);
      return frame_unwind_got_constant (this_frame, regnum,
					arm_addr_bits_remove (gdbarch, lr));

    case ARM_PS_REGNUM:
      /* Reconstruct the T bit; see arm_prologue_prev_register for details.  */
      cpsr = get_frame_register_unsigned (this_frame, regnum);
      lr = frame_unwind_register_unsigned (this_frame, ARM_LR_REGNUM);
      if (IS_THUMB_ADDR (lr))
	cpsr |= t_bit;
      else
	cpsr &= ~t_bit;
      return frame_unwind_got_constant (this_frame, regnum, cpsr);

    default:
      internal_error (__FILE__, __LINE__,
		      _("Unexpected register %d"), regnum);
    }
}

static void
arm_dwarf2_frame_init_reg (struct gdbarch *gdbarch, int regnum,
			   struct dwarf2_frame_state_reg *reg,
			   struct frame_info *this_frame)
{
  switch (regnum)
    {
    case ARM_PC_REGNUM:
    case ARM_PS_REGNUM:
      reg->how = DWARF2_FRAME_REG_FN;
      reg->loc.fn = arm_dwarf2_prev_register;
      break;
    case ARM_SP_REGNUM:
      reg->how = DWARF2_FRAME_REG_CFA;
      break;
    }
}

/* Return true if we are in the function's epilogue, i.e. after the
   instruction that destroyed the function's stack frame.  */

static int
thumb_in_function_epilogue_p (struct gdbarch *gdbarch, CORE_ADDR pc)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned int insn, insn2;
  int found_return = 0, found_stack_adjust = 0;
  CORE_ADDR func_start, func_end;
  CORE_ADDR scan_pc;
  gdb_byte buf[4];

  if (!find_pc_partial_function (pc, NULL, &func_start, &func_end))
    return 0;

  /* The epilogue is a sequence of instructions along the following lines:

    - add stack frame size to SP or FP
    - [if frame pointer used] restore SP from FP
    - restore registers from SP [may include PC]
    - a return-type instruction [if PC wasn't already restored]

    In a first pass, we scan forward from the current PC and verify the
    instructions we find as compatible with this sequence, ending in a
    return instruction.

    However, this is not sufficient to distinguish indirect function calls
    within a function from indirect tail calls in the epilogue in some cases.
    Therefore, if we didn't already find any SP-changing instruction during
    forward scan, we add a backward scanning heuristic to ensure we actually
    are in the epilogue.  */

  scan_pc = pc;
  while (scan_pc < func_end && !found_return)
    {
      if (target_read_memory (scan_pc, buf, 2))
	break;

      scan_pc += 2;
      insn = extract_unsigned_integer (buf, 2, byte_order_for_code);

      if ((insn & 0xff80) == 0x4700)  /* bx <Rm> */
	found_return = 1;
      else if (insn == 0x46f7)  /* mov pc, lr */
	found_return = 1;
      else if (insn == 0x46bd)  /* mov sp, r7 */
	found_stack_adjust = 1;
      else if ((insn & 0xff00) == 0xb000)  /* add sp, imm or sub sp, imm  */
	found_stack_adjust = 1;
      else if ((insn & 0xfe00) == 0xbc00)  /* pop <registers> */
	{
	  found_stack_adjust = 1;
	  if (insn & 0x0100)  /* <registers> include PC.  */
	    found_return = 1;
	}
      else if (thumb_insn_size (insn) == 4)  /* 32-bit Thumb-2 instruction */
	{
	  if (target_read_memory (scan_pc, buf, 2))
	    break;

	  scan_pc += 2;
	  insn2 = extract_unsigned_integer (buf, 2, byte_order_for_code);

	  if (insn == 0xe8bd)  /* ldm.w sp!, <registers> */
	    {
	      found_stack_adjust = 1;
	      if (insn2 & 0x8000)  /* <registers> include PC.  */
		found_return = 1;
	    }
	  else if (insn == 0xf85d  /* ldr.w <Rt>, [sp], #4 */
		   && (insn2 & 0x0fff) == 0x0b04)
	    {
	      found_stack_adjust = 1;
	      if ((insn2 & 0xf000) == 0xf000) /* <Rt> is PC.  */
		found_return = 1;
	    }
	  else if ((insn & 0xffbf) == 0xecbd  /* vldm sp!, <list> */
		   && (insn2 & 0x0e00) == 0x0a00)
	    found_stack_adjust = 1;
	  else
	    break;
	}
      else
	break;
    }

  if (!found_return)
    return 0;

  /* Since any instruction in the epilogue sequence, with the possible
     exception of return itself, updates the stack pointer, we need to
     scan backwards for at most one instruction.  Try either a 16-bit or
     a 32-bit instruction.  This is just a heuristic, so we do not worry
     too much about false positives.  */

  if (!found_stack_adjust)
    {
      if (pc - 4 < func_start)
	return 0;
      if (target_read_memory (pc - 4, buf, 4))
	return 0;

      insn = extract_unsigned_integer (buf, 2, byte_order_for_code);
      insn2 = extract_unsigned_integer (buf + 2, 2, byte_order_for_code);

      if (insn2 == 0x46bd)  /* mov sp, r7 */
	found_stack_adjust = 1;
      else if ((insn2 & 0xff00) == 0xb000)  /* add sp, imm or sub sp, imm  */
	found_stack_adjust = 1;
      else if ((insn2 & 0xff00) == 0xbc00)  /* pop <registers> without PC */
	found_stack_adjust = 1;
      else if (insn == 0xe8bd)  /* ldm.w sp!, <registers> */
	found_stack_adjust = 1;
      else if (insn == 0xf85d  /* ldr.w <Rt>, [sp], #4 */
	       && (insn2 & 0x0fff) == 0x0b04)
	found_stack_adjust = 1;
      else if ((insn & 0xffbf) == 0xecbd  /* vldm sp!, <list> */
	       && (insn2 & 0x0e00) == 0x0a00)
	found_stack_adjust = 1;
    }

  return found_stack_adjust;
}

/* Return true if we are in the function's epilogue, i.e. after the
   instruction that destroyed the function's stack frame.  */

static int
arm_in_function_epilogue_p (struct gdbarch *gdbarch, CORE_ADDR pc)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned int insn;
  int found_return, found_stack_adjust;
  CORE_ADDR func_start, func_end;

  if (arm_pc_is_thumb (gdbarch, pc))
    return thumb_in_function_epilogue_p (gdbarch, pc);

  if (!find_pc_partial_function (pc, NULL, &func_start, &func_end))
    return 0;

  /* We are in the epilogue if the previous instruction was a stack
     adjustment and the next instruction is a possible return (bx, mov
     pc, or pop).  We could have to scan backwards to find the stack
     adjustment, or forwards to find the return, but this is a decent
     approximation.  First scan forwards.  */

  found_return = 0;
  insn = read_memory_unsigned_integer (pc, 4, byte_order_for_code);
  if (bits (insn, 28, 31) != INST_NV)
    {
      if ((insn & 0x0ffffff0) == 0x012fff10)
	/* BX.  */
	found_return = 1;
      else if ((insn & 0x0ffffff0) == 0x01a0f000)
	/* MOV PC.  */
	found_return = 1;
      else if ((insn & 0x0fff0000) == 0x08bd0000
	  && (insn & 0x0000c000) != 0)
	/* POP (LDMIA), including PC or LR.  */
	found_return = 1;
    }

  if (!found_return)
    return 0;

  /* Scan backwards.  This is just a heuristic, so do not worry about
     false positives from mode changes.  */

  if (pc < func_start + 4)
    return 0;

  found_stack_adjust = 0;
  insn = read_memory_unsigned_integer (pc - 4, 4, byte_order_for_code);
  if (bits (insn, 28, 31) != INST_NV)
    {
      if ((insn & 0x0df0f000) == 0x0080d000)
	/* ADD SP (register or immediate).  */
	found_stack_adjust = 1;
      else if ((insn & 0x0df0f000) == 0x0040d000)
	/* SUB SP (register or immediate).  */
	found_stack_adjust = 1;
      else if ((insn & 0x0ffffff0) == 0x01a0d000)
	/* MOV SP.  */
	found_stack_adjust = 1;
      else if ((insn & 0x0fff0000) == 0x08bd0000)
	/* POP (LDMIA).  */
	found_stack_adjust = 1;
    }

  if (found_stack_adjust)
    return 1;

  return 0;
}


/* When arguments must be pushed onto the stack, they go on in reverse
   order.  The code below implements a FILO (stack) to do this.  */

struct stack_item
{
  int len;
  struct stack_item *prev;
  void *data;
};

static struct stack_item *
push_stack_item (struct stack_item *prev, const void *contents, int len)
{
  struct stack_item *si;
  si = xmalloc (sizeof (struct stack_item));
  si->data = xmalloc (len);
  si->len = len;
  si->prev = prev;
  memcpy (si->data, contents, len);
  return si;
}

static struct stack_item *
pop_stack_item (struct stack_item *si)
{
  struct stack_item *dead = si;
  si = si->prev;
  xfree (dead->data);
  xfree (dead);
  return si;
}


/* Return the alignment (in bytes) of the given type.  */

static int
arm_type_align (struct type *t)
{
  int n;
  int align;
  int falign;

  t = check_typedef (t);
  switch (TYPE_CODE (t))
    {
    default:
      /* Should never happen.  */
      internal_error (__FILE__, __LINE__, _("unknown type alignment"));
      return 4;

    case TYPE_CODE_PTR:
    case TYPE_CODE_ENUM:
    case TYPE_CODE_INT:
    case TYPE_CODE_FLT:
    case TYPE_CODE_SET:
    case TYPE_CODE_RANGE:
    case TYPE_CODE_BITSTRING:
    case TYPE_CODE_REF:
    case TYPE_CODE_CHAR:
    case TYPE_CODE_BOOL:
      return TYPE_LENGTH (t);

    case TYPE_CODE_ARRAY:
    case TYPE_CODE_COMPLEX:
      /* TODO: What about vector types?  */
      return arm_type_align (TYPE_TARGET_TYPE (t));

    case TYPE_CODE_STRUCT:
    case TYPE_CODE_UNION:
      align = 1;
      for (n = 0; n < TYPE_NFIELDS (t); n++)
	{
	  falign = arm_type_align (TYPE_FIELD_TYPE (t, n));
	  if (falign > align)
	    align = falign;
	}
      return align;
    }
}

/* Possible base types for a candidate for passing and returning in
   VFP registers.  */

enum arm_vfp_cprc_base_type
{
  VFP_CPRC_UNKNOWN,
  VFP_CPRC_SINGLE,
  VFP_CPRC_DOUBLE,
  VFP_CPRC_VEC64,
  VFP_CPRC_VEC128
};

/* The length of one element of base type B.  */

static unsigned
arm_vfp_cprc_unit_length (enum arm_vfp_cprc_base_type b)
{
  switch (b)
    {
    case VFP_CPRC_SINGLE:
      return 4;
    case VFP_CPRC_DOUBLE:
      return 8;
    case VFP_CPRC_VEC64:
      return 8;
    case VFP_CPRC_VEC128:
      return 16;
    default:
      internal_error (__FILE__, __LINE__, _("Invalid VFP CPRC type: %d."),
		      (int) b);
    }
}

/* The character ('s', 'd' or 'q') for the type of VFP register used
   for passing base type B.  */

static int
arm_vfp_cprc_reg_char (enum arm_vfp_cprc_base_type b)
{
  switch (b)
    {
    case VFP_CPRC_SINGLE:
      return 's';
    case VFP_CPRC_DOUBLE:
      return 'd';
    case VFP_CPRC_VEC64:
      return 'd';
    case VFP_CPRC_VEC128:
      return 'q';
    default:
      internal_error (__FILE__, __LINE__, _("Invalid VFP CPRC type: %d."),
		      (int) b);
    }
}

/* Determine whether T may be part of a candidate for passing and
   returning in VFP registers, ignoring the limit on the total number
   of components.  If *BASE_TYPE is VFP_CPRC_UNKNOWN, set it to the
   classification of the first valid component found; if it is not
   VFP_CPRC_UNKNOWN, all components must have the same classification
   as *BASE_TYPE.  If it is found that T contains a type not permitted
   for passing and returning in VFP registers, a type differently
   classified from *BASE_TYPE, or two types differently classified
   from each other, return -1, otherwise return the total number of
   base-type elements found (possibly 0 in an empty structure or
   array).  Vectors and complex types are not currently supported,
   matching the generic AAPCS support.  */

static int
arm_vfp_cprc_sub_candidate (struct type *t,
			    enum arm_vfp_cprc_base_type *base_type)
{
  t = check_typedef (t);
  switch (TYPE_CODE (t))
    {
    case TYPE_CODE_FLT:
      switch (TYPE_LENGTH (t))
	{
	case 4:
	  if (*base_type == VFP_CPRC_UNKNOWN)
	    *base_type = VFP_CPRC_SINGLE;
	  else if (*base_type != VFP_CPRC_SINGLE)
	    return -1;
	  return 1;

	case 8:
	  if (*base_type == VFP_CPRC_UNKNOWN)
	    *base_type = VFP_CPRC_DOUBLE;
	  else if (*base_type != VFP_CPRC_DOUBLE)
	    return -1;
	  return 1;

	default:
	  return -1;
	}
      break;

    case TYPE_CODE_ARRAY:
      {
	int count;
	unsigned unitlen;
	count = arm_vfp_cprc_sub_candidate (TYPE_TARGET_TYPE (t), base_type);
	if (count == -1)
	  return -1;
	if (TYPE_LENGTH (t) == 0)
	  {
	    gdb_assert (count == 0);
	    return 0;
	  }
	else if (count == 0)
	  return -1;
	unitlen = arm_vfp_cprc_unit_length (*base_type);
	gdb_assert ((TYPE_LENGTH (t) % unitlen) == 0);
	return TYPE_LENGTH (t) / unitlen;
      }
      break;

    case TYPE_CODE_STRUCT:
      {
	int count = 0;
	unsigned unitlen;
	int i;
	for (i = 0; i < TYPE_NFIELDS (t); i++)
	  {
	    int sub_count = arm_vfp_cprc_sub_candidate (TYPE_FIELD_TYPE (t, i),
							base_type);
	    if (sub_count == -1)
	      return -1;
	    count += sub_count;
	  }
	if (TYPE_LENGTH (t) == 0)
	  {
	    gdb_assert (count == 0);
	    return 0;
	  }
	else if (count == 0)
	  return -1;
	unitlen = arm_vfp_cprc_unit_length (*base_type);
	if (TYPE_LENGTH (t) != unitlen * count)
	  return -1;
	return count;
      }

    case TYPE_CODE_UNION:
      {
	int count = 0;
	unsigned unitlen;
	int i;
	for (i = 0; i < TYPE_NFIELDS (t); i++)
	  {
	    int sub_count = arm_vfp_cprc_sub_candidate (TYPE_FIELD_TYPE (t, i),
							base_type);
	    if (sub_count == -1)
	      return -1;
	    count = (count > sub_count ? count : sub_count);
	  }
	if (TYPE_LENGTH (t) == 0)
	  {
	    gdb_assert (count == 0);
	    return 0;
	  }
	else if (count == 0)
	  return -1;
	unitlen = arm_vfp_cprc_unit_length (*base_type);
	if (TYPE_LENGTH (t) != unitlen * count)
	  return -1;
	return count;
      }

    default:
      break;
    }

  return -1;
}

/* Determine whether T is a VFP co-processor register candidate (CPRC)
   if passed to or returned from a non-variadic function with the VFP
   ABI in effect.  Return 1 if it is, 0 otherwise.  If it is, set
   *BASE_TYPE to the base type for T and *COUNT to the number of
   elements of that base type before returning.  */

static int
arm_vfp_call_candidate (struct type *t, enum arm_vfp_cprc_base_type *base_type,
			int *count)
{
  enum arm_vfp_cprc_base_type b = VFP_CPRC_UNKNOWN;
  int c = arm_vfp_cprc_sub_candidate (t, &b);
  if (c <= 0 || c > 4)
    return 0;
  *base_type = b;
  *count = c;
  return 1;
}

/* Return 1 if the VFP ABI should be used for passing arguments to and
   returning values from a function of type FUNC_TYPE, 0
   otherwise.  */

static int
arm_vfp_abi_for_function (struct gdbarch *gdbarch, struct type *func_type)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  /* Variadic functions always use the base ABI.  Assume that functions
     without debug info are not variadic.  */
  if (func_type && TYPE_VARARGS (check_typedef (func_type)))
    return 0;
  /* The VFP ABI is only supported as a variant of AAPCS.  */
  if (tdep->arm_abi != ARM_ABI_AAPCS)
    return 0;
  return gdbarch_tdep (gdbarch)->fp_model == ARM_FLOAT_VFP;
}

/* We currently only support passing parameters in integer registers, which
   conforms with GCC's default model, and VFP argument passing following
   the VFP variant of AAPCS.  Several other variants exist and
   we should probably support some of them based on the selected ABI.  */

static CORE_ADDR
arm_push_dummy_call (struct gdbarch *gdbarch, struct value *function,
		     struct regcache *regcache, CORE_ADDR bp_addr, int nargs,
		     struct value **args, CORE_ADDR sp, int struct_return,
		     CORE_ADDR struct_addr)
{
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  int argnum;
  int argreg;
  int nstack;
  struct stack_item *si = NULL;
  int use_vfp_abi;
  struct type *ftype;
  unsigned vfp_regs_free = (1 << 16) - 1;

  /* Determine the type of this function and whether the VFP ABI
     applies.  */
  ftype = check_typedef (value_type (function));
  if (TYPE_CODE (ftype) == TYPE_CODE_PTR)
    ftype = check_typedef (TYPE_TARGET_TYPE (ftype));
  use_vfp_abi = arm_vfp_abi_for_function (gdbarch, ftype);

  /* Set the return address.  For the ARM, the return breakpoint is
     always at BP_ADDR.  */
  if (arm_pc_is_thumb (gdbarch, bp_addr))
    bp_addr |= 1;
  regcache_cooked_write_unsigned (regcache, ARM_LR_REGNUM, bp_addr);

  /* Walk through the list of args and determine how large a temporary
     stack is required.  Need to take care here as structs may be
     passed on the stack, and we have to push them.  */
  nstack = 0;

  argreg = ARM_A1_REGNUM;
  nstack = 0;

  /* The struct_return pointer occupies the first parameter
     passing register.  */
  if (struct_return)
    {
      if (arm_debug)
	fprintf_unfiltered (gdb_stdlog, "struct return in %s = %s\n",
			    gdbarch_register_name (gdbarch, argreg),
			    paddress (gdbarch, struct_addr));
      regcache_cooked_write_unsigned (regcache, argreg, struct_addr);
      argreg++;
    }

  for (argnum = 0; argnum < nargs; argnum++)
    {
      int len;
      struct type *arg_type;
      struct type *target_type;
      enum type_code typecode;
      const bfd_byte *val;
      int align;
      enum arm_vfp_cprc_base_type vfp_base_type;
      int vfp_base_count;
      int may_use_core_reg = 1;

      arg_type = check_typedef (value_type (args[argnum]));
      len = TYPE_LENGTH (arg_type);
      target_type = TYPE_TARGET_TYPE (arg_type);
      typecode = TYPE_CODE (arg_type);
      val = value_contents (args[argnum]);

      align = arm_type_align (arg_type);
      /* Round alignment up to a whole number of words.  */
      align = (align + INT_REGISTER_SIZE - 1) & ~(INT_REGISTER_SIZE - 1);
      /* Different ABIs have different maximum alignments.  */
      if (gdbarch_tdep (gdbarch)->arm_abi == ARM_ABI_APCS)
	{
	  /* The APCS ABI only requires word alignment.  */
	  align = INT_REGISTER_SIZE;
	}
      else
	{
	  /* The AAPCS requires at most doubleword alignment.  */
	  if (align > INT_REGISTER_SIZE * 2)
	    align = INT_REGISTER_SIZE * 2;
	}

      if (use_vfp_abi
	  && arm_vfp_call_candidate (arg_type, &vfp_base_type,
				     &vfp_base_count))
	{
	  int regno;
	  int unit_length;
	  int shift;
	  unsigned mask;

	  /* Because this is a CPRC it cannot go in a core register or
	     cause a core register to be skipped for alignment.
	     Either it goes in VFP registers and the rest of this loop
	     iteration is skipped for this argument, or it goes on the
	     stack (and the stack alignment code is correct for this
	     case).  */
	  may_use_core_reg = 0;

	  unit_length = arm_vfp_cprc_unit_length (vfp_base_type);
	  shift = unit_length / 4;
	  mask = (1 << (shift * vfp_base_count)) - 1;
	  for (regno = 0; regno < 16; regno += shift)
	    if (((vfp_regs_free >> regno) & mask) == mask)
	      break;

	  if (regno < 16)
	    {
	      int reg_char;
	      int reg_scaled;
	      int i;

	      vfp_regs_free &= ~(mask << regno);
	      reg_scaled = regno / shift;
	      reg_char = arm_vfp_cprc_reg_char (vfp_base_type);
	      for (i = 0; i < vfp_base_count; i++)
		{
		  char name_buf[4];
		  int regnum;
		  if (reg_char == 'q')
		    arm_neon_quad_write (gdbarch, regcache, reg_scaled + i,
					 val + i * unit_length);
		  else
		    {
		      sprintf (name_buf, "%c%d", reg_char, reg_scaled + i);
		      regnum = user_reg_map_name_to_regnum (gdbarch, name_buf,
							    strlen (name_buf));
		      regcache_cooked_write (regcache, regnum,
					     val + i * unit_length);
		    }
		}
	      continue;
	    }
	  else
	    {
	      /* This CPRC could not go in VFP registers, so all VFP
		 registers are now marked as used.  */
	      vfp_regs_free = 0;
	    }
	}

      /* Push stack padding for dowubleword alignment.  */
      if (nstack & (align - 1))
	{
	  si = push_stack_item (si, val, INT_REGISTER_SIZE);
	  nstack += INT_REGISTER_SIZE;
	}
      
      /* Doubleword aligned quantities must go in even register pairs.  */
      if (may_use_core_reg
	  && argreg <= ARM_LAST_ARG_REGNUM
	  && align > INT_REGISTER_SIZE
	  && argreg & 1)
	argreg++;

      /* If the argument is a pointer to a function, and it is a
	 Thumb function, create a LOCAL copy of the value and set
	 the THUMB bit in it.  */
      if (TYPE_CODE_PTR == typecode
	  && target_type != NULL
	  && TYPE_CODE_FUNC == TYPE_CODE (check_typedef (target_type)))
	{
	  CORE_ADDR regval = extract_unsigned_integer (val, len, byte_order);
	  if (arm_pc_is_thumb (gdbarch, regval))
	    {
	      bfd_byte *copy = alloca (len);
	      store_unsigned_integer (copy, len, byte_order,
				      MAKE_THUMB_ADDR (regval));
	      val = copy;
	    }
	}

      /* Copy the argument to general registers or the stack in
	 register-sized pieces.  Large arguments are split between
	 registers and stack.  */
      while (len > 0)
	{
	  int partial_len = len < INT_REGISTER_SIZE ? len : INT_REGISTER_SIZE;

	  if (may_use_core_reg && argreg <= ARM_LAST_ARG_REGNUM)
	    {
	      /* The argument is being passed in a general purpose
		 register.  */
	      CORE_ADDR regval
		= extract_unsigned_integer (val, partial_len, byte_order);
	      if (byte_order == BFD_ENDIAN_BIG)
		regval <<= (INT_REGISTER_SIZE - partial_len) * 8;
	      if (arm_debug)
		fprintf_unfiltered (gdb_stdlog, "arg %d in %s = 0x%s\n",
				    argnum,
				    gdbarch_register_name
				      (gdbarch, argreg),
				    phex (regval, INT_REGISTER_SIZE));
	      regcache_cooked_write_unsigned (regcache, argreg, regval);
	      argreg++;
	    }
	  else
	    {
	      /* Push the arguments onto the stack.  */
	      if (arm_debug)
		fprintf_unfiltered (gdb_stdlog, "arg %d @ sp + %d\n",
				    argnum, nstack);
	      si = push_stack_item (si, val, INT_REGISTER_SIZE);
	      nstack += INT_REGISTER_SIZE;
	    }
	      
	  len -= partial_len;
	  val += partial_len;
	}
    }
  /* If we have an odd number of words to push, then decrement the stack
     by one word now, so first stack argument will be dword aligned.  */
  if (nstack & 4)
    sp -= 4;

  while (si)
    {
      sp -= si->len;
      write_memory (sp, si->data, si->len);
      si = pop_stack_item (si);
    }

  /* Finally, update teh SP register.  */
  regcache_cooked_write_unsigned (regcache, ARM_SP_REGNUM, sp);

  return sp;
}


/* Always align the frame to an 8-byte boundary.  This is required on
   some platforms and harmless on the rest.  */

static CORE_ADDR
arm_frame_align (struct gdbarch *gdbarch, CORE_ADDR sp)
{
  /* Align the stack to eight bytes.  */
  return sp & ~ (CORE_ADDR) 7;
}

static void
print_fpu_flags (int flags)
{
  if (flags & (1 << 0))
    fputs ("IVO ", stdout);
  if (flags & (1 << 1))
    fputs ("DVZ ", stdout);
  if (flags & (1 << 2))
    fputs ("OFL ", stdout);
  if (flags & (1 << 3))
    fputs ("UFL ", stdout);
  if (flags & (1 << 4))
    fputs ("INX ", stdout);
  putchar ('\n');
}

/* Print interesting information about the floating point processor
   (if present) or emulator.  */
static void
arm_print_float_info (struct gdbarch *gdbarch, struct ui_file *file,
		      struct frame_info *frame, const char *args)
{
  unsigned long status = get_frame_register_unsigned (frame, ARM_FPS_REGNUM);
  int type;

  type = (status >> 24) & 127;
  if (status & (1 << 31))
    printf (_("Hardware FPU type %d\n"), type);
  else
    printf (_("Software FPU type %d\n"), type);
  /* i18n: [floating point unit] mask */
  fputs (_("mask: "), stdout);
  print_fpu_flags (status >> 16);
  /* i18n: [floating point unit] flags */
  fputs (_("flags: "), stdout);
  print_fpu_flags (status);
}

/* Construct the ARM extended floating point type.  */
static struct type *
arm_ext_type (struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  if (!tdep->arm_ext_type)
    tdep->arm_ext_type
      = arch_float_type (gdbarch, -1, "builtin_type_arm_ext",
			 floatformats_arm_ext);

  return tdep->arm_ext_type;
}

static struct type *
arm_neon_double_type (struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  if (tdep->neon_double_type == NULL)
    {
      struct type *t, *elem;

      t = arch_composite_type (gdbarch, "__gdb_builtin_type_neon_d",
			       TYPE_CODE_UNION);
      elem = builtin_type (gdbarch)->builtin_uint8;
      append_composite_type_field (t, "u8", init_vector_type (elem, 8));
      elem = builtin_type (gdbarch)->builtin_uint16;
      append_composite_type_field (t, "u16", init_vector_type (elem, 4));
      elem = builtin_type (gdbarch)->builtin_uint32;
      append_composite_type_field (t, "u32", init_vector_type (elem, 2));
      elem = builtin_type (gdbarch)->builtin_uint64;
      append_composite_type_field (t, "u64", elem);
      elem = builtin_type (gdbarch)->builtin_float;
      append_composite_type_field (t, "f32", init_vector_type (elem, 2));
      elem = builtin_type (gdbarch)->builtin_double;
      append_composite_type_field (t, "f64", elem);

      TYPE_VECTOR (t) = 1;
      TYPE_NAME (t) = "neon_d";
      tdep->neon_double_type = t;
    }

  return tdep->neon_double_type;
}

/* FIXME: The vector types are not correctly ordered on big-endian
   targets.  Just as s0 is the low bits of d0, d0[0] is also the low
   bits of d0 - regardless of what unit size is being held in d0.  So
   the offset of the first uint8 in d0 is 7, but the offset of the
   first float is 4.  This code works as-is for little-endian
   targets.  */

static struct type *
arm_neon_quad_type (struct gdbarch *gdbarch)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  if (tdep->neon_quad_type == NULL)
    {
      struct type *t, *elem;

      t = arch_composite_type (gdbarch, "__gdb_builtin_type_neon_q",
			       TYPE_CODE_UNION);
      elem = builtin_type (gdbarch)->builtin_uint8;
      append_composite_type_field (t, "u8", init_vector_type (elem, 16));
      elem = builtin_type (gdbarch)->builtin_uint16;
      append_composite_type_field (t, "u16", init_vector_type (elem, 8));
      elem = builtin_type (gdbarch)->builtin_uint32;
      append_composite_type_field (t, "u32", init_vector_type (elem, 4));
      elem = builtin_type (gdbarch)->builtin_uint64;
      append_composite_type_field (t, "u64", init_vector_type (elem, 2));
      elem = builtin_type (gdbarch)->builtin_float;
      append_composite_type_field (t, "f32", init_vector_type (elem, 4));
      elem = builtin_type (gdbarch)->builtin_double;
      append_composite_type_field (t, "f64", init_vector_type (elem, 2));

      TYPE_VECTOR (t) = 1;
      TYPE_NAME (t) = "neon_q";
      tdep->neon_quad_type = t;
    }

  return tdep->neon_quad_type;
}

/* Return the GDB type object for the "standard" data type of data in
   register N.  */

static struct type *
arm_register_type (struct gdbarch *gdbarch, int regnum)
{
  int num_regs = gdbarch_num_regs (gdbarch);

  if (gdbarch_tdep (gdbarch)->have_vfp_pseudos
      && regnum >= num_regs && regnum < num_regs + 32)
    return builtin_type (gdbarch)->builtin_float;

  if (gdbarch_tdep (gdbarch)->have_neon_pseudos
      && regnum >= num_regs + 32 && regnum < num_regs + 32 + 16)
    return arm_neon_quad_type (gdbarch);

  /* If the target description has register information, we are only
     in this function so that we can override the types of
     double-precision registers for NEON.  */
  if (tdesc_has_registers (gdbarch_target_desc (gdbarch)))
    {
      struct type *t = tdesc_register_type (gdbarch, regnum);

      if (regnum >= ARM_D0_REGNUM && regnum < ARM_D0_REGNUM + 32
	  && TYPE_CODE (t) == TYPE_CODE_FLT
	  && gdbarch_tdep (gdbarch)->have_neon)
	return arm_neon_double_type (gdbarch);
      else
	return t;
    }

  if (regnum >= ARM_F0_REGNUM && regnum < ARM_F0_REGNUM + NUM_FREGS)
    {
      if (!gdbarch_tdep (gdbarch)->have_fpa_registers)
	return builtin_type (gdbarch)->builtin_void;

      return arm_ext_type (gdbarch);
    }
  else if (regnum == ARM_SP_REGNUM)
    return builtin_type (gdbarch)->builtin_data_ptr;
  else if (regnum == ARM_PC_REGNUM)
    return builtin_type (gdbarch)->builtin_func_ptr;
  else if (regnum >= ARRAY_SIZE (arm_register_names))
    /* These registers are only supported on targets which supply
       an XML description.  */
    return builtin_type (gdbarch)->builtin_int0;
  else
    return builtin_type (gdbarch)->builtin_uint32;
}

/* Map a DWARF register REGNUM onto the appropriate GDB register
   number.  */

static int
arm_dwarf_reg_to_regnum (struct gdbarch *gdbarch, int reg)
{
  /* Core integer regs.  */
  if (reg >= 0 && reg <= 15)
    return reg;

  /* Legacy FPA encoding.  These were once used in a way which
     overlapped with VFP register numbering, so their use is
     discouraged, but GDB doesn't support the ARM toolchain
     which used them for VFP.  */
  if (reg >= 16 && reg <= 23)
    return ARM_F0_REGNUM + reg - 16;

  /* New assignments for the FPA registers.  */
  if (reg >= 96 && reg <= 103)
    return ARM_F0_REGNUM + reg - 96;

  /* WMMX register assignments.  */
  if (reg >= 104 && reg <= 111)
    return ARM_WCGR0_REGNUM + reg - 104;

  if (reg >= 112 && reg <= 127)
    return ARM_WR0_REGNUM + reg - 112;

  if (reg >= 192 && reg <= 199)
    return ARM_WC0_REGNUM + reg - 192;

  /* VFP v2 registers.  A double precision value is actually
     in d1 rather than s2, but the ABI only defines numbering
     for the single precision registers.  This will "just work"
     in GDB for little endian targets (we'll read eight bytes,
     starting in s0 and then progressing to s1), but will be
     reversed on big endian targets with VFP.  This won't
     be a problem for the new Neon quad registers; you're supposed
     to use DW_OP_piece for those.  */
  if (reg >= 64 && reg <= 95)
    {
      char name_buf[4];

      sprintf (name_buf, "s%d", reg - 64);
      return user_reg_map_name_to_regnum (gdbarch, name_buf,
					  strlen (name_buf));
    }

  /* VFP v3 / Neon registers.  This range is also used for VFP v2
     registers, except that it now describes d0 instead of s0.  */
  if (reg >= 256 && reg <= 287)
    {
      char name_buf[4];

      sprintf (name_buf, "d%d", reg - 256);
      return user_reg_map_name_to_regnum (gdbarch, name_buf,
					  strlen (name_buf));
    }

  return -1;
}

/* Map GDB internal REGNUM onto the Arm simulator register numbers.  */
static int
arm_register_sim_regno (struct gdbarch *gdbarch, int regnum)
{
  int reg = regnum;
  gdb_assert (reg >= 0 && reg < gdbarch_num_regs (gdbarch));

  if (regnum >= ARM_WR0_REGNUM && regnum <= ARM_WR15_REGNUM)
    return regnum - ARM_WR0_REGNUM + SIM_ARM_IWMMXT_COP0R0_REGNUM;

  if (regnum >= ARM_WC0_REGNUM && regnum <= ARM_WC7_REGNUM)
    return regnum - ARM_WC0_REGNUM + SIM_ARM_IWMMXT_COP1R0_REGNUM;

  if (regnum >= ARM_WCGR0_REGNUM && regnum <= ARM_WCGR7_REGNUM)
    return regnum - ARM_WCGR0_REGNUM + SIM_ARM_IWMMXT_COP1R8_REGNUM;

  if (reg < NUM_GREGS)
    return SIM_ARM_R0_REGNUM + reg;
  reg -= NUM_GREGS;

  if (reg < NUM_FREGS)
    return SIM_ARM_FP0_REGNUM + reg;
  reg -= NUM_FREGS;

  if (reg < NUM_SREGS)
    return SIM_ARM_FPS_REGNUM + reg;
  reg -= NUM_SREGS;

  internal_error (__FILE__, __LINE__, _("Bad REGNUM %d"), regnum);
}

/* NOTE: cagney/2001-08-20: Both convert_from_extended() and
   convert_to_extended() use floatformat_arm_ext_littlebyte_bigword.
   It is thought that this is is the floating-point register format on
   little-endian systems.  */

static void
convert_from_extended (const struct floatformat *fmt, const void *ptr,
		       void *dbl, int endianess)
{
  DOUBLEST d;

  if (endianess == BFD_ENDIAN_BIG)
    floatformat_to_doublest (&floatformat_arm_ext_big, ptr, &d);
  else
    floatformat_to_doublest (&floatformat_arm_ext_littlebyte_bigword,
			     ptr, &d);
  floatformat_from_doublest (fmt, &d, dbl);
}

static void
convert_to_extended (const struct floatformat *fmt, void *dbl, const void *ptr,
		     int endianess)
{
  DOUBLEST d;

  floatformat_to_doublest (fmt, ptr, &d);
  if (endianess == BFD_ENDIAN_BIG)
    floatformat_from_doublest (&floatformat_arm_ext_big, &d, dbl);
  else
    floatformat_from_doublest (&floatformat_arm_ext_littlebyte_bigword,
			       &d, dbl);
}

static int
condition_true (unsigned long cond, unsigned long status_reg)
{
  if (cond == INST_AL || cond == INST_NV)
    return 1;

  switch (cond)
    {
    case INST_EQ:
      return ((status_reg & FLAG_Z) != 0);
    case INST_NE:
      return ((status_reg & FLAG_Z) == 0);
    case INST_CS:
      return ((status_reg & FLAG_C) != 0);
    case INST_CC:
      return ((status_reg & FLAG_C) == 0);
    case INST_MI:
      return ((status_reg & FLAG_N) != 0);
    case INST_PL:
      return ((status_reg & FLAG_N) == 0);
    case INST_VS:
      return ((status_reg & FLAG_V) != 0);
    case INST_VC:
      return ((status_reg & FLAG_V) == 0);
    case INST_HI:
      return ((status_reg & (FLAG_C | FLAG_Z)) == FLAG_C);
    case INST_LS:
      return ((status_reg & (FLAG_C | FLAG_Z)) != FLAG_C);
    case INST_GE:
      return (((status_reg & FLAG_N) == 0) == ((status_reg & FLAG_V) == 0));
    case INST_LT:
      return (((status_reg & FLAG_N) == 0) != ((status_reg & FLAG_V) == 0));
    case INST_GT:
      return (((status_reg & FLAG_Z) == 0)
	      && (((status_reg & FLAG_N) == 0)
		  == ((status_reg & FLAG_V) == 0)));
    case INST_LE:
      return (((status_reg & FLAG_Z) != 0)
	      || (((status_reg & FLAG_N) == 0)
		  != ((status_reg & FLAG_V) == 0)));
    }
  return 1;
}

static unsigned long
shifted_reg_val (struct frame_info *frame, unsigned long inst, int carry,
		 unsigned long pc_val, unsigned long status_reg)
{
  unsigned long res, shift;
  int rm = bits (inst, 0, 3);
  unsigned long shifttype = bits (inst, 5, 6);

  if (bit (inst, 4))
    {
      int rs = bits (inst, 8, 11);
      shift = (rs == 15 ? pc_val + 8
			: get_frame_register_unsigned (frame, rs)) & 0xFF;
    }
  else
    shift = bits (inst, 7, 11);

  res = (rm == ARM_PC_REGNUM
	 ? (pc_val + (bit (inst, 4) ? 12 : 8))
	 : get_frame_register_unsigned (frame, rm));

  switch (shifttype)
    {
    case 0:			/* LSL */
      res = shift >= 32 ? 0 : res << shift;
      break;

    case 1:			/* LSR */
      res = shift >= 32 ? 0 : res >> shift;
      break;

    case 2:			/* ASR */
      if (shift >= 32)
	shift = 31;
      res = ((res & 0x80000000L)
	     ? ~((~res) >> shift) : res >> shift);
      break;

    case 3:			/* ROR/RRX */
      shift &= 31;
      if (shift == 0)
	res = (res >> 1) | (carry ? 0x80000000L : 0);
      else
	res = (res >> shift) | (res << (32 - shift));
      break;
    }

  return res & 0xffffffff;
}

/* Return number of 1-bits in VAL.  */

static int
bitcount (unsigned long val)
{
  int nbits;
  for (nbits = 0; val != 0; nbits++)
    val &= val - 1;		/* Delete rightmost 1-bit in val.  */
  return nbits;
}

/* Return the size in bytes of the complete Thumb instruction whose
   first halfword is INST1.  */

static int
thumb_insn_size (unsigned short inst1)
{
  if ((inst1 & 0xe000) == 0xe000 && (inst1 & 0x1800) != 0)
    return 4;
  else
    return 2;
}

static int
thumb_advance_itstate (unsigned int itstate)
{
  /* Preserve IT[7:5], the first three bits of the condition.  Shift
     the upcoming condition flags left by one bit.  */
  itstate = (itstate & 0xe0) | ((itstate << 1) & 0x1f);

  /* If we have finished the IT block, clear the state.  */
  if ((itstate & 0x0f) == 0)
    itstate = 0;

  return itstate;
}

/* Find the next PC after the current instruction executes.  In some
   cases we can not statically determine the answer (see the IT state
   handling in this function); in that case, a breakpoint may be
   inserted in addition to the returned PC, which will be used to set
   another breakpoint by our caller.  */

static CORE_ADDR
thumb_get_next_pc_raw (struct frame_info *frame, CORE_ADDR pc)
{
  struct gdbarch *gdbarch = get_frame_arch (frame);
  struct address_space *aspace = get_frame_address_space (frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned long pc_val = ((unsigned long) pc) + 4;	/* PC after prefetch */
  unsigned short inst1;
  CORE_ADDR nextpc = pc + 2;		/* Default is next instruction.  */
  unsigned long offset;
  ULONGEST status, itstate;

  nextpc = MAKE_THUMB_ADDR (nextpc);
  pc_val = MAKE_THUMB_ADDR (pc_val);

  inst1 = read_memory_unsigned_integer (pc, 2, byte_order_for_code);

  /* Thumb-2 conditional execution support.  There are eight bits in
     the CPSR which describe conditional execution state.  Once
     reconstructed (they're in a funny order), the low five bits
     describe the low bit of the condition for each instruction and
     how many instructions remain.  The high three bits describe the
     base condition.  One of the low four bits will be set if an IT
     block is active.  These bits read as zero on earlier
     processors.  */
  status = get_frame_register_unsigned (frame, ARM_PS_REGNUM);
  itstate = ((status >> 8) & 0xfc) | ((status >> 25) & 0x3);

  /* If-Then handling.  On GNU/Linux, where this routine is used, we
     use an undefined instruction as a breakpoint.  Unlike BKPT, IT
     can disable execution of the undefined instruction.  So we might
     miss the breakpoint if we set it on a skipped conditional
     instruction.  Because conditional instructions can change the
     flags, affecting the execution of further instructions, we may
     need to set two breakpoints.  */

  if (gdbarch_tdep (gdbarch)->thumb2_breakpoint != NULL)
    {
      if ((inst1 & 0xff00) == 0xbf00 && (inst1 & 0x000f) != 0)
	{
	  /* An IT instruction.  Because this instruction does not
	     modify the flags, we can accurately predict the next
	     executed instruction.  */
	  itstate = inst1 & 0x00ff;
	  pc += thumb_insn_size (inst1);

	  while (itstate != 0 && ! condition_true (itstate >> 4, status))
	    {
	      inst1 = read_memory_unsigned_integer (pc, 2,
						    byte_order_for_code);
	      pc += thumb_insn_size (inst1);
	      itstate = thumb_advance_itstate (itstate);
	    }

	  return MAKE_THUMB_ADDR (pc);
	}
      else if (itstate != 0)
	{
	  /* We are in a conditional block.  Check the condition.  */
	  if (! condition_true (itstate >> 4, status))
	    {
	      /* Advance to the next executed instruction.  */
	      pc += thumb_insn_size (inst1);
	      itstate = thumb_advance_itstate (itstate);

	      while (itstate != 0 && ! condition_true (itstate >> 4, status))
		{
		  inst1 = read_memory_unsigned_integer (pc, 2, 
							byte_order_for_code);
		  pc += thumb_insn_size (inst1);
		  itstate = thumb_advance_itstate (itstate);
		}

	      return MAKE_THUMB_ADDR (pc);
	    }
	  else if ((itstate & 0x0f) == 0x08)
	    {
	      /* This is the last instruction of the conditional
		 block, and it is executed.  We can handle it normally
		 because the following instruction is not conditional,
		 and we must handle it normally because it is
		 permitted to branch.  Fall through.  */
	    }
	  else
	    {
	      int cond_negated;

	      /* There are conditional instructions after this one.
		 If this instruction modifies the flags, then we can
		 not predict what the next executed instruction will
		 be.  Fortunately, this instruction is architecturally
		 forbidden to branch; we know it will fall through.
		 Start by skipping past it.  */
	      pc += thumb_insn_size (inst1);
	      itstate = thumb_advance_itstate (itstate);

	      /* Set a breakpoint on the following instruction.  */
	      gdb_assert ((itstate & 0x0f) != 0);
	      arm_insert_single_step_breakpoint (gdbarch, aspace,
						 MAKE_THUMB_ADDR (pc));
	      cond_negated = (itstate >> 4) & 1;

	      /* Skip all following instructions with the same
		 condition.  If there is a later instruction in the IT
		 block with the opposite condition, set the other
		 breakpoint there.  If not, then set a breakpoint on
		 the instruction after the IT block.  */
	      do
		{
		  inst1 = read_memory_unsigned_integer (pc, 2,
							byte_order_for_code);
		  pc += thumb_insn_size (inst1);
		  itstate = thumb_advance_itstate (itstate);
		}
	      while (itstate != 0 && ((itstate >> 4) & 1) == cond_negated);

	      return MAKE_THUMB_ADDR (pc);
	    }
	}
    }
  else if (itstate & 0x0f)
    {
      /* We are in a conditional block.  Check the condition.  */
      int cond = itstate >> 4;

      if (! condition_true (cond, status))
	/* Advance to the next instruction.  All the 32-bit
	   instructions share a common prefix.  */
	return MAKE_THUMB_ADDR (pc + thumb_insn_size (inst1));

      /* Otherwise, handle the instruction normally.  */
    }

  if ((inst1 & 0xff00) == 0xbd00)	/* pop {rlist, pc} */
    {
      CORE_ADDR sp;

      /* Fetch the saved PC from the stack.  It's stored above
         all of the other registers.  */
      offset = bitcount (bits (inst1, 0, 7)) * INT_REGISTER_SIZE;
      sp = get_frame_register_unsigned (frame, ARM_SP_REGNUM);
      nextpc = read_memory_unsigned_integer (sp + offset, 4, byte_order);
    }
  else if ((inst1 & 0xf000) == 0xd000)	/* conditional branch */
    {
      unsigned long cond = bits (inst1, 8, 11);
      if (cond == 0x0f)  /* 0x0f = SWI */
	{
	  struct gdbarch_tdep *tdep;
	  tdep = gdbarch_tdep (gdbarch);

	  if (tdep->syscall_next_pc != NULL)
	    nextpc = tdep->syscall_next_pc (frame);

	}
      else if (cond != 0x0f && condition_true (cond, status))
	nextpc = pc_val + (sbits (inst1, 0, 7) << 1);
    }
  else if ((inst1 & 0xf800) == 0xe000)	/* unconditional branch */
    {
      nextpc = pc_val + (sbits (inst1, 0, 10) << 1);
    }
  else if (thumb_insn_size (inst1) == 4) /* 32-bit instruction */
    {
      unsigned short inst2;
      inst2 = read_memory_unsigned_integer (pc + 2, 2, byte_order_for_code);

      /* Default to the next instruction.  */
      nextpc = pc + 4;
      nextpc = MAKE_THUMB_ADDR (nextpc);

      if ((inst1 & 0xf800) == 0xf000 && (inst2 & 0x8000) == 0x8000)
	{
	  /* Branches and miscellaneous control instructions.  */

	  if ((inst2 & 0x1000) != 0 || (inst2 & 0xd001) == 0xc000)
	    {
	      /* B, BL, BLX.  */
	      int j1, j2, imm1, imm2;

	      imm1 = sbits (inst1, 0, 10);
	      imm2 = bits (inst2, 0, 10);
	      j1 = bit (inst2, 13);
	      j2 = bit (inst2, 11);

	      offset = ((imm1 << 12) + (imm2 << 1));
	      offset ^= ((!j2) << 22) | ((!j1) << 23);

	      nextpc = pc_val + offset;
	      /* For BLX make sure to clear the low bits.  */
	      if (bit (inst2, 12) == 0)
		nextpc = nextpc & 0xfffffffc;
	    }
	  else if (inst1 == 0xf3de && (inst2 & 0xff00) == 0x3f00)
	    {
	      /* SUBS PC, LR, #imm8.  */
	      nextpc = get_frame_register_unsigned (frame, ARM_LR_REGNUM);
	      nextpc -= inst2 & 0x00ff;
	    }
	  else if ((inst2 & 0xd000) == 0x8000 && (inst1 & 0x0380) != 0x0380)
	    {
	      /* Conditional branch.  */
	      if (condition_true (bits (inst1, 6, 9), status))
		{
		  int sign, j1, j2, imm1, imm2;

		  sign = sbits (inst1, 10, 10);
		  imm1 = bits (inst1, 0, 5);
		  imm2 = bits (inst2, 0, 10);
		  j1 = bit (inst2, 13);
		  j2 = bit (inst2, 11);

		  offset = (sign << 20) + (j2 << 19) + (j1 << 18);
		  offset += (imm1 << 12) + (imm2 << 1);

		  nextpc = pc_val + offset;
		}
	    }
	}
      else if ((inst1 & 0xfe50) == 0xe810)
	{
	  /* Load multiple or RFE.  */
	  int rn, offset, load_pc = 1;

	  rn = bits (inst1, 0, 3);
	  if (bit (inst1, 7) && !bit (inst1, 8))
	    {
	      /* LDMIA or POP */
	      if (!bit (inst2, 15))
		load_pc = 0;
	      offset = bitcount (inst2) * 4 - 4;
	    }
	  else if (!bit (inst1, 7) && bit (inst1, 8))
	    {
	      /* LDMDB */
	      if (!bit (inst2, 15))
		load_pc = 0;
	      offset = -4;
	    }
	  else if (bit (inst1, 7) && bit (inst1, 8))
	    {
	      /* RFEIA */
	      offset = 0;
	    }
	  else if (!bit (inst1, 7) && !bit (inst1, 8))
	    {
	      /* RFEDB */
	      offset = -8;
	    }
	  else
	    load_pc = 0;

	  if (load_pc)
	    {
	      CORE_ADDR addr = get_frame_register_unsigned (frame, rn);
	      nextpc = get_frame_memory_unsigned (frame, addr + offset, 4);
	    }
	}
      else if ((inst1 & 0xffef) == 0xea4f && (inst2 & 0xfff0) == 0x0f00)
	{
	  /* MOV PC or MOVS PC.  */
	  nextpc = get_frame_register_unsigned (frame, bits (inst2, 0, 3));
	  nextpc = MAKE_THUMB_ADDR (nextpc);
	}
      else if ((inst1 & 0xff70) == 0xf850 && (inst2 & 0xf000) == 0xf000)
	{
	  /* LDR PC.  */
	  CORE_ADDR base;
	  int rn, load_pc = 1;

	  rn = bits (inst1, 0, 3);
	  base = get_frame_register_unsigned (frame, rn);
	  if (rn == ARM_PC_REGNUM)
	    {
	      base = (base + 4) & ~(CORE_ADDR) 0x3;
	      if (bit (inst1, 7))
		base += bits (inst2, 0, 11);
	      else
		base -= bits (inst2, 0, 11);
	    }
	  else if (bit (inst1, 7))
	    base += bits (inst2, 0, 11);
	  else if (bit (inst2, 11))
	    {
	      if (bit (inst2, 10))
		{
		  if (bit (inst2, 9))
		    base += bits (inst2, 0, 7);
		  else
		    base -= bits (inst2, 0, 7);
		}
	    }
	  else if ((inst2 & 0x0fc0) == 0x0000)
	    {
	      int shift = bits (inst2, 4, 5), rm = bits (inst2, 0, 3);
	      base += get_frame_register_unsigned (frame, rm) << shift;
	    }
	  else
	    /* Reserved.  */
	    load_pc = 0;

	  if (load_pc)
	    nextpc = get_frame_memory_unsigned (frame, base, 4);
	}
      else if ((inst1 & 0xfff0) == 0xe8d0 && (inst2 & 0xfff0) == 0xf000)
	{
	  /* TBB.  */
	  CORE_ADDR tbl_reg, table, offset, length;

	  tbl_reg = bits (inst1, 0, 3);
	  if (tbl_reg == 0x0f)
	    table = pc + 4;  /* Regcache copy of PC isn't right yet.  */
	  else
	    table = get_frame_register_unsigned (frame, tbl_reg);

	  offset = get_frame_register_unsigned (frame, bits (inst2, 0, 3));
	  length = 2 * get_frame_memory_unsigned (frame, table + offset, 1);
	  nextpc = pc_val + length;
	}
      else if ((inst1 & 0xfff0) == 0xe8d0 && (inst2 & 0xfff0) == 0xf010)
	{
	  /* TBH.  */
	  CORE_ADDR tbl_reg, table, offset, length;

	  tbl_reg = bits (inst1, 0, 3);
	  if (tbl_reg == 0x0f)
	    table = pc + 4;  /* Regcache copy of PC isn't right yet.  */
	  else
	    table = get_frame_register_unsigned (frame, tbl_reg);

	  offset = 2 * get_frame_register_unsigned (frame, bits (inst2, 0, 3));
	  length = 2 * get_frame_memory_unsigned (frame, table + offset, 2);
	  nextpc = pc_val + length;
	}
    }
  else if ((inst1 & 0xff00) == 0x4700)	/* bx REG, blx REG */
    {
      if (bits (inst1, 3, 6) == 0x0f)
	nextpc = pc_val;
      else
	nextpc = get_frame_register_unsigned (frame, bits (inst1, 3, 6));
    }
  else if ((inst1 & 0xff87) == 0x4687)	/* mov pc, REG */
    {
      if (bits (inst1, 3, 6) == 0x0f)
	nextpc = pc_val;
      else
	nextpc = get_frame_register_unsigned (frame, bits (inst1, 3, 6));

      nextpc = MAKE_THUMB_ADDR (nextpc);
    }
  else if ((inst1 & 0xf500) == 0xb100)
    {
      /* CBNZ or CBZ.  */
      int imm = (bit (inst1, 9) << 6) + (bits (inst1, 3, 7) << 1);
      ULONGEST reg = get_frame_register_unsigned (frame, bits (inst1, 0, 2));

      if (bit (inst1, 11) && reg != 0)
	nextpc = pc_val + imm;
      else if (!bit (inst1, 11) && reg == 0)
	nextpc = pc_val + imm;
    }
  return nextpc;
}

/* Get the raw next address.  PC is the current program counter, in 
   FRAME, which is assumed to be executing in ARM mode.

   The value returned has the execution state of the next instruction 
   encoded in it.  Use IS_THUMB_ADDR () to see whether the instruction is
   in Thumb-State, and gdbarch_addr_bits_remove () to get the plain memory
   address.  */

static CORE_ADDR
arm_get_next_pc_raw (struct frame_info *frame, CORE_ADDR pc)
{
  struct gdbarch *gdbarch = get_frame_arch (frame);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  unsigned long pc_val;
  unsigned long this_instr;
  unsigned long status;
  CORE_ADDR nextpc;

  pc_val = (unsigned long) pc;
  this_instr = read_memory_unsigned_integer (pc, 4, byte_order_for_code);

  status = get_frame_register_unsigned (frame, ARM_PS_REGNUM);
  nextpc = (CORE_ADDR) (pc_val + 4);	/* Default case */

  if (bits (this_instr, 28, 31) == INST_NV)
    switch (bits (this_instr, 24, 27))
      {
      case 0xa:
      case 0xb:
	{
	  /* Branch with Link and change to Thumb.  */
	  nextpc = BranchDest (pc, this_instr);
	  nextpc |= bit (this_instr, 24) << 1;
	  nextpc = MAKE_THUMB_ADDR (nextpc);
	  break;
	}
      case 0xc:
      case 0xd:
      case 0xe:
	/* Coprocessor register transfer.  */
        if (bits (this_instr, 12, 15) == 15)
	  error (_("Invalid update to pc in instruction"));
	break;
      }
  else if (condition_true (bits (this_instr, 28, 31), status))
    {
      switch (bits (this_instr, 24, 27))
	{
	case 0x0:
	case 0x1:			/* data processing */
	case 0x2:
	case 0x3:
	  {
	    unsigned long operand1, operand2, result = 0;
	    unsigned long rn;
	    int c;

	    if (bits (this_instr, 12, 15) != 15)
	      break;

	    if (bits (this_instr, 22, 25) == 0
		&& bits (this_instr, 4, 7) == 9)	/* multiply */
	      error (_("Invalid update to pc in instruction"));

	    /* BX <reg>, BLX <reg> */
	    if (bits (this_instr, 4, 27) == 0x12fff1
		|| bits (this_instr, 4, 27) == 0x12fff3)
	      {
		rn = bits (this_instr, 0, 3);
		nextpc = ((rn == ARM_PC_REGNUM)
			  ? (pc_val + 8)
			  : get_frame_register_unsigned (frame, rn));

		return nextpc;
	      }

	    /* Multiply into PC.  */
	    c = (status & FLAG_C) ? 1 : 0;
	    rn = bits (this_instr, 16, 19);
	    operand1 = ((rn == ARM_PC_REGNUM)
			? (pc_val + 8)
			: get_frame_register_unsigned (frame, rn));

	    if (bit (this_instr, 25))
	      {
		unsigned long immval = bits (this_instr, 0, 7);
		unsigned long rotate = 2 * bits (this_instr, 8, 11);
		operand2 = ((immval >> rotate) | (immval << (32 - rotate)))
		  & 0xffffffff;
	      }
	    else		/* operand 2 is a shifted register.  */
	      operand2 = shifted_reg_val (frame, this_instr, c,
					  pc_val, status);

	    switch (bits (this_instr, 21, 24))
	      {
	      case 0x0:	/*and */
		result = operand1 & operand2;
		break;

	      case 0x1:	/*eor */
		result = operand1 ^ operand2;
		break;

	      case 0x2:	/*sub */
		result = operand1 - operand2;
		break;

	      case 0x3:	/*rsb */
		result = operand2 - operand1;
		break;

	      case 0x4:	/*add */
		result = operand1 + operand2;
		break;

	      case 0x5:	/*adc */
		result = operand1 + operand2 + c;
		break;

	      case 0x6:	/*sbc */
		result = operand1 - operand2 + c;
		break;

	      case 0x7:	/*rsc */
		result = operand2 - operand1 + c;
		break;

	      case 0x8:
	      case 0x9:
	      case 0xa:
	      case 0xb:	/* tst, teq, cmp, cmn */
		result = (unsigned long) nextpc;
		break;

	      case 0xc:	/*orr */
		result = operand1 | operand2;
		break;

	      case 0xd:	/*mov */
		/* Always step into a function.  */
		result = operand2;
		break;

	      case 0xe:	/*bic */
		result = operand1 & ~operand2;
		break;

	      case 0xf:	/*mvn */
		result = ~operand2;
		break;
	      }

            /* In 26-bit APCS the bottom two bits of the result are 
	       ignored, and we always end up in ARM state.  */
	    if (!arm_apcs_32)
	      nextpc = arm_addr_bits_remove (gdbarch, result);
	    else
	      nextpc = result;

	    break;
	  }

	case 0x4:
	case 0x5:		/* data transfer */
	case 0x6:
	case 0x7:
	  if (bit (this_instr, 20))
	    {
	      /* load */
	      if (bits (this_instr, 12, 15) == 15)
		{
		  /* rd == pc */
		  unsigned long rn;
		  unsigned long base;

		  if (bit (this_instr, 22))
		    error (_("Invalid update to pc in instruction"));

		  /* byte write to PC */
		  rn = bits (this_instr, 16, 19);
		  base = ((rn == ARM_PC_REGNUM)
			  ? (pc_val + 8)
			  : get_frame_register_unsigned (frame, rn));

		  if (bit (this_instr, 24))
		    {
		      /* pre-indexed */
		      int c = (status & FLAG_C) ? 1 : 0;
		      unsigned long offset =
		      (bit (this_instr, 25)
		       ? shifted_reg_val (frame, this_instr, c, pc_val, status)
		       : bits (this_instr, 0, 11));

		      if (bit (this_instr, 23))
			base += offset;
		      else
			base -= offset;
		    }
		  nextpc =
		    (CORE_ADDR) read_memory_unsigned_integer ((CORE_ADDR) base,
							      4, byte_order);
		}
	    }
	  break;

	case 0x8:
	case 0x9:		/* block transfer */
	  if (bit (this_instr, 20))
	    {
	      /* LDM */
	      if (bit (this_instr, 15))
		{
		  /* loading pc */
		  int offset = 0;
		  unsigned long rn_val
		    = get_frame_register_unsigned (frame,
						   bits (this_instr, 16, 19));

		  if (bit (this_instr, 23))
		    {
		      /* up */
		      unsigned long reglist = bits (this_instr, 0, 14);
		      offset = bitcount (reglist) * 4;
		      if (bit (this_instr, 24))		/* pre */
			offset += 4;
		    }
		  else if (bit (this_instr, 24))
		    offset = -4;

		  nextpc =
		    (CORE_ADDR) read_memory_unsigned_integer ((CORE_ADDR)
							      (rn_val + offset),
							      4, byte_order);
		}
	    }
	  break;

	case 0xb:		/* branch & link */
	case 0xa:		/* branch */
	  {
	    nextpc = BranchDest (pc, this_instr);
	    break;
	  }

	case 0xc:
	case 0xd:
	case 0xe:		/* coproc ops */
	  break;
	case 0xf:		/* SWI */
	  {
	    struct gdbarch_tdep *tdep;
	    tdep = gdbarch_tdep (gdbarch);

	    if (tdep->syscall_next_pc != NULL)
	      nextpc = tdep->syscall_next_pc (frame);

	  }
	  break;

	default:
	  fprintf_filtered (gdb_stderr, _("Bad bit-field extraction\n"));
	  return (pc);
	}
    }

  return nextpc;
}

/* Determine next PC after current instruction executes.  Will call either
   arm_get_next_pc_raw or thumb_get_next_pc_raw.  Error out if infinite
   loop is detected.  */

CORE_ADDR
arm_get_next_pc (struct frame_info *frame, CORE_ADDR pc)
{
  CORE_ADDR nextpc;

  if (arm_frame_is_thumb (frame))
    {
      nextpc = thumb_get_next_pc_raw (frame, pc);
      if (nextpc == MAKE_THUMB_ADDR (pc))
	error (_("Infinite loop detected"));
    }
  else
    {
      nextpc = arm_get_next_pc_raw (frame, pc);
      if (nextpc == pc)
	error (_("Infinite loop detected"));
    }

  return nextpc;
}

/* Like insert_single_step_breakpoint, but make sure we use a breakpoint
   of the appropriate mode (as encoded in the PC value), even if this
   differs from what would be expected according to the symbol tables.  */

void
arm_insert_single_step_breakpoint (struct gdbarch *gdbarch,
				   struct address_space *aspace,
				   CORE_ADDR pc)
{
  struct cleanup *old_chain
    = make_cleanup_restore_integer (&arm_override_mode);

  arm_override_mode = IS_THUMB_ADDR (pc);
  pc = gdbarch_addr_bits_remove (gdbarch, pc);

  insert_single_step_breakpoint (gdbarch, aspace, pc);

  do_cleanups (old_chain);
}

/* Checks for an atomic sequence of instructions beginning with a LDREX{,B,H,D}
   instruction and ending with a STREX{,B,H,D} instruction.  If such a sequence
   is found, attempt to step through it.  A breakpoint is placed at the end of
   the sequence.  */

static int
thumb_deal_with_atomic_sequence_raw (struct frame_info *frame)
{
  struct gdbarch *gdbarch = get_frame_arch (frame);
  struct address_space *aspace = get_frame_address_space (frame);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  CORE_ADDR pc = get_frame_pc (frame);
  CORE_ADDR breaks[2] = {-1, -1};
  CORE_ADDR loc = pc;
  unsigned short insn1, insn2;
  int insn_count;
  int index;
  int last_breakpoint = 0; /* Defaults to 0 (no breakpoints placed).  */
  const int atomic_sequence_length = 16; /* Instruction sequence length.  */
  ULONGEST status, itstate;

  /* We currently do not support atomic sequences within an IT block.  */
  status = get_frame_register_unsigned (frame, ARM_PS_REGNUM);
  itstate = ((status >> 8) & 0xfc) | ((status >> 25) & 0x3);
  if (itstate & 0x0f)
    return 0;

  /* Assume all atomic sequences start with a ldrex{,b,h,d} instruction.  */
  insn1 = read_memory_unsigned_integer (loc, 2, byte_order_for_code);
  loc += 2;
  if (thumb_insn_size (insn1) != 4)
    return 0;

  insn2 = read_memory_unsigned_integer (loc, 2, byte_order_for_code);
  loc += 2;
  if (!((insn1 & 0xfff0) == 0xe850
        || ((insn1 & 0xfff0) == 0xe8d0 && (insn2 & 0x00c0) == 0x0040)))
    return 0;

  /* Assume that no atomic sequence is longer than "atomic_sequence_length"
     instructions.  */
  for (insn_count = 0; insn_count < atomic_sequence_length; ++insn_count)
    {
      insn1 = read_memory_unsigned_integer (loc, 2, byte_order_for_code);
      loc += 2;

      if (thumb_insn_size (insn1) != 4)
	{
	  /* Assume that there is at most one conditional branch in the
	     atomic sequence.  If a conditional branch is found, put a
	     breakpoint in its destination address.  */
	  if ((insn1 & 0xf000) == 0xd000 && bits (insn1, 8, 11) != 0x0f)
	    {
	      if (last_breakpoint > 0)
		return 0; /* More than one conditional branch found,
			     fallback to the standard code.  */

	      breaks[1] = loc + 2 + (sbits (insn1, 0, 7) << 1);
	      last_breakpoint++;
	    }

	  /* We do not support atomic sequences that use any *other*
	     instructions but conditional branches to change the PC.
	     Fall back to standard code to avoid losing control of
	     execution.  */
	  else if (thumb_instruction_changes_pc (insn1))
	    return 0;
	}
      else
	{
	  insn2 = read_memory_unsigned_integer (loc, 2, byte_order_for_code);
	  loc += 2;

	  /* Assume that there is at most one conditional branch in the
	     atomic sequence.  If a conditional branch is found, put a
	     breakpoint in its destination address.  */
	  if ((insn1 & 0xf800) == 0xf000
	      && (insn2 & 0xd000) == 0x8000
	      && (insn1 & 0x0380) != 0x0380)
	    {
	      int sign, j1, j2, imm1, imm2;
	      unsigned int offset;

	      sign = sbits (insn1, 10, 10);
	      imm1 = bits (insn1, 0, 5);
	      imm2 = bits (insn2, 0, 10);
	      j1 = bit (insn2, 13);
	      j2 = bit (insn2, 11);

	      offset = (sign << 20) + (j2 << 19) + (j1 << 18);
	      offset += (imm1 << 12) + (imm2 << 1);

	      if (last_breakpoint > 0)
		return 0; /* More than one conditional branch found,
			     fallback to the standard code.  */

	      breaks[1] = loc + offset;
	      last_breakpoint++;
	    }

	  /* We do not support atomic sequences that use any *other*
	     instructions but conditional branches to change the PC.
	     Fall back to standard code to avoid losing control of
	     execution.  */
	  else if (thumb2_instruction_changes_pc (insn1, insn2))
	    return 0;

	  /* If we find a strex{,b,h,d}, we're done.  */
	  if ((insn1 & 0xfff0) == 0xe840
	      || ((insn1 & 0xfff0) == 0xe8c0 && (insn2 & 0x00c0) == 0x0040))
	    break;
	}
    }

  /* If we didn't find the strex{,b,h,d}, we cannot handle the sequence.  */
  if (insn_count == atomic_sequence_length)
    return 0;

  /* Insert a breakpoint right after the end of the atomic sequence.  */
  breaks[0] = loc;

  /* Check for duplicated breakpoints.  Check also for a breakpoint
     placed (branch instruction's destination) anywhere in sequence.  */
  if (last_breakpoint
      && (breaks[1] == breaks[0]
	  || (breaks[1] >= pc && breaks[1] < loc)))
    last_breakpoint = 0;

  /* Effectively inserts the breakpoints.  */
  for (index = 0; index <= last_breakpoint; index++)
    arm_insert_single_step_breakpoint (gdbarch, aspace,
				       MAKE_THUMB_ADDR (breaks[index]));

  return 1;
}

static int
arm_deal_with_atomic_sequence_raw (struct frame_info *frame)
{
  struct gdbarch *gdbarch = get_frame_arch (frame);
  struct address_space *aspace = get_frame_address_space (frame);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  CORE_ADDR pc = get_frame_pc (frame);
  CORE_ADDR breaks[2] = {-1, -1};
  CORE_ADDR loc = pc;
  unsigned int insn;
  int insn_count;
  int index;
  int last_breakpoint = 0; /* Defaults to 0 (no breakpoints placed).  */
  const int atomic_sequence_length = 16; /* Instruction sequence length.  */

  /* Assume all atomic sequences start with a ldrex{,b,h,d} instruction.
     Note that we do not currently support conditionally executed atomic
     instructions.  */
  insn = read_memory_unsigned_integer (loc, 4, byte_order_for_code);
  loc += 4;
  if ((insn & 0xff9000f0) != 0xe1900090)
    return 0;

  /* Assume that no atomic sequence is longer than "atomic_sequence_length"
     instructions.  */
  for (insn_count = 0; insn_count < atomic_sequence_length; ++insn_count)
    {
      insn = read_memory_unsigned_integer (loc, 4, byte_order_for_code);
      loc += 4;

      /* Assume that there is at most one conditional branch in the atomic
         sequence.  If a conditional branch is found, put a breakpoint in
         its destination address.  */
      if (bits (insn, 24, 27) == 0xa)
	{
          if (last_breakpoint > 0)
            return 0; /* More than one conditional branch found, fallback
                         to the standard single-step code.  */

	  breaks[1] = BranchDest (loc - 4, insn);
	  last_breakpoint++;
        }

      /* We do not support atomic sequences that use any *other* instructions
         but conditional branches to change the PC.  Fall back to standard
	 code to avoid losing control of execution.  */
      else if (arm_instruction_changes_pc (insn))
	return 0;

      /* If we find a strex{,b,h,d}, we're done.  */
      if ((insn & 0xff9000f0) == 0xe1800090)
	break;
    }

  /* If we didn't find the strex{,b,h,d}, we cannot handle the sequence.  */
  if (insn_count == atomic_sequence_length)
    return 0;

  /* Insert a breakpoint right after the end of the atomic sequence.  */
  breaks[0] = loc;

  /* Check for duplicated breakpoints.  Check also for a breakpoint
     placed (branch instruction's destination) anywhere in sequence.  */
  if (last_breakpoint
      && (breaks[1] == breaks[0]
	  || (breaks[1] >= pc && breaks[1] < loc)))
    last_breakpoint = 0;

  /* Effectively inserts the breakpoints.  */
  for (index = 0; index <= last_breakpoint; index++)
    arm_insert_single_step_breakpoint (gdbarch, aspace, breaks[index]);

  return 1;
}

int
arm_deal_with_atomic_sequence (struct frame_info *frame)
{
  if (arm_frame_is_thumb (frame))
    return thumb_deal_with_atomic_sequence_raw (frame);
  else
    return arm_deal_with_atomic_sequence_raw (frame);
}

/* single_step() is called just before we want to resume the inferior,
   if we want to single-step it but there is no hardware or kernel
   single-step support.  We find the target of the coming instruction
   and breakpoint it.  */

int
arm_software_single_step (struct frame_info *frame)
{
  struct gdbarch *gdbarch = get_frame_arch (frame);
  struct address_space *aspace = get_frame_address_space (frame);
  CORE_ADDR next_pc;

  if (arm_deal_with_atomic_sequence (frame))
    return 1;

  next_pc = arm_get_next_pc (frame, get_frame_pc (frame));
  arm_insert_single_step_breakpoint (gdbarch, aspace, next_pc);

  return 1;
}

/* Given BUF, which is OLD_LEN bytes ending at ENDADDR, expand
   the buffer to be NEW_LEN bytes ending at ENDADDR.  Return
   NULL if an error occurs.  BUF is freed.  */

static gdb_byte *
extend_buffer_earlier (gdb_byte *buf, CORE_ADDR endaddr,
		       int old_len, int new_len)
{
  gdb_byte *new_buf, *middle;
  int bytes_to_read = new_len - old_len;

  new_buf = xmalloc (new_len);
  memcpy (new_buf + bytes_to_read, buf, old_len);
  xfree (buf);
  if (target_read_memory (endaddr - new_len, new_buf, bytes_to_read) != 0)
    {
      xfree (new_buf);
      return NULL;
    }
  return new_buf;
}

/* An IT block is at most the 2-byte IT instruction followed by
   four 4-byte instructions.  The furthest back we must search to
   find an IT block that affects the current instruction is thus
   2 + 3 * 4 == 14 bytes.  */
#define MAX_IT_BLOCK_PREFIX 14

/* Use a quick scan if there are more than this many bytes of
   code.  */
#define IT_SCAN_THRESHOLD 32

/* Adjust a breakpoint's address to move breakpoints out of IT blocks.
   A breakpoint in an IT block may not be hit, depending on the
   condition flags.  */
static CORE_ADDR
arm_adjust_breakpoint_address (struct gdbarch *gdbarch, CORE_ADDR bpaddr)
{
  gdb_byte *buf;
  char map_type;
  CORE_ADDR boundary, func_start;
  int buf_len, buf2_len;
  enum bfd_endian order = gdbarch_byte_order_for_code (gdbarch);
  int i, any, last_it, last_it_count;

  /* If we are using BKPT breakpoints, none of this is necessary.  */
  if (gdbarch_tdep (gdbarch)->thumb2_breakpoint == NULL)
    return bpaddr;

  /* ARM mode does not have this problem.  */
  if (!arm_pc_is_thumb (gdbarch, bpaddr))
    return bpaddr;

  /* We are setting a breakpoint in Thumb code that could potentially
     contain an IT block.  The first step is to find how much Thumb
     code there is; we do not need to read outside of known Thumb
     sequences.  */
  map_type = arm_find_mapping_symbol (bpaddr, &boundary);
  if (map_type == 0)
    /* Thumb-2 code must have mapping symbols to have a chance.  */
    return bpaddr;

  bpaddr = gdbarch_addr_bits_remove (gdbarch, bpaddr);

  if (find_pc_partial_function (bpaddr, NULL, &func_start, NULL)
      && func_start > boundary)
    boundary = func_start;

  /* Search for a candidate IT instruction.  We have to do some fancy
     footwork to distinguish a real IT instruction from the second
     half of a 32-bit instruction, but there is no need for that if
     there's no candidate.  */
  buf_len = min (bpaddr - boundary, MAX_IT_BLOCK_PREFIX);
  if (buf_len == 0)
    /* No room for an IT instruction.  */
    return bpaddr;

  buf = xmalloc (buf_len);
  if (target_read_memory (bpaddr - buf_len, buf, buf_len) != 0)
    return bpaddr;
  any = 0;
  for (i = 0; i < buf_len; i += 2)
    {
      unsigned short inst1 = extract_unsigned_integer (&buf[i], 2, order);
      if ((inst1 & 0xff00) == 0xbf00 && (inst1 & 0x000f) != 0)
	{
	  any = 1;
	  break;
	}
    }
  if (any == 0)
    {
      xfree (buf);
      return bpaddr;
    }

  /* OK, the code bytes before this instruction contain at least one
     halfword which resembles an IT instruction.  We know that it's
     Thumb code, but there are still two possibilities.  Either the
     halfword really is an IT instruction, or it is the second half of
     a 32-bit Thumb instruction.  The only way we can tell is to
     scan forwards from a known instruction boundary.  */
  if (bpaddr - boundary > IT_SCAN_THRESHOLD)
    {
      int definite;

      /* There's a lot of code before this instruction.  Start with an
	 optimistic search; it's easy to recognize halfwords that can
	 not be the start of a 32-bit instruction, and use that to
	 lock on to the instruction boundaries.  */
      buf = extend_buffer_earlier (buf, bpaddr, buf_len, IT_SCAN_THRESHOLD);
      if (buf == NULL)
	return bpaddr;
      buf_len = IT_SCAN_THRESHOLD;

      definite = 0;
      for (i = 0; i < buf_len - sizeof (buf) && ! definite; i += 2)
	{
	  unsigned short inst1 = extract_unsigned_integer (&buf[i], 2, order);
	  if (thumb_insn_size (inst1) == 2)
	    {
	      definite = 1;
	      break;
	    }
	}

      /* At this point, if DEFINITE, BUF[I] is the first place we
	 are sure that we know the instruction boundaries, and it is far
	 enough from BPADDR that we could not miss an IT instruction
	 affecting BPADDR.  If ! DEFINITE, give up - start from a
	 known boundary.  */
      if (! definite)
	{
	  buf = extend_buffer_earlier (buf, bpaddr, buf_len,
				       bpaddr - boundary);
	  if (buf == NULL)
	    return bpaddr;
	  buf_len = bpaddr - boundary;
	  i = 0;
	}
    }
  else
    {
      buf = extend_buffer_earlier (buf, bpaddr, buf_len, bpaddr - boundary);
      if (buf == NULL)
	return bpaddr;
      buf_len = bpaddr - boundary;
      i = 0;
    }

  /* Scan forwards.  Find the last IT instruction before BPADDR.  */
  last_it = -1;
  last_it_count = 0;
  while (i < buf_len)
    {
      unsigned short inst1 = extract_unsigned_integer (&buf[i], 2, order);
      last_it_count--;
      if ((inst1 & 0xff00) == 0xbf00 && (inst1 & 0x000f) != 0)
	{
	  last_it = i;
	  if (inst1 & 0x0001)
	    last_it_count = 4;
	  else if (inst1 & 0x0002)
	    last_it_count = 3;
	  else if (inst1 & 0x0004)
	    last_it_count = 2;
	  else
	    last_it_count = 1;
	}
      i += thumb_insn_size (inst1);
    }

  xfree (buf);

  if (last_it == -1)
    /* There wasn't really an IT instruction after all.  */
    return bpaddr;

  if (last_it_count < 1)
    /* It was too far away.  */
    return bpaddr;

  /* This really is a trouble spot.  Move the breakpoint to the IT
     instruction.  */
  return bpaddr - buf_len + last_it;
}

/* ARM displaced stepping support.

   Generally ARM displaced stepping works as follows:

   1. When an instruction is to be single-stepped, it is first decoded by
      arm_process_displaced_insn (called from arm_displaced_step_copy_insn).
      Depending on the type of instruction, it is then copied to a scratch
      location, possibly in a modified form.  The copy_* set of functions
      performs such modification, as necessary.  A breakpoint is placed after
      the modified instruction in the scratch space to return control to GDB.
      Note in particular that instructions which modify the PC will no longer
      do so after modification.

   2. The instruction is single-stepped, by setting the PC to the scratch
      location address, and resuming.  Control returns to GDB when the
      breakpoint is hit.

   3. A cleanup function (cleanup_*) is called corresponding to the copy_*
      function used for the current instruction.  This function's job is to
      put the CPU/memory state back to what it would have been if the
      instruction had been executed unmodified in its original location.  */

/* NOP instruction (mov r0, r0).  */
#define ARM_NOP				0xe1a00000
#define THUMB_NOP 0x4600

/* Helper for register reads for displaced stepping.  In particular, this
   returns the PC as it would be seen by the instruction at its original
   location.  */

ULONGEST
displaced_read_reg (struct regcache *regs, struct displaced_step_closure *dsc,
		    int regno)
{
  ULONGEST ret;
  CORE_ADDR from = dsc->insn_addr;

  if (regno == ARM_PC_REGNUM)
    {
      /* Compute pipeline offset:
	 - When executing an ARM instruction, PC reads as the address of the
	 current instruction plus 8.
	 - When executing a Thumb instruction, PC reads as the address of the
	 current instruction plus 4.  */

      if (!dsc->is_thumb)
	from += 8;
      else
	from += 4;

      if (debug_displaced)
	fprintf_unfiltered (gdb_stdlog, "displaced: read pc value %.8lx\n",
			    (unsigned long) from);
      return (ULONGEST) from;
    }
  else
    {
      regcache_cooked_read_unsigned (regs, regno, &ret);
      if (debug_displaced)
	fprintf_unfiltered (gdb_stdlog, "displaced: read r%d value %.8lx\n",
			    regno, (unsigned long) ret);
      return ret;
    }
}

static int
displaced_in_arm_mode (struct regcache *regs)
{
  ULONGEST ps;
  ULONGEST t_bit = arm_psr_thumb_bit (get_regcache_arch (regs));

  regcache_cooked_read_unsigned (regs, ARM_PS_REGNUM, &ps);

  return (ps & t_bit) == 0;
}

/* Write to the PC as from a branch instruction.  */

static void
branch_write_pc (struct regcache *regs, struct displaced_step_closure *dsc,
		 ULONGEST val)
{
  if (!dsc->is_thumb)
    /* Note: If bits 0/1 are set, this branch would be unpredictable for
       architecture versions < 6.  */
    regcache_cooked_write_unsigned (regs, ARM_PC_REGNUM,
				    val & ~(ULONGEST) 0x3);
  else
    regcache_cooked_write_unsigned (regs, ARM_PC_REGNUM,
				    val & ~(ULONGEST) 0x1);
}

/* Write to the PC as from a branch-exchange instruction.  */

static void
bx_write_pc (struct regcache *regs, ULONGEST val)
{
  ULONGEST ps;
  ULONGEST t_bit = arm_psr_thumb_bit (get_regcache_arch (regs));

  regcache_cooked_read_unsigned (regs, ARM_PS_REGNUM, &ps);

  if ((val & 1) == 1)
    {
      regcache_cooked_write_unsigned (regs, ARM_PS_REGNUM, ps | t_bit);
      regcache_cooked_write_unsigned (regs, ARM_PC_REGNUM, val & 0xfffffffe);
    }
  else if ((val & 2) == 0)
    {
      regcache_cooked_write_unsigned (regs, ARM_PS_REGNUM, ps & ~t_bit);
      regcache_cooked_write_unsigned (regs, ARM_PC_REGNUM, val);
    }
  else
    {
      /* Unpredictable behaviour.  Try to do something sensible (switch to ARM
	  mode, align dest to 4 bytes).  */
      warning (_("Single-stepping BX to non-word-aligned ARM instruction."));
      regcache_cooked_write_unsigned (regs, ARM_PS_REGNUM, ps & ~t_bit);
      regcache_cooked_write_unsigned (regs, ARM_PC_REGNUM, val & 0xfffffffc);
    }
}

/* Write to the PC as if from a load instruction.  */

static void
load_write_pc (struct regcache *regs, struct displaced_step_closure *dsc,
	       ULONGEST val)
{
  if (DISPLACED_STEPPING_ARCH_VERSION >= 5)
    bx_write_pc (regs, val);
  else
    branch_write_pc (regs, dsc, val);
}

/* Write to the PC as if from an ALU instruction.  */

static void
alu_write_pc (struct regcache *regs, struct displaced_step_closure *dsc,
	      ULONGEST val)
{
  if (DISPLACED_STEPPING_ARCH_VERSION >= 7 && !dsc->is_thumb)
    bx_write_pc (regs, val);
  else
    branch_write_pc (regs, dsc, val);
}

/* Helper for writing to registers for displaced stepping.  Writing to the PC
   has a varying effects depending on the instruction which does the write:
   this is controlled by the WRITE_PC argument.  */

void
displaced_write_reg (struct regcache *regs, struct displaced_step_closure *dsc,
		     int regno, ULONGEST val, enum pc_write_style write_pc)
{
  if (regno == ARM_PC_REGNUM)
    {
      if (debug_displaced)
	fprintf_unfiltered (gdb_stdlog, "displaced: writing pc %.8lx\n",
			    (unsigned long) val);
      switch (write_pc)
	{
	case BRANCH_WRITE_PC:
	  branch_write_pc (regs, dsc, val);
	  break;

	case BX_WRITE_PC:
	  bx_write_pc (regs, val);
  	  break;

	case LOAD_WRITE_PC:
	  load_write_pc (regs, dsc, val);
  	  break;

	case ALU_WRITE_PC:
	  alu_write_pc (regs, dsc, val);
  	  break;

	case CANNOT_WRITE_PC:
	  warning (_("Instruction wrote to PC in an unexpected way when "
		     "single-stepping"));
	  break;

	default:
	  internal_error (__FILE__, __LINE__,
			  _("Invalid argument to displaced_write_reg"));
	}

      dsc->wrote_to_pc = 1;
    }
  else
    {
      if (debug_displaced)
	fprintf_unfiltered (gdb_stdlog, "displaced: writing r%d value %.8lx\n",
			    regno, (unsigned long) val);
      regcache_cooked_write_unsigned (regs, regno, val);
    }
}

/* This function is used to concisely determine if an instruction INSN
   references PC.  Register fields of interest in INSN should have the
   corresponding fields of BITMASK set to 0b1111.  The function
   returns return 1 if any of these fields in INSN reference the PC
   (also 0b1111, r15), else it returns 0.  */

static int
insn_references_pc (uint32_t insn, uint32_t bitmask)
{
  uint32_t lowbit = 1;

  while (bitmask != 0)
    {
      uint32_t mask;

      for (; lowbit && (bitmask & lowbit) == 0; lowbit <<= 1)
	;

      if (!lowbit)
	break;

      mask = lowbit * 0xf;

      if ((insn & mask) == mask)
	return 1;

      bitmask &= ~mask;
    }

  return 0;
}

/* The simplest copy function.  Many instructions have the same effect no
   matter what address they are executed at: in those cases, use this.  */

static int
arm_copy_unmodified (struct gdbarch *gdbarch, uint32_t insn,
		     const char *iname, struct displaced_step_closure *dsc)
{
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying insn %.8lx, "
			"opcode/class '%s' unmodified\n", (unsigned long) insn,
			iname);

  dsc->modinsn[0] = insn;

  return 0;
}

static int
thumb_copy_unmodified_32bit (struct gdbarch *gdbarch, uint16_t insn1,
			     uint16_t insn2, const char *iname,
			     struct displaced_step_closure *dsc)
{
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying insn %.4x %.4x, "
			"opcode/class '%s' unmodified\n", insn1, insn2,
			iname);

  dsc->modinsn[0] = insn1;
  dsc->modinsn[1] = insn2;
  dsc->numinsns = 2;

  return 0;
}

/* Copy 16-bit Thumb(Thumb and 16-bit Thumb-2) instruction without any
   modification.  */
static int
thumb_copy_unmodified_16bit (struct gdbarch *gdbarch, unsigned int insn,
			     const char *iname,
			     struct displaced_step_closure *dsc)
{
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying insn %.4x, "
			"opcode/class '%s' unmodified\n", insn,
			iname);

  dsc->modinsn[0] = insn;

  return 0;
}

/* Preload instructions with immediate offset.  */

static void
cleanup_preload (struct gdbarch *gdbarch,
		 struct regcache *regs, struct displaced_step_closure *dsc)
{
  displaced_write_reg (regs, dsc, 0, dsc->tmp[0], CANNOT_WRITE_PC);
  if (!dsc->u.preload.immed)
    displaced_write_reg (regs, dsc, 1, dsc->tmp[1], CANNOT_WRITE_PC);
}

static void
install_preload (struct gdbarch *gdbarch, struct regcache *regs,
		 struct displaced_step_closure *dsc, unsigned int rn)
{
  ULONGEST rn_val;
  /* Preload instructions:

     {pli/pld} [rn, #+/-imm]
     ->
     {pli/pld} [r0, #+/-imm].  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  rn_val = displaced_read_reg (regs, dsc, rn);
  displaced_write_reg (regs, dsc, 0, rn_val, CANNOT_WRITE_PC);
  dsc->u.preload.immed = 1;

  dsc->cleanup = &cleanup_preload;
}

static int
arm_copy_preload (struct gdbarch *gdbarch, uint32_t insn, struct regcache *regs,
		  struct displaced_step_closure *dsc)
{
  unsigned int rn = bits (insn, 16, 19);

  if (!insn_references_pc (insn, 0x000f0000ul))
    return arm_copy_unmodified (gdbarch, insn, "preload", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying preload insn %.8lx\n",
			(unsigned long) insn);

  dsc->modinsn[0] = insn & 0xfff0ffff;

  install_preload (gdbarch, regs, dsc, rn);

  return 0;
}

static int
thumb2_copy_preload (struct gdbarch *gdbarch, uint16_t insn1, uint16_t insn2,
		     struct regcache *regs, struct displaced_step_closure *dsc)
{
  unsigned int rn = bits (insn1, 0, 3);
  unsigned int u_bit = bit (insn1, 7);
  int imm12 = bits (insn2, 0, 11);
  ULONGEST pc_val;

  if (rn != ARM_PC_REGNUM)
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2, "preload", dsc);

  /* PC is only allowed to use in PLI (immediate,literal) Encoding T3, and
     PLD (literal) Encoding T1.  */
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying pld/pli pc (0x%x) %c imm12 %.4x\n",
			(unsigned int) dsc->insn_addr, u_bit ? '+' : '-',
			imm12);

  if (!u_bit)
    imm12 = -1 * imm12;

  /* Rewrite instruction {pli/pld} PC imm12 into:
     Prepare: tmp[0] <- r0, tmp[1] <- r1, r0 <- pc, r1 <- imm12

     {pli/pld} [r0, r1]

     Cleanup: r0 <- tmp[0], r1 <- tmp[1].  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[1] = displaced_read_reg (regs, dsc, 1);

  pc_val = displaced_read_reg (regs, dsc, ARM_PC_REGNUM);

  displaced_write_reg (regs, dsc, 0, pc_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, imm12, CANNOT_WRITE_PC);
  dsc->u.preload.immed = 0;

  /* {pli/pld} [r0, r1] */
  dsc->modinsn[0] = insn1 & 0xfff0;
  dsc->modinsn[1] = 0xf001;
  dsc->numinsns = 2;

  dsc->cleanup = &cleanup_preload;
  return 0;
}

/* Preload instructions with register offset.  */

static void
install_preload_reg(struct gdbarch *gdbarch, struct regcache *regs,
		    struct displaced_step_closure *dsc, unsigned int rn,
		    unsigned int rm)
{
  ULONGEST rn_val, rm_val;

  /* Preload register-offset instructions:

     {pli/pld} [rn, rm {, shift}]
     ->
     {pli/pld} [r0, r1 {, shift}].  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[1] = displaced_read_reg (regs, dsc, 1);
  rn_val = displaced_read_reg (regs, dsc, rn);
  rm_val = displaced_read_reg (regs, dsc, rm);
  displaced_write_reg (regs, dsc, 0, rn_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, rm_val, CANNOT_WRITE_PC);
  dsc->u.preload.immed = 0;

  dsc->cleanup = &cleanup_preload;
}

static int
arm_copy_preload_reg (struct gdbarch *gdbarch, uint32_t insn,
		      struct regcache *regs,
		      struct displaced_step_closure *dsc)
{
  unsigned int rn = bits (insn, 16, 19);
  unsigned int rm = bits (insn, 0, 3);


  if (!insn_references_pc (insn, 0x000f000ful))
    return arm_copy_unmodified (gdbarch, insn, "preload reg", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying preload insn %.8lx\n",
			(unsigned long) insn);

  dsc->modinsn[0] = (insn & 0xfff0fff0) | 0x1;

  install_preload_reg (gdbarch, regs, dsc, rn, rm);
  return 0;
}

/* Copy/cleanup coprocessor load and store instructions.  */

static void
cleanup_copro_load_store (struct gdbarch *gdbarch,
			  struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  ULONGEST rn_val = displaced_read_reg (regs, dsc, 0);

  displaced_write_reg (regs, dsc, 0, dsc->tmp[0], CANNOT_WRITE_PC);

  if (dsc->u.ldst.writeback)
    displaced_write_reg (regs, dsc, dsc->u.ldst.rn, rn_val, LOAD_WRITE_PC);
}

static void
install_copro_load_store (struct gdbarch *gdbarch, struct regcache *regs,
			  struct displaced_step_closure *dsc,
			  int writeback, unsigned int rn)
{
  ULONGEST rn_val;

  /* Coprocessor load/store instructions:

     {stc/stc2} [<Rn>, #+/-imm]  (and other immediate addressing modes)
     ->
     {stc/stc2} [r0, #+/-imm].

     ldc/ldc2 are handled identically.  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  rn_val = displaced_read_reg (regs, dsc, rn);
  /* PC should be 4-byte aligned.  */
  rn_val = rn_val & 0xfffffffc;
  displaced_write_reg (regs, dsc, 0, rn_val, CANNOT_WRITE_PC);

  dsc->u.ldst.writeback = writeback;
  dsc->u.ldst.rn = rn;

  dsc->cleanup = &cleanup_copro_load_store;
}

static int
arm_copy_copro_load_store (struct gdbarch *gdbarch, uint32_t insn,
			   struct regcache *regs,
			   struct displaced_step_closure *dsc)
{
  unsigned int rn = bits (insn, 16, 19);

  if (!insn_references_pc (insn, 0x000f0000ul))
    return arm_copy_unmodified (gdbarch, insn, "copro load/store", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying coprocessor "
			"load/store insn %.8lx\n", (unsigned long) insn);

  dsc->modinsn[0] = insn & 0xfff0ffff;

  install_copro_load_store (gdbarch, regs, dsc, bit (insn, 25), rn);

  return 0;
}

static int
thumb2_copy_copro_load_store (struct gdbarch *gdbarch, uint16_t insn1,
			      uint16_t insn2, struct regcache *regs,
			      struct displaced_step_closure *dsc)
{
  unsigned int rn = bits (insn1, 0, 3);

  if (rn != ARM_PC_REGNUM)
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					"copro load/store", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying coprocessor "
			"load/store insn %.4x%.4x\n", insn1, insn2);

  dsc->modinsn[0] = insn1 & 0xfff0;
  dsc->modinsn[1] = insn2;
  dsc->numinsns = 2;

  /* This function is called for copying instruction LDC/LDC2/VLDR, which
     doesn't support writeback, so pass 0.  */
  install_copro_load_store (gdbarch, regs, dsc, 0, rn);

  return 0;
}

/* Clean up branch instructions (actually perform the branch, by setting
   PC).  */

static void
cleanup_branch (struct gdbarch *gdbarch, struct regcache *regs,
		struct displaced_step_closure *dsc)
{
  uint32_t status = displaced_read_reg (regs, dsc, ARM_PS_REGNUM);
  int branch_taken = condition_true (dsc->u.branch.cond, status);
  enum pc_write_style write_pc = dsc->u.branch.exchange
				 ? BX_WRITE_PC : BRANCH_WRITE_PC;

  if (!branch_taken)
    return;

  if (dsc->u.branch.link)
    {
      /* The value of LR should be the next insn of current one.  In order
       not to confuse logic hanlding later insn `bx lr', if current insn mode
       is Thumb, the bit 0 of LR value should be set to 1.  */
      ULONGEST next_insn_addr = dsc->insn_addr + dsc->insn_size;

      if (dsc->is_thumb)
	next_insn_addr |= 0x1;

      displaced_write_reg (regs, dsc, ARM_LR_REGNUM, next_insn_addr,
			   CANNOT_WRITE_PC);
    }

  displaced_write_reg (regs, dsc, ARM_PC_REGNUM, dsc->u.branch.dest, write_pc);
}

/* Copy B/BL/BLX instructions with immediate destinations.  */

static void
install_b_bl_blx (struct gdbarch *gdbarch, struct regcache *regs,
		  struct displaced_step_closure *dsc,
		  unsigned int cond, int exchange, int link, long offset)
{
  /* Implement "BL<cond> <label>" as:

     Preparation: cond <- instruction condition
     Insn: mov r0, r0  (nop)
     Cleanup: if (condition true) { r14 <- pc; pc <- label }.

     B<cond> similar, but don't set r14 in cleanup.  */

  dsc->u.branch.cond = cond;
  dsc->u.branch.link = link;
  dsc->u.branch.exchange = exchange;

  dsc->u.branch.dest = dsc->insn_addr;
  if (link && exchange)
    /* For BLX, offset is computed from the Align (PC, 4).  */
    dsc->u.branch.dest = dsc->u.branch.dest & 0xfffffffc;

  if (dsc->is_thumb)
    dsc->u.branch.dest += 4 + offset;
  else
    dsc->u.branch.dest += 8 + offset;

  dsc->cleanup = &cleanup_branch;
}
static int
arm_copy_b_bl_blx (struct gdbarch *gdbarch, uint32_t insn,
		   struct regcache *regs, struct displaced_step_closure *dsc)
{
  unsigned int cond = bits (insn, 28, 31);
  int exchange = (cond == 0xf);
  int link = exchange || bit (insn, 24);
  long offset;

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying %s immediate insn "
			"%.8lx\n", (exchange) ? "blx" : (link) ? "bl" : "b",
			(unsigned long) insn);
  if (exchange)
    /* For BLX, set bit 0 of the destination.  The cleanup_branch function will
       then arrange the switch into Thumb mode.  */
    offset = (bits (insn, 0, 23) << 2) | (bit (insn, 24) << 1) | 1;
  else
    offset = bits (insn, 0, 23) << 2;

  if (bit (offset, 25))
    offset = offset | ~0x3ffffff;

  dsc->modinsn[0] = ARM_NOP;

  install_b_bl_blx (gdbarch, regs, dsc, cond, exchange, link, offset);
  return 0;
}

static int
thumb2_copy_b_bl_blx (struct gdbarch *gdbarch, uint16_t insn1,
		      uint16_t insn2, struct regcache *regs,
		      struct displaced_step_closure *dsc)
{
  int link = bit (insn2, 14);
  int exchange = link && !bit (insn2, 12);
  int cond = INST_AL;
  long offset = 0;
  int j1 = bit (insn2, 13);
  int j2 = bit (insn2, 11);
  int s = sbits (insn1, 10, 10);
  int i1 = !(j1 ^ bit (insn1, 10));
  int i2 = !(j2 ^ bit (insn1, 10));

  if (!link && !exchange) /* B */
    {
      offset = (bits (insn2, 0, 10) << 1);
      if (bit (insn2, 12)) /* Encoding T4 */
	{
	  offset |= (bits (insn1, 0, 9) << 12)
	    | (i2 << 22)
	    | (i1 << 23)
	    | (s << 24);
	  cond = INST_AL;
	}
      else /* Encoding T3 */
	{
	  offset |= (bits (insn1, 0, 5) << 12)
	    | (j1 << 18)
	    | (j2 << 19)
	    | (s << 20);
	  cond = bits (insn1, 6, 9);
	}
    }
  else
    {
      offset = (bits (insn1, 0, 9) << 12);
      offset |= ((i2 << 22) | (i1 << 23) | (s << 24));
      offset |= exchange ?
	(bits (insn2, 1, 10) << 2) : (bits (insn2, 0, 10) << 1);
    }

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying %s insn "
			"%.4x %.4x with offset %.8lx\n",
			link ? (exchange) ? "blx" : "bl" : "b",
			insn1, insn2, offset);

  dsc->modinsn[0] = THUMB_NOP;

  install_b_bl_blx (gdbarch, regs, dsc, cond, exchange, link, offset);
  return 0;
}

/* Copy B Thumb instructions.  */
static int
thumb_copy_b (struct gdbarch *gdbarch, unsigned short insn,
	      struct displaced_step_closure *dsc)
{
  unsigned int cond = 0;
  int offset = 0;
  unsigned short bit_12_15 = bits (insn, 12, 15);
  CORE_ADDR from = dsc->insn_addr;

  if (bit_12_15 == 0xd)
    {
      /* offset = SignExtend (imm8:0, 32) */
      offset = sbits ((insn << 1), 0, 8);
      cond = bits (insn, 8, 11);
    }
  else if (bit_12_15 == 0xe) /* Encoding T2 */
    {
      offset = sbits ((insn << 1), 0, 11);
      cond = INST_AL;
    }

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying b immediate insn %.4x "
			"with offset %d\n", insn, offset);

  dsc->u.branch.cond = cond;
  dsc->u.branch.link = 0;
  dsc->u.branch.exchange = 0;
  dsc->u.branch.dest = from + 4 + offset;

  dsc->modinsn[0] = THUMB_NOP;

  dsc->cleanup = &cleanup_branch;

  return 0;
}

/* Copy BX/BLX with register-specified destinations.  */

static void
install_bx_blx_reg (struct gdbarch *gdbarch, struct regcache *regs,
		    struct displaced_step_closure *dsc, int link,
		    unsigned int cond, unsigned int rm)
{
  /* Implement {BX,BLX}<cond> <reg>" as:

     Preparation: cond <- instruction condition
     Insn: mov r0, r0 (nop)
     Cleanup: if (condition true) { r14 <- pc; pc <- dest; }.

     Don't set r14 in cleanup for BX.  */

  dsc->u.branch.dest = displaced_read_reg (regs, dsc, rm);

  dsc->u.branch.cond = cond;
  dsc->u.branch.link = link;

  dsc->u.branch.exchange = 1;

  dsc->cleanup = &cleanup_branch;
}

static int
arm_copy_bx_blx_reg (struct gdbarch *gdbarch, uint32_t insn,
		     struct regcache *regs, struct displaced_step_closure *dsc)
{
  unsigned int cond = bits (insn, 28, 31);
  /* BX:  x12xxx1x
     BLX: x12xxx3x.  */
  int link = bit (insn, 5);
  unsigned int rm = bits (insn, 0, 3);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying insn %.8lx",
			(unsigned long) insn);

  dsc->modinsn[0] = ARM_NOP;

  install_bx_blx_reg (gdbarch, regs, dsc, link, cond, rm);
  return 0;
}

static int
thumb_copy_bx_blx_reg (struct gdbarch *gdbarch, uint16_t insn,
		       struct regcache *regs,
		       struct displaced_step_closure *dsc)
{
  int link = bit (insn, 7);
  unsigned int rm = bits (insn, 3, 6);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying insn %.4x",
			(unsigned short) insn);

  dsc->modinsn[0] = THUMB_NOP;

  install_bx_blx_reg (gdbarch, regs, dsc, link, INST_AL, rm);

  return 0;
}


/* Copy/cleanup arithmetic/logic instruction with immediate RHS.  */

static void
cleanup_alu_imm (struct gdbarch *gdbarch,
		 struct regcache *regs, struct displaced_step_closure *dsc)
{
  ULONGEST rd_val = displaced_read_reg (regs, dsc, 0);
  displaced_write_reg (regs, dsc, 0, dsc->tmp[0], CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, dsc->tmp[1], CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, dsc->rd, rd_val, ALU_WRITE_PC);
}

static int
arm_copy_alu_imm (struct gdbarch *gdbarch, uint32_t insn, struct regcache *regs,
		  struct displaced_step_closure *dsc)
{
  unsigned int rn = bits (insn, 16, 19);
  unsigned int rd = bits (insn, 12, 15);
  unsigned int op = bits (insn, 21, 24);
  int is_mov = (op == 0xd);
  ULONGEST rd_val, rn_val;

  if (!insn_references_pc (insn, 0x000ff000ul))
    return arm_copy_unmodified (gdbarch, insn, "ALU immediate", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying immediate %s insn "
			"%.8lx\n", is_mov ? "move" : "ALU",
			(unsigned long) insn);

  /* Instruction is of form:

     <op><cond> rd, [rn,] #imm

     Rewrite as:

     Preparation: tmp1, tmp2 <- r0, r1;
		  r0, r1 <- rd, rn
     Insn: <op><cond> r0, r1, #imm
     Cleanup: rd <- r0; r0 <- tmp1; r1 <- tmp2
  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[1] = displaced_read_reg (regs, dsc, 1);
  rn_val = displaced_read_reg (regs, dsc, rn);
  rd_val = displaced_read_reg (regs, dsc, rd);
  displaced_write_reg (regs, dsc, 0, rd_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, rn_val, CANNOT_WRITE_PC);
  dsc->rd = rd;

  if (is_mov)
    dsc->modinsn[0] = insn & 0xfff00fff;
  else
    dsc->modinsn[0] = (insn & 0xfff00fff) | 0x10000;

  dsc->cleanup = &cleanup_alu_imm;

  return 0;
}

static int
thumb2_copy_alu_imm (struct gdbarch *gdbarch, uint16_t insn1,
		     uint16_t insn2, struct regcache *regs,
		     struct displaced_step_closure *dsc)
{
  unsigned int op = bits (insn1, 5, 8);
  unsigned int rn, rm, rd;
  ULONGEST rd_val, rn_val;

  rn = bits (insn1, 0, 3); /* Rn */
  rm = bits (insn2, 0, 3); /* Rm */
  rd = bits (insn2, 8, 11); /* Rd */

  /* This routine is only called for instruction MOV.  */
  gdb_assert (op == 0x2 && rn == 0xf);

  if (rm != ARM_PC_REGNUM && rd != ARM_PC_REGNUM)
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2, "ALU imm", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying reg %s insn %.4x%.4x\n",
			"ALU", insn1, insn2);

  /* Instruction is of form:

     <op><cond> rd, [rn,] #imm

     Rewrite as:

     Preparation: tmp1, tmp2 <- r0, r1;
		  r0, r1 <- rd, rn
     Insn: <op><cond> r0, r1, #imm
     Cleanup: rd <- r0; r0 <- tmp1; r1 <- tmp2
  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[1] = displaced_read_reg (regs, dsc, 1);
  rn_val = displaced_read_reg (regs, dsc, rn);
  rd_val = displaced_read_reg (regs, dsc, rd);
  displaced_write_reg (regs, dsc, 0, rd_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, rn_val, CANNOT_WRITE_PC);
  dsc->rd = rd;

  dsc->modinsn[0] = insn1;
  dsc->modinsn[1] = ((insn2 & 0xf0f0) | 0x1);
  dsc->numinsns = 2;

  dsc->cleanup = &cleanup_alu_imm;

  return 0;
}

/* Copy/cleanup arithmetic/logic insns with register RHS.  */

static void
cleanup_alu_reg (struct gdbarch *gdbarch,
		 struct regcache *regs, struct displaced_step_closure *dsc)
{
  ULONGEST rd_val;
  int i;

  rd_val = displaced_read_reg (regs, dsc, 0);

  for (i = 0; i < 3; i++)
    displaced_write_reg (regs, dsc, i, dsc->tmp[i], CANNOT_WRITE_PC);

  displaced_write_reg (regs, dsc, dsc->rd, rd_val, ALU_WRITE_PC);
}

static void
install_alu_reg (struct gdbarch *gdbarch, struct regcache *regs,
		 struct displaced_step_closure *dsc,
		 unsigned int rd, unsigned int rn, unsigned int rm)
{
  ULONGEST rd_val, rn_val, rm_val;

  /* Instruction is of form:

     <op><cond> rd, [rn,] rm [, <shift>]

     Rewrite as:

     Preparation: tmp1, tmp2, tmp3 <- r0, r1, r2;
		  r0, r1, r2 <- rd, rn, rm
     Insn: <op><cond> r0, r1, r2 [, <shift>]
     Cleanup: rd <- r0; r0, r1, r2 <- tmp1, tmp2, tmp3
  */

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[1] = displaced_read_reg (regs, dsc, 1);
  dsc->tmp[2] = displaced_read_reg (regs, dsc, 2);
  rd_val = displaced_read_reg (regs, dsc, rd);
  rn_val = displaced_read_reg (regs, dsc, rn);
  rm_val = displaced_read_reg (regs, dsc, rm);
  displaced_write_reg (regs, dsc, 0, rd_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, rn_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 2, rm_val, CANNOT_WRITE_PC);
  dsc->rd = rd;

  dsc->cleanup = &cleanup_alu_reg;
}

static int
arm_copy_alu_reg (struct gdbarch *gdbarch, uint32_t insn, struct regcache *regs,
		  struct displaced_step_closure *dsc)
{
  unsigned int op = bits (insn, 21, 24);
  int is_mov = (op == 0xd);

  if (!insn_references_pc (insn, 0x000ff00ful))
    return arm_copy_unmodified (gdbarch, insn, "ALU reg", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying reg %s insn %.8lx\n",
			is_mov ? "move" : "ALU", (unsigned long) insn);

  if (is_mov)
    dsc->modinsn[0] = (insn & 0xfff00ff0) | 0x2;
  else
    dsc->modinsn[0] = (insn & 0xfff00ff0) | 0x10002;

  install_alu_reg (gdbarch, regs, dsc, bits (insn, 12, 15), bits (insn, 16, 19),
		   bits (insn, 0, 3));
  return 0;
}

static int
thumb_copy_alu_reg (struct gdbarch *gdbarch, uint16_t insn,
		    struct regcache *regs,
		    struct displaced_step_closure *dsc)
{
  unsigned rn, rm, rd;

  rd = bits (insn, 3, 6);
  rn = (bit (insn, 7) << 3) | bits (insn, 0, 2);
  rm = 2;

  if (rd != ARM_PC_REGNUM && rn != ARM_PC_REGNUM)
    return thumb_copy_unmodified_16bit (gdbarch, insn, "ALU reg", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying reg %s insn %.4x\n",
			"ALU", (unsigned short) insn);

  dsc->modinsn[0] = ((insn & 0xff00) | 0x08);

  install_alu_reg (gdbarch, regs, dsc, rd, rn, rm);

  return 0;
}

/* Cleanup/copy arithmetic/logic insns with shifted register RHS.  */

static void
cleanup_alu_shifted_reg (struct gdbarch *gdbarch,
			 struct regcache *regs,
			 struct displaced_step_closure *dsc)
{
  ULONGEST rd_val = displaced_read_reg (regs, dsc, 0);
  int i;

  for (i = 0; i < 4; i++)
    displaced_write_reg (regs, dsc, i, dsc->tmp[i], CANNOT_WRITE_PC);

  displaced_write_reg (regs, dsc, dsc->rd, rd_val, ALU_WRITE_PC);
}

static void
install_alu_shifted_reg (struct gdbarch *gdbarch, struct regcache *regs,
			 struct displaced_step_closure *dsc,
			 unsigned int rd, unsigned int rn, unsigned int rm,
			 unsigned rs)
{
  int i;
  ULONGEST rd_val, rn_val, rm_val, rs_val;

  /* Instruction is of form:

     <op><cond> rd, [rn,] rm, <shift> rs

     Rewrite as:

     Preparation: tmp1, tmp2, tmp3, tmp4 <- r0, r1, r2, r3
		  r0, r1, r2, r3 <- rd, rn, rm, rs
     Insn: <op><cond> r0, r1, r2, <shift> r3
     Cleanup: tmp5 <- r0
	      r0, r1, r2, r3 <- tmp1, tmp2, tmp3, tmp4
	      rd <- tmp5
  */

  for (i = 0; i < 4; i++)
    dsc->tmp[i] = displaced_read_reg (regs, dsc, i);

  rd_val = displaced_read_reg (regs, dsc, rd);
  rn_val = displaced_read_reg (regs, dsc, rn);
  rm_val = displaced_read_reg (regs, dsc, rm);
  rs_val = displaced_read_reg (regs, dsc, rs);
  displaced_write_reg (regs, dsc, 0, rd_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 1, rn_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 2, rm_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 3, rs_val, CANNOT_WRITE_PC);
  dsc->rd = rd;
  dsc->cleanup = &cleanup_alu_shifted_reg;
}

static int
arm_copy_alu_shifted_reg (struct gdbarch *gdbarch, uint32_t insn,
			  struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  unsigned int op = bits (insn, 21, 24);
  int is_mov = (op == 0xd);
  unsigned int rd, rn, rm, rs;

  if (!insn_references_pc (insn, 0x000fff0ful))
    return arm_copy_unmodified (gdbarch, insn, "ALU shifted reg", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying shifted reg %s insn "
			"%.8lx\n", is_mov ? "move" : "ALU",
			(unsigned long) insn);

  rn = bits (insn, 16, 19);
  rm = bits (insn, 0, 3);
  rs = bits (insn, 8, 11);
  rd = bits (insn, 12, 15);

  if (is_mov)
    dsc->modinsn[0] = (insn & 0xfff000f0) | 0x302;
  else
    dsc->modinsn[0] = (insn & 0xfff000f0) | 0x10302;

  install_alu_shifted_reg (gdbarch, regs, dsc, rd, rn, rm, rs);

  return 0;
}

/* Clean up load instructions.  */

static void
cleanup_load (struct gdbarch *gdbarch, struct regcache *regs,
	      struct displaced_step_closure *dsc)
{
  ULONGEST rt_val, rt_val2 = 0, rn_val;

  rt_val = displaced_read_reg (regs, dsc, 0);
  if (dsc->u.ldst.xfersize == 8)
    rt_val2 = displaced_read_reg (regs, dsc, 1);
  rn_val = displaced_read_reg (regs, dsc, 2);

  displaced_write_reg (regs, dsc, 0, dsc->tmp[0], CANNOT_WRITE_PC);
  if (dsc->u.ldst.xfersize > 4)
    displaced_write_reg (regs, dsc, 1, dsc->tmp[1], CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 2, dsc->tmp[2], CANNOT_WRITE_PC);
  if (!dsc->u.ldst.immed)
    displaced_write_reg (regs, dsc, 3, dsc->tmp[3], CANNOT_WRITE_PC);

  /* Handle register writeback.  */
  if (dsc->u.ldst.writeback)
    displaced_write_reg (regs, dsc, dsc->u.ldst.rn, rn_val, CANNOT_WRITE_PC);
  /* Put result in right place.  */
  displaced_write_reg (regs, dsc, dsc->rd, rt_val, LOAD_WRITE_PC);
  if (dsc->u.ldst.xfersize == 8)
    displaced_write_reg (regs, dsc, dsc->rd + 1, rt_val2, LOAD_WRITE_PC);
}

/* Clean up store instructions.  */

static void
cleanup_store (struct gdbarch *gdbarch, struct regcache *regs,
	       struct displaced_step_closure *dsc)
{
  ULONGEST rn_val = displaced_read_reg (regs, dsc, 2);

  displaced_write_reg (regs, dsc, 0, dsc->tmp[0], CANNOT_WRITE_PC);
  if (dsc->u.ldst.xfersize > 4)
    displaced_write_reg (regs, dsc, 1, dsc->tmp[1], CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 2, dsc->tmp[2], CANNOT_WRITE_PC);
  if (!dsc->u.ldst.immed)
    displaced_write_reg (regs, dsc, 3, dsc->tmp[3], CANNOT_WRITE_PC);
  if (!dsc->u.ldst.restore_r4)
    displaced_write_reg (regs, dsc, 4, dsc->tmp[4], CANNOT_WRITE_PC);

  /* Writeback.  */
  if (dsc->u.ldst.writeback)
    displaced_write_reg (regs, dsc, dsc->u.ldst.rn, rn_val, CANNOT_WRITE_PC);
}

/* Copy "extra" load/store instructions.  These are halfword/doubleword
   transfers, which have a different encoding to byte/word transfers.  */

static int
arm_copy_extra_ld_st (struct gdbarch *gdbarch, uint32_t insn, int unpriveleged,
		      struct regcache *regs, struct displaced_step_closure *dsc)
{
  unsigned int op1 = bits (insn, 20, 24);
  unsigned int op2 = bits (insn, 5, 6);
  unsigned int rt = bits (insn, 12, 15);
  unsigned int rn = bits (insn, 16, 19);
  unsigned int rm = bits (insn, 0, 3);
  char load[12]     = {0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1};
  char bytesize[12] = {2, 2, 2, 2, 8, 1, 8, 1, 8, 2, 8, 2};
  int immed = (op1 & 0x4) != 0;
  int opcode;
  ULONGEST rt_val, rt_val2 = 0, rn_val, rm_val = 0;

  if (!insn_references_pc (insn, 0x000ff00ful))
    return arm_copy_unmodified (gdbarch, insn, "extra load/store", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying %sextra load/store "
			"insn %.8lx\n", unpriveleged ? "unpriveleged " : "",
			(unsigned long) insn);

  opcode = ((op2 << 2) | (op1 & 0x1) | ((op1 & 0x4) >> 1)) - 4;

  if (opcode < 0)
    internal_error (__FILE__, __LINE__,
		    _("copy_extra_ld_st: instruction decode error"));

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[1] = displaced_read_reg (regs, dsc, 1);
  dsc->tmp[2] = displaced_read_reg (regs, dsc, 2);
  if (!immed)
    dsc->tmp[3] = displaced_read_reg (regs, dsc, 3);

  rt_val = displaced_read_reg (regs, dsc, rt);
  if (bytesize[opcode] == 8)
    rt_val2 = displaced_read_reg (regs, dsc, rt + 1);
  rn_val = displaced_read_reg (regs, dsc, rn);
  if (!immed)
    rm_val = displaced_read_reg (regs, dsc, rm);

  displaced_write_reg (regs, dsc, 0, rt_val, CANNOT_WRITE_PC);
  if (bytesize[opcode] == 8)
    displaced_write_reg (regs, dsc, 1, rt_val2, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 2, rn_val, CANNOT_WRITE_PC);
  if (!immed)
    displaced_write_reg (regs, dsc, 3, rm_val, CANNOT_WRITE_PC);

  dsc->rd = rt;
  dsc->u.ldst.xfersize = bytesize[opcode];
  dsc->u.ldst.rn = rn;
  dsc->u.ldst.immed = immed;
  dsc->u.ldst.writeback = bit (insn, 24) == 0 || bit (insn, 21) != 0;
  dsc->u.ldst.restore_r4 = 0;

  if (immed)
    /* {ldr,str}<width><cond> rt, [rt2,] [rn, #imm]
	->
       {ldr,str}<width><cond> r0, [r1,] [r2, #imm].  */
    dsc->modinsn[0] = (insn & 0xfff00fff) | 0x20000;
  else
    /* {ldr,str}<width><cond> rt, [rt2,] [rn, +/-rm]
	->
       {ldr,str}<width><cond> r0, [r1,] [r2, +/-r3].  */
    dsc->modinsn[0] = (insn & 0xfff00ff0) | 0x20003;

  dsc->cleanup = load[opcode] ? &cleanup_load : &cleanup_store;

  return 0;
}

/* Copy byte/half word/word loads and stores.  */

static void
install_load_store (struct gdbarch *gdbarch, struct regcache *regs,
		    struct displaced_step_closure *dsc, int load,
		    int immed, int writeback, int size, int usermode,
		    int rt, int rm, int rn)
{
  ULONGEST rt_val, rn_val, rm_val = 0;

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[2] = displaced_read_reg (regs, dsc, 2);
  if (!immed)
    dsc->tmp[3] = displaced_read_reg (regs, dsc, 3);
  if (!load)
    dsc->tmp[4] = displaced_read_reg (regs, dsc, 4);

  rt_val = displaced_read_reg (regs, dsc, rt);
  rn_val = displaced_read_reg (regs, dsc, rn);
  if (!immed)
    rm_val = displaced_read_reg (regs, dsc, rm);

  displaced_write_reg (regs, dsc, 0, rt_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 2, rn_val, CANNOT_WRITE_PC);
  if (!immed)
    displaced_write_reg (regs, dsc, 3, rm_val, CANNOT_WRITE_PC);
  dsc->rd = rt;
  dsc->u.ldst.xfersize = size;
  dsc->u.ldst.rn = rn;
  dsc->u.ldst.immed = immed;
  dsc->u.ldst.writeback = writeback;

  /* To write PC we can do:

     Before this sequence of instructions:
     r0 is the PC value got from displaced_read_reg, so r0 = from + 8;
     r2 is the Rn value got from dispalced_read_reg.

     Insn1: push {pc} Write address of STR instruction + offset on stack
     Insn2: pop  {r4} Read it back from stack, r4 = addr(Insn1) + offset
     Insn3: sub r4, r4, pc   r4 = addr(Insn1) + offset - pc
                                = addr(Insn1) + offset - addr(Insn3) - 8
                                = offset - 16
     Insn4: add r4, r4, #8   r4 = offset - 8
     Insn5: add r0, r0, r4   r0 = from + 8 + offset - 8
                                = from + offset
     Insn6: str r0, [r2, #imm] (or str r0, [r2, r3])

     Otherwise we don't know what value to write for PC, since the offset is
     architecture-dependent (sometimes PC+8, sometimes PC+12).  More details
     of this can be found in Section "Saving from r15" in
     http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.dui0204g/Cihbjifh.html */

  dsc->cleanup = load ? &cleanup_load : &cleanup_store;
}


static int
thumb2_copy_load_literal (struct gdbarch *gdbarch, uint16_t insn1,
			  uint16_t insn2, struct regcache *regs,
			  struct displaced_step_closure *dsc, int size)
{
  unsigned int u_bit = bit (insn1, 7);
  unsigned int rt = bits (insn2, 12, 15);
  int imm12 = bits (insn2, 0, 11);
  ULONGEST pc_val;

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying ldr pc (0x%x) R%d %c imm12 %.4x\n",
			(unsigned int) dsc->insn_addr, rt, u_bit ? '+' : '-',
			imm12);

  if (!u_bit)
    imm12 = -1 * imm12;

  /* Rewrite instruction LDR Rt imm12 into:

     Prepare: tmp[0] <- r0, tmp[1] <- r2, tmp[2] <- r3, r2 <- pc, r3 <- imm12

     LDR R0, R2, R3,

     Cleanup: rt <- r0, r0 <- tmp[0], r2 <- tmp[1], r3 <- tmp[2].  */


  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[2] = displaced_read_reg (regs, dsc, 2);
  dsc->tmp[3] = displaced_read_reg (regs, dsc, 3);

  pc_val = displaced_read_reg (regs, dsc, ARM_PC_REGNUM);

  pc_val = pc_val & 0xfffffffc;

  displaced_write_reg (regs, dsc, 2, pc_val, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 3, imm12, CANNOT_WRITE_PC);

  dsc->rd = rt;

  dsc->u.ldst.xfersize = size;
  dsc->u.ldst.immed = 0;
  dsc->u.ldst.writeback = 0;
  dsc->u.ldst.restore_r4 = 0;

  /* LDR R0, R2, R3 */
  dsc->modinsn[0] = 0xf852;
  dsc->modinsn[1] = 0x3;
  dsc->numinsns = 2;

  dsc->cleanup = &cleanup_load;

  return 0;
}

static int
thumb2_copy_load_reg_imm (struct gdbarch *gdbarch, uint16_t insn1,
			  uint16_t insn2, struct regcache *regs,
			  struct displaced_step_closure *dsc,
			  int writeback, int immed)
{
  unsigned int rt = bits (insn2, 12, 15);
  unsigned int rn = bits (insn1, 0, 3);
  unsigned int rm = bits (insn2, 0, 3);  /* Only valid if !immed.  */
  /* In LDR (register), there is also a register Rm, which is not allowed to
     be PC, so we don't have to check it.  */

  if (rt != ARM_PC_REGNUM && rn != ARM_PC_REGNUM)
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2, "load",
					dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying ldr r%d [r%d] insn %.4x%.4x\n",
			 rt, rn, insn1, insn2);

  install_load_store (gdbarch, regs, dsc, 1, immed, writeback, 4,
		      0, rt, rm, rn);

  dsc->u.ldst.restore_r4 = 0;

  if (immed)
    /* ldr[b]<cond> rt, [rn, #imm], etc.
       ->
       ldr[b]<cond> r0, [r2, #imm].  */
    {
      dsc->modinsn[0] = (insn1 & 0xfff0) | 0x2;
      dsc->modinsn[1] = insn2 & 0x0fff;
    }
  else
    /* ldr[b]<cond> rt, [rn, rm], etc.
       ->
       ldr[b]<cond> r0, [r2, r3].  */
    {
      dsc->modinsn[0] = (insn1 & 0xfff0) | 0x2;
      dsc->modinsn[1] = (insn2 & 0x0ff0) | 0x3;
    }

  dsc->numinsns = 2;

  return 0;
}


static int
arm_copy_ldr_str_ldrb_strb (struct gdbarch *gdbarch, uint32_t insn,
			    struct regcache *regs,
			    struct displaced_step_closure *dsc,
			    int load, int size, int usermode)
{
  int immed = !bit (insn, 25);
  int writeback = (bit (insn, 24) == 0 || bit (insn, 21) != 0);
  unsigned int rt = bits (insn, 12, 15);
  unsigned int rn = bits (insn, 16, 19);
  unsigned int rm = bits (insn, 0, 3);  /* Only valid if !immed.  */

  if (!insn_references_pc (insn, 0x000ff00ful))
    return arm_copy_unmodified (gdbarch, insn, "load/store", dsc);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying %s%s r%d [r%d] insn %.8lx\n",
			load ? (size == 1 ? "ldrb" : "ldr")
			     : (size == 1 ? "strb" : "str"), usermode ? "t" : "",
			rt, rn,
			(unsigned long) insn);

  install_load_store (gdbarch, regs, dsc, load, immed, writeback, size,
		      usermode, rt, rm, rn);

  if (load || rt != ARM_PC_REGNUM)
    {
      dsc->u.ldst.restore_r4 = 0;

      if (immed)
	/* {ldr,str}[b]<cond> rt, [rn, #imm], etc.
	   ->
	   {ldr,str}[b]<cond> r0, [r2, #imm].  */
	dsc->modinsn[0] = (insn & 0xfff00fff) | 0x20000;
      else
	/* {ldr,str}[b]<cond> rt, [rn, rm], etc.
	   ->
	   {ldr,str}[b]<cond> r0, [r2, r3].  */
	dsc->modinsn[0] = (insn & 0xfff00ff0) | 0x20003;
    }
  else
    {
      /* We need to use r4 as scratch.  Make sure it's restored afterwards.  */
      dsc->u.ldst.restore_r4 = 1;
      dsc->modinsn[0] = 0xe92d8000;  /* push {pc} */
      dsc->modinsn[1] = 0xe8bd0010;  /* pop  {r4} */
      dsc->modinsn[2] = 0xe044400f;  /* sub r4, r4, pc.  */
      dsc->modinsn[3] = 0xe2844008;  /* add r4, r4, #8.  */
      dsc->modinsn[4] = 0xe0800004;  /* add r0, r0, r4.  */

      /* As above.  */
      if (immed)
	dsc->modinsn[5] = (insn & 0xfff00fff) | 0x20000;
      else
	dsc->modinsn[5] = (insn & 0xfff00ff0) | 0x20003;

      dsc->numinsns = 6;
    }

  dsc->cleanup = load ? &cleanup_load : &cleanup_store;

  return 0;
}

/* Cleanup LDM instructions with fully-populated register list.  This is an
   unfortunate corner case: it's impossible to implement correctly by modifying
   the instruction.  The issue is as follows: we have an instruction,

   ldm rN, {r0-r15}

   which we must rewrite to avoid loading PC.  A possible solution would be to
   do the load in two halves, something like (with suitable cleanup
   afterwards):

   mov r8, rN
   ldm[id][ab] r8!, {r0-r7}
   str r7, <temp>
   ldm[id][ab] r8, {r7-r14}
   <bkpt>

   but at present there's no suitable place for <temp>, since the scratch space
   is overwritten before the cleanup routine is called.  For now, we simply
   emulate the instruction.  */

static void
cleanup_block_load_all (struct gdbarch *gdbarch, struct regcache *regs,
			struct displaced_step_closure *dsc)
{
  int inc = dsc->u.block.increment;
  int bump_before = dsc->u.block.before ? (inc ? 4 : -4) : 0;
  int bump_after = dsc->u.block.before ? 0 : (inc ? 4 : -4);
  uint32_t regmask = dsc->u.block.regmask;
  int regno = inc ? 0 : 15;
  CORE_ADDR xfer_addr = dsc->u.block.xfer_addr;
  int exception_return = dsc->u.block.load && dsc->u.block.user
			 && (regmask & 0x8000) != 0;
  uint32_t status = displaced_read_reg (regs, dsc, ARM_PS_REGNUM);
  int do_transfer = condition_true (dsc->u.block.cond, status);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  if (!do_transfer)
    return;

  /* If the instruction is ldm rN, {...pc}^, I don't think there's anything
     sensible we can do here.  Complain loudly.  */
  if (exception_return)
    error (_("Cannot single-step exception return"));

  /* We don't handle any stores here for now.  */
  gdb_assert (dsc->u.block.load != 0);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: emulating block transfer: "
			"%s %s %s\n", dsc->u.block.load ? "ldm" : "stm",
			dsc->u.block.increment ? "inc" : "dec",
			dsc->u.block.before ? "before" : "after");

  while (regmask)
    {
      uint32_t memword;

      if (inc)
	while (regno <= ARM_PC_REGNUM && (regmask & (1 << regno)) == 0)
	  regno++;
      else
	while (regno >= 0 && (regmask & (1 << regno)) == 0)
	  regno--;

      xfer_addr += bump_before;

      memword = read_memory_unsigned_integer (xfer_addr, 4, byte_order);
      displaced_write_reg (regs, dsc, regno, memword, LOAD_WRITE_PC);

      xfer_addr += bump_after;

      regmask &= ~(1 << regno);
    }

  if (dsc->u.block.writeback)
    displaced_write_reg (regs, dsc, dsc->u.block.rn, xfer_addr,
			 CANNOT_WRITE_PC);
}

/* Clean up an STM which included the PC in the register list.  */

static void
cleanup_block_store_pc (struct gdbarch *gdbarch, struct regcache *regs,
			struct displaced_step_closure *dsc)
{
  uint32_t status = displaced_read_reg (regs, dsc, ARM_PS_REGNUM);
  int store_executed = condition_true (dsc->u.block.cond, status);
  CORE_ADDR pc_stored_at, transferred_regs = bitcount (dsc->u.block.regmask);
  CORE_ADDR stm_insn_addr;
  uint32_t pc_val;
  long offset;
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  /* If condition code fails, there's nothing else to do.  */
  if (!store_executed)
    return;

  if (dsc->u.block.increment)
    {
      pc_stored_at = dsc->u.block.xfer_addr + 4 * transferred_regs;

      if (dsc->u.block.before)
	 pc_stored_at += 4;
    }
  else
    {
      pc_stored_at = dsc->u.block.xfer_addr;

      if (dsc->u.block.before)
	 pc_stored_at -= 4;
    }

  pc_val = read_memory_unsigned_integer (pc_stored_at, 4, byte_order);
  stm_insn_addr = dsc->scratch_base;
  offset = pc_val - stm_insn_addr;

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: detected PC offset %.8lx for "
			"STM instruction\n", offset);

  /* Rewrite the stored PC to the proper value for the non-displaced original
     instruction.  */
  write_memory_unsigned_integer (pc_stored_at, 4, byte_order,
				 dsc->insn_addr + offset);
}

/* Clean up an LDM which includes the PC in the register list.  We clumped all
   the registers in the transferred list into a contiguous range r0...rX (to
   avoid loading PC directly and losing control of the debugged program), so we
   must undo that here.  */

static void
cleanup_block_load_pc (struct gdbarch *gdbarch,
		       struct regcache *regs,
		       struct displaced_step_closure *dsc)
{
  uint32_t status = displaced_read_reg (regs, dsc, ARM_PS_REGNUM);
  int load_executed = condition_true (dsc->u.block.cond, status), i;
  unsigned int mask = dsc->u.block.regmask, write_reg = ARM_PC_REGNUM;
  unsigned int regs_loaded = bitcount (mask);
  unsigned int num_to_shuffle = regs_loaded, clobbered;

  /* The method employed here will fail if the register list is fully populated
     (we need to avoid loading PC directly).  */
  gdb_assert (num_to_shuffle < 16);

  if (!load_executed)
    return;

  clobbered = (1 << num_to_shuffle) - 1;

  while (num_to_shuffle > 0)
    {
      if ((mask & (1 << write_reg)) != 0)
	{
	  unsigned int read_reg = num_to_shuffle - 1;

	  if (read_reg != write_reg)
	    {
	      ULONGEST rval = displaced_read_reg (regs, dsc, read_reg);
	      displaced_write_reg (regs, dsc, write_reg, rval, LOAD_WRITE_PC);
	      if (debug_displaced)
		fprintf_unfiltered (gdb_stdlog, _("displaced: LDM: move "
				    "loaded register r%d to r%d\n"), read_reg,
				    write_reg);
	    }
	  else if (debug_displaced)
	    fprintf_unfiltered (gdb_stdlog, _("displaced: LDM: register "
				"r%d already in the right place\n"),
				write_reg);

	  clobbered &= ~(1 << write_reg);

	  num_to_shuffle--;
	}

      write_reg--;
    }

  /* Restore any registers we scribbled over.  */
  for (write_reg = 0; clobbered != 0; write_reg++)
    {
      if ((clobbered & (1 << write_reg)) != 0)
	{
	  displaced_write_reg (regs, dsc, write_reg, dsc->tmp[write_reg],
			       CANNOT_WRITE_PC);
	  if (debug_displaced)
	    fprintf_unfiltered (gdb_stdlog, _("displaced: LDM: restored "
				"clobbered register r%d\n"), write_reg);
	  clobbered &= ~(1 << write_reg);
	}
    }

  /* Perform register writeback manually.  */
  if (dsc->u.block.writeback)
    {
      ULONGEST new_rn_val = dsc->u.block.xfer_addr;

      if (dsc->u.block.increment)
	new_rn_val += regs_loaded * 4;
      else
	new_rn_val -= regs_loaded * 4;

      displaced_write_reg (regs, dsc, dsc->u.block.rn, new_rn_val,
			   CANNOT_WRITE_PC);
    }
}

/* Handle ldm/stm, apart from some tricky cases which are unlikely to occur
   in user-level code (in particular exception return, ldm rn, {...pc}^).  */

static int
arm_copy_block_xfer (struct gdbarch *gdbarch, uint32_t insn,
		     struct regcache *regs,
		     struct displaced_step_closure *dsc)
{
  int load = bit (insn, 20);
  int user = bit (insn, 22);
  int increment = bit (insn, 23);
  int before = bit (insn, 24);
  int writeback = bit (insn, 21);
  int rn = bits (insn, 16, 19);

  /* Block transfers which don't mention PC can be run directly
     out-of-line.  */
  if (rn != ARM_PC_REGNUM && (insn & 0x8000) == 0)
    return arm_copy_unmodified (gdbarch, insn, "ldm/stm", dsc);

  if (rn == ARM_PC_REGNUM)
    {
      warning (_("displaced: Unpredictable LDM or STM with "
		 "base register r15"));
      return arm_copy_unmodified (gdbarch, insn, "unpredictable ldm/stm", dsc);
    }

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying block transfer insn "
			"%.8lx\n", (unsigned long) insn);

  dsc->u.block.xfer_addr = displaced_read_reg (regs, dsc, rn);
  dsc->u.block.rn = rn;

  dsc->u.block.load = load;
  dsc->u.block.user = user;
  dsc->u.block.increment = increment;
  dsc->u.block.before = before;
  dsc->u.block.writeback = writeback;
  dsc->u.block.cond = bits (insn, 28, 31);

  dsc->u.block.regmask = insn & 0xffff;

  if (load)
    {
      if ((insn & 0xffff) == 0xffff)
	{
	  /* LDM with a fully-populated register list.  This case is
	     particularly tricky.  Implement for now by fully emulating the
	     instruction (which might not behave perfectly in all cases, but
	     these instructions should be rare enough for that not to matter
	     too much).  */
	  dsc->modinsn[0] = ARM_NOP;

	  dsc->cleanup = &cleanup_block_load_all;
	}
      else
	{
	  /* LDM of a list of registers which includes PC.  Implement by
	     rewriting the list of registers to be transferred into a
	     contiguous chunk r0...rX before doing the transfer, then shuffling
	     registers into the correct places in the cleanup routine.  */
	  unsigned int regmask = insn & 0xffff;
	  unsigned int num_in_list = bitcount (regmask), new_regmask, bit = 1;
	  unsigned int to = 0, from = 0, i, new_rn;

	  for (i = 0; i < num_in_list; i++)
	    dsc->tmp[i] = displaced_read_reg (regs, dsc, i);

	  /* Writeback makes things complicated.  We need to avoid clobbering
	     the base register with one of the registers in our modified
	     register list, but just using a different register can't work in
	     all cases, e.g.:

	       ldm r14!, {r0-r13,pc}

	     which would need to be rewritten as:

	       ldm rN!, {r0-r14}

	     but that can't work, because there's no free register for N.

	     Solve this by turning off the writeback bit, and emulating
	     writeback manually in the cleanup routine.  */

	  if (writeback)
	    insn &= ~(1 << 21);

	  new_regmask = (1 << num_in_list) - 1;

	  if (debug_displaced)
	    fprintf_unfiltered (gdb_stdlog, _("displaced: LDM r%d%s, "
				"{..., pc}: original reg list %.4x, modified "
				"list %.4x\n"), rn, writeback ? "!" : "",
				(int) insn & 0xffff, new_regmask);

	  dsc->modinsn[0] = (insn & ~0xffff) | (new_regmask & 0xffff);

	  dsc->cleanup = &cleanup_block_load_pc;
	}
    }
  else
    {
      /* STM of a list of registers which includes PC.  Run the instruction
	 as-is, but out of line: this will store the wrong value for the PC,
	 so we must manually fix up the memory in the cleanup routine.
	 Doing things this way has the advantage that we can auto-detect
	 the offset of the PC write (which is architecture-dependent) in
	 the cleanup routine.  */
      dsc->modinsn[0] = insn;

      dsc->cleanup = &cleanup_block_store_pc;
    }

  return 0;
}

static int
thumb2_copy_block_xfer (struct gdbarch *gdbarch, uint16_t insn1, uint16_t insn2,
			struct regcache *regs,
			struct displaced_step_closure *dsc)
{
  int rn = bits (insn1, 0, 3);
  int load = bit (insn1, 4);
  int writeback = bit (insn1, 5);

  /* Block transfers which don't mention PC can be run directly
     out-of-line.  */
  if (rn != ARM_PC_REGNUM && (insn2 & 0x8000) == 0)
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2, "ldm/stm", dsc);

  if (rn == ARM_PC_REGNUM)
    {
      warning (_("displaced: Unpredictable LDM or STM with "
		 "base register r15"));
      return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					  "unpredictable ldm/stm", dsc);
    }

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying block transfer insn "
			"%.4x%.4x\n", insn1, insn2);

  /* Clear bit 13, since it should be always zero.  */
  dsc->u.block.regmask = (insn2 & 0xdfff);
  dsc->u.block.rn = rn;

  dsc->u.block.load = load;
  dsc->u.block.user = 0;
  dsc->u.block.increment = bit (insn1, 7);
  dsc->u.block.before = bit (insn1, 8);
  dsc->u.block.writeback = writeback;
  dsc->u.block.cond = INST_AL;
  dsc->u.block.xfer_addr = displaced_read_reg (regs, dsc, rn);

  if (load)
    {
      if (dsc->u.block.regmask == 0xffff)
	{
	  /* This branch is impossible to happen.  */
	  gdb_assert (0);
	}
      else
	{
	  unsigned int regmask = dsc->u.block.regmask;
	  unsigned int num_in_list = bitcount (regmask), new_regmask, bit = 1;
	  unsigned int to = 0, from = 0, i, new_rn;

	  for (i = 0; i < num_in_list; i++)
	    dsc->tmp[i] = displaced_read_reg (regs, dsc, i);

	  if (writeback)
	    insn1 &= ~(1 << 5);

	  new_regmask = (1 << num_in_list) - 1;

	  if (debug_displaced)
	    fprintf_unfiltered (gdb_stdlog, _("displaced: LDM r%d%s, "
				"{..., pc}: original reg list %.4x, modified "
				"list %.4x\n"), rn, writeback ? "!" : "",
				(int) dsc->u.block.regmask, new_regmask);

	  dsc->modinsn[0] = insn1;
	  dsc->modinsn[1] = (new_regmask & 0xffff);
	  dsc->numinsns = 2;

	  dsc->cleanup = &cleanup_block_load_pc;
	}
    }
  else
    {
      dsc->modinsn[0] = insn1;
      dsc->modinsn[1] = insn2;
      dsc->numinsns = 2;
      dsc->cleanup = &cleanup_block_store_pc;
    }
  return 0;
}

/* Cleanup/copy SVC (SWI) instructions.  These two functions are overridden
   for Linux, where some SVC instructions must be treated specially.  */

static void
cleanup_svc (struct gdbarch *gdbarch, struct regcache *regs,
	     struct displaced_step_closure *dsc)
{
  CORE_ADDR resume_addr = dsc->insn_addr + dsc->insn_size;

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: cleanup for svc, resume at "
			"%.8lx\n", (unsigned long) resume_addr);

  displaced_write_reg (regs, dsc, ARM_PC_REGNUM, resume_addr, BRANCH_WRITE_PC);
}


/* Common copy routine for svc instruciton.  */

static int
install_svc (struct gdbarch *gdbarch, struct regcache *regs,
	     struct displaced_step_closure *dsc)
{
  /* Preparation: none.
     Insn: unmodified svc.
     Cleanup: pc <- insn_addr + insn_size.  */

  /* Pretend we wrote to the PC, so cleanup doesn't set PC to the next
     instruction.  */
  dsc->wrote_to_pc = 1;

  /* Allow OS-specific code to override SVC handling.  */
  if (dsc->u.svc.copy_svc_os)
    return dsc->u.svc.copy_svc_os (gdbarch, regs, dsc);
  else
    {
      dsc->cleanup = &cleanup_svc;
      return 0;
    }
}

static int
arm_copy_svc (struct gdbarch *gdbarch, uint32_t insn,
	      struct regcache *regs, struct displaced_step_closure *dsc)
{

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying svc insn %.8lx\n",
			(unsigned long) insn);

  dsc->modinsn[0] = insn;

  return install_svc (gdbarch, regs, dsc);
}

static int
thumb_copy_svc (struct gdbarch *gdbarch, uint16_t insn,
		struct regcache *regs, struct displaced_step_closure *dsc)
{

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying svc insn %.4x\n",
			insn);

  dsc->modinsn[0] = insn;

  return install_svc (gdbarch, regs, dsc);
}

/* Copy undefined instructions.  */

static int
arm_copy_undef (struct gdbarch *gdbarch, uint32_t insn,
		struct displaced_step_closure *dsc)
{
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying undefined insn %.8lx\n",
			(unsigned long) insn);

  dsc->modinsn[0] = insn;

  return 0;
}

static int
thumb_32bit_copy_undef (struct gdbarch *gdbarch, uint16_t insn1, uint16_t insn2,
                       struct displaced_step_closure *dsc)
{

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying undefined insn "
                       "%.4x %.4x\n", (unsigned short) insn1,
                       (unsigned short) insn2);

  dsc->modinsn[0] = insn1;
  dsc->modinsn[1] = insn2;
  dsc->numinsns = 2;

  return 0;
}

/* Copy unpredictable instructions.  */

static int
arm_copy_unpred (struct gdbarch *gdbarch, uint32_t insn,
		 struct displaced_step_closure *dsc)
{
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying unpredictable insn "
			"%.8lx\n", (unsigned long) insn);

  dsc->modinsn[0] = insn;

  return 0;
}

/* The decode_* functions are instruction decoding helpers.  They mostly follow
   the presentation in the ARM ARM.  */

static int
arm_decode_misc_memhint_neon (struct gdbarch *gdbarch, uint32_t insn,
			      struct regcache *regs,
			      struct displaced_step_closure *dsc)
{
  unsigned int op1 = bits (insn, 20, 26), op2 = bits (insn, 4, 7);
  unsigned int rn = bits (insn, 16, 19);

  if (op1 == 0x10 && (op2 & 0x2) == 0x0 && (rn & 0xe) == 0x0)
    return arm_copy_unmodified (gdbarch, insn, "cps", dsc);
  else if (op1 == 0x10 && op2 == 0x0 && (rn & 0xe) == 0x1)
    return arm_copy_unmodified (gdbarch, insn, "setend", dsc);
  else if ((op1 & 0x60) == 0x20)
    return arm_copy_unmodified (gdbarch, insn, "neon dataproc", dsc);
  else if ((op1 & 0x71) == 0x40)
    return arm_copy_unmodified (gdbarch, insn, "neon elt/struct load/store",
				dsc);
  else if ((op1 & 0x77) == 0x41)
    return arm_copy_unmodified (gdbarch, insn, "unallocated mem hint", dsc);
  else if ((op1 & 0x77) == 0x45)
    return arm_copy_preload (gdbarch, insn, regs, dsc);  /* pli.  */
  else if ((op1 & 0x77) == 0x51)
    {
      if (rn != 0xf)
	return arm_copy_preload (gdbarch, insn, regs, dsc);  /* pld/pldw.  */
      else
	return arm_copy_unpred (gdbarch, insn, dsc);
    }
  else if ((op1 & 0x77) == 0x55)
    return arm_copy_preload (gdbarch, insn, regs, dsc);  /* pld/pldw.  */
  else if (op1 == 0x57)
    switch (op2)
      {
      case 0x1: return arm_copy_unmodified (gdbarch, insn, "clrex", dsc);
      case 0x4: return arm_copy_unmodified (gdbarch, insn, "dsb", dsc);
      case 0x5: return arm_copy_unmodified (gdbarch, insn, "dmb", dsc);
      case 0x6: return arm_copy_unmodified (gdbarch, insn, "isb", dsc);
      default: return arm_copy_unpred (gdbarch, insn, dsc);
      }
  else if ((op1 & 0x63) == 0x43)
    return arm_copy_unpred (gdbarch, insn, dsc);
  else if ((op2 & 0x1) == 0x0)
    switch (op1 & ~0x80)
      {
      case 0x61:
	return arm_copy_unmodified (gdbarch, insn, "unallocated mem hint", dsc);
      case 0x65:
	return arm_copy_preload_reg (gdbarch, insn, regs, dsc);  /* pli reg.  */
      case 0x71: case 0x75:
        /* pld/pldw reg.  */
	return arm_copy_preload_reg (gdbarch, insn, regs, dsc);
      case 0x63: case 0x67: case 0x73: case 0x77:
	return arm_copy_unpred (gdbarch, insn, dsc);
      default:
	return arm_copy_undef (gdbarch, insn, dsc);
      }
  else
    return arm_copy_undef (gdbarch, insn, dsc);  /* Probably unreachable.  */
}

static int
arm_decode_unconditional (struct gdbarch *gdbarch, uint32_t insn,
			  struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  if (bit (insn, 27) == 0)
    return arm_decode_misc_memhint_neon (gdbarch, insn, regs, dsc);
  /* Switch on bits: 0bxxxxx321xxx0xxxxxxxxxxxxxxxxxxxx.  */
  else switch (((insn & 0x7000000) >> 23) | ((insn & 0x100000) >> 20))
    {
    case 0x0: case 0x2:
      return arm_copy_unmodified (gdbarch, insn, "srs", dsc);

    case 0x1: case 0x3:
      return arm_copy_unmodified (gdbarch, insn, "rfe", dsc);

    case 0x4: case 0x5: case 0x6: case 0x7:
      return arm_copy_b_bl_blx (gdbarch, insn, regs, dsc);

    case 0x8:
      switch ((insn & 0xe00000) >> 21)
	{
	case 0x1: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7:
	  /* stc/stc2.  */
	  return arm_copy_copro_load_store (gdbarch, insn, regs, dsc);

	case 0x2:
	  return arm_copy_unmodified (gdbarch, insn, "mcrr/mcrr2", dsc);

	default:
	  return arm_copy_undef (gdbarch, insn, dsc);
	}

    case 0x9:
      {
	 int rn_f = (bits (insn, 16, 19) == 0xf);
	switch ((insn & 0xe00000) >> 21)
	  {
	  case 0x1: case 0x3:
	    /* ldc/ldc2 imm (undefined for rn == pc).  */
	    return rn_f ? arm_copy_undef (gdbarch, insn, dsc)
			: arm_copy_copro_load_store (gdbarch, insn, regs, dsc);

	  case 0x2:
	    return arm_copy_unmodified (gdbarch, insn, "mrrc/mrrc2", dsc);

	  case 0x4: case 0x5: case 0x6: case 0x7:
	    /* ldc/ldc2 lit (undefined for rn != pc).  */
	    return rn_f ? arm_copy_copro_load_store (gdbarch, insn, regs, dsc)
			: arm_copy_undef (gdbarch, insn, dsc);

	  default:
	    return arm_copy_undef (gdbarch, insn, dsc);
	  }
      }

    case 0xa:
      return arm_copy_unmodified (gdbarch, insn, "stc/stc2", dsc);

    case 0xb:
      if (bits (insn, 16, 19) == 0xf)
        /* ldc/ldc2 lit.  */
	return arm_copy_copro_load_store (gdbarch, insn, regs, dsc);
      else
	return arm_copy_undef (gdbarch, insn, dsc);

    case 0xc:
      if (bit (insn, 4))
	return arm_copy_unmodified (gdbarch, insn, "mcr/mcr2", dsc);
      else
	return arm_copy_unmodified (gdbarch, insn, "cdp/cdp2", dsc);

    case 0xd:
      if (bit (insn, 4))
	return arm_copy_unmodified (gdbarch, insn, "mrc/mrc2", dsc);
      else
	return arm_copy_unmodified (gdbarch, insn, "cdp/cdp2", dsc);

    default:
      return arm_copy_undef (gdbarch, insn, dsc);
    }
}

/* Decode miscellaneous instructions in dp/misc encoding space.  */

static int
arm_decode_miscellaneous (struct gdbarch *gdbarch, uint32_t insn,
			  struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  unsigned int op2 = bits (insn, 4, 6);
  unsigned int op = bits (insn, 21, 22);
  unsigned int op1 = bits (insn, 16, 19);

  switch (op2)
    {
    case 0x0:
      return arm_copy_unmodified (gdbarch, insn, "mrs/msr", dsc);

    case 0x1:
      if (op == 0x1)  /* bx.  */
	return arm_copy_bx_blx_reg (gdbarch, insn, regs, dsc);
      else if (op == 0x3)
	return arm_copy_unmodified (gdbarch, insn, "clz", dsc);
      else
	return arm_copy_undef (gdbarch, insn, dsc);

    case 0x2:
      if (op == 0x1)
        /* Not really supported.  */
	return arm_copy_unmodified (gdbarch, insn, "bxj", dsc);
      else
	return arm_copy_undef (gdbarch, insn, dsc);

    case 0x3:
      if (op == 0x1)
	return arm_copy_bx_blx_reg (gdbarch, insn,
				regs, dsc);  /* blx register.  */
      else
	return arm_copy_undef (gdbarch, insn, dsc);

    case 0x5:
      return arm_copy_unmodified (gdbarch, insn, "saturating add/sub", dsc);

    case 0x7:
      if (op == 0x1)
	return arm_copy_unmodified (gdbarch, insn, "bkpt", dsc);
      else if (op == 0x3)
        /* Not really supported.  */
	return arm_copy_unmodified (gdbarch, insn, "smc", dsc);

    default:
      return arm_copy_undef (gdbarch, insn, dsc);
    }
}

static int
arm_decode_dp_misc (struct gdbarch *gdbarch, uint32_t insn,
		    struct regcache *regs,
		    struct displaced_step_closure *dsc)
{
  if (bit (insn, 25))
    switch (bits (insn, 20, 24))
      {
      case 0x10:
	return arm_copy_unmodified (gdbarch, insn, "movw", dsc);

      case 0x14:
	return arm_copy_unmodified (gdbarch, insn, "movt", dsc);

      case 0x12: case 0x16:
	return arm_copy_unmodified (gdbarch, insn, "msr imm", dsc);

      default:
	return arm_copy_alu_imm (gdbarch, insn, regs, dsc);
      }
  else
    {
      uint32_t op1 = bits (insn, 20, 24), op2 = bits (insn, 4, 7);

      if ((op1 & 0x19) != 0x10 && (op2 & 0x1) == 0x0)
	return arm_copy_alu_reg (gdbarch, insn, regs, dsc);
      else if ((op1 & 0x19) != 0x10 && (op2 & 0x9) == 0x1)
	return arm_copy_alu_shifted_reg (gdbarch, insn, regs, dsc);
      else if ((op1 & 0x19) == 0x10 && (op2 & 0x8) == 0x0)
	return arm_decode_miscellaneous (gdbarch, insn, regs, dsc);
      else if ((op1 & 0x19) == 0x10 && (op2 & 0x9) == 0x8)
	return arm_copy_unmodified (gdbarch, insn, "halfword mul/mla", dsc);
      else if ((op1 & 0x10) == 0x00 && op2 == 0x9)
	return arm_copy_unmodified (gdbarch, insn, "mul/mla", dsc);
      else if ((op1 & 0x10) == 0x10 && op2 == 0x9)
	return arm_copy_unmodified (gdbarch, insn, "synch", dsc);
      else if (op2 == 0xb || (op2 & 0xd) == 0xd)
	/* 2nd arg means "unpriveleged".  */
	return arm_copy_extra_ld_st (gdbarch, insn, (op1 & 0x12) == 0x02, regs,
				     dsc);
    }

  /* Should be unreachable.  */
  return 1;
}

static int
arm_decode_ld_st_word_ubyte (struct gdbarch *gdbarch, uint32_t insn,
			     struct regcache *regs,
			     struct displaced_step_closure *dsc)
{
  int a = bit (insn, 25), b = bit (insn, 4);
  uint32_t op1 = bits (insn, 20, 24);
  int rn_f = bits (insn, 16, 19) == 0xf;

  if ((!a && (op1 & 0x05) == 0x00 && (op1 & 0x17) != 0x02)
      || (a && (op1 & 0x05) == 0x00 && (op1 & 0x17) != 0x02 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 0, 4, 0);
  else if ((!a && (op1 & 0x17) == 0x02)
	    || (a && (op1 & 0x17) == 0x02 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 0, 4, 1);
  else if ((!a && (op1 & 0x05) == 0x01 && (op1 & 0x17) != 0x03)
	    || (a && (op1 & 0x05) == 0x01 && (op1 & 0x17) != 0x03 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 1, 4, 0);
  else if ((!a && (op1 & 0x17) == 0x03)
	   || (a && (op1 & 0x17) == 0x03 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 1, 4, 1);
  else if ((!a && (op1 & 0x05) == 0x04 && (op1 & 0x17) != 0x06)
	    || (a && (op1 & 0x05) == 0x04 && (op1 & 0x17) != 0x06 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 0, 1, 0);
  else if ((!a && (op1 & 0x17) == 0x06)
	   || (a && (op1 & 0x17) == 0x06 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 0, 1, 1);
  else if ((!a && (op1 & 0x05) == 0x05 && (op1 & 0x17) != 0x07)
	   || (a && (op1 & 0x05) == 0x05 && (op1 & 0x17) != 0x07 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 1, 1, 0);
  else if ((!a && (op1 & 0x17) == 0x07)
	   || (a && (op1 & 0x17) == 0x07 && !b))
    return arm_copy_ldr_str_ldrb_strb (gdbarch, insn, regs, dsc, 1, 1, 1);

  /* Should be unreachable.  */
  return 1;
}

static int
arm_decode_media (struct gdbarch *gdbarch, uint32_t insn,
		  struct displaced_step_closure *dsc)
{
  switch (bits (insn, 20, 24))
    {
    case 0x00: case 0x01: case 0x02: case 0x03:
      return arm_copy_unmodified (gdbarch, insn, "parallel add/sub signed", dsc);

    case 0x04: case 0x05: case 0x06: case 0x07:
      return arm_copy_unmodified (gdbarch, insn, "parallel add/sub unsigned", dsc);

    case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x0d: case 0x0e: case 0x0f:
      return arm_copy_unmodified (gdbarch, insn,
			      "decode/pack/unpack/saturate/reverse", dsc);

    case 0x18:
      if (bits (insn, 5, 7) == 0)  /* op2.  */
	 {
	  if (bits (insn, 12, 15) == 0xf)
	    return arm_copy_unmodified (gdbarch, insn, "usad8", dsc);
	  else
	    return arm_copy_unmodified (gdbarch, insn, "usada8", dsc);
	}
      else
	 return arm_copy_undef (gdbarch, insn, dsc);

    case 0x1a: case 0x1b:
      if (bits (insn, 5, 6) == 0x2)  /* op2[1:0].  */
	return arm_copy_unmodified (gdbarch, insn, "sbfx", dsc);
      else
	return arm_copy_undef (gdbarch, insn, dsc);

    case 0x1c: case 0x1d:
      if (bits (insn, 5, 6) == 0x0)  /* op2[1:0].  */
	 {
	  if (bits (insn, 0, 3) == 0xf)
	    return arm_copy_unmodified (gdbarch, insn, "bfc", dsc);
	  else
	    return arm_copy_unmodified (gdbarch, insn, "bfi", dsc);
	}
      else
	return arm_copy_undef (gdbarch, insn, dsc);

    case 0x1e: case 0x1f:
      if (bits (insn, 5, 6) == 0x2)  /* op2[1:0].  */
	return arm_copy_unmodified (gdbarch, insn, "ubfx", dsc);
      else
	return arm_copy_undef (gdbarch, insn, dsc);
    }

  /* Should be unreachable.  */
  return 1;
}

static int
arm_decode_b_bl_ldmstm (struct gdbarch *gdbarch, int32_t insn,
			struct regcache *regs,
			struct displaced_step_closure *dsc)
{
  if (bit (insn, 25))
    return arm_copy_b_bl_blx (gdbarch, insn, regs, dsc);
  else
    return arm_copy_block_xfer (gdbarch, insn, regs, dsc);
}

static int
arm_decode_ext_reg_ld_st (struct gdbarch *gdbarch, uint32_t insn,
			  struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  unsigned int opcode = bits (insn, 20, 24);

  switch (opcode)
    {
    case 0x04: case 0x05:  /* VFP/Neon mrrc/mcrr.  */
      return arm_copy_unmodified (gdbarch, insn, "vfp/neon mrrc/mcrr", dsc);

    case 0x08: case 0x0a: case 0x0c: case 0x0e:
    case 0x12: case 0x16:
      return arm_copy_unmodified (gdbarch, insn, "vfp/neon vstm/vpush", dsc);

    case 0x09: case 0x0b: case 0x0d: case 0x0f:
    case 0x13: case 0x17:
      return arm_copy_unmodified (gdbarch, insn, "vfp/neon vldm/vpop", dsc);

    case 0x10: case 0x14: case 0x18: case 0x1c:  /* vstr.  */
    case 0x11: case 0x15: case 0x19: case 0x1d:  /* vldr.  */
      /* Note: no writeback for these instructions.  Bit 25 will always be
	 zero though (via caller), so the following works OK.  */
      return arm_copy_copro_load_store (gdbarch, insn, regs, dsc);
    }

  /* Should be unreachable.  */
  return 1;
}

/* Decode shifted register instructions.  */

static int
thumb2_decode_dp_shift_reg (struct gdbarch *gdbarch, uint16_t insn1,
			    uint16_t insn2,  struct regcache *regs,
			    struct displaced_step_closure *dsc)
{
  /* PC is only allowed to be used in instruction MOV.  */

  unsigned int op = bits (insn1, 5, 8);
  unsigned int rn = bits (insn1, 0, 3);

  if (op == 0x2 && rn == 0xf) /* MOV */
    return thumb2_copy_alu_imm (gdbarch, insn1, insn2, regs, dsc);
  else
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					"dp (shift reg)", dsc);
}


/* Decode extension register load/store.  Exactly the same as
   arm_decode_ext_reg_ld_st.  */

static int
thumb2_decode_ext_reg_ld_st (struct gdbarch *gdbarch, uint16_t insn1,
			     uint16_t insn2,  struct regcache *regs,
			     struct displaced_step_closure *dsc)
{
  unsigned int opcode = bits (insn1, 4, 8);

  switch (opcode)
    {
    case 0x04: case 0x05:
      return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					  "vfp/neon vmov", dsc);

    case 0x08: case 0x0c: /* 01x00 */
    case 0x0a: case 0x0e: /* 01x10 */
    case 0x12: case 0x16: /* 10x10 */
      return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					  "vfp/neon vstm/vpush", dsc);

    case 0x09: case 0x0d: /* 01x01 */
    case 0x0b: case 0x0f: /* 01x11 */
    case 0x13: case 0x17: /* 10x11 */
      return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					  "vfp/neon vldm/vpop", dsc);

    case 0x10: case 0x14: case 0x18: case 0x1c:  /* vstr.  */
      return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					  "vstr", dsc);
    case 0x11: case 0x15: case 0x19: case 0x1d:  /* vldr.  */
      return thumb2_copy_copro_load_store (gdbarch, insn1, insn2, regs, dsc);
    }

  /* Should be unreachable.  */
  return 1;
}

static int
arm_decode_svc_copro (struct gdbarch *gdbarch, uint32_t insn, CORE_ADDR to,
		      struct regcache *regs, struct displaced_step_closure *dsc)
{
  unsigned int op1 = bits (insn, 20, 25);
  int op = bit (insn, 4);
  unsigned int coproc = bits (insn, 8, 11);
  unsigned int rn = bits (insn, 16, 19);

  if ((op1 & 0x20) == 0x00 && (op1 & 0x3a) != 0x00 && (coproc & 0xe) == 0xa)
    return arm_decode_ext_reg_ld_st (gdbarch, insn, regs, dsc);
  else if ((op1 & 0x21) == 0x00 && (op1 & 0x3a) != 0x00
	   && (coproc & 0xe) != 0xa)
    /* stc/stc2.  */
    return arm_copy_copro_load_store (gdbarch, insn, regs, dsc);
  else if ((op1 & 0x21) == 0x01 && (op1 & 0x3a) != 0x00
	   && (coproc & 0xe) != 0xa)
    /* ldc/ldc2 imm/lit.  */
    return arm_copy_copro_load_store (gdbarch, insn, regs, dsc);
  else if ((op1 & 0x3e) == 0x00)
    return arm_copy_undef (gdbarch, insn, dsc);
  else if ((op1 & 0x3e) == 0x04 && (coproc & 0xe) == 0xa)
    return arm_copy_unmodified (gdbarch, insn, "neon 64bit xfer", dsc);
  else if (op1 == 0x04 && (coproc & 0xe) != 0xa)
    return arm_copy_unmodified (gdbarch, insn, "mcrr/mcrr2", dsc);
  else if (op1 == 0x05 && (coproc & 0xe) != 0xa)
    return arm_copy_unmodified (gdbarch, insn, "mrrc/mrrc2", dsc);
  else if ((op1 & 0x30) == 0x20 && !op)
    {
      if ((coproc & 0xe) == 0xa)
	return arm_copy_unmodified (gdbarch, insn, "vfp dataproc", dsc);
      else
	return arm_copy_unmodified (gdbarch, insn, "cdp/cdp2", dsc);
    }
  else if ((op1 & 0x30) == 0x20 && op)
    return arm_copy_unmodified (gdbarch, insn, "neon 8/16/32 bit xfer", dsc);
  else if ((op1 & 0x31) == 0x20 && op && (coproc & 0xe) != 0xa)
    return arm_copy_unmodified (gdbarch, insn, "mcr/mcr2", dsc);
  else if ((op1 & 0x31) == 0x21 && op && (coproc & 0xe) != 0xa)
    return arm_copy_unmodified (gdbarch, insn, "mrc/mrc2", dsc);
  else if ((op1 & 0x30) == 0x30)
    return arm_copy_svc (gdbarch, insn, regs, dsc);
  else
    return arm_copy_undef (gdbarch, insn, dsc);  /* Possibly unreachable.  */
}

static int
thumb2_decode_svc_copro (struct gdbarch *gdbarch, uint16_t insn1,
			 uint16_t insn2, struct regcache *regs,
			 struct displaced_step_closure *dsc)
{
  unsigned int coproc = bits (insn2, 8, 11);
  unsigned int op1 = bits (insn1, 4, 9);
  unsigned int bit_5_8 = bits (insn1, 5, 8);
  unsigned int bit_9 = bit (insn1, 9);
  unsigned int bit_4 = bit (insn1, 4);
  unsigned int rn = bits (insn1, 0, 3);

  if (bit_9 == 0)
    {
      if (bit_5_8 == 2)
	return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					    "neon 64bit xfer/mrrc/mrrc2/mcrr/mcrr2",
					    dsc);
      else if (bit_5_8 == 0) /* UNDEFINED.  */
	return thumb_32bit_copy_undef (gdbarch, insn1, insn2, dsc);
      else
	{
	   /*coproc is 101x.  SIMD/VFP, ext registers load/store.  */
	  if ((coproc & 0xe) == 0xa)
	    return thumb2_decode_ext_reg_ld_st (gdbarch, insn1, insn2, regs,
						dsc);
	  else /* coproc is not 101x.  */
	    {
	      if (bit_4 == 0) /* STC/STC2.  */
		return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						    "stc/stc2", dsc);
	      else /* LDC/LDC2 {literal, immeidate}.  */
		return thumb2_copy_copro_load_store (gdbarch, insn1, insn2,
						     regs, dsc);
	    }
	}
    }
  else
    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2, "coproc", dsc);

  return 0;
}

static void
install_pc_relative (struct gdbarch *gdbarch, struct regcache *regs,
		     struct displaced_step_closure *dsc, int rd)
{
  /* ADR Rd, #imm

     Rewrite as:

     Preparation: Rd <- PC
     Insn: ADD Rd, #imm
     Cleanup: Null.
  */

  /* Rd <- PC */
  int val = displaced_read_reg (regs, dsc, ARM_PC_REGNUM);
  displaced_write_reg (regs, dsc, rd, val, CANNOT_WRITE_PC);
}

static int
thumb_copy_pc_relative_16bit (struct gdbarch *gdbarch, struct regcache *regs,
			      struct displaced_step_closure *dsc,
			      int rd, unsigned int imm)
{

  /* Encoding T2: ADDS Rd, #imm */
  dsc->modinsn[0] = (0x3000 | (rd << 8) | imm);

  install_pc_relative (gdbarch, regs, dsc, rd);

  return 0;
}

static int
thumb_decode_pc_relative_16bit (struct gdbarch *gdbarch, uint16_t insn,
				struct regcache *regs,
				struct displaced_step_closure *dsc)
{
  unsigned int rd = bits (insn, 8, 10);
  unsigned int imm8 = bits (insn, 0, 7);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying thumb adr r%d, #%d insn %.4x\n",
			rd, imm8, insn);

  return thumb_copy_pc_relative_16bit (gdbarch, regs, dsc, rd, imm8);
}

static int
thumb_copy_pc_relative_32bit (struct gdbarch *gdbarch, uint16_t insn1,
			      uint16_t insn2, struct regcache *regs,
			      struct displaced_step_closure *dsc)
{
  unsigned int rd = bits (insn2, 8, 11);
  /* Since immediate has the same encoding in ADR ADD and SUB, so we simply
     extract raw immediate encoding rather than computing immediate.  When
     generating ADD or SUB instruction, we can simply perform OR operation to
     set immediate into ADD.  */
  unsigned int imm_3_8 = insn2 & 0x70ff;
  unsigned int imm_i = insn1 & 0x0400; /* Clear all bits except bit 10.  */

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying thumb adr r%d, #%d:%d insn %.4x%.4x\n",
			rd, imm_i, imm_3_8, insn1, insn2);

  if (bit (insn1, 7)) /* Encoding T2 */
    {
      /* Encoding T3: SUB Rd, Rd, #imm */
      dsc->modinsn[0] = (0xf1a0 | rd | imm_i);
      dsc->modinsn[1] = ((rd << 8) | imm_3_8);
    }
  else /* Encoding T3 */
    {
      /* Encoding T3: ADD Rd, Rd, #imm */
      dsc->modinsn[0] = (0xf100 | rd | imm_i);
      dsc->modinsn[1] = ((rd << 8) | imm_3_8);
    }
  dsc->numinsns = 2;

  install_pc_relative (gdbarch, regs, dsc, rd);

  return 0;
}

static int
thumb_copy_16bit_ldr_literal (struct gdbarch *gdbarch, unsigned short insn1,
			      struct regcache *regs,
			      struct displaced_step_closure *dsc)
{
  unsigned int rt = bits (insn1, 8, 10);
  unsigned int pc;
  int imm8 = (bits (insn1, 0, 7) << 2);
  CORE_ADDR from = dsc->insn_addr;

  /* LDR Rd, #imm8

     Rwrite as:

     Preparation: tmp0 <- R0, tmp2 <- R2, tmp3 <- R3, R2 <- PC, R3 <- #imm8;

     Insn: LDR R0, [R2, R3];
     Cleanup: R2 <- tmp2, R3 <- tmp3, Rd <- R0, R0 <- tmp0 */

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying thumb ldr r%d [pc #%d]\n"
			, rt, imm8);

  dsc->tmp[0] = displaced_read_reg (regs, dsc, 0);
  dsc->tmp[2] = displaced_read_reg (regs, dsc, 2);
  dsc->tmp[3] = displaced_read_reg (regs, dsc, 3);
  pc = displaced_read_reg (regs, dsc, ARM_PC_REGNUM);
  /* The assembler calculates the required value of the offset from the
     Align(PC,4) value of this instruction to the label.  */
  pc = pc & 0xfffffffc;

  displaced_write_reg (regs, dsc, 2, pc, CANNOT_WRITE_PC);
  displaced_write_reg (regs, dsc, 3, imm8, CANNOT_WRITE_PC);

  dsc->rd = rt;
  dsc->u.ldst.xfersize = 4;
  dsc->u.ldst.rn = 0;
  dsc->u.ldst.immed = 0;
  dsc->u.ldst.writeback = 0;
  dsc->u.ldst.restore_r4 = 0;

  dsc->modinsn[0] = 0x58d0; /* ldr r0, [r2, r3]*/

  dsc->cleanup = &cleanup_load;

  return 0;
}

/* Copy Thumb cbnz/cbz insruction.  */

static int
thumb_copy_cbnz_cbz (struct gdbarch *gdbarch, uint16_t insn1,
		     struct regcache *regs,
		     struct displaced_step_closure *dsc)
{
  int non_zero = bit (insn1, 11);
  unsigned int imm5 = (bit (insn1, 9) << 6) | (bits (insn1, 3, 7) << 1);
  CORE_ADDR from = dsc->insn_addr;
  int rn = bits (insn1, 0, 2);
  int rn_val = displaced_read_reg (regs, dsc, rn);

  dsc->u.branch.cond = (rn_val && non_zero) || (!rn_val && !non_zero);
  /* CBNZ and CBZ do not affect the condition flags.  If condition is true,
     set it INST_AL, so cleanup_branch will know branch is taken, otherwise,
     condition is false, let it be, cleanup_branch will do nothing.  */
  if (dsc->u.branch.cond)
    {
      dsc->u.branch.cond = INST_AL;
      dsc->u.branch.dest = from + 4 + imm5;
    }
  else
      dsc->u.branch.dest = from + 2;

  dsc->u.branch.link = 0;
  dsc->u.branch.exchange = 0;

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copying %s [r%d = 0x%x]"
			" insn %.4x to %.8lx\n", non_zero ? "cbnz" : "cbz",
			rn, rn_val, insn1, dsc->u.branch.dest);

  dsc->modinsn[0] = THUMB_NOP;

  dsc->cleanup = &cleanup_branch;
  return 0;
}

/* Copy Table Branch Byte/Halfword */
static int
thumb2_copy_table_branch (struct gdbarch *gdbarch, uint16_t insn1,
			  uint16_t insn2, struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  ULONGEST rn_val, rm_val;
  int is_tbh = bit (insn2, 4);
  CORE_ADDR halfwords = 0;
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  rn_val = displaced_read_reg (regs, dsc, bits (insn1, 0, 3));
  rm_val = displaced_read_reg (regs, dsc, bits (insn2, 0, 3));

  if (is_tbh)
    {
      gdb_byte buf[2];

      target_read_memory (rn_val + 2 * rm_val, buf, 2);
      halfwords = extract_unsigned_integer (buf, 2, byte_order);
    }
  else
    {
      gdb_byte buf[1];

      target_read_memory (rn_val + rm_val, buf, 1);
      halfwords = extract_unsigned_integer (buf, 1, byte_order);
    }

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: %s base 0x%x offset 0x%x"
			" offset 0x%x\n", is_tbh ? "tbh" : "tbb",
			(unsigned int) rn_val, (unsigned int) rm_val,
			(unsigned int) halfwords);

  dsc->u.branch.cond = INST_AL;
  dsc->u.branch.link = 0;
  dsc->u.branch.exchange = 0;
  dsc->u.branch.dest = dsc->insn_addr + 4 + 2 * halfwords;

  dsc->cleanup = &cleanup_branch;

  return 0;
}

static void
cleanup_pop_pc_16bit_all (struct gdbarch *gdbarch, struct regcache *regs,
			  struct displaced_step_closure *dsc)
{
  /* PC <- r7 */
  int val = displaced_read_reg (regs, dsc, 7);
  displaced_write_reg (regs, dsc, ARM_PC_REGNUM, val, BX_WRITE_PC);

  /* r7 <- r8 */
  val = displaced_read_reg (regs, dsc, 8);
  displaced_write_reg (regs, dsc, 7, val, CANNOT_WRITE_PC);

  /* r8 <- tmp[0] */
  displaced_write_reg (regs, dsc, 8, dsc->tmp[0], CANNOT_WRITE_PC);

}

static int
thumb_copy_pop_pc_16bit (struct gdbarch *gdbarch, unsigned short insn1,
			 struct regcache *regs,
			 struct displaced_step_closure *dsc)
{
  dsc->u.block.regmask = insn1 & 0x00ff;

  /* Rewrite instruction: POP {rX, rY, ...,rZ, PC}
     to :

     (1) register list is full, that is, r0-r7 are used.
     Prepare: tmp[0] <- r8

     POP {r0, r1, ...., r6, r7}; remove PC from reglist
     MOV r8, r7; Move value of r7 to r8;
     POP {r7}; Store PC value into r7.

     Cleanup: PC <- r7, r7 <- r8, r8 <-tmp[0]

     (2) register list is not full, supposing there are N registers in
     register list (except PC, 0 <= N <= 7).
     Prepare: for each i, 0 - N, tmp[i] <- ri.

     POP {r0, r1, ...., rN};

     Cleanup: Set registers in original reglist from r0 - rN.  Restore r0 - rN
     from tmp[] properly.
  */
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog,
			"displaced: copying thumb pop {%.8x, pc} insn %.4x\n",
			dsc->u.block.regmask, insn1);

  if (dsc->u.block.regmask == 0xff)
    {
      dsc->tmp[0] = displaced_read_reg (regs, dsc, 8);

      dsc->modinsn[0] = (insn1 & 0xfeff); /* POP {r0,r1,...,r6, r7} */
      dsc->modinsn[1] = 0x46b8; /* MOV r8, r7 */
      dsc->modinsn[2] = 0xbc80; /* POP {r7} */

      dsc->numinsns = 3;
      dsc->cleanup = &cleanup_pop_pc_16bit_all;
    }
  else
    {
      unsigned int num_in_list = bitcount (dsc->u.block.regmask);
      unsigned int new_regmask, bit = 1;
      unsigned int to = 0, from = 0, i, new_rn;

      for (i = 0; i < num_in_list + 1; i++)
	dsc->tmp[i] = displaced_read_reg (regs, dsc, i);

      new_regmask = (1 << (num_in_list + 1)) - 1;

      if (debug_displaced)
	fprintf_unfiltered (gdb_stdlog, _("displaced: POP "
					  "{..., pc}: original reg list %.4x,"
					  " modified list %.4x\n"),
			    (int) dsc->u.block.regmask, new_regmask);

      dsc->u.block.regmask |= 0x8000;
      dsc->u.block.writeback = 0;
      dsc->u.block.cond = INST_AL;

      dsc->modinsn[0] = (insn1 & ~0x1ff) | (new_regmask & 0xff);

      dsc->cleanup = &cleanup_block_load_pc;
    }

  return 0;
}

static void
thumb_process_displaced_16bit_insn (struct gdbarch *gdbarch, uint16_t insn1,
				    struct regcache *regs,
				    struct displaced_step_closure *dsc)
{
  unsigned short op_bit_12_15 = bits (insn1, 12, 15);
  unsigned short op_bit_10_11 = bits (insn1, 10, 11);
  int err = 0;

  /* 16-bit thumb instructions.  */
  switch (op_bit_12_15)
    {
      /* Shift (imme), add, subtract, move and compare.  */
    case 0: case 1: case 2: case 3:
      err = thumb_copy_unmodified_16bit (gdbarch, insn1,
					 "shift/add/sub/mov/cmp",
					 dsc);
      break;
    case 4:
      switch (op_bit_10_11)
	{
	case 0: /* Data-processing */
	  err = thumb_copy_unmodified_16bit (gdbarch, insn1,
					     "data-processing",
					     dsc);
	  break;
	case 1: /* Special data instructions and branch and exchange.  */
	  {
	    unsigned short op = bits (insn1, 7, 9);
	    if (op == 6 || op == 7) /* BX or BLX */
	      err = thumb_copy_bx_blx_reg (gdbarch, insn1, regs, dsc);
	    else if (bits (insn1, 6, 7) != 0) /* ADD/MOV/CMP high registers.  */
	      err = thumb_copy_alu_reg (gdbarch, insn1, regs, dsc);
	    else
	      err = thumb_copy_unmodified_16bit (gdbarch, insn1, "special data",
						 dsc);
	  }
	  break;
	default: /* LDR (literal) */
	  err = thumb_copy_16bit_ldr_literal (gdbarch, insn1, regs, dsc);
	}
      break;
    case 5: case 6: case 7: case 8: case 9: /* Load/Store single data item */
      err = thumb_copy_unmodified_16bit (gdbarch, insn1, "ldr/str", dsc);
      break;
    case 10:
      if (op_bit_10_11 < 2) /* Generate PC-relative address */
	err = thumb_decode_pc_relative_16bit (gdbarch, insn1, regs, dsc);
      else /* Generate SP-relative address */
	err = thumb_copy_unmodified_16bit (gdbarch, insn1, "sp-relative", dsc);
      break;
    case 11: /* Misc 16-bit instructions */
      {
	switch (bits (insn1, 8, 11))
	  {
	  case 1: case 3:  case 9: case 11: /* CBNZ, CBZ */
	    err = thumb_copy_cbnz_cbz (gdbarch, insn1, regs, dsc);
	    break;
	  case 12: case 13: /* POP */
	    if (bit (insn1, 8)) /* PC is in register list.  */
	      err = thumb_copy_pop_pc_16bit (gdbarch, insn1, regs, dsc);
	    else
	      err = thumb_copy_unmodified_16bit (gdbarch, insn1, "pop", dsc);
	    break;
	  case 15: /* If-Then, and hints */
	    if (bits (insn1, 0, 3))
	      /* If-Then makes up to four following instructions conditional.
		 IT instruction itself is not conditional, so handle it as a
		 common unmodified instruction.  */
	      err = thumb_copy_unmodified_16bit (gdbarch, insn1, "If-Then",
						 dsc);
	    else
	      err = thumb_copy_unmodified_16bit (gdbarch, insn1, "hints", dsc);
	    break;
	  default:
	    err = thumb_copy_unmodified_16bit (gdbarch, insn1, "misc", dsc);
	  }
      }
      break;
    case 12:
      if (op_bit_10_11 < 2) /* Store multiple registers */
	err = thumb_copy_unmodified_16bit (gdbarch, insn1, "stm", dsc);
      else /* Load multiple registers */
	err = thumb_copy_unmodified_16bit (gdbarch, insn1, "ldm", dsc);
      break;
    case 13: /* Conditional branch and supervisor call */
      if (bits (insn1, 9, 11) != 7) /* conditional branch */
	err = thumb_copy_b (gdbarch, insn1, dsc);
      else
	err = thumb_copy_svc (gdbarch, insn1, regs, dsc);
      break;
    case 14: /* Unconditional branch */
      err = thumb_copy_b (gdbarch, insn1, dsc);
      break;
    default:
      err = 1;
    }

  if (err)
    internal_error (__FILE__, __LINE__,
		    _("thumb_process_displaced_16bit_insn: Instruction decode error"));
}

static int
decode_thumb_32bit_ld_mem_hints (struct gdbarch *gdbarch,
				 uint16_t insn1, uint16_t insn2,
				 struct regcache *regs,
				 struct displaced_step_closure *dsc)
{
  int rt = bits (insn2, 12, 15);
  int rn = bits (insn1, 0, 3);
  int op1 = bits (insn1, 7, 8);
  int err = 0;

  switch (bits (insn1, 5, 6))
    {
    case 0: /* Load byte and memory hints */
      if (rt == 0xf) /* PLD/PLI */
	{
	  if (rn == 0xf)
	    /* PLD literal or Encoding T3 of PLI(immediate, literal).  */
	    return thumb2_copy_preload (gdbarch, insn1, insn2, regs, dsc);
	  else
	    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						"pli/pld", dsc);
	}
      else
	{
	  if (rn == 0xf) /* LDRB/LDRSB (literal) */
	    return thumb2_copy_load_literal (gdbarch, insn1, insn2, regs, dsc,
					     1);
	  else
	    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						"ldrb{reg, immediate}/ldrbt",
						dsc);
	}

      break;
    case 1: /* Load halfword and memory hints.  */
      if (rt == 0xf) /* PLD{W} and Unalloc memory hint.  */
	return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					    "pld/unalloc memhint", dsc);
      else
	{
	  if (rn == 0xf)
	    return thumb2_copy_load_literal (gdbarch, insn1, insn2, regs, dsc,
					     2);
	  else
	    return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						"ldrh/ldrht", dsc);
	}
      break;
    case 2: /* Load word */
      {
	int insn2_bit_8_11 = bits (insn2, 8, 11);

	if (rn == 0xf)
	  return thumb2_copy_load_literal (gdbarch, insn1, insn2, regs, dsc, 4);
	else if (op1 == 0x1) /* Encoding T3 */
	  return thumb2_copy_load_reg_imm (gdbarch, insn1, insn2, regs, dsc,
					   0, 1);
	else /* op1 == 0x0 */
	  {
	    if (insn2_bit_8_11 == 0xc || (insn2_bit_8_11 & 0x9) == 0x9)
	      /* LDR (immediate) */
	      return thumb2_copy_load_reg_imm (gdbarch, insn1, insn2, regs,
					       dsc, bit (insn2, 8), 1);
	    else if (insn2_bit_8_11 == 0xe) /* LDRT */
	      return thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						  "ldrt", dsc);
	    else
	      /* LDR (register) */
	      return thumb2_copy_load_reg_imm (gdbarch, insn1, insn2, regs,
					       dsc, 0, 0);
	  }
	break;
      }
    default:
      return thumb_32bit_copy_undef (gdbarch, insn1, insn2, dsc);
      break;
    }
  return 0;
}

static void
thumb_process_displaced_32bit_insn (struct gdbarch *gdbarch, uint16_t insn1,
				    uint16_t insn2, struct regcache *regs,
				    struct displaced_step_closure *dsc)
{
  int err = 0;
  unsigned short op = bit (insn2, 15);
  unsigned int op1 = bits (insn1, 11, 12);

  switch (op1)
    {
    case 1:
      {
	switch (bits (insn1, 9, 10))
	  {
	  case 0:
	    if (bit (insn1, 6))
	      {
		/* Load/store {dual, execlusive}, table branch.  */
		if (bits (insn1, 7, 8) == 1 && bits (insn1, 4, 5) == 1
		    && bits (insn2, 5, 7) == 0)
		  err = thumb2_copy_table_branch (gdbarch, insn1, insn2, regs,
						  dsc);
		else
		  /* PC is not allowed to use in load/store {dual, exclusive}
		     instructions.  */
		  err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						     "load/store dual/ex", dsc);
	      }
	    else /* load/store multiple */
	      {
		switch (bits (insn1, 7, 8))
		  {
		  case 0: case 3: /* SRS, RFE */
		    err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						       "srs/rfe", dsc);
		    break;
		  case 1: case 2: /* LDM/STM/PUSH/POP */
		    err = thumb2_copy_block_xfer (gdbarch, insn1, insn2, regs, dsc);
		    break;
		  }
	      }
	    break;

	  case 1:
	    /* Data-processing (shift register).  */
	    err = thumb2_decode_dp_shift_reg (gdbarch, insn1, insn2, regs,
					      dsc);
	    break;
	  default: /* Coprocessor instructions.  */
	    err = thumb2_decode_svc_copro (gdbarch, insn1, insn2, regs, dsc);
	    break;
	  }
      break;
      }
    case 2: /* op1 = 2 */
      if (op) /* Branch and misc control.  */
	{
	  if (bit (insn2, 14)  /* BLX/BL */
	      || bit (insn2, 12) /* Unconditional branch */
	      || (bits (insn1, 7, 9) != 0x7)) /* Conditional branch */
	    err = thumb2_copy_b_bl_blx (gdbarch, insn1, insn2, regs, dsc);
	  else
	    err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					       "misc ctrl", dsc);
	}
      else
	{
	  if (bit (insn1, 9)) /* Data processing (plain binary imm).  */
	    {
	      int op = bits (insn1, 4, 8);
	      int rn = bits (insn1, 0, 3);
	      if ((op == 0 || op == 0xa) && rn == 0xf)
		err = thumb_copy_pc_relative_32bit (gdbarch, insn1, insn2,
						    regs, dsc);
	      else
		err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						   "dp/pb", dsc);
	    }
	  else /* Data processing (modified immeidate) */
	    err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					       "dp/mi", dsc);
	}
      break;
    case 3: /* op1 = 3 */
      switch (bits (insn1, 9, 10))
	{
	case 0:
	  if (bit (insn1, 4))
	    err = decode_thumb_32bit_ld_mem_hints (gdbarch, insn1, insn2,
						   regs, dsc);
	  else /* NEON Load/Store and Store single data item */
	    err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
					       "neon elt/struct load/store",
					       dsc);
	  break;
	case 1: /* op1 = 3, bits (9, 10) == 1 */
	  switch (bits (insn1, 7, 8))
	    {
	    case 0: case 1: /* Data processing (register) */
	      err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						 "dp(reg)", dsc);
	      break;
	    case 2: /* Multiply and absolute difference */
	      err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						 "mul/mua/diff", dsc);
	      break;
	    case 3: /* Long multiply and divide */
	      err = thumb_copy_unmodified_32bit (gdbarch, insn1, insn2,
						 "lmul/lmua", dsc);
	      break;
	    }
	  break;
	default: /* Coprocessor instructions */
	  err = thumb2_decode_svc_copro (gdbarch, insn1, insn2, regs, dsc);
	  break;
	}
      break;
    default:
      err = 1;
    }

  if (err)
    internal_error (__FILE__, __LINE__,
		    _("thumb_process_displaced_32bit_insn: Instruction decode error"));

}

static void
thumb_process_displaced_insn (struct gdbarch *gdbarch, CORE_ADDR from,
			      CORE_ADDR to, struct regcache *regs,
			      struct displaced_step_closure *dsc)
{
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  uint16_t insn1
    = read_memory_unsigned_integer (from, 2, byte_order_for_code);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: process thumb insn %.4x "
			"at %.8lx\n", insn1, (unsigned long) from);

  dsc->is_thumb = 1;
  dsc->insn_size = thumb_insn_size (insn1);
  if (thumb_insn_size (insn1) == 4)
    {
      uint16_t insn2
	= read_memory_unsigned_integer (from + 2, 2, byte_order_for_code);
      thumb_process_displaced_32bit_insn (gdbarch, insn1, insn2, regs, dsc);
    }
  else
    thumb_process_displaced_16bit_insn (gdbarch, insn1, regs, dsc);
}

void
arm_process_displaced_insn (struct gdbarch *gdbarch, CORE_ADDR from,
			    CORE_ADDR to, struct regcache *regs,
			    struct displaced_step_closure *dsc)
{
  int err = 0;
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  uint32_t insn;

  /* Most displaced instructions use a 1-instruction scratch space, so set this
     here and override below if/when necessary.  */
  dsc->numinsns = 1;
  dsc->insn_addr = from;
  dsc->scratch_base = to;
  dsc->cleanup = NULL;
  dsc->wrote_to_pc = 0;

  if (!displaced_in_arm_mode (regs))
    return thumb_process_displaced_insn (gdbarch, from, to, regs, dsc);

  dsc->is_thumb = 0;
  dsc->insn_size = 4;
  insn = read_memory_unsigned_integer (from, 4, byte_order_for_code);
  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: stepping insn %.8lx "
			"at %.8lx\n", (unsigned long) insn,
			(unsigned long) from);

  if ((insn & 0xf0000000) == 0xf0000000)
    err = arm_decode_unconditional (gdbarch, insn, regs, dsc);
  else switch (((insn & 0x10) >> 4) | ((insn & 0xe000000) >> 24))
    {
    case 0x0: case 0x1: case 0x2: case 0x3:
      err = arm_decode_dp_misc (gdbarch, insn, regs, dsc);
      break;

    case 0x4: case 0x5: case 0x6:
      err = arm_decode_ld_st_word_ubyte (gdbarch, insn, regs, dsc);
      break;

    case 0x7:
      err = arm_decode_media (gdbarch, insn, dsc);
      break;

    case 0x8: case 0x9: case 0xa: case 0xb:
      err = arm_decode_b_bl_ldmstm (gdbarch, insn, regs, dsc);
      break;

    case 0xc: case 0xd: case 0xe: case 0xf:
      err = arm_decode_svc_copro (gdbarch, insn, to, regs, dsc);
      break;
    }

  if (err)
    internal_error (__FILE__, __LINE__,
		    _("arm_process_displaced_insn: Instruction decode error"));
}

/* Actually set up the scratch space for a displaced instruction.  */

void
arm_displaced_init_closure (struct gdbarch *gdbarch, CORE_ADDR from,
			    CORE_ADDR to, struct displaced_step_closure *dsc)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  unsigned int i, len, offset;
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);
  int size = dsc->is_thumb? 2 : 4;
  const unsigned char *bkp_insn;

  offset = 0;
  /* Poke modified instruction(s).  */
  for (i = 0; i < dsc->numinsns; i++)
    {
      if (debug_displaced)
	{
	  fprintf_unfiltered (gdb_stdlog, "displaced: writing insn ");
	  if (size == 4)
	    fprintf_unfiltered (gdb_stdlog, "%.8lx",
				dsc->modinsn[i]);
	  else if (size == 2)
	    fprintf_unfiltered (gdb_stdlog, "%.4x",
				(unsigned short)dsc->modinsn[i]);

	  fprintf_unfiltered (gdb_stdlog, " at %.8lx\n",
			      (unsigned long) to + offset);

	}
      write_memory_unsigned_integer (to + offset, size,
				     byte_order_for_code,
				     dsc->modinsn[i]);
      offset += size;
    }

  /* Choose the correct breakpoint instruction.  */
  if (dsc->is_thumb)
    {
      bkp_insn = tdep->thumb_breakpoint;
      len = tdep->thumb_breakpoint_size;
    }
  else
    {
      bkp_insn = tdep->arm_breakpoint;
      len = tdep->arm_breakpoint_size;
    }

  /* Put breakpoint afterwards.  */
  write_memory (to + offset, bkp_insn, len);

  if (debug_displaced)
    fprintf_unfiltered (gdb_stdlog, "displaced: copy %s->%s: ",
			paddress (gdbarch, from), paddress (gdbarch, to));
}

/* Entry point for copying an instruction into scratch space for displaced
   stepping.  */

struct displaced_step_closure *
arm_displaced_step_copy_insn (struct gdbarch *gdbarch,
			      CORE_ADDR from, CORE_ADDR to,
			      struct regcache *regs)
{
  struct displaced_step_closure *dsc
    = xmalloc (sizeof (struct displaced_step_closure));
  arm_process_displaced_insn (gdbarch, from, to, regs, dsc);
  arm_displaced_init_closure (gdbarch, from, to, dsc);

  return dsc;
}

/* Entry point for cleaning things up after a displaced instruction has been
   single-stepped.  */

void
arm_displaced_step_fixup (struct gdbarch *gdbarch,
			  struct displaced_step_closure *dsc,
			  CORE_ADDR from, CORE_ADDR to,
			  struct regcache *regs)
{
  if (dsc->cleanup)
    dsc->cleanup (gdbarch, regs, dsc);

  if (!dsc->wrote_to_pc)
    regcache_cooked_write_unsigned (regs, ARM_PC_REGNUM,
				    dsc->insn_addr + dsc->insn_size);

}

#include "bfd-in2.h"
#include "libcoff.h"

static int
gdb_print_insn_arm (bfd_vma memaddr, disassemble_info *info)
{
  struct gdbarch *gdbarch = info->application_data;

  if (arm_pc_is_thumb (gdbarch, memaddr))
    {
      static asymbol *asym;
      static combined_entry_type ce;
      static struct coff_symbol_struct csym;
      static struct bfd fake_bfd;
      static bfd_target fake_target;

      if (csym.native == NULL)
	{
	  /* Create a fake symbol vector containing a Thumb symbol.
	     This is solely so that the code in print_insn_little_arm() 
	     and print_insn_big_arm() in opcodes/arm-dis.c will detect
	     the presence of a Thumb symbol and switch to decoding
	     Thumb instructions.  */

	  fake_target.flavour = bfd_target_coff_flavour;
	  fake_bfd.xvec = &fake_target;
	  ce.u.syment.n_sclass = C_THUMBEXTFUNC;
	  csym.native = &ce;
	  csym.symbol.the_bfd = &fake_bfd;
	  csym.symbol.name = "fake";
	  asym = (asymbol *) & csym;
	}

      memaddr = UNMAKE_THUMB_ADDR (memaddr);
      info->symbols = &asym;
    }
  else
    info->symbols = NULL;

  if (info->endian == BFD_ENDIAN_BIG)
    return print_insn_big_arm (memaddr, info);
  else
    return print_insn_little_arm (memaddr, info);
}

/* The following define instruction sequences that will cause ARM
   cpu's to take an undefined instruction trap.  These are used to
   signal a breakpoint to GDB.
   
   The newer ARMv4T cpu's are capable of operating in ARM or Thumb
   modes.  A different instruction is required for each mode.  The ARM
   cpu's can also be big or little endian.  Thus four different
   instructions are needed to support all cases.
   
   Note: ARMv4 defines several new instructions that will take the
   undefined instruction trap.  ARM7TDMI is nominally ARMv4T, but does
   not in fact add the new instructions.  The new undefined
   instructions in ARMv4 are all instructions that had no defined
   behaviour in earlier chips.  There is no guarantee that they will
   raise an exception, but may be treated as NOP's.  In practice, it
   may only safe to rely on instructions matching:
   
   3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 
   1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
   C C C C 0 1 1 x x x x x x x x x x x x x x x x x x x x 1 x x x x
   
   Even this may only true if the condition predicate is true.  The
   following use a condition predicate of ALWAYS so it is always TRUE.
   
   There are other ways of forcing a breakpoint.  GNU/Linux, RISC iX,
   and NetBSD all use a software interrupt rather than an undefined
   instruction to force a trap.  This can be handled by by the
   abi-specific code during establishment of the gdbarch vector.  */

#define ARM_LE_BREAKPOINT {0xFE,0xDE,0xFF,0xE7}
#define ARM_BE_BREAKPOINT {0xE7,0xFF,0xDE,0xFE}
#define THUMB_LE_BREAKPOINT {0xbe,0xbe}
#define THUMB_BE_BREAKPOINT {0xbe,0xbe}

static const char arm_default_arm_le_breakpoint[] = ARM_LE_BREAKPOINT;
static const char arm_default_arm_be_breakpoint[] = ARM_BE_BREAKPOINT;
static const char arm_default_thumb_le_breakpoint[] = THUMB_LE_BREAKPOINT;
static const char arm_default_thumb_be_breakpoint[] = THUMB_BE_BREAKPOINT;

/* Determine the type and size of breakpoint to insert at PCPTR.  Uses
   the program counter value to determine whether a 16-bit or 32-bit
   breakpoint should be used.  It returns a pointer to a string of
   bytes that encode a breakpoint instruction, stores the length of
   the string to *lenptr, and adjusts the program counter (if
   necessary) to point to the actual memory location where the
   breakpoint should be inserted.  */

static const unsigned char *
arm_breakpoint_from_pc (struct gdbarch *gdbarch, CORE_ADDR *pcptr, int *lenptr)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  enum bfd_endian byte_order_for_code = gdbarch_byte_order_for_code (gdbarch);

  if (arm_pc_is_thumb (gdbarch, *pcptr))
    {
      *pcptr = UNMAKE_THUMB_ADDR (*pcptr);

      /* If we have a separate 32-bit breakpoint instruction for Thumb-2,
	 check whether we are replacing a 32-bit instruction.  */
      if (tdep->thumb2_breakpoint != NULL)
	{
	  gdb_byte buf[2];
	  if (target_read_memory (*pcptr, buf, 2) == 0)
	    {
	      unsigned short inst1;
	      inst1 = extract_unsigned_integer (buf, 2, byte_order_for_code);
	      if (thumb_insn_size (inst1) == 4)
		{
		  *lenptr = tdep->thumb2_breakpoint_size;
		  return tdep->thumb2_breakpoint;
		}
	    }
	}

      *lenptr = tdep->thumb_breakpoint_size;
      return tdep->thumb_breakpoint;
    }
  else
    {
      *lenptr = tdep->arm_breakpoint_size;
      return tdep->arm_breakpoint;
    }
}

static void
arm_remote_breakpoint_from_pc (struct gdbarch *gdbarch, CORE_ADDR *pcptr,
			       int *kindptr)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  arm_breakpoint_from_pc (gdbarch, pcptr, kindptr);

  if (arm_pc_is_thumb (gdbarch, *pcptr) && *kindptr == 4)
    /* The documented magic value for a 32-bit Thumb-2 breakpoint, so
       that this is not confused with a 32-bit ARM breakpoint.  */
    *kindptr = 3;
}

/* Extract from an array REGBUF containing the (raw) register state a
   function return value of type TYPE, and copy that, in virtual
   format, into VALBUF.  */

static void
arm_extract_return_value (struct type *type, struct regcache *regs,
			  gdb_byte *valbuf)
{
  struct gdbarch *gdbarch = get_regcache_arch (regs);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  if (TYPE_CODE_FLT == TYPE_CODE (type))
    {
      switch (gdbarch_tdep (gdbarch)->fp_model)
	{
	case ARM_FLOAT_FPA:
	  {
	    /* The value is in register F0 in internal format.  We need to
	       extract the raw value and then convert it to the desired
	       internal type.  */
	    bfd_byte tmpbuf[FP_REGISTER_SIZE];

	    regcache_cooked_read (regs, ARM_F0_REGNUM, tmpbuf);
	    convert_from_extended (floatformat_from_type (type), tmpbuf,
				   valbuf, gdbarch_byte_order (gdbarch));
	  }
	  break;

	case ARM_FLOAT_SOFT_FPA:
	case ARM_FLOAT_SOFT_VFP:
	  /* ARM_FLOAT_VFP can arise if this is a variadic function so
	     not using the VFP ABI code.  */
	case ARM_FLOAT_VFP:
	  regcache_cooked_read (regs, ARM_A1_REGNUM, valbuf);
	  if (TYPE_LENGTH (type) > 4)
	    regcache_cooked_read (regs, ARM_A1_REGNUM + 1,
				  valbuf + INT_REGISTER_SIZE);
	  break;

	default:
	  internal_error (__FILE__, __LINE__,
			  _("arm_extract_return_value: "
			    "Floating point model not supported"));
	  break;
	}
    }
  else if (TYPE_CODE (type) == TYPE_CODE_INT
	   || TYPE_CODE (type) == TYPE_CODE_CHAR
	   || TYPE_CODE (type) == TYPE_CODE_BOOL
	   || TYPE_CODE (type) == TYPE_CODE_PTR
	   || TYPE_CODE (type) == TYPE_CODE_REF
	   || TYPE_CODE (type) == TYPE_CODE_ENUM)
    {
      /* If the type is a plain integer, then the access is
	 straight-forward.  Otherwise we have to play around a bit
	 more.  */
      int len = TYPE_LENGTH (type);
      int regno = ARM_A1_REGNUM;
      ULONGEST tmp;

      while (len > 0)
	{
	  /* By using store_unsigned_integer we avoid having to do
	     anything special for small big-endian values.  */
	  regcache_cooked_read_unsigned (regs, regno++, &tmp);
	  store_unsigned_integer (valbuf, 
				  (len > INT_REGISTER_SIZE
				   ? INT_REGISTER_SIZE : len),
				  byte_order, tmp);
	  len -= INT_REGISTER_SIZE;
	  valbuf += INT_REGISTER_SIZE;
	}
    }
  else
    {
      /* For a structure or union the behaviour is as if the value had
         been stored to word-aligned memory and then loaded into 
         registers with 32-bit load instruction(s).  */
      int len = TYPE_LENGTH (type);
      int regno = ARM_A1_REGNUM;
      bfd_byte tmpbuf[INT_REGISTER_SIZE];

      while (len > 0)
	{
	  regcache_cooked_read (regs, regno++, tmpbuf);
	  memcpy (valbuf, tmpbuf,
		  len > INT_REGISTER_SIZE ? INT_REGISTER_SIZE : len);
	  len -= INT_REGISTER_SIZE;
	  valbuf += INT_REGISTER_SIZE;
	}
    }
}


/* Will a function return an aggregate type in memory or in a
   register?  Return 0 if an aggregate type can be returned in a
   register, 1 if it must be returned in memory.  */

static int
arm_return_in_memory (struct gdbarch *gdbarch, struct type *type)
{
  int nRc;
  enum type_code code;

  CHECK_TYPEDEF (type);

  /* In the ARM ABI, "integer" like aggregate types are returned in
     registers.  For an aggregate type to be integer like, its size
     must be less than or equal to INT_REGISTER_SIZE and the
     offset of each addressable subfield must be zero.  Note that bit
     fields are not addressable, and all addressable subfields of
     unions always start at offset zero.

     This function is based on the behaviour of GCC 2.95.1.
     See: gcc/arm.c: arm_return_in_memory() for details.

     Note: All versions of GCC before GCC 2.95.2 do not set up the
     parameters correctly for a function returning the following
     structure: struct { float f;}; This should be returned in memory,
     not a register.  Richard Earnshaw sent me a patch, but I do not
     know of any way to detect if a function like the above has been
     compiled with the correct calling convention.  */

  /* All aggregate types that won't fit in a register must be returned
     in memory.  */
  if (TYPE_LENGTH (type) > INT_REGISTER_SIZE)
    {
      return 1;
    }

  /* The AAPCS says all aggregates not larger than a word are returned
     in a register.  */
  if (gdbarch_tdep (gdbarch)->arm_abi != ARM_ABI_APCS)
    return 0;

  /* The only aggregate types that can be returned in a register are
     structs and unions.  Arrays must be returned in memory.  */
  code = TYPE_CODE (type);
  if ((TYPE_CODE_STRUCT != code) && (TYPE_CODE_UNION != code))
    {
      return 1;
    }

  /* Assume all other aggregate types can be returned in a register.
     Run a check for structures, unions and arrays.  */
  nRc = 0;

  if ((TYPE_CODE_STRUCT == code) || (TYPE_CODE_UNION == code))
    {
      int i;
      /* Need to check if this struct/union is "integer" like.  For
         this to be true, its size must be less than or equal to
         INT_REGISTER_SIZE and the offset of each addressable
         subfield must be zero.  Note that bit fields are not
         addressable, and unions always start at offset zero.  If any
         of the subfields is a floating point type, the struct/union
         cannot be an integer type.  */

      /* For each field in the object, check:
         1) Is it FP? --> yes, nRc = 1;
         2) Is it addressable (bitpos != 0) and
         not packed (bitsize == 0)?
         --> yes, nRc = 1  
       */

      for (i = 0; i < TYPE_NFIELDS (type); i++)
	{
	  enum type_code field_type_code;
	  field_type_code = TYPE_CODE (check_typedef (TYPE_FIELD_TYPE (type,
								       i)));

	  /* Is it a floating point type field?  */
	  if (field_type_code == TYPE_CODE_FLT)
	    {
	      nRc = 1;
	      break;
	    }

	  /* If bitpos != 0, then we have to care about it.  */
	  if (TYPE_FIELD_BITPOS (type, i) != 0)
	    {
	      /* Bitfields are not addressable.  If the field bitsize is 
	         zero, then the field is not packed.  Hence it cannot be
	         a bitfield or any other packed type.  */
	      if (TYPE_FIELD_BITSIZE (type, i) == 0)
		{
		  nRc = 1;
		  break;
		}
	    }
	}
    }

  return nRc;
}

/* Write into appropriate registers a function return value of type
   TYPE, given in virtual format.  */

static void
arm_store_return_value (struct type *type, struct regcache *regs,
			const gdb_byte *valbuf)
{
  struct gdbarch *gdbarch = get_regcache_arch (regs);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);

  if (TYPE_CODE (type) == TYPE_CODE_FLT)
    {
      char buf[MAX_REGISTER_SIZE];

      switch (gdbarch_tdep (gdbarch)->fp_model)
	{
	case ARM_FLOAT_FPA:

	  convert_to_extended (floatformat_from_type (type), buf, valbuf,
			       gdbarch_byte_order (gdbarch));
	  regcache_cooked_write (regs, ARM_F0_REGNUM, buf);
	  break;

	case ARM_FLOAT_SOFT_FPA:
	case ARM_FLOAT_SOFT_VFP:
	  /* ARM_FLOAT_VFP can arise if this is a variadic function so
	     not using the VFP ABI code.  */
	case ARM_FLOAT_VFP:
	  regcache_cooked_write (regs, ARM_A1_REGNUM, valbuf);
	  if (TYPE_LENGTH (type) > 4)
	    regcache_cooked_write (regs, ARM_A1_REGNUM + 1, 
				   valbuf + INT_REGISTER_SIZE);
	  break;

	default:
	  internal_error (__FILE__, __LINE__,
			  _("arm_store_return_value: Floating "
			    "point model not supported"));
	  break;
	}
    }
  else if (TYPE_CODE (type) == TYPE_CODE_INT
	   || TYPE_CODE (type) == TYPE_CODE_CHAR
	   || TYPE_CODE (type) == TYPE_CODE_BOOL
	   || TYPE_CODE (type) == TYPE_CODE_PTR
	   || TYPE_CODE (type) == TYPE_CODE_REF
	   || TYPE_CODE (type) == TYPE_CODE_ENUM)
    {
      if (TYPE_LENGTH (type) <= 4)
	{
	  /* Values of one word or less are zero/sign-extended and
	     returned in r0.  */
	  bfd_byte tmpbuf[INT_REGISTER_SIZE];
	  LONGEST val = unpack_long (type, valbuf);

	  store_signed_integer (tmpbuf, INT_REGISTER_SIZE, byte_order, val);
	  regcache_cooked_write (regs, ARM_A1_REGNUM, tmpbuf);
	}
      else
	{
	  /* Integral values greater than one word are stored in consecutive
	     registers starting with r0.  This will always be a multiple of
	     the regiser size.  */
	  int len = TYPE_LENGTH (type);
	  int regno = ARM_A1_REGNUM;

	  while (len > 0)
	    {
	      regcache_cooked_write (regs, regno++, valbuf);
	      len -= INT_REGISTER_SIZE;
	      valbuf += INT_REGISTER_SIZE;
	    }
	}
    }
  else
    {
      /* For a structure or union the behaviour is as if the value had
         been stored to word-aligned memory and then loaded into 
         registers with 32-bit load instruction(s).  */
      int len = TYPE_LENGTH (type);
      int regno = ARM_A1_REGNUM;
      bfd_byte tmpbuf[INT_REGISTER_SIZE];

      while (len > 0)
	{
	  memcpy (tmpbuf, valbuf,
		  len > INT_REGISTER_SIZE ? INT_REGISTER_SIZE : len);
	  regcache_cooked_write (regs, regno++, tmpbuf);
	  len -= INT_REGISTER_SIZE;
	  valbuf += INT_REGISTER_SIZE;
	}
    }
}


/* Handle function return values.  */

static enum return_value_convention
arm_return_value (struct gdbarch *gdbarch, struct type *func_type,
		  struct type *valtype, struct regcache *regcache,
		  gdb_byte *readbuf, const gdb_byte *writebuf)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  enum arm_vfp_cprc_base_type vfp_base_type;
  int vfp_base_count;

  if (arm_vfp_abi_for_function (gdbarch, func_type)
      && arm_vfp_call_candidate (valtype, &vfp_base_type, &vfp_base_count))
    {
      int reg_char = arm_vfp_cprc_reg_char (vfp_base_type);
      int unit_length = arm_vfp_cprc_unit_length (vfp_base_type);
      int i;
      for (i = 0; i < vfp_base_count; i++)
	{
	  if (reg_char == 'q')
	    {
	      if (writebuf)
		arm_neon_quad_write (gdbarch, regcache, i,
				     writebuf + i * unit_length);

	      if (readbuf)
		arm_neon_quad_read (gdbarch, regcache, i,
				    readbuf + i * unit_length);
	    }
	  else
	    {
	      char name_buf[4];
	      int regnum;

	      sprintf (name_buf, "%c%d", reg_char, i);
	      regnum = user_reg_map_name_to_regnum (gdbarch, name_buf,
						    strlen (name_buf));
	      if (writebuf)
		regcache_cooked_write (regcache, regnum,
				       writebuf + i * unit_length);
	      if (readbuf)
		regcache_cooked_read (regcache, regnum,
				      readbuf + i * unit_length);
	    }
	}
      return RETURN_VALUE_REGISTER_CONVENTION;
    }

  if (TYPE_CODE (valtype) == TYPE_CODE_STRUCT
      || TYPE_CODE (valtype) == TYPE_CODE_UNION
      || TYPE_CODE (valtype) == TYPE_CODE_ARRAY)
    {
      if (tdep->struct_return == pcc_struct_return
	  || arm_return_in_memory (gdbarch, valtype))
	return RETURN_VALUE_STRUCT_CONVENTION;
    }

  /* AAPCS returns complex types longer than a register in memory.  */
  if (tdep->arm_abi != ARM_ABI_APCS
      && TYPE_CODE (valtype) == TYPE_CODE_COMPLEX
      && TYPE_LENGTH (valtype) > INT_REGISTER_SIZE)
    return RETURN_VALUE_STRUCT_CONVENTION;

  if (writebuf)
    arm_store_return_value (valtype, regcache, writebuf);

  if (readbuf)
    arm_extract_return_value (valtype, regcache, readbuf);

  return RETURN_VALUE_REGISTER_CONVENTION;
}


static int
arm_get_longjmp_target (struct frame_info *frame, CORE_ADDR *pc)
{
  struct gdbarch *gdbarch = get_frame_arch (frame);
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);
  enum bfd_endian byte_order = gdbarch_byte_order (gdbarch);
  CORE_ADDR jb_addr;
  char buf[INT_REGISTER_SIZE];
  
  jb_addr = get_frame_register_unsigned (frame, ARM_A1_REGNUM);

  if (target_read_memory (jb_addr + tdep->jb_pc * tdep->jb_elt_size, buf,
			  INT_REGISTER_SIZE))
    return 0;

  *pc = extract_unsigned_integer (buf, INT_REGISTER_SIZE, byte_order);
  return 1;
}

/* Recognize GCC and GNU ld's trampolines.  If we are in a trampoline,
   return the target PC.  Otherwise return 0.  */

CORE_ADDR
arm_skip_stub (struct frame_info *frame, CORE_ADDR pc)
{
  char *name;
  int namelen;
  CORE_ADDR start_addr;

  /* Find the starting address and name of the function containing the PC.  */
  if (find_pc_partial_function (pc, &name, &start_addr, NULL) == 0)
    return 0;

  /* If PC is in a Thumb call or return stub, return the address of the
     target PC, which is in a register.  The thunk functions are called
     _call_via_xx, where x is the register name.  The possible names
     are r0-r9, sl, fp, ip, sp, and lr.  ARM RealView has similar
     functions, named __ARM_call_via_r[0-7].  */
  if (strncmp (name, "_call_via_", 10) == 0
      || strncmp (name, "__ARM_call_via_", strlen ("__ARM_call_via_")) == 0)
    {
      /* Use the name suffix to determine which register contains the
         target PC.  */
      static char *table[15] =
      {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
       "r8", "r9", "sl", "fp", "ip", "sp", "lr"
      };
      int regno;
      int offset = strlen (name) - 2;

      for (regno = 0; regno <= 14; regno++)
	if (strcmp (&name[offset], table[regno]) == 0)
	  return get_frame_register_unsigned (frame, regno);
    }

  /* GNU ld generates __foo_from_arm or __foo_from_thumb for
     non-interworking calls to foo.  We could decode the stubs
     to find the target but it's easier to use the symbol table.  */
  namelen = strlen (name);
  if (name[0] == '_' && name[1] == '_'
      && ((namelen > 2 + strlen ("_from_thumb")
	   && strncmp (name + namelen - strlen ("_from_thumb"), "_from_thumb",
		       strlen ("_from_thumb")) == 0)
	  || (namelen > 2 + strlen ("_from_arm")
	      && strncmp (name + namelen - strlen ("_from_arm"), "_from_arm",
			  strlen ("_from_arm")) == 0)))
    {
      char *target_name;
      int target_len = namelen - 2;
      struct minimal_symbol *minsym;
      struct objfile *objfile;
      struct obj_section *sec;

      if (name[namelen - 1] == 'b')
	target_len -= strlen ("_from_thumb");
      else
	target_len -= strlen ("_from_arm");

      target_name = alloca (target_len + 1);
      memcpy (target_name, name + 2, target_len);
      target_name[target_len] = '\0';

      sec = find_pc_section (pc);
      objfile = (sec == NULL) ? NULL : sec->objfile;
      minsym = lookup_minimal_symbol (target_name, NULL, objfile);
      if (minsym != NULL)
	return SYMBOL_VALUE_ADDRESS (minsym);
      else
	return 0;
    }

  return 0;			/* not a stub */
}

static void
set_arm_command (char *args, int from_tty)
{
  printf_unfiltered (_("\
\"set arm\" must be followed by an apporpriate subcommand.\n"));
  help_list (setarmcmdlist, "set arm ", all_commands, gdb_stdout);
}

static void
show_arm_command (char *args, int from_tty)
{
  cmd_show_list (showarmcmdlist, from_tty, "");
}

static void
arm_update_current_architecture (void)
{
  struct gdbarch_info info;

  /* If the current architecture is not ARM, we have nothing to do.  */
  if (gdbarch_bfd_arch_info (target_gdbarch)->arch != bfd_arch_arm)
    return;

  /* Update the architecture.  */
  gdbarch_info_init (&info);

  if (!gdbarch_update_p (info))
    internal_error (__FILE__, __LINE__, _("could not update architecture"));
}

static void
set_fp_model_sfunc (char *args, int from_tty,
		    struct cmd_list_element *c)
{
  enum arm_float_model fp_model;

  for (fp_model = ARM_FLOAT_AUTO; fp_model != ARM_FLOAT_LAST; fp_model++)
    if (strcmp (current_fp_model, fp_model_strings[fp_model]) == 0)
      {
	arm_fp_model = fp_model;
	break;
      }

  if (fp_model == ARM_FLOAT_LAST)
    internal_error (__FILE__, __LINE__, _("Invalid fp model accepted: %s."),
		    current_fp_model);

  arm_update_current_architecture ();
}

static void
show_fp_model (struct ui_file *file, int from_tty,
	       struct cmd_list_element *c, const char *value)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (target_gdbarch);

  if (arm_fp_model == ARM_FLOAT_AUTO
      && gdbarch_bfd_arch_info (target_gdbarch)->arch == bfd_arch_arm)
    fprintf_filtered (file, _("\
The current ARM floating point model is \"auto\" (currently \"%s\").\n"),
		      fp_model_strings[tdep->fp_model]);
  else
    fprintf_filtered (file, _("\
The current ARM floating point model is \"%s\".\n"),
		      fp_model_strings[arm_fp_model]);
}

static void
arm_set_abi (char *args, int from_tty,
	     struct cmd_list_element *c)
{
  enum arm_abi_kind arm_abi;

  for (arm_abi = ARM_ABI_AUTO; arm_abi != ARM_ABI_LAST; arm_abi++)
    if (strcmp (arm_abi_string, arm_abi_strings[arm_abi]) == 0)
      {
	arm_abi_global = arm_abi;
	break;
      }

  if (arm_abi == ARM_ABI_LAST)
    internal_error (__FILE__, __LINE__, _("Invalid ABI accepted: %s."),
		    arm_abi_string);

  arm_update_current_architecture ();
}

static void
arm_show_abi (struct ui_file *file, int from_tty,
	     struct cmd_list_element *c, const char *value)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (target_gdbarch);

  if (arm_abi_global == ARM_ABI_AUTO
      && gdbarch_bfd_arch_info (target_gdbarch)->arch == bfd_arch_arm)
    fprintf_filtered (file, _("\
The current ARM ABI is \"auto\" (currently \"%s\").\n"),
		      arm_abi_strings[tdep->arm_abi]);
  else
    fprintf_filtered (file, _("The current ARM ABI is \"%s\".\n"),
		      arm_abi_string);
}

static void
arm_show_fallback_mode (struct ui_file *file, int from_tty,
			struct cmd_list_element *c, const char *value)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (target_gdbarch);

  fprintf_filtered (file,
		    _("The current execution mode assumed "
		      "(when symbols are unavailable) is \"%s\".\n"),
		    arm_fallback_mode_string);
}

static void
arm_show_force_mode (struct ui_file *file, int from_tty,
		     struct cmd_list_element *c, const char *value)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (target_gdbarch);

  fprintf_filtered (file,
		    _("The current execution mode assumed "
		      "(even when symbols are available) is \"%s\".\n"),
		    arm_force_mode_string);
}

/* If the user changes the register disassembly style used for info
   register and other commands, we have to also switch the style used
   in opcodes for disassembly output.  This function is run in the "set
   arm disassembly" command, and does that.  */

static void
set_disassembly_style_sfunc (char *args, int from_tty,
			      struct cmd_list_element *c)
{
  set_disassembly_style ();
}

/* Return the ARM register name corresponding to register I.  */
static const char *
arm_register_name (struct gdbarch *gdbarch, int i)
{
  const int num_regs = gdbarch_num_regs (gdbarch);

  if (gdbarch_tdep (gdbarch)->have_vfp_pseudos
      && i >= num_regs && i < num_regs + 32)
    {
      static const char *const vfp_pseudo_names[] = {
	"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
	"s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
	"s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
	"s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31",
      };

      return vfp_pseudo_names[i - num_regs];
    }

  if (gdbarch_tdep (gdbarch)->have_neon_pseudos
      && i >= num_regs + 32 && i < num_regs + 32 + 16)
    {
      static const char *const neon_pseudo_names[] = {
	"q0", "q1", "q2", "q3", "q4", "q5", "q6", "q7",
	"q8", "q9", "q10", "q11", "q12", "q13", "q14", "q15",
      };

      return neon_pseudo_names[i - num_regs - 32];
    }

  if (i >= ARRAY_SIZE (arm_register_names))
    /* These registers are only supported on targets which supply
       an XML description.  */
    return "";

  return arm_register_names[i];
}

static void
set_disassembly_style (void)
{
  int current;

  /* Find the style that the user wants.  */
  for (current = 0; current < num_disassembly_options; current++)
    if (disassembly_style == valid_disassembly_styles[current])
      break;
  gdb_assert (current < num_disassembly_options);

  /* Synchronize the disassembler.  */
  set_arm_regname_option (current);
}

/* Test whether the coff symbol specific value corresponds to a Thumb
   function.  */

static int
coff_sym_is_thumb (int val)
{
  return (val == C_THUMBEXT
	  || val == C_THUMBSTAT
	  || val == C_THUMBEXTFUNC
	  || val == C_THUMBSTATFUNC
	  || val == C_THUMBLABEL);
}

/* arm_coff_make_msymbol_special()
   arm_elf_make_msymbol_special()
   
   These functions test whether the COFF or ELF symbol corresponds to
   an address in thumb code, and set a "special" bit in a minimal
   symbol to indicate that it does.  */
   
static void
arm_elf_make_msymbol_special(asymbol *sym, struct minimal_symbol *msym)
{
  if (ARM_SYM_BRANCH_TYPE (&((elf_symbol_type *)sym)->internal_elf_sym)
      == ST_BRANCH_TO_THUMB)
    MSYMBOL_SET_SPECIAL (msym);
}

static void
arm_coff_make_msymbol_special(int val, struct minimal_symbol *msym)
{
  if (coff_sym_is_thumb (val))
    MSYMBOL_SET_SPECIAL (msym);
}

static void
arm_objfile_data_free (struct objfile *objfile, void *arg)
{
  struct arm_per_objfile *data = arg;
  unsigned int i;

  for (i = 0; i < objfile->obfd->section_count; i++)
    VEC_free (arm_mapping_symbol_s, data->section_maps[i]);
}

static void
arm_record_special_symbol (struct gdbarch *gdbarch, struct objfile *objfile,
			   asymbol *sym)
{
  const char *name = bfd_asymbol_name (sym);
  struct arm_per_objfile *data;
  VEC(arm_mapping_symbol_s) **map_p;
  struct arm_mapping_symbol new_map_sym;

  gdb_assert (name[0] == '$');
  if (name[1] != 'a' && name[1] != 't' && name[1] != 'd')
    return;

  data = objfile_data (objfile, arm_objfile_data_key);
  if (data == NULL)
    {
      data = OBSTACK_ZALLOC (&objfile->objfile_obstack,
			     struct arm_per_objfile);
      set_objfile_data (objfile, arm_objfile_data_key, data);
      data->section_maps = OBSTACK_CALLOC (&objfile->objfile_obstack,
					   objfile->obfd->section_count,
					   VEC(arm_mapping_symbol_s) *);
    }
  map_p = &data->section_maps[bfd_get_section (sym)->index];

  new_map_sym.value = sym->value;
  new_map_sym.type = name[1];

  /* Assume that most mapping symbols appear in order of increasing
     value.  If they were randomly distributed, it would be faster to
     always push here and then sort at first use.  */
  if (!VEC_empty (arm_mapping_symbol_s, *map_p))
    {
      struct arm_mapping_symbol *prev_map_sym;

      prev_map_sym = VEC_last (arm_mapping_symbol_s, *map_p);
      if (prev_map_sym->value >= sym->value)
	{
	  unsigned int idx;
	  idx = VEC_lower_bound (arm_mapping_symbol_s, *map_p, &new_map_sym,
				 arm_compare_mapping_symbols);
	  VEC_safe_insert (arm_mapping_symbol_s, *map_p, idx, &new_map_sym);
	  return;
	}
    }

  VEC_safe_push (arm_mapping_symbol_s, *map_p, &new_map_sym);
}

static void
arm_write_pc (struct regcache *regcache, CORE_ADDR pc)
{
  struct gdbarch *gdbarch = get_regcache_arch (regcache);
  regcache_cooked_write_unsigned (regcache, ARM_PC_REGNUM, pc);

  /* If necessary, set the T bit.  */
  if (arm_apcs_32)
    {
      ULONGEST val, t_bit;
      regcache_cooked_read_unsigned (regcache, ARM_PS_REGNUM, &val);
      t_bit = arm_psr_thumb_bit (gdbarch);
      if (arm_pc_is_thumb (gdbarch, pc))
	regcache_cooked_write_unsigned (regcache, ARM_PS_REGNUM,
					val | t_bit);
      else
	regcache_cooked_write_unsigned (regcache, ARM_PS_REGNUM,
					val & ~t_bit);
    }
}

/* Read the contents of a NEON quad register, by reading from two
   double registers.  This is used to implement the quad pseudo
   registers, and for argument passing in case the quad registers are
   missing; vectors are passed in quad registers when using the VFP
   ABI, even if a NEON unit is not present.  REGNUM is the index of
   the quad register, in [0, 15].  */

static enum register_status
arm_neon_quad_read (struct gdbarch *gdbarch, struct regcache *regcache,
		    int regnum, gdb_byte *buf)
{
  char name_buf[4];
  gdb_byte reg_buf[8];
  int offset, double_regnum;
  enum register_status status;

  sprintf (name_buf, "d%d", regnum << 1);
  double_regnum = user_reg_map_name_to_regnum (gdbarch, name_buf,
					       strlen (name_buf));

  /* d0 is always the least significant half of q0.  */
  if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
    offset = 8;
  else
    offset = 0;

  status = regcache_raw_read (regcache, double_regnum, reg_buf);
  if (status != REG_VALID)
    return status;
  memcpy (buf + offset, reg_buf, 8);

  offset = 8 - offset;
  status = regcache_raw_read (regcache, double_regnum + 1, reg_buf);
  if (status != REG_VALID)
    return status;
  memcpy (buf + offset, reg_buf, 8);

  return REG_VALID;
}

static enum register_status
arm_pseudo_read (struct gdbarch *gdbarch, struct regcache *regcache,
		 int regnum, gdb_byte *buf)
{
  const int num_regs = gdbarch_num_regs (gdbarch);
  char name_buf[4];
  gdb_byte reg_buf[8];
  int offset, double_regnum;

  gdb_assert (regnum >= num_regs);
  regnum -= num_regs;

  if (gdbarch_tdep (gdbarch)->have_neon_pseudos && regnum >= 32 && regnum < 48)
    /* Quad-precision register.  */
    return arm_neon_quad_read (gdbarch, regcache, regnum - 32, buf);
  else
    {
      enum register_status status;

      /* Single-precision register.  */
      gdb_assert (regnum < 32);

      /* s0 is always the least significant half of d0.  */
      if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
	offset = (regnum & 1) ? 0 : 4;
      else
	offset = (regnum & 1) ? 4 : 0;

      sprintf (name_buf, "d%d", regnum >> 1);
      double_regnum = user_reg_map_name_to_regnum (gdbarch, name_buf,
						   strlen (name_buf));

      status = regcache_raw_read (regcache, double_regnum, reg_buf);
      if (status == REG_VALID)
	memcpy (buf, reg_buf + offset, 4);
      return status;
    }
}

/* Store the contents of BUF to a NEON quad register, by writing to
   two double registers.  This is used to implement the quad pseudo
   registers, and for argument passing in case the quad registers are
   missing; vectors are passed in quad registers when using the VFP
   ABI, even if a NEON unit is not present.  REGNUM is the index
   of the quad register, in [0, 15].  */

static void
arm_neon_quad_write (struct gdbarch *gdbarch, struct regcache *regcache,
		     int regnum, const gdb_byte *buf)
{
  char name_buf[4];
  gdb_byte reg_buf[8];
  int offset, double_regnum;

  sprintf (name_buf, "d%d", regnum << 1);
  double_regnum = user_reg_map_name_to_regnum (gdbarch, name_buf,
					       strlen (name_buf));

  /* d0 is always the least significant half of q0.  */
  if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
    offset = 8;
  else
    offset = 0;

  regcache_raw_write (regcache, double_regnum, buf + offset);
  offset = 8 - offset;
  regcache_raw_write (regcache, double_regnum + 1, buf + offset);
}

static void
arm_pseudo_write (struct gdbarch *gdbarch, struct regcache *regcache,
		  int regnum, const gdb_byte *buf)
{
  const int num_regs = gdbarch_num_regs (gdbarch);
  char name_buf[4];
  gdb_byte reg_buf[8];
  int offset, double_regnum;

  gdb_assert (regnum >= num_regs);
  regnum -= num_regs;

  if (gdbarch_tdep (gdbarch)->have_neon_pseudos && regnum >= 32 && regnum < 48)
    /* Quad-precision register.  */
    arm_neon_quad_write (gdbarch, regcache, regnum - 32, buf);
  else
    {
      /* Single-precision register.  */
      gdb_assert (regnum < 32);

      /* s0 is always the least significant half of d0.  */
      if (gdbarch_byte_order (gdbarch) == BFD_ENDIAN_BIG)
	offset = (regnum & 1) ? 0 : 4;
      else
	offset = (regnum & 1) ? 4 : 0;

      sprintf (name_buf, "d%d", regnum >> 1);
      double_regnum = user_reg_map_name_to_regnum (gdbarch, name_buf,
						   strlen (name_buf));

      regcache_raw_read (regcache, double_regnum, reg_buf);
      memcpy (reg_buf + offset, buf, 4);
      regcache_raw_write (regcache, double_regnum, reg_buf);
    }
}

static struct value *
value_of_arm_user_reg (struct frame_info *frame, const void *baton)
{
  const int *reg_p = baton;
  return value_of_register (*reg_p, frame);
}

static enum gdb_osabi
arm_elf_osabi_sniffer (bfd *abfd)
{
  unsigned int elfosabi;
  enum gdb_osabi osabi = GDB_OSABI_UNKNOWN;

  elfosabi = elf_elfheader (abfd)->e_ident[EI_OSABI];

  if (elfosabi == ELFOSABI_ARM)
    /* GNU tools use this value.  Check note sections in this case,
       as well.  */
    bfd_map_over_sections (abfd,
			   generic_elf_osabi_sniff_abi_tag_sections, 
			   &osabi);

  /* Anything else will be handled by the generic ELF sniffer.  */
  return osabi;
}

static int
arm_register_reggroup_p (struct gdbarch *gdbarch, int regnum,
			  struct reggroup *group)
{
  /* FPS register's type is INT, but belongs to float_reggroup.  Beside
     this, FPS register belongs to save_regroup, restore_reggroup, and
     all_reggroup, of course.  */
  if (regnum == ARM_FPS_REGNUM)
    return (group == float_reggroup
	    || group == save_reggroup
	    || group == restore_reggroup
	    || group == all_reggroup);
  else
    return default_register_reggroup_p (gdbarch, regnum, group);
}


/* Initialize the current architecture based on INFO.  If possible,
   re-use an architecture from ARCHES, which is a list of
   architectures already created during this debugging session.

   Called e.g. at program startup, when reading a core file, and when
   reading a binary file.  */

static struct gdbarch *
arm_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  struct gdbarch_tdep *tdep;
  struct gdbarch *gdbarch;
  struct gdbarch_list *best_arch;
  enum arm_abi_kind arm_abi = arm_abi_global;
  enum arm_float_model fp_model = arm_fp_model;
  struct tdesc_arch_data *tdesc_data = NULL;
  int i, is_m = 0;
  int have_vfp_registers = 0, have_vfp_pseudos = 0, have_neon_pseudos = 0;
  int have_neon = 0;
  int have_fpa_registers = 1;
  const struct target_desc *tdesc = info.target_desc;

  /* If we have an object to base this architecture on, try to determine
     its ABI.  */

  if (arm_abi == ARM_ABI_AUTO && info.abfd != NULL)
    {
      int ei_osabi, e_flags;

      switch (bfd_get_flavour (info.abfd))
	{
	case bfd_target_aout_flavour:
	  /* Assume it's an old APCS-style ABI.  */
	  arm_abi = ARM_ABI_APCS;
	  break;

	case bfd_target_coff_flavour:
	  /* Assume it's an old APCS-style ABI.  */
	  /* XXX WinCE?  */
	  arm_abi = ARM_ABI_APCS;
	  break;

	case bfd_target_elf_flavour:
	  ei_osabi = elf_elfheader (info.abfd)->e_ident[EI_OSABI];
	  e_flags = elf_elfheader (info.abfd)->e_flags;

	  if (ei_osabi == ELFOSABI_ARM)
	    {
	      /* GNU tools used to use this value, but do not for EABI
		 objects.  There's nowhere to tag an EABI version
		 anyway, so assume APCS.  */
	      arm_abi = ARM_ABI_APCS;
	    }
	  else if (ei_osabi == ELFOSABI_NONE)
	    {
	      int eabi_ver = EF_ARM_EABI_VERSION (e_flags);
	      int attr_arch, attr_profile;

	      switch (eabi_ver)
		{
		case EF_ARM_EABI_UNKNOWN:
		  /* Assume GNU tools.  */
		  arm_abi = ARM_ABI_APCS;
		  break;

		case EF_ARM_EABI_VER4:
		case EF_ARM_EABI_VER5:
		  arm_abi = ARM_ABI_AAPCS;
		  /* EABI binaries default to VFP float ordering.
		     They may also contain build attributes that can
		     be used to identify if the VFP argument-passing
		     ABI is in use.  */
		  if (fp_model == ARM_FLOAT_AUTO)
		    {
#ifdef HAVE_ELF
		      switch (bfd_elf_get_obj_attr_int (info.abfd,
							OBJ_ATTR_PROC,
							Tag_ABI_VFP_args))
			{
			case 0:
			  /* "The user intended FP parameter/result
			     passing to conform to AAPCS, base
			     variant".  */
			  fp_model = ARM_FLOAT_SOFT_VFP;
			  break;
			case 1:
			  /* "The user intended FP parameter/result
			     passing to conform to AAPCS, VFP
			     variant".  */
			  fp_model = ARM_FLOAT_VFP;
			  break;
			case 2:
			  /* "The user intended FP parameter/result
			     passing to conform to tool chain-specific
			     conventions" - we don't know any such
			     conventions, so leave it as "auto".  */
			  break;
			default:
			  /* Attribute value not mentioned in the
			     October 2008 ABI, so leave it as
			     "auto".  */
			  break;
			}
#else
		      fp_model = ARM_FLOAT_SOFT_VFP;
#endif
		    }
		  break;

		default:
		  /* Leave it as "auto".  */
		  warning (_("unknown ARM EABI version 0x%x"), eabi_ver);
		  break;
		}

#ifdef HAVE_ELF
	      /* Detect M-profile programs.  This only works if the
		 executable file includes build attributes; GCC does
		 copy them to the executable, but e.g. RealView does
		 not.  */
	      attr_arch = bfd_elf_get_obj_attr_int (info.abfd, OBJ_ATTR_PROC,
						    Tag_CPU_arch);
	      attr_profile = bfd_elf_get_obj_attr_int (info.abfd,
						       OBJ_ATTR_PROC,
						       Tag_CPU_arch_profile);
	      /* GCC specifies the profile for v6-M; RealView only
		 specifies the profile for architectures starting with
		 V7 (as opposed to architectures with a tag
		 numerically greater than TAG_CPU_ARCH_V7).  */
	      if (!tdesc_has_registers (tdesc)
		  && (attr_arch == TAG_CPU_ARCH_V6_M
		      || attr_arch == TAG_CPU_ARCH_V6S_M
		      || attr_profile == 'M'))
		tdesc = tdesc_arm_with_m;
#endif
	    }

	  if (fp_model == ARM_FLOAT_AUTO)
	    {
	      int e_flags = elf_elfheader (info.abfd)->e_flags;

	      switch (e_flags & (EF_ARM_SOFT_FLOAT | EF_ARM_VFP_FLOAT))
		{
		case 0:
		  /* Leave it as "auto".  Strictly speaking this case
		     means FPA, but almost nobody uses that now, and
		     many toolchains fail to set the appropriate bits
		     for the floating-point model they use.  */
		  break;
		case EF_ARM_SOFT_FLOAT:
		  fp_model = ARM_FLOAT_SOFT_FPA;
		  break;
		case EF_ARM_VFP_FLOAT:
		  fp_model = ARM_FLOAT_VFP;
		  break;
		case EF_ARM_SOFT_FLOAT | EF_ARM_VFP_FLOAT:
		  fp_model = ARM_FLOAT_SOFT_VFP;
		  break;
		}
	    }

	  if (e_flags & EF_ARM_BE8)
	    info.byte_order_for_code = BFD_ENDIAN_LITTLE;

	  break;

	default:
	  /* Leave it as "auto".  */
	  break;
	}
    }

  /* Check any target description for validity.  */
  if (tdesc_has_registers (tdesc))
    {
      /* For most registers we require GDB's default names; but also allow
	 the numeric names for sp / lr / pc, as a convenience.  */
      static const char *const arm_sp_names[] = { "r13", "sp", NULL };
      static const char *const arm_lr_names[] = { "r14", "lr", NULL };
      static const char *const arm_pc_names[] = { "r15", "pc", NULL };

      const struct tdesc_feature *feature;
      int valid_p;

      feature = tdesc_find_feature (tdesc,
				    "org.gnu.gdb.arm.core");
      if (feature == NULL)
	{
	  feature = tdesc_find_feature (tdesc,
					"org.gnu.gdb.arm.m-profile");
	  if (feature == NULL)
	    return NULL;
	  else
	    is_m = 1;
	}

      tdesc_data = tdesc_data_alloc ();

      valid_p = 1;
      for (i = 0; i < ARM_SP_REGNUM; i++)
	valid_p &= tdesc_numbered_register (feature, tdesc_data, i,
					    arm_register_names[i]);
      valid_p &= tdesc_numbered_register_choices (feature, tdesc_data,
						  ARM_SP_REGNUM,
						  arm_sp_names);
      valid_p &= tdesc_numbered_register_choices (feature, tdesc_data,
						  ARM_LR_REGNUM,
						  arm_lr_names);
      valid_p &= tdesc_numbered_register_choices (feature, tdesc_data,
						  ARM_PC_REGNUM,
						  arm_pc_names);
      if (is_m)
	valid_p &= tdesc_numbered_register (feature, tdesc_data,
					    ARM_PS_REGNUM, "xpsr");
      else
	valid_p &= tdesc_numbered_register (feature, tdesc_data,
					    ARM_PS_REGNUM, "cpsr");

      if (!valid_p)
	{
	  tdesc_data_cleanup (tdesc_data);
	  return NULL;
	}

      feature = tdesc_find_feature (tdesc,
				    "org.gnu.gdb.arm.fpa");
      if (feature != NULL)
	{
	  valid_p = 1;
	  for (i = ARM_F0_REGNUM; i <= ARM_FPS_REGNUM; i++)
	    valid_p &= tdesc_numbered_register (feature, tdesc_data, i,
						arm_register_names[i]);
	  if (!valid_p)
	    {
	      tdesc_data_cleanup (tdesc_data);
	      return NULL;
	    }
	}
      else
	have_fpa_registers = 0;

      feature = tdesc_find_feature (tdesc,
				    "org.gnu.gdb.xscale.iwmmxt");
      if (feature != NULL)
	{
	  static const char *const iwmmxt_names[] = {
	    "wR0", "wR1", "wR2", "wR3", "wR4", "wR5", "wR6", "wR7",
	    "wR8", "wR9", "wR10", "wR11", "wR12", "wR13", "wR14", "wR15",
	    "wCID", "wCon", "wCSSF", "wCASF", "", "", "", "",
	    "wCGR0", "wCGR1", "wCGR2", "wCGR3", "", "", "", "",
	  };

	  valid_p = 1;
	  for (i = ARM_WR0_REGNUM; i <= ARM_WR15_REGNUM; i++)
	    valid_p
	      &= tdesc_numbered_register (feature, tdesc_data, i,
					  iwmmxt_names[i - ARM_WR0_REGNUM]);

	  /* Check for the control registers, but do not fail if they
	     are missing.  */
	  for (i = ARM_WC0_REGNUM; i <= ARM_WCASF_REGNUM; i++)
	    tdesc_numbered_register (feature, tdesc_data, i,
				     iwmmxt_names[i - ARM_WR0_REGNUM]);

	  for (i = ARM_WCGR0_REGNUM; i <= ARM_WCGR3_REGNUM; i++)
	    valid_p
	      &= tdesc_numbered_register (feature, tdesc_data, i,
					  iwmmxt_names[i - ARM_WR0_REGNUM]);

	  if (!valid_p)
	    {
	      tdesc_data_cleanup (tdesc_data);
	      return NULL;
	    }
	}

      /* If we have a VFP unit, check whether the single precision registers
	 are present.  If not, then we will synthesize them as pseudo
	 registers.  */
      feature = tdesc_find_feature (tdesc,
				    "org.gnu.gdb.arm.vfp");
      if (feature != NULL)
	{
	  static const char *const vfp_double_names[] = {
	    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
	    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
	    "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
	    "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
	  };

	  /* Require the double precision registers.  There must be either
	     16 or 32.  */
	  valid_p = 1;
	  for (i = 0; i < 32; i++)
	    {
	      valid_p &= tdesc_numbered_register (feature, tdesc_data,
						  ARM_D0_REGNUM + i,
						  vfp_double_names[i]);
	      if (!valid_p)
		break;
	    }
	  if (!valid_p && i == 16)
	    valid_p = 1;

	  /* Also require FPSCR.  */
	  valid_p &= tdesc_numbered_register (feature, tdesc_data,
					      ARM_FPSCR_REGNUM, "fpscr");
	  if (!valid_p)
	    {
	      tdesc_data_cleanup (tdesc_data);
	      return NULL;
	    }

	  if (tdesc_unnumbered_register (feature, "s0") == 0)
	    have_vfp_pseudos = 1;

	  have_vfp_registers = 1;

	  /* If we have VFP, also check for NEON.  The architecture allows
	     NEON without VFP (integer vector operations only), but GDB
	     does not support that.  */
	  feature = tdesc_find_feature (tdesc,
					"org.gnu.gdb.arm.neon");
	  if (feature != NULL)
	    {
	      /* NEON requires 32 double-precision registers.  */
	      if (i != 32)
		{
		  tdesc_data_cleanup (tdesc_data);
		  return NULL;
		}

	      /* If there are quad registers defined by the stub, use
		 their type; otherwise (normally) provide them with
		 the default type.  */
	      if (tdesc_unnumbered_register (feature, "q0") == 0)
		have_neon_pseudos = 1;

	      have_neon = 1;
	    }
	}
    }

  /* If there is already a candidate, use it.  */
  for (best_arch = gdbarch_list_lookup_by_info (arches, &info);
       best_arch != NULL;
       best_arch = gdbarch_list_lookup_by_info (best_arch->next, &info))
    {
      if (arm_abi != ARM_ABI_AUTO
	  && arm_abi != gdbarch_tdep (best_arch->gdbarch)->arm_abi)
	continue;

      if (fp_model != ARM_FLOAT_AUTO
	  && fp_model != gdbarch_tdep (best_arch->gdbarch)->fp_model)
	continue;

      /* There are various other properties in tdep that we do not
	 need to check here: those derived from a target description,
	 since gdbarches with a different target description are
	 automatically disqualified.  */

      /* Do check is_m, though, since it might come from the binary.  */
      if (is_m != gdbarch_tdep (best_arch->gdbarch)->is_m)
	continue;

      /* Found a match.  */
      break;
    }

  if (best_arch != NULL)
    {
      if (tdesc_data != NULL)
	tdesc_data_cleanup (tdesc_data);
      return best_arch->gdbarch;
    }

  tdep = xcalloc (1, sizeof (struct gdbarch_tdep));
  gdbarch = gdbarch_alloc (&info, tdep);

  /* Record additional information about the architecture we are defining.
     These are gdbarch discriminators, like the OSABI.  */
  tdep->arm_abi = arm_abi;
  tdep->fp_model = fp_model;
  tdep->is_m = is_m;
  tdep->have_fpa_registers = have_fpa_registers;
  tdep->have_vfp_registers = have_vfp_registers;
  tdep->have_vfp_pseudos = have_vfp_pseudos;
  tdep->have_neon_pseudos = have_neon_pseudos;
  tdep->have_neon = have_neon;

  /* Breakpoints.  */
  switch (info.byte_order_for_code)
    {
    case BFD_ENDIAN_BIG:
      tdep->arm_breakpoint = arm_default_arm_be_breakpoint;
      tdep->arm_breakpoint_size = sizeof (arm_default_arm_be_breakpoint);
      tdep->thumb_breakpoint = arm_default_thumb_be_breakpoint;
      tdep->thumb_breakpoint_size = sizeof (arm_default_thumb_be_breakpoint);

      break;

    case BFD_ENDIAN_LITTLE:
      tdep->arm_breakpoint = arm_default_arm_le_breakpoint;
      tdep->arm_breakpoint_size = sizeof (arm_default_arm_le_breakpoint);
      tdep->thumb_breakpoint = arm_default_thumb_le_breakpoint;
      tdep->thumb_breakpoint_size = sizeof (arm_default_thumb_le_breakpoint);

      break;

    default:
      internal_error (__FILE__, __LINE__,
		      _("arm_gdbarch_init: bad byte order for float format"));
    }

  /* On ARM targets char defaults to unsigned.  */
  set_gdbarch_char_signed (gdbarch, 0);

  /* Note: for displaced stepping, this includes the breakpoint, and one word
     of additional scratch space.  This setting isn't used for anything beside
     displaced stepping at present.  */
  set_gdbarch_max_insn_length (gdbarch, 4 * DISPLACED_MODIFIED_INSNS);

  /* This should be low enough for everything.  */
  tdep->lowest_pc = 0x20;
  tdep->jb_pc = -1;	/* Longjump support not enabled by default.  */

  /* The default, for both APCS and AAPCS, is to return small
     structures in registers.  */
  tdep->struct_return = reg_struct_return;

  set_gdbarch_push_dummy_call (gdbarch, arm_push_dummy_call);
  set_gdbarch_frame_align (gdbarch, arm_frame_align);

  set_gdbarch_write_pc (gdbarch, arm_write_pc);

  /* Frame handling.  */
  set_gdbarch_dummy_id (gdbarch, arm_dummy_id);
  set_gdbarch_unwind_pc (gdbarch, arm_unwind_pc);
  set_gdbarch_unwind_sp (gdbarch, arm_unwind_sp);

  frame_base_set_default (gdbarch, &arm_normal_base);

  /* Address manipulation.  */
  set_gdbarch_smash_text_address (gdbarch, arm_smash_text_address);
  set_gdbarch_addr_bits_remove (gdbarch, arm_addr_bits_remove);

  /* Advance PC across function entry code.  */
  set_gdbarch_skip_prologue (gdbarch, arm_skip_prologue);

  /* Detect whether PC is in function epilogue.  */
  set_gdbarch_in_function_epilogue_p (gdbarch, arm_in_function_epilogue_p);

  /* Skip trampolines.  */
  set_gdbarch_skip_trampoline_code (gdbarch, arm_skip_stub);

  /* The stack grows downward.  */
  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);

  /* Breakpoint manipulation.  */
  set_gdbarch_breakpoint_from_pc (gdbarch, arm_breakpoint_from_pc);
  set_gdbarch_remote_breakpoint_from_pc (gdbarch,
					 arm_remote_breakpoint_from_pc);

  /* Information about registers, etc.  */
  set_gdbarch_sp_regnum (gdbarch, ARM_SP_REGNUM);
  set_gdbarch_pc_regnum (gdbarch, ARM_PC_REGNUM);
  set_gdbarch_num_regs (gdbarch, ARM_NUM_REGS);
  set_gdbarch_register_type (gdbarch, arm_register_type);
  set_gdbarch_register_reggroup_p (gdbarch, arm_register_reggroup_p);

  /* This "info float" is FPA-specific.  Use the generic version if we
     do not have FPA.  */
  if (gdbarch_tdep (gdbarch)->have_fpa_registers)
    set_gdbarch_print_float_info (gdbarch, arm_print_float_info);

  /* Internal <-> external register number maps.  */
  set_gdbarch_dwarf2_reg_to_regnum (gdbarch, arm_dwarf_reg_to_regnum);
  set_gdbarch_register_sim_regno (gdbarch, arm_register_sim_regno);

  set_gdbarch_register_name (gdbarch, arm_register_name);

  /* Returning results.  */
  set_gdbarch_return_value (gdbarch, arm_return_value);

  /* Disassembly.  */
  set_gdbarch_print_insn (gdbarch, gdb_print_insn_arm);

  /* Minsymbol frobbing.  */
  set_gdbarch_elf_make_msymbol_special (gdbarch, arm_elf_make_msymbol_special);
  set_gdbarch_coff_make_msymbol_special (gdbarch,
					 arm_coff_make_msymbol_special);
  set_gdbarch_record_special_symbol (gdbarch, arm_record_special_symbol);

  /* Thumb-2 IT block support.  */
  set_gdbarch_adjust_breakpoint_address (gdbarch,
					 arm_adjust_breakpoint_address);

  /* Virtual tables.  */
  set_gdbarch_vbit_in_delta (gdbarch, 1);

  /* Hook in the ABI-specific overrides, if they have been registered.  */
  gdbarch_init_osabi (info, gdbarch);

  dwarf2_frame_set_init_reg (gdbarch, arm_dwarf2_frame_init_reg);

  /* Add some default predicates.  */
  frame_unwind_append_unwinder (gdbarch, &arm_stub_unwind);
  dwarf2_append_unwinders (gdbarch);
  frame_unwind_append_unwinder (gdbarch, &arm_exidx_unwind);
  frame_unwind_append_unwinder (gdbarch, &arm_prologue_unwind);

  /* Now we have tuned the configuration, set a few final things,
     based on what the OS ABI has told us.  */

  /* If the ABI is not otherwise marked, assume the old GNU APCS.  EABI
     binaries are always marked.  */
  if (tdep->arm_abi == ARM_ABI_AUTO)
    tdep->arm_abi = ARM_ABI_APCS;

  /* Watchpoints are not steppable.  */
  set_gdbarch_have_nonsteppable_watchpoint (gdbarch, 1);

  /* We used to default to FPA for generic ARM, but almost nobody
     uses that now, and we now provide a way for the user to force
     the model.  So default to the most useful variant.  */
  if (tdep->fp_model == ARM_FLOAT_AUTO)
    tdep->fp_model = ARM_FLOAT_SOFT_FPA;

  if (tdep->jb_pc >= 0)
    set_gdbarch_get_longjmp_target (gdbarch, arm_get_longjmp_target);

  /* Floating point sizes and format.  */
  set_gdbarch_float_format (gdbarch, floatformats_ieee_single);
  if (tdep->fp_model == ARM_FLOAT_SOFT_FPA || tdep->fp_model == ARM_FLOAT_FPA)
    {
      set_gdbarch_double_format
	(gdbarch, floatformats_ieee_double_littlebyte_bigword);
      set_gdbarch_long_double_format
	(gdbarch, floatformats_ieee_double_littlebyte_bigword);
    }
  else
    {
      set_gdbarch_double_format (gdbarch, floatformats_ieee_double);
      set_gdbarch_long_double_format (gdbarch, floatformats_ieee_double);
    }

  if (have_vfp_pseudos)
    {
      /* NOTE: These are the only pseudo registers used by
	 the ARM target at the moment.  If more are added, a
	 little more care in numbering will be needed.  */

      int num_pseudos = 32;
      if (have_neon_pseudos)
	num_pseudos += 16;
      set_gdbarch_num_pseudo_regs (gdbarch, num_pseudos);
      set_gdbarch_pseudo_register_read (gdbarch, arm_pseudo_read);
      set_gdbarch_pseudo_register_write (gdbarch, arm_pseudo_write);
    }

  if (tdesc_data)
    {
      set_tdesc_pseudo_register_name (gdbarch, arm_register_name);

      tdesc_use_registers (gdbarch, tdesc, tdesc_data);

      /* Override tdesc_register_type to adjust the types of VFP
	 registers for NEON.  */
      set_gdbarch_register_type (gdbarch, arm_register_type);
    }

  /* Add standard register aliases.  We add aliases even for those
     nanes which are used by the current architecture - it's simpler,
     and does no harm, since nothing ever lists user registers.  */
  for (i = 0; i < ARRAY_SIZE (arm_register_aliases); i++)
    user_reg_add (gdbarch, arm_register_aliases[i].name,
		  value_of_arm_user_reg, &arm_register_aliases[i].regnum);

  return gdbarch;
}

static void
arm_dump_tdep (struct gdbarch *gdbarch, struct ui_file *file)
{
  struct gdbarch_tdep *tdep = gdbarch_tdep (gdbarch);

  if (tdep == NULL)
    return;

  fprintf_unfiltered (file, _("arm_dump_tdep: Lowest pc = 0x%lx"),
		      (unsigned long) tdep->lowest_pc);
}

extern initialize_file_ftype _initialize_arm_tdep; /* -Wmissing-prototypes */

void
_initialize_arm_tdep (void)
{
  struct ui_file *stb;
  long length;
  struct cmd_list_element *new_set, *new_show;
  const char *setname;
  const char *setdesc;
  const char *const *regnames;
  int numregs, i, j;
  static char *helptext;
  char regdesc[1024], *rdptr = regdesc;
  size_t rest = sizeof (regdesc);

  gdbarch_register (bfd_arch_arm, arm_gdbarch_init, arm_dump_tdep);

  arm_objfile_data_key
    = register_objfile_data_with_cleanup (NULL, arm_objfile_data_free);

  /* Add ourselves to objfile event chain.  */
  observer_attach_new_objfile (arm_exidx_new_objfile);
  arm_exidx_data_key
    = register_objfile_data_with_cleanup (NULL, arm_exidx_data_free);

  /* Register an ELF OS ABI sniffer for ARM binaries.  */
  gdbarch_register_osabi_sniffer (bfd_arch_arm,
				  bfd_target_elf_flavour,
				  arm_elf_osabi_sniffer);

  /* Initialize the standard target descriptions.  */
  initialize_tdesc_arm_with_m ();
  initialize_tdesc_arm_with_iwmmxt ();
  initialize_tdesc_arm_with_vfpv2 ();
  initialize_tdesc_arm_with_vfpv3 ();
  initialize_tdesc_arm_with_neon ();

  /* Get the number of possible sets of register names defined in opcodes.  */
  num_disassembly_options = get_arm_regname_num_options ();

  /* Add root prefix command for all "set arm"/"show arm" commands.  */
  add_prefix_cmd ("arm", no_class, set_arm_command,
		  _("Various ARM-specific commands."),
		  &setarmcmdlist, "set arm ", 0, &setlist);

  add_prefix_cmd ("arm", no_class, show_arm_command,
		  _("Various ARM-specific commands."),
		  &showarmcmdlist, "show arm ", 0, &showlist);

  /* Sync the opcode insn printer with our register viewer.  */
  parse_arm_disassembler_option ("reg-names-std");

  /* Initialize the array that will be passed to
     add_setshow_enum_cmd().  */
  valid_disassembly_styles
    = xmalloc ((num_disassembly_options + 1) * sizeof (char *));
  for (i = 0; i < num_disassembly_options; i++)
    {
      numregs = get_arm_regnames (i, &setname, &setdesc, &regnames);
      valid_disassembly_styles[i] = setname;
      length = snprintf (rdptr, rest, "%s - %s\n", setname, setdesc);
      rdptr += length;
      rest -= length;
      /* When we find the default names, tell the disassembler to use
	 them.  */
      if (!strcmp (setname, "std"))
	{
          disassembly_style = setname;
          set_arm_regname_option (i);
	}
    }
  /* Mark the end of valid options.  */
  valid_disassembly_styles[num_disassembly_options] = NULL;

  /* Create the help text.  */
  stb = mem_fileopen ();
  fprintf_unfiltered (stb, "%s%s%s",
		      _("The valid values are:\n"),
		      regdesc,
		      _("The default is \"std\"."));
  helptext = ui_file_xstrdup (stb, NULL);
  ui_file_delete (stb);

  add_setshow_enum_cmd("disassembler", no_class,
		       valid_disassembly_styles, &disassembly_style,
		       _("Set the disassembly style."),
		       _("Show the disassembly style."),
		       helptext,
		       set_disassembly_style_sfunc,
		       NULL, /* FIXME: i18n: The disassembly style is
				\"%s\".  */
		       &setarmcmdlist, &showarmcmdlist);

  add_setshow_boolean_cmd ("apcs32", no_class, &arm_apcs_32,
			   _("Set usage of ARM 32-bit mode."),
			   _("Show usage of ARM 32-bit mode."),
			   _("When off, a 26-bit PC will be used."),
			   NULL,
			   NULL, /* FIXME: i18n: Usage of ARM 32-bit
				    mode is %s.  */
			   &setarmcmdlist, &showarmcmdlist);

  /* Add a command to allow the user to force the FPU model.  */
  add_setshow_enum_cmd ("fpu", no_class, fp_model_strings, &current_fp_model,
			_("Set the floating point type."),
			_("Show the floating point type."),
			_("auto - Determine the FP typefrom the OS-ABI.\n\
softfpa - Software FP, mixed-endian doubles on little-endian ARMs.\n\
fpa - FPA co-processor (GCC compiled).\n\
softvfp - Software FP with pure-endian doubles.\n\
vfp - VFP co-processor."),
			set_fp_model_sfunc, show_fp_model,
			&setarmcmdlist, &showarmcmdlist);

  /* Add a command to allow the user to force the ABI.  */
  add_setshow_enum_cmd ("abi", class_support, arm_abi_strings, &arm_abi_string,
			_("Set the ABI."),
			_("Show the ABI."),
			NULL, arm_set_abi, arm_show_abi,
			&setarmcmdlist, &showarmcmdlist);

  /* Add two commands to allow the user to force the assumed
     execution mode.  */
  add_setshow_enum_cmd ("fallback-mode", class_support,
			arm_mode_strings, &arm_fallback_mode_string,
			_("Set the mode assumed when symbols are unavailable."),
			_("Show the mode assumed when symbols are unavailable."),
			NULL, NULL, arm_show_fallback_mode,
			&setarmcmdlist, &showarmcmdlist);
  add_setshow_enum_cmd ("force-mode", class_support,
			arm_mode_strings, &arm_force_mode_string,
			_("Set the mode assumed even when symbols are available."),
			_("Show the mode assumed even when symbols are available."),
			NULL, NULL, arm_show_force_mode,
			&setarmcmdlist, &showarmcmdlist);

  /* Debugging flag.  */
  add_setshow_boolean_cmd ("arm", class_maintenance, &arm_debug,
			   _("Set ARM debugging."),
			   _("Show ARM debugging."),
			   _("When on, arm-specific debugging is enabled."),
			   NULL,
			   NULL, /* FIXME: i18n: "ARM debugging is %s.  */
			   &setdebuglist, &showdebuglist);
}

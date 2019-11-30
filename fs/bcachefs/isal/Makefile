########################################################################
#  Copyright(c) 2019 Robbie Litchfield <blam.kiwi@gmail.com> All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
########################################################################

########################
# ISA-L Build Makefile #
########################

#	The Intel provided build system does not support being integrated easily into KBuild. 
# 	This is a partially generated Makefile. In the end this should be made trivial by porting
#	optimized assembly code to GAS compatible code.

# 	Setup ISA-L build parameters.
ISAL_INCLUDE = -I$(src)/include -I$(src)/crc
ccflags-y += $(ISAL_INCLUDE) -O2 -Wall -D NDEBUG

#   Build commands
ASSEMBLE=nasm -f elf64 -D NDEBUG $(ISAL_INCLUDE) -DAS_FEATURE_LEVEL=10 -DHAVE_AS_KNOWS_AVX512

# 	Conditionally add ISAL to KBuild goals
ifeq ($(CONFIG_BCACHEFS_ISAL_BACKEND), y)
obj-$(CONFIG_BCACHEFS_FS) += isal.o
endif

isal-y += \
    crc/crc64_base.o \
    crc/crc_base.o \
	crc/crc64_ecma_norm_by8.o \
	crc/crc64_iso_refl_by16_10.o \
	crc/crc64_ecma_norm_by16_10.o \
	crc/crc64_ecma_refl_by8.o \
	crc/crc16_t10dif_copy_by4.o \
	crc/crc32_ieee_by4.o \
	crc/crc32_iscsi_00.o \
	crc/crc64_iso_norm_by16_10.o \
	crc/crc32_iscsi_01.o \
	crc/crc64_jones_norm_by8.o \
	crc/crc64_iso_norm_by8.o \
	crc/crc64_multibinary.o \
	crc/crc16_t10dif_by4.o \
	crc/crc64_ecma_refl_by16_10.o \
	crc/crc32_ieee_01.o \
	crc/crc64_iso_refl_by8.o \
	crc/crc64_jones_refl_by16_10.o \
	crc/crc_multibinary.o \
	crc/crc64_jones_norm_by16_10.o \
	crc/crc64_jones_refl_by8.o \
	crc/crc32_gzip_refl_by8.o \
	crc/crc16_t10dif_01.o


# CRC ASM
$(obj)/crc/crc64_ecma_norm_by8.o: $(src)/crc/crc64_ecma_norm_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_ecma_norm_by8.o $(src)/crc/crc64_ecma_norm_by8.asm
$(obj)/crc/crc64_iso_refl_by16_10.o: $(src)/crc/crc64_iso_refl_by16_10.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_iso_refl_by16_10.o $(src)/crc/crc64_iso_refl_by16_10.asm
$(obj)/crc/crc64_ecma_norm_by16_10.o: $(src)/crc/crc64_ecma_norm_by16_10.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_ecma_norm_by16_10.o $(src)/crc/crc64_ecma_norm_by16_10.asm
$(obj)/crc/crc64_ecma_refl_by8.o: $(src)/crc/crc64_ecma_refl_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_ecma_refl_by8.o $(src)/crc/crc64_ecma_refl_by8.asm
$(obj)/crc/crc16_t10dif_copy_by4.o: $(src)/crc/crc16_t10dif_copy_by4.asm
	$(ASSEMBLE) -o $(obj)/crc/crc16_t10dif_copy_by4.o $(src)/crc/crc16_t10dif_copy_by4.asm
$(obj)/crc/crc32_ieee_by4.o: $(src)/crc/crc32_ieee_by4.asm
	$(ASSEMBLE) -o $(obj)/crc/crc32_ieee_by4.o $(src)/crc/crc32_ieee_by4.asm
$(obj)/crc/crc32_iscsi_00.o: $(src)/crc/crc32_iscsi_00.asm
	$(ASSEMBLE) -o $(obj)/crc/crc32_iscsi_00.o $(src)/crc/crc32_iscsi_00.asm
$(obj)/crc/crc64_iso_norm_by16_10.o: $(src)/crc/crc64_iso_norm_by16_10.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_iso_norm_by16_10.o $(src)/crc/crc64_iso_norm_by16_10.asm
$(obj)/crc/crc32_iscsi_01.o: $(src)/crc/crc32_iscsi_01.asm
	$(ASSEMBLE) -o $(obj)/crc/crc32_iscsi_01.o $(src)/crc/crc32_iscsi_01.asm
$(obj)/crc/crc64_jones_norm_by8.o: $(src)/crc/crc64_jones_norm_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_jones_norm_by8.o $(src)/crc/crc64_jones_norm_by8.asm
$(obj)/crc/crc64_iso_norm_by8.o: $(src)/crc/crc64_iso_norm_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_iso_norm_by8.o $(src)/crc/crc64_iso_norm_by8.asm
$(obj)/crc/crc64_multibinary.o: $(src)/crc/crc64_multibinary.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_multibinary.o $(src)/crc/crc64_multibinary.asm
$(obj)/crc/crc16_t10dif_by4.o: $(src)/crc/crc16_t10dif_by4.asm
	$(ASSEMBLE) -o $(obj)/crc/crc16_t10dif_by4.o $(src)/crc/crc16_t10dif_by4.asm
$(obj)/crc/crc64_ecma_refl_by16_10.o: $(src)/crc/crc64_ecma_refl_by16_10.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_ecma_refl_by16_10.o $(src)/crc/crc64_ecma_refl_by16_10.asm
$(obj)/crc/crc32_ieee_01.o: $(src)/crc/crc32_ieee_01.asm
	$(ASSEMBLE) -o $(obj)/crc/crc32_ieee_01.o $(src)/crc/crc32_ieee_01.asm
$(obj)/crc/crc64_iso_refl_by8.o: $(src)/crc/crc64_iso_refl_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_iso_refl_by8.o $(src)/crc/crc64_iso_refl_by8.asm
$(obj)/crc/crc64_jones_refl_by16_10.o: $(src)/crc/crc64_jones_refl_by16_10.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_jones_refl_by16_10.o $(src)/crc/crc64_jones_refl_by16_10.asm
$(obj)/crc/crc_multibinary.o: $(src)/crc/crc_multibinary.asm
	$(ASSEMBLE) -o $(obj)/crc/crc_multibinary.o $(src)/crc/crc_multibinary.asm
$(obj)/crc/crc64_jones_norm_by16_10.o: $(src)/crc/crc64_jones_norm_by16_10.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_jones_norm_by16_10.o $(src)/crc/crc64_jones_norm_by16_10.asm
$(obj)/crc/crc64_jones_refl_by8.o: $(src)/crc/crc64_jones_refl_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc64_jones_refl_by8.o $(src)/crc/crc64_jones_refl_by8.asm
$(obj)/crc/crc32_gzip_refl_by8.o: $(src)/crc/crc32_gzip_refl_by8.asm
	$(ASSEMBLE) -o $(obj)/crc/crc32_gzip_refl_by8.o $(src)/crc/crc32_gzip_refl_by8.asm
$(obj)/crc/crc16_t10dif_01.o: $(src)/crc/crc16_t10dif_01.asm
	$(ASSEMBLE) -o $(obj)/crc/crc16_t10dif_01.o $(src)/crc/crc16_t10dif_01.asm

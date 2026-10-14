#   Copyright (C) 2023 John Törnblom
#
# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := byepervisor_reborn.elf

PAYLOAD_DIR := break_hv_payload
PAYLOAD_HDR := $(PAYLOAD_DIR)/break_hv.h
DIAG_HDR := $(PAYLOAD_DIR)/break_hv_diag.h

CFLAGS := -Wall -Werror -Wno-unused-function -g -I./include

all: $(ELF)

preflight-1270.elf: preflight_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ preflight_1270.c

cpu-tss-map-probe-1270.elf: cpu_tss_map_probe_1270.c
	$(CC) $(CFLAGS) -o $@ cpu_tss_map_probe_1270.c

cpu-tss-live-probe-1270.elf: cpu_tss_live_probe_1270.c
	$(CC) $(CFLAGS) -o $@ cpu_tss_live_probe_1270.c

sysent-getuid-probe-1270.elf: sysent_getuid_probe_1270.c
	$(CC) $(CFLAGS) -o $@ sysent_getuid_probe_1270.c

second-db-prepare-1270.elf: second_db_probe_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DSECOND_DB_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

second-db-trigger-1270.elf: second_db_probe_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -o $@ $(filter %.c,$^)

resume-stock-probe-1270.elf: resume_stock_probe_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -o $@ $(filter %.c,$^)

resume-stock-prepare-1270.elf: resume_stock_probe_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DRESUME_STOCK_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

resume-stock-apcr3-1270.elf: resume_stock_probe_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DRESUME_STOCK_AP_CR3_1270 -o $@ $(filter %.c,$^)

resume-store-prepare-1270.elf: resume_store_probe_1270.c carrier_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DRESUME_STORE_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

resume-store-probe-1270.elf: resume_store_probe_1270.c carrier_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -o $@ $(filter %.c,$^)

fixed-ap-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -o $@ $(filter %.c,$^)

owned-resume-ap-prepare-1270.elf: ap_fixed_probe_1270.c carrier_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_OWNED_RESUME_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

owned-resume-ap-probe-1270.elf: ap_fixed_probe_1270.c carrier_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_OWNED_RESUME_1270 -o $@ $(filter %.c,$^)

fixed-ap-hvread-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-hvread-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -o $@ $(filter %.c,$^)

fixed-ap-pcpus-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_PCPUS_READ_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-pcpus-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_PCPUS_READ_1270 -o $@ $(filter %.c,$^)

fixed-ap-hvpair-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-hvpair-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcbptr-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_VMCB_PTR_READ_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcbptr-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_VMCB_PTR_READ_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcbfields-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_VMCB_FIELDS_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcbfields-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_VMCB_FIELDS_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb15fields-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_VMCB15_FIELDS_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb15fields-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_VMCB15_FIELDS_1270 -o $@ $(filter %.c,$^)

break_hv_payload/vmcb_survey_1270.h: break_hv_payload/vmcb_survey_1270.c break_hv_payload/entry_1270.S break_hv_payload/link_1270.ld break_hv_payload/build_vmcb_survey_1270.sh
	sh break_hv_payload/build_vmcb_survey_1270.sh

break_hv_payload/vmcb_early_late_1270.h: break_hv_payload/vmcb_early_late_1270.c break_hv_payload/entry_1270.S break_hv_payload/link_1270.ld break_hv_payload/build_vmcb_early_late_1270.sh
	sh break_hv_payload/build_vmcb_early_late_1270.sh

fixed-ap-vmcb-early-late-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_early_late_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_EARLY_LATE_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb-early-late-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_early_late_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_EARLY_LATE_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb-survey-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_survey_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_SURVEY_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb-survey-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_survey_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_SURVEY_1270 -o $@ $(filter %.c,$^)

break_hv_payload/vmcb_strike_1270.h: break_hv_payload/vmcb_strike_1270.c break_hv_payload/entry_1270.S break_hv_payload/link_strike_1270.ld break_hv_payload/build_vmcb_strike_1270.sh
	sh break_hv_payload/build_vmcb_strike_1270.sh

fixed-ap-vmcb-strike-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_strike_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_STRIKE_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb-strike-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_strike_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_STRIKE_1270 -o $@ $(filter %.c,$^)

break_hv_payload/ap_arg_probe_1270.h: break_hv_payload/ap_arg_probe_1270.S break_hv_payload/build_ap_arg_probe_1270.sh
	sh break_hv_payload/build_ap_arg_probe_1270.sh

fixed-ap-arg-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/ap_arg_probe_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_CORE_ARG_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-arg-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/ap_arg_probe_1270.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_CORE_ARG_1270 -o $@ $(filter %.c,$^)

# Rebuild with -B whenever NPT_READ_PA changes; the output record logs the PA.
NPT_READ_PA ?= 0x62a77000ULL
fixed-ap-nptentry-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_NPT_ENTRY_1270 -DNPT_READ_PA_1270=$(NPT_READ_PA) -o $@ $(filter %.c,$^)

NPT_READ_PA2 ?= 0x62a78008ULL
fixed-ap-nptpair-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_NPT_ENTRY_1270 -DNPT_READ_PA_1270=$(NPT_READ_PA) -DNPT_READ_PA2_1270=$(NPT_READ_PA2) -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-nptpair-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_HV_READ_1270 -DAP_HV_DUAL_READ_1270 -DAP_HV_NPT_ENTRY_1270 -DNPT_READ_PA_1270=$(NPT_READ_PA) -DNPT_READ_PA2_1270=$(NPT_READ_PA2) -o $@ $(filter %.c,$^)

break_hv_payload/npt_table_dump.h: break_hv_payload/npt_table_dump.S break_hv_payload/build_npt_table_dump.sh
	sh break_hv_payload/build_npt_table_dump.sh

fixed-ap-nptdump-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/npt_table_dump.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_NPT_TABLE_DUMP_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-nptdump-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/npt_table_dump.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_NPT_TABLE_DUMP_1270 -o $@ $(filter %.c,$^)

break_hv_payload/vmcb_rw_probe.h: break_hv_payload/vmcb_rw_probe.S break_hv_payload/build_vmcb_rw_probe.sh
	sh break_hv_payload/build_vmcb_rw_probe.sh

fixed-ap-vmcb-rw-prepare-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_rw_probe.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_RW_1270 -DFIXED_AP_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

fixed-ap-vmcb-rw-probe-1270.elf: ap_fixed_probe_1270.c khook.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h break_hv_payload/vmcb_rw_probe.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_DIAG_STORE_HANDLER_1270 -DAP_VMCB_RW_1270 -o $@ $(filter %.c,$^)

ap-cr3-high-probe-1270.elf: ap-cr3-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_CR3_HIGH_1270 -o $@ $(filter %.c,$^)

ap-entry-prepare-1270.elf: ap_entry_probe_1270.c carrier_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -DAP_ENTRY_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

ap-entry-probe-1270.elf: ap_entry_probe_1270.c carrier_1270.c khook.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_SECOND_MARKER_1270 -o $@ $(filter %.c,$^)

pivot-iret-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-iret-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

pivot-pop-rax-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_POP_RAX_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-pop-rax-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_POP_RAX_1270 -o $@ $(filter %.c,$^)

pivot-dbreg-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_DBREG_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-dbreg-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_DBREG_1270 -o $@ $(filter %.c,$^)

pivot-candidate-iret-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_CANDIDATE_IRET_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-candidate-iret-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_CANDIDATE_IRET_1270 -o $@ $(filter %.c,$^)

pivot-continuation-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_DBREG_CONT_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-continuation-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_DBREG_CONT_1270 -o $@ $(filter %.c,$^)

pivot-continuation-alt-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_DBREG_CONT_1270 -DPIVOT_CONT_ALT_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-continuation-alt-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_DBREG_CONT_1270 -DPIVOT_CONT_ALT_1270 -o $@ $(filter %.c,$^)

pivot-pop-rcx-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_FIRST_POP_1270 -DPIVOT_FIRST_POP_OFF=0x3410d2ULL -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-pop-rcx-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_FIRST_POP_1270 -DPIVOT_FIRST_POP_OFF=0x3410d2ULL -o $@ $(filter %.c,$^)

pivot-pop-r8-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_FIRST_POP_1270 -DPIVOT_FIRST_POP_OFF=0x7dedbfULL -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-pop-r8-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_FIRST_POP_1270 -DPIVOT_FIRST_POP_OFF=0x7dedbfULL -o $@ $(filter %.c,$^)

pivot-store-rcx-rax-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_STORE_RCX_RAX_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-store-rcx-rax-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_STORE_RCX_RAX_1270 -o $@ $(filter %.c,$^)

pivot-set-dr7-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_SET_DR7_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-set-dr7-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_SET_DR7_1270 -o $@ $(filter %.c,$^)

pivot-pop-pair-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_POP_PAIR_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-pop-pair-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_POP_PAIR_1270 -o $@ $(filter %.c,$^)

pivot-pop-rsp-prepare-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_POP_RSP_1270 -DPIVOT_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

pivot-pop-rsp-trigger-1270.elf: pivot_iret_probe_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DPIVOT_POP_RSP_1270 -o $@ $(filter %.c,$^)

facs-preflight-1270.elf: facs_preflight_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ facs_preflight_1270.c

gadget-page-probe-1270.elf: gadget_page_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gadget_page_probe_1270.c

kernel-text-copyout-probe-1270.elf: kernel_text_copyout_probe_1270.c
	$(CC) $(CFLAGS) -o $@ kernel_text_copyout_probe_1270.c

text-base-pte-inspect-1270.elf: text_base_pte_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

text-base-pte-prepare-1270.elf: text_base_pte_1270.c kdlsym.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DTEXT_PTE_APPLY_1270 -o $@ $(filter %.c,$^)

text-base-status-1270.elf: text_base_status_1270.c kdlsym.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

kernel-text-qword-probe-1270.elf: kernel_text_qword_probe_1270.c
	$(CC) $(CFLAGS) -o $@ kernel_text_qword_probe_1270.c

kernel-text-record-probe-1270.elf: kernel_text_record_probe_1270.c
	$(CC) $(CFLAGS) -o $@ kernel_text_record_probe_1270.c

carrier-read-probe-1270.elf: carrier-read-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ carrier-read-probe-1270.c

carrier-layout-probe-1270.elf: carrier-read-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DCARR_LAYOUT_1270 -o $@ carrier-read-probe-1270.c

carrier-save-probe-1270.elf: carrier-read-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DCARR_SAVE_1270 -o $@ carrier-read-probe-1270.c

carrier-save-current-probe-1270.elf: carrier-read-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DCARR_SAVE_1270 -DCARR_SAVE_CURRENT_1270 -o $@ carrier-read-probe-1270.c

carrier-neighborhood-probe-1270.elf: carrier-neighborhood-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ carrier-neighborhood-probe-1270.c

owned-carrier-probe-1270.elf: owned-carrier-probe-1270.c
	$(CC) $(CFLAGS) -o $@ owned-carrier-probe-1270.c

owned-carrier-map-probe-1270.elf: owned-carrier-map-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ owned-carrier-map-probe-1270.c

anon-carrier-probe-1270.elf: anon-carrier-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ anon-carrier-probe-1270.c

anon-carrier-hold-probe-1270.elf: anon-carrier-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DANON_HOLD_1270 -o $@ anon-carrier-probe-1270.c

anon-carrier-stage-probe-1270.elf: anon-carrier-probe-1270.c include/offsets/12_70_candidate.h $(DIAG_HDR)
	$(CC) $(CFLAGS) -DANON_STAGE_1270 -o $@ anon-carrier-probe-1270.c

ap-cr3-probe-1270.elf: ap-cr3-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ ap-cr3-probe-1270.c

gpuvm-preflight-1270.elf: gpuvm_preflight_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gpuvm_preflight_1270.c

gpuvm-map-probe-1270.elf: gpuvm_map_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gpuvm_map_probe_1270.c

gpuvm-dma-probe-1270.elf: gpuvm_dma_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gpuvm_dma_probe_1270.c

gpuvm-pa-preflight-1270.elf: gpuvm_pa_preflight_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gpuvm_pa_preflight_1270.c

gpuvm-known-data-probe-1270.elf: gpuvm_known_data_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gpuvm_known_data_probe_1270.c

gpuvm-xotext-probe-1270.elf: gpuvm_xotext_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gpuvm_xotext_probe_1270.c

gpuvm-xotext-pivot-probe-1270.elf: gpuvm_xotext_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DTARGET_TEXT_OFFSET=0x2A8000ULL -o $@ gpuvm_xotext_probe_1270.c

gpuvm-xotext-db-probe-1270.elf: gpuvm_xotext_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DTARGET_DB_HANDLER=1 -o $@ gpuvm_xotext_probe_1270.c

$(DIAG_HDR): $(PAYLOAD_DIR)/break_hv_diag.c $(PAYLOAD_DIR)/entry.S $(PAYLOAD_DIR)/link.ld $(PAYLOAD_DIR)/build_diag.sh
	sh $(PAYLOAD_DIR)/build_diag.sh

byepervisor_1270_diag.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -o $@ $(filter %.c,$^)

byepervisor_1270_prestage_diag.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c carrier_1270.c include/carrier_1270.h include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -DAP_DIAG_PRESTAGE_1270 -o $@ $(filter %.c,$^)

byepervisor_1270_prep_only.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c carrier_1270.c include/carrier_1270.h include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

byepervisor_1270_install_only.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c carrier_1270.c include/carrier_1270.h include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_INSTALL_ONLY_1270 -o $@ $(filter %.c,$^)

byepervisor_1270_trigger_only.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c carrier_1270.c include/carrier_1270.h include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_TRIGGER_ONLY_1270 -o $@ $(filter %.c,$^)

byepervisor_1270_trigger_prepare_only.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c carrier_1270.c include/carrier_1270.h include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -DAP_DIAG_PRESTAGE_1270 -DAP_DIAG_TRIGGER_ONLY_1270 -DAP_DIAG_TRIGGER_PREP_ONLY_1270 -o $@ $(filter %.c,$^)

$(PAYLOAD_HDR): $(PAYLOAD_DIR)/break_hv.c $(PAYLOAD_DIR)/entry.S $(PAYLOAD_DIR)/link.ld
	sh $(PAYLOAD_DIR)/build.sh

$(ELF): $(PAYLOAD_HDR) main.c kdlsym.c khook.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

clean:
	rm -f cpu-tss-map-probe-1270.elf
	rm -f cpu-tss-live-probe-1270.elf
	rm -f sysent-getuid-probe-1270.elf
	rm -f second-db-prepare-1270.elf second-db-trigger-1270.elf
	rm -f resume-stock-probe-1270.elf
	rm -f resume-stock-prepare-1270.elf
	rm -f resume-stock-apcr3-1270.elf
	rm -f ap-cr3-high-probe-1270.elf
	rm -f ap-entry-prepare-1270.elf ap-entry-probe-1270.elf ap-entry-stack-probe-1270.elf
	rm -f pivot-iret-prepare-1270.elf pivot-iret-trigger-1270.elf
	rm -f pivot-pop-rax-prepare-1270.elf pivot-pop-rax-trigger-1270.elf
	rm -f pivot-dbreg-prepare-1270.elf pivot-dbreg-trigger-1270.elf
	rm -f pivot-candidate-iret-prepare-1270.elf pivot-candidate-iret-trigger-1270.elf
	rm -f pivot-continuation-prepare-1270.elf pivot-continuation-trigger-1270.elf
	rm -f pivot-continuation-alt-prepare-1270.elf pivot-continuation-alt-trigger-1270.elf
	rm -f pivot-pop-rcx-prepare-1270.elf pivot-pop-rcx-trigger-1270.elf
	rm -f pivot-pop-r8-prepare-1270.elf pivot-pop-r8-trigger-1270.elf
	rm -f pivot-store-rcx-rax-prepare-1270.elf pivot-store-rcx-rax-trigger-1270.elf
	rm -f pivot-set-dr7-prepare-1270.elf pivot-set-dr7-trigger-1270.elf
	rm -f pivot-pop-pair-prepare-1270.elf pivot-pop-pair-trigger-1270.elf
	rm -f pivot-pop-rsp-prepare-1270.elf pivot-pop-rsp-trigger-1270.elf
	rm -f $(ELF)
	rm -f preflight-1270.elf
	rm -f text-base-pte-inspect-1270.elf text-base-pte-prepare-1270.elf
	rm -f text-base-status-1270.elf
	rm -f facs-preflight-1270.elf
	rm -f gadget-page-probe-1270.elf
	rm -f carrier-read-probe-1270.elf
	rm -f carrier-layout-probe-1270.elf
	rm -f carrier-save-probe-1270.elf
	rm -f carrier-save-current-probe-1270.elf
	rm -f carrier-neighborhood-probe-1270.elf
	rm -f fixed-ap-prepare-1270.elf fixed-ap-probe-1270.elf
	rm -f fixed-ap-hvread-prepare-1270.elf fixed-ap-hvread-probe-1270.elf
	rm -f fixed-ap-pcpus-prepare-1270.elf fixed-ap-pcpus-probe-1270.elf
	rm -f fixed-ap-hvpair-prepare-1270.elf fixed-ap-hvpair-probe-1270.elf
	rm -f fixed-ap-vmcbptr-prepare-1270.elf fixed-ap-vmcbptr-probe-1270.elf
	rm -f fixed-ap-vmcbfields-prepare-1270.elf fixed-ap-vmcbfields-probe-1270.elf
	rm -f fixed-ap-vmcb15fields-prepare-1270.elf fixed-ap-vmcb15fields-probe-1270.elf
	rm -f fixed-ap-vmcb-survey-prepare-1270.elf fixed-ap-vmcb-survey-probe-1270.elf
	rm -f fixed-ap-vmcb-poll-prepare-1270.elf fixed-ap-vmcb-poll-probe-1270.elf
	rm -f fixed-ap-vmcb-strike-prepare-1270.elf fixed-ap-vmcb-strike-probe-1270.elf
	rm -f fixed-ap-arg-prepare-1270.elf fixed-ap-arg-probe-1270.elf
	rm -f fixed-ap-nptentry-probe-1270.elf
	rm -f fixed-ap-nptpair-prepare-1270.elf fixed-ap-nptpair-probe-1270.elf
	rm -f fixed-ap-nptdump-prepare-1270.elf fixed-ap-nptdump-probe-1270.elf
	rm -f fixed-ap-vmcb-rw-prepare-1270.elf fixed-ap-vmcb-rw-probe-1270.elf
	rm -f owned-resume-ap-prepare-1270.elf owned-resume-ap-probe-1270.elf
	rm -f owned-carrier-probe-1270.elf
	rm -f owned-carrier-map-probe-1270.elf
	rm -f anon-carrier-probe-1270.elf
	rm -f anon-carrier-hold-probe-1270.elf
	rm -f anon-carrier-stage-probe-1270.elf
	rm -f ap-cr3-probe-1270.elf
	rm -f gpuvm-preflight-1270.elf
	rm -f gpuvm-map-probe-1270.elf
	rm -f gpuvm-dma-probe-1270.elf
	rm -f gpuvm-pa-preflight-1270.elf
	rm -f gpuvm-known-data-probe-1270.elf
	rm -f gpuvm-xotext-probe-1270.elf
	rm -f gpuvm-xotext-pivot-probe-1270.elf
	rm -f gpuvm-xotext-db-probe-1270.elf
	rm -f byepervisor_1270_diag.elf
	rm -f byepervisor_1270_prestage_diag.elf
	rm -f byepervisor_1270_ap_readonly_diag.elf
	rm -f byepervisor_1270_prep_only.elf
	rm -f byepervisor_1270_install_only.elf
	rm -f byepervisor_1270_trigger_only.elf
	rm -f byepervisor_1270_trigger_prepare_only.elf
	rm -f $(PAYLOAD_DIR)/break_hv_diag.h $(PAYLOAD_DIR)/break_hv_diag.bin $(PAYLOAD_DIR)/break_hv_diag.elf $(PAYLOAD_DIR)/break_hv_diag.o
	rm -f $(PAYLOAD_DIR)/break_hv.h $(PAYLOAD_DIR)/break_hv.bin $(PAYLOAD_DIR)/break_hv.elf $(PAYLOAD_DIR)/*.o

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

debug: $(ELF)
	gdb-multiarch \
	-ex "set architecture i386:x86-64" \
	-ex "target extended-remote $(PS5_HOST):2159" \
	-ex "file $(ELF)" \
	-ex "remote put $(ELF) /data/$(ELF)" \
	-ex "set remote exec-file /data/$(ELF)" \
	-ex "start"

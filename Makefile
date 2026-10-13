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

facs-preflight-1270.elf: facs_preflight_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ facs_preflight_1270.c

gadget-page-probe-1270.elf: gadget_page_probe_1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ gadget_page_probe_1270.c

carrier-read-probe-1270.elf: carrier-read-probe-1270.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ carrier-read-probe-1270.c

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

byepervisor_1270_prestage_diag.elf: $(DIAG_HDR) main.c kdlsym.c khook.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -DAP_DIAGNOSTIC_1270 -DAP_DIAG_PRESTAGE_1270 -o $@ $(filter %.c,$^)

$(PAYLOAD_HDR): $(PAYLOAD_DIR)/break_hv.c $(PAYLOAD_DIR)/entry.S $(PAYLOAD_DIR)/link.ld
	sh $(PAYLOAD_DIR)/build.sh

$(ELF): $(PAYLOAD_HDR) main.c kdlsym.c khook.c paging.c util.c include/offsets/12_70_candidate.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^)

clean:
	rm -f $(ELF)
	rm -f preflight-1270.elf
	rm -f facs-preflight-1270.elf
	rm -f gadget-page-probe-1270.elf
	rm -f carrier-read-probe-1270.elf
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

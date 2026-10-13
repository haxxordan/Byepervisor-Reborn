#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#include "kdlsym.h"
#include "offsets/12_70_candidate.h"

/* Bounded mapping probe. No GPU ioctl, DMA, or page-table write. */
#define FW_1270             0x12700000U
#define DATA_GVMSPACE_OFF   0x2E7E570ULL
#define GVM_ENTRY_SIZE      0x100ULL
#define GVM_ENTRY_COUNT     16U
#define BUFFER_SIZE         0x200000ULL
#define MAX_PHYSICAL_PA     0x400000000ULL
#define GPU_ADDR_MASK       0x0000FFFFFFFFFFC0ULL
#define GPU_VALID           1ULL
#define GPU_IS_PTE          (1ULL << 54)
#define PROT_GPU_READ       0x10
#define PROT_GPU_WRITE      0x20
#define MAP_NO_COALESCE     0x00400000
#define DMEM_TYPE_GARLIC    0xb

typedef int (*fn_alloc_dmem)(off_t, off_t, size_t, size_t, int, off_t *);
typedef int (*fn_map_dmem)(void **, size_t, int, int, off_t, size_t);
typedef int (*fn_release_dmem)(off_t, size_t);

typedef struct {
  unsigned char prefix[45];
  char message[3075];
} notify_request_t;
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

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

static int get_dmap(uint64_t text, uint64_t *dmap)
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
  return 0;
}

static unsigned inspect_vmid(uint64_t gvmspace, uint64_t dmap,
                             unsigned vmid, uint64_t buffer_va)
{
  uint64_t row = gvmspace + vmid * GVM_ENTRY_SIZE;
  uint64_t state = 0, page_dir = 0, e4 = 0, e3 = 0, e2 = 0;
  unsigned level = 0;
  if (read_qword(row, &state) != 0 ||
      read_qword(row + 0x38, &page_dir) != 0)
    return 0;
  if ((uint32_t)state == 0x20 || !page_dir ||
      (page_dir >> 48) != 0xffffULL || (page_dir & 0xfff))
    return 0;

  uint64_t i4 = (buffer_va >> 39) & 0x1ff;
  uint64_t i3 = (buffer_va >> 30) & 0x1ff;
  uint64_t i2 = (buffer_va >> 21) & 0x1ff;
  if (read_qword(page_dir + i4 * 8, &e4) != 0)
    goto report;
  if (!(e4 & GPU_VALID))
    goto report;
  level = 1;
  uint64_t pdpt_pa = e4 & GPU_ADDR_MASK;
  if (read_phys(dmap, pdpt_pa + i3 * 8, &e3) != 0)
    goto report;
  if (!(e3 & GPU_VALID))
    goto report;
  level = 2;
  uint64_t pd_pa = e3 & GPU_ADDR_MASK;
  if (read_phys(dmap, pd_pa + i2 * 8, &e2) != 0)
    goto report;
  if (e2 & GPU_VALID)
    level = 3;

report:
  klog_printf("GVMMAP1270,1,vm,index=%u,state=0x%016" PRIx64
              ",dir=0x%016" PRIx64 ",level=%u,pml4e=0x%016" PRIx64
              ",pdpte=0x%016" PRIx64 ",pde=0x%016" PRIx64
              ",leaf_2m=%u\n", vmid, state, page_dir, level,
              e4, e3, e2, (unsigned)(level == 3 && (e2 & GPU_IS_PTE) != 0));
  return level == 3;
}

int main(void)
{
  int result = 1, gc = -1, allocated = 0, mapped = 0;
  off_t offset = 0;
  void *buffer = NULL;
  fn_alloc_dmem allocate = NULL;
  fn_map_dmem map = NULL;
  fn_release_dmem release = NULL;
  uint32_t fw = kernel_get_fw_version();
  uint64_t text = (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
  uint64_t data = (uint64_t)KERNEL_ADDRESS_DATA_BASE;
  uint64_t dmap = 0;
  unsigned matches = 0;
  uint64_t idt = text + g_sym_map_1270[KERNEL_SYM_IDT_ARRAY];

  if ((fw & 0xffff0000U) != FW_1270 || !text ||
      data <= text || data - text != 0xD50000ULL ||
      idt != data + 0x2E88300ULL ||
      idt - 0x9D90ULL != data + DATA_GVMSPACE_OFF ||
      get_dmap(text, &dmap) != 0) {
    klog_printf("GVMMAP1270,1,stop,stage=preflight,fw=0x%08x\n", fw);
    return 1;
  }
  klog_printf("GVMMAP1270,1,start,fw=0x%08x,dmap=0x%016" PRIx64 "\n", fw, dmap);

  allocate = (fn_alloc_dmem)dlsym(RTLD_DEFAULT, "sceKernelAllocateDirectMemory");
  map = (fn_map_dmem)dlsym(RTLD_DEFAULT, "sceKernelMapDirectMemory");
  release = (fn_release_dmem)dlsym(RTLD_DEFAULT, "sceKernelReleaseDirectMemory");
  if (!allocate || !map || !release) {
    klog_printf("GVMMAP1270,1,stop,stage=symbols,alloc=%u,map=%u,release=%u\n",
                !!allocate, !!map, !!release);
    goto done;
  }

  gc = open("/dev/gc", O_RDWR);
  if (gc < 0)
    gc = open("/dev/gc0", O_RDWR);
  if (gc < 0) {
    klog_printf("GVMMAP1270,1,stop,stage=device,errno=%d\n", errno);
    goto done;
  }
  klog_printf("GVMMAP1270,1,device,fd=%d\n", gc);

  int rc = allocate(0, MAX_PHYSICAL_PA, BUFFER_SIZE, BUFFER_SIZE,
                    DMEM_TYPE_GARLIC, &offset);
  klog_printf("GVMMAP1270,1,alloc,result=%d,offset=0x%016" PRIx64 "\n",
              rc, (uint64_t)offset);
  if (rc != 0)
    goto done;
  allocated = 1;

  int prot = PROT_READ | PROT_WRITE | PROT_GPU_READ | PROT_GPU_WRITE;
  rc = map(&buffer, BUFFER_SIZE, prot, MAP_NO_COALESCE, offset, BUFFER_SIZE);
  klog_printf("GVMMAP1270,1,map,result=%d,va=0x%016" PRIx64 "\n",
              rc, (uint64_t)(uintptr_t)buffer);
  if (rc != 0 || !buffer)
    goto done;
  mapped = 1;

  uint64_t gvmspace = data + DATA_GVMSPACE_OFF;
  for (unsigned i = 0; i < GVM_ENTRY_COUNT; i++)
    matches += inspect_vmid(gvmspace, dmap, i, (uint64_t)(uintptr_t)buffer);
  klog_printf("GVMMAP1270,1,result,matches=%u\n", matches);
  result = matches ? 0 : 1;

done:
  if (mapped) {
    int rc = munmap(buffer, BUFFER_SIZE);
    klog_printf("GVMMAP1270,1,cleanup,munmap_result=%d,errno=%d\n", rc, errno);
    if (rc != 0)
      result = 1;
  }
  if (allocated) {
    int rc = release(offset, BUFFER_SIZE);
    klog_printf("GVMMAP1270,1,cleanup,release_result=%d\n", rc);
    if (rc != 0)
      result = 1;
  }
  if (gc >= 0)
    close(gc);
  klog_printf("GVMMAP1270,1,end,%s\n", result == 0 ? "ok" : "stop");
  notify_result(result == 0 ? "12.70 GPU mapping probe complete; see GVMMAP1270 log"
                            : "12.70 GPU mapping probe stopped; see GVMMAP1270 log");
  return result;
}

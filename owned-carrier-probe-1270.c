#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <sys/types.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define FW_1270 0x12700000U
#define EXEC_RANGE_END 0x40000000ULL

typedef int (*fn_alloc_dmem)(off_t, off_t, size_t, size_t, int, off_t *);
typedef int (*fn_release_dmem)(off_t, size_t);

int main(void)
{
  uint32_t fw = kernel_get_fw_version();
  if ((fw & 0xffff0000U) != FW_1270) {
    klog_printf("OWNCARR1270,1,stop,stage=firmware,fw=0x%08x\n", fw);
    return 1;
  }

  fn_alloc_dmem allocate =
      (fn_alloc_dmem)dlsym(RTLD_DEFAULT, "sceKernelAllocateDirectMemory");
  fn_release_dmem release =
      (fn_release_dmem)dlsym(RTLD_DEFAULT, "sceKernelReleaseDirectMemory");
  if (!allocate || !release) {
    klog_printf("OWNCARR1270,1,stop,stage=symbols,alloc=%u,release=%u\n",
                !!allocate, !!release);
    return 1;
  }

  const size_t sizes[] = {0x1000, 0x200000};
  klog_printf("OWNCARR1270,1,start,range=[0,0x%08" PRIx64 ")\n",
              EXEC_RANGE_END);
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    off_t offset = 0;
    size_t size = sizes[i];
    int rc = allocate(0, (off_t)EXEC_RANGE_END, size, size, 0, &offset);
    klog_printf("OWNCARR1270,1,alloc,size=0x%zx,type=0,result=%d,offset=0x%016" PRIx64 "\n",
                size, rc, (uint64_t)offset);
    if (rc == 0) {
      int release_rc = release(offset, size);
      klog_printf("OWNCARR1270,1,release,size=0x%zx,result=%d\n",
                  size, release_rc);
      klog_printf("OWNCARR1270,1,end,%s\n",
                  release_rc == 0 ? "candidate" : "stop");
      return release_rc == 0 ? 0 : 1;
    }
  }
  klog_printf("OWNCARR1270,1,end,unavailable\n");
  return 0;
}

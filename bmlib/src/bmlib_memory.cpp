#include "bmlib_memory.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "api.h"
#include "bmlib_log.h"
#include "bmlib_internal.h"
#ifdef __linux__
    #include "linux/bmlib_ioctl.h"
    #include <asm/ioctl.h>
    #include <linux/types.h>
    #include <sys/ioctl.h>
    #include <sys/mman.h>
    #include <sys/time.h>
    #include <unistd.h>
    #include "bmlib_mmpool.h"
    #include "ion.h"
    #include "rbtree.h"
#else
    #include "window\bmlib_ioctl.h"
    #include <io.h>
    #include <set>
    EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#endif

#define BMLIB_MEMORY_LOG_TAG "bmlib_memory"
#define BMLIB_MEM_GUARD_SIZE 1024
static unsigned char guard_data[BMLIB_MEM_GUARD_SIZE] = {0};

#ifdef _WIN32
    u64 bm_getpagesize(void){
       SYSTEM_INFO sysInfo;
       GetSystemInfo(&sysInfo);
       return sysInfo.dwPageSize;
    }
#endif

static inline void guard_mem_set(bm_handle_t handle, u64 addr, u64 size) {
  if(guard_data[0]==0) {
    for(int i=0; i<sizeof(guard_data); i++) {
      guard_data[i] = 0x0A;
    }
  }
  bm_device_mem_u64_t mem = bm_mem_from_device_u64(addr+size, sizeof(guard_data));
  bm_status_t ret = bm_memcpy_s2d_u64(handle, mem, guard_data);
  if(ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING, "mem guard set data failed! addr=0x%llx, size=%lld, status=%d\n", addr, size, ret);
  }
}

static inline void guard_mem_check(bm_handle_t handle, u64 addr, u64 size) {
  unsigned char device_data[BMLIB_MEM_GUARD_SIZE];
  bm_device_mem_u64_t mem = bm_mem_from_device_u64(addr+size, sizeof(guard_data));
  bm_status_t ret = bm_memcpy_d2s_u64(handle, device_data, mem);
  if(ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING, "mem guard fetch data failed! addr=0x%llx, size=%lld, status=%d\n", addr, size, ret);
    return;
  }
  if(memcmp(device_data, guard_data, sizeof(guard_data)) != 0) {
    char filename[128];
    snprintf(filename, sizeof(filename), "device_data_0x%llx_%lld.dat",addr, size);
    FILE* fp = fopen(filename, "wb");
    fwrite(device_data, 1, sizeof(device_data), fp);
    fclose(fp);

    fp = fopen("guard_data.dat", "wb");
    fwrite(guard_data, 1, sizeof(guard_data), fp);
    fclose(fp);

    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_FATAL, "mem guard check data failed! addr=0x%llx, size=%lld\n", addr, size);
    throw "device mem is overwrite!";
  }
}

#ifdef __linux__
static bm_status_t buffer_add(bm_handle_t handle,
          struct bm_mem_paddr *buffer)
{
  struct rb_node **p = &(handle->root.rb_node);
  struct rb_node *parent = NULL;
  struct bm_mem_paddr *entry;
  long long result;

  while (*p) {
    entry = container_of(*p, struct bm_mem_paddr, node);
    result = buffer->paddr - entry->paddr;
    parent = *p;
    if (result < 0)
      p = &((*p)->rb_left);
    else if (result > 0)
      p = &((*p)->rb_right);
    else
    return BM_ERR_FAILURE;
  }

  rb_link_node(&buffer->node, parent, p);
  rb_insert_color(&buffer->node, &handle->root);

  return BM_SUCCESS;
}

static struct bm_mem_paddr *buffer_search(bm_handle_t handle,
                  unsigned long long paddr)
{
  struct rb_node *node = handle->root.rb_node;
  long long result;

  while (node) {
    struct bm_mem_paddr *data = container_of(node, struct bm_mem_paddr, node);

    result = paddr - data->paddr;
    if (result < 0)
      node = node->rb_left;
    else if (result > 0)
      node = node->rb_right;
    else
      return data;
  }

  return NULL;
}
#endif

u32 bm_mem_get_size(struct bm_mem_desc mem) { return mem.size; }

u64 sg_mem_get_size(struct sg_mem_desc mem) { return mem.size; }

u64 bm_mem_get_size_u64(struct bm_mem_desc_u64 mem) { return mem.size; }

static u64 bm_get_neuron_size(int n, int c, int h, int w) {
  u64 tensor_dim = (u64)n * (u64)c * (u64)h * (u64)w * FLOAT_SIZE;
  if (tensor_dim >= 0x400000000ULL)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "tensor_dim = 0x%llx is illegal %s: %s: %d\n",
           tensor_dim, __FILE__, __func__, __LINE__);
  return tensor_dim;
}

static u64 sg_get_neuron_size(u64 n, u64 c, u64 h, u64 w) {
  u64 tensor_dim = n * c * h * w * FLOAT_SIZE;
  if (tensor_dim >= 0x400000000ULL)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "tensor_dim = 0x%llx is illegal %s: %s: %d\n",
           tensor_dim, __FILE__, __func__, __LINE__);
  return tensor_dim;
}

static u64 bm_get_neuron_size_u64(u64 n, u64 c, u64 h, u64 w) {
  u64 tensor_dim = n * c * h * w * FLOAT_SIZE;
  if (tensor_dim >= 0x400000000ULL)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "tensor_dim = 0x%llx is illegal %s: %s: %d\n",
           tensor_dim, __FILE__, __func__, __LINE__);
  return tensor_dim;
}

static u32 bm_get_coeff_size(int coeff_count) {
  return (coeff_count * FLOAT_SIZE);
}

static u64 sg_get_coeff_size(u64 coeff_count) {
  return (coeff_count * FLOAT_SIZE);
}

static u64 bm_get_coeff_size_u64(u64 coeff_count) {
  return (coeff_count * FLOAT_SIZE);
}

bm_mem_type_t bm_mem_get_type(struct bm_mem_desc mem) {
    return mem.flags.u.mem_type;
}

bm_mem_type_t sg_mem_get_type(struct sg_mem_desc mem) {
    return mem.flags.u.mem_type;
}

bm_mem_type_t bm_mem_get_type_u64(struct bm_mem_desc_u64 mem) {
    return mem.flags.u.mem_type;
}

bm_device_mem_t bm_mem_from_device(unsigned long long device_addr,
                                   unsigned int       len) {
    bm_device_mem_t mem;
    memset(&mem, 0x0, sizeof(bm_device_mem_t));
    mem.u.device.device_addr = device_addr;
    mem.flags.u.mem_type     = BM_MEM_TYPE_DEVICE;
    mem.size                 = len;
    return mem;
}

sg_device_mem_t sg_mem_from_device(unsigned long long device_addr,
                                   unsigned long long len) {
    sg_device_mem_t mem;
    memset(&mem, 0x0, sizeof(sg_device_mem_t));
    mem.u.device.device_addr = device_addr;
    mem.flags.u.mem_type     = BM_MEM_TYPE_DEVICE;
    mem.size                 = len;
    return mem;
}

bm_device_mem_u64_t bm_mem_from_device_u64(unsigned long long device_addr,
                                   unsigned long long len) {
    bm_device_mem_u64_t mem;
    memset(&mem, 0x0, sizeof(bm_device_mem_u64_t));
    mem.u.device.device_addr = device_addr;
    mem.flags.u.mem_type     = BM_MEM_TYPE_DEVICE;
    mem.size                 = len;
    return mem;
}

u32 bm_mem_get_device_size(struct bm_mem_desc mem) {
  if (bm_mem_get_type(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
  return mem.size;
}

u64 sg_mem_get_device_size(struct sg_mem_desc mem) {
  if (sg_mem_get_type(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
  return mem.size;
}

u64 bm_mem_get_device_size_u64(struct bm_mem_desc_u64 mem) {
  if (bm_mem_get_type_u64(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
  return mem.size;
}

void bm_mem_set_device_size(struct bm_mem_desc* pmem, unsigned int size) {
  if (size % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "size = 0x%x is illegal %s: %s: %d\n", size,
           __FILE__, __func__, __LINE__);
  pmem->size = size;
}

void sg_mem_set_device_size(struct sg_mem_desc* pmem, unsigned long long size) {
  if (size % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "size = 0x%x is illegal %s: %s: %d\n", size,
           __FILE__, __func__, __LINE__);
  pmem->size = size;
}

void bm_mem_set_device_size_u64(struct bm_mem_desc_u64* pmem, unsigned long long size) {
  if (size % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "size = 0x%x is illegal %s: %s: %d\n", size,
           __FILE__, __func__, __LINE__);
  pmem->size = size;
}

u64 bm_mem_get_device_addr(struct bm_mem_desc mem) {
  if (bm_mem_get_type(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
  return mem.u.device.device_addr;
}

u64 sg_mem_get_device_addr(struct sg_mem_desc mem) {
  if (sg_mem_get_type(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
  return mem.u.device.device_addr;
}

u64 bm_mem_get_device_addr_u64(struct bm_mem_desc_u64 mem) {
  if (bm_mem_get_type_u64(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
  return mem.u.device.device_addr;
}

void bm_mem_set_device_addr(struct bm_mem_desc* pmem, unsigned long long addr) {
  if (addr % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "addr = 0x%llx  is illegal %s: %s: %d\n", addr,
           __FILE__, __func__, __LINE__);

  pmem->u.device.device_addr = addr;
}

void sg_mem_set_device_addr(struct sg_mem_desc* pmem, unsigned long long addr) {
  if (addr % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "addr = 0x%llx  is illegal %s: %s: %d\n", addr,
           __FILE__, __func__, __LINE__);

  pmem->u.device.device_addr = addr;
}

void bm_mem_set_device_addr_u64(struct bm_mem_desc_u64* pmem, unsigned long long addr) {
  if (addr % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
           "addr = 0x%llx  is illegal %s: %s: %d\n", addr,
           __FILE__, __func__, __LINE__);

  pmem->u.device.device_addr = addr;
}

void bm_mem_set_system_addr(struct bm_mem_desc* pmem, void *addr) {
  if ((u64)addr % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "addr = 0x%llx  is illegal %s: %s: %d\n", (u64)addr,
           __FILE__, __func__, __LINE__);

  pmem->u.system.system_addr = addr;
}

void sg_mem_set_system_addr(struct sg_mem_desc* pmem, void *addr) {
  if ((u64)addr % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "addr = 0x%llx  is illegal %s: %s: %d\n", (u64)addr,
           __FILE__, __func__, __LINE__);

  pmem->u.system.system_addr = addr;
}

void bm_mem_set_system_addr_u64(struct bm_mem_desc_u64* pmem, void *addr) {
  if ((u64)addr % sizeof(float) != 0)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "addr = 0x%llx  is illegal %s: %s: %d\n", (u64)addr,
           __FILE__, __func__, __LINE__);

  pmem->u.system.system_addr = addr;
}

void *bm_mem_get_system_addr(struct bm_mem_desc mem) {
  if (bm_mem_get_type(mem) != BM_MEM_TYPE_SYSTEM)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
  return mem.u.system.system_addr;
}

void *sg_mem_get_system_addr(struct sg_mem_desc mem) {
  if (sg_mem_get_type(mem) != BM_MEM_TYPE_SYSTEM)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
  return mem.u.system.system_addr;
}

void *bm_mem_get_system_addr_u64(struct bm_mem_desc_u64 mem) {
  if (bm_mem_get_type_u64(mem) != BM_MEM_TYPE_SYSTEM)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
  return mem.u.system.system_addr;
}

#ifndef USING_CMODEL
bm_status_t bm_get_carveout_heap_id(bm_handle_t ctx) {
#ifdef __linux__
  struct ion_heap_query query {};
  struct ion_heap_data heap_data[ION_NUM_HEAP_IDS];
  int ret;

  query.cnt = ION_NUM_HEAP_IDS;
  query.heaps = (unsigned long int)&heap_data[0];
  ret = platform_ioctl(ctx, ION_IOC_HEAP_QUERY, &query);
  if (ret != 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "io query heap failed\n");
    return BM_ERR_FAILURE;
  }

  ctx->heap_cnt = 0;
  for (unsigned int i = 0; i < query.cnt; i++) {
    if (heap_data[i].type == ION_HEAP_TYPE_CARVEOUT &&
        strcmp(heap_data[i].name, "npu") == 0) {
      ctx->carveout_heap_id[0] = heap_data[i].heap_id;
      ctx->heap_cnt++;
    } else if (heap_data[i].type == ION_HEAP_TYPE_CARVEOUT &&
               strcmp(heap_data[i].name, "vpp") == 0) {
      ctx->carveout_heap_id[1] = heap_data[i].heap_id;
      ctx->heap_cnt++;
    } else if (heap_data[i].type == ION_HEAP_TYPE_CARVEOUT &&
               strcmp(heap_data[i].name, "vpu") == 0) {
      ctx->carveout_heap_id[2] = heap_data[i].heap_id;
      ctx->heap_cnt++;
    }
    if (ctx->heap_cnt == ION_MAX_HEAP_CNT) break;
  }

  if (ctx->heap_cnt < 2) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "no carveout heap found\n");
    return BM_ERR_FAILURE;
  }
#endif
  return BM_SUCCESS;
}


bm_status_t bm_total_gmem(bm_handle_t ctx, u64 *total) {
#ifdef __linux__
  struct ion_custom_data ion_data;
  struct bitmain_heap_info bm_heap_info;
  u64 total_size = 0;
  if (ctx->ion_fd) {
    ion_data.cmd = ION_IOC_BITMAIN_GET_HEAP_INFO;
    ion_data.arg = (unsigned long)&bm_heap_info;
    for (int i = 0; i < ctx->heap_cnt; i++) {
      bm_heap_info.id = ctx->carveout_heap_id[i];
      if (0 != ioctl(ctx->ion_fd, ION_IOC_CUSTOM, &ion_data))
        return BM_ERR_FAILURE;
      total_size += bm_heap_info.total_size;
    }
    *total = total_size;
  } else
#endif
  {
        if (0 != platform_ioctl(ctx, BMDEV_TOTAL_GMEM, total)) return BM_ERR_FAILURE;
  }
  return BM_SUCCESS;
}

bm_status_t bm_avail_gmem(bm_handle_t ctx, u64 *avail) {
#ifdef __linux__
  struct ion_custom_data ion_data;
  struct bitmain_heap_info bm_heap_info;
  u64 avail_size = 0;

  if (ctx->ion_fd) {
    ion_data.cmd = ION_IOC_BITMAIN_GET_HEAP_INFO;
    ion_data.arg = (unsigned long)&bm_heap_info;
    for (int i = 0; i < ctx->heap_cnt; i++) {
      bm_heap_info.id = ctx->carveout_heap_id[i];
      if (0 != ioctl(ctx->ion_fd, ION_IOC_CUSTOM, &ion_data))
        return BM_ERR_FAILURE;
      avail_size += bm_heap_info.avail_size;
    }
    *avail = avail_size;
  } else
#endif
  {
    if (0 != platform_ioctl(ctx, BMDEV_AVAIL_GMEM, avail)) return BM_ERR_FAILURE;
  }
  return BM_SUCCESS;
}

static int bm_alloc_gmem(bm_handle_t ctx, bm_device_mem_t *pmem, int heap_id_mask) {
  int ret;

  struct ion_allocation_data alloc_data;
  memset(&alloc_data, 0, sizeof(alloc_data));
  alloc_data.len = pmem->size;
  alloc_data.flags = 0;
  bm_profile_record_mem_begin(ctx);
#ifdef __linux__
  if (ctx->ion_fd) {
    // try all heaps as heap_id_mask set
    alloc_data.heap_id_mask = heap_id_mask;
    ret = ioctl(ctx->ion_fd, ION_IOC_ALLOC, &alloc_data);

    if (ret == 0)
      ioctl(ctx->dev_fd, BMDEV_ALLOC_GMEM_ION, pmem);
  } else
#endif
  {
    alloc_data.heap_id_mask = heap_id_mask;
    ret = platform_ioctl(ctx, BMDEV_ALLOC_GMEM, &alloc_data);
    pmem->flags.u.gmem_heapid = alloc_data.heap_id;
  }

  if (ret) {
    pmem->u.device.device_addr = BM_MEM_ADDR_NULL;
    pmem->u.device.dmabuf_fd = -1;
    return BM_ERR_FAILURE;
  }
  pmem->u.device.device_addr = alloc_data.paddr;
  pmem->u.device.dmabuf_fd = alloc_data.fd;
  bm_profile_record_mem_end(ctx, bm_mem_op_type_t::ALLOC, alloc_data.paddr, pmem->size);
  return BM_SUCCESS;
}

static int sg_alloc_gmem(bm_handle_t ctx, sg_device_mem_t *pmem, int heap_id_mask) {
  int ret;

  struct ion_allocation_data alloc_data;
  memset(&alloc_data, 0, sizeof(alloc_data));
  alloc_data.len = pmem->size;
  alloc_data.flags = 0;
  bm_profile_record_mem_begin(ctx);
#ifdef __linux__
  if (ctx->ion_fd) {
    // try all heaps as heap_id_mask set
    for (int i = 0; i < ctx->heap_cnt; i++) {
      if (((heap_id_mask >> i) & 0x1) == 0x1) {
          pmem->flags.u.gmem_heapid = ctx->carveout_heap_id[i];
          alloc_data.heap_id_mask = (1 << ctx->carveout_heap_id[i]);
          ret = ioctl(ctx->ion_fd, ION_IOC_ALLOC, &alloc_data);

          if (ret == 0) {
            ioctl(ctx->dev_fd, BMDEV_ALLOC_GMEM_ION_U64, pmem);
            break;
          }
        }
      }
  } else
#endif
  {
    alloc_data.heap_id_mask = heap_id_mask;
    ret = platform_ioctl(ctx, BMDEV_ALLOC_GMEM, &alloc_data);
    pmem->flags.u.gmem_heapid = alloc_data.heap_id;
  }

  if (ret) {
    pmem->u.device.device_addr = BM_MEM_ADDR_NULL;
    pmem->u.device.dmabuf_fd = -1;
    return BM_ERR_FAILURE;
  }
  pmem->u.device.device_addr = alloc_data.paddr;
  pmem->u.device.dmabuf_fd = alloc_data.fd;
  bm_profile_record_mem_end(ctx, bm_mem_op_type_t::ALLOC, alloc_data.paddr, pmem->size);
  return BM_SUCCESS;
}

static int bm_alloc_gmem_u64(bm_handle_t ctx, bm_device_mem_u64_t *pmem, int heap_id_mask) {
  int ret;

  struct ion_allocation_data alloc_data;
  memset(&alloc_data, 0, sizeof(alloc_data));
  alloc_data.len = pmem->size;
  alloc_data.flags = 0;
  bm_profile_record_mem_begin(ctx);
#ifdef __linux__
  if (ctx->ion_fd) {
    // try all heaps as heap_id_mask set
    alloc_data.heap_id_mask = heap_id_mask;
    ret = ioctl(ctx->ion_fd, ION_IOC_ALLOC, &alloc_data);

    if (ret == 0)
      ioctl(ctx->dev_fd, BMDEV_ALLOC_GMEM_ION_U64, pmem);
  } else
#endif
  {
    alloc_data.heap_id_mask = heap_id_mask;
    ret = platform_ioctl(ctx, BMDEV_ALLOC_GMEM, &alloc_data);
    pmem->flags.u.gmem_heapid = alloc_data.heap_id;
  }

  if (ret) {
    pmem->u.device.device_addr = BM_MEM_ADDR_NULL;
    pmem->u.device.dmabuf_fd = -1;
    return BM_ERR_FAILURE;
  }
  pmem->u.device.device_addr = alloc_data.paddr;
  pmem->u.device.dmabuf_fd = alloc_data.fd;
  bm_profile_record_mem_end(ctx, bm_mem_op_type_t::ALLOC, alloc_data.paddr, pmem->size);
  return BM_SUCCESS;
}

static bm_status_t bm_free_gmem(bm_handle_t ctx, bm_device_mem_t *pmem) {
    if (pmem->u.device.device_addr < 0x100000000 || pmem->u.device.device_addr > 0x500000000){
        bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "free gmem addr 0x%llx is invalide!\n",pmem->u.device.device_addr);
        return BM_ERR_FAILURE;
    }
  #ifdef __linux__
    if (close(pmem->u.device.dmabuf_fd)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "free gmem failed!\n");
    return BM_ERR_FAILURE;
  }
  #endif
  bm_profile_record_mem_begin(ctx);
  if (platform_ioctl(ctx, BMDEV_FREE_GMEM, pmem)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(ctx, bm_mem_op_type_t::FREE, pmem->u.device.device_addr, pmem->size);
    return BM_SUCCESS;
}

static bm_status_t sg_free_gmem(bm_handle_t ctx, sg_device_mem_t *pmem) {
  if (pmem->u.device.device_addr < 0x100000000 || pmem->u.device.device_addr > 0x500000000){
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "free gmem addr 0x%llx is invalide!\n",pmem->u.device.device_addr);
      return BM_ERR_FAILURE;
  }
  #ifdef __linux__
    if (close(pmem->u.device.dmabuf_fd)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "free gmem failed!\n");
    return BM_ERR_FAILURE;
  }
  #endif
  bm_profile_record_mem_begin(ctx);
  if (platform_ioctl(ctx, BMDEV_FREE_GMEM_U64, pmem)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(ctx, bm_mem_op_type_t::FREE, pmem->u.device.device_addr, pmem->size);
  return BM_SUCCESS;
}

static bm_status_t bm_free_gmem_u64(bm_handle_t ctx, bm_device_mem_u64_t *pmem) {
    if (pmem->u.device.device_addr < 0x100000000 || pmem->u.device.device_addr > 0x500000000){
        bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "free gmem addr 0x%llx is invalide!\n",pmem->u.device.device_addr);
        return BM_ERR_FAILURE;
    }
  #ifdef __linux__
    if (close(pmem->u.device.dmabuf_fd)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "free gmem failed!\n");
    return BM_ERR_FAILURE;
  }
  #endif
  bm_profile_record_mem_begin(ctx);
  if (platform_ioctl(ctx, BMDEV_FREE_GMEM_U64, pmem)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(ctx, bm_mem_op_type_t::FREE, pmem->u.device.device_addr, pmem->size);
    return BM_SUCCESS;
}
#endif

static bm_status_t __alloc_device_mem_raw(bm_handle_t ctx,
                                          bm_device_mem_t *pmem,
                                          int heap_id_mask) {
  if (ctx == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }
  if(ctx->enable_mem_guard) {
    pmem->size += BMLIB_MEM_GUARD_SIZE;
  }

#ifdef USING_CMODEL
  UNUSED(heap_id_mask);
  u64 addr;
  addr = ctx->bm_dev->bm_device_alloc_mem(pmem->size);
  pmem->u.device.device_addr = addr;
  if (addr == MEM_POOL_ADDR_INVALID) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "No memory in device mem\n");
    return BM_ERR_NOMEM;
  }
#else
  int ret = 0;

  ret = bm_alloc_gmem(ctx, pmem, heap_id_mask);
  if (ret) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bm_alloc_gmem failed, dev_id = %d, size = 0x%x\n",
           ctx->dev_id, pmem->size);
    return BM_ERR_NOMEM;
  }
#ifdef SOC_MODE
  bm_mem_invalidate_device_mem(ctx, pmem);
#endif
#endif

  if(ctx->enable_mem_guard) {
    pmem->size -= BMLIB_MEM_GUARD_SIZE;
    guard_mem_set(ctx, pmem->u.device.device_addr, pmem->size);
  }

  return BM_SUCCESS;
}

static bm_status_t __alloc_sg_device_mem_raw(bm_handle_t ctx,
                                          sg_device_mem_t *pmem,
                                          int heap_id_mask) {
  if (ctx == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }

  if(ctx->enable_mem_guard) {
    pmem->size += BMLIB_MEM_GUARD_SIZE;
  }
#ifdef USING_CMODEL
  UNUSED(heap_id_mask);
  u64 addr;
  addr = ctx->bm_dev->bm_device_alloc_mem(pmem->size);
  pmem->u.device.device_addr = addr;
  if (addr == MEM_POOL_ADDR_INVALID) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "No memory in device mem\n");
    return BM_ERR_NOMEM;
  }
#else
  int ret = 0;

  ret = sg_alloc_gmem(ctx, pmem, heap_id_mask);
  if (ret) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bm_alloc_gmem failed, dev_id = %d, size = 0x%x\n",
           ctx->dev_id, pmem->size);
    return BM_ERR_NOMEM;
  }
#ifdef SOC_MODE
  sg_mem_invalidate_device_mem(ctx, pmem);
#endif
#endif

  if(ctx->enable_mem_guard) {
    pmem->size -= BMLIB_MEM_GUARD_SIZE;
    guard_mem_set(ctx, pmem->u.device.device_addr, pmem->size);
  }

  return BM_SUCCESS;
}

static bm_status_t __alloc_bm_device_mem_raw_u64(bm_handle_t ctx,
                                          bm_device_mem_u64_t *pmem,
                                          int heap_id_mask) {
  if (ctx == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }
  if(ctx->enable_mem_guard) {
    pmem->size += BMLIB_MEM_GUARD_SIZE;
  }
#ifdef USING_CMODEL
  UNUSED(heap_id_mask);
  u64 addr;
  addr = ctx->bm_dev->bm_device_alloc_mem(pmem->size);
  pmem->u.device.device_addr = addr;
  if (addr == MEM_POOL_ADDR_INVALID) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "No memory in device mem\n");
    return BM_ERR_NOMEM;
  }
#else
  int ret = 0;

  ret = bm_alloc_gmem_u64(ctx, pmem, heap_id_mask);
  if (ret) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bm_alloc_gmem failed, dev_id = %d, size = 0x%x\n",
           ctx->dev_id, pmem->size);
    return BM_ERR_NOMEM;
  }
#ifdef SOC_MODE
  bm_mem_invalidate_device_mem_u64(ctx, pmem);
#endif
#endif

  if(ctx->enable_mem_guard) {
    pmem->size -= BMLIB_MEM_GUARD_SIZE;
    guard_mem_set(ctx, pmem->u.device.device_addr, pmem->size);
  }
  return BM_SUCCESS;

}

bm_status_t bm_malloc_neuron_device(bm_handle_t handle, bm_device_mem_t *pmem,
                                    int n, int c, int h, int w) {
  u32 size = 0;
  u64 size_tmp = 0ULL;
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  size_tmp = bm_get_neuron_size(n, c, h, w);
  if (size_tmp > 0xffffffff) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "%s, malloc device size >= 4G\n", __func__);
    return BM_NOT_SUPPORTED;
  }

  size = (u32)size_tmp;
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
  BM_CHECK_RET(__alloc_device_mem_raw(handle, pmem, any_heap_mask));

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, total_size %d, num = %d, addr = %lx\n", __func__,
         size, n, pmem->u.device.device_addr);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_malloc_neuron_device(bm_handle_t handle, sg_device_mem_t *pmem,
                                    u64 n, u64 c, u64 h, u64 w) {
  u32 size = 0;
  u64 size_tmp = 0ULL;
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  size_tmp = sg_get_neuron_size(n, c, h, w);

  size = (u32)size_tmp;
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
  BM_CHECK_RET(__alloc_sg_device_mem_raw(handle, pmem, any_heap_mask));

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, total_size %d, num = %d, addr = %lx\n", __func__,
         size, n, pmem->u.device.device_addr);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_malloc_neuron_device_u64(bm_handle_t handle, bm_device_mem_u64_t *pmem,
                                    u64 n, u64 c, u64 h, u64 w) {
  u32 size = 0;
  u64 size_tmp = 0ULL;
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  size_tmp = bm_get_neuron_size_u64(n, c, h, w);

  size = (u32)size_tmp;
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
  BM_CHECK_RET(__alloc_bm_device_mem_raw_u64(handle, pmem, any_heap_mask));

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, total_size %d, num = %d, addr = %lx\n", __func__,
         size, n, pmem->u.device.device_addr);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_dword(bm_handle_t handle, bm_device_mem_t *pmem,
                                   int count) {
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  u32 size = bm_get_coeff_size(count);
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
  BM_CHECK_RET(__alloc_device_mem_raw(handle, pmem, any_heap_mask));

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, size %d, addr = 0x%lx\n", __func__, size,
         pmem->u.device.device_addr);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_malloc_device_dword(bm_handle_t handle, sg_device_mem_t *pmem,
                                   u64 count) {
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  u64 size = sg_get_coeff_size(count);

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
  BM_CHECK_RET(__alloc_sg_device_mem_raw(handle, pmem, any_heap_mask));

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, size %d, addr = 0x%lx\n", __func__, size,
         pmem->u.device.device_addr);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_dword_u64(bm_handle_t handle, bm_device_mem_u64_t *pmem,
                                   u64 count) {
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  u64 size = bm_get_coeff_size_u64(count);

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
  BM_CHECK_RET(__alloc_bm_device_mem_raw_u64(handle, pmem, any_heap_mask));

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, size %d, addr = 0x%lx\n", __func__, size,
         pmem->u.device.device_addr);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_byte(bm_handle_t handle, bm_device_mem_t *pmem,
                                  unsigned int size) {
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;

  BM_CHECK_RET(__alloc_device_mem_raw(handle, pmem, any_heap_mask));
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_mem(bm_handle_t handle, unsigned long long *paddr,
                                  int heap_id, unsigned long long size) {
  int ret;
  bm_device_mem_u64_t *dev_buffer;
  struct bm_mem_paddr *bm_mem;

  bm_mem = (struct bm_mem_paddr *)malloc(sizeof(struct bm_mem_paddr));
  dev_buffer = (bm_device_mem_u64_t *)malloc(sizeof(bm_device_mem_u64_t));

  ret = bm_malloc_device_byte_heap_u64(handle, dev_buffer, heap_id, size);
  if (ret != BM_SUCCESS) {
    printf("malloc device memory size = %llu failed, ret = %d\n", size, ret);
    return BM_ERR_DEVNOTREADY;
  }

  *paddr = bm_mem_get_device_addr_u64(*dev_buffer);
  bm_mem->paddr = *paddr;
  bm_mem->dev_buffer = dev_buffer;

#ifdef __linux__
  pthread_mutex_lock(&handle->mem_mutex);
  ret = buffer_add(handle, bm_mem);
  pthread_mutex_unlock(&handle->mem_mutex);
#else
  DWORD dwWaitResult = WaitForSingleObject(&handle->mem_mutex, INFINITE);
  if (dwWaitResult == WAIT_OBJECT_0) {
    auto result = handle->root.insert((void *)bm_mem);
    if (result.second)
        ret = BM_SUCCESS;
    else
        ret = BM_ERR_FAILURE;
  }
  ReleaseMutex(&handle->mem_mutex);
#endif
  if (ret != BM_SUCCESS) {
    printf("malloc device memory size = %llu failed, ret = %d\n", size, ret);
    return BM_ERR_DEVNOTREADY;
  }

  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_mem_mask(bm_handle_t handle, unsigned long long *paddr,
                                  int heap_id_mask, unsigned long long size) {
  int ret;
  bm_device_mem_u64_t *dev_buffer;
  struct bm_mem_paddr *bm_mem;

  bm_mem = (struct bm_mem_paddr *)malloc(sizeof(struct bm_mem_paddr));
  dev_buffer = (bm_device_mem_u64_t *)malloc(sizeof(bm_device_mem_u64_t));

  ret = bm_malloc_device_byte_heap_mask_u64(handle, dev_buffer, heap_id_mask, size);
  if (ret != BM_SUCCESS) {
    printf("malloc device memory size = %llu failed, ret = %d\n", size, ret);
    return BM_ERR_DEVNOTREADY;
  }

  *paddr = bm_mem_get_device_addr_u64(*dev_buffer);
  bm_mem->paddr = *paddr;
  bm_mem->dev_buffer = dev_buffer;

#ifdef __linux__
  pthread_mutex_lock(&handle->mem_mutex);
  ret = buffer_add(handle, bm_mem);
  pthread_mutex_unlock(&handle->mem_mutex);
#else
  DWORD dwWaitResult = WaitForSingleObject(&handle->mem_mutex, INFINITE);
  if (dwWaitResult == WAIT_OBJECT_0) {
    auto result = handle->root.insert((void *)bm_mem);
    if (result.second)
        ret = BM_SUCCESS;
    else
        ret = BM_ERR_FAILURE;
  }
  ReleaseMutex(&handle->mem_mutex);
#endif
  if (ret != BM_SUCCESS) {
    printf("malloc device memory size = %llu failed, ret = %d\n", size, ret);
    return BM_ERR_DEVNOTREADY;
  }

  return BM_SUCCESS;
}

bm_status_t sg_malloc_device_byte(bm_handle_t handle, sg_device_mem_t *pmem,
                                  unsigned long long size) {
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;

  BM_CHECK_RET(__alloc_sg_device_mem_raw(handle, pmem, any_heap_mask));
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_byte_u64(bm_handle_t handle, bm_device_mem_u64_t *pmem,
                                  unsigned long long size) {
  int any_heap_mask = 0;
  any_heap_mask = (2 << (ION_MAX_HEAP_CNT - 1)) - 1;

  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;

  BM_CHECK_RET(__alloc_bm_device_mem_raw_u64(handle, pmem, any_heap_mask));
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_byte_heap(bm_handle_t handle, bm_device_mem_t *pmem,
          int heap_id, unsigned int size) {
  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;
  BM_CHECK_RET(__alloc_device_mem_raw(handle, pmem, 0x1 << heap_id));
  return BM_SUCCESS;
}

bm_status_t sg_malloc_device_byte_heap(bm_handle_t handle, sg_device_mem_t *pmem,
          int heap_id, unsigned long long size) {
  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;
  BM_CHECK_RET(__alloc_sg_device_mem_raw(handle, pmem, 0x1 << heap_id));
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_byte_heap_u64(bm_handle_t handle, bm_device_mem_u64_t *pmem,
          int heap_id, unsigned long long size) {
  if (handle == nullptr || pmem == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr %s: %s: %d\n", handle,
           pmem, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;
  BM_CHECK_RET(__alloc_bm_device_mem_raw_u64(handle, pmem, 0x1 << heap_id));
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_byte_heap_mask(bm_handle_t handle, bm_device_mem_t *pmem,
          int heap_id_mask, unsigned int size) {
  if (handle == nullptr || pmem == nullptr || heap_id_mask == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr, or heap_id_mask = 0x%x, %s: %s: %d\n", handle,
           pmem, heap_id_mask, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;
  BM_CHECK_RET(__alloc_device_mem_raw(handle, pmem, heap_id_mask));
  return BM_SUCCESS;
}

bm_status_t sg_malloc_device_byte_heap_mask(bm_handle_t handle, sg_device_mem_t *pmem,
          int heap_id_mask, unsigned long long size) {
  if (handle == nullptr || pmem == nullptr || heap_id_mask == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr, or heap_id_mask = 0x%x, %s: %s: %d\n", handle,
           pmem, heap_id_mask, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;
  BM_CHECK_RET(__alloc_sg_device_mem_raw(handle, pmem, heap_id_mask));
  return BM_SUCCESS;
}

bm_status_t bm_malloc_device_byte_heap_mask_u64(bm_handle_t handle, bm_device_mem_u64_t *pmem,
          int heap_id_mask, unsigned long long size) {
  if (handle == nullptr || pmem == nullptr || heap_id_mask == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle = 0x%p, or pmem = 0x%p is nullptr, or heap_id_mask = 0x%x, %s: %s: %d\n", handle,
           pmem, heap_id_mask, __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;

#ifndef USING_CMODEL
  if (handle->misc_info.chipid == 0x1682) {
    // keep 4byte aligned
    size = ((size + FLOAT_SIZE - 1) / FLOAT_SIZE) * FLOAT_SIZE;
  }
#endif
  pmem->size = size;
  BM_CHECK_RET(__alloc_bm_device_mem_raw_u64(handle, pmem, heap_id_mask));
  return BM_SUCCESS;
}

void bm_free_device(bm_handle_t ctx, bm_device_mem_t mem) {
  if (ctx == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return;
  }
  if (bm_mem_get_type(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);

  if(ctx->enable_mem_guard) {
    guard_mem_check(ctx, mem.u.device.device_addr, mem.size);
    mem.size += BMLIB_MEM_GUARD_SIZE;
  }

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, type %d, size %d, addr = 0x%llx\n", __func__,
         bm_mem_get_type(mem), bm_mem_get_size(mem),
         bm_mem_get_device_addr(mem));
#endif

#ifdef USING_CMODEL
  ctx->bm_dev->bm_device_free_mem(bm_mem_get_device_addr(mem));
#else
  bm_free_gmem(ctx, &mem);
#endif
}

void bm_free_device_mem(bm_handle_t ctx, unsigned long long paddr) {

  struct bm_mem_paddr *bm_mem = NULL;
  bm_device_mem_u64_t mem;

#ifdef __linux__
  pthread_mutex_lock(&ctx->mem_mutex);
  bm_mem = buffer_search(ctx, paddr);
  mem = *(bm_mem->dev_buffer);
  rb_erase(&bm_mem->node, &ctx->root);
  pthread_mutex_unlock(&ctx->mem_mutex);
#else
  DWORD dwWaitResult = WaitForSingleObject(&ctx->mem_mutex, INFINITE);
  if (dwWaitResult == WAIT_OBJECT_0) {
    for (auto it = ctx->root.begin(); it != ctx->root.end(); ) {
        if (paddr == ((bm_mem_paddr *)(*it))->paddr) {
            bm_mem = (bm_mem_paddr *)(*it);
            mem = *(bm_mem->dev_buffer);
            it = ctx->root.erase(it);
            break;
        } else {
            ++it;
        }
    }
  }
  ReleaseMutex(&ctx->mem_mutex);
#endif
  free(bm_mem->dev_buffer);
  free(bm_mem);
  bm_free_device_u64(ctx, mem);
}

void sg_free_device(bm_handle_t ctx, sg_device_mem_t mem) {
  if (ctx == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return;
  }
  if (sg_mem_get_type(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);

  if(ctx->enable_mem_guard) {
    guard_mem_check(ctx, mem.u.device.device_addr, mem.size);
    mem.size += BMLIB_MEM_GUARD_SIZE;
  }

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, type %d, size %d, addr = 0x%llx\n", __func__,
         sg_mem_get_type(mem), sg_mem_get_size(mem),
         sg_mem_get_device_addr(mem));
#endif

#ifdef USING_CMODEL
  ctx->bm_dev->bm_device_free_mem(sg_mem_get_device_addr(mem));
#else
  sg_free_gmem(ctx, &mem);
#endif
}

void bm_free_device_u64(bm_handle_t ctx, bm_device_mem_u64_t mem) {
  if (ctx == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return;
  }
  if (bm_mem_get_type_u64(mem) != BM_MEM_TYPE_DEVICE)
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_WARNING,
          "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);

  if(ctx->enable_mem_guard) {
    guard_mem_check(ctx, mem.u.device.device_addr, mem.size);
    mem.size += BMLIB_MEM_GUARD_SIZE;
  }

#ifdef MM_DEBUG
  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
         "%s, type %d, size %d, addr = 0x%llx\n", __func__,
         bm_mem_get_type_u64(mem), bm_mem_get_size_u64(mem),
         bm_mem_get_device_addr_u64(mem));
#endif

#ifdef USING_CMODEL
  ctx->bm_dev->bm_device_free_mem(bm_mem_get_device_addr_u64(mem));
#else
  bm_free_gmem_u64(ctx, &mem);
#endif
}

void bm_set_device_mem(bm_device_mem_t *pmem, unsigned int size, u64 addr) {
  pmem->u.device.device_addr = addr;
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
}

void sg_set_device_mem(sg_device_mem_t *pmem, unsigned long long size, u64 addr) {
  pmem->u.device.device_addr = addr;
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
}

void bm_set_device_mem_u64(bm_device_mem_u64_t *pmem, unsigned long long size, u64 addr) {
  pmem->u.device.device_addr = addr;
  pmem->flags.u.mem_type = BM_MEM_TYPE_DEVICE;
  pmem->size = size;
}

static bool bm_device_mem_page_aligned(bm_device_mem_t mem) {
  u64 device_mem_addr = bm_mem_get_device_addr(mem);
  if ((device_mem_addr & (PAGE_SIZE - 1)) == 0) {
    return true;
  } else {
    return false;
  }
}

static bool sg_device_mem_page_aligned(sg_device_mem_t mem) {
  u64 device_mem_addr = sg_mem_get_device_addr(mem);
  if ((device_mem_addr & (PAGE_SIZE - 1)) == 0) {
    return true;
  } else {
    return false;
  }
}

static bool bm_device_mem_page_aligned_u64(bm_device_mem_u64_t mem) {
  u64 device_mem_addr = bm_mem_get_device_addr_u64(mem);
  if ((device_mem_addr & (PAGE_SIZE - 1)) == 0) {
    return true;
  } else {
    return false;
  }
}

static bool bm_device_mem_range_valid(bm_handle_t handle, bm_device_mem_t mem) {
#ifdef USING_CMODEL
  UNUSED(handle);
  UNUSED(mem);
#else
  u64 saddr = bm_mem_get_device_addr(mem);
  u64 eaddr = bm_mem_get_size(mem) + saddr;

  if (handle->misc_info.chipid == 0x1684 || handle->misc_info.chipid == 0x1686) {
    if (((saddr >= 0x100000000 && saddr <= 0x4ffffffff) || (saddr >= 0x0 && saddr <= 0x103fffff))
        && ((eaddr >= 0x100000000 && eaddr <= 0x500000000) || (eaddr >= 0x0 && eaddr <= 0x10400000))) {
      return true;
    } else {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
      "%s saddr=0x%llx eaddr=0x%llx out of range\n", __func__, saddr, eaddr);
      return false;
    }
  }

  if (handle->misc_info.chipid == 0x1682) {
    if (saddr >= 0x100000000 && saddr <= 0x2ffffffff
        && eaddr >= 0x100000000 && eaddr <= 0x300000000) {
      return true;
    } else {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
      "%s saddr=0x%llx eaddr=0x%llx out of range\n", __func__, saddr, eaddr);
      return false;
    }
  }
#endif
  return true;
}

static bool sg_device_mem_range_valid(bm_handle_t handle, sg_device_mem_t mem) {
#ifdef USING_CMODEL
  UNUSED(handle);
  UNUSED(mem);
#else
  u64 saddr = sg_mem_get_device_addr(mem);
  u64 eaddr = sg_mem_get_size(mem) + saddr;

  if (handle->misc_info.chipid == 0x1684 || handle->misc_info.chipid == 0x1686) {
    if (((saddr >= 0x100000000 && saddr <= 0x4ffffffff) || (saddr >= 0x0 && saddr <= 0x103fffff))
        && ((eaddr >= 0x100000000 && eaddr <= 0x500000000) || (eaddr >= 0x0 && eaddr <= 0x10400000))) {
      return true;
    } else {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
      "%s saddr=0x%llx eaddr=0x%llx out of range\n", __func__, saddr, eaddr);
      return false;
    }
  }

  if (handle->misc_info.chipid == 0x1682) {
    if (saddr >= 0x100000000 && saddr <= 0x2ffffffff
        && eaddr >= 0x100000000 && eaddr <= 0x300000000) {
      return true;
    } else {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
      "%s saddr=0x%llx eaddr=0x%llx out of range\n", __func__, saddr, eaddr);
      return false;
    }
  }
#endif
  return true;
}

static bool bm_device_mem_range_valid_u64(bm_handle_t handle, bm_device_mem_u64_t mem) {
#ifdef USING_CMODEL
  UNUSED(handle);
  UNUSED(mem);
#else
  u64 saddr = bm_mem_get_device_addr_u64(mem);
  u64 eaddr = bm_mem_get_size_u64(mem) + saddr;

  if (handle->misc_info.chipid == 0x1684 || handle->misc_info.chipid == 0x1686) {
    if (((saddr >= 0x100000000 && saddr <= 0x4ffffffff) || (saddr >= 0x0 && saddr <= 0x103fffff))
        && ((eaddr >= 0x100000000 && eaddr <= 0x500000000) || (eaddr >= 0x0 && eaddr <= 0x10400000))) {
      return true;
    } else {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
      "%s saddr=0x%llx eaddr=0x%llx out of range\n", __func__, saddr, eaddr);
      return false;
    }
  }

  if (handle->misc_info.chipid == 0x1682) {
    if (saddr >= 0x100000000 && saddr <= 0x2ffffffff
        && eaddr >= 0x100000000 && eaddr <= 0x300000000) {
      return true;
    } else {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
      "%s saddr=0x%llx eaddr=0x%llx out of range\n", __func__, saddr, eaddr);
      return false;
    }
  }
#endif
  return true;
}

bm_status_t bm_get_gmem_heap_id(bm_handle_t handle, bm_device_mem_t *pmem, unsigned int *heapid) {
  unsigned int val = 0;

  if (!handle || !pmem || !heapid) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  if (bm_mem_get_type(*pmem) != BM_MEM_TYPE_DEVICE) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }

  if (!bm_device_mem_range_valid(handle, *pmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem range is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

#ifndef USING_CMODEL
  val = pmem->flags.u.gmem_heapid;
  if (val > ION_MAX_HEAP_CNT) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "heap id is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }
#endif

  *heapid = val;
  return BM_SUCCESS;
}

bm_status_t sg_get_gmem_heap_id(bm_handle_t handle, sg_device_mem_t *pmem, unsigned int *heapid) {
  unsigned int val = 0;

  if (!handle || !pmem || !heapid) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  if (sg_mem_get_type(*pmem) != BM_MEM_TYPE_DEVICE) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }

  if (!sg_device_mem_range_valid(handle, *pmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem range is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

#ifndef USING_CMODEL
  val = pmem->flags.u.gmem_heapid;
  if (val > ION_MAX_HEAP_CNT) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "heap id is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }
#endif

  *heapid = val;
  return BM_SUCCESS;
}

bm_status_t bm_get_gmem_heap_id_u64(bm_handle_t handle, bm_device_mem_u64_t *pmem, unsigned int *heapid) {
  unsigned int val = 0;

  if (!handle || !pmem || !heapid) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  if (bm_mem_get_type_u64(*pmem) != BM_MEM_TYPE_DEVICE) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem type is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }

  if (!bm_device_mem_range_valid_u64(handle, *pmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem range is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

#ifndef USING_CMODEL
  val = pmem->flags.u.gmem_heapid;
  if (val > ION_MAX_HEAP_CNT) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "heap id is illegal %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_FAILURE;
  }
#endif

  *heapid = val;
  return BM_SUCCESS;
}

bm_status_t bm_get_gmem_total_heap_num(bm_handle_t handle, unsigned int *heap_num) {
#ifdef USING_CMODEL
  UNUSED(handle);
  UNUSED(heap_num);
#else
  if (!handle || !heap_num) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
 #ifdef __linux__
  if (handle->ion_fd)
    *heap_num = handle->heap_cnt;
  else
 #endif
  {
    if (0 != platform_ioctl(handle, BMDEV_GET_HEAP_NUM, heap_num))
      return BM_ERR_FAILURE;
  }

#endif
  return BM_SUCCESS;
}

bm_status_t bm_get_gmem_heap_stat_byte_by_id(bm_handle_t handle, bm_heap_stat_byte_t *pheap_byte, unsigned int heap_id) {
#ifdef USING_CMODEL
  UNUSED(handle);
  UNUSED(heap_id);
  UNUSED(pheap_byte);
#else
  if (!handle || !pheap_byte) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
 #ifdef __linux__
  struct ion_custom_data ion_data;
  struct bitmain_heap_info bm_heap_info;

  if (handle->ion_fd) {
    ion_data.cmd = ION_IOC_BITMAIN_GET_HEAP_INFO;
    ion_data.arg = (unsigned long)&bm_heap_info;
    bm_heap_info.id = heap_id;
    if (0 != ioctl(handle->ion_fd, ION_IOC_CUSTOM, &ion_data))
      return BM_ERR_FAILURE;
    pheap_byte->mem_total = bm_heap_info.total_size;
    pheap_byte->mem_avail = bm_heap_info.avail_size;
    pheap_byte->mem_used = bm_heap_info.total_size - bm_heap_info.avail_size;
    pheap_byte->mem_start_addr = 0;
  } else
 #endif
  {
    pheap_byte->heap_id = heap_id;
    if (0 != platform_ioctl(handle, BMDEV_GET_HEAP_STAT_BYTE, pheap_byte))
      return BM_ERR_FAILURE;
  }
#endif
  return BM_SUCCESS;
}
/*
use this function to map device memory to user space
we will map the page aligned size which should be aligned
with gmem alloc rules
*/
bm_status_t bm_mem_mmap_device_mem(bm_handle_t handle, bm_device_mem_t *dmem,
                                   u64 *vmem) {
#ifndef USING_CMODEL
  void *ret = 0;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!bm_device_mem_page_aligned(*dmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "bm_mem_mmap_device_mem device_mem_addr = 0x%llx is illegal\n",
           bm_mem_get_device_addr(*dmem));
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  unsigned int size = bm_mem_get_device_size(*dmem);
  unsigned int aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, bm_mem_get_device_addr(*dmem));
  if (MAP_FAILED != ret) {
    *vmem = (u64)ret;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }
 #endif
#else
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    bm_mem_get_device_addr(*dmem) - handle->bm_dev->cmodel_get_gmem_start_addr_());
#endif
return BM_SUCCESS;
}

bm_status_t sg_mem_mmap_device_mem(bm_handle_t handle, sg_device_mem_t *dmem,
                                   u64 *vmem) {
#ifndef USING_CMODEL
  void *ret = 0;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!sg_device_mem_page_aligned(*dmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "bm_mem_mmap_device_mem device_mem_addr = 0x%llx is illegal\n",
           sg_mem_get_device_addr(*dmem));
    return BM_ERR_PARAM;
  }

  if (!sg_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  unsigned long long size = sg_mem_get_device_size(*dmem);
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, sg_mem_get_device_addr(*dmem));
  if (MAP_FAILED != ret) {
    *vmem = (u64)ret;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }
 #endif
#else
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    sg_mem_get_device_addr(*dmem) - handle->bm_dev->cmodel_get_gmem_start_addr_());
#endif
return BM_SUCCESS;
}

bm_status_t bm_mem_mmap_device_mem_u64(bm_handle_t handle, bm_device_mem_u64_t *dmem,
                                   u64 *vmem) {
#ifndef USING_CMODEL
  void *ret = 0;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!bm_device_mem_page_aligned_u64(*dmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "bm_mem_mmap_device_mem device_mem_addr = 0x%llx is illegal\n",
           bm_mem_get_device_addr_u64(*dmem));
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid_u64(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  unsigned long long size = bm_mem_get_device_size_u64(*dmem);
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, bm_mem_get_device_addr_u64(*dmem));
  if (MAP_FAILED != ret) {
    *vmem = (u64)ret;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }
 #endif
#else
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    bm_mem_get_device_addr_u64(*dmem) - handle->bm_dev->cmodel_get_gmem_start_addr_());
#endif
return BM_SUCCESS;
}

bm_status_t bm_mem_mmap_device_mem_no_cache(bm_handle_t handle,
                                   bm_device_mem_t *dmem,
                                   u64 *vmem) {

#ifndef USING_CMODEL
  void *ret = 0;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!bm_device_mem_page_aligned(*dmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "bm_mem_mmap_device_mem device_mem_addr = 0x%llx is illegal\n",
           bm_mem_get_device_addr(*dmem));
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  unsigned int size = bm_mem_get_device_size(*dmem);
  unsigned int aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

  /*0x1000000000 is used to set the flag
  in driver bmdev_mmap function to open the mmap with no cache*/
  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, bm_mem_get_device_addr(*dmem) | 0x1000000000);

  if (MAP_FAILED != ret) {
    *vmem = (u64)ret;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }

 #endif
#else
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    bm_mem_get_device_addr(*dmem) - handle->bm_dev->cmodel_get_gmem_start_addr_());
#endif
return BM_SUCCESS;
}

bm_status_t sg_mem_mmap_device_mem_no_cache(bm_handle_t handle,
                                   sg_device_mem_t *dmem,
                                   u64 *vmem) {

#ifndef USING_CMODEL
  void *ret = 0;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!sg_device_mem_page_aligned(*dmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "bm_mem_mmap_device_mem device_mem_addr = 0x%llx is illegal\n",
           sg_mem_get_device_addr(*dmem));
    return BM_ERR_PARAM;
  }

  if (!sg_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  unsigned long long size = sg_mem_get_device_size(*dmem);
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

  /*0x1000000000 is used to set the flag
  in driver bmdev_mmap function to open the mmap with no cache*/
  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, sg_mem_get_device_addr(*dmem) | 0x1000000000);

  if (MAP_FAILED != ret) {
    *vmem = (u64)ret;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }

 #endif
#else
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    sg_mem_get_device_addr(*dmem) - handle->bm_dev->cmodel_get_gmem_start_addr_());
#endif
return BM_SUCCESS;
}

bm_status_t bm_mem_mmap_device_mem_no_cache_u64(bm_handle_t handle,
                                   bm_device_mem_u64_t *dmem,
                                   u64 *vmem) {

#ifndef USING_CMODEL
  void *ret = 0;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!bm_device_mem_page_aligned_u64(*dmem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "bm_mem_mmap_device_mem device_mem_addr = 0x%llx is illegal\n",
           bm_mem_get_device_addr_u64(*dmem));
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid_u64(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  unsigned long long size = bm_mem_get_device_size_u64(*dmem);
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));

  /*0x1000000000 is used to set the flag
  in driver bmdev_mmap function to open the mmap with no cache*/
  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, bm_mem_get_device_addr_u64(*dmem) | 0x1000000000);

  if (MAP_FAILED != ret) {
    *vmem = (u64)ret;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }

 #endif
#else
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    bm_mem_get_device_addr_u64(*dmem) - handle->bm_dev->cmodel_get_gmem_start_addr_());
#endif
return BM_SUCCESS;
}

/*
  use his funtion to make cache of part of the device memory invalid
*/
bm_status_t bm_mem_invalidate_partial_device_mem(bm_handle_t handle,
                                                 bm_device_mem_t *dmem,
                                                 u32 offset, u32 len) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support invalidate parital mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (!bm_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  u64 device_mem_addr = bm_mem_get_device_addr(*dmem);
  u64 para = (((device_mem_addr + offset)>>6) << 32) + len +
    ((device_mem_addr + offset)&63);
  bm_profile_record_mem_begin(handle);
  if (0 != platform_ioctl(handle, BMDEV_INVALIDATE_GMEM, &para)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(handle, bm_mem_op_type_t::INVALIDATE, device_mem_addr+offset, len);
#else
  UNUSED(handle);
  UNUSED(dmem);
  UNUSED(offset);
  UNUSED(len);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_mem_invalidate_partial_device_mem(bm_handle_t handle,
                                                 sg_device_mem_t *dmem,
                                                 u64 offset, u64 len) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support invalidate parital mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (!sg_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  u64 device_mem_addr = sg_mem_get_device_addr(*dmem);
  u64 para = (((device_mem_addr + offset)>>6) << 32) + len +
    ((device_mem_addr + offset)&63);
  bm_profile_record_mem_begin(handle);
  if (0 != platform_ioctl(handle, BMDEV_INVALIDATE_GMEM, &para)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(handle, bm_mem_op_type_t::INVALIDATE, device_mem_addr+offset, len);
#else
  UNUSED(handle);
  UNUSED(dmem);
  UNUSED(offset);
  UNUSED(len);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_invalidate_partial_device_mem_u64(bm_handle_t handle,
                                                 bm_device_mem_u64_t *dmem,
                                                 u64 offset, u64 len) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support invalidate parital mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (!bm_device_mem_range_valid_u64(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  u64 device_mem_addr = bm_mem_get_device_addr_u64(*dmem);
  u64 para = (((device_mem_addr + offset)>>6) << 32) + len +
    ((device_mem_addr + offset)&63);
  bm_profile_record_mem_begin(handle);
  if (0 != platform_ioctl(handle, BMDEV_INVALIDATE_GMEM, &para)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(handle, bm_mem_op_type_t::INVALIDATE, device_mem_addr+offset, len);
#else
  UNUSED(handle);
  UNUSED(dmem);
  UNUSED(offset);
  UNUSED(len);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_vir_to_phy(bm_handle_t handle, unsigned long long vir_addr, unsigned long long *phy_addr) {
#ifndef USING_CMODEL
  struct bm_gmem_addr addr;
  addr.vir_addr = vir_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support bm_mem_vir_to_phy in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (0 != platform_ioctl(handle, BMDEV_GMEM_ADDR, &addr)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bm_mem_vir_to_phy fail vir add = 0x%llx\n", vir_addr);
    return BM_ERR_PARAM;

  }

  *phy_addr = addr.phy_addr;
#else
  UNUSED(handle);
  UNUSED(vir_addr);
  UNUSED(phy_addr);
#endif
  return BM_SUCCESS;
}
/*
  use his funtion to make cache of the device memory invalid
  the real length is enough for invalidate
*/
bm_status_t bm_mem_invalidate_device_mem(bm_handle_t handle,
                                         bm_device_mem_t *dmem) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support invalidate mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#endif
  return bm_mem_invalidate_partial_device_mem(handle, dmem, 0,
                                              bm_mem_get_device_size(*dmem));
}

bm_status_t sg_mem_invalidate_device_mem(bm_handle_t handle,
                                         sg_device_mem_t *dmem) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support invalidate mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#endif
  return sg_mem_invalidate_partial_device_mem(handle, dmem, 0,
                                              sg_mem_get_device_size(*dmem));
}

bm_status_t bm_mem_invalidate_device_mem_u64(bm_handle_t handle,
                                         bm_device_mem_u64_t *dmem) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support invalidate mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#endif
  return bm_mem_invalidate_partial_device_mem_u64(handle, dmem, 0,
                                              bm_mem_get_device_size_u64(*dmem));
}

/*
  use his funtion to flush part of device mem data to real memory
  currently, the speed of mmecpy_s2d is not so slow, this function may not be
  used the real length is enough for invalidate
*/
bm_status_t bm_mem_flush_partial_device_mem(bm_handle_t handle,
                                            bm_device_mem_t *dmem,
                                            u32 offset, u32 len) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support flush parital mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (!bm_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  u64 device_mem_addr = bm_mem_get_device_addr(*dmem);
  u64 para = (((device_mem_addr + (u64)offset)>>6) << 32) + len +
    ((device_mem_addr + offset)&63);
  bm_profile_record_mem_begin(handle);
  if (0 != platform_ioctl(handle, BMDEV_FLUSH_GMEM, &para)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(handle, bm_mem_op_type_t::FLUSH, device_mem_addr+offset, len);
#else
  UNUSED(handle);
  UNUSED(dmem);
  UNUSED(offset);
  UNUSED(len);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_mem_flush_partial_device_mem(bm_handle_t handle,
                                            sg_device_mem_t *dmem,
                                            u64 offset, u64 len) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support flush parital mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (!sg_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  u64 device_mem_addr = sg_mem_get_device_addr(*dmem);
  u64 para = (((device_mem_addr + (u64)offset)>>6) << 32) + len +
    ((device_mem_addr + offset)&63);
  bm_profile_record_mem_begin(handle);
  if (0 != platform_ioctl(handle, BMDEV_FLUSH_GMEM, &para)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(handle, bm_mem_op_type_t::FLUSH, device_mem_addr+offset, len);
#else
  UNUSED(handle);
  UNUSED(dmem);
  UNUSED(offset);
  UNUSED(len);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_flush_partial_device_mem_u64(bm_handle_t handle,
                                            bm_device_mem_u64_t *dmem,
                                            u64 offset, u64 len) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support flush parital mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  if (!bm_device_mem_range_valid_u64(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  u64 device_mem_addr = bm_mem_get_device_addr_u64(*dmem);
  u64 para = (((device_mem_addr + (u64)offset)>>6) << 32) + len +
    ((device_mem_addr + offset)&63);
  bm_profile_record_mem_begin(handle);
  if (0 != platform_ioctl(handle, BMDEV_FLUSH_GMEM, &para)) return BM_ERR_FAILURE;
  bm_profile_record_mem_end(handle, bm_mem_op_type_t::FLUSH, device_mem_addr+offset, len);
#else
  UNUSED(handle);
  UNUSED(dmem);
  UNUSED(offset);
  UNUSED(len);
#endif
  return BM_SUCCESS;
}

/*
  use his funtion to flush data to real memory
  currently, the speed of mmecpy_s2d is not so slow, this function may not be
  used the real length is enough for invalidate
*/
bm_status_t bm_mem_flush_device_mem(bm_handle_t handle, bm_device_mem_t *dmem) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support flush mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#endif
  return bm_mem_flush_partial_device_mem(handle, dmem, 0,
                                         bm_mem_get_device_size(*dmem));
}

bm_status_t sg_mem_flush_device_mem(bm_handle_t handle, sg_device_mem_t *dmem) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support flush mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#endif
  return sg_mem_flush_partial_device_mem(handle, dmem, 0,
                                         sg_mem_get_device_size(*dmem));
}

bm_status_t bm_mem_flush_device_mem_u64(bm_handle_t handle, bm_device_mem_u64_t *dmem) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support flush mem in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#endif
  return bm_mem_flush_partial_device_mem_u64(handle, dmem, 0,
                                         bm_mem_get_device_size_u64(*dmem));
}

/*
use this function to unmap device memory in user space
we will unmap the page aligned size
*/
bm_status_t bm_mem_unmap_device_mem(bm_handle_t handle, void *vmem, int size) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support unmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  unsigned int aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  (void)munmap(vmem, aligned_size);
#endif
#else
  UNUSED(handle);
  UNUSED(vmem);
  UNUSED(size);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_mem_unmap_device_mem(bm_handle_t handle, void *vmem, u64 size) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support unmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  (void)munmap(vmem, aligned_size);
#endif
#else
  UNUSED(handle);
  UNUSED(vmem);
  UNUSED(size);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_unmap_device_mem_u64(bm_handle_t handle, void *vmem, u64 size) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support unmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  (void)munmap(vmem, aligned_size);
#endif
#else
  UNUSED(handle);
  UNUSED(vmem);
  UNUSED(size);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_s2d_normal(bm_handle_t handle, bm_device_mem_t dst, void *src) {
#ifdef USING_CMODEL
  return handle->bm_dev->bm_device_memcpy_s2d(dst, src);
#else
  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid(handle, dst)) {
    return BM_ERR_PARAM;
  }

  bm_memcpy_info_t bm_mem_s2d;
#ifdef __linux__
    #ifdef USING_INT_CDMA
      bm_mem_s2d.intr = true;
    #else
      bm_mem_s2d.intr = false;
    #endif
    bm_mem_s2d.host_addr = src;
#else
  bm_mem_s2d.intr      = 1;
  bm_mem_s2d.host_addr = (u64)src;
#endif
  bm_mem_s2d.device_addr = bm_mem_get_device_addr(dst);
  bm_mem_s2d.size = bm_mem_get_size(dst);
  bm_mem_s2d.dir = HOST2CHIP;
  bm_mem_s2d.src_device_addr = 0;
  bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

  union { void* ptr; u64 val; } ptr_to_u64;
  ptr_to_u64.ptr = src;
  bm_profile_record_memcpy_begin(handle);
  auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
  bm_profile_record_memcpy_end(handle, ptr_to_u64.val, bm_mem_s2d.device_addr, bm_mem_s2d.size, bm_mem_s2d.dir);
  return (0 != res)? BM_ERR_FAILURE: BM_SUCCESS;
#endif
}

bm_status_t bm_mem_mmap_device_mem_mix(bm_handle_t handle, bm_device_mem_t *dmem,
                                   u64 *vmem) {
#ifndef USING_CMODEL
  void *ret = 0;
  u64 addr, size, addr_end;
  u64 aligned_size, aligned_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!bm_device_mem_range_valid(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  size = bm_mem_get_device_size(*dmem);
  addr = bm_mem_get_device_addr(*dmem);
  addr_end = addr + size;

  aligned_addr = bm_mem_get_device_addr(*dmem) & (~(PAGE_SIZE - 1));
  aligned_size = ((addr_end + (PAGE_SIZE - 1)) & (~(PAGE_SIZE - 1))) - aligned_addr;

  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, aligned_addr);

  if (MAP_FAILED != ret) {
    //ret = (((u64)ret) + (addr - aligned_addr))
    *vmem = (u64)ret + addr - aligned_addr;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }
#endif
#else
   #define GLOBAL_MEM_START_ADDR 0x100000000
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    bm_mem_get_device_addr(*dmem) - GLOBAL_MEM_START_ADDR);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_mmap_device_mem_mix_u64(bm_handle_t handle, bm_device_mem_u64_t *dmem,
                                   u64 *vmem) {
#ifndef USING_CMODEL
  void *ret = 0;
  u64 addr, size, addr_end;
  u64 aligned_size, aligned_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support mmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  if (!bm_device_mem_range_valid_u64(handle, *dmem)) {
    return BM_ERR_PARAM;
  }

  size = bm_mem_get_device_size_u64(*dmem);
  addr = bm_mem_get_device_addr_u64(*dmem);
  addr_end = addr + size;

  aligned_addr = bm_mem_get_device_addr_u64(*dmem) & (~(PAGE_SIZE - 1));
  aligned_size = ((addr_end + (PAGE_SIZE - 1)) & (~(PAGE_SIZE - 1))) - aligned_addr;

  ret = mmap(0, aligned_size, PROT_READ | PROT_WRITE, MAP_SHARED,
             handle->dev_fd, aligned_addr);

  if (MAP_FAILED != ret) {
    //ret = (((u64)ret) + (addr - aligned_addr))
    *vmem = (u64)ret + addr - aligned_addr;
    return BM_SUCCESS;
  } else {
    return BM_ERR_FAILURE;
  }
  #endif
#else
   #define GLOBAL_MEM_START_ADDR 0x100000000
   //handle->bm_dev->get_global_memaddr_(handle->dev_id);
  *vmem = (u64)((u8*)handle->bm_dev->get_global_memaddr_(handle->dev_id) +
    bm_mem_get_device_addr_u64(*dmem) - GLOBAL_MEM_START_ADDR);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_unmap_device_mem_mix(bm_handle_t handle, void *vmem, int size) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support unmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  (void)munmap(vmem, aligned_size);
#endif
#else
  UNUSED(handle);
  UNUSED(vmem);
  UNUSED(size);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_mem_unmap_device_mem_mix_u64(bm_handle_t handle, void *vmem, u64 size) {
#ifndef USING_CMODEL
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bmlib not support unmap in pcie mode\n");
    return BM_ERR_FAILURE;
  }
#ifdef __linux__
  unsigned long long aligned_size = (size + PAGE_SIZE - 1) & (~(PAGE_SIZE - 1));
  (void)munmap(vmem, aligned_size);
#endif
#else
  UNUSED(handle);
  UNUSED(vmem);
  UNUSED(size);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_s2d_mix(bm_handle_t handle, bm_device_mem_t dst, void *src) {
#ifndef USING_CMODEL
  u64 dst_vaddr = 0;
  bm_status_t ret;
  u64 addr, size, addr_end;
  u64 aligned_size, aligned_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support s2d fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }
  ret = bm_mem_mmap_device_mem_mix(handle, &dst, &dst_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in s2d fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy((void *)dst_vaddr, src, bm_mem_get_device_size(dst));

  ret = bm_mem_flush_device_mem(handle, &dst);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in s2d fast failed\n");
    return BM_ERR_FAILURE;
  }

  size = bm_mem_get_device_size(dst);
  addr = bm_mem_get_device_addr(dst);
  addr_end = addr + size;

  aligned_addr = addr & (~(PAGE_SIZE - 1));
  aligned_size = ((addr_end + (PAGE_SIZE - 1)) & (~(PAGE_SIZE - 1))) - aligned_addr;

  dst_vaddr = dst_vaddr - (addr - aligned_addr);
  bm_mem_unmap_device_mem_mix(handle, (void *)dst_vaddr, aligned_size);
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_s2d_mix_u64(bm_handle_t handle, bm_device_mem_u64_t dst, void *src) {
#ifndef USING_CMODEL
  u64 dst_vaddr = 0;
  bm_status_t ret;
  u64 addr, size, addr_end;
  u64 aligned_size, aligned_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support s2d fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }

  ret = bm_mem_mmap_device_mem_mix_u64(handle, &dst, &dst_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in s2d fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy((void *)dst_vaddr, src, bm_mem_get_device_size_u64(dst));

  ret = bm_mem_flush_device_mem_u64(handle, &dst);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in s2d fast failed\n");
    return BM_ERR_FAILURE;
  }

  size = bm_mem_get_device_size_u64(dst);
  addr = bm_mem_get_device_addr_u64(dst);
  addr_end = addr + size;

  aligned_addr = addr & (~(PAGE_SIZE - 1));
  aligned_size = ((addr_end + (PAGE_SIZE - 1)) & (~(PAGE_SIZE - 1))) - aligned_addr;

  dst_vaddr = dst_vaddr - (addr - aligned_addr);
  bm_mem_unmap_device_mem_mix_u64(handle, (void *)dst_vaddr, aligned_size);
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_d2s_mix(bm_handle_t handle, void *dst, bm_device_mem_t src) {
#ifndef USING_CMODEL
  u64 src_vaddr = 0;
  bm_status_t ret;
  u64 addr, size, addr_end;
  u64 aligned_size, aligned_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support d2s fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }
  ret = bm_mem_mmap_device_mem_mix(handle, &src, &src_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  ret = bm_mem_invalidate_device_mem(handle, &src);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy(dst, (void *)src_vaddr, bm_mem_get_device_size(src));

  size = bm_mem_get_device_size(src);
  addr = bm_mem_get_device_addr(src);
  addr_end = addr + size;

  aligned_addr = addr & (~(PAGE_SIZE - 1));
  aligned_size = ((addr_end + (PAGE_SIZE - 1)) & (~(PAGE_SIZE - 1))) - aligned_addr;

  src_vaddr = src_vaddr - (addr - aligned_addr);

  bm_mem_unmap_device_mem_mix(handle, (void *)src_vaddr, aligned_size);
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_d2s_mix_u64(bm_handle_t handle, void *dst, bm_device_mem_u64_t src) {
#ifndef USING_CMODEL
  u64 src_vaddr = 0;
  bm_status_t ret;
  u64 addr, size, addr_end;
  u64 aligned_size, aligned_addr;

  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support d2s fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }
  ret = bm_mem_mmap_device_mem_mix_u64(handle, &src, &src_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  ret = bm_mem_invalidate_device_mem_u64(handle, &src);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy(dst, (void *)src_vaddr, bm_mem_get_device_size_u64(src));

  size = bm_mem_get_device_size_u64(src);
  addr = bm_mem_get_device_addr_u64(src);
  addr_end = addr + size;

  aligned_addr = addr & (~(PAGE_SIZE - 1));
  aligned_size = ((addr_end + (PAGE_SIZE - 1)) & (~(PAGE_SIZE - 1))) - aligned_addr;

  src_vaddr = src_vaddr - (addr - aligned_addr);

  bm_mem_unmap_device_mem_mix_u64(handle, (void *)src_vaddr, aligned_size);
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_s2d(bm_handle_t handle, bm_device_mem_t dst, void *src) {
#ifdef USING_CMODEL
  return handle->bm_dev->bm_device_memcpy_s2d(dst, src);
#else
  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid(handle, dst)) {
    return BM_ERR_PARAM;
  }

  if (handle->misc_info.pcie_soc_mode == 0 ||
      handle->misc_info.pcie_soc_mode == 1) {
    return bm_memcpy_s2d_normal(handle, dst, src);
  } else {
    return bm_memcpy_s2d_mix(handle, dst, src);
  }
#endif
}

bm_status_t bm_memcpy_p2p(bm_handle_t handle_src, bm_device_mem_t src, bm_handle_t handle_dst,bm_device_mem_t dst) {
#ifdef USING_CMODEL
  return BM_SUCCESS;
#else
  int src_num, dst_num;
  bm_memcpy_p2p_info_t bm_mem_p2p;

  if (handle_src == nullptr || handle_dst == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid(handle_src, src)) {
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle_dst, dst)) {
    return BM_ERR_PARAM;
  }
  src_num = bm_get_devid(handle_src);
  dst_num = bm_get_devid(handle_dst);

  if (src_num == dst_num) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "src chip and dst chip is the same\n");
    return BM_ERR_FAILURE;
  }


  bm_mem_p2p.intr = false;
  bm_mem_p2p.src_device_addr = bm_mem_get_device_addr(src);
  bm_mem_p2p.dst_device_addr = bm_mem_get_device_addr(dst);
  bm_mem_p2p.dst_num = dst_num;
  bm_mem_p2p.size = bm_mem_get_size(dst);
  bm_mem_p2p.cdma_iommu_mode = handle_src->cdma_iommu_mode;

  auto res = platform_ioctl(handle_src, BMDEV_MEMCPY_P2P, &bm_mem_p2p);

  return (0 != res)? BM_ERR_FAILURE: BM_SUCCESS;
#endif
}

bm_status_t sg_memcpy_s2d(bm_handle_t handle, sg_device_mem_t dst, void *src) {
#ifdef USING_CMODEL
  return handle->bm_dev->sg_device_memcpy_s2d(dst, src);
#else
  u64 size;
  int trans_size = 0x10000000;//256MB
  int tran_over = 0;

  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!sg_device_mem_range_valid(handle, dst)) {
    return BM_ERR_PARAM;
  }

  size = sg_mem_get_size(dst);

  for(int i=0; tran_over == 0; i++) {
    bm_memcpy_info_t bm_mem_s2d;
#ifdef __linux__
      #ifdef USING_INT_CDMA
        bm_mem_s2d.intr = true;
      #else
        bm_mem_s2d.intr = false;
      #endif
      bm_mem_s2d.host_addr = (void *)((u64)src + i * trans_size);
#else
    bm_mem_s2d.intr      = 1;
    bm_mem_s2d.host_addr = (u64)src + i * trans_size;
#endif
    bm_mem_s2d.device_addr = sg_mem_get_device_addr(dst) + i * trans_size;
    if(size > trans_size) {
      bm_mem_s2d.size = trans_size;
      size -= trans_size;
    } else {
      bm_mem_s2d.size = size;
      tran_over = 1;
    }

    bm_mem_s2d.dir = HOST2CHIP;
    bm_mem_s2d.src_device_addr = 0;
    bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

    union { void* ptr; u64 val; } ptr_to_u64;
    ptr_to_u64.ptr = (void *)((u64)src + i * trans_size);
    bm_profile_record_memcpy_begin(handle);
    auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
    bm_profile_record_memcpy_end(handle, ptr_to_u64.val, bm_mem_s2d.device_addr, bm_mem_s2d.size, bm_mem_s2d.dir);
    if (0 != res)
      return BM_ERR_FAILURE;
  }

  return BM_SUCCESS;
#endif
}

bm_status_t bm_memcpy_s2d_normal_u64(bm_handle_t handle, bm_device_mem_u64_t dst, void *src) {
#ifdef USING_CMODEL
  return handle->bm_dev->bm_device_memcpy_s2d_u64(dst, src);
#else
  u64 size;
  int trans_size = 0x10000000;//256MB
  int tran_over = 0;

  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid_u64(handle, dst)) {
    return BM_ERR_PARAM;
  }

  size = bm_mem_get_size_u64(dst);

  for(int i=0; tran_over == 0; i++) {
    bm_memcpy_info_t bm_mem_s2d;
#ifdef __linux__
      #ifdef USING_INT_CDMA
        bm_mem_s2d.intr = true;
      #else
        bm_mem_s2d.intr = false;
      #endif
      bm_mem_s2d.host_addr = (void *)((u64)src + i * trans_size);
#else
    bm_mem_s2d.intr      = 1;
    bm_mem_s2d.host_addr = (u64)src + i * trans_size;
#endif
    bm_mem_s2d.device_addr = bm_mem_get_device_addr_u64(dst) + i * trans_size;
    if(size > trans_size) {
      bm_mem_s2d.size = trans_size;
      size -= trans_size;
    } else {
      bm_mem_s2d.size = size;
      tran_over = 1;
    }

    bm_mem_s2d.dir = HOST2CHIP;
    bm_mem_s2d.src_device_addr = 0;
    bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

    union { void* ptr; u64 val; } ptr_to_u64;
    ptr_to_u64.ptr = (void *)((u64)src + i * trans_size);
    bm_profile_record_memcpy_begin(handle);
    auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
    bm_profile_record_memcpy_end(handle, ptr_to_u64.val, bm_mem_s2d.device_addr, bm_mem_s2d.size, bm_mem_s2d.dir);
    if (0 != res)
      return BM_ERR_FAILURE;
  }

  return BM_SUCCESS;
#endif
}

bm_status_t bm_memcpy_s2d_u64(bm_handle_t handle, bm_device_mem_u64_t dst, void *src) {
#ifdef USING_CMODEL
  return handle->bm_dev->bm_device_memcpy_s2d_u64(dst, src);
#else
  u64 size;
  int trans_size = 0x10000000;//256MB
  int tran_over = 0;

  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid_u64(handle, dst)) {
    return BM_ERR_PARAM;
  }

  if (handle->misc_info.pcie_soc_mode == 0 ||
      handle->misc_info.pcie_soc_mode == 1) {
    return bm_memcpy_s2d_normal_u64(handle, dst, src);
  } else {
    return bm_memcpy_s2d_mix_u64(handle, dst, src);
  }
#endif
}

bm_status_t bm_memcpy_s2d_poll(bm_handle_t     handle,
                               bm_device_mem_t dst,
                               void *          src) {
#ifdef USING_CMODEL
    return handle->bm_dev->bm_device_memcpy_s2d(dst, src);
#else
    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!bm_device_mem_range_valid(handle, dst)) {
        return BM_ERR_PARAM;
    }

    bm_memcpy_info_t bm_mem_s2d;
#ifdef __linux__
    bm_mem_s2d.intr = false;
    bm_mem_s2d.host_addr       = src;
#else
    bm_mem_s2d.intr      = true;
    bm_mem_s2d.host_addr = (u64)src;
#endif
    bm_mem_s2d.device_addr     = bm_mem_get_device_addr(dst);
    bm_mem_s2d.size            = bm_mem_get_size(dst);
    bm_mem_s2d.dir             = HOST2CHIP;
    bm_mem_s2d.src_device_addr = 0;
    bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

    union {
        void *ptr;
        u64   val;
    } ptr_to_u64;
    ptr_to_u64.ptr = src;
    bm_profile_record_memcpy_begin(handle);
    auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
    bm_profile_record_memcpy_end(handle,
                                 ptr_to_u64.val,
                                 bm_mem_s2d.device_addr,
                                 bm_mem_s2d.size,
                                 bm_mem_s2d.dir);
    return (0 != res) ? BM_ERR_FAILURE : BM_SUCCESS;
#endif
}

bm_status_t sg_memcpy_s2d_poll(bm_handle_t     handle,
                               sg_device_mem_t dst,
                               void *          src) {
#ifdef USING_CMODEL
    return BM_SUCCESS;
#else
    u64 size;
    int trans_size = 0x10000000;//256MB
    int tran_over = 0;

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!sg_device_mem_range_valid(handle, dst)) {
        return BM_ERR_PARAM;
    }

    size = sg_mem_get_size(dst);
    bm_memcpy_info_t bm_mem_s2d;

    for(int i=0; tran_over == 0; i++) {
#ifdef __linux__
      bm_mem_s2d.intr = false;
      bm_mem_s2d.host_addr       = (void *)((u64)src + i * trans_size);
#else
      bm_mem_s2d.intr      = true;
      bm_mem_s2d.host_addr = (u64)src + i * trans_size;
#endif
      bm_mem_s2d.device_addr     = sg_mem_get_device_addr(dst) + i * trans_size;
      if(size > trans_size) {
        bm_mem_s2d.size = trans_size;
        size -= trans_size;
      } else {
        bm_mem_s2d.size = size;
        tran_over = 1;
      }
      bm_mem_s2d.dir             = HOST2CHIP;
      bm_mem_s2d.src_device_addr = 0;
      bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

      union {
          void *ptr;
          u64   val;
      } ptr_to_u64;
      ptr_to_u64.ptr = (void *)((u64)src + i * trans_size);
      bm_profile_record_memcpy_begin(handle);
      auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
      bm_profile_record_memcpy_end(handle,
                                  ptr_to_u64.val,
                                  bm_mem_s2d.device_addr,
                                  bm_mem_s2d.size,
                                  bm_mem_s2d.dir);
      if (0 != res)
        return BM_ERR_FAILURE;
    }
    return BM_SUCCESS;
#endif
}

bm_status_t bm_memcpy_s2d_poll_u64(bm_handle_t     handle,
                               bm_device_mem_u64_t dst,
                               void *          src) {
#ifdef USING_CMODEL
    return handle->bm_dev->bm_device_memcpy_s2d_u64(dst, src);
#else
    u64 size;
    int trans_size = 0x10000000;//256MB
    int tran_over = 0;

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!bm_device_mem_range_valid_u64(handle, dst)) {
        return BM_ERR_PARAM;
    }

    size = bm_mem_get_size_u64(dst);
    bm_memcpy_info_t bm_mem_s2d;

    for(int i=0; tran_over == 0; i++) {
#ifdef __linux__
      bm_mem_s2d.intr = false;
      bm_mem_s2d.host_addr       = (void *)((u64)src + i * trans_size);
#else
      bm_mem_s2d.intr      = true;
      bm_mem_s2d.host_addr = (u64)src + i * trans_size;
#endif
      bm_mem_s2d.device_addr     = bm_mem_get_device_addr_u64(dst) + i * trans_size;
      if(size > trans_size) {
        bm_mem_s2d.size = trans_size;
        size -= trans_size;
      } else {
        bm_mem_s2d.size = size;
        tran_over = 1;
      }
      bm_mem_s2d.dir             = HOST2CHIP;
      bm_mem_s2d.src_device_addr = 0;
      bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

      union {
          void *ptr;
          u64   val;
      } ptr_to_u64;
      ptr_to_u64.ptr = (void *)((u64)src + i * trans_size);
      bm_profile_record_memcpy_begin(handle);
      auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
      bm_profile_record_memcpy_end(handle,
                                  ptr_to_u64.val,
                                  bm_mem_s2d.device_addr,
                                  bm_mem_s2d.size,
                                  bm_mem_s2d.dir);
      if (0 != res)
        return BM_ERR_FAILURE;
    }
    return BM_SUCCESS;
#endif
}

bm_status_t bm_smmu_s2d_poll(bm_handle_t     handle,
                               bm_device_mem_t dst,
                               void *          src) {
#ifdef USING_CMODEL
    return BM_SUCCESS;
#else
    u32 size;
    int trans_size = 0x800000;//8MB
    int tran_over = 0;

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!bm_device_mem_range_valid(handle, dst)) {
        return BM_ERR_PARAM;
    }

    size = bm_mem_get_size(dst);
    bm_memcpy_info_t bm_mem_s2d;

    for(int i=0; tran_over == 0; i++) {
#ifdef __linux__
      bm_mem_s2d.intr = false;
      bm_mem_s2d.host_addr       = (void *)((u64)src + i * trans_size);
#else
      bm_mem_s2d.intr      = true;
      bm_mem_s2d.host_addr = (u64)src + i * trans_size;
#endif
      bm_mem_s2d.device_addr     = bm_mem_get_device_addr(dst) + i * trans_size;
      if(size > trans_size) {
        bm_mem_s2d.size = trans_size;
        size -= trans_size;
      } else {
        bm_mem_s2d.size = size;
        tran_over = 1;
      }
      bm_mem_s2d.dir             = HOST2CHIP;
      bm_mem_s2d.src_device_addr = 0;
      bm_mem_s2d.cdma_iommu_mode = handle->cdma_iommu_mode;

      union {
          void *ptr;
          u64   val;
      } ptr_to_u64;
      ptr_to_u64.ptr = (void *)((u64)src + i * trans_size);
      bm_profile_record_memcpy_begin(handle);
      auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_s2d);
      bm_profile_record_memcpy_end(handle,
                                  ptr_to_u64.val,
                                  bm_mem_s2d.device_addr,
                                  bm_mem_s2d.size,
                                  bm_mem_s2d.dir);
      if (0 != res)
        return BM_ERR_FAILURE;
    }
    return BM_SUCCESS;
#endif
}

bm_status_t bm_memcpy_s2d_partial_offset(bm_handle_t handle,
                                         bm_device_mem_t dst, void *src,
                                         unsigned int size,
                                         unsigned int offset) {
  unsigned int old_devmem_size = bm_mem_get_device_size(dst);
#ifdef USING_CMODEL
  ASSERT(old_devmem_size >= offset + size);
#else
  if (old_devmem_size < offset + size) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "new device addr exceeds old device addr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
#endif
  u64 dev_mem_addr = bm_mem_get_device_addr(dst);

  bm_device_mem_t target_dev_mem =
      bm_mem_from_device(dev_mem_addr + offset, size);

  return bm_memcpy_s2d(handle, target_dev_mem, src);
}

bm_status_t sg_memcpy_s2d_partial_offset(bm_handle_t handle,
                                         sg_device_mem_t dst, void *src,
                                         u64 size,
                                         u64 offset) {
  unsigned long long old_devmem_size = sg_mem_get_device_size(dst);
#ifdef USING_CMODEL
  ASSERT(old_devmem_size >= offset + size);
#else
  if (old_devmem_size < offset + size) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "new device addr exceeds old device addr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
#endif
  u64 dev_mem_addr = sg_mem_get_device_addr(dst);

  sg_device_mem_t target_dev_mem =
      sg_mem_from_device(dev_mem_addr + offset, size);

  return sg_memcpy_s2d(handle, target_dev_mem, src);
}

bm_status_t bm_memcpy_s2d_partial_offset_u64(bm_handle_t handle,
                                         bm_device_mem_u64_t dst, void *src,
                                         u64 size,
                                         u64 offset) {
  unsigned long long old_devmem_size = bm_mem_get_device_size_u64(dst);
#ifdef USING_CMODEL
  ASSERT(old_devmem_size >= offset + size);
#else
  if (old_devmem_size < offset + size) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "new device addr exceeds old device addr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
#endif
  u64 dev_mem_addr = bm_mem_get_device_addr_u64(dst);

  bm_device_mem_u64_t target_dev_mem =
      bm_mem_from_device_u64(dev_mem_addr + offset, size);

  return bm_memcpy_s2d_u64(handle, target_dev_mem, src);
}

bm_status_t bm_memcpy_s2d_partial(bm_handle_t handle, bm_device_mem_t dst,
                                  void *src, unsigned int size) {
  return bm_memcpy_s2d_partial_offset(handle, dst, src, size, 0);
}

bm_status_t sg_memcpy_s2d_partial(bm_handle_t handle, sg_device_mem_t dst,
                                  void *src, u64 size) {
  return sg_memcpy_s2d_partial_offset(handle, dst, src, size, 0);
}

bm_status_t bm_memcpy_s2d_partial_u64(bm_handle_t handle, bm_device_mem_u64_t dst,
                                  void *src, u64 size) {
  return bm_memcpy_s2d_partial_offset_u64(handle, dst, src, size, 0);
}

bm_status_t bm_memcpy_d2s_normal(bm_handle_t handle, void *dst, bm_device_mem_t src) {
#ifndef USING_CMODEL
  bm_memcpy_info_t bm_mem_d2s;

  #ifdef __linux__
#ifdef USING_INT_CDMA
  bm_mem_d2s.intr = true;
#else
  bm_mem_d2s.intr = false;
#endif
  bm_mem_d2s.host_addr = dst;
#else
  bm_mem_d2s.intr      = 1;
  bm_mem_d2s.host_addr = (u64)dst;
#endif

  bm_mem_d2s.device_addr = bm_mem_get_device_addr(src);
  bm_mem_d2s.size = bm_mem_get_size(src);
  bm_mem_d2s.dir = CHIP2HOST;
  bm_mem_d2s.src_device_addr = 0;
  bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

  union { void* ptr; u64 val; } ptr_to_u64;
  ptr_to_u64.ptr = dst;
  bm_profile_record_memcpy_begin(handle);
  auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
  bm_profile_record_memcpy_end(handle, bm_mem_d2s.device_addr, ptr_to_u64.val, bm_mem_d2s.size, bm_mem_d2s.dir);
  if(0 != res) return BM_ERR_FAILURE;
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_memcpy_d2s_normal(bm_handle_t handle, void *dst, sg_device_mem_t src) {
#ifndef USING_CMODEL
  bm_memcpy_info_t bm_mem_d2s;
  u64 size;
  int trans_size = 0x10000000;//256MB
  int tran_over = 0;

  size = sg_mem_get_size(src);

  for(int i=0; tran_over == 0; i++) {
  #ifdef __linux__
#ifdef USING_INT_CDMA
    bm_mem_d2s.intr = true;
#else
    bm_mem_d2s.intr = false;
#endif
    bm_mem_d2s.host_addr = (void *)((u64)dst + i*trans_size);
#else
    bm_mem_d2s.intr      = 1;
    bm_mem_d2s.host_addr = (u64)dst + i*trans_size;
 #endif

    bm_mem_d2s.device_addr = sg_mem_get_device_addr(src) + i * trans_size;
    if (size > trans_size) {
      bm_mem_d2s.size = trans_size;
      size -= trans_size;
    } else {
      bm_mem_d2s.size = size;
      tran_over = 1;
    }

    bm_mem_d2s.dir = CHIP2HOST;
    bm_mem_d2s.src_device_addr = 0;
    bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

    union { void* ptr; u64 val; } ptr_to_u64;
    ptr_to_u64.ptr = (void *)((u64)dst + i*trans_size);
    bm_profile_record_memcpy_begin(handle);
    auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
    bm_profile_record_memcpy_end(handle, bm_mem_d2s.device_addr, ptr_to_u64.val, bm_mem_d2s.size, bm_mem_d2s.dir);
    if(0 != res) return BM_ERR_FAILURE;
  }
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_d2s_normal_u64(bm_handle_t handle, void *dst, bm_device_mem_u64_t src) {
#ifndef USING_CMODEL
  bm_memcpy_info_t bm_mem_d2s;
  u64 size;
  int trans_size = 0x10000000;//256MB
  int tran_over = 0;

  size = bm_mem_get_size_u64(src);

  for(int i=0; tran_over == 0; i++) {
  #ifdef __linux__
#ifdef USING_INT_CDMA
    bm_mem_d2s.intr = true;
#else
    bm_mem_d2s.intr = false;
#endif
    bm_mem_d2s.host_addr = (void *)((u64)dst + i*trans_size);
#else
    bm_mem_d2s.intr      = 1;
    bm_mem_d2s.host_addr = (u64)dst + i*trans_size;
 #endif

    bm_mem_d2s.device_addr = bm_mem_get_device_addr_u64(src) + i * trans_size;
    if (size > trans_size) {
      bm_mem_d2s.size = trans_size;
      size -= trans_size;
    } else {
      bm_mem_d2s.size = size;
      tran_over = 1;
    }

    bm_mem_d2s.dir = CHIP2HOST;
    bm_mem_d2s.src_device_addr = 0;
    bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

    union { void* ptr; u64 val; } ptr_to_u64;
    ptr_to_u64.ptr = (void *)((u64)dst + i*trans_size);
    bm_profile_record_memcpy_begin(handle);
    auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
    bm_profile_record_memcpy_end(handle, bm_mem_d2s.device_addr, ptr_to_u64.val, bm_mem_d2s.size, bm_mem_d2s.dir);
    if(0 != res) return BM_ERR_FAILURE;
  }
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_d2s_fast(bm_handle_t handle, void *dst, bm_device_mem_t src) {
#ifndef USING_CMODEL
  u64 src_vaddr = 0;
  bm_status_t ret;
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support d2s fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }
  ret = bm_mem_mmap_device_mem(handle, &src, &src_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  ret = bm_mem_invalidate_device_mem(handle, &src);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy(dst, (void *)src_vaddr, bm_mem_get_device_size(src));

  bm_mem_unmap_device_mem(handle, (void *)src_vaddr, bm_mem_get_device_size(src));
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t sg_memcpy_d2s_fast(bm_handle_t handle, void *dst, sg_device_mem_t src) {
#ifndef USING_CMODEL
  u64 src_vaddr = 0;
  bm_status_t ret;
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support d2s fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }
  ret = sg_mem_mmap_device_mem(handle, &src, &src_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  ret = sg_mem_invalidate_device_mem(handle, &src);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy(dst, (void *)src_vaddr, sg_mem_get_device_size(src));

  sg_mem_unmap_device_mem(handle, (void *)src_vaddr, sg_mem_get_device_size(src));
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_d2s_fast_u64(bm_handle_t handle, void *dst, bm_device_mem_u64_t src) {
#ifndef USING_CMODEL
  u64 src_vaddr = 0;
  bm_status_t ret;
  if (handle->misc_info.pcie_soc_mode == 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib not support d2s fast in pcie mode\n");
    return BM_ERR_FAILURE;
  }
  ret = bm_mem_mmap_device_mem_u64(handle, &src, &src_vaddr);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib mmap in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  ret = bm_mem_invalidate_device_mem_u64(handle, &src);
  if (ret != BM_SUCCESS) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "bmlib invalidate device mem in d2s fast failed\n");
    return BM_ERR_FAILURE;
  }

  memcpy(dst, (void *)src_vaddr, bm_mem_get_device_size_u64(src));

  bm_mem_unmap_device_mem_u64(handle, (void *)src_vaddr, bm_mem_get_device_size_u64(src));
#else
  UNUSED(handle);
  UNUSED(dst);
  UNUSED(src);
#endif
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_d2s(bm_handle_t handle, void *dst, bm_device_mem_t src) {
#ifdef USING_CMODEL
  return handle->bm_dev->bm_device_memcpy_d2s(dst, src);
#else
  bm_status_t ret;
  u64          dev_addr;
  unsigned int unaligned_size;
  u64          aligned_addr;
  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__,
           __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid(handle, src)) {
    return BM_ERR_PARAM;
  }

  if (handle->misc_info.pcie_soc_mode == 0) {
    // PCIE mode
    return bm_memcpy_d2s_normal(handle, dst, src);
  } else if (handle->misc_info.pcie_soc_mode == 2) {
    // MIX mode
    return bm_memcpy_d2s_mix(handle, dst, src);
  } else {
    // SoC mode
    if (bm_device_mem_page_aligned(src)) {
      return bm_memcpy_d2s_fast(handle, dst, src);
    } else if (bm_mem_get_device_size(src) <= PAGE_SIZE) {
      return bm_memcpy_d2s_normal(handle, dst, src);
    } else {
      dev_addr = bm_mem_get_device_addr(src);
      unaligned_size = PAGE_SIZE - (dev_addr & (PAGE_SIZE - 1));
      aligned_addr = (dev_addr + PAGE_SIZE) & (~(PAGE_SIZE - 1));
      unsigned int aligned_size = bm_mem_get_device_size(src) - unaligned_size;
      ret = bm_memcpy_d2s_normal(handle, dst, bm_mem_from_device(dev_addr, unaligned_size));
      if (ret != BM_SUCCESS) {
        return ret;
      }
      return bm_memcpy_d2s_fast(handle, (void *)((u64)dst + unaligned_size),
              bm_mem_from_device(aligned_addr, aligned_size));
    }
  }
#endif
}

bm_status_t sg_memcpy_d2s(bm_handle_t handle, void *dst, sg_device_mem_t src) {
#ifdef USING_CMODEL
  return handle->bm_dev->sg_device_memcpy_d2s(dst, src);
#else
  bm_status_t ret;
  u64          dev_addr;
  u64 unaligned_size;
  u64          aligned_addr;
  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__,
           __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!sg_device_mem_range_valid(handle, src)) {
    return BM_ERR_PARAM;
  }

  if (handle->misc_info.pcie_soc_mode == 0) {
    // PCIE mode
    return sg_memcpy_d2s_normal(handle, dst, src);
  } else {
    // SoC mode
    if (sg_device_mem_page_aligned(src)) {
      return sg_memcpy_d2s_fast(handle, dst, src);
    } else if (sg_mem_get_device_size(src) <= PAGE_SIZE) {
      return sg_memcpy_d2s_normal(handle, dst, src);
    } else {
      dev_addr = sg_mem_get_device_addr(src);
      unaligned_size = PAGE_SIZE - (dev_addr & (PAGE_SIZE - 1));
      aligned_addr = (dev_addr + PAGE_SIZE) & (~(PAGE_SIZE - 1));
      u64 aligned_size = sg_mem_get_device_size(src) - unaligned_size;
      ret = sg_memcpy_d2s_normal(handle, dst, sg_mem_from_device(dev_addr, unaligned_size));
      if (ret != BM_SUCCESS) {
        return ret;
      }
      return sg_memcpy_d2s_fast(handle, (void *)((u64)dst + unaligned_size),
              sg_mem_from_device(aligned_addr, aligned_size));
    }
  }
#endif
}

bm_status_t bm_memcpy_d2s_u64(bm_handle_t handle, void *dst, bm_device_mem_u64_t src) {
#ifdef USING_CMODEL
  return handle->bm_dev->bm_device_memcpy_d2s_u64(dst, src);
#else
  bm_status_t ret;
  u64          dev_addr;
  u64 unaligned_size;
  u64          aligned_addr;
  if (handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "handle is nullptr %s: %s: %d\n", __FILE__, __func__,
           __LINE__);
    return BM_ERR_DEVNOTREADY;
  }

  if (!bm_device_mem_range_valid_u64(handle, src)) {
    return BM_ERR_PARAM;
  }

  if (handle->misc_info.pcie_soc_mode == 0) {
    // PCIE mode
    return bm_memcpy_d2s_normal_u64(handle, dst, src);
  } else if (handle->misc_info.pcie_soc_mode == 2) {
    return bm_memcpy_d2s_mix_u64(handle, dst, src);
  } else {
    // SoC mode
    if (bm_device_mem_page_aligned_u64(src)) {
      return bm_memcpy_d2s_fast_u64(handle, dst, src);
    } else if (bm_mem_get_device_size_u64(src) <= PAGE_SIZE) {
      return bm_memcpy_d2s_normal_u64(handle, dst, src);
    } else {
      dev_addr = bm_mem_get_device_addr_u64(src);
      unaligned_size = PAGE_SIZE - (dev_addr & (PAGE_SIZE - 1));
      aligned_addr = (dev_addr + PAGE_SIZE) & (~(PAGE_SIZE - 1));
      u64 aligned_size = bm_mem_get_device_size_u64(src) - unaligned_size;
      ret = bm_memcpy_d2s_normal_u64(handle, dst, bm_mem_from_device_u64(dev_addr, unaligned_size));
      if (ret != BM_SUCCESS) {
        return ret;
      }
      return bm_memcpy_d2s_fast_u64(handle, (void *)((u64)dst + unaligned_size),
              bm_mem_from_device_u64(aligned_addr, aligned_size));
    }
  }
#endif
}

bm_status_t bm_memcpy_d2s_partial_offset(bm_handle_t handle, void *dst,
                                         bm_device_mem_t src, unsigned int size,
                                         unsigned int offset) {
  unsigned int old_devmem_size = bm_mem_get_device_size(src);
#ifdef USING_CMODEL
  ASSERT(old_devmem_size >= offset + size);
#else
  if (old_devmem_size < offset + size) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "new device addr exceeds old device addr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
#endif
  u64 dev_mem_addr = bm_mem_get_device_addr(src);

  bm_device_mem_t target_dev_mem =
      bm_mem_from_device(dev_mem_addr + offset, size);

  return bm_memcpy_d2s(handle, dst, target_dev_mem);
}

bm_status_t sg_memcpy_d2s_partial_offset(bm_handle_t handle, void *dst,
                                         sg_device_mem_t src, u64 size,
                                         u64 offset) {
  unsigned long long old_devmem_size = sg_mem_get_device_size(src);
#ifdef USING_CMODEL
  ASSERT(old_devmem_size >= offset + size);
#else
  if (old_devmem_size < offset + size) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "new device addr exceeds old device addr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
#endif
  u64 dev_mem_addr = sg_mem_get_device_addr(src);

  sg_device_mem_t target_dev_mem =
      sg_mem_from_device(dev_mem_addr + offset, size);

  return sg_memcpy_d2s(handle, dst, target_dev_mem);
}

bm_status_t bm_memcpy_d2s_partial_offset_u64(bm_handle_t handle, void *dst,
                                         bm_device_mem_u64_t src, u64 size,
                                         u64 offset) {
  unsigned long long old_devmem_size = bm_mem_get_device_size_u64(src);
#ifdef USING_CMODEL
  ASSERT(old_devmem_size >= offset + size);
#else
  if (old_devmem_size < offset + size) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "new device addr exceeds old device addr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }
#endif
  u64 dev_mem_addr = bm_mem_get_device_addr_u64(src);

  bm_device_mem_u64_t target_dev_mem =
      bm_mem_from_device_u64(dev_mem_addr + offset, size);

  return bm_memcpy_d2s_u64(handle, dst, target_dev_mem);
}

bm_status_t bm_memcpy_d2s_partial(bm_handle_t handle, void *dst,
                                  bm_device_mem_t src, unsigned int size) {
  return bm_memcpy_d2s_partial_offset(handle, dst, src, size, 0);
}

bm_status_t sg_memcpy_d2s_partial(bm_handle_t handle, void *dst,
                                  sg_device_mem_t src, u64 size) {
  return sg_memcpy_d2s_partial_offset(handle, dst, src, size, 0);
}

bm_status_t bm_memcpy_d2s_partial_u64(bm_handle_t handle, void *dst,
                                  bm_device_mem_u64_t src, u64 size) {
  return bm_memcpy_d2s_partial_offset_u64(handle, dst, src, size, 0);
}

bm_status_t bm_memcpy_d2s_poll(bm_handle_t     handle,
                               void *          dst,
                               bm_device_mem_t src,
                               unsigned int    size) {
    #ifdef USING_CMODEL
    (void)size;
    return handle->bm_dev->bm_device_memcpy_d2s(dst, src);
    #else
    unsigned int old_devmem_size = bm_mem_get_device_size(src);
    if (old_devmem_size < size) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "new device addr exceeds old device addr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_PARAM;
    }

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!bm_device_mem_range_valid(handle, src)) {
        return BM_ERR_PARAM;
    }

    if (handle->misc_info.pcie_soc_mode == 0) {
        // PCIE mode
        bm_memcpy_info_t bm_mem_d2s;

#ifdef __linux__
        bm_mem_d2s.intr      = false;
        bm_mem_d2s.host_addr = dst;
#else
        bm_mem_d2s.intr      = true;
        bm_mem_d2s.host_addr = (u64)dst;
#endif

        bm_mem_d2s.device_addr     = bm_mem_get_device_addr(src);
        bm_mem_d2s.size            = bm_mem_get_size(src);
        bm_mem_d2s.dir             = CHIP2HOST;
        bm_mem_d2s.src_device_addr = 0;
        bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

        union {
            void *ptr;
            u64   val;
        } ptr_to_u64;
        ptr_to_u64.ptr = dst;
        bm_profile_record_memcpy_begin(handle);
        auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
        bm_profile_record_memcpy_end(handle,
                                     bm_mem_d2s.device_addr,
                                     ptr_to_u64.val,
                                     bm_mem_d2s.size,
                                     bm_mem_d2s.dir);
        if (0 != res)
            return BM_ERR_FAILURE;

        return BM_SUCCESS;
    } else {
        return BM_ERR_FAILURE;
    }
    #endif
}

bm_status_t sg_memcpy_d2s_poll(bm_handle_t     handle,
                               void *          dst,
                               sg_device_mem_t src,
                               u64    size) {
#ifdef USING_CMODEL
    (void)size;
    return BM_SUCCESS;
#else

    int trans_size = 0x10000000;//256MB
    int tran_over = 0;

    unsigned long long old_devmem_size = sg_mem_get_device_size(src);
    if (old_devmem_size < size) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "new device addr exceeds old device addr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_PARAM;
    }

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!sg_device_mem_range_valid(handle, src)) {
        return BM_ERR_PARAM;
    }

    if (handle->misc_info.pcie_soc_mode == 0) {
        // PCIE mode

        bm_memcpy_info_t bm_mem_d2s;
    for(int i=0; tran_over == 0; i++) {
#ifdef __linux__
          bm_mem_d2s.intr      = false;
          bm_mem_d2s.host_addr = (void *)((u64)dst + i*trans_size);
#else
          bm_mem_d2s.intr      = true;
          bm_mem_d2s.host_addr = (u64)dst + i*trans_size;
#endif

          bm_mem_d2s.device_addr     = sg_mem_get_device_addr(src) + i * trans_size;
          if (size > trans_size) {
            bm_mem_d2s.size = trans_size;
            size -= trans_size;
          } else {
            bm_mem_d2s.size = size;
            tran_over = 1;
          }

          bm_mem_d2s.dir             = CHIP2HOST;
          bm_mem_d2s.src_device_addr = 0;
          bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

          union {
              void *ptr;
              u64   val;
          } ptr_to_u64;
          ptr_to_u64.ptr = (void *)((u64)dst + i*trans_size);
          bm_profile_record_memcpy_begin(handle);
          auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
          bm_profile_record_memcpy_end(handle,
                                      bm_mem_d2s.device_addr,
                                      ptr_to_u64.val,
                                      bm_mem_d2s.size,
                                      bm_mem_d2s.dir);
          if (0 != res)
              return BM_ERR_FAILURE;
        }
        return BM_SUCCESS;
    } else {
        return BM_ERR_FAILURE;
    }
    #endif
}

bm_status_t bm_memcpy_d2s_poll_u64(bm_handle_t     handle,
                               void *          dst,
                               bm_device_mem_u64_t src,
                               u64    size) {
#ifdef USING_CMODEL
    (void)size;
    return handle->bm_dev->bm_device_memcpy_d2s_u64(dst, src);
#else

    int trans_size = 0x10000000;//256MB
    int tran_over = 0;

    unsigned long long old_devmem_size = bm_mem_get_device_size_u64(src);
    if (old_devmem_size < size) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "new device addr exceeds old device addr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_PARAM;
    }

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!bm_device_mem_range_valid_u64(handle, src)) {
        return BM_ERR_PARAM;
    }

    if (handle->misc_info.pcie_soc_mode == 0) {
        // PCIE mode

        bm_memcpy_info_t bm_mem_d2s;
    for(int i=0; tran_over == 0; i++) {
#ifdef __linux__
          bm_mem_d2s.intr      = false;
          bm_mem_d2s.host_addr = (void *)((u64)dst + i*trans_size);
#else
          bm_mem_d2s.intr      = true;
          bm_mem_d2s.host_addr = (u64)dst + i*trans_size;
#endif

          bm_mem_d2s.device_addr     = bm_mem_get_device_addr_u64(src) + i * trans_size;
          if (size > trans_size) {
            bm_mem_d2s.size = trans_size;
            size -= trans_size;
          } else {
            bm_mem_d2s.size = size;
            tran_over = 1;
          }

          bm_mem_d2s.dir             = CHIP2HOST;
          bm_mem_d2s.src_device_addr = 0;
          bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

          union {
              void *ptr;
              u64   val;
          } ptr_to_u64;
          ptr_to_u64.ptr = (void *)((u64)dst + i*trans_size);
          bm_profile_record_memcpy_begin(handle);
          auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
          bm_profile_record_memcpy_end(handle,
                                      bm_mem_d2s.device_addr,
                                      ptr_to_u64.val,
                                      bm_mem_d2s.size,
                                      bm_mem_d2s.dir);
          if (0 != res)
              return BM_ERR_FAILURE;
        }
        return BM_SUCCESS;
    } else {
        return BM_ERR_FAILURE;
    }
    #endif
}

bm_status_t bm_smmu_d2s_poll(bm_handle_t     handle,
                               void *          dst,
                               bm_device_mem_t src,
                               unsigned int   size) {
#ifdef USING_CMODEL
    (void)size;
    return BM_SUCCESS;
#else

    int trans_size = 0x800000;//8MB
    int tran_over = 0;

    unsigned int old_devmem_size = bm_mem_get_device_size(src);
    if (old_devmem_size < size) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "new device addr exceeds old device addr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_PARAM;
    }

    if (handle == nullptr) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG,
                  BMLIB_LOG_ERROR,
                  "handle is nullptr %s: %s: %d\n",
                  __FILE__,
                  __func__,
                  __LINE__);
        return BM_ERR_DEVNOTREADY;
    }

    if (!bm_device_mem_range_valid(handle, src)) {
        return BM_ERR_PARAM;
    }

    if (handle->misc_info.pcie_soc_mode == 0) {
        // PCIE mode

        bm_memcpy_info_t bm_mem_d2s;
    for(int i=0; tran_over == 0; i++) {
#ifdef __linux__
          bm_mem_d2s.intr      = false;
          bm_mem_d2s.host_addr = (void *)((u64)dst + i*trans_size);
#else
          bm_mem_d2s.intr      = true;
          bm_mem_d2s.host_addr = (u64)dst + i*trans_size;
#endif

          bm_mem_d2s.device_addr     = bm_mem_get_device_addr(src) + i * trans_size;
          if (size > trans_size) {
            bm_mem_d2s.size = trans_size;
            size -= trans_size;
          } else {
            bm_mem_d2s.size = size;
            tran_over = 1;
          }

          bm_mem_d2s.dir             = CHIP2HOST;
          bm_mem_d2s.src_device_addr = 0;
          bm_mem_d2s.cdma_iommu_mode = handle->cdma_iommu_mode;

          union {
              void *ptr;
              u64   val;
          } ptr_to_u64;
          ptr_to_u64.ptr = (void *)((u64)dst + i*trans_size);
          bm_profile_record_memcpy_begin(handle);
          auto res = platform_ioctl(handle, BMDEV_MEMCPY, &bm_mem_d2s);
          bm_profile_record_memcpy_end(handle,
                                      bm_mem_d2s.device_addr,
                                      ptr_to_u64.val,
                                      bm_mem_d2s.size,
                                      bm_mem_d2s.dir);
          if (0 != res)
              return BM_ERR_FAILURE;
        }
        return BM_SUCCESS;
    } else {
        return BM_ERR_FAILURE;
    }
    #endif
}

#ifdef _WIN32
static int find_tpufirmware_path(char fw_path[512], const char* path){
	char* ptr;
	int dirname_len;
	int ret = 0;

	strcpy(fw_path, ".\\libbm1684x_kernel_module.so");

	// test ./libbm1684x_kernel_module.so
	ret = _access(fw_path,0);
	if (ret == 0) {
		return ret;
	}

	// test ./tpu_module/libbm1684x_kernel_module.so
	LPTSTR strDLLPath1 = (char*)malloc(512);
	GetModuleFileName((HINSTANCE)&__ImageBase, strDLLPath1, _MAX_PATH);

	ptr = strrchr(strDLLPath1, '\\');

	if (!ptr) {
		printf("Invalid absolute path name of libbm1684x_kernel_module.so\n");
		return -1;
	}

	dirname_len = strlen(strDLLPath1) - strlen(ptr) + 1;
	if (dirname_len < 0) {
		printf("Invalid length of folder name\n");
		return -1;
	}

	memset(fw_path, 0, 512);
	strncpy(fw_path, strDLLPath1, dirname_len);
	strcat(fw_path, "tpu_module\\");
	strcat(fw_path, path);

	free(strDLLPath1);
	ret = _access(fw_path, 0);
	if (ret == 0) {
		return ret;
	}
	return -1;
}
#endif

bm_status_t bm_memset_device_ext(bm_handle_t handle, void* value, int mode,
                                 bm_device_mem_t mem) {
  bm_status_t ret = BM_SUCCESS;
  int tmp = 0;
  tpu_kernel_module_t bm_module = NULL;
  tpu_kernel_function_t f_id;

  #ifdef __linux__
  const char key[64] = "memory_op.so";
  const char lib_path[80] = "/opt/sophon/libsophon-current/lib/dyn_load/memory_op.so";
  #else
  const char key[64] = "libbm1684x_kernel_module.so";
  static char lib_path[512] = {0};
  if (0 != find_tpufirmware_path(lib_path, key)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "libbm1684x_kernel_module.so does not exist\n");
    return BM_ERR_FAILURE;
  }
  #endif

  int key_size = strlen(key);

  if(!value) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "input NULL pointer = %d\n");
    return BM_ERR_PARAM;
  }

  if(mode<=0 || mode>4) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "input wrong memset mode = %d\n", mode);
    return BM_ERR_PARAM;
  }

  if (bm_mem_get_size(mem) % mode != 0) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "memset mode %d is mismatch with size %d\n",
      mode, bm_mem_get_size(mem));
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle, mem)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "memset wrong memory addr 0x%lx\n",
      bm_mem_get_device_addr(mem));
    return BM_ERR_PARAM;
  }

  memcpy(&tmp, value, mode);

  bm_api_memset_t api = {bm_mem_get_device_addr(mem),
                         1,
                         bm_mem_get_size(mem),
                         mode,
                         tmp};
  if (bm_is_dynamic_loading(handle)) {
    bm_module = tpu_kernel_load_module_file_key(handle, lib_path, key, key_size);
    if(bm_module == NULL) {
        printf("bm_module is null!\n");
        return BM_ERR_FAILURE;
    }

    f_id = tpu_kernel_get_function(handle, bm_module, "sg_api_memset");
    ret = tpu_kernel_launch(handle, f_id, (void *)(&api), sizeof(bm_api_memset_t));
    if (bm_module != NULL) {
      free(bm_module);
      bm_module = NULL;
    }
  } else {
    ret = bm_send_api(handle, BM_API_ID_MEM_SET,
                    (u8 *)(&api), sizeof(api));
    ret = bm_sync_api(handle);
  }
  return ret;
}

bm_status_t bm_memset_device_ext_u64(bm_handle_t handle, void* value, int mode,
                                 bm_device_mem_u64_t mem) {
  unsigned long long memset_size = 0x40000000; // 1G
  u64 size = bm_mem_get_device_size_u64(mem);
  u64 addr = bm_mem_get_device_addr_u64(mem);
  bm_status_t ret = BM_SUCCESS;
  bm_device_mem_t tmp_mem;
  bool memset_over = false;

  for(int i = 0; memset_over == false; i++) {
    if(size > memset_size) {
        size -= memset_size;
        tmp_mem = bm_mem_from_device(addr + i * memset_size, memset_size);
    } else {
        tmp_mem = bm_mem_from_device(addr + i * memset_size, size);
        memset_over = true;
    }
    ret = ret = bm_memset_device_ext(handle, value, mode, tmp_mem);
    if (ret != BM_SUCCESS){
      break;
    }
  }
  return ret;
}

bm_status_t bm_memset_device(bm_handle_t     handle,
                             const int       value,
                             bm_device_mem_t mem) {
    return bm_memset_device_ext(handle, (void *)&value, 4, mem);
}

bm_status_t bm_memcpy_d2d_with_core(bm_handle_t handle, bm_device_mem_t dst,
                          int dst_offset, bm_device_mem_t src, int src_offset,
                          int len, int core_id) {
  int max_n = 1;
  int src_nstride = bm_mem_get_device_size(src) / max_n / FLOAT_SIZE;
  int dst_nstride = bm_mem_get_device_size(dst) / max_n / FLOAT_SIZE;
  bm_status_t ret = BM_SUCCESS;
  tpu_kernel_module_t bm_module = NULL;
  tpu_kernel_function_t f_id;

  #ifdef __linux__
  const char key[64] = "memory_op.so";
  const char lib_path[80] = "/opt/sophon/libsophon-current/lib/dyn_load/memory_op.so";
  #else
  const char key[64] = "libbm1684x_kernel_module.so";
  static char lib_path[512] = {0};
  if (0 != find_tpufirmware_path(lib_path, key)) {
	  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "libbm1684x_kernel_module.so does not exist\n");
	  return BM_ERR_FAILURE;
  }
  #endif
  int key_size = strlen(key);

  if (!bm_device_mem_range_valid(handle, src)) {
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle, dst)) {
    return BM_ERR_PARAM;
  }

  bm_api_memcpy_t api = {bm_mem_get_device_addr(src) + src_offset,
                         bm_mem_get_device_addr(dst) + dst_offset,
                         1,
                         src_nstride,
                         dst_nstride,
                         len};
  if (bm_is_dynamic_loading(handle)) {
    bm_module = tpu_kernel_load_module_file_key(handle, lib_path, key, key_size);
    if(bm_module == NULL) {
        printf("bm_module is null!\n");
        return BM_ERR_FAILURE;
    }

    f_id = tpu_kernel_get_function(handle, bm_module, "sg_api_memcpy");
    ret = tpu_kernel_launch(handle, f_id, (void *)(&api), sizeof(bm_api_memcpy_t));
    if (bm_module != NULL) {
      free(bm_module);
      bm_module = NULL;
    }
  } else {
    ret = bm_send_api_to_core(handle, BM_API_ID_MEM_CPY, (u8 *)(&api), sizeof(api), core_id);
    if (BM_SUCCESS == ret) {
      ret = bm_sync_api_from_core(handle, core_id);
    }
  }

  return ret;
}

bm_status_t bm_memcpy_d2d(bm_handle_t handle, bm_device_mem_t dst,
                          int dst_offset, bm_device_mem_t src, int src_offset,
                          int len) {
  return bm_memcpy_d2d_with_core(handle, dst, dst_offset, src, src_offset, len, 0);
}
bm_status_t bm_memcpy_d2d_stride_with_core(bm_handle_t     handle,
                                 bm_device_mem_t dst,
                                 int             dst_stride,
                                 bm_device_mem_t src,
                                 int             src_stride,
                                 int             count,
                                 int             format_size,
                                 int             core_id) {
  bm_status_t ret = BM_SUCCESS;
  tpu_kernel_module_t bm_module = NULL;
  tpu_kernel_function_t f_id;

  #ifdef __linux__
  const char key[64] = "memory_op.so";
  const char lib_path[80] = "/opt/sophon/libsophon-current/lib/dyn_load/memory_op.so";
  #else
  const char key[64] = "libbm1684x_kernel_module.so";
  static char lib_path[512] = {0};
  if (0 != find_tpufirmware_path(lib_path, key)) {
	  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "libbm1684x_kernel_module.so does not exist\n");
	  return BM_ERR_FAILURE;
  }
  #endif

  int key_size = strlen(key);

  if (!bm_device_mem_range_valid(handle, src)) {
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle, dst)) {
    return BM_ERR_PARAM;
  }

  if (src_stride < 1 || (dst_stride != 1 && !(dst_stride == 4 && src_stride == 1 && format_size == 1))) {
      bmlib_log(BMLIB_MEMORY_LOG_TAG,
                BMLIB_LOG_ERROR,
                "stride not supported, dst_stride:%d, src_stride:%d\n",
                dst_stride,
                src_stride);
      return BM_ERR_PARAM;
  }
  if (format_size != 1 && format_size != 2 && format_size != 4) {
      bmlib_log(BMLIB_MEMORY_LOG_TAG,
                BMLIB_LOG_ERROR,
                "format_size only support 1/2/4, not support %d\n",
                format_size);
      return BM_ERR_PARAM;
  }
  size_t src_size = bm_mem_get_device_size(src);
  size_t dst_size = bm_mem_get_device_size(dst);
  if (((size_t)count * dst_stride * format_size) > dst_size) {
      bmlib_log(
          BMLIB_MEMORY_LOG_TAG,
          BMLIB_LOG_ERROR,
          "dst mem size is not enough. dst size:%ld, dst_stride:%d, count:%d, format_size:%d\n",
          dst_size,
          dst_stride,
          count,
          format_size);
      return BM_ERR_PARAM;
  }
  if (((size_t)count * src_stride * format_size) > src_size) {
      bmlib_log(
          BMLIB_MEMORY_LOG_TAG,
          BMLIB_LOG_ERROR,
          "src mem size is not enough. src size:%ld, src_stride:%d, count:%d, format_size:%d\n",
          src_size,
          src_stride,
          count,
          format_size);
      return BM_ERR_PARAM;
  }

  bm_api_memcpy_wstride_t api = {bm_mem_get_device_addr(src),
                                 bm_mem_get_device_addr(dst),
                                 src_stride,
                                 dst_stride,
                                 count,
                                 format_size};
  if (bm_is_dynamic_loading(handle)) {
    bm_module = tpu_kernel_load_module_file_key(handle, lib_path, key, key_size);
    if(bm_module == NULL) {
        printf("bm_module is null!\n");
        return BM_ERR_FAILURE;
    }

    f_id = tpu_kernel_get_function(handle, bm_module, "sg_api_memcpy_wstride");
    ret = tpu_kernel_launch(handle, f_id, (void *)(&api), sizeof(bm_api_memcpy_wstride_t));
    if (bm_module != NULL) {
      free(bm_module);
      bm_module = NULL;
    }
  } else {
    ret = bm_send_api_to_core(handle, BM_API_ID_MEMCPY_WSTRIDE, (u8 *)(&api), sizeof(api), core_id);
    if (BM_SUCCESS == ret) {
      ret = bm_sync_api_from_core(handle, core_id);
    }
  }
  return ret;
}

bm_status_t bm_memcpy_d2d_stride(bm_handle_t     handle,
                                 bm_device_mem_t dst,
                                 int             dst_stride,
                                 bm_device_mem_t src,
                                 int             src_stride,
                                 int             count,
                                 int             format_size) {
  return bm_memcpy_d2d_stride_with_core(handle, dst, dst_stride, src, src_stride,
    count, format_size, 0);
}

bm_status_t bm_memcpy_d2d_byte_with_core(bm_handle_t handle, bm_device_mem_t dst,
                               size_t dst_offset, bm_device_mem_t src,
                               size_t src_offset, size_t size, int core_id) {

  bm_status_t ret = BM_SUCCESS;
  tpu_kernel_module_t bm_module = NULL;
  tpu_kernel_function_t f_id;

  #ifdef __linux__
  const char key[64] = "memory_op.so";
  const char lib_path[80] = "/opt/sophon/libsophon-current/lib/dyn_load/memory_op.so";
  #else
  const char key[64] = "libbm1684x_kernel_module.so";
  static char lib_path[512] = {0};
  if (0 != find_tpufirmware_path(lib_path, key)) {
	  bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "libbm1684x_kernel_module.so does not exist\n");
	  return BM_ERR_FAILURE;
  }
  #endif

  int key_size = strlen(key);

  if (!bm_device_mem_range_valid(handle, src)) {
    return BM_ERR_PARAM;
  }

  if (!bm_device_mem_range_valid(handle, dst)) {
    return BM_ERR_PARAM;
  }

  bm_api_memcpy_byte_t api = {bm_mem_get_device_addr(src) + src_offset,
                              bm_mem_get_device_addr(dst) + dst_offset, size};
  if (bm_is_dynamic_loading(handle)) {
    bm_module = tpu_kernel_load_module_file_key(handle, lib_path, key, key_size);
    if(bm_module == NULL) {
        printf("bm_module is null!\n");
        return BM_ERR_FAILURE;
    }

    f_id = tpu_kernel_get_function(handle, bm_module, "sg_api_memcpy_byte");
    ret = tpu_kernel_launch(handle, f_id, (void *)(&api), sizeof(bm_api_memcpy_byte_t));
    if (bm_module != NULL) {
      free(bm_module);
      bm_module = NULL;
    }
  } else {
    ret = bm_send_api_to_core(handle, BM_API_ID_MEMCPY_BYTE, (u8 *)(&api), sizeof(api), core_id);
    if (BM_SUCCESS == ret) {
      ret = bm_sync_api_from_core(handle, core_id);
    }
  }
  return ret;
}

bm_status_t bm_memcpy_d2d_byte(bm_handle_t handle, bm_device_mem_t dst,
                               size_t dst_offset, bm_device_mem_t src,
                               size_t src_offset, size_t size) {
  return bm_memcpy_d2d_byte_with_core(handle, dst, dst_offset, src, src_offset, size, 0);
}

bm_status_t bm_memcpy_d2d_u64(bm_handle_t             handle,
                                 bm_device_mem_u64_t  dst,
                                 unsigned long long   dst_offset,
                                 bm_device_mem_u64_t  src,
                                 unsigned long long   src_offset,
                                 unsigned long long   len) {
  unsigned long long trans_size = 0x10000000; // 256MB * DWORD
  u64 tmp_len = trans_size;
  u64 src_device_addr = 0;
  u64 dst_device_addr = 0;
  bm_status_t ret = BM_SUCCESS;
  bm_device_mem_t tmp_src;
  bm_device_mem_t tmp_dst;
  bool trans_over = false;

  for(int i = 0; trans_over == false; i++) {
    if(len > trans_size) {
        len -= trans_size;
    } else {
        tmp_len = len;
        trans_over = true;
    }
    src_device_addr = bm_mem_get_device_addr_u64(src) + trans_size * 4 * i + (src_offset & 0xffffffff80000000);
    dst_device_addr = bm_mem_get_device_addr_u64(dst) + trans_size * 4 * i + (dst_offset & 0xffffffff80000000);
    tmp_src = bm_mem_from_device(src_device_addr, 4 * tmp_len);
    tmp_dst = bm_mem_from_device(dst_device_addr, 4 * tmp_len);
    ret = bm_memcpy_d2d(handle, tmp_dst, (dst_offset & 0x7fffffff), tmp_src, (src_offset & 0x7fffffff), tmp_len);
    if (ret != BM_SUCCESS){
      break;
    }
  }
  return ret;
}

bm_status_t bm_memcpy_d2d_stride_u64(bm_handle_t          handle,
                                      bm_device_mem_u64_t dst,
                                      unsigned long long  dst_stride,
                                      bm_device_mem_u64_t src,
                                      unsigned long long  src_stride,
                                      unsigned long long  count,
                                      int                 format_size) {
  unsigned long long trans_size = 0x40000000;
  u64 src_device_addr = bm_mem_get_device_addr_u64(src);
  u64 dst_device_addr = bm_mem_get_device_addr_u64(dst);
  u64 src_device_size = bm_mem_get_device_size_u64(src);
  u64 dst_device_size = bm_mem_get_device_size_u64(dst);
  u64 tmp_count = dst_stride > src_stride ? dst_stride:src_stride;
  tmp_count = trans_size/tmp_count/format_size;
  u64 tmp_size = 0;
  bm_device_mem_t tmp_src;
  bm_device_mem_t tmp_dst;
  bool trans_over = false;
  bm_status_t ret = BM_SUCCESS;

  if ((count * dst_stride * format_size) > dst_device_size) {
      bmlib_log(
          BMLIB_MEMORY_LOG_TAG,
          BMLIB_LOG_ERROR,
          "dst mem size is not enough. dst size:%lld, dst_stride:%lld, count:%lld, format_size:%d\n",
          dst_device_size,
          dst_stride,
          count,
          format_size);
      return BM_ERR_PARAM;
  }
  if ((count * src_stride * format_size) > src_device_size) {
      bmlib_log(
          BMLIB_MEMORY_LOG_TAG,
          BMLIB_LOG_ERROR,
          "src mem size is not enough. src size:%lld, src_stride:%lld, count:%lld, format_size:%d\n",
          src_device_size,
          src_stride,
          count,
          format_size);
      return BM_ERR_PARAM;
  }

  for(int i = 0; trans_over == false; i++) {
    if (count > tmp_count) {
        count -= tmp_count;
    } else {
        tmp_count = count;
        trans_over = true;
    }
    tmp_size = format_size * src_stride * tmp_count;
    tmp_src = bm_mem_from_device(src_device_addr, tmp_size);
    src_device_addr += tmp_size;
    tmp_size = format_size * dst_stride * tmp_count;
    tmp_dst = bm_mem_from_device(dst_device_addr, tmp_size);
    dst_device_addr += tmp_size;
    ret = bm_memcpy_d2d_stride(handle, tmp_dst, dst_stride, tmp_src, src_stride, tmp_count, format_size);
    if (ret != BM_SUCCESS){
      break;
    }
  }
  return ret;
}

bm_status_t bm_memcpy_d2d_byte_u64(bm_handle_t handle,
                                    bm_device_mem_u64_t dst,
                                    unsigned long long  dst_offset,
                                    bm_device_mem_u64_t src,
                                    unsigned long long  src_offset,
                                    unsigned long long  size) {
  unsigned long long trans_size = 0x40000000;
  u64 tmp_size = trans_size;
  u64 src_device_addr = 0;
  u64 dst_device_addr = 0;
  bm_status_t ret = BM_SUCCESS;
  bm_device_mem_t tmp_src;
  bm_device_mem_t tmp_dst;
  bool trans_over = false;

  for(int i = 0; trans_over == false; i++) {
    if(size > trans_size) {
        size -= trans_size;
    } else {
        tmp_size = size;
        trans_over = true;
    }
    src_device_addr = bm_mem_get_device_addr_u64(src) + trans_size * i + (src_offset & 0xffffffff80000000);
    dst_device_addr = bm_mem_get_device_addr_u64(dst) + trans_size * i + (dst_offset & 0xffffffff80000000);
    tmp_src = bm_mem_from_device(src_device_addr, tmp_size);
    tmp_dst = bm_mem_from_device(dst_device_addr, tmp_size);
    ret = bm_memcpy_d2d_byte(handle, tmp_dst, (dst_offset & 0x7fffffff), tmp_src, (src_offset & 0x7fffffff), tmp_size);
    if (ret != BM_SUCCESS){
      break;
    }
  }
  return ret;
}

#ifndef USING_CMODEL
bm_status_t bm_calculate_cdma_addr(bm_handle_t src_handle,
                                   bm_handle_t dst_handle, u64 *src, u64 *dst,
                                   bool force_use_dst_cdma) {
  bm_misc_info src_info;
  bm_misc_info dst_info;
  platform_ioctl(src_handle, BMDEV_GET_MISC_INFO, &src_info);
  platform_ioctl(dst_handle, BMDEV_GET_MISC_INFO, &dst_info);

  if ((src_info.domain_bdf & ~0x7) != (dst_info.domain_bdf & ~0x7)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG,
        BMLIB_LOG_ERROR,
        "only support transfter for same card src bdf = 0x%x, dst bdf = 0x%x\n",
        src_info.domain_bdf, dst_info.domain_bdf);
    return BM_ERR_PARAM;
  }

  if ((src_info.domain_bdf & 0x7) != (dst_info.domain_bdf & 0x7)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
           "src func num = 0x%d, dst func num = 0x%d \n",
           src_info.domain_bdf & 0x7, dst_info.domain_bdf & 0x7);
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
           "cdma src addr = 0x%llx, dst addr = 0x%llx \n", *src, *dst);
    if (force_use_dst_cdma) {
      *src = *src | 0x3ULL << 40 | ((src_info.domain_bdf & 0x7ULL) << 37);
      *dst = *dst | 0x3fULL << 36;
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
             "force dst cdma src addr = 0x%llx, dst addr = 0x%llx \n", *src,
             *dst);
    } else {
      *src = *src | 0x3fULL << 36;
      *dst = *dst | 0x3ULL << 40 | ((dst_info.domain_bdf & 0x7ULL) << 37);
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
             "cdma src addr = 0x%llx, dst addr = 0x%llx \n", *src, *dst);
    }
  } else {
    *src = *src | 0x3fULL << 36;
    *dst = *dst | 0x3fULL << 36;
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_INFO,
           "src func num = 0x%d, dst func num = 0x%d \n",
           src_info.domain_bdf & 0x7, dst_info.domain_bdf & 0x7);
  }
  return BM_SUCCESS;
}
#endif

bm_status_t bm_memcpy_c2c(bm_handle_t src_handle, bm_handle_t dst_handle,
                          bm_device_mem_t src, bm_device_mem_t dst,
                          bool force_use_dst_cdma) {
#ifdef USING_CMODEL
  UNUSED(src_handle);
  UNUSED(dst_handle);
  UNUSED(src);
  UNUSED(dst);
  UNUSED(force_use_dst_cdma);
  return BM_NOT_SUPPORTED;
#else
  if (src_handle == nullptr || dst_handle == nullptr) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR, "handle is nullptr %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_DEVNOTREADY;
  }
  u32 size = bm_mem_get_size(src);
  u64 src_addr = bm_mem_get_device_addr(src);
  u64 dst_addr = bm_mem_get_device_addr(dst);
  if (BM_SUCCESS != bm_calculate_cdma_addr(src_handle, dst_handle, &src_addr,
                                           &dst_addr, force_use_dst_cdma))
    return BM_ERR_FAILURE;
  bm_memcpy_info_t bm_mem_c2c;
  #ifdef USING_INT_CDMA
      bm_mem_c2c.intr = true;
  #else
      bm_mem_c2c.intr = false;
  #endif
  #if __linux__
  bm_mem_c2c.host_addr = nullptr;
  #else
  bm_mem_c2c.host_addr = 0;
  #endif
  bm_mem_c2c.device_addr = dst_addr;
  bm_mem_c2c.src_device_addr = src_addr;
  bm_mem_c2c.size = size;
  bm_mem_c2c.dir = CHIP2CHIP;
  bm_mem_c2c.cdma_iommu_mode = BMLIB_NOT_USE_IOMMU;

  if (force_use_dst_cdma) {
    if (0 != platform_ioctl(dst_handle, BMDEV_MEMCPY, &bm_mem_c2c))
      return BM_ERR_FAILURE;
  } else {
    if (0 != platform_ioctl(src_handle, BMDEV_MEMCPY, &bm_mem_c2c))
      return BM_ERR_FAILURE;
  }
  return BM_SUCCESS;
#endif
}

bm_system_mem_t bm_mem_from_system(void *system_addr) {
  bm_system_mem_t mem;
  memset(&mem, 0x0, sizeof(bm_device_mem_t));
  mem.u.system.system_addr = system_addr;
  mem.flags.u.mem_type = BM_MEM_TYPE_SYSTEM;
  return mem;
}

/* this function is designed to use in SOC_MODE, the parameter should be the
 * device memory from ION or something like that and the address is desired to
 * be used directly by NPU, no data copy is needed. and the right shift means
 * the input address should be 4 aligned.
 */

bm_device_mem_t bm_mem_null(void) {
  bm_device_mem_t mem;
  memset(&mem, 0x0, sizeof(bm_device_mem_t));
  mem.flags.u.mem_type = BM_MEM_TYPE_INVALID;
  return mem;
}

/*
 * CAUTION:
 *   This implies allocation (need to free later)
 *   This implies memcpy if need_copy is set, which might be slow
 */
bm_status_t bm_mem_convert_system_to_device_neuron(bm_handle_t handle,
                                                   struct bm_mem_desc *dev_mem,
                                                   struct bm_mem_desc sys_mem,
                                                   bool need_copy, int n, int c,
                                                   int h, int w) {
  if (bm_mem_get_type(sys_mem) != BM_MEM_TYPE_SYSTEM) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
           "mem type is illegal %s: %s: %d\n",
           __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  if (BM_SUCCESS != bm_malloc_neuron_device(handle, dev_mem, n, c, h, w)) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "bm_malloc_neuron_device error %s: %s: %d\n",
          __FILE__, __func__, __LINE__);

    return BM_ERR_FAILURE;
  }

  if (need_copy) {
    if (BM_SUCCESS !=
        bm_memcpy_s2d(handle, *dev_mem, bm_mem_get_system_addr(sys_mem))) {
      bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
            "bm_memcpy_s2d error %s: %s: %d\n",
            __FILE__, __func__, __LINE__);
      bm_free_device(handle, *dev_mem);

      return BM_ERR_FAILURE;
    }
  }
  return BM_SUCCESS;
}

bm_status_t bm_mem_convert_system_to_device_neuron_byte(
    bm_handle_t handle, struct bm_mem_desc *dev_mem, struct bm_mem_desc sys_mem,
    bool need_copy, int n, int c, int h, int w) {
  if (bm_mem_get_type(sys_mem) != BM_MEM_TYPE_SYSTEM) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  BM_CHECK_RET(bm_malloc_device_byte(handle, dev_mem, n * c * h * w));

  if (need_copy) {
    BM_CHECK_RET(
        bm_memcpy_s2d(handle, *dev_mem, bm_mem_get_system_addr(sys_mem)));
  }
  return BM_SUCCESS;
}

/*
 * CAUTION:
 *   This implies allocation (need to free later)
 *   This implies memcpy if need_copy is set, which might be slow
 */
bm_status_t bm_mem_convert_system_to_device_coeff(bm_handle_t handle,
                                                  struct bm_mem_desc *dev_mem,
                                                  struct bm_mem_desc sys_mem,
                                                  bool need_copy,
                                                  int coeff_count) {
  if (bm_mem_get_type(sys_mem) != BM_MEM_TYPE_SYSTEM) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
          "mem type is illegal %s: %s: %d\n",
          __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  BM_CHECK_RET(bm_malloc_device_dword(handle, dev_mem, coeff_count));

  if (need_copy) {
    BM_CHECK_RET(
        bm_memcpy_s2d(handle, *dev_mem, bm_mem_get_system_addr(sys_mem)));
  }
  return BM_SUCCESS;
}

bm_status_t bm_mem_convert_system_to_device_coeff_byte(
    bm_handle_t handle, struct bm_mem_desc *dev_mem, struct bm_mem_desc sys_mem,
    bool need_copy, int coeff_count) {
  if (bm_mem_get_type(sys_mem) != BM_MEM_TYPE_SYSTEM) {
    bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
        "mem type is illegal %s: %s: %d\n",
        __FILE__, __func__, __LINE__);
    return BM_ERR_PARAM;
  }

  BM_CHECK_RET(bm_malloc_device_byte(handle, dev_mem, coeff_count));

  if (need_copy) {
    BM_CHECK_RET(
        bm_memcpy_s2d(handle, *dev_mem, bm_mem_get_system_addr(sys_mem)));
  }
  return BM_SUCCESS;
}

bm_status_t bm_memcpy_s2d_gather(bm_handle_t handle, bm_device_mem_t dst, int argc, ...)
{
  bm_status_t ret;
  va_list args;
  void *vaddr;
  u64 len;
  u32 total = dst.size;
  u64 sum = 0;

  va_start(args, argc);
  for (int i = 0; i < argc; i+=2) {
      vaddr = va_arg(args, void *);
      len = va_arg(args, unsigned long long);
      sum += len;
      if (sum > total) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
                  "%s sum: %u, total: %lu\n", __func__, sum, total);
      }
      dst.size = len;
      ret = bm_memcpy_s2d(handle, dst, vaddr);
      if (ret != BM_SUCCESS) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
                  "%s failed, dst address: %lu, len: %llu\n", __func__, dst.u.device.device_addr, dst.size);
      }
      dst.u.device.device_addr += len;
  }
  va_end(args);

  return ret;
}

bm_status_t bm_memcpy_d2s_scatter(bm_handle_t handle, bm_device_mem_t src, int argc, ...)
{
  bm_status_t ret;
  va_list args;
  void *vaddr;
  u64 len;
  u32 total = src.size;
  u64 sum = 0;

  va_start(args, argc);
  for (int i = 0; i < argc; i+=2) {
      vaddr = va_arg(args, void *);
      len = va_arg(args, unsigned long long);
      sum += len;
      if (sum > total) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
                  "%s sum: %u, total: %lu\n", __func__, sum, total);
      }
      src.size = len;
      ret = bm_memcpy_d2s(handle, vaddr, src);
      if (ret != BM_SUCCESS) {
        bmlib_log(BMLIB_MEMORY_LOG_TAG, BMLIB_LOG_ERROR,
                  "%s failed, src address: %lu, len: %llu\n", __func__, src.u.device.device_addr, src.size);
      }
      src.u.device.device_addr += len;
  }
  va_end(args);

  return ret;
}

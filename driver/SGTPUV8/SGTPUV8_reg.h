#ifndef _SGTPUV8_REG_H_
#define _SGTPUV8_REG_H_

/* this header file contains all SGTPUV8 register definitions
 * and global base address definitions
 */

#define DTCM_MEM_START_ADDR            0x02000000
#define DTCM_MEM_SIZE                  (1<<19)           //dtcm size 512K
//DTCM layout:
//======[0,     111KB+896byte)===== reserved for normal software.
//======[111KB+896byte, 112KB)===== for device information.
//======[112KB, 128KB)===== share memory(message queue for host and device)
//======[128KB, 512KB)===== for bmcv.

#define DTCM_MEM_RESERVED_SIZE         (111*1024+896)   // 112KB + 896 byte reserved for software.
#define DTCM_MEM_DEV_INFO_SIZE         (128)            // 128 byte for device information.
#define DTCM_MEM_SHARE_MEM_SIZE        (16*1024)        // 16KB for share memory.
#define DTCM_MEM_BMCV_SIZE             (384*1024)       // 384KB for bmcv.
#define DTCM_MEM_RESERVED_OFFSET       (0)
#define DTCM_MEM_DEV_INFO_OFFSET       (DTCM_MEM_RESERVED_OFFSET + DTCM_MEM_RESERVED_SIZE)
#define DTCM_MEM_SHARE_OFFSET          (DTCM_MEM_DEV_INFO_OFFSET + DTCM_MEM_DEV_INFO_SIZE)
#define DTCM_MEM_BMCV_OFFSET           (DTCM_MEM_SHARE_OFFSET + DTCM_MEM_SHARE_MEM_SIZE)
#define DTCM_MEM_STATIC_END_OFFSET     (DTCM_MEM_BMCV_OFFSET + DTCM_MEM_BMCV_SIZE)

#define DTCM_MEM_RESERVED_START_ADDR   (DTCM_MEM_START_ADDR + DTCM_MEM_RESERVED_OFFSET)
#define SHARE_MEM_START_ADDR           (DTCM_MEM_START_ADDR + DTCM_MEM_SHARE_OFFSET)

#define L2_SRAM_START_ADDR             0x10000000
#define L2_SRAM_TPU_TABLE_OFFSET       0x300000

/* SGTPUV8 device ddr layout
 *
 * ddr_start                                      alloc_start
 * +---- ~ -----+------~------+---- ~ -------+---- ~ ------------- ~ -------- ~ -----
 * | fw image   |  fw log     | arm reserved | smmu page table | ///////////////////
 * +---- ~ -----+----- ~ -----+---- ~ -------+---- ~ ------------- ~ -------- ~ -----
 * <----12MB---><-----4M------><------64MB-----><-------1MB----->
 */

#define GLOBAL_MEM_START_ADDR          0x100000000

#define FW_DDR_IMG_START	0x0
#define FW_DDR_IMG_SIZE		0x1000000

#define ARM_RESERVED_START	(FW_DDR_IMG_START+FW_DDR_IMG_SIZE)
#define ARM_RESERVED_SIZE	0x4000000

/* 1 entry for 4KB VA, 1MB page table for 1GB VA, 4GB too big,
 * driver can hardly allocate struct pages* from slab
 */
#define SMMU_PAGE_ENTRY_NUM           (0x40000)  //256K (* 4B)

#define SMMU_RESERVED_START     (ARM_RESERVED_START + ARM_RESERVED_SIZE)
#define SMMU_RESERVED_SIZE      (SMMU_PAGE_ENTRY_NUM * 4)
#define EFECTIVE_GMEM_START     (SMMU_RESERVED_START + SMMU_RESERVED_SIZE)

#define JPU_RESERVED_SIZE        (0x10000000)

#define TOP_REG_CTRL_BASE_ADDR         (0x50010000)
#define GP_REG_BASE_ADDR            (TOP_REG_CTRL_BASE_ADDR + 0x80)


#define NV_TIMER_BASE_ADDR        0x50010180
#define OS_TIMER_CTRL_BASE_ADDR        0x50022000
#define INT0_CTRL_BASE_ADDR            0x50023000
#define EFUSE_BASE                     0x50028000
#define PWM_CTRL_BASE_ADDR             0x50029000
#define DDR_CTRL_BASE_ADDR             0x68000000

#define MMU_ENGINE_BASE_ADDR           0x58002000

#ifdef FAST_REG_ACCESS
  #define GDMA_ENGINE_BASE_ADDR          0x2080000
  #define BD_ENGINE_BASE_ADDR            0x2081000
#else
  #define GDMA_ENGINE_BASE_ADDR          0x58000000
  #define BD_ENGINE_BASE_ADDR            0x58001000
#endif

#define SPACC_ENGINE_BASE_ADDR          0x58004000
#define PKA_BASE_ADDR                   0x58008000

/*top register*/
#define TOP_ITCM_SWITCH        0x008
#define TOP_POSITION           0x01c
#define TOP_ARM_ADDRMODE       0x034
#define TOP_CLK_LOCK           0x104
#define TOP_PLL_STATUS         0x0c0
#define TOP_PLL_ENABLE         0x0c4
#define TOP_TPLL_CTL           0x29a0
#define TOP_VPLL_CTL           0x0f4
#define TOP_AUTO_CLK_GATE_EN0  0x1c0
#define TOP_AUTO_CLK_GATE_EN1  0x1c4
#define TOP_AUTO_CLK_GATE_EN2  0x1c8
#define TOP_AUTO_CLK_GATE_EN3  0x1cc
#define TOP_AUTO_CLK_GATE_EN4  0x1d0
#define TOP_AUTO_CLK_GATE_EN5  0x1d4
#define TOP_CLK_ENABLE0        0x800
#define TOP_CLK_ENABLE1        0x804
#define TOP_CLK_ENABLE2        0x808
#define TOP_CLK_SELECT         0x820
#define TOP_CLK_DIV0           0x8a0
#define TOP_CLK_DIV1           0x8a4
#define TOP_SW_RESET0          0x3000
#define TOP_SW_RESET1          0x3004
#define TOP_SW_RESET2          0x3008
#define TOP_SW_RESET3          0x300c



/*smmu register*/
#define SMMU_FARR              0x000
#define SMMU_FARW              0x008
#define SMMU_FSR               0x010
#define SMMU_ICR               0x018
#define SMMU_IR                0x020
#define SMMU_IS                0x028
#define SMMU_TTBR              0x030
#define SMMU_TTER              0x038
#define SMMU_SCR               0x040

#endif


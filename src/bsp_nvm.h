/* The BSP's NVM regions (src/bsp_nvm.c), for bsp_get_capabilities(). */
#pragma once
#include "bsp_api.h"

#define BSP_N_NVM_REGIONS 1u
#define BSP_NVM0_WRITE_SIZE 8u /* the program unit offered; bsp.toml's [[bsp.nvm_regions]] agrees */

extern const bsp_nvm_region_t bsp_nvm_regions[BSP_N_NVM_REGIONS];

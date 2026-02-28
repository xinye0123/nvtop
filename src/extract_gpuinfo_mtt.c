/*
 *
 * Copyright (C) 2021-2024 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/common.h"
#include "nvtop/extract_gpuinfo_common.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <dirent.h>

// MTT API function pointers
typedef void* MtmlLibrary;
typedef void* MtmlDevice;
typedef void* MtmlGpu;
typedef void* MtmlMemory;
typedef void* MtmlVpu;
typedef void* MtmlSystem;

typedef enum {
    MTML_SUCCESS = 0,
    MTML_ERROR_DRIVER_NOT_LOADED,
    MTML_ERROR_DRIVER_FAILURE,
    MTML_ERROR_INVALID_ARGUMENT,
    MTML_ERROR_NOT_SUPPORTED,
    MTML_ERROR_NO_PERMISSION,
    MTML_ERROR_INSUFFICIENT_SIZE,
    MTML_ERROR_NOT_FOUND,
    MTML_ERROR_INSUFFICIENT_MEMORY,
    MTML_ERROR_DRIVER_TOO_OLD,
    MTML_ERROR_DRIVER_TOO_NEW,
    MTML_ERROR_TIMEOUT,
    MTML_ERROR_RESOURCE_IS_BUSY,
    MTML_ERROR_UNKNOWN = 999
} MtmlReturn;

typedef struct {
    char sbdf[32];
    unsigned int segment;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubsystemId;
    unsigned int busWidth;
    float pciMaxSpeed;
    float pciCurSpeed;
    unsigned int pciMaxWidth;
    unsigned int pciCurWidth;
    unsigned int pciMaxGen;
    unsigned int pciCurGen;
    int rsvd[6];
} MtmlPciInfo;

typedef struct {
    unsigned int virtCap : 1;
    unsigned int virtRole : 3;
    unsigned int mpcCap : 1;
    unsigned int mpcType : 3;
    unsigned int mtLinkCap : 1;
    unsigned int rsvd : 23;
    unsigned int rsvd2 : 32;
} MtmlDeviceProperty;

// Library initialization and shutdown
static MtmlReturn (*mtmlLibraryInit)(MtmlLibrary **lib);
static MtmlReturn (*mtmlLibraryShutDown)(MtmlLibrary *lib);
static MtmlReturn (*mtmlLibraryCountDevice)(const MtmlLibrary *lib, unsigned int *count);
static MtmlReturn (*mtmlLibraryInitDeviceByIndex)(const MtmlLibrary *lib, unsigned int index, MtmlDevice **dev);

// Device functions
static MtmlReturn (*mtmlDeviceGetName)(const MtmlDevice *dev, char *name, unsigned int length);
static MtmlReturn (*mtmlDeviceGetPciInfo)(const MtmlDevice *dev, MtmlPciInfo *pci);
static MtmlReturn (*mtmlDeviceGetPowerUsage)(const MtmlDevice *dev, unsigned int *power);
static MtmlReturn (*mtmlDeviceGetProperty)(const MtmlDevice *dev, MtmlDeviceProperty *prop);
static MtmlReturn (*mtmlDeviceCountFan)(const MtmlDevice *dev, unsigned int *count);
static MtmlReturn (*mtmlDeviceGetFanSpeed)(const MtmlDevice *dev, unsigned int index, unsigned int *speed);
static MtmlReturn (*mtmlDeviceGetFanRpm)(const MtmlDevice *dev, unsigned int fanIndex, unsigned int *fanRpm);
static MtmlReturn (*mtmlDeviceGetUUID)(const MtmlDevice *dev, char *uuid, unsigned int length);

// GPU functions
static MtmlReturn (*mtmlDeviceInitGpu)(const MtmlDevice *dev, MtmlGpu **gpu);
static MtmlReturn (*mtmlGpuGetUtilization)(const MtmlGpu *gpu, unsigned int* utilization);
static MtmlReturn (*mtmlGpuGetTemperature)(const MtmlGpu *gpu, int* temp);
static MtmlReturn (*mtmlGpuGetClock)(const MtmlGpu *gpu, unsigned int *clockMhz);
static MtmlReturn (*mtmlGpuGetMaxClock)(const MtmlGpu *gpu, unsigned int *clockMhz);

// Memory functions
static MtmlReturn (*mtmlDeviceInitMemory)(const MtmlDevice *dev, MtmlMemory **mem);
static MtmlReturn (*mtmlMemoryGetTotal)(const MtmlMemory *mem, unsigned long long *total);
static MtmlReturn (*mtmlMemoryGetUsed)(const MtmlMemory *mem, unsigned long long *used);
static MtmlReturn (*mtmlMemoryGetUtilization)(const MtmlMemory *mem, unsigned int *utilization);
static MtmlReturn (*mtmlMemoryGetClock)(const MtmlMemory *mem, unsigned int *clockMhz);
static MtmlReturn (*mtmlMemoryGetMaxClock)(const MtmlMemory *mem, unsigned int *clockMhz);

// Error handling
static const char* (*mtmlErrorString)(MtmlReturn result);

static void *libmtml_handle = NULL;
static MtmlLibrary *mtml_lib = NULL;
static const char *local_error_string = "MTT extraction has not been initialized";

struct gpu_info_mtt {
    struct gpu_info base;
    struct list_head allocate_list;
    
    MtmlDevice *device;
    MtmlGpu *gpu;
    MtmlMemory *memory;
    char device_uuid[48];
    bool initialized;
};

static LIST_HEAD(mtt_allocations);

static bool gpuinfo_mtt_init(void);
static void gpuinfo_mtt_shutdown(void);
static const char *gpuinfo_mtt_last_error_string(void);
static bool gpuinfo_mtt_get_device_handles(struct list_head *devices, unsigned *count);
static void gpuinfo_mtt_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_mtt_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_mtt_get_running_processes(struct gpu_info *_gpu_info);

struct gpu_vendor gpu_vendor_mtt = {
    .init = gpuinfo_mtt_init,
    .shutdown = gpuinfo_mtt_shutdown,
    .last_error_string = gpuinfo_mtt_last_error_string,
    .get_device_handles = gpuinfo_mtt_get_device_handles,
    .populate_static_info = gpuinfo_mtt_populate_static_info,
    .refresh_dynamic_info = gpuinfo_mtt_refresh_dynamic_info,
    .refresh_running_processes = gpuinfo_mtt_get_running_processes,
    .name = "MTT",
};

__attribute__((constructor)) static void init_extract_gpuinfo_mtt(void) { 
    register_gpu_vendor(&gpu_vendor_mtt); 
}

/*
 * Initialize MTT library and load function pointers
 */
static bool gpuinfo_mtt_init(void) {
    libmtml_handle = dlopen("libmtml.so", RTLD_LAZY);
    if (!libmtml_handle) {
        libmtml_handle = dlopen("libmtml.so.1", RTLD_LAZY);
    }
    if (!libmtml_handle) {
        local_error_string = dlerror();
        return false;
    }

    // Load library functions
    mtmlLibraryInit = dlsym(libmtml_handle, "mtmlLibraryInit");
    mtmlLibraryShutDown = dlsym(libmtml_handle, "mtmlLibraryShutDown");
    mtmlLibraryCountDevice = dlsym(libmtml_handle, "mtmlLibraryCountDevice");
    mtmlLibraryInitDeviceByIndex = dlsym(libmtml_handle, "mtmlLibraryInitDeviceByIndex");
    
    // Load device functions
    mtmlDeviceGetName = dlsym(libmtml_handle, "mtmlDeviceGetName");
    mtmlDeviceGetPciInfo = dlsym(libmtml_handle, "mtmlDeviceGetPciInfo");
    mtmlDeviceGetPowerUsage = dlsym(libmtml_handle, "mtmlDeviceGetPowerUsage");
    mtmlDeviceGetProperty = dlsym(libmtml_handle, "mtmlDeviceGetProperty");
    mtmlDeviceCountFan = dlsym(libmtml_handle, "mtmlDeviceCountFan");
    mtmlDeviceGetFanSpeed = dlsym(libmtml_handle, "mtmlDeviceGetFanSpeed");
    mtmlDeviceGetFanRpm = dlsym(libmtml_handle, "mtmlDeviceGetFanRpm");
    mtmlDeviceGetUUID = dlsym(libmtml_handle, "mtmlDeviceGetUUID");
    
    // Load GPU functions
    mtmlDeviceInitGpu = dlsym(libmtml_handle, "mtmlDeviceInitGpu");
    mtmlGpuGetUtilization = dlsym(libmtml_handle, "mtmlGpuGetUtilization");
    mtmlGpuGetTemperature = dlsym(libmtml_handle, "mtmlGpuGetTemperature");
    mtmlGpuGetClock = dlsym(libmtml_handle, "mtmlGpuGetClock");
    mtmlGpuGetMaxClock = dlsym(libmtml_handle, "mtmlGpuGetMaxClock");
    
    // Load memory functions
    mtmlDeviceInitMemory = dlsym(libmtml_handle, "mtmlDeviceInitMemory");
    mtmlMemoryGetTotal = dlsym(libmtml_handle, "mtmlMemoryGetTotal");
    mtmlMemoryGetUsed = dlsym(libmtml_handle, "mtmlMemoryGetUsed");
    mtmlMemoryGetUtilization = dlsym(libmtml_handle, "mtmlMemoryGetUtilization");
    mtmlMemoryGetClock = dlsym(libmtml_handle, "mtmlMemoryGetClock");
    mtmlMemoryGetMaxClock = dlsym(libmtml_handle, "mtmlMemoryGetMaxClock");
    
    // Load error function
    mtmlErrorString = dlsym(libmtml_handle, "mtmlErrorString");
    
    // Check all required functions
    if (!mtmlLibraryInit || !mtmlLibraryShutDown || !mtmlLibraryCountDevice || 
        !mtmlLibraryInitDeviceByIndex || !mtmlDeviceGetName || !mtmlDeviceGetPciInfo ||
        !mtmlDeviceInitGpu || !mtmlGpuGetUtilization || !mtmlGpuGetTemperature ||
        !mtmlDeviceInitMemory || !mtmlMemoryGetTotal || !mtmlMemoryGetUsed ||
        !mtmlErrorString) {
        local_error_string = "Failed to load required MTT functions";
        dlclose(libmtml_handle);
        libmtml_handle = NULL;
        return false;
    }
    
    // Initialize MTT library
    MtmlReturn ret = mtmlLibraryInit(&mtml_lib);
    if (ret != MTML_SUCCESS) {
        local_error_string = mtmlErrorString ? mtmlErrorString(ret) : "Failed to initialize MTT library";
        dlclose(libmtml_handle);
        libmtml_handle = NULL;
        return false;
    }
    
    return true;
}

/*
 * Shutdown MTT library and cleanup
 */
static void gpuinfo_mtt_shutdown(void) {
    if (mtml_lib) {
        mtmlLibraryShutDown(mtml_lib);
        mtml_lib = NULL;
    }
    
    if (libmtml_handle) {
        dlclose(libmtml_handle);
        libmtml_handle = NULL;
    }
    
    // Free any allocated GPU info structures
    struct gpu_info_mtt *mtt_info, *tmp;
    list_for_each_entry_safe(mtt_info, tmp, &mtt_allocations, allocate_list) {
        list_del(&mtt_info->allocate_list);
        free(mtt_info);
    }
}

/*
 * Get last error string
 */
static const char *gpuinfo_mtt_last_error_string(void) {
    return local_error_string;
}

/*
 * Get device handles for all MTT GPUs
 */
static bool gpuinfo_mtt_get_device_handles(struct list_head *devices, unsigned *count) {
    if (!mtml_lib) {
        local_error_string = "MTT library not initialized";
        return false;
    }
    
    unsigned int device_count = 0;
    MtmlReturn ret = mtmlLibraryCountDevice(mtml_lib, &device_count);
    if (ret != MTML_SUCCESS) {
        local_error_string = mtmlErrorString ? mtmlErrorString(ret) : "Failed to get MTT device count";
        return false;
    }
    
    if (device_count == 0) {
        *count = 0;
        return true;
    }
    
    unsigned added_count = 0;
    for (unsigned int i = 0; i < device_count; i++) {
        MtmlDevice *device = NULL;
        ret = mtmlLibraryInitDeviceByIndex(mtml_lib, i, &device);
        if (ret != MTML_SUCCESS) {
            continue; // Skip devices that fail to initialize
        }
        
        // Check if device is a GPU (not a virtual device or other type)
        MtmlDeviceProperty prop;
        ret = mtmlDeviceGetProperty(device, &prop);
        if (ret != MTML_SUCCESS) {
            // Skip devices that we can't get properties for
            continue;
        }
        
        // Create GPU info structure
        struct gpu_info_mtt *mtt_info = calloc(1, sizeof(struct gpu_info_mtt));
        if (!mtt_info) {
            local_error_string = "Failed to allocate memory for MTT GPU info";
            return false;
        }
        
        mtt_info->base.vendor = &gpu_vendor_mtt;
        mtt_info->device = device;
        mtt_info->initialized = false;
        
        // Get device UUID for identification
        if (mtmlDeviceGetUUID) {
            mtmlDeviceGetUUID(device, mtt_info->device_uuid, sizeof(mtt_info->device_uuid));
        }
        
        // Initialize GPU and Memory handles
        if (mtmlDeviceInitGpu) {
            ret = mtmlDeviceInitGpu(device, &mtt_info->gpu);
            if (ret != MTML_SUCCESS) {
                // GPU initialization failed, but we can still try memory
                mtt_info->gpu = NULL;
            }
        }
        
        if (mtmlDeviceInitMemory) {
            ret = mtmlDeviceInitMemory(device, &mtt_info->memory);
            if (ret != MTML_SUCCESS) {
                mtt_info->memory = NULL;
            }
        }
        
        // Add to allocation list for cleanup
        list_add(&mtt_info->allocate_list, &mtt_allocations);
        
        // Add to devices list
        list_add_tail(&mtt_info->base.list, devices);
        added_count++;
    }
    
    *count = added_count;
    return true;
}

/*
 * Populate static information for a GPU
 */
static void gpuinfo_mtt_populate_static_info(struct gpu_info *_gpu_info) {
    struct gpu_info_mtt *mtt_info = (struct gpu_info_mtt *)_gpu_info;
    
    if (!mtt_info->device) {
        return;
    }
    
    // Get device name
    char device_name[32] = {0};
    MtmlReturn ret = mtmlDeviceGetName(mtt_info->device, device_name, sizeof(device_name));
    if (ret == MTML_SUCCESS) {
        snprintf(_gpu_info->static_info.device_name, MAX_DEVICE_NAME, "%s", device_name);
        SET_VALID(gpuinfo_device_name_valid, _gpu_info->static_info.valid);
    }
    
    // Get PCIe information
    MtmlPciInfo pci_info;
    ret = mtmlDeviceGetPciInfo(mtt_info->device, &pci_info);
    if (ret == MTML_SUCCESS) {
        // Store PCIe information in pdev field for device identification
        strncpy(_gpu_info->pdev, pci_info.sbdf, PDEV_LEN - 1);
        _gpu_info->pdev[PDEV_LEN - 1] = '\0';
        
        // Set PCIe generation and width
        _gpu_info->static_info.max_pcie_gen = pci_info.pciMaxGen;
        SET_VALID(gpuinfo_max_pcie_gen_valid, _gpu_info->static_info.valid);
        
        _gpu_info->static_info.max_pcie_link_width = pci_info.pciMaxWidth;
        SET_VALID(gpuinfo_max_pcie_link_width_valid, _gpu_info->static_info.valid);
    }
    
    // Get memory information
    if (mtt_info->memory && mtmlMemoryGetTotal) {
        unsigned long long total_memory = 0;
        ret = mtmlMemoryGetTotal(mtt_info->memory, &total_memory);
        if (ret == MTML_SUCCESS) {
            // Note: total_memory is in bytes, we store it in dynamic info
            // Static info doesn't have memory size field
        }
    }
    
    // Set integrated graphics flag (MTT GPUs are typically discrete)
    _gpu_info->static_info.integrated_graphics = false;
    
    // Set encode/decode shared flag (unknown for MTT, assume shared)
    _gpu_info->static_info.encode_decode_shared = true;
    
    mtt_info->initialized = true;
}

/*
 * Refresh dynamic information for a GPU
 */
static void gpuinfo_mtt_refresh_dynamic_info(struct gpu_info *_gpu_info) {
    struct gpu_info_mtt *mtt_info = (struct gpu_info_mtt *)_gpu_info;
    
    if (!mtt_info->device || !mtt_info->initialized) {
        return;
    }
    
    // Get GPU utilization
    if (mtt_info->gpu && mtmlGpuGetUtilization) {
        unsigned int gpu_util = 0;
        MtmlReturn ret = mtmlGpuGetUtilization(mtt_info->gpu, &gpu_util);
        if (ret == MTML_SUCCESS) {
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, gpu_util_rate, gpu_util);
        }
    }
    
    // Get GPU temperature
    if (mtt_info->gpu && mtmlGpuGetTemperature) {
        int temp = 0;
        MtmlReturn ret = mtmlGpuGetTemperature(mtt_info->gpu, &temp);
        if (ret == MTML_SUCCESS) {
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, gpu_temp, (unsigned int)temp);
        }
    }
    
    // Get GPU clock speed
    if (mtt_info->gpu && mtmlGpuGetClock) {
        unsigned int clock_mhz = 0;
        MtmlReturn ret = mtmlGpuGetClock(mtt_info->gpu, &clock_mhz);
        if (ret == MTML_SUCCESS) {
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, gpu_clock_speed, clock_mhz);
        }
    }
    
    // Get GPU max clock speed
    if (mtt_info->gpu && mtmlGpuGetMaxClock) {
        unsigned int max_clock_mhz = 0;
        MtmlReturn ret = mtmlGpuGetMaxClock(mtt_info->gpu, &max_clock_mhz);
        if (ret == MTML_SUCCESS) {
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, gpu_clock_speed_max, max_clock_mhz);
        }
    }
    
    // Get memory information
    if (mtt_info->memory) {
        unsigned long long total_memory = 0;
        unsigned long long used_memory = 0;
        unsigned int mem_util = 0;
        
        if (mtmlMemoryGetTotal) {
            MtmlReturn ret = mtmlMemoryGetTotal(mtt_info->memory, &total_memory);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, total_memory, total_memory);
            }
        }
        
        if (mtmlMemoryGetUsed) {
            MtmlReturn ret = mtmlMemoryGetUsed(mtt_info->memory, &used_memory);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, used_memory, used_memory);
                if (total_memory > 0) {
                    unsigned long long free_memory = total_memory - used_memory;
                    SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, free_memory, free_memory);
                }
            }
        }
        
        if (mtmlMemoryGetUtilization) {
            MtmlReturn ret = mtmlMemoryGetUtilization(mtt_info->memory, &mem_util);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, mem_util_rate, mem_util);
            }
        }
        
        // Get memory clock speed
        if (mtmlMemoryGetClock) {
            unsigned int mem_clock_mhz = 0;
            MtmlReturn ret = mtmlMemoryGetClock(mtt_info->memory, &mem_clock_mhz);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, mem_clock_speed, mem_clock_mhz);
            }
        }
        
        // Get memory max clock speed
        if (mtmlMemoryGetMaxClock) {
            unsigned int mem_max_clock_mhz = 0;
            MtmlReturn ret = mtmlMemoryGetMaxClock(mtt_info->memory, &mem_max_clock_mhz);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, mem_clock_speed_max, mem_max_clock_mhz);
            }
        }
    }
    
    // Get power usage
    if (mtmlDeviceGetPowerUsage) {
        unsigned int power_mw = 0;
        MtmlReturn ret = mtmlDeviceGetPowerUsage(mtt_info->device, &power_mw);
        if (ret == MTML_SUCCESS) {
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, power_draw, power_mw);
        }
    }
    
    // Get fan information
    if (mtmlDeviceCountFan && mtmlDeviceGetFanSpeed) {
        unsigned int fan_count = 0;
        MtmlReturn ret = mtmlDeviceCountFan(mtt_info->device, &fan_count);
        if (ret == MTML_SUCCESS && fan_count > 0) {
            // Get speed of first fan
            unsigned int fan_speed = 0;
            ret = mtmlDeviceGetFanSpeed(mtt_info->device, 0, &fan_speed);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, fan_speed, fan_speed);
            }
        }
    }
    
    // Get fan RPM if available
    if (mtmlDeviceCountFan && mtmlDeviceGetFanRpm) {
        unsigned int fan_count = 0;
        MtmlReturn ret = mtmlDeviceCountFan(mtt_info->device, &fan_count);
        if (ret == MTML_SUCCESS && fan_count > 0) {
            unsigned int fan_rpm = 0;
            ret = mtmlDeviceGetFanRpm(mtt_info->device, 0, &fan_rpm);
            if (ret == MTML_SUCCESS) {
                SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, fan_rpm, fan_rpm);
            }
        }
    }
    
    // Get PCIe information for current link status
    MtmlPciInfo pci_info;
    if (mtmlDeviceGetPciInfo) {
        MtmlReturn ret = mtmlDeviceGetPciInfo(mtt_info->device, &pci_info);
        if (ret == MTML_SUCCESS) {
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, pcie_link_gen, pci_info.pciCurGen);
            SET_GPUINFO_DYNAMIC(&_gpu_info->dynamic_info, pcie_link_width, pci_info.pciCurWidth);
        }
    }
}

/*
 * Get running processes on GPU
 * Note: MTT API doesn't seem to have direct process query functions,
 * so we'll use the standard DRM fdinfo method like other GPU vendors
 */
static void gpuinfo_mtt_get_running_processes(struct gpu_info *_gpu_info) {
    // MTT doesn't provide process information through its API,
    // so we use the standard DRM fdinfo parsing like other vendors
    // This will be handled by the common extraction code
    
    // Clear existing processes
    _gpu_info->processes_count = 0;
    
    // The actual process extraction will be done by the common code
    // that reads /proc/<pid>/fdinfo files
}
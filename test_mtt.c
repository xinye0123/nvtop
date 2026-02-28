#include <stdio.h>
#include <dlfcn.h>
#include <stdlib.h>

// MTT API function pointers
typedef void* MtmlLibrary;
typedef void* MtmlDevice;

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

typedef MtmlReturn (*mtmlLibraryInit_t)(MtmlLibrary **lib);
typedef MtmlReturn (*mtmlLibraryShutDown_t)(MtmlLibrary *lib);
typedef MtmlReturn (*mtmlLibraryCountDevice_t)(const MtmlLibrary *lib, unsigned int *count);
typedef MtmlReturn (*mtmlLibraryInitDeviceByIndex_t)(const MtmlLibrary *lib, unsigned int index, MtmlDevice **dev);
typedef const char* (*mtmlErrorString_t)(MtmlReturn result);

int main() {
    void *libmtml_handle = dlopen("libmtml.so", RTLD_LAZY);
    if (!libmtml_handle) {
        libmtml_handle = dlopen("libmtml.so.1", RTLD_LAZY);
    }
    
    if (!libmtml_handle) {
        printf("Failed to load libmtml.so: %s\n", dlerror());
        return 1;
    }
    
    printf("Successfully loaded libmtml.so\n");
    
    // Load functions
    mtmlLibraryInit_t mtmlLibraryInit = dlsym(libmtml_handle, "mtmlLibraryInit");
    mtmlLibraryShutDown_t mtmlLibraryShutDown = dlsym(libmtml_handle, "mtmlLibraryShutDown");
    mtmlLibraryCountDevice_t mtmlLibraryCountDevice = dlsym(libmtml_handle, "mtmlLibraryCountDevice");
    mtmlLibraryInitDeviceByIndex_t mtmlLibraryInitDeviceByIndex = dlsym(libmtml_handle, "mtmlLibraryInitDeviceByIndex");
    mtmlErrorString_t mtmlErrorString = dlsym(libmtml_handle, "mtmlErrorString");
    
    if (!mtmlLibraryInit || !mtmlLibraryShutDown || !mtmlLibraryCountDevice || !mtmlLibraryInitDeviceByIndex) {
        printf("Failed to load required MTT functions\n");
        dlclose(libmtml_handle);
        return 1;
    }
    
    printf("Successfully loaded MTT API functions\n");
    
    // Initialize library
    MtmlLibrary *mtml_lib = NULL;
    MtmlReturn ret = mtmlLibraryInit(&mtml_lib);
    
    if (ret != MTML_SUCCESS) {
        const char *error_msg = mtmlErrorString ? mtmlErrorString(ret) : "Unknown error";
        printf("Failed to initialize MTT library: %d (%s)\n", ret, error_msg);
        dlclose(libmtml_handle);
        return 1;
    }
    
    printf("Successfully initialized MTT library\n");
    
    // Get device count
    unsigned int device_count = 0;
    ret = mtmlLibraryCountDevice(mtml_lib, &device_count);
    
    if (ret != MTML_SUCCESS) {
        const char *error_msg = mtmlErrorString ? mtmlErrorString(ret) : "Unknown error";
        printf("Failed to get device count: %d (%s)\n", ret, error_msg);
    } else {
        printf("Found %u MTT devices\n", device_count);
        
        // Try to initialize each device
        for (unsigned int i = 0; i < device_count; i++) {
            MtmlDevice *device = NULL;
            ret = mtmlLibraryInitDeviceByIndex(mtml_lib, i, &device);
            
            if (ret == MTML_SUCCESS) {
                printf("  Device %u: Successfully initialized\n", i);
                // Note: In real code we would free the device here
            } else {
                const char *error_msg = mtmlErrorString ? mtmlErrorString(ret) : "Unknown error";
                printf("  Device %u: Failed to initialize: %d (%s)\n", i, ret, error_msg);
            }
        }
    }
    
    // Shutdown library
    mtmlLibraryShutDown(mtml_lib);
    dlclose(libmtml_handle);
    
    printf("MTT test completed\n");
    return 0;
}
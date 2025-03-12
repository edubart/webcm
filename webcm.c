#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "cartesi-machine/machine-c-api.h"
#include <emscripten.h>

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_EXPORT static
#include "third-party/miniz.h"
#include "third-party/miniz.c"

#define RAM_SIZE 128*1024*1024
#define ROOTFS_SIZE 128*1024*1024
#define RAM_START UINT64_C(0x80000000)
#define ROOTFS_START UINT64_C(0x80000000000000)

static uint8_t linux_bin_zz[] = {
    #embed "linux.bin.zz"
};

static uint8_t rootfs_ext2_zz[] = {
    #embed "rootfs.ext2.zz"
};


typedef struct uncompress_env {
    cm_machine *machine;
    uint64_t offset;
} uncompress_env;

int uncompress_cb(uint8_t *data, int size, uncompress_env *env) {
    if (cm_write_memory(env->machine, env->offset, data, size) != CM_ERROR_OK) {
        printf("failed to write machine memory: %s\n", cm_get_last_error_message());
        exit(1);
    }
    env->offset += size;
    return 1;
}

uint64_t uncompress_memory(cm_machine *machine, uint64_t paddr, uint8_t *data, uint64_t size) {
    uncompress_env env = {machine, paddr};
    size_t uncompressed_size = size;
    if (tinfl_decompress_mem_to_callback(data, &uncompressed_size, (tinfl_put_buf_func_ptr)uncompress_cb, &env, TINFL_FLAG_PARSE_ZLIB_HEADER) != 1) {
        printf("failed to uncompress memory\n");
        exit(1);
    }
    return uncompressed_size;
}

int main() {
    printf("Allocating...\n");

    // Set machine configuration
    unsigned long long now = (unsigned long long)time(NULL);
    char config[4096];
    snprintf(config, sizeof(config), "{\
        \"dtb\": {\
            \"bootargs\": \"quiet earlycon=sbi console=hvc1 root=/dev/pmem0 rw init=/usr/sbin/cartesi-init\",\
            \"init\": \"date -s @%llu >> /dev/null\",\
            \"entrypoint\": \"exec ash -l\"\
        },\
        \"flash_drive\": [\
            {\"length\": %u}\
        ],\
        \"virtio\": [\
            {\"type\": \"console\"}\
        ],\
        \"processor\": {\
            \"iunrep\": 1\
        },\
        \"ram\": {\"length\": %u}\
    }", now, RAM_SIZE, ROOTFS_SIZE);

    // Create a new machine
    cm_machine *machine = NULL;
    if (cm_create_new(config, NULL, &machine) != CM_ERROR_OK) {
        printf("failed to create machine: %s\n", cm_get_last_error_message());
        exit(1);
    }

    printf("Decompressing...\n");

    // Decompress kernel and rootfs
    uncompress_memory(machine, RAM_START, linux_bin_zz, sizeof(linux_bin_zz));
    uncompress_memory(machine, ROOTFS_START, rootfs_ext2_zz, sizeof(rootfs_ext2_zz));

    printf("Booting...\n");

    // Run the machine
    cm_break_reason break_reason;
    do {
        uint64_t mcycle;
        if (cm_read_reg(machine, CM_REG_MCYCLE, &mcycle) != CM_ERROR_OK) {
            printf("failed to read machine cycle: %s\n", cm_get_last_error_message());
            cm_delete(machine);
            exit(1);
        }
        if (cm_run(machine, mcycle + 4*1024*1024, &break_reason) != CM_ERROR_OK) {
            printf("failed to run machine: %s\n", cm_get_last_error_message());
            cm_delete(machine);
            exit(1);
        }
        emscripten_sleep(0);
    } while(break_reason == CM_BREAK_REASON_REACHED_TARGET_MCYCLE);

    // Print reason for run interruption
    switch (break_reason) {
        case CM_BREAK_REASON_HALTED:
        printf("Halted\n");
        break;
        case CM_BREAK_REASON_YIELDED_MANUALLY:
        printf("Yielded manually\n");
        break;
        case CM_BREAK_REASON_YIELDED_AUTOMATICALLY:
        printf("Yielded automatically\n");
        break;
        case CM_BREAK_REASON_YIELDED_SOFTLY:
        printf("Yielded softly\n");
        break;
        case CM_BREAK_REASON_REACHED_TARGET_MCYCLE:
        printf("Reached target machine cycle\n");
        break;
        case CM_BREAK_REASON_FAILED:
        default:
        printf("Interpreter failed\n");
        break;
    }

    // Read and print machine cycles
    uint64_t mcycle;
    if (cm_read_reg(machine, CM_REG_MCYCLE, &mcycle) != CM_ERROR_OK) {
        printf("failed to read machine cycle: %s\n", cm_get_last_error_message());
        cm_delete(machine);
        exit(1);
    }
    printf("Cycles: %lu\n", (unsigned long)mcycle);

    // Cleanup and exit
    cm_delete(machine);
    return 0;
}

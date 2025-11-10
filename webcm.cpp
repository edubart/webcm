#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <ctime>
#include <unordered_map>
#include <vector>
#include <string>
#include <sstream>
#include <memory>

#include "cartesi-machine/machine-c-api.h"
#include <emscripten.h>
#include <emscripten/fetch.h>

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_EXPORT static
#include "third-party/miniz.h"
#include "third-party/miniz.c"

#define RAM_SIZE (UINT64_C(128)*1024*1024)
#define ROOTFS_SIZE (UINT64_C(256)*1024*1024)
#define RAM_START UINT64_C(0x80000000)
#define ROOTFS_START UINT64_C(0x80000000000000)

extern "C" {
static uint8_t linux_bin_zz[] = {
    #embed "linux.bin.zz"
};

static uint8_t rootfs_ext2_zz[] = {
    #embed "rootfs.ext2.zz"
};
}

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

enum class yield_type : uint64_t {
    INVALID = 0,
    REQUEST,
    POLL_RESPONSE,
    POLL_RESPONSE_BODY,
};

struct yield_mmio_req final {
    uint64_t headers_count{0};
    uint64_t body_vaddr{0};
    uint64_t body_length{0};
    char url[4096]{};
    char method[32]{};
    char headers[64][2][256]{};
};

struct yield_mmio_res final {
    uint64_t ready_state{0};
    uint64_t status{0};
    uint64_t body_total_length{0};
    uint64_t headers_count{0};
    char headers[64][2][256]{};
};

struct fetch_object final {
    uint64_t uid{0};
    emscripten_fetch_t *fetch{nullptr};
    std::string body;
    bool done{false};
};

static std::unordered_map<uint64_t, std::unique_ptr<fetch_object>> fetches;

template <size_t N>
static void strsvcopy(char (&dest)[N], std::string_view sv) {
    memcpy(dest, sv.data(), std::min(sv.length(), N));
    dest[std::min(sv.length(), N - 1)] = 0;
}

static void on_fetch_success(emscripten_fetch_t *fetch) {
    fetch_object *o = reinterpret_cast<fetch_object*>(fetch->userData);
    o->done = true;
}

static void on_fetch_error(emscripten_fetch_t *fetch) {
    fetch_object *o = reinterpret_cast<fetch_object*>(fetch->userData);
    o->done = true;
}

bool handle_softyield(cm_machine *machine) {
    uint64_t type = 0;
    uint64_t uid = 0;
    uint64_t vaddr = 0;
    cm_read_reg(machine, CM_REG_X10, &type); // a0
    cm_read_reg(machine, CM_REG_X11, &uid); // a1
    cm_read_reg(machine, CM_REG_X12, &vaddr); // a2

    switch (static_cast<yield_type>(type)) {
        case yield_type::REQUEST: {
            // Read request data
            yield_mmio_req mmio_req;
            if (cm_read_virtual_memory(machine, vaddr, (uint8_t*)(&mmio_req), sizeof(mmio_req)) != 0) {
                printf("failed to read virtual memory: %s\n", cm_get_last_error_message());
                return false;
            }

            if (fetches.find(uid) != fetches.end()) {
                return true;
            }

            // Set headers
            std::vector<const char*> headers;
            for (uint64_t i = 0; i < mmio_req.headers_count; i++) {
                headers.push_back(mmio_req.headers[i][0]);
                headers.push_back(mmio_req.headers[i][1]);
            }
            headers.push_back(nullptr);

            // Set fetch attributes
            auto o = std::make_unique<fetch_object>();
            emscripten_fetch_attr_t attr;
            emscripten_fetch_attr_init(&attr);
            attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
            attr.timeoutMSecs = 0;
            attr.requestHeaders = headers.data();
            attr.onsuccess = on_fetch_success;
            attr.onerror = on_fetch_error;
            attr.userData = reinterpret_cast<void*>(o.get());
            if (mmio_req.body_length > 0) {
                o->body.resize(mmio_req.body_length);
                // Write attr.requestData by reading mmio_req.body_vaddr from machine memory
                if (cm_read_virtual_memory(machine, mmio_req.body_vaddr, reinterpret_cast<uint8_t*>(o->body.data()), mmio_req.body_length) != 0) {
                    printf("failed to read virtual memory: %s\n", cm_get_last_error_message());
                    return false;
                }
                attr.requestData = reinterpret_cast<const char*>(o->body.data());
                attr.requestDataSize = o->body.size();
            }
            strcpy(attr.requestMethod, mmio_req.method);

            // Initiate fetch
            o->fetch = emscripten_fetch(&attr, mmio_req.url);
            fetches[uid] = std::move(o);
            break;
        }
        case yield_type::POLL_RESPONSE: {
            // Retrieve fetch
            auto it = fetches.find(uid);
            if (it == fetches.end()) {
                printf("failed to retrieve fetch\n");
                return true;
            }
            auto& o = it->second;
            emscripten_fetch_t *fetch = o->fetch;

            // Wait fetch to complete
            while (!o->done) {
                emscripten_sleep(4);
            }

            // Set response
            yield_mmio_res mmio_res;
            mmio_res.ready_state = fetch->readyState;
            mmio_res.status = fetch->status;
            mmio_res.body_total_length = fetch->totalBytes;

            // Set response headers
            std::string headers_str(emscripten_fetch_get_response_headers_length(fetch) + 1, '\x0');
            emscripten_fetch_get_response_headers(fetch, headers_str.data(), headers_str.size());
            mmio_res.headers_count = 0;
            for (size_t pos = 0; mmio_res.headers_count < 64; ) {
                const size_t end = headers_str.find('\n', pos);
                if (end == std::string::npos || end == pos) {
                    break;
                }
                std::string_view line(headers_str.data() + pos, end - pos);
                if (line.back() == '\r') {
                    line.remove_suffix(1);
                }
                const auto colon_pos = line.find(": ");
                if (colon_pos != std::string_view::npos) {
                    strsvcopy(mmio_res.headers[mmio_res.headers_count][0], line.substr(0, colon_pos));
                    strsvcopy(mmio_res.headers[mmio_res.headers_count][1], line.substr(colon_pos + 2));
                    mmio_res.headers_count++;
                }
                pos = end + 1;
            }

            // Write response
            if (cm_write_virtual_memory(machine, vaddr, (uint8_t*)(&mmio_res), sizeof(mmio_res)) != 0) {
                printf("failed to write virtual memory: %s\n", cm_get_last_error_message());
                return false;
            }

            // Free
            if (mmio_res.body_total_length == 0) {
                emscripten_fetch_close(fetch);
                fetches.erase(it);
            }
            break;
        }
        case yield_type::POLL_RESPONSE_BODY: {
            // Retrieve fetch
            auto it = fetches.find(uid);
            if (it == fetches.end()) {
                printf("failed to retrieve fetch\n");
                return true;
            }
            auto& o = it->second;
            emscripten_fetch_t *fetch = o->fetch;

            // Write body
            if (cm_write_virtual_memory(machine, vaddr, (uint8_t*)(fetch->data), fetch->totalBytes) != 0) {
                printf("failed to write virtual memory: %s\n", cm_get_last_error_message());
                return false;
            }

            // Free
            emscripten_fetch_close(fetch);
            fetches.erase(it);
            break;
        }
        default:
            printf("invalid yield type\n");
            return false;
    }

    // Success
    cm_write_reg(machine, CM_REG_X10, 0); // ret a0
    return true;
}

int main() {
    printf("Allocating...\n");

    // Set machine configuration
    unsigned long long now = (unsigned long long)time(NULL);
    char config[4096];
    snprintf(config, sizeof(config), R"({
        "dtb": {
            "bootargs": "quiet earlycon=sbi console=hvc1 root=/dev/pmem0 rw init=/usr/sbin/cartesi-init",
            "init": "date -s @%llu >> /dev/null && dnsmasq --address=/#/127.0.0.1 --local=/#/ --no-resolv && https-proxy 127.0.0.1 80 443 > /dev/null 2>&1 &",
            "entrypoint": "exec ash -l"
        },
        "ram": {"length": %llu},
        "flash_drive": [
            {"length": %llu}
        ],
        "virtio": [
            {"type": "console"}
        ],
        "processor": {
            "iunrep": 1
        }
    })", now, static_cast<unsigned long long>(RAM_SIZE), static_cast<unsigned long long>(ROOTFS_SIZE));

    const char runtime_config[] = R"({
        "soft_yield": true
    })";

    // Create a new machine
    cm_machine *machine = NULL;
    if (cm_create_new(config, runtime_config, &machine) != CM_ERROR_OK) {
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
        if (break_reason == CM_BREAK_REASON_YIELDED_SOFTLY) {
            if (!handle_softyield(machine)) {
                printf("failed to handle soft yield!\n");
                cm_delete(machine);
                exit(1);
            }
        }
        emscripten_sleep(0);
    } while(break_reason == CM_BREAK_REASON_REACHED_TARGET_MCYCLE || break_reason == CM_BREAK_REASON_YIELDED_SOFTLY);

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

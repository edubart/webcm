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
#include <queue>
#include <functional>

#include "cartesi-machine/machine-c-api.h"
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/em_js.h>

#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_EXPORT static
#include "third-party/miniz.h"
#include "third-party/miniz.c"

#define RAM_SIZE (UINT64_C(256)*1024*1024)
#define ROOTFS_SIZE (UINT64_C(384)*1024*1024)
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
    POLL_REVERSE_REQUEST,
    POLL_REVERSE_REQUEST_BODY,
    SEND_REVERSE_RESPONSE,
    SEND_REVERSE_RESPONSE_BODY,
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

struct reverse_mmio_req final {
    uint64_t uid{0};
    uint64_t headers_count{0};
    uint64_t body_vaddr{0};
    uint64_t body_length{0};
    char url[4096]{};
    char method[32]{};
    char headers[64][2][256]{};
};

struct reverse_mmio_res final {
    uint64_t uid{0};
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

struct reverse_request final {
    uint64_t uid{0};
    std::string url;
    std::string method;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

struct reverse_response final {
    uint64_t status{0};
    uint64_t body_length{0};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    bool ready{false};
};

static std::unordered_map<uint64_t, std::unique_ptr<fetch_object>> fetches;
static std::queue<std::unique_ptr<reverse_request>> reverse_request_queue;
static std::unordered_map<uint64_t, std::unique_ptr<reverse_request>> pending_reverse_requests;
static std::unordered_map<uint64_t, std::unique_ptr<reverse_response>> reverse_responses;
static uint64_t next_reverse_uid = 1;

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

// Copy request body from JavaScript temporary buffer to C++ memory
EM_JS(void, js_copy_body_data, (char* dest, uint32_t len, uint32_t js_body_ref), {
    if (len > 0 && dest && Module._tempBodyData) {
        const bodyData = Module._tempBodyData;
        if (bodyData && bodyData.length >= len) {
            HEAPU8.set(bodyData.subarray(0, len), dest);
        }
        Module._tempBodyData = null;
    }
});

// Create JavaScript Response object and store in pending map for polling
EM_JS(void, js_create_response, (uint64_t uid, int status, const char* body_ptr, uint32_t body_len, const char* headers_json), {
    if (!Module._pendingResponses) {
        Module._pendingResponses = new Map();
    }

    let bodyData = null;
    if (body_len > 0 && body_ptr) {
        bodyData = new Uint8Array(HEAPU8.buffer, body_ptr, body_len).slice();
    }

    const headers = new Headers();
    if (headers_json) {
        const headersStr = UTF8ToString(headers_json);
        const lines = headersStr.split('\n');
        for (const line of lines) {
            const colonPos = line.indexOf(':');
            if (colonPos > 0) {
                const key = line.substring(0, colonPos).trim();
                const value = line.substring(colonPos + 1).trim();
                if (key) {
                    headers.append(key, value);
                }
            }
        }
    }

    Module._pendingResponses.set(Number(uid), {
        status: status,
        body: bodyData,
        headers: headers,
        ready: true
    });
});

// Parse "Key: Value\nKey: Value" format into header vector
static void parse_headers(const char* headers_json, std::vector<std::pair<std::string, std::string>>& headers) {
    if (!headers_json) return;

    std::istringstream iss(headers_json);
    std::string line;
    while (std::getline(iss, line)) {
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            while (!key.empty() && (key[0] == ' ' || key[0] == '\t')) key.erase(0, 1);
            while (!key.empty() && (key[key.length()-1] == ' ' || key[key.length()-1] == '\t')) key.erase(key.length()-1);
            while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) value.erase(0, 1);
            while (!value.empty() && (value[value.length()-1] == ' ' || value[value.length()-1] == '\t')) value.erase(value.length()-1);
            if (!key.empty()) {
                headers.emplace_back(key, value);
            }
        }
    }
}

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    void cancel_reverse_request(uint64_t uid) {
        pending_reverse_requests.erase(uid);
        reverse_responses.erase(uid);

        // Remove from queue (rebuild queue without this uid)
        std::queue<std::unique_ptr<reverse_request>> new_queue;
        while (!reverse_request_queue.empty()) {
            auto req = std::move(reverse_request_queue.front());
            reverse_request_queue.pop();
            if (req->uid != uid) {
                new_queue.push(std::move(req));
            }
        }
        reverse_request_queue = std::move(new_queue);
    }

    // Queue reverse proxy request from JavaScript (returns unique id for polling)
    EMSCRIPTEN_KEEPALIVE
    uint64_t vmFetch(const char* url, const char* method, const char* headers_json, uint32_t body_len) {
        auto req = std::make_unique<reverse_request>();
        req->uid = next_reverse_uid++;
        req->url = url ? url : "";
        req->method = method ? method : "GET";

        if (body_len > 0) {
            req->body.resize(body_len);
            js_copy_body_data(req->body.data(), body_len, 0);
        }

        parse_headers(headers_json, req->headers);

        uint64_t uid = req->uid;
        reverse_request_queue.push(std::move(req));
        return uid;
    }

    // Check if response ready and transfer to JavaScript (returns 1 if ready, 0 if not)
    EMSCRIPTEN_KEEPALIVE
    int prepare_reverse_response(uint64_t uid) {
        auto it = reverse_responses.find(uid);
        if (it == reverse_responses.end() || !it->second->ready) {
            return 0;
        }

        const auto& resp = it->second;

        std::string headers_str;
        for (const auto& [key, value] : resp->headers) {
            headers_str += key + ": " + value + "\n";
        }

        js_create_response(
            uid,
            static_cast<int>(resp->status),
            resp->body.empty() ? nullptr : resp->body.data(),
            static_cast<uint32_t>(resp->body.length()),
            headers_str.empty() ? nullptr : headers_str.c_str()
        );

        reverse_responses.erase(it);
        return 1;
    }
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
        case yield_type::POLL_REVERSE_REQUEST: {
            // Check if there's a pending reverse request
            if (reverse_request_queue.empty()) {
                // No request available, return empty request
                reverse_mmio_req empty_req{};
                if (cm_write_virtual_memory(machine, vaddr, (uint8_t*)(&empty_req), sizeof(empty_req)) != 0) {
                    printf("failed to write virtual memory: %s\n", cm_get_last_error_message());
                    return false;
                }
                return true;
            }

            // Get the next request from queue
            auto req = std::move(reverse_request_queue.front());
            reverse_request_queue.pop();

            // Convert to MMIO format (similar to fill_mmio_req in forward proxy)
            reverse_mmio_req mmio_req{};
            mmio_req.uid = req->uid;
            strsvcopy(mmio_req.url, req->url);
            strsvcopy(mmio_req.method, req->method);
            mmio_req.headers_count = 0;
            for (const auto& header : req->headers) {
                if (mmio_req.headers_count >= 64) {
                    break;
                }
                strsvcopy(mmio_req.headers[mmio_req.headers_count][0], header.first);
                strsvcopy(mmio_req.headers[mmio_req.headers_count][1], header.second);
                mmio_req.headers_count++;
            }
            // Store body length (body will be retrieved separately via POLL_REVERSE_REQUEST_BODY)
            mmio_req.body_length = req->body.length();
            mmio_req.body_vaddr = 0; // Body will be written to VM memory in separate call

            // Write request to VM memory
            if (cm_write_virtual_memory(machine, vaddr, (uint8_t*)(&mmio_req), sizeof(mmio_req)) != 0) {
                printf("failed to write virtual memory: %s\n", cm_get_last_error_message());
                return false;
            }

            // Store the request for body retrieval and response callback
            pending_reverse_requests[req->uid] = std::move(req);
            break;
        }
        case yield_type::POLL_REVERSE_REQUEST_BODY: {
            // Retrieve request and write body to VM memory
            auto it = pending_reverse_requests.find(uid);
            if (it == pending_reverse_requests.end()) {
                printf("failed to retrieve reverse request\n");
                return true;
            }
            auto& req = it->second;

            // Write body to VM memory at the specified address
            if (req->body.length() > 0) {
                if (cm_write_virtual_memory(machine, vaddr, reinterpret_cast<const uint8_t*>(req->body.data()), req->body.length()) != 0) {
                    printf("failed to write virtual memory: %s\n", cm_get_last_error_message());
                    return false;
                }
            }
            break;
        }
        case yield_type::SEND_REVERSE_RESPONSE: {
            // Read response headers from VM (similar to POLL_RESPONSE in forward proxy)
            reverse_mmio_res mmio_res;
            if (cm_read_virtual_memory(machine, vaddr, (uint8_t*)(&mmio_res), sizeof(mmio_res)) != 0) {
                printf("failed to read virtual memory: %s\n", cm_get_last_error_message());
                return false;
            }

            // Find the pending request using uid from response
            uint64_t request_uid = mmio_res.uid;
            auto it = pending_reverse_requests.find(request_uid);
            if (it == pending_reverse_requests.end()) {
                printf("failed to find pending reverse request\n");
                return true;
            }

            // Create response object and store headers
            auto resp = std::make_unique<reverse_response>();
            resp->status = mmio_res.status;
            resp->body_length = mmio_res.body_total_length;
            resp->headers.clear();
            for (uint64_t i = 0; i < mmio_res.headers_count; ++i) {
                resp->headers.emplace_back(std::string(mmio_res.headers[i][0]), std::string(mmio_res.headers[i][1]));
            }

            // If no body, mark as ready and clean up
            if (mmio_res.body_total_length == 0) {
                resp->ready = true;
                reverse_responses[request_uid] = std::move(resp);
                pending_reverse_requests.erase(it);
            } else {
                // Store response for body retrieval
                reverse_responses[request_uid] = std::move(resp);
            }
            break;
        }
        case yield_type::SEND_REVERSE_RESPONSE_BODY: {
            // Retrieve response and write body (similar to POLL_RESPONSE_BODY in forward proxy)
            auto it = reverse_responses.find(uid);
            if (it == reverse_responses.end()) {
                printf("failed to retrieve reverse response\n");
                return true;
            }
            auto& resp = it->second;

            // Read body from VM memory
            if (resp->body_length > 0) {
                resp->body.resize(resp->body_length);
                if (cm_read_virtual_memory(machine, vaddr, reinterpret_cast<uint8_t*>(resp->body.data()), resp->body_length) != 0) {
                    printf("failed to read virtual memory: %s\n", cm_get_last_error_message());
                    return false;
                }
            }

            // Mark as ready and clean up pending request
            resp->ready = true;
            auto req_it = pending_reverse_requests.find(uid);
            if (req_it != pending_reverse_requests.end()) {
                pending_reverse_requests.erase(req_it);
            }
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
            "init": "date -s @%llu >> /dev/null && https-proxy 127.254.254.254 80 443 > /dev/null 2>&1 &",
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

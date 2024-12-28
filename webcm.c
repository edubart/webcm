#include <cartesi-machine/machine-c-api.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
  // Set machine configuration
  const char *config = "{\
    \"dtb\": {\
      \"bootargs\": \"quiet earlycon=sbi console=hvc1 root=/dev/pmem0 rw init=/usr/sbin/cartesi-init\",\
      \"init\": \"\",\
      \"entrypoint\": \"exec ash -l\"\
    },\
    \"flash_drive\": [\
      {\"image_filename\": \"rootfs.ext2\"}\
    ],\
    \"virtio\": [\
        {\"type\": \"console\"}\
    ],\
    \"processor\": {\
        \"iunrep\": 1\
    },\
    \"ram\": {\
      \"length\": 134217728,\
      \"image_filename\": \"linux.bin\"\
    }\
  }";

  printf("Booting...\n");

  // Create a new machine
  cm_machine *machine = NULL;
  if (cm_create_new(config, NULL, &machine) != CM_ERROR_OK) {
    printf("failed to create machine: %s\n", cm_get_last_error_message());
    return 1;
  }

  // Run the machine
  cm_break_reason break_reason;
  do {
    uint64_t mcycle;
    if (cm_read_reg(machine, CM_REG_MCYCLE, &mcycle) != CM_ERROR_OK) {
        printf("failed to read machine cycle: %s\n", cm_get_last_error_message());
        cm_delete(machine);
        return 1;
    }
    if (cm_run(machine, mcycle + 4*1024*1024, &break_reason) != CM_ERROR_OK) {
        printf("failed to run machine: %s\n", cm_get_last_error_message());
        cm_delete(machine);
        return 1;
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
  if (cm_read_mcycle(machine, &mcycle) != CM_ERROR_OK) {
    printf("failed to read machine cycle: %s\n", cm_get_last_error_message());
    cm_delete(machine);
    return 1;
  }
  printf("Cycles: %lu\n", (unsigned long)mcycle);

  // Cleanup and exit
  cm_delete(machine);
  return 0;
}

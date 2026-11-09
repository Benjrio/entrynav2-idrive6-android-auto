/* Private local IPC. No HMI/global input injection. Fixed-size datagrams. */
#pragma once
#include <stdint.h>
#define OW_MAGIC 0x41415732u
#define OW_BUILD_REVISION 24u
enum { OW_READY=1, OW_STATE=2, OW_STOP=3, OW_DONE=4, OW_ERROR=5, OW_FRAME=6 };
struct ow_packet { uint32_t magic, type, layer, surface, reserved[3], active; };

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EDGEAI_PACK_ABI_V1 1

typedef struct EdgeAIPackHostV1 {
    void (*log)(int level, const char* message);
    const char* state_dir;
    const char* pack_root;
} EdgeAIPackHostV1;

typedef struct EdgeAIPackRequestV1 {
    const char* prompt;
    const char* options_json;
} EdgeAIPackRequestV1;

typedef struct EdgeAIPackResponseV1 {
    char output_text[256];
    char metadata_json[1024];
} EdgeAIPackResponseV1;

typedef struct EdgeAIPackInstanceV1 {
    void* user_data;
    int (*activate)(void* user_data, const char* activation_json);
    int (*configure)(void* user_data, const char* config_json);
    int (*predict)(void* user_data, const EdgeAIPackRequestV1* request, EdgeAIPackResponseV1* response);
    int (*deactivate)(void* user_data);
    void (*destroy)(void* user_data);
} EdgeAIPackInstanceV1;

int edgeai_pack_get_abi_version(void);
const char* edgeai_pack_get_manifest_json(void);
int edgeai_pack_create(const EdgeAIPackHostV1* host, EdgeAIPackInstanceV1** instance);

#ifdef __cplusplus
}
#endif

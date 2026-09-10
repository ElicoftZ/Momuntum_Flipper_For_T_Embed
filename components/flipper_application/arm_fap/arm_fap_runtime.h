#pragma once
#include <flipper_application/flipper_application.h>

typedef struct ArmFapRuntime ArmFapRuntime;
FlipperApplicationPreloadStatus arm_fap_runtime_preload(
    Storage* storage, const char* path, FlipperApplicationManifest* manifest,
    bool full, ArmFapRuntime** runtime);
FlipperApplicationLoadStatus arm_fap_runtime_map(ArmFapRuntime* runtime);
const char* arm_fap_runtime_error(const ArmFapRuntime* runtime);
int32_t arm_fap_runtime_run(ArmFapRuntime* runtime);
void arm_fap_runtime_free(ArmFapRuntime* runtime);

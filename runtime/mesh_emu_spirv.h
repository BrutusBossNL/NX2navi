#pragma once
// Mesh shader emulation for GPUs without VK_EXT_mesh_shader (NX2NAVI_MESH_EMULATION, vk_pipeline.c / vk_exec_draw.c):
// the translator's TaskEXT and MeshEXT modules (tools/xlate/MeshSpirv.cs) become GLCompute modules that write their
// vertices, triangles and task payloads into one GPU buffer (the arena), and a generated vertex shader reads the
// vertices back for the game's fragment shader. Plain C over SPIR-V words; no Vulkan types.
#include <stddef.h>
#include <stdint.h>

// The arena: one buffer, its address in the push constants of every emulation stage.
//   MESH_EMU_DOFF: draw records, MESH_EMU_DWORDS words each: DrawIndexedIndirect {indexCount, instanceCount,
//   MESH_EMU_TOFF: task records, MESH_EMU_TWORDS words each: mesh workgroup count, then the payload
//   index area (uint32 indices), vertex area (16-byte slots: position, then each generic output) at offsets the
//   replay passes in the push constants.
#define MESH_EMU_DOFF 256u
#define MESH_EMU_DWORDS 16u
#define MESH_EMU_DRECS 4096u
#define MESH_EMU_TOFF (MESH_EMU_DOFF + MESH_EMU_DRECS * MESH_EMU_DWORDS * 4u)
// Push constants of every emulation stage (compute and the vertex shader), 32 bytes:
//   u64 arena address; uint draw record (word index); uint index area (word index); uint index capacity;
#define MESH_EMU_PC_BYTES 32u

typedef struct MeshEmuInfo {
    int task;                 // 1 TaskEXT, 0 MeshEXT
    uint32_t threads;         // LocalSize x
    uint32_t max_v, max_p;    // mesh: OutputVertices, OutputPrimitivesEXT
    uint32_t payload_words;   // the task payload's size (mesh: what it reads, 0 without a task stage)
    uint32_t max_mesh;        // task: the most mesh workgroups one task workgroup launches (dispatch width)
    uint32_t slots;           // mesh: 16-byte slots per vertex: position and each generic output
    uint32_t n_generic;       // mesh: generic outputs, in slot order 1..n
    uint8_t generic_loc[32];  // their locations
} MeshEmuInfo;

// The compute module for a TaskEXT or MeshEXT module (malloc'd, *out_n words); NULL with why set when the module is not
// one the converter knows (it then stays unemulated: its draws are skipped as before).
uint32_t *mesh_emu_compute(const uint32_t *w, size_t n, MeshEmuInfo *info, size_t *out_n, const char **why);

// The vertex shader of an emulated mesh pipeline: position and the generic outputs from the vertex slots, as types[l]
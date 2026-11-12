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
// task records: as many as the push constants say (the replay sizes them, NX2NAVI_MESH_EMU_TASKS); the index area
// follows them
// Push constants of every emulation stage (compute and the vertex shader), 32 bytes:
//   u64 arena address; uint draw record (word index); uint index area (word index); uint index capacity;
//   uint vertex area (16-byte slot index); uint vertex capacity (slots); uint task record capacity
// Header words 0-2 count what the stages asked for, beyond the capacities too (the replay logs the most per
// submission); word 3 counts mesh workgroups that did not fit.
#define MESH_EMU_PC_BYTES 32u

typedef struct MeshEmuInfo {
    int task;                 // 1 TaskEXT, 0 MeshEXT
    uint32_t threads;         // LocalSize x
    uint32_t max_v, max_p;    // mesh: OutputVertices, OutputPrimitivesEXT
    uint32_t prim;            // mesh: 3 triangles, 2 lines, 1 points (triangles and points are emulated)
    uint32_t psize_slot;      // mesh: the slot holding the point size (x), 0 without one
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
// (0 float, 1 signed, 2 unsigned) with comps[l] components for every location l in need (the fragment stage's
// inputs); a location the mesh stage does not write is zero. malloc'd, *out_n words.
// A task or mesh module's output primitive: 3 triangles, 2 lines, 1 points, 0 not a mesh module.
int mesh_emu_prim(const uint32_t *w, size_t n);

// fsw: the fragment shader's SPIR-V; its input variables are matched one for one (Location, Component, type, Flat),
// else (NULL, or a declaration the parser does not take) one vector per location of need.
uint32_t *mesh_emu_vs(const MeshEmuInfo *m, uint32_t need, const uint8_t types[32], const uint8_t comps[32],
                      const uint32_t *fsw, size_t fsn, size_t *out_n);

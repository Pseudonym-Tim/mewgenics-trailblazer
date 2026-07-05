#ifndef MANUAL_PATH_H
#define MANUAL_PATH_H

#include <stdint.h>
#include <windows.h>

#define MOD_NAME "Trailblazer"
#define ENABLE_DEBUG_LOGS 1

#define RVA_MOVE_ABILITY_ON_TRIGGER 0x0075C810 // Hook point for final click/confirm so prepared manual paths can be auto-triggered on release...
#define RVA_ABILITY_TRIGGER 0x00032050 // Called after writing the prepared target back into the active MoveAbility...
#define RVA_MOVE_ABILITY_BUILD_PREVIEW_PATH 0x0075E3F0 // Hook point that observes drag previews and can overwrite preview buffers with manual tiles....
#define RVA_TACTICS_GRID_BUILD_MOVE_PATH 0x008A9DC0 // TacticsGrid move-path builder used during execution (Hook point that forces the prepared manual path into the committed move)...
#define RVA_VECTOR_REALLOC_BYTES 0x00D44928 // Game vector byte-reallocation helper (used to grow the engine-owned path buffers before overwriting them)...

#define STOLEN_MOVE_ABILITY_ON_TRIGGER 15
#define STOLEN_MOVE_ABILITY_BUILD_PREVIEW_PATH 15
#define STOLEN_TACTICS_GRID_BUILD_MOVE_PATH 17

// MoveAbility/Character field offsets...
#define MOVE_ABILITY_CHARACTER_OFFSET 0x10 // MoveAbility -> owning Character*...
#define MOVE_ABILITY_TARGET_TILE_OFFSET 0x620 // MoveAbility -> sanitized preview/target tile...
#define MOVE_ABILITY_RAW_HOVER_TILE_OFFSET 0x628 // MoveAbility -> raw hover tile before vanilla target sanitization...
#define CHARACTER_TILE_OWNER_OFFSET 0x60 // Character -> tile owner / tile component pointer...
#define CHARACTER_TILE_PACKED_OFFSET 0x48 // Tile owner/component -> packed uint64 tile coordinate...
#define CHARACTER_TILE_SIZE_OFFSET 0x138 // Tile owner/component -> footprint size code (1 is the supported 1x1 footprint)...
#define CHARACTER_PENDING_ABILITY_OFFSET 0x118 // Character -> pending Ability* used during release-trigger validation...
#define CHARACTER_PENDING_TURN_ACTION_OFFSET 0x120 // Character -> pending TurnAction* passed to Ability::Trigger...

#define MAX_MANUAL_PATH_TILES 64
#define MAX_MANUAL_STEPS 63
#define DRAG_SAMPLE_INTERVAL_MS 1U
#define FRAME_DRAG_SAMPLE_INTERVAL_MS 8ULL
#define PREPARED_PATH_WINDOW_MS 5000ULL
#define APPLY_PATH_OVERRIDE_WINDOW_MS 350ULL
#define APPLY_PATH_OVERRIDE_BUDGET 64
#define CANCEL_MOVE_TRIGGER_WINDOW_MS 500ULL
#define RELEASE_PREVIEW_STALE_MS 50ULL
#define RELEASE_CURSOR_DRIFT_PIXELS 8L
#define AUTO_MOVE_ON_RELEASE 1
#define MANUAL_DRAG_ARM_PIXELS 10L
#define MANUAL_DRAG_ARM_MIN_HELD_MS 35ULL
#define ORIGIN_DRAG_BEGIN_STRONG_RADIUS_PIXELS 12L
#define ORIGIN_DRAG_BEGIN_BOOTSTRAP_RADIUS_PIXELS 24L

#define ENABLE_OPPOSITE_DIAGONAL_CORNER_BRIDGE 1
#define REQUIRE_FRESH_PREVIEW_ON_RELEASE 1
#define REQUIRE_VANILLA_ENDPOINT_REACHABILITY 1
#define ENABLE_REPORTED_MAX_MOVE_FALLBACK 0

#define ENABLE_MANUAL_PATH_ARROW_COLOR 1
#define MANUAL_PATH_ARROW_COLOR_R 0.5098039f
#define MANUAL_PATH_ARROW_COLOR_G 0.7098039f
#define MANUAL_PATH_ARROW_COLOR_B 0.9803922f
#define MANUAL_PATH_ARROW_COLOR_A 1.00f

#define RVA_PATH_VISUAL_CREATE 0x0033FA70 // PathIndicator visual creation (hook point for tinting path arrows)...
#define STOLEN_PATH_VISUAL_CREATE 19

// (Matches the small native path/vector wrapper returned by game's path builders)...
#pragma pack(push, 1)
typedef struct ManualPathBuffer
{
    int32_t capacity;
    int32_t count;
    uint64_t* data;
} ManualPathBuffer;
#pragma pack(pop)

// Native function signatures resolved by RVA and installed as trampolines...
typedef void (__fastcall *fn_move_ability_on_trigger)(void* ability);
typedef void (__fastcall *fn_ability_trigger)(void* ability, void* turnAction);
typedef ManualPathBuffer* (__fastcall *fn_move_ability_build_preview_path)(void* ability, ManualPathBuffer* outPath, uint64_t startTile, uint64_t targetTile);
typedef ManualPathBuffer* (__fastcall *fn_tactics_grid_build_move_path)(void* grid, ManualPathBuffer* outPath, void* character, uint64_t startTile, uint64_t targetTile, int32_t maxMove, uint8_t flag);
typedef void* (__fastcall *fn_vector_realloc_bytes)(void* data, uint64_t byteCount);
typedef void* (__fastcall *fn_path_visual_create)(void* parent, int32_t visualKind, void* assetName, void* objectName, void* position, void* color, int32_t layer, void* transform, uint8_t visibleFlag);

#endif
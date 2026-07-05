#include "Trailblazer.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mewjector.h"

// Mewjector and trampoline entry points for native hooks...
static MewjectorAPI g_mj;
static fn_move_ability_on_trigger g_origMoveAbilityOnTrigger = NULL;
static fn_ability_trigger g_abilityTrigger = NULL;
static fn_move_ability_build_preview_path g_origMoveAbilityBuildPreviewPath = NULL;
static fn_tactics_grid_build_move_path g_origTacticsGridBuildMovePath = NULL;
static fn_vector_realloc_bytes g_vectorReallocBytes = NULL;
static fn_path_visual_create g_origPathVisualCreate = NULL;

// (Global runtime state protected by g_pathLock because potential interleave)...
static CRITICAL_SECTION g_pathLock;
static volatile LONG g_pathLockReady = 0;
static volatile LONG g_shutdown = 0;
static HANDLE g_timerQueue = NULL;
static volatile LONG g_timerStarted = 0;
static volatile LONG g_hookInstallBusy = 0;
static volatile LONG g_hookedOnTrigger = 0;
static volatile LONG g_hookedPreview = 0;
static volatile LONG g_hookedExecution = 0;
static volatile LONG g_hookedPathVisualCreate = 0;
static volatile LONG g_reportedMaxMoveFallback = ENABLE_REPORTED_MAX_MOVE_FALLBACK;
static volatile LONG g_manualArrowColor = ENABLE_MANUAL_PATH_ARROW_COLOR;
static volatile LONG g_pathIndicatorColorWarningLogged = 0;
static volatile LONG g_pathVisualCreateSeen = 0;
static uint32_t g_timerTickCount = 0;

// Current in-progress drag path authored from preview samples before release...
static uint64_t g_dragTiles[MAX_MANUAL_PATH_TILES];
static int32_t g_dragCount = 0;
static uint64_t g_dragStartTile = 0;
static uint64_t g_dragEndTile = 0;
static void* g_dragAbility = NULL;
static ULONGLONG g_originHoverConfirmedTick = 0;
static POINT g_originHoverConfirmedCursor = { 0, 0 };

// Last validated manual path prepared for preview/execution override after mouse release...
static uint64_t g_preparedTiles[MAX_MANUAL_PATH_TILES];
static int32_t g_preparedCount = 0;
static uint64_t g_preparedStartTile = 0;
static uint64_t g_preparedEndTile = 0;
static ULONGLONG g_preparedTick = 0;
static ULONGLONG g_lastSampleTick = 0;
static ULONGLONG g_lastFrameDragSampleTick = 0;
static volatile LONG g_frameDragSamplerBusy = 0;
static ULONGLONG g_lastDragPreviewTick = 0;
static uint64_t g_lastDragPreviewTargetTile = 0;
static POINT g_lastDragPreviewCursor = { 0, 0 };
static POINT g_previousDragPreviewCursor = { 0, 0 };
static uint8_t g_hasPreviousDragPreviewCursor = 0;
static uint8_t g_wasLeftDown = 0;
static uint8_t g_pendingPreviewRelease = 0;
static uint8_t g_manualDragArmed = 0;
static POINT g_dragBeginCursor = { 0, 0 };
static uint8_t g_hasDragBeginCursor = 0;
static ULONGLONG g_dragBeginTick = 0;
static volatile LONG g_applyNextPath = 0;
static volatile LONG g_applyOverrideBudget = 0;
static ULONGLONG g_applyOverrideUntilTick = 0;
static void* g_cancelMoveAbility = NULL;
static ULONGLONG g_cancelMoveUntilTick = 0;
static volatile LONG g_autoTriggerBusy = 0;
static volatile LONG g_autoMoveOnRelease = AUTO_MOVE_ON_RELEASE;
static void* g_lastPreviewAbility = NULL;
static uint64_t g_lastPreviewStartTile = 0;
static uint64_t g_lastPreviewTargetTile = 0;
static int32_t g_lastPreviewWasReachable = 0;
static int32_t g_rawHoverDifferentCount = 0;
static int32_t g_executionApplyCount = 0;
static void* g_lastIgnoredManualDragAbility = NULL;

// Screen-to-tile calibration samples learned from vanilla preview targets under the cursor...
typedef struct TileScreenCalibrationSample
{
    uint64_t tile;
    POINT cursor;
} TileScreenCalibrationSample;

#define TILE_SCREEN_CALIBRATION_SAMPLE_COUNT 48
#define TILE_INFER_MAX_SCREEN_DISTANCE_SQ 4096.0

static TileScreenCalibrationSample g_tileScreenSamples[TILE_SCREEN_CALIBRATION_SAMPLE_COUNT];
static int32_t g_tileScreenSampleCount = 0;
static int32_t g_tileScreenSampleWriteIndex = 0;
static int32_t g_tileScreenCalibrationReady = 0;
static double g_tileScreenOriginX = 0.0;
static double g_tileScreenOriginY = 0.0;
static double g_tileScreenAxisXX = 0.0;
static double g_tileScreenAxisXY = 0.0;
static double g_tileScreenAxisYX = 0.0;
static double g_tileScreenAxisYY = 0.0;
static int32_t g_inferredHoverDifferentCount = 0;

// Short-lived non-hook samples queued by timer when preview hook misses a drag frame...
typedef struct FrameDragCandidate
{
    uint64_t tile;
    uint64_t previewTargetTile;
    ULONGLONG tick;
    char sourceName[32];
} FrameDragCandidate;

#define FRAME_DRAG_CANDIDATE_COUNT 32
#define FRAME_DRAG_CANDIDATE_MAX_AGE_MS 250ULL
#define ORIGIN_HOVER_CONFIRM_WINDOW_MS 2000ULL
#define ORIGIN_HOVER_CONFIRM_CURSOR_DRIFT_PIXELS 12L

static FrameDragCandidate g_frameDragCandidates[FRAME_DRAG_CANDIDATE_COUNT];
static int32_t g_frameDragCandidateCount = 0;
static int32_t g_frameDragCandidateWriteIndex = 0;

// Checks whether native path buffer reached requested target tile...
static int IsPathBufferEndingAtTile(ManualPathBuffer* path, uint64_t targetTile);
static LONG AbsLong(LONG value);
static int IsCursorNearDragBeginNoLock(POINT cursor, LONG radiusPixels);
static int AppendDragTileDirectNoLock(void* ability, uint64_t tile);
static int ReturnDragToOriginIfCurrentlyHoveredNoLock(void* ability, uint64_t previewTargetTile, const char* sourceName);

static void Log(const char* fmt, ...)
{
    char buffer[512];
    va_list args;

    if (!ENABLE_DEBUG_LOGS || !g_mj.Log)
    {
        return;
    }

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    g_mj.Log(MOD_NAME, "%s", buffer);
}

static UINT_PTR GetGameBase(void)
{
    if (!g_mj.GetGameBase)
    {
        return 0U;
    }

    return g_mj.GetGameBase();
}

// Extracts the signed X coordinate from the game's packed uint64 tile format...
static int32_t TileX(uint64_t tile)
{
    return (int32_t)(tile & 0xFFFFFFFFu);
}

// Extracts the signed Y coordinate from the game's packed uint64 tile format...
static int32_t TileY(uint64_t tile)
{
    return (int32_t)((tile >> 32) & 0xFFFFFFFFu);
}

// Packs signed X/Y tile coordinates into the uint64 layout used by the game...
static uint64_t MakeTile(int32_t x, int32_t y)
{
    uint64_t packedX;
    uint64_t packedY;

    packedX = (uint64_t)(uint32_t)x;
    packedY = (uint64_t)(uint32_t)y;

    return packedX | (packedY << 32);
}

// Rejects null/outlier tile values so corrupted reads do not enter path buffers...
static int IsPlausibleTile(uint64_t tile)
{
    int32_t x;
    int32_t y;

    if (tile == 0)
    {
        return 0;
    }

    x = TileX(tile);
    y = TileY(tile);

    if (x < -512 || x > 512 || y < -512 || y > 512)
    {
        return 0;
    }

    return 1;
}

// Small integer absolute helper used by Manhattan tile-distance checks...
static int32_t AbsI32(int32_t value)
{
    return (value < 0) ? -value : value;
}

// Floating-point absolute helper for calibration math...
static double AbsF64(double value)
{
    return (value < 0.0) ? -value : value;
}

// Rounds projected screen/tile coordinates back to the nearest tile grid coordinate...
static int32_t RoundF64ToI32(double value)
{
    if (value >= 0.0)
    {
        return (int32_t)(value + 0.5);
    }

    return (int32_t)(value - 0.5);
}

// Computes squared screen distance without a sqrt for nearest-sample selection...
static double DistanceSquaredF64(double leftX, double leftY, double rightX, double rightY)
{
    double deltaX;
    double deltaY;

    deltaX = leftX - rightX;
    deltaY = leftY - rightY;

    return (deltaX * deltaX) + (deltaY * deltaY);
}

// Solves the 3x3 normal equations used by screen-to-tile affine calibration...
static int Solve3x3(double matrix[3][4], double result[3])
{
    int32_t pivotIndex;
    int32_t rowIndex;
    int32_t columnIndex;
    int32_t bestRow;
    double bestValue;
    double value;
    double scale;
    double temp;

    for (pivotIndex = 0; pivotIndex < 3; pivotIndex++)
    {
        bestRow = pivotIndex;
        bestValue = AbsF64(matrix[pivotIndex][pivotIndex]);

        for (rowIndex = pivotIndex + 1; rowIndex < 3; rowIndex++)
        {
            value = AbsF64(matrix[rowIndex][pivotIndex]);

            if (value > bestValue)
            {
                bestValue = value;
                bestRow = rowIndex;
            }
        }

        if (bestValue < 0.000001)
        {
            return 0;
        }

        if (bestRow != pivotIndex)
        {
            for (columnIndex = pivotIndex; columnIndex < 4; columnIndex++)
            {
                temp = matrix[pivotIndex][columnIndex];
                matrix[pivotIndex][columnIndex] = matrix[bestRow][columnIndex];
                matrix[bestRow][columnIndex] = temp;
            }
        }

        scale = matrix[pivotIndex][pivotIndex];

        for (columnIndex = pivotIndex; columnIndex < 4; columnIndex++)
        {
            matrix[pivotIndex][columnIndex] = matrix[pivotIndex][columnIndex] / scale;
        }

        for (rowIndex = 0; rowIndex < 3; rowIndex++)
        {
            if (rowIndex == pivotIndex)
            {
                continue;
            }

            scale = matrix[rowIndex][pivotIndex];

            if (AbsF64(scale) < 0.000001)
            {
                continue;
            }

            for (columnIndex = pivotIndex; columnIndex < 4; columnIndex++)
            {
                matrix[rowIndex][columnIndex] = matrix[rowIndex][columnIndex] - (scale * matrix[pivotIndex][columnIndex]);
            }
        }
    }

    result[0] = matrix[0][3];
    result[1] = matrix[1][3];
    result[2] = matrix[2][3];

    return 1;
}

// Fits tile-to-screen axes from remembered preview samples...
static int RebuildTileScreenCalibrationNoLock(void)
{
    double normal[3][3];
    double rhsX[3];
    double rhsY[3];
    double solveX[3][4];
    double solveY[3][4];
    double resultX[3];
    double resultY[3];
    double basis[3];
    double x;
    double y;
    double screenX;
    double screenY;
    int32_t sampleIndex;
    int32_t rowIndex;
    int32_t columnIndex;

    if (g_tileScreenSampleCount < 4)
    {
        g_tileScreenCalibrationReady = 0;
        return 0;
    }

    memset(normal, 0, sizeof(normal));
    memset(rhsX, 0, sizeof(rhsX));
    memset(rhsY, 0, sizeof(rhsY));

    for (sampleIndex = 0; sampleIndex < g_tileScreenSampleCount; sampleIndex++)
    {
        x = (double)TileX(g_tileScreenSamples[sampleIndex].tile);
        y = (double)TileY(g_tileScreenSamples[sampleIndex].tile);
        screenX = (double)g_tileScreenSamples[sampleIndex].cursor.x;
        screenY = (double)g_tileScreenSamples[sampleIndex].cursor.y;

        basis[0] = 1.0;
        basis[1] = x;
        basis[2] = y;

        for (rowIndex = 0; rowIndex < 3; rowIndex++)
        {
            rhsX[rowIndex] = rhsX[rowIndex] + (basis[rowIndex] * screenX);
            rhsY[rowIndex] = rhsY[rowIndex] + (basis[rowIndex] * screenY);

            for (columnIndex = 0; columnIndex < 3; columnIndex++)
            {
                normal[rowIndex][columnIndex] = normal[rowIndex][columnIndex] + (basis[rowIndex] * basis[columnIndex]);
            }
        }
    }

    for (rowIndex = 0; rowIndex < 3; rowIndex++)
    {
        for (columnIndex = 0; columnIndex < 3; columnIndex++)
        {
            solveX[rowIndex][columnIndex] = normal[rowIndex][columnIndex];
            solveY[rowIndex][columnIndex] = normal[rowIndex][columnIndex];
        }

        solveX[rowIndex][3] = rhsX[rowIndex];
        solveY[rowIndex][3] = rhsY[rowIndex];
    }

    if (!Solve3x3(solveX, resultX) || !Solve3x3(solveY, resultY))
    {
        g_tileScreenCalibrationReady = 0;
        return 0;
    }

    g_tileScreenOriginX = resultX[0];
    g_tileScreenAxisXX = resultX[1];
    g_tileScreenAxisXY = resultX[2];
    g_tileScreenOriginY = resultY[0];
    g_tileScreenAxisYX = resultY[1];
    g_tileScreenAxisYY = resultY[2];
    g_tileScreenCalibrationReady = 1;

    return 1;
}

// Adds one observed tile/cursor pair to calibration buffer...
static void RememberTileScreenSampleNoLock(uint64_t tile)
{
    POINT cursor;
    int32_t index;

    if (!IsPlausibleTile(tile))
    {
        return;
    }

    if (!GetCursorPos(&cursor))
    {
        return;
    }

    for (index = 0; index < g_tileScreenSampleCount; index++)
    {
        if (g_tileScreenSamples[index].tile == tile)
        {
            g_tileScreenSamples[index].cursor = cursor;
            RebuildTileScreenCalibrationNoLock();
            return;
        }
    }

    g_tileScreenSamples[g_tileScreenSampleWriteIndex].tile = tile;
    g_tileScreenSamples[g_tileScreenSampleWriteIndex].cursor = cursor;

    if (g_tileScreenSampleCount < TILE_SCREEN_CALIBRATION_SAMPLE_COUNT)
    {
        g_tileScreenSampleCount++;
    }

    g_tileScreenSampleWriteIndex = (g_tileScreenSampleWriteIndex + 1) % TILE_SCREEN_CALIBRATION_SAMPLE_COUNT;
    RebuildTileScreenCalibrationNoLock();
}

// Projects a tile into screen space...
static int ProjectTileToScreenNoLock(uint64_t tile, double* outScreenX, double* outScreenY)
{
    double tileX;
    double tileY;

    if (!outScreenX || !outScreenY || !g_tileScreenCalibrationReady || !IsPlausibleTile(tile))
    {
        return 0;
    }

    tileX = (double)TileX(tile);
    tileY = (double)TileY(tile);

    *outScreenX = g_tileScreenOriginX + (g_tileScreenAxisXX * tileX) + (g_tileScreenAxisXY * tileY);
    *outScreenY = g_tileScreenOriginY + (g_tileScreenAxisYX * tileX) + (g_tileScreenAxisYY * tileY);

    return 1;
}

// Infers the tile under the cursor when vanilla sanitized preview target differs from intent...
static int TryInferCursorTileNoLock(uint64_t* outTile)
{
    POINT cursor;
    double determinant;
    double localX;
    double localY;
    double tileXFloat;
    double tileYFloat;
    double screenX;
    double screenY;
    double distance;
    double bestDistance;
    int32_t roundedX;
    int32_t roundedY;
    int32_t offsetX;
    int32_t offsetY;
    uint64_t candidateTile;
    uint64_t bestTile;

    if (!outTile)
    {
        return 0;
    }

    *outTile = 0;

    if (!g_tileScreenCalibrationReady)
    {
        return 0;
    }

    if (!GetCursorPos(&cursor))
    {
        return 0;
    }

    determinant = (g_tileScreenAxisXX * g_tileScreenAxisYY) - (g_tileScreenAxisXY * g_tileScreenAxisYX);

    if (AbsF64(determinant) < 0.000001)
    {
        return 0;
    }

    localX = (double)cursor.x - g_tileScreenOriginX;
    localY = (double)cursor.y - g_tileScreenOriginY;

    tileXFloat = ((localX * g_tileScreenAxisYY) - (localY * g_tileScreenAxisXY)) / determinant;
    tileYFloat = ((g_tileScreenAxisXX * localY) - (g_tileScreenAxisYX * localX)) / determinant;

    roundedX = RoundF64ToI32(tileXFloat);
    roundedY = RoundF64ToI32(tileYFloat);
    bestTile = 0;
    bestDistance = 999999999.0;

    for (offsetY = -1; offsetY <= 1; offsetY++)
    {
        for (offsetX = -1; offsetX <= 1; offsetX++)
        {
            candidateTile = MakeTile(roundedX + offsetX, roundedY + offsetY);

            if (!ProjectTileToScreenNoLock(candidateTile, &screenX, &screenY))
            {
                continue;
            }

            distance = DistanceSquaredF64(screenX, screenY, (double)cursor.x, (double)cursor.y);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestTile = candidateTile;
            }
        }
    }

    if (bestTile == 0 || bestDistance > TILE_INFER_MAX_SCREEN_DISTANCE_SQ)
    {
        return 0;
    }

    *outTile = bestTile;
    return 1;
}

// Chooses the diagonal bridge corner whose projected screen position best matches cursor movement...
static uint64_t PickProjectedDiagonalCornerNoLock(uint64_t startTile, uint64_t targetTile)
{
    uint64_t candidateOne;
    uint64_t candidateTwo;
    double oneX;
    double oneY;
    double twoX;
    double twoY;
    double oneDistance;
    double twoDistance;
    POINT cursor;

    if (!g_tileScreenCalibrationReady || !GetCursorPos(&cursor))
    {
        return 0;
    }

    if (AbsI32(TileX(startTile) - TileX(targetTile)) != 1 || AbsI32(TileY(startTile) - TileY(targetTile)) != 1)
    {
        return 0;
    }

    candidateOne = MakeTile(TileX(startTile), TileY(targetTile));
    candidateTwo = MakeTile(TileX(targetTile), TileY(startTile));

    if (!ProjectTileToScreenNoLock(candidateOne, &oneX, &oneY) || !ProjectTileToScreenNoLock(candidateTwo, &twoX, &twoY))
    {
        return 0;
    }

    oneDistance = DistanceSquaredF64(oneX, oneY, (double)cursor.x, (double)cursor.y);
    twoDistance = DistanceSquaredF64(twoX, twoY, (double)cursor.x, (double)cursor.y);

    if (oneDistance <= twoDistance)
    {
        return candidateOne;
    }

    return candidateTwo;
}

// Returns true only for one-cardinal-step adjacency (Diagonal steps are bridged explicitly elsewhere)...
static int IsAdjacentTile(uint64_t leftTile, uint64_t rightTile)
{
    int32_t dx;
    int32_t dy;

    dx = TileX(leftTile) - TileX(rightTile);
    dy = TileY(leftTile) - TileY(rightTile);

    if (dx < 0)
    {
        dx = -dx;
    }

    if (dy < 0)
    {
        dy = -dy;
    }

    return ((dx + dy) == 1) ? 1 : 0;
}

// Asks the native preview path whether a direct one-step move from A to B is valid...
static int IsNativeOneStepPreviewNoLock(void* ability, uint64_t fromTile, uint64_t toTile)
{
    ManualPathBuffer tempPath;
    ManualPathBuffer* resultPath;
    ManualPathBuffer* path;
    uint64_t tempTiles[MAX_MANUAL_PATH_TILES];
    int32_t index;
    int32_t count;

    if (!ability || !g_origMoveAbilityBuildPreviewPath || fromTile == 0 || toTile == 0 || fromTile == toTile)
    {
        return 0;
    }

    tempPath.capacity = MAX_MANUAL_PATH_TILES;
    tempPath.count = 0;
    tempPath.data = tempTiles;

    for (index = 0; index < MAX_MANUAL_PATH_TILES; index++)
    {
        tempTiles[index] = 0;
    }

    resultPath = NULL;

    __try
    {
        resultPath = g_origMoveAbilityBuildPreviewPath(ability, &tempPath, fromTile, toTile);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Native one-step preview exception: from=(%d,%d) to=(%d,%d)", TileX(fromTile), TileY(fromTile), TileX(toTile), TileY(toTile));
        return 0;
    }

    path = resultPath ? resultPath : &tempPath;

    if (!path || !path->data || path->count <= 0 || path->count > MAX_MANUAL_PATH_TILES)
    {
        return 0;
    }

    if (!IsPathBufferEndingAtTile(path, toTile))
    {
        return 0;
    }

    count = path->count;

    if (count == 1)
    {
        return 1;
    }

    if (count == 2)
    {
        return 1;
    }

    return 0;
}

// Main append routine for manual drag samples (Direct step, diagonal bridge, then vanilla bridge)...
static void AddDragTile(void* ability, uint64_t tile);

// Turns off the short execution override window without deleting prepared path diagnostics...
static void DisarmPreparedPathOverrideNoLock(const char* reason);

// Validates a candidate manual tile as a legal adjacent native step from the current drag end...
static int IsManualStepTileNoLock(void* ability, uint64_t leftTile, uint64_t rightTile)
{
    if (IsAdjacentTile(leftTile, rightTile))
    {
        return 1;
    }

    if (IsNativeOneStepPreviewNoLock(ability, leftTile, rightTile))
    {
        Log("Native one-step accepted non-Manhattan manual step: from=(%d,%d) to=(%d,%d)", TileX(leftTile), TileY(leftTile), TileX(rightTile), TileY(rightTile));
        return 1;
    }

    return 0;
}

// Checks whether a native preview route from A to B includes required intermediate tile...
static int DoesNativePreviewPathContainTileNoLock(void* ability, uint64_t fromTile, uint64_t toTile, uint64_t requiredTile, int* outPathWasReachable)
{
    ManualPathBuffer tempPath;
    ManualPathBuffer* resultPath;
    ManualPathBuffer* path;
    uint64_t tempTiles[MAX_MANUAL_PATH_TILES];
    int32_t index;

    if (outPathWasReachable)
    {
        *outPathWasReachable = 0;
    }

    if (!ability || !g_origMoveAbilityBuildPreviewPath || fromTile == 0 || toTile == 0 || requiredTile == 0)
    {
        return 0;
    }

    tempPath.capacity = MAX_MANUAL_PATH_TILES;
    tempPath.count = 0;
    tempPath.data = tempTiles;

    for (index = 0; index < MAX_MANUAL_PATH_TILES; index++)
    {
        tempTiles[index] = 0;
    }

    resultPath = NULL;

    __try
    {
        resultPath = g_origMoveAbilityBuildPreviewPath(ability, &tempPath, fromTile, toTile);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Native transit probe exception: from=(%d,%d) to=(%d,%d) required=(%d,%d)", TileX(fromTile), TileY(fromTile), TileX(toTile), TileY(toTile), TileX(requiredTile), TileY(requiredTile));
        return 0;
    }

    path = resultPath ? resultPath : &tempPath;

    if (!path || !path->data || path->count <= 0 || path->count > MAX_MANUAL_PATH_TILES)
    {
        return 0;
    }

    if (!IsPathBufferEndingAtTile(path, toTile))
    {
        return 0;
    }

    if (outPathWasReachable)
    {
        *outPathWasReachable = 1;
    }

    for (index = 0; index < path->count; index++)
    {
        if (path->data[index] == requiredTile)
        {
            return 1;
        }
    }

    return 0;
}

// Checks whether the first native transit step is required occupied/pass-through candidate...
static int DoesNativePreviewPathUseImmediateTransitTileNoLock(void* ability, uint64_t fromTile, uint64_t toTile, uint64_t requiredTile, int* outPathWasReachable)
{
    ManualPathBuffer tempPath;
    ManualPathBuffer* resultPath;
    ManualPathBuffer* path;
    uint64_t tempTiles[MAX_MANUAL_PATH_TILES];
    uint64_t firstMoveTile;
    int32_t index;

    if (outPathWasReachable)
    {
        *outPathWasReachable = 0;
    }

    if (!ability || !g_origMoveAbilityBuildPreviewPath || fromTile == 0 || toTile == 0 || requiredTile == 0 || toTile == requiredTile)
    {
        return 0;
    }

    tempPath.capacity = MAX_MANUAL_PATH_TILES;
    tempPath.count = 0;
    tempPath.data = tempTiles;

    for (index = 0; index < MAX_MANUAL_PATH_TILES; index++)
    {
        tempTiles[index] = 0;
    }

    resultPath = NULL;

    __try
    {
        resultPath = g_origMoveAbilityBuildPreviewPath(ability, &tempPath, fromTile, toTile);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Native immediate-transit probe exception: from=(%d,%d) to=(%d,%d) required=(%d,%d)", TileX(fromTile), TileY(fromTile), TileX(toTile), TileY(toTile), TileX(requiredTile), TileY(requiredTile));
        return 0;
    }

    path = resultPath ? resultPath : &tempPath;

    if (!path || !path->data || path->count <= 0 || path->count > MAX_MANUAL_PATH_TILES)
    {
        return 0;
    }

    if (!IsPathBufferEndingAtTile(path, toTile))
    {
        return 0;
    }

    if (outPathWasReachable)
    {
        *outPathWasReachable = 1;
    }

    firstMoveTile = 0;

    for (index = 0; index < path->count; index++)
    {
        if (path->data[index] == 0 || path->data[index] == fromTile)
        {
            continue;
        }

        firstMoveTile = path->data[index];
        break;
    }

    if (firstMoveTile == requiredTile)
    {
        return 1;
    }

    return 0;
}

// Validates inferred/raw hover tiles by probing nearby endpoints and requiring native transit through the candidate...
static int NativeRouteUsesCandidateAsShortTransitNoLock(void* ability, uint64_t fromTile, uint64_t candidateTile, uint64_t previewTargetTile, const char* sourceName)
{
    uint64_t probeTargets[8];
    int32_t candidateX;
    int32_t candidateY;
    int32_t fromX;
    int32_t fromY;
    int32_t stepX;
    int32_t stepY;
    int32_t probeCount;
    int32_t index;
    int pathWasReachable;

    if (!ability || fromTile == 0 || candidateTile == 0 || fromTile == candidateTile)
    {
        return 0;
    }

    candidateX = TileX(candidateTile);
    candidateY = TileY(candidateTile);
    fromX = TileX(fromTile);
    fromY = TileY(fromTile);
    stepX = candidateX - fromX;
    stepY = candidateY - fromY;
    probeCount = 0;

    for (index = 0; index < 8; index++)
    {
        probeTargets[index] = 0;
    }

    /*
        Vanilla must build a path whose immediate next movement tile
        is this candidate. Just containing the candidate somewhere in a path is
        too permissive and allows hard blockers to become manual corner tiles...
    */
    if (previewTargetTile != 0 && previewTargetTile != fromTile && previewTargetTile != candidateTile && IsAdjacentTile(candidateTile, previewTargetTile))
    {
        probeTargets[probeCount] = previewTargetTile;
        probeCount++;
    }

    if (AbsI32(stepX) + AbsI32(stepY) == 1)
    {
        probeTargets[probeCount] = MakeTile(candidateX + stepX, candidateY + stepY);
        probeCount++;
    }

    probeTargets[probeCount] = MakeTile(candidateX + 1, candidateY);
    probeCount++;
    probeTargets[probeCount] = MakeTile(candidateX - 1, candidateY);
    probeCount++;
    probeTargets[probeCount] = MakeTile(candidateX, candidateY + 1);
    probeCount++;
    probeTargets[probeCount] = MakeTile(candidateX, candidateY - 1);
    probeCount++;

    for (index = 0; index < probeCount; index++)
    {
        if (probeTargets[index] == 0 || probeTargets[index] == fromTile || probeTargets[index] == candidateTile)
        {
            continue;
        }

        pathWasReachable = 0;

        if (DoesNativePreviewPathUseImmediateTransitTileNoLock(ability, fromTile, probeTargets[index], candidateTile, &pathWasReachable))
        {
            Log("Accepted inferred %s tile via native immediate-transit probe: from=(%d,%d) candidate=(%d,%d) probe=(%d,%d)", sourceName ? sourceName : "cursor", TileX(fromTile), TileY(fromTile), TileX(candidateTile), TileY(candidateTile), TileX(probeTargets[index]), TileY(probeTargets[index]));
            return 1;
        }
    }

    return 0;
}

// Adds deduplicated probe endpoint to the temporary native-route validation list...
static void AddUniqueProbeTargetNoLock(uint64_t* probeTargets, int32_t* probeCount, int32_t maxProbeCount, uint64_t probeTile)
{
    int32_t index;

    if (!probeTargets || !probeCount || maxProbeCount <= 0 || probeTile == 0)
    {
        return;
    }

    for (index = 0; index < *probeCount; index++)
    {
        if (probeTargets[index] == probeTile)
        {
            return;
        }
    }

    if (*probeCount >= maxProbeCount)
    {
        return;
    }

    probeTargets[*probeCount] = probeTile;
    *probeCount = *probeCount + 1;
}

// Confirms a candidate remains within the original vanilla movement envelope...
static int IsTileInsideOriginalMovementZoneNoLock(void* ability, uint64_t tile, const char* sourceName)
{
    ManualPathBuffer tempPath;
    ManualPathBuffer* resultPath;
    ManualPathBuffer* path;
    uint64_t tempTiles[MAX_MANUAL_PATH_TILES];
    uint64_t originalStartTile;
    int32_t index;

    if (!ability || tile == 0 || g_dragStartTile == 0)
    {
        return 0;
    }

    originalStartTile = g_dragStartTile;

    if (tile == originalStartTile)
    {
        return 1;
    }

    if (!g_origMoveAbilityBuildPreviewPath)
    {
        return 0;
    }

    tempPath.capacity = MAX_MANUAL_PATH_TILES;
    tempPath.count = 0;
    tempPath.data = tempTiles;

    for (index = 0; index < MAX_MANUAL_PATH_TILES; index++)
    {
        tempTiles[index] = 0;
    }

    resultPath = NULL;

    __try
    {
        resultPath = g_origMoveAbilityBuildPreviewPath(ability, &tempPath, originalStartTile, tile);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Original movement-zone probe exception: source=%s start=(%d,%d) tile=(%d,%d)", sourceName ? sourceName : "manual", TileX(originalStartTile), TileY(originalStartTile), TileX(tile), TileY(tile));
        return 0;
    }

    path = resultPath ? resultPath : &tempPath;

    if (IsPathBufferEndingAtTile(path, tile))
    {
        return 1;
    }

    Log("Rejected %s tile outside original movement zone: start=(%d,%d) tile=(%d,%d)", sourceName ? sourceName : "manual", TileX(originalStartTile), TileY(originalStartTile), TileX(tile), TileY(tile));
    return 0;
}

// Verifies that candidate can be reached from the original drag start inside movement range...
static int NativeRouteFromOriginalStartUsesCandidateInRangeNoLock(void* ability, uint64_t candidateTile, uint64_t previewTargetTile, const char* sourceName)
{
    uint64_t probeTargets[10];
    uint64_t originalStartTile;
    uint64_t lastTile;
    int32_t candidateX;
    int32_t candidateY;
    int32_t lastX;
    int32_t lastY;
    int32_t stepX;
    int32_t stepY;
    int32_t probeCount;
    int32_t index;
    int pathWasReachable;

    if (!ability || candidateTile == 0)
    {
        return 0;
    }

    if (g_dragStartTile == 0 || g_dragCount <= 0)
    {
        return 0;
    }

    originalStartTile = g_dragStartTile;

    if (candidateTile == originalStartTile)
    {
        return 1;
    }

    if (!IsTileInsideOriginalMovementZoneNoLock(ability, candidateTile, sourceName))
    {
        return 0;
    }

    for (index = 0; index < 10; index++)
    {
        probeTargets[index] = 0;
    }

    candidateX = TileX(candidateTile);
    candidateY = TileY(candidateTile);
    lastTile = g_dragTiles[g_dragCount - 1];
    lastX = TileX(lastTile);
    lastY = TileY(lastTile);
    stepX = candidateX - lastX;
    stepY = candidateY - lastY;
    probeCount = 0;

    if (previewTargetTile != 0 && previewTargetTile != originalStartTile)
    {
        AddUniqueProbeTargetNoLock(probeTargets, &probeCount, 10, previewTargetTile);
    }

    if ((AbsI32(stepX) + AbsI32(stepY)) == 1)
    {
        AddUniqueProbeTargetNoLock(probeTargets, &probeCount, 10, MakeTile(candidateX + stepX, candidateY + stepY));
    }

    AddUniqueProbeTargetNoLock(probeTargets, &probeCount, 10, MakeTile(candidateX + 1, candidateY));
    AddUniqueProbeTargetNoLock(probeTargets, &probeCount, 10, MakeTile(candidateX - 1, candidateY));
    AddUniqueProbeTargetNoLock(probeTargets, &probeCount, 10, MakeTile(candidateX, candidateY + 1));
    AddUniqueProbeTargetNoLock(probeTargets, &probeCount, 10, MakeTile(candidateX, candidateY - 1));

    for (index = 0; index < probeCount; index++)
    {
        if (probeTargets[index] == 0 || probeTargets[index] == originalStartTile)
        {
            continue;
        }

        pathWasReachable = 0;
        DoesNativePreviewPathContainTileNoLock(ability, originalStartTile, probeTargets[index], candidateTile, &pathWasReachable);

        if (pathWasReachable)
        {
            Log("Accepted inferred %s tile inside original endpoint envelope: start=(%d,%d) candidate=(%d,%d) probe=(%d,%d)", sourceName ? sourceName : "cursor", TileX(originalStartTile), TileY(originalStartTile), TileX(candidateTile), TileY(candidateTile), TileX(probeTargets[index]), TileY(probeTargets[index]));
            return 1;
        }
    }

    Log("Rejected inferred %s tile outside original endpoint envelope: start=(%d,%d) candidate=(%d,%d) preview=(%d,%d)", sourceName ? sourceName : "cursor", TileX(originalStartTile), TileY(originalStartTile), TileX(candidateTile), TileY(candidateTile), TileX(previewTargetTile), TileY(previewTargetTile));
    return 0;
}

// Combines all native transit/movement-zone checks before accepting an inferred hover tile...
static int IsInferredTransitTileAllowedNoLock(void* ability, uint64_t candidateTile, uint64_t previewTargetTile, const char* sourceName)
{
    uint64_t lastTile;
    int pathWasReachable;

    if (candidateTile == 0)
    {
        return 0;
    }

    if (candidateTile == previewTargetTile)
    {
        return 1;
    }

    // The occupied origin is not a legal native transit tile, but hovering it means "cancel"...
    if (g_dragStartTile != 0 && candidateTile == g_dragStartTile)
    {
        return 1;
    }

    if (g_dragCount <= 0)
    {
        return 1;
    }

    lastTile = g_dragTiles[g_dragCount - 1];

    if (lastTile == candidateTile)
    {
        return 1;
    }

    pathWasReachable = 0;

    if (NativeRouteUsesCandidateAsShortTransitNoLock(ability, lastTile, candidateTile, previewTargetTile, sourceName))
    {
        if (NativeRouteFromOriginalStartUsesCandidateInRangeNoLock(ability, candidateTile, previewTargetTile, sourceName))
        {
            return 1;
        }

        return 0;
    }

    if (previewTargetTile != 0 && DoesNativePreviewPathUseImmediateTransitTileNoLock(ability, lastTile, previewTargetTile, candidateTile, &pathWasReachable))
    {
        if (NativeRouteFromOriginalStartUsesCandidateInRangeNoLock(ability, candidateTile, previewTargetTile, sourceName))
        {
            Log("Accepted inferred %s tile because preview route uses it as immediate transit and original range contains it: last=(%d,%d) candidate=(%d,%d) preview=(%d,%d)", sourceName ? sourceName : "cursor", TileX(lastTile), TileY(lastTile), TileX(candidateTile), TileY(candidateTile), TileX(previewTargetTile), TileY(previewTargetTile));
            return 1;
        }

        return 0;
    }

    Log("Rejected inferred %s tile with no native immediate-transit proof: last=(%d,%d) candidate=(%d,%d) preview=(%d,%d) previewReachable=%d", sourceName ? sourceName : "cursor", TileX(lastTile), TileY(lastTile), TileX(candidateTile), TileY(candidateTile), TileX(previewTargetTile), TileY(previewTargetTile), pathWasReachable);
    return 0;
}

// Appends a raw/cursor-inferred tile only after transit validation succeeds...
static int AddInferredDragTile(void* ability, uint64_t tile, uint64_t previewTargetTile, const char* sourceName)
{
    if (!IsInferredTransitTileAllowedNoLock(ability, tile, previewTargetTile, sourceName))
    {
        return 0;
    }

    AddDragTile(ability, tile);
    return 1;
}

// Returns true when a low-confidence candidate would rewind an already-authored path...
static int IsEarlierDragTileNoLock(uint64_t tile)
{
    int32_t index;

    if (tile == 0 || g_dragCount <= 1)
    {
        return 0;
    }

    for (index = 0; index < (g_dragCount - 1); index++)
    {
        if (g_dragTiles[index] == tile)
        {
            return 1;
        }
    }

    return 0;
}

// Cursor-derived candidates are supplemental transit hints and must never pull the endpoint backward...
static int AddSupplementalInferredDragTile(void* ability, uint64_t tile, uint64_t previewTargetTile, const char* sourceName)
{
    if (IsEarlierDragTileNoLock(tile))
    {
        Log("Ignored stale supplemental %s tile that would rewind the drag: tile=(%d,%d) end=(%d,%d)", sourceName ? sourceName : "cursor", TileX(tile), TileY(tile), TileX(g_dragEndTile), TileY(g_dragEndTile));
        return 0;
    }

    return AddInferredDragTile(ability, tile, previewTargetTile, sourceName);
}

// Queues timer-discovered candidate so it can be consumed by the next preview hook call...
static void QueueFrameDragCandidateNoLock(uint64_t tile, uint64_t previewTargetTile, const char* sourceName)
{
    FrameDragCandidate* candidate;
    int32_t index;

    if (tile == 0)
    {
        return;
    }

    for (index = 0; index < g_frameDragCandidateCount; index++)
    {
        if (g_frameDragCandidates[index].tile == tile)
        {
            g_frameDragCandidates[index].previewTargetTile = previewTargetTile;
            g_frameDragCandidates[index].tick = GetTickCount64();
            strncpy(g_frameDragCandidates[index].sourceName, sourceName ? sourceName : "frame", sizeof(g_frameDragCandidates[index].sourceName) - 1);
            g_frameDragCandidates[index].sourceName[sizeof(g_frameDragCandidates[index].sourceName) - 1] = '\0';
            return;
        }
    }

    candidate = &g_frameDragCandidates[g_frameDragCandidateWriteIndex];
    candidate->tile = tile;
    candidate->previewTargetTile = previewTargetTile;
    candidate->tick = GetTickCount64();
    strncpy(candidate->sourceName, sourceName ? sourceName : "frame", sizeof(candidate->sourceName) - 1);
    candidate->sourceName[sizeof(candidate->sourceName) - 1] = '\0';

    if (g_frameDragCandidateCount < FRAME_DRAG_CANDIDATE_COUNT)
    {
        g_frameDragCandidateCount++;
    }

    g_frameDragCandidateWriteIndex = (g_frameDragCandidateWriteIndex + 1) % FRAME_DRAG_CANDIDATE_COUNT;
}

// Drops queued frame candidates when drag state is reset or stale...
static void ClearFrameDragCandidatesNoLock(void)
{
    int32_t index;

    for (index = 0; index < FRAME_DRAG_CANDIDATE_COUNT; index++)
    {
        g_frameDragCandidates[index].tile = 0;
        g_frameDragCandidates[index].previewTargetTile = 0;
        g_frameDragCandidates[index].tick = 0;
        g_frameDragCandidates[index].sourceName[0] = '\0';
    }

    g_frameDragCandidateCount = 0;
    g_frameDragCandidateWriteIndex = 0;
}

// Consumes queued timer candidates against the current preview target...
static void DrainFrameDragCandidatesNoLock(void* ability, uint64_t previewTargetTile)
{
    FrameDragCandidate orderedCandidates[FRAME_DRAG_CANDIDATE_COUNT];
    ULONGLONG nowTick;
    uint64_t validationPreviewTargetTile;
    int32_t orderedCount;
    int32_t index;
    int32_t sourceIndex;

    if (!ability || g_frameDragCandidateCount <= 0)
    {
        return;
    }

    nowTick = GetTickCount64();
    orderedCount = 0;

    for (index = 0; index < g_frameDragCandidateCount; index++)
    {
        sourceIndex = (g_frameDragCandidateWriteIndex - g_frameDragCandidateCount + index + FRAME_DRAG_CANDIDATE_COUNT) % FRAME_DRAG_CANDIDATE_COUNT;

        if (g_frameDragCandidates[sourceIndex].tile == 0)
        {
            continue;
        }

        if ((nowTick - g_frameDragCandidates[sourceIndex].tick) > FRAME_DRAG_CANDIDATE_MAX_AGE_MS)
        {
            continue;
        }

        orderedCandidates[orderedCount] = g_frameDragCandidates[sourceIndex];
        orderedCount++;
    }

    ClearFrameDragCandidatesNoLock();

    for (index = 0; index < orderedCount; index++)
    {
        validationPreviewTargetTile = previewTargetTile != 0 ? previewTargetTile : orderedCandidates[index].previewTargetTile;
        AddSupplementalInferredDragTile(ability, orderedCandidates[index].tile, validationPreviewTargetTile, orderedCandidates[index].sourceName);
    }
}

// Safely reads uint64 field from a native object using SEH to avoid bad pointer crashy crashies...
static int TryReadU64(void* base, UINT_PTR offset, uint64_t* outValue)
{
    uint8_t* bytes;

    if (!base || !outValue)
    {
        return 0;
    }

    bytes = (uint8_t*)base;

    __try
    {
        *outValue = *(uint64_t*)(bytes + offset);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Safely reads a signed 32-bit field from a native object...
static int TryReadI32(void* base, UINT_PTR offset, int32_t* outValue)
{
    uint8_t* bytes;

    if (!base || !outValue)
    {
        return 0;
    }

    bytes = (uint8_t*)base;

    __try
    {
        *outValue = *(int32_t*)(bytes + offset);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Safely writes uint64 field to a native object during prepared-target injection...
static int TryWriteU64(void* base, UINT_PTR offset, uint64_t value)
{
    uint8_t* bytes;

    if (!base)
    {
        return 0;
    }

    bytes = (uint8_t*)base;

    __try
    {
        *(uint64_t*)(bytes + offset) = value;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Safely reads a pointer field from a native object (Using SEH protection)...
static int TryReadPointer(void* base, UINT_PTR offset, void** outValue)
{
    uint8_t* bytes;

    if (!base || !outValue)
    {
        return 0;
    }

    bytes = (uint8_t*)base;

    __try
    {
        *outValue = *(void**)(bytes + offset);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Reads the owning Character* from MoveAbility...
static int TryGetAbilityCharacter(void* ability, void** outCharacter)
{
    return TryReadPointer(ability, MOVE_ABILITY_CHARACTER_OFFSET, outCharacter);
}

// Reads the character's current packed tile coordinate through its tile owner/component...
static int TryGetCharacterTile(void* character, uint64_t* outTile)
{
    void* tileOwner;

    if (!TryReadPointer(character, CHARACTER_TILE_OWNER_OFFSET, &tileOwner))
    {
        return 0;
    }

    return TryReadU64(tileOwner, CHARACTER_TILE_PACKED_OFFSET, outTile);
}

// Reads the tile-owner footprint code used by the game (1 means a 1x1 character)...
static int TryGetCharacterTileSize(void* character, int32_t* outSize)
{
    void* tileOwner;

    if (!TryReadPointer(character, CHARACTER_TILE_OWNER_OFFSET, &tileOwner))
    {
        return 0;
    }

    return TryReadI32(tileOwner, CHARACTER_TILE_SIZE_OFFSET, outSize);
}

// Manual paths are intentionally limited to characters occupying exactly one tile...
static int IsCharacterSingleTile(void* character, int32_t* outSize)
{
    int32_t size;

    size = 0;

    if (!TryGetCharacterTileSize(character, &size))
    {
        if (outSize)
        {
            *outSize = 0;
        }

        return 0;
    }

    if (outSize)
    {
        *outSize = size;
    }

    return (size == 1) ? 1 : 0;
}

// Resolves the ability owner before applying the 1x1-only manual-path rule...
static int IsAbilityCharacterSingleTile(void* ability, int32_t* outSize)
{
    void* character;

    if (!TryGetAbilityCharacter(ability, &character))
    {
        if (outSize)
        {
            *outSize = 0;
        }

        return 0;
    }

    return IsCharacterSingleTile(character, outSize);
}

// Extracts the current start tile and sanitized target tile from a MoveAbility...
static int TryGetAbilityStartAndTarget(void* ability, uint64_t* outStartTile, uint64_t* outTargetTile)
{
    void* character;

    if (!TryGetAbilityCharacter(ability, &character))
    {
        return 0;
    }

    if (!TryGetCharacterTile(character, outStartTile))
    {
        return 0;
    }

    if (!TryReadU64(ability, MOVE_ABILITY_TARGET_TILE_OFFSET, outTargetTile))
    {
        return 0;
    }

    return 1;
}

// Retrieves the pending turn action required when auto-triggering a prepared move...
static void* TryGetPendingTurnActionForAbility(void* ability)
{
    void* character;
    void* pendingAbility;
    uint8_t* characterBytes;
    void* turnAction;

    if (!ability)
    {
        return NULL;
    }

    if (!TryGetAbilityCharacter(ability, &character))
    {
        Log("Ability trigger skipped: could not read ability character");
        return NULL;
    }

    pendingAbility = NULL;

    if (TryReadPointer(character, CHARACTER_PENDING_ABILITY_OFFSET, &pendingAbility) && pendingAbility && pendingAbility != ability)
    {
        Log("Ability trigger skipped: character pending ability mismatch pending=%p ability=%p", pendingAbility, ability);
        return NULL;
    }

    characterBytes = (uint8_t*)character;
    turnAction = NULL;

    __try
    {
        turnAction = (void*)(characterBytes + CHARACTER_PENDING_TURN_ACTION_OFFSET);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        turnAction = NULL;
    }

    return turnAction;
}

// Reads and filters the raw hover tile before vanilla preview sanitization...
static int TryGetAbilityRawHoverTile(void* ability, uint64_t previewTargetTile, uint64_t* outTile)
{
    uint64_t primaryTile;
    uint64_t secondaryTile;

    if (!outTile)
    {
        return 0;
    }

    *outTile = 0;

    if (!ability)
    {
        return 0;
    }

    primaryTile = 0;
    secondaryTile = 0;

    TryReadU64(ability, MOVE_ABILITY_RAW_HOVER_TILE_OFFSET, &primaryTile);
    TryReadU64(ability, MOVE_ABILITY_TARGET_TILE_OFFSET, &secondaryTile);

    if (IsPlausibleTile(primaryTile) && primaryTile != previewTargetTile)
    {
        *outTile = primaryTile;
        return 1;
    }

    if (IsPlausibleTile(secondaryTile) && secondaryTile != previewTargetTile)
    {
        *outTile = secondaryTile;
        return 1;
    }

    if (IsPlausibleTile(primaryTile))
    {
        *outTile = primaryTile;
        return 1;
    }

    if (IsPlausibleTile(secondaryTile))
    {
        *outTile = secondaryTile;
        return 1;
    }

    return 0;
}

// Rechecks the live raw/cursor hover and immediately truncates the authored path to its origin...
// (This closes release/trigger races where vanilla has already sanitized the selected tile to an adjacent move)...
static int ReturnDragToOriginIfCurrentlyHoveredNoLock(void* ability, uint64_t previewTargetTile, const char* sourceName)
{
    uint64_t rawHoverTile;
    uint64_t inferredHoverTile;
    ULONGLONG nowTick;
    POINT cursor;
    LONG cursorDeltaX;
    LONG cursorDeltaY;
    int hasRawHoverTile;
    int hasInferredHoverTile;
    int anchoredOriginHover;
    int liveOriginHover;
    int recentOriginHover;

    if (!ability || g_dragStartTile == 0 || g_dragCount <= 0)
    {
        return 0;
    }

    rawHoverTile = 0;
    inferredHoverTile = 0;
    hasRawHoverTile = TryGetAbilityRawHoverTile(ability, previewTargetTile, &rawHoverTile);
    hasInferredHoverTile = TryInferCursorTileNoLock(&inferredHoverTile);
    recentOriginHover = 0;
    anchoredOriginHover = 0;
    cursor.x = 0;
    cursor.y = 0;

    if (GetCursorPos(&cursor) && g_hasDragBeginCursor)
    {
        // The mouse-down point is a calibration-free proof that the cursor began on the occupied tile.
        // Use a tight anchor even when calibration exists, and a slightly wider bootstrap anchor 
        // only while cursor-to-tile inference is unavailable...
        if (IsCursorNearDragBeginNoLock(cursor, ORIGIN_DRAG_BEGIN_STRONG_RADIUS_PIXELS) || (!hasInferredHoverTile && IsCursorNearDragBeginNoLock(cursor, ORIGIN_DRAG_BEGIN_BOOTSTRAP_RADIUS_PIXELS)))
        {
            anchoredOriginHover = 1;
        }
    }

    liveOriginHover = (previewTargetTile == g_dragStartTile || (hasRawHoverTile && rawHoverTile == g_dragStartTile) || (hasInferredHoverTile && inferredHoverTile == g_dragStartTile) || anchoredOriginHover);

    if (liveOriginHover)
    {
        g_originHoverConfirmedTick = GetTickCount64();
        g_originHoverConfirmedCursor = cursor;

        // Never teach the calibration that vanilla's sanitized adjacent target was under
        // the cursor when the cursor is actually back on the occupied origin tile...
        RememberTileScreenSampleNoLock(g_dragStartTile);
    }
    else if (g_originHoverConfirmedTick != 0 && g_dragEndTile == g_dragStartTile)
    {
        nowTick = GetTickCount64();
        cursorDeltaX = AbsLong(cursor.x - g_originHoverConfirmedCursor.x);
        cursorDeltaY = AbsLong(cursor.y - g_originHoverConfirmedCursor.y);

        if ((nowTick - g_originHoverConfirmedTick) <= ORIGIN_HOVER_CONFIRM_WINDOW_MS &&
            cursorDeltaX <= ORIGIN_HOVER_CONFIRM_CURSOR_DRIFT_PIXELS &&
            cursorDeltaY <= ORIGIN_HOVER_CONFIRM_CURSOR_DRIFT_PIXELS)
        {
            recentOriginHover = 1;
        }
        else
        {
            g_originHoverConfirmedTick = 0;
            g_originHoverConfirmedCursor.x = 0;
            g_originHoverConfirmedCursor.y = 0;
        }
    }

    if (!liveOriginHover && !recentOriginHover)
    {
        return 0;
    }

    if (!AppendDragTileDirectNoLock(ability, g_dragStartTile))
    {
        Log("Failed to truncate manual drag to hovered origin: source=%s start=(%d,%d)", sourceName ? sourceName : "origin-refresh", TileX(g_dragStartTile), TileY(g_dragStartTile));
        return 0;
    }

    ClearFrameDragCandidatesNoLock();

    if (liveOriginHover)
    {
        Log("Manual drag live origin hover confirmed: source=%s start=(%d,%d) preview=(%d,%d) raw=(%d,%d) inferred=(%d,%d) anchored=%d calibrated=%d", sourceName ? sourceName : "origin-refresh", TileX(g_dragStartTile), TileY(g_dragStartTile), TileX(previewTargetTile), TileY(previewTargetTile), TileX(rawHoverTile), TileY(rawHoverTile), TileX(inferredHoverTile), TileY(inferredHoverTile), anchoredOriginHover, g_tileScreenCalibrationReady);
    }
    else
    {
        Log("Manual drag preserved recent origin hover through sanitized release: source=%s start=(%d,%d)", sourceName ? sourceName : "origin-refresh", TileX(g_dragStartTile), TileY(g_dragStartTile));
    }

    return 1;
}

// Resets all in-progress drag state after release, rejection, or shutdown...
static void ClearDragPath(void)
{
    g_pendingPreviewRelease = 0;
    g_manualDragArmed = 0;
    g_dragBeginCursor.x = 0;
    g_dragBeginCursor.y = 0;
    g_hasDragBeginCursor = 0;
    g_dragBeginTick = 0;
    g_lastDragPreviewTick = 0;
    g_lastDragPreviewTargetTile = 0;
    g_lastDragPreviewCursor.x = 0;
    g_lastDragPreviewCursor.y = 0;
    g_previousDragPreviewCursor.x = 0;
    g_previousDragPreviewCursor.y = 0;
    g_hasPreviousDragPreviewCursor = 0;
    g_rawHoverDifferentCount = 0;
    g_dragCount = 0;
    g_dragStartTile = 0;
    g_dragEndTile = 0;
    g_dragAbility = NULL;
    g_originHoverConfirmedTick = 0;
    g_originHoverConfirmedCursor.x = 0;
    g_originHoverConfirmedCursor.y = 0;
}

// Records the latest preview target/cursor used for fresh-release validation...
static void TouchDragPreviewNoLock(uint64_t targetTile)
{
    POINT cursor;

    cursor.x = 0;
    cursor.y = 0;

    GetCursorPos(&cursor);

    if (g_lastDragPreviewTick != 0)
    {
        g_previousDragPreviewCursor = g_lastDragPreviewCursor;
        g_hasPreviousDragPreviewCursor = 1;
    }

    g_lastDragPreviewTick = GetTickCount64();
    g_lastDragPreviewTargetTile = targetTile;
    g_lastDragPreviewCursor = cursor;
}

// Absolute integer helper for cursor drift checks...
static LONG AbsLong(LONG value)
{
    return (value < 0) ? -value : value;
}

// Tests the current cursor against the true occupied-tile point captured on mouse-down...
static int IsCursorNearDragBeginNoLock(POINT cursor, LONG radiusPixels)
{
    int64_t deltaX;
    int64_t deltaY;
    int64_t radius;

    if (!g_hasDragBeginCursor || radiusPixels <= 0)
    {
        return 0;
    }

    deltaX = (int64_t)cursor.x - (int64_t)g_dragBeginCursor.x;
    deltaY = (int64_t)cursor.y - (int64_t)g_dragBeginCursor.y;
    radius = (int64_t)radiusPixels;

    return ((deltaX * deltaX) + (deltaY * deltaY)) <= (radius * radius);
}

// Clears the post-release prepared path and disarms execution override state...
static void ClearPreparedPathNoLock(const char* reason)
{
    g_preparedCount = 0;
    g_preparedStartTile = 0;
    g_preparedEndTile = 0;
    g_preparedTick = 0;
    DisarmPreparedPathOverrideNoLock(reason);
}

// Clears a pending one-shot cancellation for a drag released on its origin tile...
static void ClearMoveCancellationNoLock(void)
{
    g_cancelMoveAbility = NULL;
    g_cancelMoveUntilTick = 0;
}

// Suppresses the next MoveAbility::OnTrigger generated by releasing a cancelled manual drag...
static void ArmMoveCancellationNoLock(void* ability, const char* reason)
{
    if (!ability)
    {
        ClearMoveCancellationNoLock();
        return;
    }

    g_cancelMoveAbility = ability;
    g_cancelMoveUntilTick = GetTickCount64() + CANCEL_MOVE_TRIGGER_WINDOW_MS;
    Log("Armed manual move cancellation: reason=%s ability=%p", reason ? reason : "returned to start", ability);
}

// Consumes a fresh cancellation only for the ability that authored the cancelled drag...
static int ConsumeMoveCancellationNoLock(void* ability)
{
    ULONGLONG nowTick;

    if (!g_cancelMoveAbility)
    {
        return 0;
    }

    nowTick = GetTickCount64();

    if (nowTick > g_cancelMoveUntilTick)
    {
        Log("Expired manual move cancellation for ability=%p", g_cancelMoveAbility);
        ClearMoveCancellationNoLock();
        return 0;
    }

    if (g_cancelMoveAbility != ability)
    {
        return 0;
    }

    Log("Consumed manual move cancellation for ability=%p", ability);
    ClearMoveCancellationNoLock();
    return 1;
}

// Starts the drag arming window when left mouse is pressed...
static void BeginManualDragCandidateNoLock(void)
{
    g_manualDragArmed = 0;
    g_dragBeginCursor.x = 0;
    g_dragBeginCursor.y = 0;
    g_hasDragBeginCursor = GetCursorPos(&g_dragBeginCursor) ? 1 : 0;
    g_dragBeginTick = GetTickCount64();
}

// Turns a held left-click into an armed manual drag after distance/time thresholds are met...
static int UpdateManualDragArmNoLock(const char* sourceName)
{
    POINT cursor;
    LONG deltaX;
    LONG deltaY;
    ULONGLONG ageMs;

    if (g_manualDragArmed)
    {
        return 1;
    }

    if (g_dragBeginTick == 0)
    {
        return 0;
    }

    cursor.x = 0;
    cursor.y = 0;
    GetCursorPos(&cursor);

    deltaX = AbsLong(cursor.x - g_dragBeginCursor.x);
    deltaY = AbsLong(cursor.y - g_dragBeginCursor.y);
    ageMs = GetTickCount64() - g_dragBeginTick;

    if ((deltaX >= MANUAL_DRAG_ARM_PIXELS || deltaY >= MANUAL_DRAG_ARM_PIXELS) && ageMs >= MANUAL_DRAG_ARM_MIN_HELD_MS)
    {
        g_manualDragArmed = 1;
        Log("Manual drag armed: source=%s dx=%ld dy=%ld ageMs=%llu count=%d", sourceName ? sourceName : "unknown", deltaX, deltaY, ageMs, g_dragCount);
        return 1;
    }

    return 0;
}

// Rejects stale releases where the cursor/preview moved too far after the last sample...
static int WasReleaseOverFreshPreviewTargetNoLock(void)
{
    ULONGLONG nowTick;
    ULONGLONG ageMs;

    if (g_dragCount < 2)
    {
        return 0;
    }

    if (g_lastDragPreviewTick == 0)
    {
        Log("Manual release cancelled: no live preview sample at release");
        return 0;
    }

    nowTick = GetTickCount64();
    ageMs = nowTick - g_lastDragPreviewTick;

    if (ageMs > RELEASE_PREVIEW_STALE_MS)
    {
        Log("Manual release cancelled: release was outside/stale preview target ageMs=%llu last=(%d,%d) end=(%d,%d)", ageMs, TileX(g_lastDragPreviewTargetTile), TileY(g_lastDragPreviewTargetTile), TileX(g_dragEndTile), TileY(g_dragEndTile));
        return 0;
    }

    {
        POINT releaseCursor;
        LONG deltaX;
        LONG deltaY;

        releaseCursor.x = 0;
        releaseCursor.y = 0;
        GetCursorPos(&releaseCursor);

        deltaX = releaseCursor.x - g_lastDragPreviewCursor.x;
        deltaY = releaseCursor.y - g_lastDragPreviewCursor.y;

        if (deltaX < 0)
        {
            deltaX = -deltaX;
        }

        if (deltaY < 0)
        {
            deltaY = -deltaY;
        }

        if (deltaX > RELEASE_CURSOR_DRIFT_PIXELS || deltaY > RELEASE_CURSOR_DRIFT_PIXELS)
        {
            Log("Manual release cancelled: cursor moved after last valid preview dx=%ld dy=%ld lastTile=(%d,%d) end=(%d,%d)", deltaX, deltaY, TileX(g_lastDragPreviewTargetTile), TileY(g_lastDragPreviewTargetTile), TileX(g_dragEndTile), TileY(g_dragEndTile));
            return 0;
        }
    }

    if (g_lastDragPreviewTargetTile != g_dragEndTile)
    {
        Log("Manual release cancelled: release preview target mismatch last=(%d,%d) end=(%d,%d)", TileX(g_lastDragPreviewTargetTile), TileY(g_lastDragPreviewTargetTile), TileX(g_dragEndTile), TileY(g_dragEndTile));
        return 0;
    }

    return 1;
}

// Appends a single adjacent manual tile after native one-step validation...
static int AppendDragTileDirectNoLock(void* ability, uint64_t tile)
{
    int32_t index;

    if (tile == 0)
    {
        return 0;
    }

    if (g_dragCount > 0 && g_dragTiles[g_dragCount - 1] == tile)
    {
        return 1;
    }

    for (index = 0; index < g_dragCount; index++)
    {
        if (g_dragTiles[index] == tile)
        {
            g_dragCount = index + 1;
            g_dragEndTile = tile;
            return 1;
        }
    }

    if (g_dragStartTile != 0 && !IsTileInsideOriginalMovementZoneNoLock(ability, tile, "manual-append"))
    {
        return 0;
    }

    if (g_dragCount > 0 && !IsManualStepTileNoLock(ability, g_dragTiles[g_dragCount - 1], tile))
    {
        return 0;
    }

    if (g_dragCount >= MAX_MANUAL_PATH_TILES)
    {
        return 0;
    }

    g_dragTiles[g_dragCount] = tile;
    g_dragCount++;
    g_dragEndTile = tile;
    Log("Drag append: count=%d tile=(%d,%d)", g_dragCount, TileX(tile), TileY(tile));
    return 1;
}

// Dot product helper used to compare approximate screen/tile drag directions...
static int32_t DotI32(int32_t leftX, int32_t leftY, int32_t rightX, int32_t rightY)
{
    return (leftX * rightX) + (leftY * rightY);
}

// Maps grid deltas to approximate screen deltas for diagonal-corner decisions...
static void TileDeltaToApproxScreenDelta(int32_t tileDeltaX, int32_t tileDeltaY, int32_t* outScreenX, int32_t* outScreenY)
{
    if (outScreenX)
    {
        *outScreenX = tileDeltaX - tileDeltaY;
    }

    if (outScreenY)
    {
        *outScreenY = tileDeltaX + tileDeltaY;
    }
}

// Chooses which orthogonal corner should bridge a diagonal manual move...
static uint64_t ChooseGeneralDiagonalCornerNoLock(uint64_t startTile, uint64_t targetTile, uint64_t candidateOne, uint64_t candidateTwo, uint64_t vanillaMiddleTile)
{
    int32_t cursorDeltaX;
    int32_t cursorDeltaY;
    int32_t candidateOneScreenX;
    int32_t candidateOneScreenY;
    int32_t candidateTwoScreenX;
    int32_t candidateTwoScreenY;
    int32_t candidateOneScore;
    int32_t candidateTwoScore;
    int32_t startX;
    int32_t startY;
    int32_t candidateOneX;
    int32_t candidateOneY;
    int32_t candidateTwoX;
    int32_t candidateTwoY;

    if (g_hasPreviousDragPreviewCursor)
    {
        cursorDeltaX = (int32_t)(g_lastDragPreviewCursor.x - g_previousDragPreviewCursor.x);
        cursorDeltaY = (int32_t)(g_lastDragPreviewCursor.y - g_previousDragPreviewCursor.y);

        if (AbsI32(cursorDeltaX) >= 2 || AbsI32(cursorDeltaY) >= 2)
        {
            startX = TileX(startTile);
            startY = TileY(startTile);
            candidateOneX = TileX(candidateOne);
            candidateOneY = TileY(candidateOne);
            candidateTwoX = TileX(candidateTwo);
            candidateTwoY = TileY(candidateTwo);

            TileDeltaToApproxScreenDelta(candidateOneX - startX, candidateOneY - startY, &candidateOneScreenX, &candidateOneScreenY);
            TileDeltaToApproxScreenDelta(candidateTwoX - startX, candidateTwoY - startY, &candidateTwoScreenX, &candidateTwoScreenY);

            candidateOneScore = DotI32(cursorDeltaX, cursorDeltaY, candidateOneScreenX, candidateOneScreenY);
            candidateTwoScore = DotI32(cursorDeltaX, cursorDeltaY, candidateTwoScreenX, candidateTwoScreenY);

            if (candidateOneScore > candidateTwoScore)
            {
                Log("Directional diagonal corner intent: start=(%d,%d) target=(%d,%d) chosen=(%d,%d) other=(%d,%d) cursorDelta=(%d,%d) scores=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile), TileX(candidateOne), TileY(candidateOne), TileX(candidateTwo), TileY(candidateTwo), cursorDeltaX, cursorDeltaY, candidateOneScore, candidateTwoScore);
                return candidateOne;
            }

            if (candidateTwoScore > candidateOneScore)
            {
                Log("Directional diagonal corner intent: start=(%d,%d) target=(%d,%d) chosen=(%d,%d) other=(%d,%d) cursorDelta=(%d,%d) scores=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile), TileX(candidateTwo), TileY(candidateTwo), TileX(candidateOne), TileY(candidateOne), cursorDeltaX, cursorDeltaY, candidateTwoScore, candidateOneScore);
                return candidateTwo;
            }
        }
    }

    if (vanillaMiddleTile == candidateOne)
    {
        return candidateTwo;
    }

    if (vanillaMiddleTile == candidateTwo)
    {
        return candidateOne;
    }

    return 0;
}

// Handles one-by-one diagonal drags by inserting a validated orthogonal corner first...
static int TryAppendGeneralDiagonalCornerBridgeNoLock(void* ability, uint64_t targetTile)
{
    ManualPathBuffer tempPath;
    ManualPathBuffer* resultPath;
    ManualPathBuffer* path;
    uint64_t tempTiles[MAX_MANUAL_PATH_TILES];
    uint64_t originalTiles[MAX_MANUAL_PATH_TILES];
    uint64_t startTile;
    uint64_t candidateOne;
    uint64_t candidateTwo;
    uint64_t vanillaMiddleTile;
    uint64_t preferredCornerTile;
    int32_t originalCount;
    uint64_t originalEndTile;
    int32_t startX;
    int32_t startY;
    int32_t targetX;
    int32_t targetY;
    int32_t deltaX;
    int32_t deltaY;
    int32_t index;

    if (!ENABLE_OPPOSITE_DIAGONAL_CORNER_BRIDGE || !ability || !g_origMoveAbilityBuildPreviewPath || g_dragCount <= 0 || targetTile == 0)
    {
        return 0;
    }

    startTile = g_dragTiles[g_dragCount - 1];

    if (startTile == targetTile)
    {
        return 0;
    }

    startX = TileX(startTile);
    startY = TileY(startTile);
    targetX = TileX(targetTile);
    targetY = TileY(targetTile);
    deltaX = targetX - startX;
    deltaY = targetY - startY;

    if (AbsI32(deltaX) != 1 || AbsI32(deltaY) != 1)
    {
        return 0;
    }

    candidateOne = MakeTile(startX, targetY);
    candidateTwo = MakeTile(targetX, startY);

    tempPath.capacity = MAX_MANUAL_PATH_TILES;
    tempPath.count = 0;
    tempPath.data = tempTiles;

    for (index = 0; index < MAX_MANUAL_PATH_TILES; index++)
    {
        tempTiles[index] = 0;
        originalTiles[index] = 0;
    }

    resultPath = NULL;

    __try
    {
        resultPath = g_origMoveAbilityBuildPreviewPath(ability, &tempPath, startTile, targetTile);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("General diagonal corner probe exception: start=(%d,%d) target=(%d,%d)", startX, startY, targetX, targetY);
        return 0;
    }

    path = resultPath ? resultPath : &tempPath;

    if (!path || !path->data || path->count != 3)
    {
        return 0;
    }

    if (path->data[0] != startTile || path->data[2] != targetTile)
    {
        return 0;
    }

    vanillaMiddleTile = path->data[1];

    if (vanillaMiddleTile != candidateOne && vanillaMiddleTile != candidateTwo)
    {
        return 0;
    }

    preferredCornerTile = ChooseGeneralDiagonalCornerNoLock(startTile, targetTile, candidateOne, candidateTwo, vanillaMiddleTile);

    if (preferredCornerTile == 0)
    {
        return 0;
    }

    originalCount = g_dragCount;
    originalEndTile = g_dragEndTile;

    for (index = 0; index < g_dragCount && index < MAX_MANUAL_PATH_TILES; index++)
    {
        originalTiles[index] = g_dragTiles[index];
    }

    if (IsInferredTransitTileAllowedNoLock(ability, preferredCornerTile, targetTile, "general-diagonal-corner"))
    {
        if (AppendDragTileDirectNoLock(ability, preferredCornerTile) && AppendDragTileDirectNoLock(ability, targetTile))
        {
            Log("General diagonal corner bridge: start=(%d,%d) vanillaCorner=(%d,%d) preferredCorner=(%d,%d) target=(%d,%d)", startX, startY, TileX(vanillaMiddleTile), TileY(vanillaMiddleTile), TileX(preferredCornerTile), TileY(preferredCornerTile), targetX, targetY);
            return 1;
        }
    }
    else
    {
        Log("Rejected preferred diagonal corner without native transit proof: start=(%d,%d) preferredCorner=(%d,%d) target=(%d,%d)", startX, startY, TileX(preferredCornerTile), TileY(preferredCornerTile), targetX, targetY);
    }

    g_dragCount = originalCount;
    g_dragEndTile = originalEndTile;

    for (index = 0; index < originalCount && index < MAX_MANUAL_PATH_TILES; index++)
    {
        g_dragTiles[index] = originalTiles[index];
    }

    if (IsInferredTransitTileAllowedNoLock(ability, vanillaMiddleTile, targetTile, "general-diagonal-vanilla-corner"))
    {
        if (AppendDragTileDirectNoLock(ability, vanillaMiddleTile) && AppendDragTileDirectNoLock(ability, targetTile))
        {
            Log("General diagonal corner bridge fallback: start=(%d,%d) vanillaCorner=(%d,%d) target=(%d,%d)", startX, startY, TileX(vanillaMiddleTile), TileY(vanillaMiddleTile), targetX, targetY);
            return 1;
        }
    }

    g_dragCount = originalCount;
    g_dragEndTile = originalEndTile;

    for (index = 0; index < originalCount && index < MAX_MANUAL_PATH_TILES; index++)
    {
        g_dragTiles[index] = originalTiles[index];
    }

    return 0;
}

// Restores drag tiles after a speculative bridge/path attempt fails...
static void RestoreDragSnapshotNoLock(uint64_t* originalTiles, int32_t originalCount, uint64_t originalEndTile)
{
    int32_t index;

    g_dragCount = originalCount;
    g_dragEndTile = originalEndTile;

    for (index = 0; index < originalCount && index < MAX_MANUAL_PATH_TILES; index++)
    {
        g_dragTiles[index] = originalTiles[index];
    }
}

// Ensures a tile appears on a reachable native route from the original drag context...
static int IsTileOnReachableOriginalPreviewRouteNoLock(void* ability, uint64_t tile, uint64_t targetTile, const char* sourceName)
{
    int pathWasReachable;

    if (!ability || tile == 0 || targetTile == 0 || g_dragStartTile == 0)
    {
        return 0;
    }

    if (tile == g_dragStartTile)
    {
        return 1;
    }

    if (IsTileInsideOriginalMovementZoneNoLock(ability, tile, sourceName))
    {
        return 1;
    }

    pathWasReachable = 0;
    DoesNativePreviewPathContainTileNoLock(ability, g_dragStartTile, targetTile, tile, &pathWasReachable);
    Log("Rejected %s tile outside original movement zone: start=(%d,%d) tile=(%d,%d) target=(%d,%d) previewContainsTile=%d", sourceName ? sourceName : "bridge", TileX(g_dragStartTile), TileY(g_dragStartTile), TileX(tile), TileY(tile), TileX(targetTile), TileY(targetTile), pathWasReachable);
    return 0;
}

// Removes a "just-added preview target" if native reachability says it is invalid...
static void PruneUnreachablePreviewTargetNoLock(void* ability, uint64_t targetTile)
{
    if (!ability || targetTile == 0 || g_dragCount <= 1)
    {
        return;
    }

    if (g_lastPreviewWasReachable)
    {
        return;
    }

    if (g_dragEndTile == targetTile && g_dragTiles[g_dragCount - 1] == targetTile)
    {
        Log("Pruned unreachable preview endpoint from manual candidate path: tile=(%d,%d) countBefore=%d", TileX(targetTile), TileY(targetTile), g_dragCount);
        g_dragCount--;
        g_dragEndTile = g_dragTiles[g_dragCount - 1];
    }
}

// Falls back to the vanilla preview route to bridge multi-tile gaps when manual samples skip tiles...
static int TryAppendVanillaBridgeNoLock(void* ability, uint64_t targetTile)
{
    ManualPathBuffer tempPath;
    ManualPathBuffer* resultPath;
    ManualPathBuffer* path;
    uint64_t tempTiles[MAX_MANUAL_PATH_TILES];
    uint64_t originalTiles[MAX_MANUAL_PATH_TILES];
    uint64_t startTile;
    uint64_t originalEndTile;
    int32_t originalCount;
    int32_t index;
    int32_t remainingSlots;

    if (!ability || !g_origMoveAbilityBuildPreviewPath || g_dragCount <= 0 || targetTile == 0)
    {
        return 0;
    }

    startTile = g_dragTiles[g_dragCount - 1];

    if (startTile == targetTile || IsManualStepTileNoLock(ability, startTile, targetTile))
    {
        return 0;
    }

    tempPath.capacity = MAX_MANUAL_PATH_TILES;
    tempPath.count = 0;
    tempPath.data = tempTiles;

    for (index = 0; index < MAX_MANUAL_PATH_TILES; index++)
    {
        tempTiles[index] = 0;
        originalTiles[index] = 0;
    }

    resultPath = NULL;

    __try
    {
        resultPath = g_origMoveAbilityBuildPreviewPath(ability, &tempPath, startTile, targetTile);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Vanilla bridge path exception: start=(%d,%d) target=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile));
        return 0;
    }

    path = resultPath ? resultPath : &tempPath;

    if (!path || !path->data || path->count < 2 || path->count > MAX_MANUAL_PATH_TILES)
    {
        return 0;
    }

    if (!IsPathBufferEndingAtTile(path, targetTile))
    {
        return 0;
    }

    remainingSlots = MAX_MANUAL_PATH_TILES - g_dragCount;

    if ((path->count - 1) > remainingSlots)
    {
        Log("Vanilla bridge rejected: bridgeCount=%d remainingSlots=%d", path->count - 1, remainingSlots);
        return 0;
    }

    originalCount = g_dragCount;
    originalEndTile = g_dragEndTile;

    for (index = 0; index < g_dragCount && index < MAX_MANUAL_PATH_TILES; index++)
    {
        originalTiles[index] = g_dragTiles[index];
    }

    for (index = 1; index < path->count; index++)
    {
        if (!IsTileOnReachableOriginalPreviewRouteNoLock(ability, path->data[index], targetTile, "vanilla-bridge"))
        {
            RestoreDragSnapshotNoLock(originalTiles, originalCount, originalEndTile);
            return 0;
        }
    }

    Log("Vanilla bridge append: start=(%d,%d) target=(%d,%d) bridgeCount=%d", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile), path->count);

    for (index = 1; index < path->count; index++)
    {
        if (!AppendDragTileDirectNoLock(ability, path->data[index]))
        {
            RestoreDragSnapshotNoLock(originalTiles, originalCount, originalEndTile);
            return 0;
        }
    }

    return 1;
}

static void AddDragTile(void* ability, uint64_t tile)
{
    if (AppendDragTileDirectNoLock(ability, tile))
    {
        return;
    }

    if (TryAppendGeneralDiagonalCornerBridgeNoLock(ability, tile))
    {
        return;
    }

    if (TryAppendVanillaBridgeNoLock(ability, tile))
    {
        return;
    }

    if (g_dragCount > 0 && tile != 0)
    {
        Log("Ignored non-adjacent drag tile: last=(%d,%d) next=(%d,%d)", TileX(g_dragTiles[g_dragCount - 1]), TileY(g_dragTiles[g_dragCount - 1]), TileX(tile), TileY(tile));
    }
}

static int IsPathBufferEndingAtTile(ManualPathBuffer* path, uint64_t targetTile)
{
    uint64_t lastTile;

    if (!path || !path->data || path->count < 2)
    {
        return 0;
    }

    __try
    {
        lastTile = path->data[path->count - 1];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return (lastTile == targetTile) ? 1 : 0;
}

// Caches whether the latest vanilla preview could reach its requested endpoint...
static void RememberPreviewReachabilityNoLock(void* ability, uint64_t startTile, uint64_t targetTile, ManualPathBuffer* originalPath)
{
    g_lastPreviewAbility = ability;
    g_lastPreviewStartTile = startTile;
    g_lastPreviewTargetTile = targetTile;
    g_lastPreviewWasReachable = IsPathBufferEndingAtTile(originalPath, targetTile);

    if (!g_lastPreviewWasReachable)
    {
        Log("Preview target not reachable by vanilla movement: start=(%d,%d) target=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile));
    }
}

// Validates that the prepared path endpoint matched a recent reachable native preview...
static int IsPreparedEndpointReachableNoLock(void)
{
    if (!g_dragAbility || g_preparedCount < 2)
    {
        return 0;
    }

    if (g_lastPreviewAbility != g_dragAbility)
    {
        Log("Manual release cancelled: no matching reachable preview for ability");
        return 0;
    }

    if (g_lastPreviewStartTile != g_preparedStartTile || g_lastPreviewTargetTile != g_preparedEndTile)
    {
        Log("Manual release cancelled: latest preview does not match release endpoint latest=(%d,%d)->(%d,%d) release=(%d,%d)->(%d,%d)", TileX(g_lastPreviewStartTile), TileY(g_lastPreviewStartTile), TileX(g_lastPreviewTargetTile), TileY(g_lastPreviewTargetTile), TileX(g_preparedStartTile), TileY(g_preparedStartTile), TileX(g_preparedEndTile), TileY(g_preparedEndTile));
        return 0;
    }

    if (!g_lastPreviewWasReachable)
    {
        Log("Manual release cancelled: release endpoint outside vanilla movement range end=(%d,%d)", TileX(g_preparedEndTile), TileY(g_preparedEndTile));
        return 0;
    }

    return 1;
}

static void DisarmPreparedPathOverrideNoLock(const char* reason)
{
    InterlockedExchange(&g_applyNextPath, 0);
    InterlockedExchange(&g_applyOverrideBudget, 0);
    g_applyOverrideUntilTick = 0;
    Log("Manual path override disarmed: reason=%s", reason ? reason : "unknown");
}

// Writes the prepared endpoint back to MoveAbility so Ability::Trigger fires at the manual target...
static void WritePreparedTargetToAbilityNoLock(void* ability)
{
    if (!ability || g_preparedCount < 2)
    {
        return;
    }

    if (TryWriteU64(ability, MOVE_ABILITY_TARGET_TILE_OFFSET, g_preparedEndTile))
    {
        Log("Wrote MoveAbility target tile before auto trigger: end=(%d,%d)", TileX(g_preparedEndTile), TileY(g_preparedEndTile));
    }
    else
    {
        Log("Failed to write MoveAbility target tile before auto trigger");
    }
}

// Opens the short post-release window during which execution path builders are overwritten...
static void ArmPreparedPathOverrideNoLock(const char* reason)
{
    g_applyOverrideUntilTick = GetTickCount64() + APPLY_PATH_OVERRIDE_WINDOW_MS;
    InterlockedExchange(&g_applyOverrideBudget, APPLY_PATH_OVERRIDE_BUDGET);
    InterlockedExchange(&g_applyNextPath, 1);
    Log("Armed manual path override: reason=%s count=%d budget=%d", reason ? reason : "unknown", g_preparedCount, APPLY_PATH_OVERRIDE_BUDGET);
}

// Automatically triggers the prepared move on mouse release when auto-move is enabled...
static void TriggerPreparedMoveNoLock(void)
{
    UINT_PTR gameBase;
    fn_move_ability_on_trigger triggerFunction;
    void* ability;
    int32_t beforeApplyCount;

    if (InterlockedCompareExchange(&g_autoMoveOnRelease, 0, 0) == 0)
    {
        return;
    }

    if (g_preparedCount < 2 || !g_dragAbility)
    {
        return;
    }

    if (InterlockedCompareExchange(&g_autoTriggerBusy, 1, 0) != 0)
    {
        Log("Auto move skipped: trigger already busy");
        return;
    }

    ability = g_dragAbility;
    beforeApplyCount = g_executionApplyCount;
    WritePreparedTargetToAbilityNoLock(ability);
    ArmPreparedPathOverrideNoLock("auto-trigger");

    __try
    {
        if (g_abilityTrigger)
        {
            void* turnAction;

            turnAction = TryGetPendingTurnActionForAbility(ability);

            if (turnAction)
            {
                Log("Auto move release: calling Ability::trigger with prepared path count=%d", g_preparedCount);
                g_abilityTrigger(ability, turnAction);
            }
            else if (g_origMoveAbilityOnTrigger)
            {
                Log("Auto move release fallback: calling original MoveAbility::OnTrigger with prepared path count=%d", g_preparedCount);
                g_origMoveAbilityOnTrigger(ability);
            }
        }
        else if (g_origMoveAbilityOnTrigger)
        {
            Log("Auto move release fallback: calling original MoveAbility::OnTrigger with prepared path count=%d", g_preparedCount);
            g_origMoveAbilityOnTrigger(ability);
        }
        else
        {
            gameBase = GetGameBase();

            if (!gameBase)
            {
                Log("Auto move skipped: game base unavailable");
            }
            else
            {
                triggerFunction = (fn_move_ability_on_trigger)(gameBase + RVA_MOVE_ABILITY_ON_TRIGGER);
                Log("Auto move release fallback: calling direct MoveAbility::OnTrigger with prepared path count=%d", g_preparedCount);
                triggerFunction(ability);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Auto move exception while calling MoveAbility::OnTrigger");
    }

    if (g_executionApplyCount > beforeApplyCount)
    {
        DisarmPreparedPathOverrideNoLock("auto-trigger completed after execution replacement");
    }

    InterlockedExchange(&g_autoTriggerBusy, 0);
}

// Copies the validated drag path into prepared storage and arms the execution override...
static void PrepareDragPathNoLock(void)
{
    int32_t index;
    int32_t characterSize;

    g_preparedCount = 0;
    g_preparedStartTile = 0;
    g_preparedEndTile = 0;
    g_preparedTick = 0;

    if (!g_manualDragArmed)
    {
        Log("Drag end ignored: click/no manual drag intent count=%d", g_dragCount);
        return;
    }

    characterSize = 0;

    if (!IsAbilityCharacterSingleTile(g_dragAbility, &characterSize))
    {
        Log("Drag end ignored for non-1x1 character: sizeCode=%d ability=%p", characterSize, g_dragAbility);
        DisarmPreparedPathOverrideNoLock("non-1x1 character");
        return;
    }

    // Refresh the actual cursor/raw hover at release, (because the final preview callback may not run)...
    ReturnDragToOriginIfCurrentlyHoveredNoLock(g_dragAbility, g_lastDragPreviewTargetTile, "release-prepare");

    if (g_dragStartTile != 0 && g_dragEndTile == g_dragStartTile)
    {
        ArmMoveCancellationNoLock(g_dragAbility, "manual drag released on origin tile");
        DisarmPreparedPathOverrideNoLock("manual drag returned to origin");
        Log("Drag end cancelled at origin: start=(%d,%d)", TileX(g_dragStartTile), TileY(g_dragStartTile));
        return;
    }

    if (g_dragCount < 2)
    {
        Log("Drag end ignored: count=%d", g_dragCount);
        return;
    }

    if ((g_dragCount - 1) > MAX_MANUAL_STEPS)
    {
        Log("Drag end rejected: steps=%d max=%d", g_dragCount - 1, MAX_MANUAL_STEPS);
        return;
    }

#if REQUIRE_FRESH_PREVIEW_ON_RELEASE
    if (!WasReleaseOverFreshPreviewTargetNoLock())
    {
        g_preparedCount = 0;
        g_preparedStartTile = 0;
        g_preparedEndTile = 0;
        g_preparedTick = 0;
        DisarmPreparedPathOverrideNoLock("release target not actively previewed");
        return;
    }
#else
    if (!WasReleaseOverFreshPreviewTargetNoLock())
    {
        Log("Manual release continuing without fresh vanilla preview validation");
    }
#endif

    for (index = 0; index < g_dragCount; index++)
    {
        g_preparedTiles[index] = g_dragTiles[index];
    }

    g_preparedCount = g_dragCount;
    g_preparedStartTile = g_dragTiles[0];
    g_preparedEndTile = g_dragTiles[g_dragCount - 1];
    g_preparedTick = GetTickCount64();

#if REQUIRE_VANILLA_ENDPOINT_REACHABILITY
    if (!IsPreparedEndpointReachableNoLock())
    {
        g_preparedCount = 0;
        g_preparedStartTile = 0;
        g_preparedEndTile = 0;
        g_preparedTick = 0;
        DisarmPreparedPathOverrideNoLock("release endpoint not reachable");
        return;
    }
#else
    if (!IsPreparedEndpointReachableNoLock())
    {
        Log("Manual release accepting endpoint that vanilla preview did not mark reachable");
    }
#endif

    ArmPreparedPathOverrideNoLock("drag-release");

    Log("Drag end prepared manual path: count=%d start=(%d,%d) end=(%d,%d)", g_preparedCount, TileX(g_preparedStartTile), TileY(g_preparedStartTile), TileX(g_preparedEndTile), TileY(g_preparedEndTile));

    TriggerPreparedMoveNoLock();
}

// Expires prepared paths so old drag state cannot affect later moves...
static int IsPreparedPathRecentNoLock(void)
{
    ULONGLONG nowTick;

    if (g_preparedCount < 2)
    {
        return 0;
    }

    nowTick = GetTickCount64();

    if ((nowTick - g_preparedTick) > PREPARED_PATH_WINDOW_MS)
    {
        return 0;
    }

    return 1;
}

// Optional strict guard against exceeding native reported maxMove...
static int ShouldRejectPreparedPathForReportedMaxMoveNoLock(int32_t reportedMaxMove, const char* context)
{
    int32_t steps;

    if (InterlockedCompareExchange(&g_reportedMaxMoveFallback, 0, 0) == 0)
    {
        return 0;
    }

    if (reportedMaxMove < 0 || g_preparedCount < 2)
    {
        return 0;
    }

    steps = g_preparedCount - 1;

    if (steps <= reportedMaxMove)
    {
        return 0;
    }

    Log("Reported maxMove fallback active: not applying manual %s path steps=%d reportedMaxMove=%d; vanilla pathfinding will run", context ? context : "execution", steps, reportedMaxMove);
    return 1;
}

// Checks all start/end/age/move-budget conditions before a prepared path can be used...
static int HasPreparedPathNoLock(uint64_t startTile, uint64_t targetTile, int32_t reportedMaxMove)
{
    if (!IsPreparedPathRecentNoLock())
    {
        return 0;
    }

    if (g_preparedStartTile != startTile || g_preparedEndTile != targetTile)
    {
        return 0;
    }

    if (ShouldRejectPreparedPathForReportedMaxMoveNoLock(reportedMaxMove, "matched"))
    {
        return 0;
    }

    return 1;
}

// Determines whether the execution hook should overwrite the native path this call...
static int ShouldForcePreparedExecutionNoLock(int32_t reportedMaxMove)
{
    LONG budget;
    ULONGLONG nowTick;

    if (InterlockedCompareExchange(&g_applyNextPath, 0, 0) == 0)
    {
        return 0;
    }

    if (!IsPreparedPathRecentNoLock())
    {
        InterlockedExchange(&g_applyNextPath, 0);
        InterlockedExchange(&g_applyOverrideBudget, 0);
        return 0;
    }

    nowTick = GetTickCount64();

    if (nowTick > g_applyOverrideUntilTick)
    {
        InterlockedExchange(&g_applyNextPath, 0);
        InterlockedExchange(&g_applyOverrideBudget, 0);
        Log("Manual path override expired before execution replacement");
        return 0;
    }

    if (ShouldRejectPreparedPathForReportedMaxMoveNoLock(reportedMaxMove, "forced"))
    {
        return 0;
    }

    budget = InterlockedCompareExchange(&g_applyOverrideBudget, 0, 0);

    if (budget <= 0)
    {
        InterlockedExchange(&g_applyNextPath, 0);
        Log("Manual path override budget exhausted");
        return 0;
    }

    return 1;
}

// Grows a native path buffer with the game's vector allocator before writing manual tiles...
static int EnsurePathCapacity(ManualPathBuffer* outPath, int32_t requiredCount)
{
    uint64_t byteCount;
    void* newData;

    if (!outPath || requiredCount <= 0 || !g_vectorReallocBytes)
    {
        return 0;
    }

    if (outPath->capacity >= requiredCount && outPath->data)
    {
        return 1;
    }

    byteCount = (uint64_t)requiredCount * sizeof(uint64_t);
    newData = g_vectorReallocBytes(outPath->data, byteCount);

    if (!newData)
    {
        return 0;
    }

    outPath->data = (uint64_t*)newData;
    outPath->capacity = requiredCount;

    return 1;
}

// Replaces a native preview path buffer with the prepared/manual tile list...
static void OverwritePathBufferFromTiles(ManualPathBuffer* outPath, const uint64_t* tiles, int32_t count, const char* reason)
{
    int32_t index;

    if (!outPath || !tiles || count < 2)
    {
        return;
    }

    __try
    {
        if (!EnsurePathCapacity(outPath, count))
        {
            Log("Failed to ensure path capacity for %s", reason);
            return;
        }

        for (index = 0; index < count; index++)
        {
            outPath->data[index] = tiles[index];
        }

        outPath->count = count;
        Log("Applied %s path buffer: count=%d", reason, count);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception while overwriting %s path buffer", reason);
    }
}

// Replaces an execution path and normalizes it to the actual execution start tile...
static void OverwriteExecutionPathBufferFromTiles(ManualPathBuffer* outPath, const uint64_t* tiles, int32_t count, uint64_t actualStartTile)
{
    int32_t sourceIndex;
    int32_t writeIndex;
    uint64_t previousTile;

    if (!outPath || !tiles || count < 2)
    {
        return;
    }

    sourceIndex = 0;

    if (tiles[0] == actualStartTile)
    {
        sourceIndex = 1;
    }

    if ((count - sourceIndex) < 1)
    {
        return;
    }

    __try
    {
        if (!EnsurePathCapacity(outPath, count - sourceIndex))
        {
            Log("Failed to ensure path capacity for execution-no-start");
            return;
        }

        writeIndex = 0;
        previousTile = actualStartTile;

        while (sourceIndex < count)
        {
            if (tiles[sourceIndex] != previousTile)
            {
                outPath->data[writeIndex] = tiles[sourceIndex];
                previousTile = tiles[sourceIndex];
                writeIndex++;
            }

            sourceIndex++;
        }

        outPath->count = writeIndex;
        Log("Applied execution path buffer without source tile: count=%d", writeIndex);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception while overwriting execution-no-start path buffer");
    }
}

// Hides the vanilla preview while an armed manual drag is back on its origin tile...
static void ClearPathBufferNoLock(ManualPathBuffer* path, const char* reason)
{
    if (!path)
    {
        return;
    }

    __try
    {
        path->count = 0;
        Log("Cleared path buffer: reason=%s", reason ? reason : "manual cancellation");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Exception while clearing path buffer: reason=%s", reason ? reason : "manual cancellation");
    }
}

// Keeps the manual endpoint synchronized with the native preview target while using projections only as transit hints...
// The occupied origin is the sole exception because native targeting sanitizes it to an adjacent legal move tile.
static void AddDragSampleTiles(void* ability, uint64_t previewTargetTile)
{
    uint64_t rawHoverTile;
    uint64_t inferredHoverTile;
    uint64_t projectedCornerTile;
    uint64_t lastTile;
    int hasRawHoverTile;
    int hasInferredHoverTile;

    // Resolve the occupied origin before recording the native target. Vanilla can sanitize an
    // origin hover to an adjacent legal destination, which could poison calibration...
    if (ReturnDragToOriginIfCurrentlyHoveredNoLock(ability, previewTargetTile, "preview-sample"))
    {
        return;
    }

    RememberTileScreenSampleNoLock(previewTargetTile);

    rawHoverTile = 0;
    hasRawHoverTile = TryGetAbilityRawHoverTile(ability, previewTargetTile, &rawHoverTile);
    inferredHoverTile = 0;
    hasInferredHoverTile = TryInferCursorTileNoLock(&inferredHoverTile);

    DrainFrameDragCandidatesNoLock(ability, previewTargetTile);

    if (hasRawHoverTile && rawHoverTile != 0 && rawHoverTile != previewTargetTile)
    {
        g_rawHoverDifferentCount++;
        Log("Raw hover transit differs from preview: raw=(%d,%d) preview=(%d,%d) count=%d", TileX(rawHoverTile), TileY(rawHoverTile), TileX(previewTargetTile), TileY(previewTargetTile), g_rawHoverDifferentCount);
        AddSupplementalInferredDragTile(ability, rawHoverTile, previewTargetTile, "raw-hover");
    }

    if (hasInferredHoverTile && inferredHoverTile != 0 && inferredHoverTile != previewTargetTile)
    {
        g_inferredHoverDifferentCount++;
        Log("Inferred cursor transit differs from preview: inferred=(%d,%d) preview=(%d,%d) count=%d", TileX(inferredHoverTile), TileY(inferredHoverTile), TileX(previewTargetTile), TileY(previewTargetTile), g_inferredHoverDifferentCount);
        AddSupplementalInferredDragTile(ability, inferredHoverTile, previewTargetTile, "cursor-projected");
    }

    if (g_dragCount > 0)
    {
        lastTile = g_dragTiles[g_dragCount - 1];
        projectedCornerTile = PickProjectedDiagonalCornerNoLock(lastTile, previewTargetTile);

        if (projectedCornerTile != 0 && projectedCornerTile != lastTile && projectedCornerTile != previewTargetTile)
        {
            Log("Projected diagonal corner transit: last=(%d,%d) corner=(%d,%d) preview=(%d,%d)", TileX(lastTile), TileY(lastTile), TileX(projectedCornerTile), TileY(projectedCornerTile), TileX(previewTargetTile), TileY(previewTargetTile));
            AddSupplementalInferredDragTile(ability, projectedCornerTile, previewTargetTile, "diagonal-corner");
        }
    }

    // (The target passed to the native preview builder is the selected tile used at release)
    // Always settle the manual endpoint on it so the drawn path and release validation agree...
    AddDragTile(ability, previewTargetTile);
}

// Timer-side sampling path that queues cursor/raw candidates between preview hook calls...
static void AddFrameCursorDragSampleNoLock(void* ability, uint64_t previewTargetTile)
{
    uint64_t inferredHoverTile;
    uint64_t projectedCornerTile;
    uint64_t lastTile;
    int hasInferredHoverTile;

    if (!ability || previewTargetTile == 0 || g_dragCount <= 0)
    {
        return;
    }

    if (ReturnDragToOriginIfCurrentlyHoveredNoLock(ability, previewTargetTile, "frame-sampler"))
    {
        return;
    }

    inferredHoverTile = 0;
    hasInferredHoverTile = TryInferCursorTileNoLock(&inferredHoverTile);

    if (hasInferredHoverTile && inferredHoverTile != 0)
    {
        QueueFrameDragCandidateNoLock(inferredHoverTile, previewTargetTile, "frame-cursor-projected");
    }

    if (g_dragCount > 0)
    {
        lastTile = g_dragTiles[g_dragCount - 1];
        projectedCornerTile = PickProjectedDiagonalCornerNoLock(lastTile, previewTargetTile);

        if (projectedCornerTile != 0 && projectedCornerTile != lastTile && projectedCornerTile != previewTargetTile)
        {
            QueueFrameDragCandidateNoLock(projectedCornerTile, previewTargetTile, "frame-diagonal-corner");
        }
    }
}

// Preview-hook sampling entry point that records tiles from path-builder arguments...
static void SampleDragFromPathArgs(void* ability, uint64_t startTile, uint64_t targetTile)
{
    ULONGLONG nowTick;
    uint8_t isLeftDown;
    int32_t characterSize;
    int allowManualDrag;

    if (InterlockedCompareExchange(&g_pathLockReady, 0, 0) == 0)
    {
        return;
    }

    nowTick = GetTickCount64();

    if ((nowTick - g_lastSampleTick) < DRAG_SAMPLE_INTERVAL_MS)
    {
        return;
    }

    g_lastSampleTick = nowTick;
    isLeftDown = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ? 1 : 0;
    characterSize = 0;
    allowManualDrag = IsAbilityCharacterSingleTile(ability, &characterSize);

    EnterCriticalSection(&g_pathLock);

    if (!allowManualDrag)
    {
        // Do not let unrelated background previews disturb an active 1x1 drag...
        if (g_dragAbility && g_dragAbility != ability && g_wasLeftDown)
        {
            LeaveCriticalSection(&g_pathLock);
            return;
        }

        if (g_lastIgnoredManualDragAbility != ability)
        {
            Log("Manual drag ignored for non-1x1 character: sizeCode=%d ability=%p", characterSize, ability);
            g_lastIgnoredManualDragAbility = ability;
        }

        if (isLeftDown && !g_wasLeftDown)
        {
            ClearPreparedPathNoLock("non-1x1 mouse down");
            ClearMoveCancellationNoLock();
        }

        ClearDragPath();
        ClearFrameDragCandidatesNoLock();
        g_wasLeftDown = isLeftDown;
        LeaveCriticalSection(&g_pathLock);
        return;
    }

    g_lastIgnoredManualDragAbility = NULL;

    if (isLeftDown)
    {
        TouchDragPreviewNoLock(targetTile);
    }

    if (isLeftDown && !g_wasLeftDown)
    {
        ClearDragPath();
        ClearFrameDragCandidatesNoLock();
        ClearPreparedPathNoLock("new mouse down before manual intent");
        ClearMoveCancellationNoLock();
        BeginManualDragCandidateNoLock();
        TouchDragPreviewNoLock(targetTile);
        g_dragAbility = ability;
        g_dragStartTile = startTile;
        AddDragTile(ability, startTile);
        AddDragSampleTiles(ability, targetTile);
        UpdateManualDragArmNoLock("drag-begin");
        Log("Drag begin: start=(%d,%d) target=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile));
    }
    else if (isLeftDown && g_wasLeftDown)
    {
        if (g_dragCount == 0)
        {
            ClearDragPath();
            BeginManualDragCandidateNoLock();
            TouchDragPreviewNoLock(targetTile);
            g_dragAbility = ability;
            g_dragStartTile = startTile;
            AddDragTile(ability, startTile);
        }
        else if (g_dragAbility != ability)
        {
            Log("Drag sample from different ability ignored instead of resetting active manual path");
            LeaveCriticalSection(&g_pathLock);
            return;
        }
        else if (g_dragStartTile != startTile)
        {
            Log("Drag sample start changed while dragging; keeping original manual path start=(%d,%d) sampleStart=(%d,%d)", TileX(g_dragStartTile), TileY(g_dragStartTile), TileX(startTile), TileY(startTile));
        }

        AddDragSampleTiles(ability, targetTile);
        UpdateManualDragArmNoLock("drag-sample");
    }
    else if (!isLeftDown && g_wasLeftDown)
    {
        TouchDragPreviewNoLock(targetTile);
        AddDragSampleTiles(ability, targetTile);

        if (UpdateManualDragArmNoLock("drag-release") && (g_dragCount >= 2 || (g_dragStartTile != 0 && g_dragEndTile == g_dragStartTile)))
        {
            g_pendingPreviewRelease = 1;
        }
        else
        {
            Log("Mouse release treated as vanilla click: manualDragArmed=%d count=%d", g_manualDragArmed, g_dragCount);
            ClearPreparedPathNoLock("mouse release without manual drag intent");
            ClearDragPath();
            ClearFrameDragCandidatesNoLock();
        }
    }

    g_wasLeftDown = isLeftDown;
    LeaveCriticalSection(&g_pathLock);
}

// Timer callback helper for drag sampling when no preview hook call occurs this frame...
static void PollDragFrameSamplerNoHook(void)
{
    uint8_t isLeftDown;
    ULONGLONG nowTick;

    if (InterlockedCompareExchange(&g_pathLockReady, 0, 0) == 0)
    {
        return;
    }

    if (InterlockedCompareExchange(&g_frameDragSamplerBusy, 1, 0) != 0)
    {
        return;
    }

    nowTick = GetTickCount64();

    if ((nowTick - g_lastFrameDragSampleTick) < FRAME_DRAG_SAMPLE_INTERVAL_MS)
    {
        InterlockedExchange(&g_frameDragSamplerBusy, 0);
        return;
    }

    g_lastFrameDragSampleTick = nowTick;
    isLeftDown = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ? 1 : 0;

    EnterCriticalSection(&g_pathLock);

    // A cancellation belongs only to the release that created it; never carry it into a new click...
    if (isLeftDown && !g_wasLeftDown && g_cancelMoveAbility)
    {
        ClearMoveCancellationNoLock();
    }

    if (isLeftDown && g_wasLeftDown && g_dragAbility && g_dragCount > 0 && g_lastDragPreviewTargetTile != 0)
    {
        TouchDragPreviewNoLock(g_lastDragPreviewTargetTile);

        if (UpdateManualDragArmNoLock("frame-sampler"))
        {
            AddFrameCursorDragSampleNoLock(g_dragAbility, g_lastDragPreviewTargetTile);
        }
    }

    LeaveCriticalSection(&g_pathLock);
    InterlockedExchange(&g_frameDragSamplerBusy, 0);
}

// Timer callback helper that detects release and prepares/triggers the manual path...
static void PollMouseReleaseNoHook(void)
{
    uint8_t isLeftDown;

    if (InterlockedCompareExchange(&g_pathLockReady, 0, 0) == 0)
    {
        return;
    }

    isLeftDown = ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) ? 1 : 0;

    EnterCriticalSection(&g_pathLock);

    if (!isLeftDown && g_wasLeftDown)
    {
        if (g_manualDragArmed)
        {
            PrepareDragPathNoLock();
        }
        else
        {
            Log("Mouse release without manual drag intent; vanilla click remains in control count=%d", g_dragCount);
            ClearPreparedPathNoLock("mouse release without manual drag intent");
        }

        ClearDragPath();
        ClearFrameDragCandidatesNoLock();
    }

    g_wasLeftDown = isLeftDown;
    LeaveCriticalSection(&g_pathLock);
}

// OnTrigger detour used to write the manual target immediately before native move triggering...
static void __fastcall HookMoveAbilityOnTrigger(void* ability)
{
    uint64_t startTile;
    uint64_t targetTile;
    int32_t characterSize;
    int hasStartAndTarget;
    int allowManualPath;
    int cancelTrigger;
    int originReleasePending;

    startTile = 0;
    targetTile = 0;
    characterSize = 0;
    hasStartAndTarget = TryGetAbilityStartAndTarget(ability, &startTile, &targetTile);
    allowManualPath = IsAbilityCharacterSingleTile(ability, &characterSize);
    cancelTrigger = 0;
    originReleasePending = 0;

    EnterCriticalSection(&g_pathLock);

    if (g_manualDragArmed && g_dragAbility == ability && g_dragStartTile != 0)
    {
        ReturnDragToOriginIfCurrentlyHoveredNoLock(ability, hasStartAndTarget ? targetTile : g_lastDragPreviewTargetTile, "on-trigger");
        originReleasePending = (g_dragEndTile == g_dragStartTile) ? 1 : 0;
    }

    if (ConsumeMoveCancellationNoLock(ability))
    {
        cancelTrigger = 1;
    }
    else if (originReleasePending)
    {
        ArmMoveCancellationNoLock(ability, "origin release reached OnTrigger before preview cleanup");
        cancelTrigger = ConsumeMoveCancellationNoLock(ability);
        ClearPreparedPathNoLock("origin release trigger cancelled");
    }
    else if (hasStartAndTarget && allowManualPath)
    {
        if (g_manualDragArmed && g_dragCount >= 2 && g_dragAbility == ability)
        {
            PrepareDragPathNoLock();
        }

        if (HasPreparedPathNoLock(startTile, targetTile, MAX_MANUAL_STEPS))
        {
            InterlockedExchange(&g_applyNextPath, 1);
            Log("Move trigger matched prepared path: start=(%d,%d) end=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile));
        }
        else
        {
            Log("Move trigger did not match prepared path: start=(%d,%d) target=(%d,%d) preparedCount=%d", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile), g_preparedCount);
        }
    }
    else if (hasStartAndTarget)
    {
        Log("Move trigger left vanilla for non-1x1 character: sizeCode=%d ability=%p", characterSize, ability);
    }

    LeaveCriticalSection(&g_pathLock);

    if (cancelTrigger)
    {
        Log("MoveAbility::OnTrigger suppressed because manual drag ended on its origin tile");
        return;
    }

    if (g_origMoveAbilityOnTrigger)
    {
        g_origMoveAbilityOnTrigger(ability);
    }
}

// Preview path-builder detour (Observes drag samples and optionally overwrites preview visuals)...
static ManualPathBuffer* __fastcall HookMoveAbilityBuildPreviewPath(void* ability, ManualPathBuffer* outPath, uint64_t startTile, uint64_t targetTile)
{
    ManualPathBuffer* result;
    ManualPathBuffer* path;
    int32_t characterSize;
    int allowManualPath;

    SampleDragFromPathArgs(ability, startTile, targetTile);

    result = NULL;

    if (g_origMoveAbilityBuildPreviewPath)
    {
        result = g_origMoveAbilityBuildPreviewPath(ability, outPath, startTile, targetTile);
    }

    path = result ? result : outPath;
    characterSize = 0;
    allowManualPath = IsAbilityCharacterSingleTile(ability, &characterSize);

    EnterCriticalSection(&g_pathLock);
    RememberPreviewReachabilityNoLock(ability, startTile, targetTile, path);

    if (allowManualPath)
    {
        PruneUnreachablePreviewTargetNoLock(ability, targetTile);
    }

    if (g_pendingPreviewRelease && g_dragAbility == ability)
    {
        g_pendingPreviewRelease = 0;

        if (g_manualDragArmed && allowManualPath)
        {
            PrepareDragPathNoLock();
        }
        else if (!allowManualPath)
        {
            Log("Pending manual release discarded for non-1x1 character: sizeCode=%d", characterSize);
            ClearPreparedPathNoLock("non-1x1 pending release");
        }

        ClearDragPath();
        ClearFrameDragCandidatesNoLock();
    }

    if (allowManualPath && g_manualDragArmed && g_dragAbility == ability && g_dragStartTile != 0 && g_dragEndTile == g_dragStartTile)
    {
        ClearPathBufferNoLock(path, "manual drag returned to origin");
    }
    else if (allowManualPath && g_manualDragArmed && g_dragCount >= 2 && g_dragAbility == ability && g_lastPreviewWasReachable)
    {
        OverwritePathBufferFromTiles(path, g_dragTiles, g_dragCount, "live preview pinned to manual drag");
    }
    else if (allowManualPath && g_manualDragArmed && g_dragCount >= 2 && g_dragAbility == ability && !g_lastPreviewWasReachable)
    {
        Log("Live manual preview suppressed because current preview target is unreachable: target=(%d,%d) count=%d", TileX(targetTile), TileY(targetTile), g_dragCount);
    }
    else if (allowManualPath && HasPreparedPathNoLock(startTile, targetTile, MAX_MANUAL_STEPS))
    {
        OverwritePathBufferFromTiles(path, g_preparedTiles, g_preparedCount, "prepared preview");
    }

    LeaveCriticalSection(&g_pathLock);

    return path;
}

// Execution path-builder detour (Forces the prepared manual route during the release window)...
static ManualPathBuffer* __fastcall HookTacticsGridBuildMovePath(void* grid, ManualPathBuffer* outPath, void* character, uint64_t startTile, uint64_t targetTile, int32_t maxMove, uint8_t flag)
{
    ManualPathBuffer* result;
    int shouldApply;
    int32_t characterSize;
    LONG remainingBudget;

    result = NULL;

    if (g_origTacticsGridBuildMovePath)
    {
        result = g_origTacticsGridBuildMovePath(grid, outPath, character, startTile, targetTile, maxMove, flag);
    }

    characterSize = 0;

    if (!IsCharacterSingleTile(character, &characterSize))
    {
        if (InterlockedCompareExchange(&g_applyNextPath, 0, 0) != 0)
        {
            Log("Execution manual path ignored for non-1x1 character: sizeCode=%d character=%p", characterSize, character);
        }

        return result ? result : outPath;
    }

    shouldApply = 0;

    if (InterlockedCompareExchange(&g_applyNextPath, 0, 0) != 0)
    {
        EnterCriticalSection(&g_pathLock);
        shouldApply = HasPreparedPathNoLock(startTile, targetTile, maxMove);

        if (!shouldApply && ShouldForcePreparedExecutionNoLock(maxMove))
        {
            shouldApply = 1;
            Log("Forced execution path buffer ignoring reported maxMove/pathfinder args: argsStart=(%d,%d) argsTarget=(%d,%d) preparedStart=(%d,%d) preparedEnd=(%d,%d)", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile), TileX(g_preparedStartTile), TileY(g_preparedStartTile), TileX(g_preparedEndTile), TileY(g_preparedEndTile));
        }

        if (shouldApply)
        {
            OverwriteExecutionPathBufferFromTiles(result ? result : outPath, g_preparedTiles, g_preparedCount, g_preparedStartTile);
            g_executionApplyCount++;
            remainingBudget = InterlockedDecrement(&g_applyOverrideBudget);

            if (remainingBudget > 0 && GetTickCount64() <= g_applyOverrideUntilTick)
            {
                InterlockedExchange(&g_applyNextPath, 1);
                Log("Manual path override remains armed: remainingBudget=%ld", remainingBudget);
            }
            else
            {
                DisarmPreparedPathOverrideNoLock("execution budget/window completed");
            }
        }
        else
        {
            Log("Execution pathfinder did not match prepared path: start=(%d,%d) target=(%d,%d) reportedMaxMove=%d preparedCount=%d", TileX(startTile), TileY(startTile), TileX(targetTile), TileY(targetTile), maxMove, g_preparedCount);
        }

        LeaveCriticalSection(&g_pathLock);
    }

    return result ? result : outPath;
}

#pragma pack(push, 1)
typedef struct GameInlineString
{
    char inlineBytes[16];
    uint64_t length;
    uint64_t capacity;
} GameInlineString;
#pragma pack(pop)

// Rejects obvious invalid pointers before reading gon-style strings...
static int IsReadablePointerCandidate(void* pointer)
{
    MEMORY_BASIC_INFORMATION info;
    SIZE_T result;

    if (!pointer)
    {
        return 0;
    }

    result = VirtualQuery(pointer, &info, sizeof(info));

    if (result == 0)
    {
        return 0;
    }

    if (info.State != MEM_COMMIT)
    {
        return 0;
    }

    if ((info.Protect & PAGE_NOACCESS) != 0 || (info.Protect & PAGE_GUARD) != 0)
    {
        return 0;
    }

    return 1;
}

static const float g_manualPathArrowColor[4] =
{
    MANUAL_PATH_ARROW_COLOR_R,
    MANUAL_PATH_ARROW_COLOR_G,
    MANUAL_PATH_ARROW_COLOR_B,
    MANUAL_PATH_ARROW_COLOR_A
};

// Safely extracts bytes/length from a game inline/heap string candidate...
static const char* GetGameStringBytes(void* gameString, uint64_t* lengthOut)
{
    GameInlineString* text;
    uint64_t length;
    const char* bytes;

    if (lengthOut)
    {
        *lengthOut = 0ULL;
    }

    if (!gameString || !IsReadablePointerCandidate(gameString))
    {
        return NULL;
    }

    text = (GameInlineString*)gameString;
    bytes = NULL;
    length = 0ULL;

    __try
    {
        length = text->length;

        if (length > 256ULL)
        {
            return NULL;
        }

        if (text->capacity > 15ULL)
        {
            bytes = *(const char**)text->inlineBytes;
        }
        else
        {
            bytes = text->inlineBytes;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return NULL;
    }

    if (!bytes || !IsReadablePointerCandidate((void*)bytes))
    {
        return NULL;
    }

    if (lengthOut)
    {
        *lengthOut = length;
    }

    return bytes;
}

// Prefix check for identifying PathIndicator asset/object names...
static int GameStringStartsWith(void* gameString, const char* prefix)
{
    const char* bytes;
    uint64_t length;
    size_t prefixLength;
    int result;

    if (!prefix)
    {
        return 0;
    }

    bytes = GetGameStringBytes(gameString, &length);

    if (!bytes)
    {
        return 0;
    }

    prefixLength = strlen(prefix);

    if (length < (uint64_t)prefixLength)
    {
        return 0;
    }

    result = 0;

    __try
    {
        result = (memcmp(bytes, prefix, prefixLength) == 0) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result = 0;
    }

    return result;
}

// Decides whether a visual-create call appears to be a move path indicator arrow...
static int ShouldTintPathVisualCreate(void* assetName, void* objectName)
{
    LONG applyNext;
    LONG budget;
    int hasLiveManualDragIntent;
    int hasPreparedManualExecution;

    if (InterlockedCompareExchange(&g_manualArrowColor, 0, 0) == 0)
    {
        return 0;
    }

    applyNext = InterlockedCompareExchange(&g_applyNextPath, 0, 0);
    budget = InterlockedCompareExchange(&g_applyOverrideBudget, 0, 0);

    hasLiveManualDragIntent = (g_manualDragArmed && g_dragCount >= 2) ? 1 : 0;
    hasPreparedManualExecution = (applyNext != 0 && budget > 0) ? 1 : 0;

    if (!hasLiveManualDragIntent && !hasPreparedManualExecution)
    {
        return 0;
    }

    if (!GameStringStartsWith(objectName, "PathIndicator"))
    {
        return 0;
    }

    if (GameStringStartsWith(objectName, "KnockbackArrow"))
    {
        return 0;
    }

    if (GameStringStartsWith(assetName, "path") || GameStringStartsWith(assetName, "begin") || GameStringStartsWith(assetName, "end"))
    {
        return 1;
    }

    return 0;
}

// Optional visual-create detour that tints manual path arrows without altering path logic...
static void* __fastcall HookPathVisualCreate(void* parent, int32_t visualKind, void* assetName, void* objectName, void* position, void* color, int32_t layer, void* transform, uint8_t visibleFlag)
{
    void* colorToUse;
    void* result;

    colorToUse = color;

    if (InterlockedCompareExchange(&g_pathVisualCreateSeen, 1, 0) == 0)
    {
        Log("Path visual creation hook is live: parent=%p kind=%d asset=%p object=%p", parent, visualKind, assetName, objectName);
    }

    if (ShouldTintPathVisualCreate(assetName, objectName))
    {
        colorToUse = (void*)g_manualPathArrowColor;

        if (InterlockedCompareExchange(&g_pathIndicatorColorWarningLogged, 1, 0) == 0)
        {
            Log("Applying manual PathIndicator arrow color through visual creation hook kind=%d", visualKind);
        }
    }

    if (!g_origPathVisualCreate)
    {
        return NULL;
    }

    result = g_origPathVisualCreate(parent, visualKind, assetName, objectName, position, colorToUse, layer, transform, visibleFlag);
    return result;
}

// Build guard that confirms expected bytes are present before installing a hook...
static int VerifyTargetBytes(UINT_PTR gameBase, UINT_PTR rva, const uint8_t* expectedBytes, SIZE_T expectedCount, const char* name)
{
    const uint8_t* actualBytes;
    SIZE_T index;

    if (!gameBase || !expectedBytes || !name || expectedCount == 0)
    {
        return 0;
    }

    actualBytes = (const uint8_t*)(gameBase + rva);

    __try
    {
        for (index = 0; index < expectedCount; index++)
        {
            if (actualBytes[index] != expectedBytes[index])
            {
                Log("Skipped %s hook: byte mismatch at +0x%IX expected=0x%02X actual=0x%02X", name, index, expectedBytes[index], actualBytes[index]);
                return 0;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("Skipped %s hook: exception while verifying target bytes", name);
        return 0;
    }

    return 1;
}

// Installs MoveAbility::OnTrigger detour...
static int InstallMoveAbilityOnTriggerHook(void)
{
    UINT_PTR gameBase;
    void* trampoline;
    const uint8_t moveAbilityOnTriggerBytes[STOLEN_MOVE_ABILITY_ON_TRIGGER] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };

    if (InterlockedCompareExchange(&g_hookedOnTrigger, 0, 0) != 0)
    {
        Log("MoveAbility::OnTrigger hook already installed");
        return 1;
    }

    gameBase = GetGameBase();

    if (gameBase)
    {
        g_abilityTrigger = (fn_ability_trigger)(gameBase + RVA_ABILITY_TRIGGER);
    }

    if (!gameBase)
    {
        Log("OnTrigger hook skipped: game base unavailable");
        return 0;
    }

    trampoline = NULL;

    if (!VerifyTargetBytes(gameBase, RVA_MOVE_ABILITY_ON_TRIGGER, moveAbilityOnTriggerBytes, sizeof(moveAbilityOnTriggerBytes), "MoveAbility::OnTrigger"))
    {
        return 0;
    }

    if (!g_mj.InstallHook(RVA_MOVE_ABILITY_ON_TRIGGER, STOLEN_MOVE_ABILITY_ON_TRIGGER, (void*)HookMoveAbilityOnTrigger, &trampoline, 20, MOD_NAME))
    {
        Log("MoveAbility::OnTrigger InstallHook failed at RVA 0x%X", RVA_MOVE_ABILITY_ON_TRIGGER);
        return 0;
    }

    g_origMoveAbilityOnTrigger = (fn_move_ability_on_trigger)trampoline;
    InterlockedExchange(&g_hookedOnTrigger, 1);
    Log("Hooked MoveAbility::OnTrigger at RVA 0x%X trampoline=%p", RVA_MOVE_ABILITY_ON_TRIGGER, trampoline);
    return 1;
}

// Installs preview path-builder detour...
static int InstallMoveAbilityPreviewHook(void)
{
    UINT_PTR gameBase;
    void* trampoline;
    const uint8_t moveAbilityPreviewBytes[STOLEN_MOVE_ABILITY_BUILD_PREVIEW_PATH] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x4C, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10 };

    if (InterlockedCompareExchange(&g_hookedPreview, 0, 0) != 0)
    {
        Log("Preview hook already installed");
        return 1;
    }

    gameBase = GetGameBase();

    if (!gameBase)
    {
        Log("Preview hook skipped: game base unavailable");
        return 0;
    }

    g_vectorReallocBytes = (fn_vector_realloc_bytes)(gameBase + RVA_VECTOR_REALLOC_BYTES);
    trampoline = NULL;

    if (!VerifyTargetBytes(gameBase, RVA_MOVE_ABILITY_BUILD_PREVIEW_PATH, moveAbilityPreviewBytes, sizeof(moveAbilityPreviewBytes), "MoveAbility preview path builder"))
    {
        return 0;
    }

    if (!g_mj.InstallHook(RVA_MOVE_ABILITY_BUILD_PREVIEW_PATH, STOLEN_MOVE_ABILITY_BUILD_PREVIEW_PATH, (void*)HookMoveAbilityBuildPreviewPath, &trampoline, 20, MOD_NAME))
    {
        Log("Preview InstallHook failed at RVA 0x%X", RVA_MOVE_ABILITY_BUILD_PREVIEW_PATH);
        return 0;
    }

    g_origMoveAbilityBuildPreviewPath = (fn_move_ability_build_preview_path)trampoline;
    InterlockedExchange(&g_hookedPreview, 1);
    Log("Hooked preview path builder at RVA 0x%X trampoline=%p", RVA_MOVE_ABILITY_BUILD_PREVIEW_PATH, trampoline);
    return 1;
}

// Installs the execution path-builder detour...
static int InstallTacticsGridExecutionHook(void)
{
    UINT_PTR gameBase;
    void* trampoline;
    const uint8_t tacticsGridMoveBytes[STOLEN_TACTICS_GRID_BUILD_MOVE_PATH] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0xB6, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, 0x48, 0x8B, 0xDA };

    if (InterlockedCompareExchange(&g_hookedExecution, 0, 0) != 0)
    {
        Log("Execution hook already installed");
        return 1;
    }

    gameBase = GetGameBase();

    if (!gameBase)
    {
        Log("Execution hook skipped: game base unavailable");
        return 0;
    }

    g_vectorReallocBytes = (fn_vector_realloc_bytes)(gameBase + RVA_VECTOR_REALLOC_BYTES);
    trampoline = NULL;

    if (!VerifyTargetBytes(gameBase, RVA_TACTICS_GRID_BUILD_MOVE_PATH, tacticsGridMoveBytes, sizeof(tacticsGridMoveBytes), "TacticsGrid execution pathfinder"))
    {
        return 0;
    }

    if (!g_mj.InstallHook(RVA_TACTICS_GRID_BUILD_MOVE_PATH, STOLEN_TACTICS_GRID_BUILD_MOVE_PATH, (void*)HookTacticsGridBuildMovePath, &trampoline, 10, MOD_NAME))
    {
        Log("Execution InstallHook failed at RVA 0x%X", RVA_TACTICS_GRID_BUILD_MOVE_PATH);
        return 0;
    }

    g_origTacticsGridBuildMovePath = (fn_tactics_grid_build_move_path)trampoline;
    InterlockedExchange(&g_hookedExecution, 1);
    Log("Hooked execution pathfinder at RVA 0x%X trampoline=%p", RVA_TACTICS_GRID_BUILD_MOVE_PATH, trampoline);
    return 1;
}

// Installs the PathIndicator visual-create detour...
static int InstallPathVisualCreateHook(void)
{
    UINT_PTR gameBase;
    void* trampoline;
    const uint8_t pathVisualCreateBytes[STOLEN_PATH_VISUAL_CREATE] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08 };

    if (InterlockedCompareExchange(&g_hookedPathVisualCreate, 0, 0) != 0)
    {
        Log("Path visual creation hook already installed");
        return 1;
    }

    gameBase = GetGameBase();

    if (!gameBase)
    {
        Log("Path visual creation hook skipped: game base unavailable");
        return 0;
    }

    if (!VerifyTargetBytes(gameBase, RVA_PATH_VISUAL_CREATE, pathVisualCreateBytes, sizeof(pathVisualCreateBytes), "Path visual create"))
    {
        return 0;
    }

    trampoline = NULL;

    if (!g_mj.InstallHook(RVA_PATH_VISUAL_CREATE, STOLEN_PATH_VISUAL_CREATE, (void*)HookPathVisualCreate, &trampoline, 30, MOD_NAME))
    {
        Log("Path visual creation InstallHook failed at RVA 0x%X", RVA_PATH_VISUAL_CREATE);
        return 0;
    }

    g_origPathVisualCreate = (fn_path_visual_create)trampoline;
    InterlockedExchange(&g_hookedPathVisualCreate, 1);
    Log("Hooked PathIndicator visual creation at RVA 0x%X trampoline=%p", RVA_PATH_VISUAL_CREATE, trampoline);
    return 1;
}

// Reports whether all enabled hooks are installed...
static int AreAllRequestedHooksInstalled(void)
{
    if (InterlockedCompareExchange(&g_hookedPreview, 0, 0) == 0)
    {
        return 0;
    }

    if (InterlockedCompareExchange(&g_hookedOnTrigger, 0, 0) == 0)
    {
        return 0;
    }

    if (InterlockedCompareExchange(&g_hookedExecution, 0, 0) == 0)
    {
        return 0;
    }

    if (InterlockedCompareExchange(&g_manualArrowColor, 0, 0) != 0 && InterlockedCompareExchange(&g_hookedPathVisualCreate, 0, 0) == 0)
    {
        return 0;
    }

    return 1;
}

// Hook installer, retried by the runtime timer until game base is ready...
static void InstallAllHooks(void)
{
    if (AreAllRequestedHooksInstalled())
    {
        return;
    }

    if (InterlockedCompareExchange(&g_hookInstallBusy, 1, 0) != 0)
    {
        return;
    }

    InstallMoveAbilityPreviewHook();
    InstallMoveAbilityOnTriggerHook();
    InstallTacticsGridExecutionHook();

    if (InterlockedCompareExchange(&g_manualArrowColor, 0, 0) != 0)
    {
        InstallPathVisualCreateHook();
    }

    InterlockedExchange(&g_hookInstallBusy, 0);
}

// One-shot startup status message for confirming hook state in logs...
static void LogStatus(void)
{
    UINT_PTR gameBase;

    gameBase = GetGameBase();
    Log("Status: gameBase=%p onTrigger=%ld preview=%ld execution=%ld visualCreate=%ld autoMove=%ld maxMoveFallback=%ld arrowColor=%ld apply=%ld budget=%ld preparedCount=%d dragCount=%d", (void*)gameBase, InterlockedCompareExchange(&g_hookedOnTrigger, 0, 0), InterlockedCompareExchange(&g_hookedPreview, 0, 0), InterlockedCompareExchange(&g_hookedExecution, 0, 0), InterlockedCompareExchange(&g_hookedPathVisualCreate, 0, 0), InterlockedCompareExchange(&g_autoMoveOnRelease, 0, 0), InterlockedCompareExchange(&g_reportedMaxMoveFallback, 0, 0), InterlockedCompareExchange(&g_manualArrowColor, 0, 0), InterlockedCompareExchange(&g_applyNextPath, 0, 0), InterlockedCompareExchange(&g_applyOverrideBudget, 0, 0), g_preparedCount, g_dragCount);
    Log("Hooks are installed automatically at startup; no runtime keyboard controls are registered or polled.");
    Log("Auto move on release: %ld", InterlockedCompareExchange(&g_autoMoveOnRelease, 0, 0));
}

// Runtime timer that retries hook install, samples drag frames, and catches mouse releases...
static VOID CALLBACK TimerProc(PVOID parameter, BOOLEAN timerOrWaitFired)
{
    (void)parameter;
    (void)timerOrWaitFired;

    if (InterlockedCompareExchange(&g_shutdown, 0, 0) != 0)
    {
        return;
    }

    if (!g_mj.GetGameBase)
    {
        MJ_Resolve(&g_mj);
    }

    g_timerTickCount++;

    if (g_timerTickCount == 1)
    {
        Log("Runtime sampler active; hooks install automatically and keyboard controls are disabled");
        LogStatus();
    }

    if (!AreAllRequestedHooksInstalled() && ((g_timerTickCount % 60U) == 1U))
    {
        InstallAllHooks();
    }

    PollDragFrameSamplerNoHook();
    PollMouseReleaseNoHook();
}

// Creates the timer queue used for auto-install retry and input polling...
static void StartRuntimeTimer(void)
{
    HANDLE timerHandle;

    if (InterlockedCompareExchange(&g_timerStarted, 1, 0) != 0)
    {
        return;
    }

    g_timerQueue = CreateTimerQueue();

    if (!g_timerQueue)
    {
        InterlockedExchange(&g_timerStarted, 0);
        Log("CreateTimerQueue failed");
        return;
    }

    timerHandle = NULL;

    if (!CreateTimerQueueTimer(&timerHandle, g_timerQueue, TimerProc, NULL, 250U, 16U, WT_EXECUTEDEFAULT))
    {
        DeleteTimerQueue(g_timerQueue);
        g_timerQueue = NULL;
        InterlockedExchange(&g_timerStarted, 0);
        Log("CreateTimerQueueTimer failed");
        return;
    }

    Log("Runtime timer started");
}

// Stops timer processing during detach and waits for callbacks to drain...
static void StopRuntimeTimer(void)
{
    if (g_timerQueue)
    {
        DeleteTimerQueueEx(g_timerQueue, INVALID_HANDLE_VALUE);
        g_timerQueue = NULL;
    }

    InterlockedExchange(&g_timerStarted, 0);
}

// DLL attach entry point: Initializes state, attempts immediate hook install, and starts retry/poll timer...
static void Initialize(void)
{
    if (!MJ_Resolve(&g_mj))
    {
        return;
    }

    InitializeCriticalSection(&g_pathLock);
    InterlockedExchange(&g_pathLockReady, 1);
    InterlockedExchange(&g_shutdown, 0);
    Log("Loaded automatic-hook build; installing hooks at startup");
    InstallAllHooks();
    StartRuntimeTimer();
}

// Windows DLL entry point used by Mewjector to start and stop the mod safely...
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)hModule;
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        Initialize();
        Log("Loaded");
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        InterlockedExchange(&g_shutdown, 1);

        StopRuntimeTimer();

        if (InterlockedCompareExchange(&g_pathLockReady, 0, 0) != 0)
        {
            InterlockedExchange(&g_pathLockReady, 0);
            DeleteCriticalSection(&g_pathLock);
        }

        if (MJ_Resolve(&g_mj) && g_mj.Log)
        {
            g_mj.Log(MOD_NAME, "Unloaded");
        }
    }

    return TRUE;
}
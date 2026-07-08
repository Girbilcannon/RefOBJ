///----------------------------------------------------------------------------------------------------
/// RefOBJ Nexus Addon
/// OBJ reference overlay using Nexus + MumbleLink + ImGui draw lists.
///----------------------------------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>


#include "nexus/Nexus.h"
#include "mumble/Mumble.h"
#include "imgui/imgui.h"

/* proto */
void AddonLoad(AddonAPI* aApi);
void AddonUnload();
void AddonRender();
void AddonOptions();
void LoadConfig();
void SaveConfig();
void MarkConfigDirty();
void AutoSaveConfigIfDirty();
void UpdateManipulatorMouseState();
UINT AddonWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
bool LoadObjFile(const char* path);
void DrawRefObjOverlay();
void DrawManipulatorOverlay(const struct CameraFrame& camera, const ImVec2& viewport, ImDrawList* draw);
void PlaceObjectAtAvatarPosition();
void EnsureReferenceObjectFolder();
void RefreshReferenceObjectList(bool updateStatus = true);
bool LoadSelectedReferenceObject();
void OpenReferenceObjectFolder();
std::string GetConfigPath();
std::string GetFileNameOnly(const char* path);
void StoreCurrentTransformForLoadedObject();
bool ApplyStoredTransformForObject(const std::string& objectName);

/* globals */
AddonDefinition AddonDef = {};
HMODULE hSelf = nullptr;
AddonAPI* APIDefs = nullptr;
NexusLinkData* NexusLink = nullptr;
Mumble::Data* MumbleLink = nullptr;

static const char* CONFIG_FILE = "RefOBJ.json";
static const char* REFERENCE_OBJECT_FOLDER = "ReferenceObjects";

bool  g_overlayEnabled = true;
bool  g_showControlWindow = true;
bool  g_showDebug = false;
bool  g_drawWireframe = true;
bool  g_drawSolid = false;
bool  g_backfaceCull = true;
bool  g_frontSurfaceWireOnly = true;
bool  g_showManipulators = true;
int   g_manipulatorMode = 0; // 0=Move, 1=Scale, 2=Rotate

char  g_objPath[MAX_PATH] = "";
char  g_loadedObjectName[MAX_PATH] = "";
char  g_referenceFolder[MAX_PATH] = "";
char  g_lastStatus[512] = "Load Low-Poly 3D models as in-game overlay references";
std::vector<std::string> g_referenceObjects;
int   g_selectedReferenceObject = -1;
DWORD g_lastReferenceRefreshTick = 0;
bool  g_configDirty = false;
DWORD g_lastConfigDirtyTick = 0;
int   g_activeManipulator = 0;
ImVec2 g_manipDragStartMouse = ImVec2(0.0f, 0.0f);
float g_manipDragStartValue[3] = { 0.0f, 0.0f, 0.0f };
float g_manipDragStartScale = 1.0f;
bool  g_leftMouseWasDown = false;
bool  g_leftMouseDown = false;
bool  g_leftMouseClicked = false;
ImVec2 g_manipMousePos = ImVec2(0.0f, 0.0f);
bool  g_manipulatorWantsCapture = false;
bool  g_manipulatorInputCaptured = false;
int   g_hoveredManipulator = 0;
bool  g_wndLeftMouseDown = false;
bool  g_wndLeftMouseClickedPending = false;
ImVec2 g_wndMousePos = ImVec2(0.0f, 0.0f);

float g_worldPos[3] = { 0.0f, 0.0f, 0.0f };
float g_rotationDeg[3] = { 0.0f, 0.0f, 0.0f }; // pitch, yaw, roll in degrees
float g_objScale = 1.0f;
float g_nearClip = 0.05f;
float g_wireThickness = 1.5f;
float g_cullEpsilon = 0.0f;
float g_depthBias = 0.35f;
int   g_depthCellSize = 4;

float g_wireColor[4] = { 1.0f, 1.0f, 1.0f, 0.95f };
float g_solidColor[4] = { 0.25f, 0.75f, 1.0f, 0.20f };

struct Vec3f
{
    float x;
    float y;
    float z;
};

struct Triangle
{
    int a;
    int b;
    int c;
};

struct MeshData
{
    std::vector<Vec3f> vertices;
    std::vector<Triangle> triangles;
    Vec3f center = { 0.0f, 0.0f, 0.0f };
    float largestDimension = 1.0f;
};

MeshData g_mesh;

struct StoredTransform
{
    std::string objectName;
    float pos[3] = { 0.0f, 0.0f, 0.0f };
    float rot[3] = { 0.0f, 0.0f, 0.0f };
    float scale = 1.0f;
};

std::vector<StoredTransform> g_modelTransforms;


static Vec3f MakeVec3(float x, float y, float z)
{
    return { x, y, z };
}

static Vec3f ToVec3(const Vector3& v)
{
    return { v.X, v.Y, v.Z };
}

static Vec3f Add(Vec3f a, Vec3f b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static Vec3f Sub(Vec3f a, Vec3f b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static Vec3f Mul(Vec3f a, float s)
{
    return { a.x * s, a.y * s, a.z * s };
}

static float Dot(Vec3f a, Vec3f b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

static Vec3f Cross(Vec3f a, Vec3f b)
{
    return {
        (a.y * b.z) - (a.z * b.y),
        (a.z * b.x) - (a.x * b.z),
        (a.x * b.y) - (a.y * b.x)
    };
}

static float LengthSq(Vec3f v)
{
    return Dot(v, v);
}

static Vec3f NormalizeOr(Vec3f v, Vec3f fallback)
{
    float lenSq = LengthSq(v);
    if (lenSq < 0.000001f)
        return fallback;

    float invLen = 1.0f / std::sqrt(lenSq);
    return Mul(v, invLen);
}

static float DegToRad(float degrees)
{
    return degrees * 0.01745329251994329577f;
}

static float RadToDeg(float radians)
{
    return radians * 57.295779513082320876f;
}

struct Mat3f
{
    float m[3][3];
};

static Mat3f Mat3Identity()
{
    Mat3f out = {};
    out.m[0][0] = 1.0f;
    out.m[1][1] = 1.0f;
    out.m[2][2] = 1.0f;
    return out;
}

static Mat3f Mat3Mul(const Mat3f& a, const Mat3f& b)
{
    Mat3f out = {};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
        {
            out.m[r][c] =
                (a.m[r][0] * b.m[0][c]) +
                (a.m[r][1] * b.m[1][c]) +
                (a.m[r][2] * b.m[2][c]);
        }
    }
    return out;
}

static Mat3f Mat3RotateX(float radians)
{
    Mat3f out = Mat3Identity();
    float c = std::cos(radians);
    float s = std::sin(radians);
    out.m[1][1] = c;
    out.m[1][2] = -s;
    out.m[2][1] = s;
    out.m[2][2] = c;
    return out;
}

static Mat3f Mat3RotateY(float radians)
{
    Mat3f out = Mat3Identity();
    float c = std::cos(radians);
    float s = std::sin(radians);
    out.m[0][0] = c;
    out.m[0][2] = s;
    out.m[2][0] = -s;
    out.m[2][2] = c;
    return out;
}

static Mat3f Mat3RotateZ(float radians)
{
    Mat3f out = Mat3Identity();
    float c = std::cos(radians);
    float s = std::sin(radians);
    out.m[0][0] = c;
    out.m[0][1] = -s;
    out.m[1][0] = s;
    out.m[1][1] = c;
    return out;
}

static Mat3f Mat3AxisRotation(int axis, float radians)
{
    if (axis == 0) return Mat3RotateX(radians);
    if (axis == 1) return Mat3RotateY(radians);
    return Mat3RotateZ(radians);
}

static Mat3f RotationMatrixFromEulerXYZ(const float degrees[3])
{
    // Matches RotateObjectVector(): local vector is rotated X, then Y, then Z.
    // In matrix form for column vectors, that is M = Rz * Ry * Rx.
    Mat3f rx = Mat3RotateX(DegToRad(degrees[0]));
    Mat3f ry = Mat3RotateY(DegToRad(degrees[1]));
    Mat3f rz = Mat3RotateZ(DegToRad(degrees[2]));
    return Mat3Mul(rz, Mat3Mul(ry, rx));
}

static void EulerXYZFromRotationMatrix(const Mat3f& m, float outDegrees[3])
{
    // Extract angles for M = Rz * Ry * Rx.
    float sy = -m.m[2][0];
    sy = (std::max)(-1.0f, (std::min)(1.0f, sy));

    float y = std::asin(sy);
    float cy = std::cos(y);
    float x = 0.0f;
    float z = 0.0f;

    if (std::fabs(cy) > 0.00001f)
    {
        x = std::atan2(m.m[2][1], m.m[2][2]);
        z = std::atan2(m.m[1][0], m.m[0][0]);
    }
    else
    {
        // Gimbal fallback. Keep Z stable and recover X from the remaining matrix terms.
        x = std::atan2(-m.m[1][2], m.m[1][1]);
        z = 0.0f;
    }

    outDegrees[0] = RadToDeg(x);
    outDegrees[1] = RadToDeg(y);
    outDegrees[2] = RadToDeg(z);
}

static void ApplyLocalRotationDeltaFromDragStart(int localAxis, float deltaDegrees)
{
    // Apply incremental rotation in the object's own local space, not around world axes.
    // This prevents Y/Z rings from tumbling the whole gizmo after another axis is already rotated.
    Mat3f start = RotationMatrixFromEulerXYZ(g_manipDragStartValue);
    Mat3f delta = Mat3AxisRotation(localAxis, DegToRad(deltaDegrees));
    Mat3f result = Mat3Mul(start, delta);
    EulerXYZFromRotationMatrix(result, g_rotationDeg);
}

static Vec3f RotateX(Vec3f v, float radians)
{
    float c = std::cos(radians);
    float s = std::sin(radians);
    return { v.x, (v.y * c) - (v.z * s), (v.y * s) + (v.z * c) };
}

static Vec3f RotateY(Vec3f v, float radians)
{
    float c = std::cos(radians);
    float s = std::sin(radians);
    return { (v.x * c) + (v.z * s), v.y, (-v.x * s) + (v.z * c) };
}

static Vec3f RotateZ(Vec3f v, float radians)
{
    float c = std::cos(radians);
    float s = std::sin(radians);
    return { (v.x * c) - (v.y * s), (v.x * s) + (v.y * c), v.z };
}

static Vec3f RotateObjectVector(Vec3f v)
{
    v = RotateX(v, DegToRad(g_rotationDeg[0]));
    v = RotateY(v, DegToRad(g_rotationDeg[1]));
    v = RotateZ(v, DegToRad(g_rotationDeg[2]));
    return v;
}


static Vec3f TransformVertex(Vec3f v)
{
    Vec3f local = v;

    // RefOBJ keeps authored OBJ floor/height space intact.
    // The old normalize-on-load path centered and rescaled meshes, which broke floor contact.
    // GW2/Nexus projection currently needs an automatic horizontal flip to match reference meshes.
    local.x = -local.x;

    local = Mul(local, g_objScale);

    local = RotateObjectVector(local);

    return Add(local, MakeVec3(g_worldPos[0], g_worldPos[1], g_worldPos[2]));
}

struct CameraFrame
{
    Vec3f position;
    Vec3f forward;
    Vec3f up;
    Vec3f right;
    float fovRadians;

    static CameraFrame FromMumble(const Mumble::Data* mumble)
    {
        CameraFrame cam = {};
        cam.position = ToVec3(mumble->CameraPosition);
        cam.forward = NormalizeOr(ToVec3(mumble->CameraFront), MakeVec3(0.0f, 0.0f, 1.0f));

        // Same raw-space convention that fixed RaceFlow Planner:
        // raw GW2/Mumble world-up is +Y.
        Vec3f worldUp = MakeVec3(0.0f, 1.0f, 0.0f);
        Vec3f right = Cross(worldUp, cam.forward);
        if (LengthSq(right) < 0.000001f)
            right = MakeVec3(1.0f, 0.0f, 0.0f);

        cam.right = NormalizeOr(right, MakeVec3(1.0f, 0.0f, 0.0f));
        cam.up = NormalizeOr(Cross(cam.forward, cam.right), MakeVec3(0.0f, 1.0f, 0.0f));

        // Nexus Mumble.h exposes FOV through parsed Identity if available.
        cam.fovRadians = mumble->Context.MapID != 0 ? mumble->Context.Compass.Scale : 0.0f;

        // Do not use Compass.Scale as FOV. Parse FOV from Identity JSON is unreliable here,
        // so start with a stable 65-degree fallback. We can wire a proper FOV source next.
        cam.fovRadians = DegToRad(65.0f);

        return cam;
    }

    bool ProjectToScreen(Vec3f world, const ImVec2& viewport, ImVec2& outScreen, float& outDepth) const
    {
        outDepth = 0.0f;

        if (viewport.x <= 1.0f || viewport.y <= 1.0f)
            return false;

        Vec3f relative = Sub(world, position);

        float cameraX = Dot(relative, right);
        float cameraY = Dot(relative, up);
        float cameraZ = Dot(relative, forward);

        if (cameraZ <= g_nearClip)
            return false;

        float focalY = (viewport.y * 0.5f) / std::tan(fovRadians * 0.5f);

        outScreen.x = (viewport.x * 0.5f) + ((cameraX * focalY) / cameraZ);
        outScreen.y = (viewport.y * 0.5f) - ((cameraY * focalY) / cameraZ);
        outDepth = cameraZ;

        if (outScreen.x < -4000.0f || outScreen.x > viewport.x + 4000.0f ||
            outScreen.y < -4000.0f || outScreen.y > viewport.y + 4000.0f)
        {
            return false;
        }

        return true;
    }

    bool TryProject(Vec3f world, const ImVec2& viewport, ImVec2& outScreen) const
    {
        float depth = 0.0f;
        return ProjectToScreen(world, viewport, outScreen, depth);
    }
};


static std::string GetAddonDirectory()
{
    char modulePath[MAX_PATH] = "";
    if (hSelf != nullptr && GetModuleFileNameA(hSelf, modulePath, MAX_PATH) > 0)
    {
        std::string path(modulePath);
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            return path.substr(0, slash);
    }

    char currentDir[MAX_PATH] = "";
    if (GetCurrentDirectoryA(MAX_PATH, currentDir) > 0)
        return std::string(currentDir);

    return ".";
}


std::string GetConfigPath()
{
    EnsureReferenceObjectFolder();
    return std::string(g_referenceFolder) + "\\" + CONFIG_FILE;
}

std::string GetFileNameOnly(const char* path)
{
    if (path == nullptr || path[0] == '\0')
        return "";

    std::string value(path);
    size_t slash = value.find_last_of("\\/");
    if (slash != std::string::npos)
        value = value.substr(slash + 1);

    return value;
}

static int FindStoredTransformIndex(const std::string& objectName)
{
    if (objectName.empty())
        return -1;

    for (int i = 0; i < (int)g_modelTransforms.size(); ++i)
    {
        if (_stricmp(g_modelTransforms[(size_t)i].objectName.c_str(), objectName.c_str()) == 0)
            return i;
    }

    return -1;
}

void StoreCurrentTransformForLoadedObject()
{
    std::string objectName = g_loadedObjectName[0] ? std::string(g_loadedObjectName) : GetFileNameOnly(g_objPath);
    if (objectName.empty())
        return;

    int index = FindStoredTransformIndex(objectName);
    if (index < 0)
    {
        StoredTransform st;
        st.objectName = objectName;
        g_modelTransforms.push_back(st);
        index = (int)g_modelTransforms.size() - 1;
    }

    StoredTransform& st = g_modelTransforms[(size_t)index];
    for (int i = 0; i < 3; ++i)
    {
        st.pos[i] = g_worldPos[i];
        st.rot[i] = g_rotationDeg[i];
    }
    st.scale = g_objScale;
}

bool ApplyStoredTransformForObject(const std::string& objectName)
{
    int index = FindStoredTransformIndex(objectName);
    if (index < 0)
        return false;

    const StoredTransform& st = g_modelTransforms[(size_t)index];
    for (int i = 0; i < 3; ++i)
    {
        g_worldPos[i] = st.pos[i];
        g_rotationDeg[i] = st.rot[i];
    }
    g_objScale = st.scale;
    return true;
}

void MarkConfigDirty()
{
    StoreCurrentTransformForLoadedObject();
    g_configDirty = true;
    g_lastConfigDirtyTick = GetTickCount();
}

void AutoSaveConfigIfDirty()
{
    if (!g_configDirty)
        return;

    if (GetTickCount() - g_lastConfigDirtyTick < 800)
        return;

    SaveConfig();
}

void EnsureReferenceObjectFolder()
{
    std::string folder = GetAddonDirectory() + "\\" + REFERENCE_OBJECT_FOLDER;
    strncpy_s(g_referenceFolder, folder.c_str(), _TRUNCATE);
    CreateDirectoryA(g_referenceFolder, nullptr);
}

static bool EndsWithObj(const char* name)
{
    if (name == nullptr)
        return false;

    std::string s(name);
    if (s.size() < 4)
        return false;

    std::string ext = s.substr(s.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext == ".obj";
}

void RefreshReferenceObjectList(bool updateStatus)
{
    EnsureReferenceObjectFolder();

    std::string previouslySelected;
    if (g_selectedReferenceObject >= 0 && g_selectedReferenceObject < (int)g_referenceObjects.size())
        previouslySelected = g_referenceObjects[(size_t)g_selectedReferenceObject];

    std::vector<std::string> newList;
    std::string pattern = std::string(g_referenceFolder) + "\\*.*";

    WIN32_FIND_DATAA fd = {};
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && EndsWithObj(fd.cFileName))
                newList.push_back(fd.cFileName);
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
    }

    std::sort(newList.begin(), newList.end());

    bool changed = (newList != g_referenceObjects);
    g_referenceObjects = newList;
    g_selectedReferenceObject = -1;

    if (!previouslySelected.empty())
    {
        for (int i = 0; i < (int)g_referenceObjects.size(); ++i)
        {
            if (g_referenceObjects[(size_t)i] == previouslySelected)
            {
                g_selectedReferenceObject = i;
                break;
            }
        }
    }

    if (g_selectedReferenceObject < 0 && !g_referenceObjects.empty())
        g_selectedReferenceObject = 0;

    g_lastReferenceRefreshTick = GetTickCount();

    if (updateStatus || changed)
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus),
            "Reference folder refreshed: %zu OBJ file(s).", g_referenceObjects.size());
    }
}

void OpenReferenceObjectFolder()
{
    EnsureReferenceObjectFolder();

    if (g_referenceFolder[0] == '\0')
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "Reference object folder is not initialized.");
        return;
    }

    std::string quotedFolder = std::string("\"") + g_referenceFolder + "\"";

    SHELLEXECUTEINFOA sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = "open";
    sei.lpFile = "explorer.exe";
    sei.lpParameters = quotedFolder.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&sei))
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "Failed to open ReferenceObjects folder.");
        return;
    }

    std::snprintf(g_lastStatus, sizeof(g_lastStatus), "Opened ReferenceObjects folder.");
}

bool LoadSelectedReferenceObject()
{
    if (g_selectedReferenceObject < 0 || g_selectedReferenceObject >= (int)g_referenceObjects.size())
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "No reference OBJ selected.");
        return false;
    }

    std::string fullPath = std::string(g_referenceFolder) + "\\" + g_referenceObjects[(size_t)g_selectedReferenceObject];
    return LoadObjFile(fullPath.c_str());
}

static int ParseObjIndex(const std::string& token, int vertexCount)
{
    if (token.empty())
        return -1;

    size_t slash = token.find('/');
    std::string indexText = (slash == std::string::npos) ? token : token.substr(0, slash);

    if (indexText.empty())
        return -1;

    int idx = std::atoi(indexText.c_str());

    if (idx > 0)
        return idx - 1;

    if (idx < 0)
        return vertexCount + idx;

    return -1;
}

void PlaceObjectAtAvatarPosition()
{
    if (MumbleLink == nullptr)
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "Cannot place at character: MumbleLink unavailable.");
        return;
    }

    g_worldPos[0] = MumbleLink->AvatarPosition.X;
    g_worldPos[1] = MumbleLink->AvatarPosition.Y;
    g_worldPos[2] = MumbleLink->AvatarPosition.Z;
    MarkConfigDirty();
}

bool LoadObjFile(const char* path)
{
    std::ifstream file(path);
    if (!file.is_open())
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "OBJ load failed: could not open file.");
        return false;
    }

    MeshData mesh;
    std::string line;

    while (std::getline(file, line))
    {
        if (line.size() < 2)
            continue;

        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "v")
        {
            Vec3f v = {};
            ss >> v.x >> v.y >> v.z;
            mesh.vertices.push_back(v);
        }
        else if (tag == "f")
        {
            std::vector<int> faceIndices;
            std::string token;

            while (ss >> token)
            {
                int idx = ParseObjIndex(token, (int)mesh.vertices.size());
                if (idx >= 0 && idx < (int)mesh.vertices.size())
                    faceIndices.push_back(idx);
            }

            if (faceIndices.size() >= 3)
            {
                // Fan triangulation supports triangles, quads, and simple n-gons.
                for (size_t i = 1; i + 1 < faceIndices.size(); ++i)
                {
                    mesh.triangles.push_back({ faceIndices[0], faceIndices[i], faceIndices[i + 1] });
                }
            }
        }
    }

    if (mesh.vertices.empty() || mesh.triangles.empty())
    {
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "OBJ load failed: no usable vertices/faces.");
        return false;
    }

    Vec3f minV = mesh.vertices[0];
    Vec3f maxV = mesh.vertices[0];

    for (const Vec3f& v : mesh.vertices)
    {
        minV.x = (std::min)(minV.x, v.x); minV.y = (std::min)(minV.y, v.y); minV.z = (std::min)(minV.z, v.z);
        maxV.x = (std::max)(maxV.x, v.x); maxV.y = (std::max)(maxV.y, v.y); maxV.z = (std::max)(maxV.z, v.z);
    }

    mesh.center = MakeVec3(
        (minV.x + maxV.x) * 0.5f,
        (minV.y + maxV.y) * 0.5f,
        (minV.z + maxV.z) * 0.5f
    );

    float sizeX = (std::max)(0.001f, maxV.x - minV.x);
    float sizeY = (std::max)(0.001f, maxV.y - minV.y);
    float sizeZ = (std::max)(0.001f, maxV.z - minV.z);
    mesh.largestDimension = (std::max)(sizeX, (std::max)(sizeY, sizeZ));

    StoreCurrentTransformForLoadedObject();

    g_mesh = mesh;
    strncpy_s(g_objPath, path, _TRUNCATE);

    std::string objectName = GetFileNameOnly(path);
    strncpy_s(g_loadedObjectName, objectName.c_str(), _TRUNCATE);

    bool restoredTransform = ApplyStoredTransformForObject(objectName);
    MarkConfigDirty();

    std::snprintf(g_lastStatus, sizeof(g_lastStatus), restoredTransform
        ? "Loaded OBJ: %zu verts, %zu tris. Restored saved transform."
        : "Loaded OBJ: %zu verts, %zu tris.",
        g_mesh.vertices.size(), g_mesh.triangles.size());
    return true;
}



void UpdateManipulatorMouseState()
{
    // Use ImGui/Nexus state for normal UI, but once a manipulator owns the mouse,
    // trust the WndProc mouse state. Nexus/ImGui may not update MousePos/MouseDown
    // for background overlay drags after we swallow the message before the game sees it.
    ImGuiIO& io = ImGui::GetIO();

    bool usingWndMouse =
        g_manipulatorInputCaptured ||
        g_activeManipulator > 0 ||
        g_wndLeftMouseDown ||
        g_wndLeftMouseClickedPending;

    if (usingWndMouse)
        g_manipMousePos = g_wndMousePos;
    else if (io.MousePos.x >= 0.0f && io.MousePos.y >= 0.0f)
        g_manipMousePos = io.MousePos;
    else
        g_manipMousePos = g_wndMousePos;

    bool isDown = usingWndMouse ? g_wndLeftMouseDown : (io.MouseDown[0] || g_wndLeftMouseDown);

    g_leftMouseClicked = (isDown && !g_leftMouseWasDown) || g_wndLeftMouseClickedPending;
    g_leftMouseDown = isDown;
    g_leftMouseWasDown = isDown;
    g_wndLeftMouseClickedPending = false;
}

static ImU32 ColorU32(float r, float g, float b, float a = 1.0f)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
}

static float DistancePointToSegment(ImVec2 p, ImVec2 a, ImVec2 b)
{
    float vx = b.x - a.x;
    float vy = b.y - a.y;
    float wx = p.x - a.x;
    float wy = p.y - a.y;

    float c1 = (wx * vx) + (wy * vy);
    if (c1 <= 0.0f)
    {
        float dx = p.x - a.x;
        float dy = p.y - a.y;
        return std::sqrt((dx * dx) + (dy * dy));
    }

    float c2 = (vx * vx) + (vy * vy);
    if (c2 <= c1)
    {
        float dx = p.x - b.x;
        float dy = p.y - b.y;
        return std::sqrt((dx * dx) + (dy * dy));
    }

    float t = c1 / c2;
    ImVec2 proj(a.x + (t * vx), a.y + (t * vy));
    float dx = p.x - proj.x;
    float dy = p.y - proj.y;
    return std::sqrt((dx * dx) + (dy * dy));
}

static bool CanStartManipulatorDrag()
{
    // Do not start manipulator drags while the mouse is over normal Nexus/RefOBJ UI.
    // Outside UI windows, the manipulator uses ImGui/Nexus mouse state so camera/game input remains available.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        return false;

    if (ImGui::IsAnyItemHovered())
        return false;

    return true;
}

static void RequestManipulatorMouseCapture()
{
    // This is both a soft ImGui hint and the flag used by our Nexus WndProc hook
    // to stop mouse messages from reaching the game while a handle is hovered/dragged.
    g_manipulatorWantsCapture = true;

    ImGuiIO& io = ImGui::GetIO();
    io.WantCaptureMouse = true;
}

static bool ManipulatorCaptureButton(const char* id, ImVec2 center, float halfSize)
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImVec2 size(halfSize * 2.0f, halfSize * 2.0f);
    ImVec2 pos(center.x - halfSize, center.y - halfSize);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);

    bool clicked = false;
    if (ImGui::Begin(id, nullptr, flags))
    {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##capture", size);

        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            RequestManipulatorMouseCapture();
        }

        clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    }
    ImGui::End();

    return clicked;
}


static void DrawArrow2D(ImDrawList* draw, ImVec2 a, ImVec2 b, ImU32 color, float thickness)
{
    draw->AddLine(a, b, color, thickness);

    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float len = std::sqrt((dx * dx) + (dy * dy));
    if (len < 0.001f)
        return;

    dx /= len;
    dy /= len;

    float px = -dy;
    float py = dx;
    float head = 10.0f;
    float wing = 5.5f;

    ImVec2 p1(b.x - dx * head + px * wing, b.y - dy * head + py * wing);
    ImVec2 p2(b.x - dx * head - px * wing, b.y - dy * head - py * wing);
    draw->AddTriangleFilled(b, p1, p2, color);
}

static void DrawWorldRing(ImDrawList* draw, const CameraFrame& camera, const ImVec2& viewport,
    Vec3f center, int axis, float radius, ImU32 color, float thickness)
{
    const int segments = 96;
    ImVec2 prev = ImVec2(0.0f, 0.0f);
    bool prevValid = false;

    for (int i = 0; i <= segments; ++i)
    {
        float t = ((float)i / (float)segments) * 6.28318530718f;
        float c = std::cos(t);
        float sn = std::sin(t);

        Vec3f local = MakeVec3(0.0f, 0.0f, 0.0f);
        if (axis == 0)      // local X ring lives in local Y/Z
            local = MakeVec3(0.0f, c * radius, sn * radius);
        else if (axis == 1) // local Y ring lives in local X/Z
            local = MakeVec3(c * radius, 0.0f, sn * radius);
        else                // local Z ring lives in local X/Y
            local = MakeVec3(c * radius, sn * radius, 0.0f);

        Vec3f p = Add(center, RotateObjectVector(local));

        ImVec2 screen;
        bool valid = camera.TryProject(p, viewport, screen);
        if (valid && prevValid)
            draw->AddLine(prev, screen, color, thickness);

        prev = screen;
        prevValid = valid;
    }
}


static float DistancePointToWorldRing(const CameraFrame& camera, const ImVec2& viewport,
    Vec3f center, int axis, float radius, ImVec2 mouse)
{
    const int segments = 96;
    ImVec2 prev = ImVec2(0.0f, 0.0f);
    bool prevValid = false;
    float best = 999999.0f;

    for (int i = 0; i <= segments; ++i)
    {
        float t = ((float)i / (float)segments) * 6.28318530718f;
        float c = std::cos(t);
        float sn = std::sin(t);

        Vec3f local = MakeVec3(0.0f, 0.0f, 0.0f);
        if (axis == 0)
            local = MakeVec3(0.0f, c * radius, sn * radius);
        else if (axis == 1)
            local = MakeVec3(c * radius, 0.0f, sn * radius);
        else
            local = MakeVec3(c * radius, sn * radius, 0.0f);

        Vec3f p = Add(center, RotateObjectVector(local));

        ImVec2 screen;
        bool valid = camera.TryProject(p, viewport, screen);
        if (valid && prevValid)
            best = (std::min)(best, DistancePointToSegment(mouse, prev, screen));

        prev = screen;
        prevValid = valid;
    }

    return best;
}


void DrawManipulatorOverlay(const CameraFrame& camera, const ImVec2& viewport, ImDrawList* draw)
{
    g_hoveredManipulator = 0;

    if (!g_showManipulators || MumbleLink == nullptr || MumbleLink->Context.MapID == 0)
        return;

    Vec3f origin = MakeVec3(g_worldPos[0], g_worldPos[1], g_worldPos[2]);

    ImVec2 originScreen;
    if (!camera.TryProject(origin, viewport, originScreen))
        return;

    ImVec2 mouse = g_manipMousePos;

    ImU32 red = ColorU32(1.0f, 0.18f, 0.15f, 1.0f);
    ImU32 green = ColorU32(0.20f, 0.90f, 0.25f, 1.0f);
    ImU32 blue = ColorU32(0.25f, 0.55f, 1.0f, 1.0f);
    ImU32 yellow = ColorU32(1.0f, 0.85f, 0.10f, 1.0f);
    ImU32 white = ColorU32(1.0f, 1.0f, 1.0f, 0.95f);

    float distanceToCamera = std::sqrt(LengthSq(Sub(origin, camera.position)));
    float axisWorldLength = (std::max)(1.5f, distanceToCamera * 0.035f);

    Vec3f localAxisDir[3] =
    {
        NormalizeOr(RotateObjectVector(MakeVec3(1.0f, 0.0f, 0.0f)), MakeVec3(1.0f, 0.0f, 0.0f)),
        NormalizeOr(RotateObjectVector(MakeVec3(0.0f, 1.0f, 0.0f)), MakeVec3(0.0f, 1.0f, 0.0f)),
        NormalizeOr(RotateObjectVector(MakeVec3(0.0f, 0.0f, 1.0f)), MakeVec3(0.0f, 0.0f, 1.0f))
    };

    Vec3f axisWorld[3] =
    {
        Mul(localAxisDir[0], axisWorldLength),
        Mul(localAxisDir[1], axisWorldLength),
        Mul(localAxisDir[2], axisWorldLength)
    };

    ImVec2 axisScreen[3];
    bool axisValid[3] =
    {
        camera.TryProject(Add(origin, axisWorld[0]), viewport, axisScreen[0]),
        camera.TryProject(Add(origin, axisWorld[1]), viewport, axisScreen[1]),
        camera.TryProject(Add(origin, axisWorld[2]), viewport, axisScreen[2])
    };

    if (g_manipulatorMode == 0)
    {
        int hoverManipulator = 0;
        float bestDist = 9999.0f;
        int bestAxis = 0;

        for (int i = 0; i < 3; ++i)
        {
            if (!axisValid[i])
                continue;

            float d = DistancePointToSegment(mouse, originScreen, axisScreen[i]);
            if (d < bestDist)
            {
                bestDist = d;
                bestAxis = i + 1;
            }
        }

        float centerDx = mouse.x - originScreen.x;
        float centerDy = mouse.y - originScreen.y;
        float centerDist = std::sqrt((centerDx * centerDx) + (centerDy * centerDy));

        if (centerDist < 38.0f)
            hoverManipulator = 4;
        else if (bestDist < 38.0f)
            hoverManipulator = bestAxis;

        bool canManipulatorInput = (g_activeManipulator > 0) || CanStartManipulatorDrag();
        if (canManipulatorInput && hoverManipulator != 0)
            g_hoveredManipulator = hoverManipulator;

        bool captureClicked = false;
        if (canManipulatorInput && (hoverManipulator != 0 || g_activeManipulator > 0))
            captureClicked = ManipulatorCaptureButton("##RefOBJMoveManipulatorCapture", mouse, 64.0f);

        ImU32 highlight = ColorU32(0.00f, 0.95f, 1.00f, 1.0f);
        ImU32 xColor = (hoverManipulator == 1 || g_activeManipulator == 1) ? highlight : red;
        ImU32 yColor = (hoverManipulator == 2 || g_activeManipulator == 2) ? highlight : green;
        ImU32 zColor = (hoverManipulator == 3 || g_activeManipulator == 3) ? highlight : blue;
        ImU32 centerColor = (hoverManipulator == 4 || g_activeManipulator == 4) ? yellow : white;

        if (axisValid[0]) DrawArrow2D(draw, originScreen, axisScreen[0], xColor, (hoverManipulator == 1 || g_activeManipulator == 1) ? 5.0f : 3.0f);
        if (axisValid[1]) DrawArrow2D(draw, originScreen, axisScreen[1], yColor, (hoverManipulator == 2 || g_activeManipulator == 2) ? 5.0f : 3.0f);
        if (axisValid[2]) DrawArrow2D(draw, originScreen, axisScreen[2], zColor, (hoverManipulator == 3 || g_activeManipulator == 3) ? 5.0f : 3.0f);

        draw->AddCircleFilled(originScreen, (hoverManipulator == 4 || g_activeManipulator == 4) ? 9.0f : 6.0f, centerColor, 16);

        if (hoverManipulator != 0 || g_activeManipulator > 0)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            RequestManipulatorMouseCapture();
        }

        if ((captureClicked || g_leftMouseClicked) && canManipulatorInput && hoverManipulator != 0)
        {
            g_activeManipulator = hoverManipulator;
            g_manipDragStartMouse = mouse;
            for (int i = 0; i < 3; ++i)
                g_manipDragStartValue[i] = g_worldPos[i];
        }

        if (g_activeManipulator > 0 && g_leftMouseDown)
        {
            float dx = mouse.x - g_manipDragStartMouse.x;
            float dy = mouse.y - g_manipDragStartMouse.y;
            float worldPerPixel = distanceToCamera * 0.0008f;

            if (g_activeManipulator >= 1 && g_activeManipulator <= 3 && axisValid[g_activeManipulator - 1])
            {
                ImVec2 end = axisScreen[g_activeManipulator - 1];
                float ax = end.x - originScreen.x;
                float ay = end.y - originScreen.y;
                float alen = std::sqrt((ax * ax) + (ay * ay));
                if (alen > 0.001f)
                {
                    ax /= alen;
                    ay /= alen;
                    float signedPixels = (dx * ax) + (dy * ay);
                    float amount = signedPixels * worldPerPixel;
                    Vec3f dir = localAxisDir[g_activeManipulator - 1];
                    g_worldPos[0] = g_manipDragStartValue[0] + (dir.x * amount);
                    g_worldPos[1] = g_manipDragStartValue[1] + (dir.y * amount);
                    g_worldPos[2] = g_manipDragStartValue[2] + (dir.z * amount);
                    MarkConfigDirty();
                }
            }
            else if (g_activeManipulator == 4)
            {
                g_worldPos[0] = g_manipDragStartValue[0] + dx * worldPerPixel;
                g_worldPos[1] = g_manipDragStartValue[1] - dy * worldPerPixel;
                g_worldPos[2] = g_manipDragStartValue[2];
                MarkConfigDirty();
            }
        }
    }
    else if (g_manipulatorMode == 1)
    {
        ImVec2 boxMin(originScreen.x - 10.0f, originScreen.y - 10.0f);
        ImVec2 boxMax(originScreen.x + 10.0f, originScreen.y + 10.0f);

        bool scaleHover =
            mouse.x >= boxMin.x - 22.0f && mouse.x <= boxMax.x + 22.0f &&
            mouse.y >= boxMin.y - 22.0f && mouse.y <= boxMax.y + 22.0f;

        ImU32 highlight = ColorU32(0.00f, 0.95f, 1.00f, 1.0f);
        ImU32 scaleColor = (scaleHover || g_activeManipulator == 10) ? highlight : yellow;

        bool canManipulatorInput = (g_activeManipulator == 10) || CanStartManipulatorDrag();
        if (canManipulatorInput && scaleHover)
            g_hoveredManipulator = 10;

        bool captureClicked = false;
        if (canManipulatorInput && (scaleHover || g_activeManipulator == 10))
            captureClicked = ManipulatorCaptureButton("##RefOBJScaleManipulatorCapture", mouse, 46.0f);

        draw->AddRectFilled(boxMin, boxMax, scaleColor);
        draw->AddRect(boxMin, boxMax, white, 0.0f, 0, (scaleHover || g_activeManipulator == 10) ? 3.0f : 2.0f);

        if (scaleHover || g_activeManipulator == 10)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            RequestManipulatorMouseCapture();
        }

        if ((captureClicked || g_leftMouseClicked) && canManipulatorInput && scaleHover)
        {
            g_activeManipulator = 10;
            g_manipDragStartMouse = mouse;
            g_manipDragStartScale = g_objScale;
        }

        if (g_activeManipulator == 10 && g_leftMouseDown)
        {
            float dy = mouse.y - g_manipDragStartMouse.y;
            float factor = 1.0f - (dy * 0.01f);
            g_objScale = (std::max)(0.001f, g_manipDragStartScale * factor);
            MarkConfigDirty();
        }
    }
    else
    {
        float ringWorldRadius = (std::max)(1.0f, distanceToCamera * 0.025f);

        // Keep the visible ring planes aligned to their matching axis/color:
        // X/red = X ring, Y/green = Y ring, Z/blue = Z ring.
        // Do not swap the visible rings. The selected ring must rotate with its own axis.
        float ringDist[3] =
        {
            DistancePointToWorldRing(camera, viewport, origin, 0, ringWorldRadius, mouse),
            DistancePointToWorldRing(camera, viewport, origin, 1, ringWorldRadius, mouse),
            DistancePointToWorldRing(camera, viewport, origin, 2, ringWorldRadius, mouse)
        };

        int hoverAxis = -1;
        float bestRingDist = 44.0f;
        for (int i = 0; i < 3; ++i)
        {
            if (ringDist[i] < bestRingDist)
            {
                bestRingDist = ringDist[i];
                hoverAxis = i;
            }
        }

        bool canManipulatorInput = (g_activeManipulator >= 20 && g_activeManipulator <= 22) || CanStartManipulatorDrag();
        if (canManipulatorInput && hoverAxis >= 0)
            g_hoveredManipulator = 20 + hoverAxis;

        bool captureClicked = false;
        if (canManipulatorInput && (hoverAxis >= 0 || (g_activeManipulator >= 20 && g_activeManipulator <= 22)))
            captureClicked = ManipulatorCaptureButton("##RefOBJRotateManipulatorCapture", mouse, 66.0f);

        ImU32 highlight = ColorU32(0.00f, 0.95f, 1.00f, 1.0f);
        ImU32 ringColor[3] =
        {
            (hoverAxis == 0 || g_activeManipulator == 20) ? highlight : red,
            (hoverAxis == 1 || g_activeManipulator == 21) ? highlight : green,
            (hoverAxis == 2 || g_activeManipulator == 22) ? highlight : blue
        };

        DrawWorldRing(draw, camera, viewport, origin, 0, ringWorldRadius, ringColor[0], (hoverAxis == 0 || g_activeManipulator == 20) ? 3.5f : 2.0f);
        DrawWorldRing(draw, camera, viewport, origin, 1, ringWorldRadius, ringColor[1], (hoverAxis == 1 || g_activeManipulator == 21) ? 3.5f : 2.0f);
        DrawWorldRing(draw, camera, viewport, origin, 2, ringWorldRadius, ringColor[2], (hoverAxis == 2 || g_activeManipulator == 22) ? 3.5f : 2.0f);

        if (hoverAxis >= 0 || (g_activeManipulator >= 20 && g_activeManipulator <= 22))
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            RequestManipulatorMouseCapture();
        }

        if ((captureClicked || g_leftMouseClicked) && canManipulatorInput && hoverAxis >= 0)
        {
            g_activeManipulator = 20 + hoverAxis;
            g_manipDragStartMouse = mouse;
            for (int i = 0; i < 3; ++i)
                g_manipDragStartValue[i] = g_rotationDeg[i];
        }

        if (g_activeManipulator >= 20 && g_activeManipulator <= 22 && g_leftMouseDown)
        {
            int visualAxis = g_activeManipulator - 20;

            // Rotate around the selected ring's current LOCAL axis.
            // Do not directly edit only the Euler field, because after one axis is rotated,
            // changing another Euler value behaves like a world/order-dependent tumble.
            float dx = mouse.x - g_manipDragStartMouse.x;
            float dy = mouse.y - g_manipDragStartMouse.y;
            float amount = (std::fabs(dx) >= std::fabs(dy) ? dx : -dy) * 0.5f;
            ApplyLocalRotationDeltaFromDragStart(visualAxis, amount);
            MarkConfigDirty();
        }
    }

    if (!g_leftMouseDown)
    {
        g_activeManipulator = 0;
        g_manipulatorInputCaptured = false;
    }
}

struct ProjectedTri
{
    ImVec2 p[3];
    float z[3];
    Vec3f w[3];
    bool valid = false;
};

static float EdgeFunction(const ImVec2& a, const ImVec2& b, const ImVec2& c)
{
    return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

static void RasterizeDepthTriangle(const ProjectedTri& tri, std::vector<float>& depth, int cellsX, int cellsY, int cellSize)
{
    float minXf = (std::min)(tri.p[0].x, (std::min)(tri.p[1].x, tri.p[2].x));
    float maxXf = (std::max)(tri.p[0].x, (std::max)(tri.p[1].x, tri.p[2].x));
    float minYf = (std::min)(tri.p[0].y, (std::min)(tri.p[1].y, tri.p[2].y));
    float maxYf = (std::max)(tri.p[0].y, (std::max)(tri.p[1].y, tri.p[2].y));

    int minX = (std::max)(0, (int)std::floor(minXf / (float)cellSize));
    int maxX = (std::min)(cellsX - 1, (int)std::ceil(maxXf / (float)cellSize));
    int minY = (std::max)(0, (int)std::floor(minYf / (float)cellSize));
    int maxY = (std::min)(cellsY - 1, (int)std::ceil(maxYf / (float)cellSize));

    float area = EdgeFunction(tri.p[0], tri.p[1], tri.p[2]);
    if (std::fabs(area) < 0.00001f)
        return;

    for (int y = minY; y <= maxY; ++y)
    {
        for (int x = minX; x <= maxX; ++x)
        {
            ImVec2 p((x + 0.5f) * (float)cellSize, (y + 0.5f) * (float)cellSize);

            float w0 = EdgeFunction(tri.p[1], tri.p[2], p) / area;
            float w1 = EdgeFunction(tri.p[2], tri.p[0], p) / area;
            float w2 = EdgeFunction(tri.p[0], tri.p[1], p) / area;

            if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f)
                continue;

            float z = (w0 * tri.z[0]) + (w1 * tri.z[1]) + (w2 * tri.z[2]);
            size_t idx = (size_t)y * (size_t)cellsX + (size_t)x;
            if (z < depth[idx])
                depth[idx] = z;
        }
    }
}

static bool IsLineSampleVisible(const ImVec2& p, float z, const std::vector<float>& depth, int cellsX, int cellsY, int cellSize)
{
    int cx = (int)(p.x / (float)cellSize);
    int cy = (int)(p.y / (float)cellSize);

    if (cx < 0 || cy < 0 || cx >= cellsX || cy >= cellsY)
        return false;

    float nearest = depth[(size_t)cy * (size_t)cellsX + (size_t)cx];
    return z <= nearest + g_depthBias;
}

static void DrawDepthTestedLine(ImDrawList* draw, const ImVec2& a, float za, const ImVec2& b, float zb,
    const std::vector<float>& depth, int cellsX, int cellsY, int cellSize, ImU32 color, float thickness)
{
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float length = std::sqrt((dx * dx) + (dy * dy));
    int steps = (std::max)(1, (int)(length / 8.0f));

    bool inSpan = false;
    ImVec2 spanStart = a;
    float spanStartT = 0.0f;
    ImVec2 prev = a;
    bool prevVisible = false;

    for (int i = 0; i <= steps; ++i)
    {
        float t = (float)i / (float)steps;
        ImVec2 p(a.x + dx * t, a.y + dy * t);
        float z = za + ((zb - za) * t);
        bool visible = IsLineSampleVisible(p, z, depth, cellsX, cellsY, cellSize);

        if (visible && !inSpan)
        {
            inSpan = true;
            spanStart = p;
            spanStartT = t;
        }
        else if (!visible && inSpan)
        {
            draw->AddLine(spanStart, prev, color, thickness);
            inSpan = false;
        }

        prev = p;
        prevVisible = visible;
    }

    if (inSpan)
        draw->AddLine(spanStart, b, color, thickness);
}

void DrawRefObjOverlay()
{
    if (!g_overlayEnabled || MumbleLink == nullptr || MumbleLink->Context.MapID == 0 || g_mesh.vertices.empty() || g_mesh.triangles.empty())
        return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 viewport = io.DisplaySize;
    if (viewport.x <= 1.0f || viewport.y <= 1.0f)
        return;

    CameraFrame camera = CameraFrame::FromMumble(MumbleLink);
    // Background draw list keeps RefOBJ geometry behind Nexus/ImGui panel content.
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    ImU32 wireColor = ImGui::ColorConvertFloat4ToU32(ImVec4(g_wireColor[0], g_wireColor[1], g_wireColor[2], g_wireColor[3]));
    ImU32 solidColor = ImGui::ColorConvertFloat4ToU32(ImVec4(g_solidColor[0], g_solidColor[1], g_solidColor[2], g_solidColor[3]));

    std::vector<ProjectedTri> projected;
    projected.reserve(g_mesh.triangles.size());

    for (const Triangle& tri : g_mesh.triangles)
    {
        if (tri.a < 0 || tri.b < 0 || tri.c < 0 ||
            tri.a >= (int)g_mesh.vertices.size() ||
            tri.b >= (int)g_mesh.vertices.size() ||
            tri.c >= (int)g_mesh.vertices.size())
        {
            continue;
        }

        Vec3f wa = TransformVertex(g_mesh.vertices[tri.a]);
        Vec3f wb = TransformVertex(g_mesh.vertices[tri.b]);
        Vec3f wc = TransformVertex(g_mesh.vertices[tri.c]);

        if (g_backfaceCull)
        {
            Vec3f normal = Cross(Sub(wc, wa), Sub(wb, wa));
            Vec3f toCamera = Sub(camera.position, wa);

            if (Dot(normal, toCamera) <= g_cullEpsilon)
                continue;
        }

        ProjectedTri pt = {};
        pt.w[0] = wa; pt.w[1] = wb; pt.w[2] = wc;

        if (!camera.ProjectToScreen(wa, viewport, pt.p[0], pt.z[0]) ||
            !camera.ProjectToScreen(wb, viewport, pt.p[1], pt.z[1]) ||
            !camera.ProjectToScreen(wc, viewport, pt.p[2], pt.z[2]))
        {
            continue;
        }

        pt.valid = true;
        projected.push_back(pt);
    }

    if (projected.empty())
        return;

    if (g_drawSolid)
    {
        // Far-to-near painter order makes translucent fill behave better in ImGui.
        std::vector<size_t> order(projected.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
            {
                float za = projected[a].z[0] + projected[a].z[1] + projected[a].z[2];
                float zb = projected[b].z[0] + projected[b].z[1] + projected[b].z[2];
                return za > zb;
            });

        for (size_t idx : order)
        {
            const ProjectedTri& pt = projected[idx];
            draw->AddTriangleFilled(pt.p[0], pt.p[1], pt.p[2], solidColor);
        }
    }

    if (!g_drawWireframe)
    {
        DrawManipulatorOverlay(camera, viewport, draw);
        return;
    }

    if (!g_frontSurfaceWireOnly)
    {
        for (const ProjectedTri& pt : projected)
        {
            draw->AddLine(pt.p[0], pt.p[1], wireColor, g_wireThickness);
            draw->AddLine(pt.p[1], pt.p[2], wireColor, g_wireThickness);
            draw->AddLine(pt.p[2], pt.p[0], wireColor, g_wireThickness);
        }
        DrawManipulatorOverlay(camera, viewport, draw);
        return;
    }

    int cellSize = (std::max)(1, g_depthCellSize);
    int cellsX = (std::max)(1, (int)std::ceil(viewport.x / (float)cellSize));
    int cellsY = (std::max)(1, (int)std::ceil(viewport.y / (float)cellSize));
    std::vector<float> depth((size_t)cellsX * (size_t)cellsY, 1.0e30f);

    for (const ProjectedTri& pt : projected)
        RasterizeDepthTriangle(pt, depth, cellsX, cellsY, cellSize);

    for (const ProjectedTri& pt : projected)
    {
        DrawDepthTestedLine(draw, pt.p[0], pt.z[0], pt.p[1], pt.z[1], depth, cellsX, cellsY, cellSize, wireColor, g_wireThickness);
        DrawDepthTestedLine(draw, pt.p[1], pt.z[1], pt.p[2], pt.z[2], depth, cellsX, cellsY, cellSize, wireColor, g_wireThickness);
        DrawDepthTestedLine(draw, pt.p[2], pt.z[2], pt.p[0], pt.z[0], depth, cellsX, cellsY, cellSize, wireColor, g_wireThickness);
    }

    DrawManipulatorOverlay(camera, viewport, draw);
}

static bool IsManipulatorMouseMessage(UINT uMsg)
{
    switch (uMsg)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        return true;
    default:
        return false;
    }
}

UINT AddonWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (IsManipulatorMouseMessage(uMsg))
    {
        g_wndMousePos = ImVec2((float)(short)LOWORD(lParam), (float)(short)HIWORD(lParam));

        // Blish-style rule: do not block the game just because the overlay exists.
        // Only claim input after the previous render frame identified a manipulator
        // handle under the cursor, or while a manipulator drag is already active.
        bool canClaimManipulatorInput =
            g_manipulatorInputCaptured ||
            g_activeManipulator > 0 ||
            g_hoveredManipulator != 0 ||
            g_manipulatorWantsCapture;

        if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK)
        {
            if (canClaimManipulatorInput)
            {
                g_manipulatorInputCaptured = true;
                g_wndLeftMouseDown = true;
                g_wndLeftMouseClickedPending = true;
                return 0;
            }
        }
        else if (uMsg == WM_LBUTTONUP)
        {
            if (g_manipulatorInputCaptured || g_activeManipulator > 0)
            {
                g_wndLeftMouseDown = false;
                g_manipulatorInputCaptured = false;
                return 0;
            }

            g_wndLeftMouseDown = false;
        }
        else if (uMsg == WM_MOUSEMOVE)
        {
            if (g_manipulatorInputCaptured || g_activeManipulator > 0)
                return 0;
        }
    }

    return 1;
}

///----------------------------------------------------------------------------------------------------
/// DllMain
///----------------------------------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH: hSelf = hModule; break;
    case DLL_PROCESS_DETACH: break;
    case DLL_THREAD_ATTACH: break;
    case DLL_THREAD_DETACH: break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef()
{
    AddonDef.Signature = 0xFF81F9E2;
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "RefOBJ";
    AddonDef.Version.Major = 1;
    AddonDef.Version.Minor = 1;
    AddonDef.Version.Build = 0;
    AddonDef.Version.Revision = 21;
    AddonDef.Author = "Girbilcannon.8259";
    AddonDef.Description = "Load Low-Poly 3D models as in-game overlay references.";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = EAddonFlags_None;

    return &AddonDef;
}

void AddonLoad(AddonAPI* aApi)
{
    APIDefs = aApi;

    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc, (void(*)(void*, void*))APIDefs->ImguiFree);

    NexusLink = (NexusLinkData*)APIDefs->DataLink.Get("DL_NEXUS_LINK");
    MumbleLink = (Mumble::Data*)APIDefs->DataLink.Get("DL_MUMBLE_LINK");

    APIDefs->Renderer.Register(ERenderType_Render, AddonRender);
    APIDefs->Renderer.Register(ERenderType_OptionsRender, AddonOptions);
    APIDefs->WndProc.Register(AddonWndProc);

    EnsureReferenceObjectFolder();
    RefreshReferenceObjectList();

    LoadConfig();

    if (g_objPath[0] != '\0')
        LoadObjFile(g_objPath);

    APIDefs->Log(ELogLevel_INFO, "RefOBJ", "RefOBJ loaded.");
}

void AddonUnload()
{
    SaveConfig();

    APIDefs->WndProc.Deregister(AddonWndProc);
    APIDefs->Renderer.Deregister(AddonRender);
    APIDefs->Renderer.Deregister(AddonOptions);

    APIDefs->Log(ELogLevel_INFO, "RefOBJ", "RefOBJ unloaded.");
}


static void UiSectionLabel(const char* text)
{
    ImGui::TextColored(ImVec4(0.36f, 0.70f, 1.00f, 1.00f), "%s", text);
}

static void UiHelpText(const char* text)
{
    ImGui::TextDisabled("%s", text);
}

void AddonRender()
{
    AutoSaveConfigIfDirty();

    if (GetTickCount() - g_lastReferenceRefreshTick > 1500)
        RefreshReferenceObjectList(false);

    // Reset each frame; DrawManipulatorOverlay sets this true again when a handle is hovered/dragged.
    g_manipulatorWantsCapture = (g_activeManipulator > 0);

    UpdateManipulatorMouseState();
    DrawRefObjOverlay();

    if (!g_showControlWindow)
        return;

    ImGui::SetNextWindowSize(ImVec2(390.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));

    if (ImGui::Begin("RefOBJ v1.1.0.21"))
    {
        ImGui::TextWrapped("Load Low-Poly 3D models as in-game overlay references");
        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Checkbox("Enable Overlay", &g_overlayEnabled)) MarkConfigDirty();
        ImGui::SameLine();
        if (ImGui::Checkbox("Debug", &g_showDebug)) MarkConfigDirty();

        ImGui::Spacing();
        ImGui::Text("Reference Objects Folder");
        ImGui::TextWrapped("%s", g_referenceFolder[0] ? g_referenceFolder : "(not initialized)");
        ImGui::TextDisabled("Add .obj files here, then refresh or wait for auto-refresh.");

        if (ImGui::Button("Open OBJ Folder"))
            OpenReferenceObjectFolder();
        ImGui::SameLine();
        if (ImGui::Button("Refresh OBJ List"))
            RefreshReferenceObjectList();
        ImGui::SameLine();
        if (ImGui::Button("Load Selected OBJ"))
            LoadSelectedReferenceObject();

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("Reference OBJ", (g_selectedReferenceObject >= 0 && g_selectedReferenceObject < (int)g_referenceObjects.size()) ? g_referenceObjects[(size_t)g_selectedReferenceObject].c_str() : "<none>"))
        {
            for (int i = 0; i < (int)g_referenceObjects.size(); ++i)
            {
                bool selected = (i == g_selectedReferenceObject);
                if (ImGui::Selectable(g_referenceObjects[(size_t)i].c_str(), selected))
                    g_selectedReferenceObject = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button("Place At Character"))
            PlaceObjectAtAvatarPosition();

        ImGui::Spacing();
        ImGui::TextWrapped("Status: %s", g_lastStatus);

        ImGui::Spacing();
        ImGui::Separator();

        if (ImGui::Checkbox("Show Manipulator Handles", &g_showManipulators)) MarkConfigDirty();
        if (g_showManipulators)
        {
            const char* manipModes[] = { "Move", "Scale", "Rotate" };
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##ManipulatorMode", &g_manipulatorMode, manipModes, IM_ARRAYSIZE(manipModes))) MarkConfigDirty();
        }

        UiSectionLabel("World Position");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("##WorldPosition", g_worldPos, 0.1f, -50000.0f, 50000.0f, "%.3f")) MarkConfigDirty();

        UiSectionLabel("Scale");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##Scale", &g_objScale, 0.05f, 0.001f, 10000.0f, "%.4f")) MarkConfigDirty();

        UiSectionLabel("Rotation XYZ");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat3("##RotationXYZ", g_rotationDeg, 0.5f, -360.0f, 360.0f, "%.2f")) MarkConfigDirty();

        ImGui::Spacing();
        ImGui::Separator();
        UiSectionLabel("Display");
        if (ImGui::Checkbox("Wireframe", &g_drawWireframe)) MarkConfigDirty();
        ImGui::SameLine();
        if (ImGui::Checkbox("Solid Fill", &g_drawSolid)) MarkConfigDirty();
        if (ImGui::Checkbox("Backface Culling", &g_backfaceCull)) MarkConfigDirty();
        ImGui::SameLine();
        if (ImGui::Checkbox("Front Surface Wire Only", &g_frontSurfaceWireOnly)) MarkConfigDirty();

        UiSectionLabel("Depth Bias");
        UiHelpText("Tolerance for front-surface filtering. Raise slightly if front wires flicker.");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##DepthBias", &g_depthBias, 0.02f, 0.0f, 10.0f, "%.2f")) MarkConfigDirty();

        UiSectionLabel("Depth Cell Size");
        UiHelpText("Depth filter grid size. Smaller is more precise; larger is smoother/faster.");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragInt("##DepthCellSize", &g_depthCellSize, 1.0f, 1, 16)) MarkConfigDirty();

        UiSectionLabel("Wire Thickness");
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::DragFloat("##WireThickness", &g_wireThickness, 0.05f, 0.1f, 10.0f, "%.2f")) MarkConfigDirty();

        UiSectionLabel("Wire Color");
        if (ImGui::ColorEdit4("##WireColor", g_wireColor)) MarkConfigDirty();
        UiSectionLabel("Material Color");
        if (ImGui::ColorEdit4("##MaterialColor", g_solidColor)) MarkConfigDirty();

        if (g_showDebug)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Debug");

            if (MumbleLink == nullptr)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "MumbleLink: unavailable");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.35f, 1.0f), "MumbleLink: available");
                ImGui::Text("Map ID: %u", MumbleLink->Context.MapID);
                ImGui::Text("Avatar: %.6f, %.6f, %.6f", MumbleLink->AvatarPosition.X, MumbleLink->AvatarPosition.Y, MumbleLink->AvatarPosition.Z);
                ImGui::Text("Camera Pos: %.6f, %.6f, %.6f", MumbleLink->CameraPosition.X, MumbleLink->CameraPosition.Y, MumbleLink->CameraPosition.Z);
                ImGui::Text("Camera Front: %.6f, %.6f, %.6f", MumbleLink->CameraFront.X, MumbleLink->CameraFront.Y, MumbleLink->CameraFront.Z);
                ImGui::Text("Camera Top: %.6f, %.6f, %.6f", MumbleLink->CameraTop.X, MumbleLink->CameraTop.Y, MumbleLink->CameraTop.Z);
            }

            ImGui::Text("Mesh: %zu vertices, %zu triangles", g_mesh.vertices.size(), g_mesh.triangles.size());
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
}

void AddonOptions()
{
    ImGui::Text("RefOBJ v1.1.0.21");
    if (ImGui::Checkbox("Show RefOBJ control window", &g_showControlWindow)) MarkConfigDirty();
    if (ImGui::Checkbox("Enable overlay", &g_overlayEnabled)) MarkConfigDirty();
    if (ImGui::Checkbox("Show debug values", &g_showDebug)) MarkConfigDirty();

}

static std::string JsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value)
    {
        switch (c)
        {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

static bool JsonFindBool(const std::string& json, const char* key, bool fallback)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return fallback;
    p = json.find(':', p);
    if (p == std::string::npos) return fallback;
    size_t v = json.find_first_not_of(" \t\r\n", p + 1);
    if (v == std::string::npos) return fallback;
    if (json.compare(v, 4, "true") == 0) return true;
    if (json.compare(v, 5, "false") == 0) return false;
    return fallback;
}

static float JsonFindFloat(const std::string& json, const char* key, float fallback)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return fallback;
    p = json.find(':', p);
    if (p == std::string::npos) return fallback;
    return (float)std::atof(json.c_str() + p + 1);
}

static int JsonFindInt(const std::string& json, const char* key, int fallback)
{
    return (int)JsonFindFloat(json, key, (float)fallback);
}

static std::string JsonFindString(const std::string& json, const char* key, const std::string& fallback)
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return fallback;
    p = json.find(':', p);
    if (p == std::string::npos) return fallback;
    p = json.find('"', p + 1);
    if (p == std::string::npos) return fallback;

    std::string out;
    bool escaped = false;
    for (size_t i = p + 1; i < json.size(); ++i)
    {
        char c = json[i];
        if (escaped)
        {
            switch (c)
            {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            default: out += c; break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\')
        {
            escaped = true;
            continue;
        }
        if (c == '"')
            return out;
        out += c;
    }

    return fallback;
}

static bool JsonFindFloatArray3(const std::string& json, const char* key, float values[3])
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    p = json.find('[', p);
    if (p == std::string::npos) return false;
    return sscanf_s(json.c_str() + p + 1, "%f , %f , %f", &values[0], &values[1], &values[2]) == 3;
}

static bool JsonFindFloatArray4(const std::string& json, const char* key, float values[4])
{
    std::string pattern = std::string("\"") + key + "\"";
    size_t p = json.find(pattern);
    if (p == std::string::npos) return false;
    p = json.find('[', p);
    if (p == std::string::npos) return false;
    return sscanf_s(json.c_str() + p + 1, "%f , %f , %f , %f", &values[0], &values[1], &values[2], &values[3]) == 4;
}

void SaveConfig()
{
    StoreCurrentTransformForLoadedObject();

    std::string path = GetConfigPath();
    std::ofstream f(path, std::ios::out | std::ios::trunc);
    if (!f.is_open())
        return;

    f << "{\n";
    f << "  \"version\": \"1.1.0.20\",\n";
    f << "  \"overlay\": " << (g_overlayEnabled ? "true" : "false") << ",\n";
    f << "  \"showWindow\": " << (g_showControlWindow ? "true" : "false") << ",\n";
    f << "  \"showDebug\": " << (g_showDebug ? "true" : "false") << ",\n";
    f << "  \"drawWire\": " << (g_drawWireframe ? "true" : "false") << ",\n";
    f << "  \"drawSolid\": " << (g_drawSolid ? "true" : "false") << ",\n";
    f << "  \"backfaceCull\": " << (g_backfaceCull ? "true" : "false") << ",\n";
    f << "  \"frontSurfaceWireOnly\": " << (g_frontSurfaceWireOnly ? "true" : "false") << ",\n";
    f << "  \"showManipulators\": " << (g_showManipulators ? "true" : "false") << ",\n";
    f << "  \"manipulatorMode\": " << g_manipulatorMode << ",\n";
    f << "  \"depthBias\": " << g_depthBias << ",\n";
    f << "  \"depthCellSize\": " << g_depthCellSize << ",\n";
    f << "  \"objPath\": \"" << JsonEscape(g_objPath) << "\",\n";
    f << "  \"activeObject\": \"" << JsonEscape(g_loadedObjectName) << "\",\n";
    f << "  \"wireThickness\": " << g_wireThickness << ",\n";
    f << "  \"wireColor\": [" << g_wireColor[0] << ", " << g_wireColor[1] << ", " << g_wireColor[2] << ", " << g_wireColor[3] << "],\n";
    f << "  \"solidColor\": [" << g_solidColor[0] << ", " << g_solidColor[1] << ", " << g_solidColor[2] << ", " << g_solidColor[3] << "],\n";
    f << "  \"transforms\": [\n";

    for (size_t i = 0; i < g_modelTransforms.size(); ++i)
    {
        const StoredTransform& st = g_modelTransforms[i];
        f << "    {\n";
        f << "      \"model\": \"" << JsonEscape(st.objectName) << "\",\n";
        f << "      \"position\": [" << st.pos[0] << ", " << st.pos[1] << ", " << st.pos[2] << "],\n";
        f << "      \"rotation\": [" << st.rot[0] << ", " << st.rot[1] << ", " << st.rot[2] << "],\n";
        f << "      \"scale\": " << st.scale << "\n";
        f << "    }" << ((i + 1 < g_modelTransforms.size()) ? "," : "") << "\n";
    }

    f << "  ]\n";
    f << "}\n";

    g_manipulatorMode = (std::max)(0, (std::min)(2, g_manipulatorMode));
    g_configDirty = false;
}

void LoadConfig()
{
    std::string path = GetConfigPath();
    std::ifstream f(path);
    if (!f.is_open())
        return;

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string json = buffer.str();

    g_overlayEnabled = JsonFindBool(json, "overlay", g_overlayEnabled);
    g_showControlWindow = JsonFindBool(json, "showWindow", g_showControlWindow);
    g_showDebug = JsonFindBool(json, "showDebug", g_showDebug);
    g_drawWireframe = JsonFindBool(json, "drawWire", g_drawWireframe);
    g_drawSolid = JsonFindBool(json, "drawSolid", g_drawSolid);
    g_backfaceCull = JsonFindBool(json, "backfaceCull", g_backfaceCull);
    g_frontSurfaceWireOnly = JsonFindBool(json, "frontSurfaceWireOnly", g_frontSurfaceWireOnly);
    g_showManipulators = JsonFindBool(json, "showManipulators", g_showManipulators);
    g_manipulatorMode = JsonFindInt(json, "manipulatorMode", g_manipulatorMode);
    g_depthBias = JsonFindFloat(json, "depthBias", g_depthBias);
    g_depthCellSize = JsonFindInt(json, "depthCellSize", g_depthCellSize);
    g_wireThickness = JsonFindFloat(json, "wireThickness", g_wireThickness);

    std::string objPath = JsonFindString(json, "objPath", "");
    if (!objPath.empty())
        strncpy_s(g_objPath, objPath.c_str(), _TRUNCATE);

    std::string activeObject = JsonFindString(json, "activeObject", "");
    if (!activeObject.empty())
        strncpy_s(g_loadedObjectName, activeObject.c_str(), _TRUNCATE);

    JsonFindFloatArray4(json, "wireColor", g_wireColor);
    JsonFindFloatArray4(json, "solidColor", g_solidColor);

    g_modelTransforms.clear();
    size_t transformsKey = json.find("\"transforms\"");
    if (transformsKey != std::string::npos)
    {
        size_t arrayStart = json.find('[', transformsKey);
        size_t arrayEnd = json.rfind(']');
        if (arrayStart != std::string::npos && arrayEnd != std::string::npos)
        {
            size_t p = arrayStart;
            while (true)
            {
                size_t objStart = json.find('{', p);
                if (objStart == std::string::npos || objStart > arrayEnd)
                    break;
                size_t objEnd = json.find('}', objStart);
                if (objEnd == std::string::npos || objEnd > arrayEnd)
                    break;

                std::string block = json.substr(objStart, objEnd - objStart + 1);
                StoredTransform st;
                st.objectName = JsonFindString(block, "model", "");
                JsonFindFloatArray3(block, "position", st.pos);
                JsonFindFloatArray3(block, "rotation", st.rot);
                st.scale = JsonFindFloat(block, "scale", st.scale);

                if (!st.objectName.empty())
                    g_modelTransforms.push_back(st);

                p = objEnd + 1;
            }
        }
    }

    if (g_loadedObjectName[0] != '\0')
        ApplyStoredTransformForObject(g_loadedObjectName);

    g_manipulatorMode = (std::max)(0, (std::min)(2, g_manipulatorMode));
    g_configDirty = false;
}

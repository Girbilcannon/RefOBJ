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
bool LoadObjFile(const char* path);
void DrawRefObjOverlay();
void PlaceObjectAtAvatarPosition();
void EnsureReferenceObjectFolder();
void RefreshReferenceObjectList(bool updateStatus = true);
bool LoadSelectedReferenceObject();
void OpenReferenceObjectFolder();

/* globals */
AddonDefinition AddonDef = {};
HMODULE hSelf = nullptr;
AddonAPI* APIDefs = nullptr;
NexusLinkData* NexusLink = nullptr;
Mumble::Data* MumbleLink = nullptr;

static const char* CONFIG_FILE = "RefOBJ.ini";
static const char* REFERENCE_OBJECT_FOLDER = "ReferenceObjects";

bool  g_overlayEnabled = true;
bool  g_showControlWindow = true;
bool  g_showDebug = false;
bool  g_drawWireframe = true;
bool  g_drawSolid = false;
bool  g_backfaceCull = true;
bool  g_placeAtAvatarOnLoad = true;
bool  g_frontSurfaceWireOnly = true;

char  g_objPath[MAX_PATH] = "";
char  g_referenceFolder[MAX_PATH] = "";
char  g_lastStatus[512] = "Load Low-Poly 3D models as in-game overlay references";
std::vector<std::string> g_referenceObjects;
int   g_selectedReferenceObject = -1;
DWORD g_lastReferenceRefreshTick = 0;

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

static Vec3f TransformVertex(Vec3f v)
{
    Vec3f local = v;

    // RefOBJ keeps authored OBJ floor/height space intact.
    // The old normalize-on-load path centered and rescaled meshes, which broke floor contact.
    // GW2/Nexus projection currently needs an automatic horizontal flip to match reference meshes.
    local.x = -local.x;

    local = Mul(local, g_objScale);

    local = RotateX(local, DegToRad(g_rotationDeg[0]));
    local = RotateY(local, DegToRad(g_rotationDeg[1]));
    local = RotateZ(local, DegToRad(g_rotationDeg[2]));

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

    g_mesh = mesh;
    strncpy_s(g_objPath, path, _TRUNCATE);

    if (g_placeAtAvatarOnLoad && MumbleLink != nullptr)
        PlaceObjectAtAvatarPosition();

    std::snprintf(g_lastStatus, sizeof(g_lastStatus), "Loaded OBJ: %zu verts, %zu tris", g_mesh.vertices.size(), g_mesh.triangles.size());
    return true;
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
    if (!g_overlayEnabled || MumbleLink == nullptr || g_mesh.vertices.empty() || g_mesh.triangles.empty())
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
        return;

    if (!g_frontSurfaceWireOnly)
    {
        for (const ProjectedTri& pt : projected)
        {
            draw->AddLine(pt.p[0], pt.p[1], wireColor, g_wireThickness);
            draw->AddLine(pt.p[1], pt.p[2], wireColor, g_wireThickness);
            draw->AddLine(pt.p[2], pt.p[0], wireColor, g_wireThickness);
        }
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
    AddonDef.Signature = -8259102;
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "RefOBJ";
    AddonDef.Version.Major = 1;
    AddonDef.Version.Minor = 0;
    AddonDef.Version.Build = 0;
    AddonDef.Version.Revision = 0;
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
    if (GetTickCount() - g_lastReferenceRefreshTick > 1500)
        RefreshReferenceObjectList(false);

    DrawRefObjOverlay();

    if (!g_showControlWindow)
        return;

    ImGui::SetNextWindowSize(ImVec2(390.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));

    if (ImGui::Begin("RefOBJ v1.0.0"))
    {
        ImGui::TextWrapped("Load Low-Poly 3D models as in-game overlay references");
        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Checkbox("Enable Overlay", &g_overlayEnabled);
        ImGui::SameLine();
        ImGui::Checkbox("Debug", &g_showDebug);

        ImGui::Spacing();
        UiSectionLabel("Reference Objects Folder");
        ImGui::TextWrapped("%s", g_referenceFolder[0] ? g_referenceFolder : "(not initialized)");

        ImGui::Spacing();
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
        ImGui::SameLine();
        ImGui::Checkbox("Place when loading", &g_placeAtAvatarOnLoad);

        ImGui::Spacing();
        ImGui::TextWrapped("Status: %s", g_lastStatus);

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();

        UiSectionLabel("World Position");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##WorldPosition", g_worldPos, 0.1f, -50000.0f, 50000.0f, "%.3f");

        UiSectionLabel("Scale");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##Scale", &g_objScale, 0.05f, 0.001f, 10000.0f, "%.4f");

        UiSectionLabel("Rotation XYZ");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat3("##RotationXYZ", g_rotationDeg, 0.5f, -360.0f, 360.0f, "%.2f");

        ImGui::Spacing();
        ImGui::Separator();
        UiSectionLabel("Display");
        ImGui::Checkbox("Wireframe", &g_drawWireframe);
        ImGui::SameLine();
        ImGui::Checkbox("Solid Fill", &g_drawSolid);
        ImGui::Checkbox("Backface Culling", &g_backfaceCull);
        ImGui::SameLine();
        ImGui::Checkbox("Front Surface Wire Only", &g_frontSurfaceWireOnly);

        UiSectionLabel("Depth Bias");
        UiHelpText("Front-surface filtering. Raise slightly if flickering.");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##DepthBias", &g_depthBias, 0.02f, 0.0f, 10.0f, "%.2f");

        UiSectionLabel("Depth Cell Size");
        UiHelpText("larger is smoother/faster.");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragInt("##DepthCellSize", &g_depthCellSize, 1.0f, 1, 16);

        UiSectionLabel("Wire Thickness");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##WireThickness", &g_wireThickness, 0.05f, 0.1f, 10.0f, "%.2f");

        UiSectionLabel("Wire Color");
        ImGui::ColorEdit4("##WireColor", g_wireColor);
        UiSectionLabel("Material Color");
        ImGui::ColorEdit4("##MaterialColor", g_solidColor);

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
    ImGui::Text("RefOBJ v1.0.0");
    ImGui::Checkbox("Show RefOBJ control window", &g_showControlWindow);
    ImGui::Checkbox("Enable overlay", &g_overlayEnabled);
    ImGui::Checkbox("Show debug values", &g_showDebug);

    if (ImGui::Button("Save RefOBJ Config"))
    {
        SaveConfig();
        std::snprintf(g_lastStatus, sizeof(g_lastStatus), "Config saved.");
    }
}

void SaveConfig()
{
    FILE* f = nullptr;
    fopen_s(&f, CONFIG_FILE, "w");
    if (!f)
        return;

    fprintf(f, "overlay=%d\n", g_overlayEnabled ? 1 : 0);
    fprintf(f, "showWindow=%d\n", g_showControlWindow ? 1 : 0);
    fprintf(f, "showDebug=%d\n", g_showDebug ? 1 : 0);
    fprintf(f, "drawWire=%d\n", g_drawWireframe ? 1 : 0);
    fprintf(f, "drawSolid=%d\n", g_drawSolid ? 1 : 0);
    fprintf(f, "backfaceCull=%d\n", g_backfaceCull ? 1 : 0);
    fprintf(f, "placeAtAvatarOnLoad=%d\n", g_placeAtAvatarOnLoad ? 1 : 0);
    fprintf(f, "frontSurfaceWireOnly=%d\n", g_frontSurfaceWireOnly ? 1 : 0);
    fprintf(f, "depthBias=%.9f\n", g_depthBias);
    fprintf(f, "depthCellSize=%d\n", g_depthCellSize);
    fprintf(f, "objPath=%s\n", g_objPath);
    fprintf(f, "pos=%.9f,%.9f,%.9f\n", g_worldPos[0], g_worldPos[1], g_worldPos[2]);
    fprintf(f, "rot=%.9f,%.9f,%.9f\n", g_rotationDeg[0], g_rotationDeg[1], g_rotationDeg[2]);
    fprintf(f, "scale=%.9f\n", g_objScale);
    fprintf(f, "wireThickness=%.9f\n", g_wireThickness);
    fprintf(f, "wireColor=%.9f,%.9f,%.9f,%.9f\n", g_wireColor[0], g_wireColor[1], g_wireColor[2], g_wireColor[3]);
    fprintf(f, "solidColor=%.9f,%.9f,%.9f,%.9f\n", g_solidColor[0], g_solidColor[1], g_solidColor[2], g_solidColor[3]);

    fclose(f);
}

void LoadConfig()
{
    FILE* f = nullptr;
    fopen_s(&f, CONFIG_FILE, "r");
    if (!f)
        return;

    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        char key[128] = "";
        char value[896] = "";

        if (sscanf_s(line, "%127[^=]=%895[^\n]", key, (unsigned)_countof(key), value, (unsigned)_countof(value)) != 2)
            continue;

        if (strcmp(key, "overlay") == 0) g_overlayEnabled = atoi(value) == 1;
        else if (strcmp(key, "showWindow") == 0) g_showControlWindow = atoi(value) == 1;
        else if (strcmp(key, "showDebug") == 0) g_showDebug = atoi(value) == 1;
        else if (strcmp(key, "drawWire") == 0) g_drawWireframe = atoi(value) == 1;
        else if (strcmp(key, "drawSolid") == 0) g_drawSolid = atoi(value) == 1;
        else if (strcmp(key, "backfaceCull") == 0) g_backfaceCull = atoi(value) == 1;
        else if (strcmp(key, "placeAtAvatarOnLoad") == 0) g_placeAtAvatarOnLoad = atoi(value) == 1;
        else if (strcmp(key, "frontSurfaceWireOnly") == 0) g_frontSurfaceWireOnly = atoi(value) == 1;
        else if (strcmp(key, "depthBias") == 0) g_depthBias = (float)atof(value);
        else if (strcmp(key, "depthCellSize") == 0) g_depthCellSize = atoi(value);
        else if (strcmp(key, "objPath") == 0) strncpy_s(g_objPath, value, _TRUNCATE);
        else if (strcmp(key, "scale") == 0) g_objScale = (float)atof(value);
        else if (strcmp(key, "wireThickness") == 0) g_wireThickness = (float)atof(value);
        else if (strcmp(key, "pos") == 0) sscanf_s(value, "%f,%f,%f", &g_worldPos[0], &g_worldPos[1], &g_worldPos[2]);
        else if (strcmp(key, "rot") == 0) sscanf_s(value, "%f,%f,%f", &g_rotationDeg[0], &g_rotationDeg[1], &g_rotationDeg[2]);
        else if (strcmp(key, "wireColor") == 0) sscanf_s(value, "%f,%f,%f,%f", &g_wireColor[0], &g_wireColor[1], &g_wireColor[2], &g_wireColor[3]);
        else if (strcmp(key, "solidColor") == 0) sscanf_s(value, "%f,%f,%f,%f", &g_solidColor[0], &g_solidColor[1], &g_solidColor[2], &g_solidColor[3]);
    }

    fclose(f);
}

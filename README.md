# RefOBJ
RefOBJ is a lightweight Nexus addon for Guild Wars 2 that allows you to load and display low-poly 3D reference models directly inside the game. Designed primarily for homestead and decoration work, RefOBJ lets creators place visual reference objects in-world to aid with decoration placement, alignment, scale matching, layout planning, and scene construction.

---

## Features

- Load standard Wavefront `.obj` files
- Render reference models directly inside Guild Wars 2
- Live world-space positioning
- Scale and rotation controls
- Wireframe rendering
- Optional solid fill rendering
- Backface culling
- Front-surface wireframe mode
- Adjustable wire thickness
- Adjustable depth filtering
- Color customization
- Automatic placement at player location

---

## Reference Object Folder

RefOBJ automatically creates and uses a dedicated Reference Objects folder.

Place your OBJ files here:
```text
Guild Wars 2
└── addons
    └── ReferenceObjects
```

After adding new OBJ files:

1. Open RefOBJ
2. Click **Refresh OBJ List**
3. Select an object
4. Click **Load Selected OBJ**

---

## Supported File Types

Currently supported:
```text
.obj
```

Recommended:
- Low-poly models
- Game-ready meshes
- Simple topology
- Triangles and quads

Very high-poly models may impact performance.

---

## Performance Notes

For best performance:

- Use low-poly reference models
- Keep wire thickness modest
- Increase Depth Cell Size on large meshes
- Avoid extremely dense CAD or scanned meshes

---

## Third-Party Components
### Dear ImGui

RefOBJ uses Dear ImGui:
https://github.com/ocornut/imgui

The ImGui source is included as a Git submodule.

After cloning this repository:
```bash
git submodule update --init --recursive
```

---

## Disclaimer
RefOBJ is an unofficial Guild Wars 2 addon.
Guild Wars 2 is a registered trademark of ArenaNet, LLC.
This project is not affiliated with or endorsed by ArenaNet.


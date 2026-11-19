# Voxelforge Default Scene

## Scene Description
Mountain/forest scene with river, cabin, and vegetation based on the EpicMobile 3D Voxel Winter Set.

## Assets
- **Terrain**: landscape.vxw (baked from heightmap.png, 4.2M records)
- **EpicMobile Winter Set objects** (converted from FBX to .vxw):
  - EM_Old_Tree_Winter_01a.vxw — large old tree
  - EM_BLD_House_Winter_01d.vxw — winter cabin
  - EM_Plant_Bush_Winter_03e.vxw — large bush
  - EM_Plant_Bush_Ball_PX_Winter_02c.vxw — ball bush
  - EM_Plant_Roots_Winter_01b.vxw — root decorations
- **Original voxelforge layers**: bridge, dock, ferns, bushes, shore, etc.

## World Configuration
- `world.json`: 24 layers (19 enabled)
- `world_all.json`: Full manifest
- `heightmap.png`: 2048×2048 terrain heightmap

## Available Layers (disabled by default)
The following EpicMobile layers are available in world.json but disabled:
- EM_Old_Tree_Winter_01a.vxw
- EM_BLD_House_Winter_01d.vxw  
- EM_Plant_Bush_Winter_03e.vxw
- EM_Plant_Bush_Ball_PX_Winter_02c.vxw
- EM_Plant_Roots_Winter_01b.vxw

To enable, edit world.json and set `"enabled": true`.

## Source
Converted from [EpicMobile 3d Voxel Forest And Atolls - Winter Set](https://opengameart.org/content/free-epicmobile-3d-voxel-forest-and-atolls-winter-set) (CC0 license).

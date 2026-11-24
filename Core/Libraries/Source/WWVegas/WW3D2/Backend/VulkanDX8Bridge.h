/*
**	ZeroHourReloaded — Native Vulkan 1.3 Bridge for WW3D2 / DX8Wrapper
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

#pragma once

#include <d3d8.h>

/// Creates an in-process `IDirect3D8` implementation backed directly by Rust `zhr-vulkan` (Vulkan 1.3).
/// Zero dependency on `d3d8.dll`, `d3d9.dll`, or external translation layers.
IDirect3D8* ZhrCreateVulkanD3D8();

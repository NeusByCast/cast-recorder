#pragma once
// Single source of the app version (exe resources, CLI --help, CMake). Keep the installer fallback in sync.
// Scheme: 1.0, 1.1, 1.2, … (MAJOR.MINOR; PATCH stays 0).
#define CR_VERSION_MAJOR 1
#define CR_VERSION_MINOR 1
#define CR_VERSION_PATCH 0
#define CR_VERSION_STR "1.1"

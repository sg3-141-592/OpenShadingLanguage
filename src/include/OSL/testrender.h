// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#pragma once

#include <OSL/export.h>
#include <OSL/oslconfig.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

OSL_NAMESPACE_BEGIN

/// Description of a triangle mesh for the CPU testrender path tracer.
///
/// Positions, normals and texture coordinates are flat arrays with three
/// (positions/normals) or two (texture coordinates) components per vertex.
/// `triangles` contains three position indices per triangle.  The optional
/// `normalIndices` and `texcoordIndices` arrays may be empty, in which case
/// the position indices are used for those attributes.
struct TestRenderMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<int> triangles;
    std::vector<int> normalIndices;
    std::vector<int> texcoordIndices;
};

/// Camera description.  The field of view uses the conventional full-angle
/// convention (the same value MaterialX and the graph editor use).
struct TestRenderCamera {
    float eye[3]    = { 0.0f, 0.0f, 5.0f };
    float target[3] = { 0.0f, 0.0f, 0.0f };
    float up[3]     = { 0.0f, 1.0f, 0.0f };
    float fov       = 45.0f;
    int width       = 512;
    int height      = 512;
};

struct TestRenderSettings {
    int maxBounces = 16;
    int rrDepth    = 5;
    bool noJitter  = false;
    /// Number of render threads; 0 uses the process default.  Note that this
    /// configures the process-wide OpenImageIO thread pool.
    int threads = 0;
    std::string shaderSearchPath;
    std::string textureSearchPath;
    std::string stdoslPath;
    std::vector<std::string> includePaths;
    float screenColor[3] = { 0.5f, 0.5f, 0.5f };
};

/// Reusable CPU path-tracing session used by interactive previews.
///
/// The session owns an OSL ShadingSystem and a SimpleRaytracer.  It supports
/// incremental, per-sample rendering: call renderSamples() repeatedly with a
/// small sample count and copyFrame() after each pass.  The implementation is
/// intentionally free of any UI or OpenGL dependency.
class TestRenderSession {
public:
    /// Return false to stop rendering early.
    using ProgressCallback = std::function<bool(int samplesDone)>;

    TestRenderSession();
    ~TestRenderSession();

    TestRenderSession(const TestRenderSession&)            = delete;
    TestRenderSession& operator=(const TestRenderSession&) = delete;

    /// Initialize the shading system.  Must be called before any other method.
    /// Calling it again on an initialized session resets the session and
    /// discards the current scene, material and frame.
    bool initialize(const TestRenderSettings& settings, std::string& error);

    /// Upload a mesh and build the acceleration structure.  Once the session is
    /// prepared, the scene is fixed; create a new session to change geometry.
    bool setScene(const TestRenderMesh& mesh, std::string& error);

    /// Set or update the camera and render resolution.  This resets the
    /// progressive accumulation.
    bool setCamera(const TestRenderCamera& camera, std::string& error);

    /// Compile an OSL surface shader and use it as the scene material.  The
    /// shader source is expected to declare a shader with a closure color
    /// output; `outputName` identifies that output.
    bool setMaterialFromOsl(const std::string& oslSource,
                            const std::string& shaderName,
                            const std::string& outputName, std::string& error);

    /// Select the display mode: "combined", "albedo", "normal" or "uv".
    bool setRenderPass(const std::string& pass, std::string& error);

    /// Set an environment map used for lighting.  May be called before or
    /// after prepare().  Returns false if a non-empty filename does not exist.
    bool setEnvironmentMap(const std::string& filename, std::string& error);

    /// Set the environment light intensity multiplier (0 disables it).
    bool setLightIntensity(float intensity, std::string& error);

    /// Finish scene/material setup and prepare the ray tracer.  Must be called
    /// after setScene(), setCamera(), setMaterialFromOsl() and
    /// setRenderPass().  Subsequent material/camera changes are allowed.
    bool prepare(std::string& error);

    /// Render `sampleCount` additional samples per pixel.  Each sample range is
    /// accumulated into the current frame, so callers can display after every
    /// pass.  Progress is invoked after each pass and may cancel rendering.
    bool renderSamples(int sampleCount, std::string& error,
                       const ProgressCallback& progress = {});

    /// Request cancellation of the current renderSamples() call.  Safe to call
    /// from another thread.
    void cancel();

    /// Copy the most recently published RGB float frame.
    bool copyFrame(std::vector<float>& rgb, int& width, int& height) const;

    /// Number of samples per pixel accumulated in the current frame.
    int samplesDone() const;

    /// True after prepare() succeeds.
    bool isPrepared() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

OSL_NAMESPACE_END

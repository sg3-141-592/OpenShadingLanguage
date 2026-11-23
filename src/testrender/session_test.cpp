// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

// Smoke test for the reusable TestRenderSession API.

#include <OSL/testrender.h>

#include <cstdio>
#include <string>
#include <vector>

#ifndef TESTRENDER_STDOSL_PATH
#    error "TESTRENDER_STDOSL_PATH must be defined by the build"
#endif


namespace {

int
fail(const char* what, const std::string& error)
{
    std::fprintf(stderr, "%s: %s\n", what, error.c_str());
    return 1;
}

}  // namespace


int
main()
{
    OSL::TestRenderSession session;
    std::string error;
    OSL::TestRenderSettings settings;
    settings.stdoslPath = TESTRENDER_STDOSL_PATH;
    if (!session.initialize(settings, error))
        return fail("initialize", error);

    OSL::TestRenderMesh mesh;
    mesh.positions = { -1, -1, 0, 1, -1, 0, 0, 1, 0 };
    mesh.normals   = { 0, 0, 1, 0, 0, 1, 0, 0, 1 };
    mesh.texcoords = { 0, 0, 1, 0, 0.5f, 1 };

    // Out-of-range indices must be rejected without disturbing the session.
    mesh.triangles = { 0, 1, 3 };
    if (session.setScene(mesh, error))
        return fail("setScene with an out-of-range index should fail", error);

    mesh.triangles = { 0, 1, 2 };
    if (!session.setScene(mesh, error))
        return fail("setScene", error);

    OSL::TestRenderCamera camera;
    camera.width  = 32;
    camera.height = 32;
    if (!session.setCamera(camera, error))
        return fail("setCamera", error);

    static const char* shader = "shader session_test(color Cin = color(0.8))\n"
                                "{\n"
                                "    Ci = Cin * diffuse(N);\n"
                                "}\n";
    if (!session.setMaterialFromOsl(shader, "session_test", "Ci", error))
        return fail("setMaterialFromOsl", error);

    // A missing environment map must be reported, not silently ignored.
    if (session.setEnvironmentMap("testrender-does-not-exist.exr", error))
        return fail("setEnvironmentMap with a missing file should fail", error);

    if (!session.prepare(error))
        return fail("prepare", error);
    if (!session.renderSamples(1, error))
        return fail("renderSamples", error);

    std::vector<float> rgb;
    int width = 0, height = 0;
    if (!session.copyFrame(rgb, width, height))
        return fail("copyFrame", "no frame");
    if (width != camera.width || height != camera.height
        || rgb.size() != size_t(width) * size_t(height) * 3
        || session.samplesDone() != 1) {
        std::fprintf(stderr,
                     "unexpected frame %dx%d (%zu values), samples=%d\n", width,
                     height, rgb.size(), session.samplesDone());
        return 1;
    }

    return 0;
}

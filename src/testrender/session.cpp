// Copyright Contributors to the Open Shading Language project.
// SPDX-License-Identifier: BSD-3-Clause
// https://github.com/AcademySoftwareFoundation/OpenShadingLanguage

#include <OSL/testrender.h>

#include <OSL/oslcomp.h>
#include <OSL/oslexec.h>

#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebuf.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "raytracer.h"
#include "shading.h"
#include "simpleraytracer.h"

OSL_NAMESPACE_BEGIN

namespace {

class CompileErrorHandler final : public OIIO::ErrorHandler {
public:
    CompileErrorHandler() = default;

    void operator()(int errcode, const std::string& message) override
    {
        if (errcode & (EH_ERROR | EH_SEVERE))
            _messages += message + "\n";
    }

    std::string messages() const { return _messages; }

private:
    std::string _messages;
};

std::string
make_temp_directory()
{
    namespace fs = std::filesystem;
    static std::atomic<unsigned long long> counter { 0 };
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec)
        return std::string();

    for (int attempt = 0; attempt < 100; ++attempt) {
        const fs::path candidate = base
                                   / ("osl_testrender_" + std::to_string(now)
                                      + "_"
                                      + std::to_string(counter.fetch_add(1)));
        if (fs::create_directories(candidate, ec) && !ec)
            return candidate.string();
        ec.clear();
    }
    return std::string();
}

}  // namespace

class TestRenderSession::Impl {
public:
    ~Impl() { shutdown(); }

    bool initialize(const TestRenderSettings& settings, std::string& error)
    {
        // Re-initializing a session must not leave state from the previous
        // scene behind.
        shutdown();
        _materialID   = -1;
        _backgroundID = -1;
        _sceneSet     = false;
        _prepared     = false;
        _renderPass   = "combined";
        _envMapFilename.clear();
        _lightIntensity = 1.0f;
        _samplesDone    = 0;
        _cancel         = false;
        {
            std::lock_guard<std::mutex> lock(_frameMutex);
            _published.clear();
            _publishedWidth = _publishedHeight = 0;
        }

        _settings = settings;
        _tempDir  = make_temp_directory();
        if (_tempDir.empty()) {
            error = "could not create a temporary directory for OSL shaders";
            return false;
        }

        _renderer   = std::make_unique<SimpleRaytracer>(false);
        _shadingsys = std::make_unique<ShadingSystem>(_renderer.get(), nullptr,
                                                      &_renderer->errhandler());
        _renderer->shadingsys = _shadingsys.get();

        register_closures(_shadingsys.get());

        _shadingsys->attribute("debug", 0);
        _shadingsys->attribute("compile_report", 0);
        _shadingsys->attribute("optimize", 2);
        _shadingsys->attribute("llvm_optimize", 1);
        _shadingsys->attribute("profile", 0);
        _shadingsys->attribute("debug_nan", 0);
        _shadingsys->attribute("debug_uninit", 0);
        _shadingsys->attribute("userdata_isconnected", 0);
        _shadingsys->attribute("llvm_debugging_symbols", 0);
        _shadingsys->attribute("llvm_profiling_events", 0);

        std::string shaderPath = _tempDir;
        if (!_settings.shaderSearchPath.empty()) {
#ifdef _WIN32
            shaderPath += ';';
#else
            shaderPath += ':';
#endif
            shaderPath += _settings.shaderSearchPath;
        }
        _shadingsys->attribute("searchpath:shader", shaderPath);

        if (!_settings.textureSearchPath.empty()) {
            if (OIIO::TextureSystem* texturesys = _shadingsys->texturesys())
                texturesys->attribute("searchpath",
                                      _settings.textureSearchPath);
        }

        if (_settings.threads > 0)
            OIIO::attribute("threads", _settings.threads);

        _renderer->attribute("max_bounces", _settings.maxBounces);
        _renderer->attribute("rr_depth", _settings.rrDepth);
        _renderer->attribute("aa", 1);
        _renderer->attribute("no_jitter", _settings.noJitter ? 1 : 0);
        _renderer->attribute("show_albedo_scale", 0.0f);
        _renderer->attribute("show_globals", 0);
        return true;
    }

    void shutdown()
    {
        // The renderer references the shading system, so destroy it first.
        _renderer.reset();
        _shadingsys.reset();
        if (!_tempDir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(_tempDir, ec);
            _tempDir.clear();
        }
    }

    bool setScene(const TestRenderMesh& mesh, std::string& error)
    {
        if (!_renderer) {
            error = "initialize() must be called before setScene()";
            return false;
        }
        if (_prepared) {
            error = "setScene() cannot be called after prepare()";
            return false;
        }
        if (mesh.positions.empty() || mesh.triangles.empty()
            || mesh.positions.size() % 3 != 0
            || mesh.triangles.size() % 3 != 0) {
            error = "invalid testrender mesh";
            return false;
        }

        const size_t vertexCount = mesh.positions.size() / 3;
        const bool haveNormals   = !mesh.normals.empty()
                                 && mesh.normals.size() % 3 == 0;
        const size_t normalCount = haveNormals ? mesh.normals.size() / 3 : 0;
        const bool haveUVs       = !mesh.texcoords.empty()
                             && mesh.texcoords.size() % 2 == 0;
        const size_t uvCount = haveUVs ? mesh.texcoords.size() / 2 : 0;
        const bool haveNormalIndices
            = haveNormals && mesh.normalIndices.size() == mesh.triangles.size();
        const bool haveUVIndices
            = haveUVs && mesh.texcoordIndices.size() == mesh.triangles.size();
        auto in_range = [](const std::vector<int>& indices, size_t count) {
            for (int index : indices)
                if (index < 0 || size_t(index) >= count)
                    return false;
            return true;
        };
        if (!in_range(mesh.triangles, vertexCount)
            || (haveNormalIndices && !in_range(mesh.normalIndices, normalCount))
            || (haveUVIndices && !in_range(mesh.texcoordIndices, uvCount))) {
            error = "testrender mesh index out of range";
            return false;
        }

        Scene& scene = _renderer->scene;
        scene.clear();

        scene.verts.reserve(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i)
            scene.verts.emplace_back(mesh.positions[i * 3 + 0],
                                     mesh.positions[i * 3 + 1],
                                     mesh.positions[i * 3 + 2]);

        if (haveNormals) {
            scene.normals.reserve(normalCount);
            for (size_t i = 0; i < normalCount; ++i)
                scene.normals.emplace_back(mesh.normals[i * 3 + 0],
                                           mesh.normals[i * 3 + 1],
                                           mesh.normals[i * 3 + 2]);
        }

        if (haveUVs) {
            scene.uvs.reserve(uvCount);
            for (size_t i = 0; i < uvCount; ++i)
                scene.uvs.emplace_back(mesh.texcoords[i * 2 + 0],
                                       mesh.texcoords[i * 2 + 1]);
        }

        const size_t triangleCount = mesh.triangles.size() / 3;
        const bool normalPerVertex = haveNormals
                                     && scene.normals.size() == vertexCount;
        const bool uvPerVertex = haveUVs && scene.uvs.size() == vertexCount;

        scene.triangles.reserve(triangleCount);
        scene.n_triangles.reserve(triangleCount);
        scene.uv_triangles.reserve(triangleCount);
        scene.shaderids.assign(triangleCount,
                               _materialID >= 0 ? _materialID : 0);

        for (size_t t = 0; t < triangleCount; ++t) {
            const int i0 = mesh.triangles[t * 3 + 0];
            const int i1 = mesh.triangles[t * 3 + 1];
            const int i2 = mesh.triangles[t * 3 + 2];
            scene.triangles.emplace_back(TriangleIndices { i0, i1, i2 });

            if (haveNormalIndices) {
                scene.n_triangles.emplace_back(
                    TriangleIndices { mesh.normalIndices[t * 3 + 0],
                                      mesh.normalIndices[t * 3 + 1],
                                      mesh.normalIndices[t * 3 + 2] });
            } else if (normalPerVertex) {
                scene.n_triangles.emplace_back(TriangleIndices { i0, i1, i2 });
            } else {
                scene.n_triangles.emplace_back(TriangleIndices { -1, -1, -1 });
            }

            if (haveUVIndices) {
                scene.uv_triangles.emplace_back(
                    TriangleIndices { mesh.texcoordIndices[t * 3 + 0],
                                      mesh.texcoordIndices[t * 3 + 1],
                                      mesh.texcoordIndices[t * 3 + 2] });
            } else if (uvPerVertex) {
                scene.uv_triangles.emplace_back(TriangleIndices { i0, i1, i2 });
            } else {
                scene.uv_triangles.emplace_back(TriangleIndices { -1, -1, -1 });
            }
        }
        scene.last_index.emplace_back(scene.triangles.size());

        _sceneSet = true;
        return true;
    }

    bool setCamera(const TestRenderCamera& camera, std::string& error)
    {
        if (camera.width <= 0 || camera.height <= 0 || camera.fov <= 0.0f) {
            error = "invalid testrender camera";
            return false;
        }
        _camera = camera;
        if (_prepared) {
            applyCamera();
            resetAccumulation();
        }
        return true;
    }

    bool setMaterialFromOsl(const std::string& oslSource,
                            const std::string& shaderName,
                            const std::string& outputName, std::string& error)
    {
        if (oslSource.empty() || shaderName.empty()) {
            error = "empty OSL material";
            return false;
        }
        if (!_renderer || !_shadingsys) {
            error = "initialize() must be called before setMaterialFromOsl()";
            return false;
        }

        CompileErrorHandler compileErrors;
        OSLCompiler compiler(&compileErrors);
        std::vector<std::string> options;
        options.emplace_back("-q");
        for (const std::string& includePath : _settings.includePaths)
            options.emplace_back("-I" + includePath);

        std::string oso;
        if (!compiler.compile_buffer(oslSource, oso, options,
                                     _settings.stdoslPath,
                                     shaderName + ".osl")) {
            error = compileErrors.messages();
            if (error.empty())
                error = "OSL compilation failed";
            return false;
        }

        const std::string osoPath
            = (std::filesystem::path(_tempDir) / (shaderName + ".oso")).string();
        std::ofstream out(osoPath, std::ios::binary);
        if (!out) {
            error = "could not write " + osoPath;
            return false;
        }
        out.write(oso.data(), std::streamsize(oso.size()));
        out.close();

        // A generated MaterialX shader exposes its result as a closure color
        // output (for example "out").  Wrap it in a closure passthrough layer
        // so that the shader group's root output is the standard "Ci" that the
        // testrender shading path reads.
        static const char* passthroughSource
            = "surface osl_testrender_closure_passthrough(\n"
              "    closure color Cin = 0.0)\n"
              "{\n"
              "    Ci = Cin;\n"
              "}\n";
        const std::string passthroughName = "osl_testrender_closure_passthrough";
        const std::string passthroughOso = (std::filesystem::path(_tempDir)
                                            / (passthroughName + ".oso"))
                                               .string();
        if (!std::filesystem::exists(passthroughOso)) {
            CompileErrorHandler passErrors;
            OSLCompiler passCompiler(&passErrors);
            std::vector<std::string> passOptions { "-q" };
            for (const std::string& includePath : _settings.includePaths)
                passOptions.emplace_back("-I" + includePath);
            std::string passBuffer;
            if (!passCompiler.compile_buffer(passthroughSource, passBuffer,
                                             passOptions, _settings.stdoslPath,
                                             passthroughName + ".osl")) {
                error = passErrors.messages();
                if (error.empty())
                    error = "could not compile closure passthrough shader";
                return false;
            }
            std::ofstream passOut(passthroughOso, std::ios::binary);
            if (!passOut) {
                error = "could not write " + passthroughOso;
                return false;
            }
            passOut.write(passBuffer.data(),
                          std::streamsize(passBuffer.size()));
            passOut.close();
        }

        if (outputName.empty()) {
            error = "an OSL material output name is required";
            return false;
        }

        _renderer->clear_error();
        ShaderGroupRef group = _shadingsys->ShaderGroupBegin(shaderName);
        if (!group) {
            error = "could not begin OSL shader group";
            return false;
        }
        bool ok = _shadingsys->Shader(*group, "surface", shaderName,
                                      "material");
        ok = _shadingsys->Shader(*group, "surface", passthroughName, "output")
             && ok;
        ok = _shadingsys->ConnectShaders(*group, "material", outputName,
                                         "output", "Cin")
             && ok;
        ok = _shadingsys->ShaderGroupEnd(*group) && ok;
        if (!ok) {
            error = _renderer->last_error();
            if (error.empty())
                error = "OSL shader group compilation failed";
            return false;
        }

        if (_materialID < 0) {
            _materialID = _renderer->add_shader_group(group, false);
        } else {
            _renderer->shaders()[_materialID].surf = group;
        }

        // All geometry uses the single preview material.
        for (int& shaderID : _renderer->scene.shaderids)
            shaderID = _materialID;

        (void)outputName;  // The group root output is the generated closure.
        if (_prepared)
            resetAccumulation();
        return true;
    }

    bool setRenderPass(const std::string& pass, std::string& error)
    {
        if (pass == "combined") {
            _renderer->set_show_albedo_scale(0.0f);
            _renderer->set_show_globals(0);
        } else if (pass == "albedo") {
            _renderer->set_show_albedo_scale(1.0f);
            _renderer->set_show_globals(0);
        } else if (pass == "normal") {
            _renderer->set_show_albedo_scale(0.0f);
            _renderer->set_show_globals(1);
        } else if (pass == "uv") {
            _renderer->set_show_albedo_scale(0.0f);
            _renderer->set_show_globals(5);
        } else {
            error = "unsupported render pass: " + pass;
            return false;
        }
        _renderPass = pass;
        if (_prepared)
            resetAccumulation();
        return true;
    }

    bool setEnvironmentMap(const std::string& filename, std::string& error)
    {
        std::error_code ec;
        if (!filename.empty() && !std::filesystem::exists(filename, ec)) {
            error = "environment map not found: " + filename;
            return false;
        }
        _envMapFilename = filename;
        if (_prepared) {
            if (!buildBackground(error))
                return false;
            _renderer->prepare_background();
            resetAccumulation();
        }
        return true;
    }

    bool setLightIntensity(float intensity, std::string& error)
    {
        intensity = std::max(0.0f, intensity);
        if (intensity == _lightIntensity)
            return true;
        _lightIntensity = intensity;
        if (_prepared) {
            if (!buildBackground(error))
                return false;
            _renderer->prepare_background();
            resetAccumulation();
        }
        return true;
    }

    bool prepare(std::string& error)
    {
        if (_prepared)
            return true;
        if (!_sceneSet) {
            error = "prepare() called before setScene()";
            return false;
        }
        if (_materialID < 0) {
            error = "prepare() called before setMaterialFromOsl()";
            return false;
        }
        if (_camera.width <= 0 || _camera.height <= 0) {
            error = "prepare() called before setCamera()";
            return false;
        }
        if (!buildBackground(error))
            return false;

        applyCamera();
        _renderer->prepare_render();
        _renderer->pixelbuf.reset(
            OIIO::ImageSpec(_camera.width, _camera.height, 3, OIIO::TypeFloat));
        if (_renderer->had_error()) {
            error = _renderer->last_error();
            if (error.empty())
                error = "testrender failed to prepare the scene";
            return false;
        }
        _prepared = true;

        // prepare_render() reads the pass settings from the renderer's option
        // list, so restore the requested pass afterwards.
        std::string passError;
        setRenderPass(_renderPass, passError);
        resetAccumulation();
        return true;
    }

    bool renderSamples(int sampleCount, std::string& error,
                       const TestRenderSession::ProgressCallback& progress)
    {
        if (!_prepared) {
            error = "renderSamples() called before prepare()";
            return false;
        }
        if (sampleCount <= 0)
            return true;

        _cancel = false;
        _renderer->reset_cancel();
        for (int i = 0; i < sampleCount; ++i) {
            if (_cancel.load())
                break;

            _renderer->render_samples(_camera.width, _camera.height,
                                      _samplesDone.load(), 1);
            _samplesDone.fetch_add(1);
            publishFrame();

            if (progress && !progress(_samplesDone.load()))
                break;
        }

        if (_renderer->had_error()) {
            error = _renderer->last_error();
            if (error.empty())
                error = "testrender reported an error";
            return false;
        }
        return true;
    }

    void cancel()
    {
        _cancel = true;
        if (_renderer)
            _renderer->cancel_render();
    }

    bool copyFrame(std::vector<float>& rgb, int& width, int& height) const
    {
        std::lock_guard<std::mutex> lock(_frameMutex);
        rgb    = _published;
        width  = _publishedWidth;
        height = _publishedHeight;
        return !rgb.empty();
    }

    void publishFrame()
    {
        const OIIO::ImageSpec spec = _renderer->pixelbuf.spec();
        if (spec.width <= 0 || spec.height <= 0 || spec.nchannels < 3)
            return;

        std::vector<float> rgb(size_t(spec.width) * size_t(spec.height) * 3);
        OIIO::ImageBuf::ConstIterator<float> p(_renderer->pixelbuf);
        for (; !p.done(); ++p) {
            const size_t index
                = (size_t(p.y()) * size_t(spec.width) + size_t(p.x())) * 3;
            rgb[index + 0] = p[0];
            rgb[index + 1] = p[1];
            rgb[index + 2] = p[2];
        }

        std::lock_guard<std::mutex> lock(_frameMutex);
        _published       = std::move(rgb);
        _publishedWidth  = spec.width;
        _publishedHeight = spec.height;
    }

    void resetAccumulation()
    {
        if (!_renderer || !_renderer->pixelbuf.initialized())
            return;
        _renderer->reset_accumulation();
        _samplesDone = 0;
        publishFrame();
    }

    bool buildBackground(std::string& error)
    {
        std::error_code ec;
        const bool useEnv = !_envMapFilename.empty()
                            && std::filesystem::exists(_envMapFilename, ec);
        if (!_envMapFilename.empty() && !useEnv) {
            error = "environment map not found: " + _envMapFilename;
            return false;
        }
        const std::string name = useEnv ? "osl_testrender_envmap"
                                        : "osl_testrender_background";

        static const char* constantSource
            = "shader osl_testrender_background(color Cin = color(0.5), "
              "float intensity = 1.0)\n"
              "{\n"
              "    Ci = Cin * intensity * background();\n"
              "}\n";
        static const char* envSource
            = "shader osl_testrender_envmap(float Kb = 1.0, "
              "string envmap_filename = \"\", color Cin = color(0.5))\n"
              "{\n"
              "    if (iscameraray()) {\n"
              "        Ci = Cin * background();\n"
              "    } else {\n"
              "        color envColor = environment(envmap_filename, I);\n"
              "        Ci = Kb * envColor * background();\n"
              "    }\n"
              "}\n";

        const char* source = useEnv ? envSource : constantSource;
        CompileErrorHandler compileErrors;
        OSLCompiler compiler(&compileErrors);
        std::vector<std::string> options { "-q" };
        for (const std::string& includePath : _settings.includePaths)
            options.emplace_back("-I" + includePath);

        std::string oso;
        if (!compiler.compile_buffer(source, oso, options, _settings.stdoslPath,
                                     name + ".osl")) {
            error = compileErrors.messages();
            if (error.empty())
                error = "background OSL compilation failed";
            return false;
        }

        const std::string osoPath
            = (std::filesystem::path(_tempDir) / (name + ".oso")).string();
        std::ofstream out(osoPath, std::ios::binary);
        if (!out) {
            error = "could not write " + osoPath;
            return false;
        }
        out.write(oso.data(), std::streamsize(oso.size()));
        out.close();

        Color3 color(_settings.screenColor[0], _settings.screenColor[1],
                     _settings.screenColor[2]);
        Color3 scaled             = color * _lightIntensity;
        const float intensity     = 1.0f;
        const std::string envName = _envMapFilename;

        _renderer->clear_error();
        ShaderGroupRef group = _shadingsys->ShaderGroupBegin(name);
        if (!group) {
            error = "could not begin background shader group";
            return false;
        }
        bool ok = true;
        if (useEnv) {
            ok = _shadingsys->Parameter(*group, "Kb", TypeFloat,
                                        &_lightIntensity)
                 && ok;
            const char* envmap = envName.c_str();
            ok = _shadingsys->Parameter(*group, "envmap_filename", TypeString,
                                        &envmap)
                 && ok;
            ok = _shadingsys->Parameter(*group, "Cin", TypeColor, &color) && ok;
        } else {
            ok = _shadingsys->Parameter(*group, "Cin", TypeColor, &scaled)
                 && ok;
            ok = _shadingsys->Parameter(*group, "intensity", TypeFloat,
                                        &intensity)
                 && ok;
        }
        ok = _shadingsys->Shader(*group, "surface", name, "layer0") && ok;
        ok = _shadingsys->ShaderGroupEnd(*group) && ok;
        if (!ok) {
            error = _renderer->last_error();
            if (error.empty())
                error = "background shader group compilation failed";
            return false;
        }

        const int resolution = useEnv ? 256 : 0;
        if (_backgroundID < 0) {
            _backgroundID = _renderer->add_shader_group(group, false);
        } else {
            _renderer->shaders()[_backgroundID].surf = group;
        }
        _renderer->set_background_shader(_backgroundID, resolution);
        return true;
    }

    void applyCamera()
    {
        const float pi = std::acos(-1.0f);
        const float testrenderFov
            = 2.0f * std::atan(2.0f * std::tan(_camera.fov * pi / 360.0f))
              * 180.0f / pi;

        _renderer->camera.resolution(_camera.width, _camera.height);
        _renderer->camera.lookat(
            Vec3(_camera.eye[0], _camera.eye[1], _camera.eye[2]),
            Vec3(_camera.target[0] - _camera.eye[0],
                 _camera.target[1] - _camera.eye[1],
                 _camera.target[2] - _camera.eye[2]),
            Vec3(_camera.up[0], _camera.up[1], _camera.up[2]), testrenderFov);

        Matrix44 identity;
        identity.makeIdentity();
        _renderer->camera_params(identity, ustringhash("perspective"),
                                 testrenderFov, 0.1f, 1000.0f, _camera.width,
                                 _camera.height);
    }

    TestRenderSettings _settings;
    TestRenderCamera _camera;
    std::unique_ptr<SimpleRaytracer> _renderer;
    std::unique_ptr<ShadingSystem> _shadingsys;
    std::string _tempDir;
    std::string _renderPass = "combined";
    std::string _envMapFilename;
    int _materialID       = -1;
    int _backgroundID     = -1;
    bool _sceneSet        = false;
    bool _prepared        = false;
    float _lightIntensity = 1.0f;

    mutable std::mutex _frameMutex;
    std::vector<float> _published;
    int _publishedWidth  = 0;
    int _publishedHeight = 0;

    std::atomic<bool> _cancel { false };
    std::atomic<int> _samplesDone { 0 };
};


TestRenderSession::TestRenderSession() : _impl(std::make_unique<Impl>()) {}

TestRenderSession::~TestRenderSession() = default;

bool
TestRenderSession::initialize(const TestRenderSettings& settings,
                              std::string& error)
{
    return _impl->initialize(settings, error);
}

bool
TestRenderSession::setScene(const TestRenderMesh& mesh, std::string& error)
{
    return _impl->setScene(mesh, error);
}

bool
TestRenderSession::setCamera(const TestRenderCamera& camera, std::string& error)
{
    return _impl->setCamera(camera, error);
}

bool
TestRenderSession::setMaterialFromOsl(const std::string& oslSource,
                                      const std::string& shaderName,
                                      const std::string& outputName,
                                      std::string& error)
{
    return _impl->setMaterialFromOsl(oslSource, shaderName, outputName, error);
}

bool
TestRenderSession::setRenderPass(const std::string& pass, std::string& error)
{
    return _impl->setRenderPass(pass, error);
}

bool
TestRenderSession::setEnvironmentMap(const std::string& filename,
                                     std::string& error)
{
    return _impl->setEnvironmentMap(filename, error);
}

bool
TestRenderSession::setLightIntensity(float intensity, std::string& error)
{
    return _impl->setLightIntensity(intensity, error);
}

bool
TestRenderSession::prepare(std::string& error)
{
    return _impl->prepare(error);
}

bool
TestRenderSession::renderSamples(int sampleCount, std::string& error,
                                 const ProgressCallback& progress)
{
    return _impl->renderSamples(sampleCount, error, progress);
}

void
TestRenderSession::cancel()
{
    _impl->cancel();
}

bool
TestRenderSession::copyFrame(std::vector<float>& rgb, int& width,
                             int& height) const
{
    return _impl->copyFrame(rgb, width, height);
}

int
TestRenderSession::samplesDone() const
{
    return _impl->_samplesDone.load();
}

bool
TestRenderSession::isPrepared() const
{
    return _impl->_prepared;
}

OSL_NAMESPACE_END

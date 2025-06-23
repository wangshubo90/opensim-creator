#include <OpenSim/Simulation/Model/Model.h>
#include <libopensimcreator/Documents/Model/UndoableModelStatePair.h>
#include <libopensimcreator/Documents/Simulation/Simulation.h>
#include <libopensimcreator/Graphics/CachedModelRenderer.h>
#include <libopensimcreator/Graphics/CustomRenderingOptions.h>
#include <libopensimcreator/Graphics/ModelRendererParams.h>
#include <libopensimcreator/Graphics/OpenSimDecorationGenerator.h>
#include <libopensimcreator/Graphics/OpenSimGraphicsHelpers.h>
#include <libopensimcreator/Platform/OpenSimCreatorApp.h>
#include <libopensimcreator/Utils/OpenSimHelpers.h>
#include <liboscar/Formats/Image.h>  // for write_to_png
#include <liboscar/Graphics/ColorRenderBufferFormat.h>
#include <liboscar/Graphics/ColorRenderBufferParams.h>
#include <liboscar/Graphics/Graphics.h>
#include <liboscar/Graphics/RenderTexture.h>
#include <liboscar/Graphics/Scene/SceneCache.h>
#include <liboscar/Graphics/Scene/SceneHelpers.h>
#include <liboscar/Graphics/Scene/SceneRenderer.h>
#include <liboscar/Graphics/Scene/SceneRendererParams.h>
#include <liboscar/Graphics/SharedColorRenderBuffer.h>
#include <liboscar/Maths.h>
#include <liboscar/Maths/AABBFunctions.h>
#include <liboscar/Maths/Angle.h>
#include <liboscar/Maths/MathHelpers.h>
#include <liboscar/Maths/PolarPerspectiveCamera.h>
#include <liboscar/Platform/App.h>
#include <toml++/toml.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include "libopensimcreator/Documents/Simulation/SimulationModelStatePair.h"
#include "libopensimcreator/Documents/Simulation/SimulationReport.h"
#include "libopensimcreator/Documents/Simulation/StoFileSimulation.h"

using namespace osc;

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#endif

namespace {

template <typename T>
T get_or_default(const toml::table& tbl, const std::string& key, T default_val) {
    if (auto node = tbl.get(key)) {
        if (auto val = node->value<T>()) {
            return *val;
        }
    }
    return default_val;
}

std::string replace_extension(const std::string& path, const std::string& new_ext) {
    std::filesystem::path p(path);
    p.replace_extension(new_ext);
    return p.string();
}

osc::ModelRendererParams loadRenderParamsFromToml(const std::string& path) {
    ModelRendererParams params;
    auto tbl = toml::parse_file(path);

    // backgroundColor
    if (auto bg = tbl["backgroundColor"].as_array()) {
        params.backgroundColor = Color{
            static_cast<float>((*bg)[0].value_or(0.0)),
            static_cast<float>((*bg)[1].value_or(0.0)),
            static_cast<float>((*bg)[2].value_or(0.0)),
            static_cast<float>((*bg)[3].value_or(1.0))};
    }

    // lightColor
    if (auto lc = tbl["lightColor"].as_array()) {
        params.lightColor = Color{
            static_cast<float>((*lc)[0].value_or(1.0)),
            static_cast<float>((*lc)[1].value_or(1.0)),
            static_cast<float>((*lc)[2].value_or(1.0)),
            static_cast<float>((*lc)[3].value_or(1.0))};
    }

    // floorLocation
    if (auto fl = tbl["floorLocation"].as_array()) {
        params.floorLocation = Vec3{
            static_cast<float>((*fl)[0].value_or(0.0)),
            static_cast<float>((*fl)[1].value_or(0.0)),
            static_cast<float>((*fl)[2].value_or(0.0))};
    }

    // camera
    if (auto cam = tbl["camera"].as_table()) {
        params.camera.radius = get_or_default<float>(*cam, "camera", 1.0f);
        params.camera.theta = Degrees(get_or_default(*cam, "theta", 0.0f));
        params.camera.phi = Degrees(get_or_default(*cam, "phi", 0.0f));
        params.camera.vertical_field_of_view = Degrees(get_or_default(*cam, "vfov", 35.0f));
        params.camera.znear = get_or_default(*cam, "znear", 0.1f);
        params.camera.zfar = get_or_default(*cam, "zfar", 10.0f);
        if (auto f = cam->get_as<toml::array>("focus")) {
            params.camera.focus_point = Vec3{
                static_cast<float>((*f)[0].value_or(0.0)),
                static_cast<float>((*f)[1].value_or(0.0)),
                static_cast<float>((*f)[2].value_or(0.0))};
        }
    }

    // renderingOptions
    if (auto ro = tbl["renderingOptions"].as_table()) {
        params.renderingOptions.setDrawFloor(get_or_default(*ro, "drawFloor", true));
        // add more flags if needed
    }

    return params;
};
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: osc_render_tool <model.osim> <output.png> [<motion.sto>]\n";
        return 1;
    }

    const std::string osimPath = argv[1];
    const std::string outputImagePath = argv[2];
    const std::filesystem::path motionFilePath(argv[3]);

    // fixed render config
    constexpr int width = 1024;
    constexpr int height = 768;

    // 1. Init application and OpenSim environment
    App app;
    GloballyInitOpenSim();
    GloballyAddDirectoryToOpenSimGeometrySearchPath(App::resource_filepath("geometry").string());

    // 2. Load model and generate decorations
    // setup rendering state
    UndoableModelStatePair model{osimPath};
    auto meshCache = std::make_shared<SceneCache>(App::resource_loader());

    CachedModelRenderer renderer{meshCache};

    // 3. Set up camera
    ModelRendererParams renderParams;
    renderer.autoFocusCamera(model, renderParams, static_cast<float>(width) / height);
    renderParams.camera.radius = 1.219606f;
    renderParams.camera.theta = Degrees(180.0f);
    renderParams.camera.phi = Degrees(0.0f);
    renderParams.camera.vertical_field_of_view = Degrees(35.0f);
    renderParams.camera.znear = 0.121961f;
    renderParams.camera.zfar = 12.196062f;
    renderParams.camera.focus_point.x = -0.068828f;
    renderParams.camera.focus_point.y = -0.197732f;
    renderParams.camera.focus_point.z = -0.008137f;
    renderParams.backgroundColor = Color{0.1f, 0.1f, 0.1f, 1.0f};  // dark background
    renderParams.renderingOptions.setDrawFloor(false);
    // renderParams.decorationOptions
    // renderParams.camera.theta = renderParams.camera.theta + Radians(45);  // fixed 45 degree rotation

    // 4. Render scene
    RenderTexture& tex = renderer.onDraw(
        model,
        renderParams,
        {width, height},
        1.0f,
        app.anti_aliasing_level());

    // 5. Save to PNG
    Texture2D tex2D{tex.dimensions(), TextureFormat::RGB24, ColorSpace::sRGB};
    ;
    graphics::copy_texture(tex, tex2D);
    std::ofstream fout{outputImagePath, std::ios::binary};
    write_to_png(tex2D, fout);
    fout.close();

    std::cout << "Saved screenshot to " << outputImagePath << "\n";

    auto modelCopy = std::make_unique<OpenSim::Model>(model.getModel());
    InitializeModel(*modelCopy);
    InitializeState(*modelCopy);

    auto simulation = std::make_shared<Simulation>(
        StoFileSimulation{std::move(modelCopy),
                          motionFilePath,
                          model.getFixupScaleFactor(),
                          model.tryUpdEnvironment()});

    std::shared_ptr<SimulationModelStatePair> showModelState = std::make_shared<SimulationModelStatePair>();
    showModelState->setSimulation(simulation);

    const ptrdiff_t numSimulationReports = simulation->getNumReports();
    auto outputGifPath = replace_extension(outputImagePath, "mp4");
    // Open pipe to ffmpeg
    // std::string ffmpegCmd = "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size " +
    //                         std::to_string(width) + "x" + std::to_string(height) +
    //                         " -framerate 10 -i - -filter_complex \"split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse\" -loop -1 -f gif " +
    //                         outputGifPath;
    std::string ffmpegCmd = "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size " +
                            std::to_string(width) + "x" + std::to_string(height) +
                            " -framerate 10 -i pipe:0 -c:v libx264 -pix_fmt yuv420p -preset fast " +
                            outputGifPath;

    FILE* ffmpegPipe = popen(ffmpegCmd.c_str(), "w");
    if (!ffmpegPipe) {
        std::cerr << "Failed to open ffmpeg pipe\n";
        return 1;
    }

    std::vector<uint8_t> frameBuffer(static_cast<size_t>(width * height * 3));
    std::ofstream ofs(outputImagePath + "debug_frame.raw", std::ios::binary);

    for (ptrdiff_t i = 0; i < numSimulationReports; ++i) {
        const SimulationReport r = simulation->getSimulationReport(i);
        showModelState->setSimulationReport(r);

        RenderTexture& textureMot = renderer.onDraw(
            *showModelState,
            renderParams,
            {width, height},
            1.0f,
            app.anti_aliasing_level());

        Texture2D tex2DMot{textureMot.dimensions(), TextureFormat::RGB24, ColorSpace::sRGB};
        graphics::copy_texture(textureMot, tex2DMot);

        auto pixel_data = tex2DMot.pixel_data();
        assert(pixel_data.size() == frameBuffer.size());
        // std::memcpy(frameBuffer.data(), pixel_data.data(), frameBuffer.size());

        const int rowBytes = width * 3;
        for (int y = 0; y < height; ++y) {
            const uint8_t* src = pixel_data.data() + (height - 1 - y) * rowBytes;
            std::memcpy(frameBuffer.data() + y * rowBytes, src, rowBytes);
        }
        size_t bytes_written = fwrite(frameBuffer.data(), 1, frameBuffer.size(), ffmpegPipe);
        // size_t bytes_written = fwrite(tex2DMot.pixel_data().data(), 1, frameBuffer.size(), ffmpegPipe);

        ofs.write(reinterpret_cast<const char*>(frameBuffer.data()), frameBuffer.size());

        assert(tex2DMot.pixel_data().size() == frameBuffer.size());
        if (bytes_written != frameBuffer.size()) {
            std::cerr << "Error writing frame data to pipe. Bytes written: " << bytes_written << " Expected: " << frameBuffer.size() << std::endl;
            break;
        }
    }

    ofs.close();
    pclose(ffmpegPipe);

    std::cout << "Saved motion cap to " << outputGifPath << "\n";

    return 0;
}

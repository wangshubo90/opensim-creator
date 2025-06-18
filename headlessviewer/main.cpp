#include <cstdio>
#include <cstdlib>
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

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <algorithm>
#include <string>
#include "libopensimcreator/Documents/Simulation/SimulationModelStatePair.h"
#include "libopensimcreator/Documents/Simulation/SimulationReport.h"
#include "libopensimcreator/Documents/Simulation/StoFileSimulation.h"

using namespace osc;

std::string replace_extension(const std::string& path, const std::string& new_ext) {
    std::filesystem::path p(path);
    p.replace_extension(new_ext);
    return p.string();
}

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

    auto simulation = std::make_shared<Simulation>(
            StoFileSimulation{std::make_unique<OpenSim::Model>(model.getModel()),
            motionFilePath,
            model.getFixupScaleFactor(),
            model.tryUpdEnvironment()
        });

    std::shared_ptr<SimulationModelStatePair> showModelState = std::make_shared<SimulationModelStatePair>();
    showModelState->setSimulation(simulation);


    const ptrdiff_t numSimulationReports = simulation->getNumReports();
    // Open pipe to ffmpeg
    std::string ffmpegCmd = "ffmpeg -y -f rawvideo -pixel_format rgb24 -video_size " +
                            std::to_string(width) + "x" + std::to_string(height) +
                            " -framerate 10 -i - -filter_complex \"split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse\" -loop 0 -f gif " +
                            replace_extension(outputImagePath, "gif");
    FILE* ffmpegPipe = popen(ffmpegCmd.c_str(), "w");
    if (!ffmpegPipe) {
        std::cerr << "Failed to open ffmpeg pipe\n";
        return 1;
    }

    std::vector<uint8_t> frameBuffer(static_cast<size_t>(width * height * 3));

    for (ptrdiff_t i = 0; i < numSimulationReports; ++i) {
        const SimulationReport r = simulation->getSimulationReport(i);
        showModelState->setSimulationReport(r);

        RenderTexture& texture = renderer.onDraw(
            *showModelState,
            renderParams,
            {width, height},
            1.0f,
            app.anti_aliasing_level());

        Texture2D tex2D{texture.dimensions(), TextureFormat::RGB24, ColorSpace::sRGB};
        graphics::copy_texture(tex, tex2D);

        auto pixel_data = tex2D.pixel_data();
        std::memcpy(frameBuffer.data(), &pixel_data, frameBuffer.size());
        fwrite(frameBuffer.data(), 1, frameBuffer.size(), ffmpegPipe);
    }

    pclose(ffmpegPipe);

    return 0;
}

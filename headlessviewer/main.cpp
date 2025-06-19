#include <libopensimcreator/Documents/Model/UndoableModelStatePair.h>
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
#include <fstream>
#include <iostream>

using namespace osc;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: osc_render_tool <model.osim> <output.png>\n";
        return 1;
    }

    const std::string osimPath = argv[1];
    const std::string outputImagePath = argv[2];

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

    OpenSimDecorationOptions decorationOpts;
    decorationOpts.setShouldShowEverything(true);

    // generate decorations
    std::vector<SceneDecoration> decorations;
    GenerateModelDecorations(
        *meshCache,
        model.getModel(),
        model.getState(),
        decorationOpts,
        1.0f,  // 1:1 scaling
        [&decorations](const OpenSim::Component& component, SceneDecoration&& dec) {
            dec.id = GetAbsolutePathStringName(component);
            decorations.push_back(std::move(dec));
        });

    auto sceneCache = std::make_shared<SceneCache>(App::resource_loader());
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
    return 0;
}

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
#include <liboscar/Platform/Log.h>
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

namespace fs = std::filesystem;
namespace {
struct MockState final {
    std::optional<ResourcePath> last_open_call_path;
    std::optional<ResourcePath> last_existence_check_path;
};

class MockResourceLoader : public IResourceLoader {
   public:
    explicit MockResourceLoader(std::shared_ptr<MockState> state_) : state_{std::move(state_)} {}

   private:
    bool impl_resource_exists(const ResourcePath& resource_path) final {
        state_->last_existence_check_path = resource_path;
        return true;
    }

    ResourceStream impl_open(const ResourcePath& resource_path) override {
        state_->last_open_call_path = resource_path;
        return ResourceStream{};
    }

    std::function<std::optional<ResourceDirectoryEntry>()> impl_iterate_directory(const ResourcePath&) override {
        return [] { return std::nullopt; };
    }

    std::shared_ptr<MockState> state_;
};

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
    fs::path p(path);
    p.replace_extension(new_ext);
    return p.string();
}

void updateRenderParamsFromToml(const toml::table tbl, ModelRendererParams& params) {
    // backgroundColor
    if (auto bg = tbl["background_color"].as_array()) {
        params.backgroundColor = Color{
            static_cast<float>((*bg)[0].value_or(0.0)),
            static_cast<float>((*bg)[1].value_or(0.0)),
            static_cast<float>((*bg)[2].value_or(0.0)),
            static_cast<float>((*bg)[3].value_or(1.0))};
    }

    // lightColor
    if (auto lc = tbl["light_color"].as_array()) {
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
        params.camera.radius = get_or_default<float>(*cam, "radius", 1.0f);
        params.camera.theta = Degrees(get_or_default(*cam, "theta", 180.0f));
        params.camera.phi = Degrees(get_or_default(*cam, "phi", 0.0f));
        params.camera.vertical_field_of_view = Degrees(get_or_default(*cam, "vfov", 35.0f));
        params.camera.znear = get_or_default(*cam, "znear", 0.1f);
        params.camera.zfar = get_or_default(*cam, "zfar", 10.0f);
        if (auto f = cam->get_as<toml::array>("focus_point")) {
            params.camera.focus_point = Vec3{
                static_cast<float>((*f)[0].value_or(0.0)),
                static_cast<float>((*f)[1].value_or(0.0)),
                static_cast<float>((*f)[2].value_or(0.0))};
        }
    }

    // renderingOptions
    if (auto ro = tbl["renderer"].as_table()) {
        params.renderingOptions.setDrawFloor(get_or_default(*ro, "draw_floor", false));
        params.renderingOptions.setDrawShadows(get_or_default(*ro, "draw_shadows", true));
    }

    // decorationOptions
    if (auto deco = tbl["decoration"].as_table()) {
        params.overlayOptions.setDrawXYGrid(get_or_default(*deco, "xy_grid", false));
        params.overlayOptions.setDrawXZGrid(get_or_default(*deco, "xz_grid", false));
        params.overlayOptions.setDrawYZGrid(get_or_default(*deco, "yz_grid", false));
        params.overlayOptions.setDrawAxisLines(get_or_default(*deco, "axis_lines", false));
    }
};
}  // namespace

bool is_executable(const fs::path& p) {
    if (fs::exists(p) && fs::is_regular_file(p)) {
        // On POSIX, check executable permission
#ifndef _WIN32
        auto perms = fs::status(p).permissions();
        return (perms & fs::perms::owner_exec) != fs::perms::none ||
               (perms & fs::perms::group_exec) != fs::perms::none ||
               (perms & fs::perms::others_exec) != fs::perms::none;
#else
        // On Windows, simply existing as a file is usually enough,
        // though you might try to actually run it with a silent command.
        // For simplicity, we'll assume existence is sufficient.
        return true;
#endif
    }
    return false;
}

fs::path find_ffmpeg_executable() {
// 1. Check common default/expected locations
#ifdef _WIN32
    std::vector<fs::path> common_paths = {
        "C:/Program Files/ffmpeg/bin/ffmpeg.exe",
        "C:/ffmpeg/bin/ffmpeg.exe",
        "D:/ffmpeg/bin/ffmpeg.exe"  // Or other drives
    };
#else  // Linux/macOS
    std::vector<fs::path> common_paths = {
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
        "/bin/ffmpeg",
        "/opt/ffmpeg/bin/ffmpeg"};
#endif

    for (const auto& p : common_paths) {
        if (is_executable(p)) {
            std::cout << "Found ffmpeg in common path: " << p.string() << std::endl;
            return p;
        }
    }

    // 2. Search PATH environment variable
    std::string path_env;
#ifdef _WIN32
    // Windows requires a different way to get env vars with potentially large buffers
    DWORD bufferSize = GetEnvironmentVariableA("PATH", NULL, 0);
    if (bufferSize > 0) {
        std::vector<char> buffer(bufferSize);
        GetEnvironmentVariableA("PATH", buffer.data(), bufferSize);
        path_env = std::string(buffer.data());
    }
#else
    const char* path_cstr = getenv("PATH");
    if (path_cstr) {
        path_env = path_cstr;
    }
#endif

    if (!path_env.empty()) {
        const char path_sep =
#ifdef _WIN32
            ';';
#else
            ':';
#endif
        std::string current_path;
        std::stringstream ss(path_env);

        while (std::getline(ss, current_path, path_sep)) {
            fs::path potential_path = fs::path(current_path) /
#ifdef _WIN32
                                      "ffmpeg.exe";
#else
                                      "ffmpeg";
#endif
            if (is_executable(potential_path)) {
                std::cout << "Found ffmpeg in PATH: " << potential_path.string() << std::endl;
                return potential_path;
            }
        }
    }

    return {};  // Return empty path if not found
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: osc_render_tool <model.osim> <output.png> [<motion.sto>]\n"
                  << "Options:\n"
                  << "  --ffmpeg-path <path>   Specify custom ffmpeg executable path\n"
                  << "  --config <path>        Load custom render parameters from a config file\n";
        return 1;
    }

    fs::path ffmpeg_path;
    std::string user_provided_path;
    toml::table renderParamsTable;

    // Example: Parse command-line arguments for --ffmpeg-path
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--ffmpeg-path" && i + 1 < argc) {
            user_provided_path = argv[++i];
            log_info("User provided ffmpeg path: %s", user_provided_path.c_str());
        } else if (arg == "--config" && i + 1 < argc) {
            // Load custom render parameters from a config file
            std::string configPath = argv[++i];
            if (fs::exists(configPath)) {
                renderParamsTable = toml::parse_file(configPath);
                // Use `renderParams` as needed
                log_info("Loaded render parameters from config file: %s", configPath.c_str());
            } else {
                std::cerr << "Config file not found: " << configPath << "\n";
                return 1;
            }
        }
    }

    const std::string osimPath = argv[1];
    const std::string outputImagePath = argv[2];
    fs::path motionFilePath;

    if (argc > 3) {
        motionFilePath = argv[3];
    } else {
        motionFilePath = "";  // No motion file provided
    }

    // fixed render config
    int width = get_or_default<int>(renderParamsTable, "width", 1024);
    int height = get_or_default<int>(renderParamsTable, "height", 768);

    // 1. Init application and OpenSim environment
    App app;
    GloballyInitOpenSim();
    GloballyAddDirectoryToOpenSimGeometrySearchPath(App::resource_filepath("geometry").string());

    // 2. Load model and generate decorations & setup rendering state
    UndoableModelStatePair model{osimPath};

    // const auto mock_state = std::make_shared<MockState>();
    // const ResourcePath resource_path{"some/path"};
    // ResourceLoader resource_loader = make_resource_loader<MockResourceLoader>(mock_state);
    // auto meshCache = std::make_shared<SceneCache>(resource_loader);

    auto meshCache = std::make_shared<SceneCache>(App::resource_loader());
    CachedModelRenderer renderer{meshCache};

    // 3. Set up camera
    ModelRendererParams renderParams;
    renderer.autoFocusCamera(model, renderParams, static_cast<float>(width) / height);
    if (!renderParamsTable.empty()) {
        updateRenderParamsFromToml(renderParamsTable, renderParams);
    }

    // 4. Render scene
    RenderTexture& tex = renderer.onDraw(
        model,
        renderParams,
        {width, height},
        1.0f,
        // app.anti_aliasing_level());
        AntiAliasingLevel{1});  // Use no anti-aliasing for headless rendering

    // 5. Save to PNG
    Texture2D tex2D{tex.dimensions(), TextureFormat::RGB24, ColorSpace::sRGB};
    ;
    graphics::copy_texture(tex, tex2D);
    std::ofstream fout{outputImagePath, std::ios::binary};
    write_to_png(tex2D, fout);
    fout.close();

    log_info("Saved screenshot to %s", outputImagePath.c_str());

    // 6. If motion file is provided, render it to a video
    if (motionFilePath.empty() || !fs::exists(motionFilePath)) {
        log_info("No motion file provided or file does not exist. Exiting.");
        return 0;  // No motion file to process
    }

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

    if (!user_provided_path.empty()) {
        fs::path p(user_provided_path);
        if (is_executable(p)) {
            ffmpeg_path = p;
            log_info("Using user-specified ffmpeg path: %s", ffmpeg_path.c_str());
        } else {
            log_error("Provided ffmpeg path is not a valid executable: %s", p.c_str());
            return 1;
        }
    } else {
        ffmpeg_path = find_ffmpeg_executable();
        log_info("Found ffmpeg at: %s", ffmpeg_path.c_str());
    }

    if (ffmpeg_path.empty()) {
        log_error("ffmpeg executable not found. Please install ffmpeg or specify its path with --ffmpeg-path.");
        return 1;  // Return an error code
    }

    // Open pipe to ffmpeg
    // std::string ffmpegCmd = ffmpeg_path.string() + " -y -f rawvideo -pixel_format rgb24 -video_size " +
    //                         std::to_string(width) + "x" + std::to_string(height) +
    //                         " -framerate " + std::to_string(fps) + " -i - -filter_complex \"split[s0][s1];[s0]palettegen[p];[s1][p]paletteuse\" -loop -1 -f gif " +
    //                         outputGifPath;
    auto fps = get_or_default<float>(renderParamsTable, "fps", 10.0f);
    auto outputGifPath = replace_extension(outputImagePath, "mp4");
    std::string ffmpegCmd = ffmpeg_path.string() + " -y -f rawvideo -pixel_format rgb24 -video_size " +
                            std::to_string(width) + "x" + std::to_string(height) +
                            " -framerate " + std::to_string(fps) + " -i pipe:0 -c:v libx264 -pix_fmt yuv420p -preset fast " +
                            outputGifPath;

#ifdef _WIN32
    FILE* ffmpegPipe = _popen(ffmpegCmd.c_str(), "wb");
#else
    FILE* ffmpegPipe = popen(ffmpegCmd.c_str(), "w");  // Use "w" for writing to stdin
#endif

    if (!ffmpegPipe) {
        std::cerr << "Failed to open ffmpeg pipe\n";
        return 1;
    } else {
        std::cout << "Opened ffmpeg pipe for writing with " << ffmpegCmd << "\n";
    }

    std::vector<uint8_t> frameBuffer(static_cast<size_t>(width * height * 3));

    for (ptrdiff_t i = 0; i < numSimulationReports; ++i) {
        const SimulationReport r = simulation->getSimulationReport(i);
        showModelState->setSimulationReport(r);

        RenderTexture& textureMot = renderer.onDraw(
            *showModelState,
            renderParams,
            {width, height},
            1.0f,
            // app.anti_aliasing_level());
            AntiAliasingLevel{1});  // Use no anti-aliasing for headless rendering

        Texture2D tex2DMot{textureMot.dimensions(), TextureFormat::RGB24, ColorSpace::sRGB};
        graphics::copy_texture(textureMot, tex2DMot);

        auto pixel_data = tex2DMot.pixel_data();

        const int rowBytes = width * 3;
        for (int y = 0; y < height; ++y) {
            const uint8_t* src = pixel_data.data() + (height - 1 - y) * rowBytes;
            std::memcpy(frameBuffer.data() + y * rowBytes, src, rowBytes);
        }

        size_t bytes_written = fwrite(frameBuffer.data(), 1, frameBuffer.size(), ffmpegPipe);

        if (bytes_written != frameBuffer.size()) {
            std::cerr << "Error writing frame data to pipe. Bytes written: " << bytes_written << " Expected: " << frameBuffer.size() << std::endl;
            return 1;  // Return an error code
        }
    }

    pclose(ffmpegPipe);

    std::cout << "Saved motion cap to " << outputGifPath << "\n";

    return 0;
}

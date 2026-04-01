#include <yetty/shader-manager.hpp>
#include <ytrace/ytrace.hpp>

#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <map>
#include <filesystem>

namespace yetty {

// Placeholder markers in the base shader
static const char* BINDINGS_PLACEHOLDER = "// RENDER_LAYER_BINDINGS_PLACEHOLDER";
static const char* FUNCTIONS_PLACEHOLDER = "// SHADER_GLYPH_FUNCTIONS_PLACEHOLDER";
static const char* PRE_EFFECT_FUNCTIONS_PLACEHOLDER = "// PRE_EFFECT_FUNCTIONS_PLACEHOLDER";
static const char* PRE_EFFECT_APPLY_PLACEHOLDER = "// PRE_EFFECT_APPLY_PLACEHOLDER";
static const char* EFFECT_FUNCTIONS_PLACEHOLDER = "// EFFECT_FUNCTIONS_PLACEHOLDER";
static const char* EFFECT_APPLY_PLACEHOLDER = "// EFFECT_APPLY_PLACEHOLDER";
static const char* POST_EFFECT_FUNCTIONS_PLACEHOLDER = "// POST_EFFECT_FUNCTIONS_PLACEHOLDER";
static const char* POST_EFFECT_APPLY_PLACEHOLDER = "// POST_EFFECT_APPLY_PLACEHOLDER";

class ShaderManagerImpl : public ShaderManager {
public:
    ShaderManagerImpl() = default;
    ~ShaderManagerImpl() override = default;

    Result<void> init(const std::string& shadersDir) noexcept;

    void addProvider(std::shared_ptr<ShaderProvider> provider, const std::string& dispatchName) override;
    void addLibrary(const std::string& name, const std::string& code) override;

    void setBindingCode(const std::string& wgslCode) override {
        _bindingCode = wgslCode;
    }

    bool needsRemerge() const override;
    Result<std::string> merge() override;
    const std::string& getMergedSource() const override { return _mergedSource; }

private:
    Result<void> loadBaseShader(const std::string& path);
    std::string mergeShaders() const;

    std::string _shadersDir;
    std::string _baseShader;
    struct ProviderEntry {
        std::shared_ptr<ShaderProvider> provider;
        std::string dispatchName;
    };
    std::vector<ProviderEntry> _providers;
    std::map<std::string, std::string> _libraries;
    std::string _mergedSource;
    std::string _bindingCode;

    struct EffectFile {
        uint32_t index;
        std::string name;
        std::string functionName;
        std::string code;
    };
    std::vector<EffectFile> _preEffects;
    std::vector<EffectFile> _effects;
    std::vector<EffectFile> _postEffects;

};

// Factory implementation
Result<ShaderManager*> ShaderManager::createImpl(const std::string& shadersDir) noexcept {
    ShaderManagerImpl* shaderManager = new ShaderManagerImpl();
    if (auto result = shaderManager->init(shadersDir); !result) {
        yerror("ShaderManager creation failed: {}", error_msg(result));
        delete shaderManager;
        return Err<ShaderManager*>("ShaderManager init failed", result);
    }
    return Ok(shaderManager);
}

Result<void> ShaderManagerImpl::init(const std::string& shadersDir) noexcept {
    _shadersDir = shadersDir;

    // Load base shader (raster font only for now)
    std::string shaderPath = _shadersDir + "/terminal-screen-raster-font.wgsl";
    ydebug("ShaderManager: loading shaders from {}", _shadersDir);
    if (auto res = loadBaseShader(shaderPath); !res) {
        return res;
    }

    // Load all shader libraries from lib directory
    std::string libDir = _shadersDir + "/lib";
    if (std::filesystem::exists(libDir) && std::filesystem::is_directory(libDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(libDir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".wgsl") {
                std::ifstream libFile(entry.path());
                if (libFile.is_open()) {
                    std::stringstream buffer;
                    buffer << libFile.rdbuf();
                    std::string libCode = buffer.str();
                    if (!libCode.empty()) {
                        std::string libName = entry.path().stem().string();
                        addLibrary(libName, libCode);
                        ydebug("ShaderManager: loaded library '{}' ({} bytes)", libName, libCode.size());
                    }
                }
            }
        }
    } else {
        ywarn("ShaderManager: lib directory not found at {}", libDir);
    }

    // Load pre-effect and post-effect shaders with optional validation
    // Validation is enabled when WEBGPU_BACKEND_DAWN is defined (compile-time check)
    auto loadEffects = [this](const std::string& dir, std::vector<EffectFile>& dest, const std::string& prefix) {
        if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
            ydebug("ShaderManager: no {} directory at {}", prefix, dir);
            return;
        }
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".wgsl") continue;
            std::ifstream f(entry.path());
            if (!f.is_open()) continue;
            std::stringstream buf;
            buf << f.rdbuf();
            std::string code = buf.str();
            if (code.empty()) continue;

            // Parse "// Index: N" from file header
            uint32_t idx = 0;
            auto idxPos = code.find("// Index: ");
            if (idxPos != std::string::npos) {
                try { idx = std::stoul(code.substr(idxPos + 10, 10)); } catch (...) {
                    ywarn("ShaderManager: failed to parse index from file header in {}", entry.path().string());
                }
            }
            if (idx == 0) {
                // Try to parse from filename: "N-name.wgsl"
                std::string stem = entry.path().stem().string();
                auto dash = stem.find('-');
                if (dash != std::string::npos) {
                    try { idx = std::stoul(stem.substr(0, dash)); } catch (...) {
                        ywarn("ShaderManager: failed to parse index from filename {}", entry.path().string());
                    }
                }
            }
            if (idx == 0) {
                ywarn("ShaderManager: skipping effect {} (no valid index)", entry.path().string());
                continue;
            }

            // Extract function name: find "fn <prefix>Effect_" (or "fn effect_" for empty prefix)
            std::string funcName;
            std::string searchPattern = prefix.empty() ? "fn effect_" : ("fn " + prefix + "Effect_");
            auto fnPos = code.find(searchPattern);
            if (fnPos != std::string::npos) {
                auto nameStart = fnPos + 3;
                auto paren = code.find('(', nameStart);
                if (paren != std::string::npos) {
                    funcName = code.substr(nameStart, paren - nameStart);
                }
            }

            std::string name = entry.path().stem().string();
            std::string filename = entry.path().filename().string();

            // TODO: Per-file WGSL validation is currently disabled.
            //
            // The intention was to validate each effect file individually before merging,
            // so that syntax errors would show file-specific error messages instead of
            // cryptic line numbers in the merged shader (e.g., ":932:9 error").
            //
            // However, the current implementation validates files in isolation with
            // incomplete stubs. Effects that use library functions (e.g., renderGlyphInCell
            // from lib/text.wgsl) fail validation even though they work fine in the
            // merged shader where all libraries are available.
            //
            // To fix: build validation stubs dynamically from the already-loaded _libraries
            // map, so all library functions are available during per-file validation.
            //
            // See: validateShaderFile() and VALIDATION_STUB_PREAMBLE above.
#if 0  // Disabled - needs complete library stubs
            std::string validationError;
            if (!validateShaderFile(_yettyGpuContext.device, code, filename, validationError)) {
                yerror("ShaderManager: WGSL validation FAILED for {}", filename);
                yerror("  Error: {}", validationError);
                std::istringstream iss(code);
                std::string line;
                int lineNum = 1;
                yerror("  --- Source code ---");
                while (std::getline(iss, line)) {
                    yerror("  {:3d}: {}", lineNum++, line);
                }
                yerror("  --- End of source ---");
                ywarn("ShaderManager: skipping invalid effect {}", name);
                continue;
            }
            ydebug("ShaderManager: validated {} effect '{}'", prefix.empty() ? "coord" : prefix, name);
#endif

            dest.push_back({idx, name, funcName, code});
            ydebug("ShaderManager: loaded {} effect '{}' index={} func={}", prefix, name, idx, funcName);
        }
        // Sort by index
        std::sort(dest.begin(), dest.end(), [](const EffectFile& a, const EffectFile& b) {
            return a.index < b.index;
        });
    };

    std::string preEffectsDir = _shadersDir + "/pre-effects";
    std::string effectsDir = _shadersDir + "/effects";
    std::string postEffectsDir = _shadersDir + "/post-effects";
    loadEffects(preEffectsDir, _preEffects, "pre");
    loadEffects(effectsDir, _effects, "");  // no prefix for coord effects
    loadEffects(postEffectsDir, _postEffects, "post");

    ydebug("ShaderManager: initialized ({} pre-effects, {} effects, {} post-effects)",
           _preEffects.size(), _effects.size(), _postEffects.size());
    return Ok();
}

void ShaderManagerImpl::addProvider(std::shared_ptr<ShaderProvider> provider, const std::string& dispatchName) {
    if (provider) {
        _providers.push_back({std::move(provider), dispatchName});
        ydebug("ShaderManager: provider registered for '{}' ({} total)", dispatchName, _providers.size());
    }
}

void ShaderManagerImpl::addLibrary(const std::string& name, const std::string& code) {
    _libraries[name] = code;
    ydebug("ShaderManager: added library '{}'", name);
}

Result<void> ShaderManagerImpl::loadBaseShader(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Err<void>("Failed to open shader file: " + path);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    _baseShader = buffer.str();

    if (_baseShader.empty()) {
        return Err<void>("Shader file is empty: " + path);
    }

    ydebug("ShaderManager: loaded base shader from {} ({} lines)",
          path, std::count(_baseShader.begin(), _baseShader.end(), '\n') + 1);

    return Ok();
}

bool ShaderManagerImpl::needsRemerge() const {
    for (const auto& providerEntry : _providers) {
        if (providerEntry.provider && providerEntry.provider->isDirty()) {
            return true;
        }
    }
    return false;
}

std::string ShaderManagerImpl::mergeShaders() const {
    std::string result = _baseShader;

    // Collect all function code
    std::string allFunctions;
    allFunctions.reserve(64 * 1024);

    // Add library code first
    for (const auto& [name, code] : _libraries) {
        allFunctions += "// Library: " + name + "\n";
        allFunctions += code;
        allFunctions += "\n\n";
    }

    // Add provider function code
    for (const auto& entry : _providers) {
        if (entry.provider) {
            allFunctions += entry.provider->getCode();
        }
    }

    // Collect dispatch code grouped by dispatch name
    std::map<std::string, std::string> dispatchByName;

    for (const auto& entry : _providers) {
        if (entry.provider) {
            std::string dispatch = entry.provider->getDispatchCode();
            if (!dispatch.empty()) {
                auto& target = dispatchByName[entry.dispatchName];
                if (!target.empty()) {
                    target += " else ";
                }
                target += dispatch;
            }
        }
    }

    // Replace placeholders
    auto replacePlaceholder = [](std::string& str, const std::string& placeholder,
                                  const std::string& replacement) {
        size_t pos = str.find(placeholder);
        if (pos != std::string::npos) {
            str.replace(pos, placeholder.length(), replacement);
            return true;
        }
        return false;
    };

    // Replace render layer bindings placeholder with code from GpuResourceBinder
    {
        if (_bindingCode.empty()) {
            ywarn("ShaderManager: no render layer bindings set");
        }
        if (!replacePlaceholder(result, BINDINGS_PLACEHOLDER, _bindingCode)) {
            ywarn("ShaderManager: RENDER_LAYER_BINDINGS_PLACEHOLDER not found in base shader");
        }
    }

    if (!replacePlaceholder(result, FUNCTIONS_PLACEHOLDER, allFunctions)) {
        ywarn("ShaderManager: functions placeholder not found in base shader");
    }

    // Replace dispatch placeholders for each dispatch name
    for (const auto& [dispatchName, dispatchCode] : dispatchByName) {
        std::string placeholder = "// " + dispatchName;
        ydebug("ShaderManager: dispatch '{}' code length={}", dispatchName, dispatchCode.size());
        if (dispatchCode.size() > 200) {
            ydebug("  dispatch start: '{}'", dispatchCode.substr(0, 200));
        } else {
            ydebug("  dispatch: '{}'", dispatchCode);
        }
        if (!replacePlaceholder(result, placeholder, dispatchCode)) {
            ywarn("ShaderManager: dispatch placeholder '{}' not found in base shader", placeholder);
        }
    }

    // --- Effect placeholders ---

    // Pre-effect functions
    std::string preEffectFunctions;
    for (const auto& ef : _preEffects) {
        preEffectFunctions += "// Pre-effect: " + ef.name + "\n";
        preEffectFunctions += ef.code + "\n\n";
    }
    if (!replacePlaceholder(result, PRE_EFFECT_FUNCTIONS_PLACEHOLDER, preEffectFunctions)) {
        ywarn("ShaderManager: pre-effect functions placeholder not found");
    }

    // Pre-effect apply dispatch
    // Pre-effects modify glyphIndex: glyphIndex = preEffect_xxx(glyphIndex, cellCol, cellRow, time, params)
    std::string preEffectApply;
    if (!_preEffects.empty()) {
        preEffectApply = "if (grid.preEffectIndex != 0u) {\n";
        for (size_t i = 0; i < _preEffects.size(); i++) {
            const auto& ef = _preEffects[i];
            if (ef.functionName.empty()) continue;
            if (i > 0) preEffectApply += " else ";
            preEffectApply += "    if (grid.preEffectIndex == " + std::to_string(ef.index) + "u) {\n";
            preEffectApply += "        glyphIndex = " + ef.functionName + "(glyphIndex, cellCol, cellRow, globals.time, array<f32, 6>(grid.preEffectP0, grid.preEffectP1, grid.preEffectP2, grid.preEffectP3, grid.preEffectP4, grid.preEffectP5));\n";
            preEffectApply += "    }";
        }
        preEffectApply += "\n    }\n";
    }
    if (!replacePlaceholder(result, PRE_EFFECT_APPLY_PLACEHOLDER, preEffectApply)) {
        ywarn("ShaderManager: pre-effect apply placeholder not found");
    }

    // Coord effect functions (no prefix)
    std::string effectFunctions;
    for (const auto& ef : _effects) {
        effectFunctions += "// Effect: " + ef.name + "\n";
        effectFunctions += ef.code + "\n\n";
    }
    if (!replacePlaceholder(result, EFFECT_FUNCTIONS_PLACEHOLDER, effectFunctions)) {
        ywarn("ShaderManager: effect functions placeholder not found");
    }

    // Coord effect apply dispatch
    // Effects modify distortedPos: distortedPos = effect_xxx(pixelPos)
    std::string effectApply;
    if (!_effects.empty()) {
        effectApply = "if (grid.effectIndex != 0u) {\n";
        for (size_t i = 0; i < _effects.size(); i++) {
            const auto& ef = _effects[i];
            if (ef.functionName.empty()) continue;
            if (i > 0) effectApply += " else ";
            effectApply += "    if (grid.effectIndex == " + std::to_string(ef.index) + "u) {\n";
            effectApply += "        distortedPos = " + ef.functionName + "(pixelPos);\n";
            effectApply += "    }";
        }
        effectApply += "\n    }\n";
    }
    if (!replacePlaceholder(result, EFFECT_APPLY_PLACEHOLDER, effectApply)) {
        ywarn("ShaderManager: effect apply placeholder not found");
    }

    // Post-effect functions
    std::string postEffectFunctions;
    for (const auto& ef : _postEffects) {
        postEffectFunctions += "// Post-effect: " + ef.name + "\n";
        postEffectFunctions += ef.code + "\n\n";
    }
    if (!replacePlaceholder(result, POST_EFFECT_FUNCTIONS_PLACEHOLDER, postEffectFunctions)) {
        ywarn("ShaderManager: post-effect functions placeholder not found");
    }

    // Post-effect apply dispatch
    // Post-effects modify finalColor: finalColor = postEffect_xxx(finalColor, pixelPos, screenSize, time, params)
    std::string postEffectApply;
    if (!_postEffects.empty()) {
        postEffectApply = "if (grid.postEffectIndex != 0u) {\n";
        for (size_t i = 0; i < _postEffects.size(); i++) {
            const auto& ef = _postEffects[i];
            if (ef.functionName.empty()) continue;
            if (i > 0) postEffectApply += " else ";
            postEffectApply += "    if (grid.postEffectIndex == " + std::to_string(ef.index) + "u) {\n";
            postEffectApply += "        finalColor = " + ef.functionName + "(finalColor, fbPixelPos, vec2<f32>(globals.screenWidth, globals.screenHeight), globals.time, array<f32, 6>(grid.postEffectP0, grid.postEffectP1, grid.postEffectP2, grid.postEffectP3, grid.postEffectP4, grid.postEffectP5));\n";
            postEffectApply += "    }";
        }
        postEffectApply += "\n    }\n";
    }
    if (!replacePlaceholder(result, POST_EFFECT_APPLY_PLACEHOLDER, postEffectApply)) {
        ywarn("ShaderManager: post-effect apply placeholder not found");
    }

    return result;
}

Result<std::string> ShaderManagerImpl::merge() {
    if (_baseShader.empty()) {
        return Err<std::string>("ShaderManager: no base shader loaded");
    }

    _mergedSource = mergeShaders();

    int lineCount = static_cast<int>(std::count(_mergedSource.begin(), _mergedSource.end(), '\n')) + 1;
    ydebug("ShaderManager: merged shader ({} lines)", lineCount);

    // Dump merged shader for debugging
    {
        FILE* dumpFile = fopen("/tmp/yetty-merged-shader.wgsl", "w");
        if (dumpFile) {
            fwrite(_mergedSource.data(), 1, _mergedSource.size(), dumpFile);
            fclose(dumpFile);
            ydebug("ShaderManager: dumped merged shader to /tmp/yetty-merged-shader.wgsl");
        }
    }

    // Clear dirty flags
    for (auto& providerEntry : _providers) {
        if (providerEntry.provider) {
            providerEntry.provider->clearDirty();
        }
    }

    return Ok(_mergedSource);
}

} // namespace yetty

#pragma once

#include <yetty/shader-provider.hpp>
#include <yetty/core/factory-object.hpp>
#include <yetty/core/result.hpp>
#include <string>
#include <memory>

namespace yetty {

/**
 * ShaderManager - text processing only, NO GPU operations.
 *
 * Collects shader code from providers, replaces placeholders, and outputs
 * merged WGSL source code. All GPU compilation is done by GpuResourceBinder.
 */
class ShaderManager : public core::FactoryObject<ShaderManager> {
public:
    virtual ~ShaderManager() = default;

    // Factory - shadersDir is path to WGSL shader files
    static Result<ShaderManager*> createImpl(const std::string& shadersDir) noexcept;

    /**
     * Register a shader provider.
     * @param provider The shader provider
     * @param dispatchName Name of the dispatch placeholder (e.g., "shaderGlyphDispatch")
     */
    virtual void addProvider(std::shared_ptr<ShaderProvider> provider, const std::string& dispatchName) = 0;

    /**
     * Add a shared library (reusable WGSL functions).
     */
    virtual void addLibrary(const std::string& name, const std::string& code) = 0;

    /**
     * Set WGSL binding declarations to inject into shader.
     */
    virtual void setBindingCode(const std::string& wgslCode) = 0;

    /**
     * Check if any provider changed and remerge is needed.
     */
    virtual bool needsRemerge() const = 0;

    /**
     * Merge all shaders and return complete WGSL source.
     */
    virtual Result<std::string> merge() = 0;

    /**
     * Get last merged source (for debugging).
     */
    virtual const std::string& getMergedSource() const = 0;

protected:
    ShaderManager() = default;
};

} // namespace yetty

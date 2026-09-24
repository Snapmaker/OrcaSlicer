// This file is part of OrcaSlicer.

#ifndef slic3r_GLSubTextureBindRenderer_hpp_
#define slic3r_GLSubTextureBindRenderer_hpp_

#include "GLTexture.hpp"

namespace Slic3r {

class GLShaderProgram;

namespace GUI {

class GLSubTextureBindRenderer
{
public:
    GLSubTextureBindRenderer();
    ~GLSubTextureBindRenderer();

    bool Begin();
    void End();
    void Reset();

    bool Render(unsigned int texId, float left, float right, float bottom, float top, const GLTexture::Quad_UVs& uvs);

private:
    struct GpuObjects
    {
        unsigned int vboId     = 0;
        unsigned int iboId     = 0;
        unsigned int vaoId     = 0;
        unsigned int indexType = 0;
        int indexCount         = 0;
        bool initialized       = false;
        bool vaoInitialized    = false;
    };

    bool EnsureGpuObjects();
    bool EnsureVao();
    bool BindVertexLayout();
    void BindTextureIfNeeded(unsigned int texId);
    void DeleteVao();
    void ResetActiveState();
    void SetMatrices(float left, float right, float bottom, float top, const GLTexture::Quad_UVs& uvs);

private:
    GpuObjects _gpuObjects;
    GLShaderProgram* _shader = nullptr;
    unsigned int _currentTextureId = 0;
    int _positionAttribId = -1;
    bool _active = false;
    bool _usingVao = false;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLSubTextureBindRenderer_hpp_

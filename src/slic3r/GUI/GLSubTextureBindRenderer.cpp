// This file is part of OrcaSlicer.

#include "GLSubTextureBindRenderer.hpp"

#include "3DScene.hpp"
#include "GUI_App.hpp"
#include "GLShader.hpp"
#include "OpenGLManager.hpp"

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {

namespace
{

const float UNIT_QUAD_VERTICES[] = {
    -0.5f, -0.5f, 0.0f,
     0.5f, -0.5f, 0.0f,
     0.5f,  0.5f, 0.0f,
    -0.5f,  0.5f, 0.0f
};

const unsigned int UNIT_QUAD_INDICES[] = {
    0, 1, 2,
    2, 3, 0
};

const int UNIT_QUAD_INDEX_COUNT = 6;
const int UNIT_QUAD_POSITION_COMPONENTS = 3;

} // namespace

GLSubTextureBindRenderer::GLSubTextureBindRenderer()
{
}

GLSubTextureBindRenderer::~GLSubTextureBindRenderer()
{
    Reset();
}

bool GLSubTextureBindRenderer::Begin()
{
    if (_active)
    {
        return true;
    }

    if (!EnsureGpuObjects())
    {
        return false;
    }

    _shader = wxGetApp().get_shader("flat_texture");
    if (_shader == nullptr)
    {
        return false;
    }

    _currentTextureId = 0;
    _positionAttribId = -1;
    _usingVao = false;

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_TEXTURE_2D));
    glsafe(::glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE));

    _shader->start_using();
    _shader->set_uniform("projection_matrix", Transform3d::Identity());

    if (OpenGLManager::VertexArraysSupported() && EnsureVao())
    {
        glsafe(::glBindVertexArray(_gpuObjects.vaoId));

        _usingVao = true;
    }
    else
    {
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _gpuObjects.vboId));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _gpuObjects.iboId));

        if (!BindVertexLayout())
        {
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            _shader->stop_using();
            glsafe(::glDisable(GL_TEXTURE_2D));
            glsafe(::glDisable(GL_BLEND));
            ResetActiveState();
            return false;
        }
    }

    _active = true;
    return true;
}

void GLSubTextureBindRenderer::End()
{
    if (!_active)
    {
        return;
    }

    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    if (_usingVao)
    {
        glsafe(::glBindVertexArray(0));
    }
    else
    {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

        if (_positionAttribId != -1)
        {
            glsafe(::glDisableVertexAttribArray(_positionAttribId));
        }

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
    }

    if (_shader != nullptr)
    {
        _shader->stop_using();
    }

    glsafe(::glDisable(GL_TEXTURE_2D));
    glsafe(::glDisable(GL_BLEND));

    ResetActiveState();
}

void GLSubTextureBindRenderer::Reset()
{
    if (_active)
    {
        End();
    }

    DeleteVao();

    if (_gpuObjects.iboId != 0)
    {
        glsafe(::glDeleteBuffers(1, &_gpuObjects.iboId));
        _gpuObjects.iboId = 0;
    }

    if (_gpuObjects.vboId != 0)
    {
        glsafe(::glDeleteBuffers(1, &_gpuObjects.vboId));
        _gpuObjects.vboId = 0;
    }

    _gpuObjects = GpuObjects();
}

bool GLSubTextureBindRenderer::Render(unsigned int texId, float left, float right, float bottom, float top, const GLTexture::Quad_UVs& uvs)
{
    if (!_active || _shader == nullptr || texId == 0)
    {
        return false;
    }

    SetMatrices(left, right, bottom, top, uvs);
    BindTextureIfNeeded(texId);
    glsafe(::glDrawElements(GL_TRIANGLES, _gpuObjects.indexCount, static_cast<GLenum>(_gpuObjects.indexType), nullptr));
    return true;
}

bool GLSubTextureBindRenderer::EnsureGpuObjects()
{
    if (_gpuObjects.initialized)
    {
        return true;
    }

    glsafe(::glGenBuffers(1, &_gpuObjects.vboId));
    if (_gpuObjects.vboId == 0)
    {
        return false;
    }

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _gpuObjects.vboId));
    glsafe(::glBufferData(GL_ARRAY_BUFFER, sizeof(UNIT_QUAD_VERTICES), UNIT_QUAD_VERTICES, GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

    glsafe(::glGenBuffers(1, &_gpuObjects.iboId));
    if (_gpuObjects.iboId == 0)
    {
        glsafe(::glDeleteBuffers(1, &_gpuObjects.vboId));
        _gpuObjects.vboId = 0;
        return false;
    }

    const bool vertexArraysSupported = OpenGLManager::VertexArraysSupported();
    GLint previousIbo = 0;
    if (vertexArraysSupported)
        glsafe(::glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &previousIbo));

    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _gpuObjects.iboId));
    glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(UNIT_QUAD_INDICES), UNIT_QUAD_INDICES, GL_STATIC_DRAW));
    const GLuint restoredIbo = vertexArraysSupported ? static_cast<GLuint>(previousIbo) : 0;
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, restoredIbo));

    _gpuObjects.indexType = GL_UNSIGNED_INT;
    _gpuObjects.indexCount = UNIT_QUAD_INDEX_COUNT;
    _gpuObjects.initialized = true;
    return true;
}

bool GLSubTextureBindRenderer::EnsureVao()
{
    if (!OpenGLManager::VertexArraysSupported())
    {
        return false;
    }

    if (_gpuObjects.vaoInitialized)
    {
        return _gpuObjects.vaoId != 0;
    }

    if (_shader == nullptr || _gpuObjects.vboId == 0 || _gpuObjects.iboId == 0)
    {
        return false;
    }

    const int positionAttribId = _shader->get_attrib_location("v_position");
    if (positionAttribId == -1)
    {
        return false;
    }

    glsafe(::glGenVertexArrays(static_cast<GLsizei>(1), &_gpuObjects.vaoId));
    if (_gpuObjects.vaoId == 0)
    {
        return false;
    }

    glsafe(::glBindVertexArray(_gpuObjects.vaoId));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, _gpuObjects.vboId));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _gpuObjects.iboId));

    glsafe(::glVertexAttribPointer(positionAttribId, UNIT_QUAD_POSITION_COMPONENTS, GL_FLOAT, GL_FALSE,
        UNIT_QUAD_POSITION_COMPONENTS * sizeof(float), nullptr));
    glsafe(::glEnableVertexAttribArray(positionAttribId));
    glsafe(::glBindVertexArray(0));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));

    _gpuObjects.vaoInitialized = true;
    return true;
}

bool GLSubTextureBindRenderer::BindVertexLayout()
{
    if (_shader == nullptr)
    {
        return false;
    }

    _positionAttribId = _shader->get_attrib_location("v_position");
    if (_positionAttribId == -1)
    {
        return false;
    }

    glsafe(::glVertexAttribPointer(_positionAttribId, UNIT_QUAD_POSITION_COMPONENTS, GL_FLOAT, GL_FALSE,
        UNIT_QUAD_POSITION_COMPONENTS * sizeof(float), nullptr));
    glsafe(::glEnableVertexAttribArray(_positionAttribId));
    return true;
}

void GLSubTextureBindRenderer::BindTextureIfNeeded(unsigned int texId)
{
    if (_currentTextureId == texId)
    {
        return;
    }

    glsafe(::glBindTexture(GL_TEXTURE_2D, texId));
    _currentTextureId = texId;
}

void GLSubTextureBindRenderer::DeleteVao()
{
    if (_gpuObjects.vaoId == 0)
    {
        return;
    }

    glsafe(::glDeleteVertexArrays(static_cast<GLsizei>(1), &_gpuObjects.vaoId));
    _gpuObjects.vaoId = 0;
    _gpuObjects.vaoInitialized = false;
}

void GLSubTextureBindRenderer::ResetActiveState()
{
    _shader = nullptr;
    _currentTextureId = 0;
    _positionAttribId = -1;
    _active = false;
    _usingVao = false;
}

void GLSubTextureBindRenderer::SetMatrices(float left, float right, float bottom, float top, const GLTexture::Quad_UVs& uvs)
{
    if (_shader == nullptr)
    {
        return;
    }

    const float centerX = (left + right) * 0.5f;
    const float centerY = (bottom + top) * 0.5f;
    const float scaleX = right - left;
    const float scaleY = top - bottom;

    Transform3d modelMatrix = Transform3d::Identity();
    modelMatrix.data()[3 * 4 + 0] = centerX;
    modelMatrix.data()[3 * 4 + 1] = centerY;
    modelMatrix.data()[0 * 4 + 0] = scaleX;
    modelMatrix.data()[1 * 4 + 1] = scaleY;

    const float centerU = (uvs.right_bottom.u + uvs.left_bottom.u) * 0.5f;
    const float centerV = (uvs.right_top.v + uvs.right_bottom.v) * 0.5f;
    const float scaleU = uvs.right_bottom.u - uvs.left_bottom.u;
    const float scaleV = uvs.right_bottom.v - uvs.right_top.v;

    Matrix3f uvMatrix = Matrix3f::Identity();
    uvMatrix(0, 2) = centerU;
    uvMatrix(1, 2) = centerV;
    uvMatrix(0, 0) = scaleU;
    uvMatrix(1, 1) = scaleV;

    _shader->set_uniform("view_model_matrix", modelMatrix);
    _shader->set_uniform("projection_matrix", Transform3d::Identity());
    _shader->set_uniform("u_uvTransformMatrix", uvMatrix);
}

} // namespace GUI
} // namespace Slic3r

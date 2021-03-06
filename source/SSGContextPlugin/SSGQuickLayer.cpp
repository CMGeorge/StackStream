/****************************************************************************
**
** Portions (C) 2015 The Qt Company Ltd.
** Portions (C) 2016 Erik Hvatum
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtQuick module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtQuick/private/qsgdefaultlayer_p.h>
#include <QOpenGLFramebufferObject>
#include "SSGQuickLayer.h"

#include <QtQml/private/qqmlglobal_p.h>
#include <QtQuick/private/qsgrenderer_p.h>

#ifdef QSG_DEBUG_FBO_OVERLAY
DEFINE_BOOL_CONFIG_OPTION(qmlFboOverlay, QML_FBO_OVERLAY)
#endif
DEFINE_BOOL_CONFIG_OPTION(qmlFboFlushBeforeDetach, QML_FBO_FLUSH_BEFORE_DETACH)

namespace
{
    class BindableFbo : public QSGBindable
    {
    public:
        BindableFbo(QOpenGLFramebufferObject *fbo, QSGDepthStencilBuffer *depthStencil);
        virtual ~BindableFbo();
        void bind() const override;
    private:
        QOpenGLFramebufferObject *m_fbo;
        QSGDepthStencilBuffer *m_depthStencil;
    };

    BindableFbo::BindableFbo(QOpenGLFramebufferObject *fbo, QSGDepthStencilBuffer *depthStencil)
        : m_fbo(fbo)
        , m_depthStencil(depthStencil)
    {
    }

    BindableFbo::~BindableFbo()
    {
        if (qmlFboFlushBeforeDetach())
            QOpenGLContext::currentContext()->functions()->glFlush();
        if (m_depthStencil)
            m_depthStencil->detach();
    }

    void BindableFbo::bind() const
    {
        m_fbo->bind();
        if (m_depthStencil)
            m_depthStencil->attach();
    }
}

SSGQuickLayer::SSGQuickLayer(QSGRenderContext *context)
    : QSGLayer()
    , m_item(0)
    , m_device_pixel_ratio(1)
    , m_format(GL_RGBA32F)
    , m_renderer(0)
    , m_fbo(0)
    , m_secondaryFbo(0)
    , m_transparentTexture(0)
#ifdef QSG_DEBUG_FBO_OVERLAY
    , m_debugOverlay(0)
#endif
    , m_context(context)
    , m_mipmap(false)
    , m_live(true)
    , m_recursive(false)
    , m_dirtyTexture(true)
    , m_multisamplingChecked(false)
    , m_multisampling(false)
    , m_grab(false)
    , m_mirrorHorizontal(false)
    , m_mirrorVertical(true)
{
}

SSGQuickLayer::~SSGQuickLayer()
{
    invalidated();
}

void SSGQuickLayer::invalidated()
{
    delete m_renderer;
    m_renderer = 0;
    delete m_fbo;
    delete m_secondaryFbo;
    m_fbo = m_secondaryFbo = 0;
#ifdef QSG_DEBUG_FBO_OVERLAY
    delete m_debugOverlay;
    m_debugOverlay = 0;
#endif
    if (m_transparentTexture) {
        QOpenGLContext::currentContext()->functions()->glDeleteTextures(1, &m_transparentTexture);
        m_transparentTexture = 0;
    }
}

int SSGQuickLayer::textureId() const
{
    return m_fbo ? m_fbo->texture() : 0;
}

bool SSGQuickLayer::hasAlphaChannel() const
{
    return true;
}

bool SSGQuickLayer::hasMipmaps() const
{
    return m_mipmap;
}


void SSGQuickLayer::bind()
{
#ifndef QT_NO_DEBUG
    if (!m_recursive && m_fbo && ((m_multisampling && m_secondaryFbo->isBound()) || m_fbo->isBound()))
        qWarning("ShaderEffectSource: \'recursive\' must be set to true when rendering recursively.");
#endif
    QOpenGLFunctions *funcs = QOpenGLContext::currentContext()->functions();
    if (!m_fbo && m_format == GL_RGBA32F) {
        if (m_transparentTexture == 0) {
            funcs->glGenTextures(1, &m_transparentTexture);
            funcs->glBindTexture(GL_TEXTURE_2D, m_transparentTexture);
            const float zero[4] = {0, 0, 0, 0};
            funcs->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, zero);
        } else {
            funcs->glBindTexture(GL_TEXTURE_2D, m_transparentTexture);
        }
    } else {
        funcs->glBindTexture(GL_TEXTURE_2D, m_fbo ? m_fbo->texture() : 0);
        updateBindOptions();
    }
}

bool SSGQuickLayer::updateTexture()
{
    bool doGrab = (m_live || m_grab) && m_dirtyTexture;
    if (doGrab)
        grab();
    if (m_grab)
        emit scheduledUpdateCompleted();
    m_grab = false;
    return doGrab;
}

void SSGQuickLayer::setHasMipmaps(bool mipmap)
{
    if (mipmap == m_mipmap)
        return;
    m_mipmap = mipmap;
    if (m_mipmap && m_fbo && !m_fbo->format().mipmap())
        markDirtyTexture();
}


void SSGQuickLayer::setItem(QSGNode *item)
{
    if (item == m_item)
        return;
    m_item = item;

    if (m_live && !m_item) {
        delete m_fbo;
        delete m_secondaryFbo;
        m_fbo = m_secondaryFbo = 0;
        m_depthStencilBuffer.clear();
    }

    markDirtyTexture();
}

void SSGQuickLayer::setRect(const QRectF &rect)
{
    if (rect == m_rect)
        return;
    m_rect = rect;
    markDirtyTexture();
}

void SSGQuickLayer::setSize(const QSize &size)
{
    if (size == m_size)
        return;
    m_size = size;

    if (m_live && m_size.isNull()) {
        delete m_fbo;
        delete m_secondaryFbo;
        m_fbo = m_secondaryFbo = 0;
        m_depthStencilBuffer.clear();
    }

    markDirtyTexture();
}

void SSGQuickLayer::setFormat(GLenum)
{
    // NOPE!  We're always GL_RGBA32F!
}

void SSGQuickLayer::setLive(bool live)
{
    if (live == m_live)
        return;
    m_live = live;

    if (m_live && (!m_item || m_size.isNull())) {
        delete m_fbo;
        delete m_secondaryFbo;
        m_fbo = m_secondaryFbo = 0;
        m_depthStencilBuffer.clear();
    }

    markDirtyTexture();
}

void SSGQuickLayer::scheduleUpdate()
{
    if (m_grab)
        return;
    m_grab = true;
    if (m_dirtyTexture)
        emit updateRequested();
}

void SSGQuickLayer::setRecursive(bool recursive)
{
    m_recursive = recursive;
}

void SSGQuickLayer::setMirrorHorizontal(bool mirror)
{
    m_mirrorHorizontal = mirror;
}

void SSGQuickLayer::setMirrorVertical(bool mirror)
{
    m_mirrorVertical = mirror;
}

void SSGQuickLayer::markDirtyTexture()
{
    m_dirtyTexture = true;
    if (m_live || m_grab)
        emit updateRequested();
}

void SSGQuickLayer::grab()
{
    if (!m_item || m_size.isNull()) {
        delete m_fbo;
        delete m_secondaryFbo;
        m_fbo = m_secondaryFbo = 0;
        m_depthStencilBuffer.clear();
        m_dirtyTexture = false;
        return;
    }
    QSGNode *root = m_item;
    while (root->firstChild() && root->type() != QSGNode::RootNodeType)
        root = root->firstChild();
    if (root->type() != QSGNode::RootNodeType)
        return;

    if (!m_renderer) {
        m_renderer = m_context->createRenderer();
        connect(m_renderer, SIGNAL(sceneGraphChanged()), this, SLOT(markDirtyTexture()));
    }
    m_renderer->setDevicePixelRatio(m_device_pixel_ratio);
    m_renderer->setRootNode(static_cast<QSGRootNode *>(root));

    QOpenGLFunctions *funcs = QOpenGLContext::currentContext()->functions();
    bool deleteFboLater = false;
    if (!m_fbo || m_fbo->size() != m_size || m_fbo->format().internalTextureFormat() != m_format
        || (!m_fbo->format().mipmap() && m_mipmap))
    {
        if (!m_multisamplingChecked) {
            if (m_context->openglContext()->format().samples() <= 1) {
                m_multisampling = false;
            } else {
                const QSet<QByteArray> extensions = m_context->openglContext()->extensions();
                m_multisampling = extensions.contains(QByteArrayLiteral("GL_EXT_framebuffer_multisample"))
                    && extensions.contains(QByteArrayLiteral("GL_EXT_framebuffer_blit"));
            }
            m_multisamplingChecked = true;
        }
        if (m_multisampling) {
            // Don't delete the FBO right away in case it is used recursively.
            deleteFboLater = true;
            delete m_secondaryFbo;
            QOpenGLFramebufferObjectFormat format;

            format.setInternalTextureFormat(m_format);
            format.setSamples(m_context->openglContext()->format().samples());
            m_secondaryFbo = new QOpenGLFramebufferObject(m_size, format);
            m_depthStencilBuffer = m_context->depthStencilBufferForFbo(m_secondaryFbo);
        } else {
            QOpenGLFramebufferObjectFormat format;
            format.setInternalTextureFormat(m_format);
            format.setMipmap(m_mipmap);
            if (m_recursive) {
                deleteFboLater = true;
                delete m_secondaryFbo;
                m_secondaryFbo = new QOpenGLFramebufferObject(m_size, format);
                funcs->glBindTexture(GL_TEXTURE_2D, m_secondaryFbo->texture());
                updateBindOptions(true);
                m_depthStencilBuffer = m_context->depthStencilBufferForFbo(m_secondaryFbo);
            } else {
                delete m_fbo;
                delete m_secondaryFbo;
                m_fbo = new QOpenGLFramebufferObject(m_size, format);
                m_secondaryFbo = 0;
                funcs->glBindTexture(GL_TEXTURE_2D, m_fbo->texture());
                updateBindOptions(true);
                m_depthStencilBuffer = m_context->depthStencilBufferForFbo(m_fbo);
            }
        }
    }

    if (m_recursive && !m_secondaryFbo) {
        // m_fbo already created, m_recursive was just set.
        Q_ASSERT(m_fbo);
        Q_ASSERT(!m_multisampling);

        m_secondaryFbo = new QOpenGLFramebufferObject(m_size, m_fbo->format());
        funcs->glBindTexture(GL_TEXTURE_2D, m_secondaryFbo->texture());
        updateBindOptions(true);
    }

    // Render texture.
    root->markDirty(QSGNode::DirtyForceUpdate); // Force matrix, clip and opacity update.
    m_renderer->nodeChanged(root, QSGNode::DirtyForceUpdate); // Force render list update.

#ifdef QSG_DEBUG_FBO_OVERLAY
    if (qmlFboOverlay()) {
        if (!m_debugOverlay)
            m_debugOverlay = new QSGSimpleRectNode();
        m_debugOverlay->setRect(QRectF(0, 0, m_size.width(), m_size.height()));
        m_debugOverlay->setColor(QColor(0xff, 0x00, 0x80, 0x40));
        root->appendChildNode(m_debugOverlay);
    }
#endif

    m_dirtyTexture = false;

    m_renderer->setDeviceRect(m_size);
    m_renderer->setViewportRect(m_size);
    QRectF mirrored(m_mirrorHorizontal ? m_rect.right() : m_rect.left(),
                    m_mirrorVertical ? m_rect.bottom() : m_rect.top(),
                    m_mirrorHorizontal ? -m_rect.width() : m_rect.width(),
                    m_mirrorVertical ? -m_rect.height() : m_rect.height());
    m_renderer->setProjectionMatrixToRect(mirrored);
    m_renderer->setClearColor(Qt::transparent);

    if (m_multisampling) {
        m_renderer->renderScene(BindableFbo(m_secondaryFbo, m_depthStencilBuffer.data()));

        if (deleteFboLater) {
            delete m_fbo;
            QOpenGLFramebufferObjectFormat format;
            format.setInternalTextureFormat(m_format);
            format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
            format.setMipmap(m_mipmap);
            format.setSamples(0);
            m_fbo = new QOpenGLFramebufferObject(m_size, format);
            funcs->glBindTexture(GL_TEXTURE_2D, m_fbo->texture());
            updateBindOptions(true);
        }

        QRect r(QPoint(), m_size);
        QOpenGLFramebufferObject::blitFramebuffer(m_fbo, r, m_secondaryFbo, r);
    } else {
        if (m_recursive) {
            m_renderer->renderScene(BindableFbo(m_secondaryFbo, m_depthStencilBuffer.data()));

            if (deleteFboLater) {
                delete m_fbo;
                QOpenGLFramebufferObjectFormat format;
                format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
                format.setInternalTextureFormat(m_format);
                format.setMipmap(m_mipmap);
                m_fbo = new QOpenGLFramebufferObject(m_size, format);
                funcs->glBindTexture(GL_TEXTURE_2D, m_fbo->texture());
                updateBindOptions(true);
            }
            qSwap(m_fbo, m_secondaryFbo);
        } else {
            m_renderer->renderScene(BindableFbo(m_fbo, m_depthStencilBuffer.data()));
        }
    }

    if (m_mipmap) {
        funcs->glBindTexture(GL_TEXTURE_2D, textureId());
        funcs->glGenerateMipmap(GL_TEXTURE_2D);
    }

    root->markDirty(QSGNode::DirtyForceUpdate); // Force matrix, clip, opacity and render list update.

#ifdef QSG_DEBUG_FBO_OVERLAY
    if (qmlFboOverlay())
        root->removeChildNode(m_debugOverlay);
#endif
    if (m_recursive)
        markDirtyTexture(); // Continuously update if 'live' and 'recursive'.
}

QImage SSGQuickLayer::toImage() const
{
    if (m_fbo)
        return m_fbo->toImage();

    return QImage();
}

QRectF SSGQuickLayer::normalizedTextureSubRect() const
{
    return QRectF(m_mirrorHorizontal ? 1 : 0,
                  m_mirrorVertical ? 0 : 1,
                  m_mirrorHorizontal ? -1 : 1,
                  m_mirrorVertical ? 1 : -1);
}


#include <QtQuick/private/qsgdepthstencilbuffer_p.h>

QT_BEGIN_NAMESPACE

QSGDepthStencilBuffer::QSGDepthStencilBuffer(QOpenGLContext *context, const Format &format)
    : m_functions(context)
    , m_manager(0)
    , m_format(format)
    , m_depthBuffer(0)
    , m_stencilBuffer(0)
{
    // 'm_manager' is set by QSGDepthStencilBufferManager::insertBuffer().
}

QSGDepthStencilBuffer::~QSGDepthStencilBuffer()
{
    if (m_manager)
        m_manager->m_buffers.remove(m_format);
}

void QSGDepthStencilBuffer::attach()
{
    m_functions.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, m_depthBuffer);
    m_functions.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                          GL_RENDERBUFFER, m_stencilBuffer);
}

void QSGDepthStencilBuffer::detach()
{
    m_functions.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                          GL_RENDERBUFFER, 0);
    m_functions.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                          GL_RENDERBUFFER, 0);
}

#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif

#ifndef GL_DEPTH_COMPONENT24_OES
#define GL_DEPTH_COMPONENT24_OES 0x81A6
#endif

QSGDefaultDepthStencilBuffer::QSGDefaultDepthStencilBuffer(QOpenGLContext *context, const Format &format)
    : QSGDepthStencilBuffer(context, format)
{
    const GLsizei width = format.size.width();
    const GLsizei height = format.size.height();

    if (format.attachments == (DepthAttachment | StencilAttachment)
            && m_functions.hasOpenGLExtension(QOpenGLExtensions::PackedDepthStencil))
    {
        m_functions.glGenRenderbuffers(1, &m_depthBuffer);
        m_functions.glBindRenderbuffer(GL_RENDERBUFFER, m_depthBuffer);
        if (format.samples && m_functions.hasOpenGLExtension(QOpenGLExtensions::FramebufferMultisample)) {
#if defined(QT_OPENGL_ES_2)
            m_functions.glRenderbufferStorageMultisample(GL_RENDERBUFFER, format.samples,
                GL_DEPTH24_STENCIL8_OES, width, height);
#else
            m_functions.glRenderbufferStorageMultisample(GL_RENDERBUFFER, format.samples,
                GL_DEPTH24_STENCIL8, width, height);
#endif
        } else {
#if defined(QT_OPENGL_ES_2)
            m_functions.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, width, height);
#else
            m_functions.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
#endif
        }
        m_stencilBuffer = m_depthBuffer;
    }
    if (!m_depthBuffer && (format.attachments & DepthAttachment)) {
        m_functions.glGenRenderbuffers(1, &m_depthBuffer);
        m_functions.glBindRenderbuffer(GL_RENDERBUFFER, m_depthBuffer);
        GLenum internalFormat = GL_DEPTH_COMPONENT;
        if (context->isOpenGLES())
            internalFormat = m_functions.hasOpenGLExtension(QOpenGLExtensions::Depth24)
                ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
        if (format.samples && m_functions.hasOpenGLExtension(QOpenGLExtensions::FramebufferMultisample)) {
            m_functions.glRenderbufferStorageMultisample(GL_RENDERBUFFER, format.samples,
                internalFormat, width, height);
        } else {
            m_functions.glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, width, height);
        }
    }
    if (!m_stencilBuffer && (format.attachments & StencilAttachment)) {
        m_functions.glGenRenderbuffers(1, &m_stencilBuffer);
        m_functions.glBindRenderbuffer(GL_RENDERBUFFER, m_stencilBuffer);
#ifdef QT_OPENGL_ES
        const GLenum internalFormat = GL_STENCIL_INDEX8;
#else
        const GLenum internalFormat = context->isOpenGLES() ? GL_STENCIL_INDEX8 : GL_STENCIL_INDEX;
#endif
        if (format.samples && m_functions.hasOpenGLExtension(QOpenGLExtensions::FramebufferMultisample)) {
            m_functions.glRenderbufferStorageMultisample(GL_RENDERBUFFER, format.samples,
                internalFormat, width, height);
        } else {
            m_functions.glRenderbufferStorage(GL_RENDERBUFFER, internalFormat, width, height);
        }
    }
}

QSGDefaultDepthStencilBuffer::~QSGDefaultDepthStencilBuffer()
{
    free();
}

void QSGDefaultDepthStencilBuffer::free()
{
    if (m_depthBuffer)
        m_functions.glDeleteRenderbuffers(1, &m_depthBuffer);
    if (m_stencilBuffer && m_stencilBuffer != m_depthBuffer)
        m_functions.glDeleteRenderbuffers(1, &m_stencilBuffer);
    m_depthBuffer = m_stencilBuffer = 0;
}


QSGDepthStencilBufferManager::~QSGDepthStencilBufferManager()
{
    for (Hash::const_iterator it = m_buffers.constBegin(), cend = m_buffers.constEnd(); it != cend; ++it) {
        QSGDepthStencilBuffer *buffer = it.value().data();
        buffer->free();
        buffer->m_manager = 0;
    }
}

QSharedPointer<QSGDepthStencilBuffer> QSGDepthStencilBufferManager::bufferForFormat(const QSGDepthStencilBuffer::Format &fmt)
{
    Hash::const_iterator it = m_buffers.constFind(fmt);
    if (it != m_buffers.constEnd())
        return it.value().toStrongRef();
    return QSharedPointer<QSGDepthStencilBuffer>();
}

void QSGDepthStencilBufferManager::insertBuffer(const QSharedPointer<QSGDepthStencilBuffer> &buffer)
{
    Q_ASSERT(buffer->m_manager == 0);
    Q_ASSERT(!m_buffers.contains(buffer->m_format));
    buffer->m_manager = this;
    m_buffers.insert(buffer->m_format, buffer.toWeakRef());
}

uint qHash(const QSGDepthStencilBuffer::Format &format)
{
    return qHash(qMakePair(format.size.width(), format.size.height()))
            ^ (uint(format.samples) << 12) ^ (uint(format.attachments) << 28);
}

QT_END_NAMESPACE

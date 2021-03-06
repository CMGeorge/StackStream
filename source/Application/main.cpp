// The MIT License (MIT)
//
// Copyright (c) 2016 Erik Hvatum
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Authors: Erik Hvatum <ice.rikh@gmail.com>

#include "../common.h"
#include "../SSImageItem.h"
#include <QApplication>
#include <cstdio>

static QSurfaceFormat fmt;

static void onStackStreamWindowCreated(QObject* object, const QUrl&)
{
    QQuickWindow* stackStreamMainWindow{qobject_cast<QQuickWindow*>(object)};
    if(stackStreamMainWindow && stackStreamMainWindow->objectName() == "stackStreamMainWindow")
    {
        stackStreamMainWindow->setFormat(fmt);
        stackStreamMainWindow->show();
    }
}

int main(int argc, char *argv[])
{
    setenv("QMLSCENE_DEVICE", "SSGContextPlugin", 1);
    QApplication app(argc, argv);
    {
        fmt.setRenderableType(QSurfaceFormat::OpenGL);
        fmt.setProfile(QSurfaceFormat::CompatibilityProfile);
        fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        fmt.setSwapInterval(1);
        fmt.setVersion(4, 5);
#ifdef ENABLE_GL_DEBUG_LOGGING
        fmt.setOptions(QSurfaceFormat::DebugContext | QSurfaceFormat::DeprecatedFunctions);
#else
        fmt.setOptions(QSurfaceFormat::DeprecatedFunctions);
#endif
        fmt.setStencilBufferSize(8);
        fmt.setSamples(8);
        // We request 30-bit color; if it's not available, Qt automatically falls back to 24-bit
        fmt.setRedBufferSize(10);
        fmt.setGreenBufferSize(10);
        fmt.setBlueBufferSize(10);
        // NB: Requesting 2-bit alpha and getting it on Linux leads to crashes
    //    fmt.setAlphaBufferSize(2);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, onStackStreamWindowCreated);
    engine.load(QUrl(QStringLiteral("qrc:/StackStreamMainWindow.qml")));
//    MakeImage ssimageFactory;
//    engine.rootContext()->setContextProperty("ssimageFactory", &ssimageFactory);
//    engine.setObjectOwnership(&ssimageFactory, QQmlEngine::CppOwnership);

    int ret{app.exec()};
#ifdef ENABLE_GL_DEBUG_LOGGING
    if(g_glDebugLogger)
    {
        delete g_glDebugLogger;
        g_glDebugLogger = nullptr;
    }
#endif
    return ret;
}

#include "main.moc"

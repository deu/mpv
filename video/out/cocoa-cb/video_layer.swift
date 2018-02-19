/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

import Cocoa
import OpenGL.GL
import OpenGL.GL3

class VideoLayer: CAOpenGLLayer {

    weak var cocoaCB: CocoaCB!
    var mpv: MPVHelper! {
        get { return cocoaCB == nil ? nil : cocoaCB.mpv }
    }

    let videoLock = NSLock()
    var hasVideo: Bool = false
    var neededFlips: Int = 0
    var cglContext: CGLContextObj? = nil

    var canDrawOffScreen: Bool = false
    var lastThread: Thread? = nil

    var needsICCUpdate: Bool = false {
        didSet {
            if needsICCUpdate == true {
                neededFlips += 1
            }
        }
    }

    let surfaceLock = NSLock()
    var surfaceSize: NSSize?

    var inLiveResize: Bool = false {
        didSet {
            if inLiveResize == false {
                isAsynchronous = false
                display()
            } else {
                surfaceLock.lock()
                updateSurfaceSize()
                surfaceLock.unlock()
                isAsynchronous = true
            }
        }
    }

    init(cocoaCB ccb: CocoaCB) {
        cocoaCB = ccb
        super.init()
        autoresizingMask = [.layerWidthSizable, .layerHeightSizable]
        backgroundColor = NSColor.black.cgColor
        contentsScale = cocoaCB.window.backingScaleFactor
    }

    override init(layer: Any) {
        let oldLayer = layer as! VideoLayer
        cocoaCB = oldLayer.cocoaCB
        super.init()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func setUpGLCB() {
        self.mpv.initGLCB()
        self.mpv.setGLCBUpdateCallback(self.updateCallback, context: self)
        self.mpv.setGLCBControlCallback(self.cocoaCB.controlCallback, context: self.cocoaCB)
    }

    override func canDraw(inCGLContext ctx: CGLContextObj,
                          pixelFormat pf: CGLPixelFormatObj,
                          forLayerTime t: CFTimeInterval,
                          displayTime ts: UnsafePointer<CVTimeStamp>?) -> Bool {
        return mpv != nil && cocoaCB.backendState == .init
    }

    override func draw(inCGLContext ctx: CGLContextObj,
                       pixelFormat pf: CGLPixelFormatObj,
                       forLayerTime t: CFTimeInterval,
                       displayTime ts: UnsafePointer<CVTimeStamp>?) {
        neededFlips = 0
        canDrawOffScreen = Thread.current == lastThread
        lastThread = Thread.current
        draw(ctx)
    }

    func draw(_ ctx: CGLContextObj) {
        surfaceLock.lock()
        if inLiveResize == false {
            updateSurfaceSize()
        }

        mpv.drawGLCB(surfaceSize!)
        surfaceLock.unlock()
        CGLFlushDrawable(ctx)

        if needsICCUpdate {
            needsICCUpdate = false
            cocoaCB.updateICCProfile()
        }
    }

    func updateSurfaceSize() {
        surfaceSize = bounds.size
        surfaceSize!.width *= contentsScale
        surfaceSize!.height *= contentsScale
    }

    override func copyCGLPixelFormat(forDisplayMask mask: UInt32) -> CGLPixelFormatObj {
        let glVersions: [CGLOpenGLProfile] = [
            kCGLOGLPVersion_3_2_Core,
            kCGLOGLPVersion_Legacy
        ]

        var pix: CGLPixelFormatObj?
        var err: CGLError = CGLError(rawValue: 0)
        var npix: GLint = 0

        verLoop : for ver in glVersions {
            var glAttributes: [CGLPixelFormatAttribute] = [
                kCGLPFAOpenGLProfile, CGLPixelFormatAttribute(ver.rawValue),
                kCGLPFAAccelerated,
                kCGLPFADoubleBuffer,
                kCGLPFABackingStore,
                kCGLPFAAllowOfflineRenderers,
                kCGLPFASupportsAutomaticGraphicsSwitching,
                _CGLPixelFormatAttribute(rawValue: 0)
            ]

            for index in stride(from: glAttributes.count-2, through: 4, by: -1) {
                err = CGLChoosePixelFormat(glAttributes, &pix, &npix)
                if err == kCGLBadAttribute {
                    glAttributes.remove(at: index)
                } else {
                    break verLoop
                }
            }
        }

        if err != kCGLNoError {
            fatalError("Couldn't create CGL pixel format: \(CGLErrorString(err)) (\(err))")
        }
        return pix!
    }

    override func copyCGLContext(forPixelFormat pf: CGLPixelFormatObj) -> CGLContextObj {
        let ctx = super.copyCGLContext(forPixelFormat: pf)
        var i: GLint = 1
        CGLSetParameter(ctx, kCGLCPSwapInterval, &i)
        CGLSetCurrentContext(ctx)
        cglContext = ctx

        if let app = NSApp as? Application {
            app.initMPVCore()
        }
        return ctx
    }

    let updateCallback: mpv_opengl_cb_update_fn = { (ctx) in
        let layer: VideoLayer = MPVHelper.bridge(ptr: ctx!)
        layer.neededFlips += 1
    }

    override func display() {
        super.display()
        if !isAsynchronous {
            CATransaction.flush()
        }
    }

    func setVideo(_ state: Bool) {
        videoLock.lock()
        hasVideo = state
        neededFlips = 0
        videoLock.unlock()
    }

    func reportFlip() {
        mpv.reportGLCBFlip()
        videoLock.lock()
        if !isAsynchronous && neededFlips > 0 && hasVideo {
            if !cocoaCB.window.occlusionState.contains(.visible) &&
                neededFlips > 1 && canDrawOffScreen
            {
                CGLSetCurrentContext(cglContext!)
                draw(cglContext!)
                display()
            } else {
                display()
            }
        }
        videoLock.unlock()
    }

}

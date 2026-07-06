// electron-liquid-glass JS 入口：原生模块加载守护 + 面板对象封装。
// 原生不可用（非 Windows、系统过旧、构建缺失）时 isSupported() 返回 false，
// 调用方据此回退自己的方案（如 Chromium 屏幕采集管线）。
let native = null
try {
    native = require('./build/Release/liquid_glass.node')
} catch {
    native = null
}

const panels = new Map()
let lumaHooked = false

function ensureLumaDispatch() {
    if (lumaHooked || !native) return
    lumaHooked = true
    native.setLumaCallback((panelId, bands) => {
        const panel = panels.get(panelId)
        if (panel && panel._onLuma) panel._onLuma(bands)
    })
}

function isSupported() {
    return Boolean(native && native.isSupported())
}

/**
 * 创建玻璃面板（所有长度均为物理像素；dpr 用于内部几何常量缩放）。
 * 返回面板句柄对象；原生不可用时返回 null。
 */
function createPanel(options) {
    if (!isSupported()) return null
    const { onLuma, lumaBands, anchorWindow, ...config } = options
    if (anchorWindow) config.anchorHwnd = resolveHwnd(anchorWindow)

    const id = native.createPanel(config)
    const panel = {
        id,
        _onLuma: onLuma ?? null,
        show(fadeMs = 120) { native.showPanel(id, fadeMs) },
        hide(fadeMs = 100) { native.hidePanel(id, fadeMs) },
        destroy() {
            panels.delete(id)
            native.destroyPanel(id)
        },
        setBounds(bounds) { native.setPanelBounds(id, bounds) },
        setParams(params) { native.setPanelParams(id, params) },
        anchor(windowOrHwnd) { native.anchorPanel(id, resolveHwnd(windowOrHwnd)) },
        setLumaBands(bands) { native.setLumaBands(id, bands) },
        onLuma(cb) { panel._onLuma = cb }
    }
    panels.set(id, panel)
    if (lumaBands) native.setLumaBands(id, lumaBands)
    if (onLuma) ensureLumaDispatch()
    return panel
}

// 接受 Electron BrowserWindow 或 getNativeWindowHandle() 的 Buffer
function resolveHwnd(windowOrHwnd) {
    if (!windowOrHwnd) return null
    if (Buffer.isBuffer(windowOrHwnd)) return windowOrHwnd
    if (typeof windowOrHwnd.getNativeWindowHandle === 'function') {
        return windowOrHwnd.getNativeWindowHandle()
    }
    return null
}

function shutdown() {
    // 非 Windows 的桩实现只导出 isSupported/osBuild，其余入口必须判空
    if (native && typeof native.shutdown === 'function') native.shutdown()
    panels.clear()
    lumaHooked = false
}

module.exports = { isSupported, createPanel, shutdown, _native: native }
